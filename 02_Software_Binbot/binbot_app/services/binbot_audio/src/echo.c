#include "echo.h"

#include <math.h>
#include <fcntl.h>
#include <pthread.h>
#include <speex/speex_echo.h>
#include <speex/speex_preprocess.h>
#include <speex/speex_resampler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef BINBOT_HAVE_RKAUDIO_AEC
#include "rkaudio_preprocess.h"
#endif

enum {
    ECHO_RATE = BINBOT_AUDIO_SAMPLE_RATE,
    ECHO_FRAME = BINBOT_AUDIO_FRAME_SAMPLES,
    ECHO_HISTORY = 16384,
    ECHO_SAMPLE_NS = 1000000000 / ECHO_RATE,
    ECHO_FILTER_SAMPLES = ECHO_RATE * 80 / 1000,
#ifdef BINBOT_HAVE_RKAUDIO_AEC
    RK_FRAME = 256,
#endif
};

struct audio_echo {
#ifndef BINBOT_HAVE_RKAUDIO_AEC
    SpeexEchoState *state;
    SpeexPreprocessState *preprocess;
#else
    RKAUDIOParam rk_params;
    void *rk_state;
    int16_t rk_input[RK_FRAME * 2];
    int16_t rk_output[RK_FRAME * 4];
    unsigned rk_input_count;
    unsigned rk_head;
    unsigned rk_count;
#endif
    pthread_mutex_t mutex;
    int16_t reference[ECHO_HISTORY];
    uint64_t ticks[ECHO_HISTORY];
    uint64_t delay_ns;
    uint64_t capture_tick;
    bool capture_started;
    FILE *trace;
    unsigned trace_frames;
    unsigned frames;
    unsigned far_frames;
    double near_energy;
    double clean_energy;
};

const char *audio_echo_engine(void)
{
#ifdef BINBOT_HAVE_RKAUDIO_AEC
    return "rockchip_aec_anr";
#else
    return "speexdsp";
#endif
}

audio_echo_t *audio_echo_create(unsigned delay_ms)
{
    if(delay_ms > 500) return NULL;
    audio_echo_t *echo = calloc(1, sizeof(*echo));
    if(!echo) return NULL;
    if(pthread_mutex_init(&echo->mutex, NULL)) { free(echo); return NULL; }
#ifndef BINBOT_HAVE_RKAUDIO_AEC
    echo->state = speex_echo_state_init(ECHO_FRAME, ECHO_FILTER_SAMPLES);
    echo->preprocess = speex_preprocess_state_init(ECHO_FRAME, ECHO_RATE);
    if(!echo->state || !echo->preprocess) { audio_echo_destroy(echo); return NULL; }
    int rate = ECHO_RATE, denoise = 1, noise_db = -12, echo_db = -50, active_db = -30;
    speex_echo_ctl(echo->state, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);
    speex_preprocess_ctl(echo->preprocess, SPEEX_PREPROCESS_SET_DENOISE, &denoise);
    speex_preprocess_ctl(echo->preprocess, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &noise_db);
    speex_preprocess_ctl(echo->preprocess, SPEEX_PREPROCESS_SET_ECHO_SUPPRESS, &echo_db);
    speex_preprocess_ctl(echo->preprocess, SPEEX_PREPROCESS_SET_ECHO_SUPPRESS_ACTIVE, &active_db);
    speex_preprocess_ctl(echo->preprocess, SPEEX_PREPROCESS_SET_ECHO_STATE, echo->state);
#endif
    echo->delay_ns = (uint64_t)delay_ms * 1000000ULL;
#ifdef BINBOT_HAVE_RKAUDIO_AEC
    echo->rk_params.model_en = RKAUDIO_EN_AEC | RKAUDIO_EN_BF;
    echo->rk_params.aec_param = rkaudio_aec_param_init();
    echo->rk_params.bf_param = rkaudio_preprocess_param_init();
    if(!echo->rk_params.aec_param || !echo->rk_params.bf_param) {
        audio_echo_destroy(echo);
        return NULL;
    }
    SKVAECParameter *aec = echo->rk_params.aec_param;
    SKVPreprocessParam *bf = echo->rk_params.bf_param;
    /* The software reference is aligned with the ALSA DMA clock. The SDK's
     * coarse estimator misaligns this already aligned reference on speech. */
    aec->model_aec_en = 0;
    aec->filter_len = 8;
    bf->model_bf_en = EN_STDT | EN_Anr;
    bf->num_ref_channel = 1;
    /* Residual AES can erase quiet near-end speech during speaker playback. */
    SKVANRParam *anr = bf->anr_para;
    anr->noiseFactor = 0.6f;
    anr->fGmin = 0.15f;
    echo->rk_state = rkaudio_preprocess_init(ECHO_RATE, 16, 1, 1, &echo->rk_params);
    if(!echo->rk_state) { audio_echo_destroy(echo); return NULL; }
    /* Bridge the SDK's 16 ms blocks to IPC's 20 ms frames at fixed latency. */
    echo->rk_count = RK_FRAME;
#endif
    const char *trace_path = getenv("BINBOT_AUDIO_AEC_TRACE");
    if(trace_path && trace_path[0]) {
        int fd = open(trace_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if(fd >= 0) {
            echo->trace = fdopen(fd, "wb");
            if(!echo->trace) close(fd);
        }
    }
    return echo;
}

void audio_echo_destroy(audio_echo_t *echo)
{
    if(!echo) return;
#ifndef BINBOT_HAVE_RKAUDIO_AEC
    if(echo->preprocess) speex_preprocess_state_destroy(echo->preprocess);
    if(echo->state) speex_echo_state_destroy(echo->state);
#endif
    if(echo->trace) fclose(echo->trace);
#ifdef BINBOT_HAVE_RKAUDIO_AEC
    if(echo->rk_state) rkaudio_preprocess_destory(echo->rk_state);
    rkaudio_param_deinit(&echo->rk_params);
#endif
    pthread_mutex_destroy(&echo->mutex);
    free(echo);
}

void audio_echo_reference(audio_echo_t *echo, const int16_t *samples,
                           size_t count, uint64_t first_sample_ns)
{
    if(!echo) return;
    uint64_t tick = first_sample_ns / ECHO_SAMPLE_NS;
    pthread_mutex_lock(&echo->mutex);
    for(size_t i = 0; i < count; ++i) {
        size_t slot = (tick + i) % ECHO_HISTORY;
        echo->reference[slot] = samples[i];
        echo->ticks[slot] = tick + i + 1;
    }
    pthread_mutex_unlock(&echo->mutex);
}

void audio_echo_discard(audio_echo_t *echo, uint64_t from_ns)
{
    if(!echo) return;
    uint64_t tick = from_ns / ECHO_SAMPLE_NS;
    pthread_mutex_lock(&echo->mutex);
    for(size_t i = 0; i < ECHO_HISTORY; ++i)
        if(echo->ticks[i] > tick) echo->ticks[i] = 0;
    pthread_mutex_unlock(&echo->mutex);
}

void audio_echo_render_close(audio_echo_render_t *render)
{
    if(render->resampler) speex_resampler_destroy(render->resampler);
    memset(render, 0, sizeof(*render));
}

void audio_echo_render(audio_echo_t *echo, audio_echo_render_t *render,
                        const int16_t *samples, size_t frames, unsigned rate,
                        unsigned channels, uint64_t first_sample_ns)
{
    if(!echo || !rate || (channels != 1 && channels != 2)) return;
    uint64_t tick = first_sample_ns / ECHO_SAMPLE_NS;
    if(!render->started || render->rate != rate || tick + ECHO_FRAME / 2 < render->next_tick ||
       tick > render->next_tick + ECHO_FRAME / 2) {
        if(render->started && render->rate == rate)
            fprintf(stdout, "audio: AEC playback timeline jump_ms=%lld\n",
                ((long long)tick - (long long)render->next_tick) / (ECHO_RATE / 1000));
        audio_echo_render_close(render);
        render->rate = rate;
        render->next_tick = tick;
        render->started = true;
        if(rate != ECHO_RATE) {
            int error;
            render->resampler = speex_resampler_init(1, rate, ECHO_RATE, 4, &error);
            if(!render->resampler) { render->started = false; return; }
            speex_resampler_skip_zeros(render->resampler);
        }
    }
    while(frames) {
        int16_t mono[2048], output[4096];
        size_t chunk = frames < 2048 ? frames : 2048;
        for(size_t i = 0; i < chunk; ++i)
            mono[i] = channels == 1 ? samples[i] :
                (int16_t)(((int32_t)samples[i * 2] + samples[i * 2 + 1]) / 2);
        size_t used = 0;
        while(used < chunk) {
            spx_uint32_t input_count = (spx_uint32_t)(chunk - used), output_count = 4096;
            const int16_t *reference = mono + used;
            if(render->resampler) {
                int error = speex_resampler_process_int(render->resampler, 0, mono + used,
                    &input_count, output, &output_count);
                if(error != RESAMPLER_ERR_SUCCESS || !input_count) return;
                reference = output;
            } else output_count = input_count;
            audio_echo_reference(echo, reference, output_count, render->next_tick * ECHO_SAMPLE_NS);
            render->next_tick += output_count;
            used += input_count;
        }
        samples += chunk * channels;
        frames -= chunk;
    }
}

void audio_echo_process(audio_echo_t *echo, int16_t *microphone,
                         uint64_t first_sample_ns)
{
    audio_echo_process_with_reference(echo, microphone, first_sample_ns, NULL);
}

void audio_echo_process_with_reference(audio_echo_t *echo, int16_t *microphone,
                                        uint64_t first_sample_ns,
                                        int16_t *reference_output)
{
    if(reference_output) memset(reference_output, 0, ECHO_FRAME * sizeof(int16_t));
    if(!echo) return;
    uint64_t tick = first_sample_ns > echo->delay_ns ?
        (first_sample_ns - echo->delay_ns) / ECHO_SAMPLE_NS : 0;
    /* ALSA's sample position advances continuously even when period wakeups
     * jitter. Changing the reference offset each frame prevents convergence. */
    if(!echo->capture_started || tick + ECHO_FRAME * 2 < echo->capture_tick ||
       tick > echo->capture_tick + ECHO_FRAME * 2) {
        if(echo->capture_started) {
            fprintf(stdout, "audio: AEC capture timeline jump_ms=%lld\n",
                ((long long)tick - (long long)echo->capture_tick) / (ECHO_RATE / 1000));
#ifndef BINBOT_HAVE_RKAUDIO_AEC
            speex_echo_state_reset(echo->state);
#else
            void *state = rkaudio_preprocess_init(ECHO_RATE, 16, 1, 1, &echo->rk_params);
            if(state) {
                rkaudio_preprocess_destory(echo->rk_state);
                echo->rk_state = state;
            }
            echo->rk_input_count = echo->rk_head = 0;
            echo->rk_count = RK_FRAME;
            memset(echo->rk_output, 0, sizeof(echo->rk_output));
#endif
        }
        echo->capture_tick = tick;
        echo->capture_started = true;
    }
    tick = echo->capture_tick;
    echo->capture_tick += ECHO_FRAME;
    int16_t reference[ECHO_FRAME], clean[ECHO_FRAME];
    double near_energy = 0, far_energy = 0;
    pthread_mutex_lock(&echo->mutex);
    for(size_t i = 0; i < ECHO_FRAME; ++i) {
        size_t slot = (tick + i) % ECHO_HISTORY;
        reference[i] = echo->ticks[slot] == tick + i + 1 ? echo->reference[slot] : 0;
        far_energy += (double)reference[i] * reference[i];
        near_energy += (double)microphone[i] * microphone[i];
    }
    pthread_mutex_unlock(&echo->mutex);
    if(reference_output) memcpy(reference_output, reference, sizeof(reference));
#ifdef BINBOT_HAVE_RKAUDIO_AEC
    for(size_t i = 0; i < ECHO_FRAME; ++i) {
        /* The board's ADC/amplifier makes the acoustic return about 6x the
         * digital reference. Normalize only inside the SDK to avoid its slow
         * adaptation at that ratio; restore the original uplink level below.
         * Unlike boosting the reference, this cannot clip loud render PCM. */
        echo->rk_input[echo->rk_input_count * 2] = microphone[i] / 8;
        echo->rk_input[echo->rk_input_count * 2 + 1] = reference[i];
        if(++echo->rk_input_count == RK_FRAME) {
            int16_t output[RK_FRAME * 2] = {0};
            int wakeup = 0;
            int bytes = rkaudio_preprocess_short(echo->rk_state, echo->rk_input,
                output, RK_FRAME * 2, &wakeup);
            if(bytes != RK_FRAME * (int)sizeof(int16_t)) {
                fprintf(stderr, "audio: unexpected AEC output size=%d\n", bytes);
                memset(output, 0, sizeof(output));
            }
            for(size_t j = 0; j < RK_FRAME; ++j)
                echo->rk_output[(echo->rk_head + echo->rk_count + j) % (RK_FRAME * 4)] = output[j];
            echo->rk_count += RK_FRAME;
            echo->rk_input_count = 0;
        }
    }
    for(size_t i = 0; i < ECHO_FRAME; ++i) {
        int sample = (int)echo->rk_output[echo->rk_head] * 8;
        clean[i] = (int16_t)(sample > 32767 ? 32767 : sample < -32768 ? -32768 : sample);
        echo->rk_head = (echo->rk_head + 1) % (RK_FRAME * 4);
    }
    echo->rk_count -= ECHO_FRAME;
#else
    speex_echo_cancellation(echo->state, microphone, reference, clean);
#endif
    int16_t linear[ECHO_FRAME];
    if(echo->trace) memcpy(linear, clean, sizeof(linear));
#ifndef BINBOT_HAVE_RKAUDIO_AEC
    speex_preprocess_run(echo->preprocess, clean);
#endif
    if(echo->trace) {
        int16_t trace[ECHO_FRAME * 4];
        for(size_t i = 0; i < ECHO_FRAME; ++i) {
            trace[i * 4] = microphone[i];
            trace[i * 4 + 1] = reference[i];
            trace[i * 4 + 2] = linear[i];
            trace[i * 4 + 3] = clean[i];
        }
        if(fwrite(trace, sizeof(trace), 1, echo->trace) != 1 || ++echo->trace_frames >= 1500) {
            fclose(echo->trace);
            echo->trace = NULL;
        }
    }
    if(far_energy > ECHO_FRAME * 100.0) {
        echo->near_energy += near_energy;
        for(size_t i = 0; i < ECHO_FRAME; ++i)
            echo->clean_energy += (double)clean[i] * clean[i];
        echo->far_frames++;
    }
    memcpy(microphone, clean, sizeof(clean));
    if(++echo->frames % 250 == 0 && echo->far_frames) {
        fprintf(stdout, "audio: AEC far_frames=%u attenuation_db=%.1f\n", echo->far_frames,
            10 * log10((echo->near_energy + 1) / (echo->clean_energy + 1)));
        echo->far_frames = 0;
        echo->near_energy = echo->clean_energy = 0;
    }
}
