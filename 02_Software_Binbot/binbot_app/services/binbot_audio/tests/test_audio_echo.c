#include "echo.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { FRAME = BINBOT_AUDIO_FRAME_SAMPLES, RATE = BINBOT_AUDIO_SAMPLE_RATE };

static uint32_t random_state = 912345;

static float random_sample(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return ((float)(random_state & 65535) - 32768) / 32768;
}

static void cancellation_and_doubletalk(float near_gain, float path_gain)
{
    audio_echo_t *echo = audio_echo_create(0);
    assert(echo);
    int16_t *far = calloc(RATE * 20, sizeof(*far));
    assert(far);
    float previous = 0;
    for(unsigned i = 0; i < RATE * 20; ++i) {
        previous = previous * 0.6f + random_sample() * 0.4f;
        far[i] = (int16_t)(previous * (path_gain > 1.0f ? 2000 : 16000));
    }
    double before = 0, after = 0, near_energy = 0, mixed_output = 0;
    const uint64_t start = 10000000000ULL;
    for(unsigned block = 0; block < 800; ++block) {
        int16_t mic[FRAME];
        unsigned offset = block * FRAME;
        audio_echo_reference(echo, far + offset, FRAME, start + (uint64_t)block * 20000000);
        for(unsigned i = 0; i < FRAME; ++i) {
            unsigned pos = offset + i;
            float acoustic = pos >= 560 ? 0.5f * path_gain * far[pos - 560] : 0;
            if(pos >= 1120) acoustic += 0.15f * path_gain * far[pos - 1120];
            float near = 0;
            if(block >= 500) {
                double t = (double)pos / RATE;
                double envelope = 0.5 + 0.5 * sin(2 * 3.141592653589793 * 3.1 * t);
                near = (float)(near_gain * envelope * (1800 * sin(2 * 3.141592653589793 * 211 * t) +
                    1300 * sin(2 * 3.141592653589793 * 683 * t)));
            }
            mic[i] = (int16_t)(acoustic + near);
            if(block >= 350 && block < 500) before += acoustic * acoustic;
            if(block >= 600) near_energy += near * near;
        }
        int64_t timestamp_jitter = block ? ((int)(block % 5) - 2) * 200000LL : 0;
        audio_echo_process(echo, mic, start + (uint64_t)block * 20000000 + timestamp_jitter);
        for(unsigned i = 0; i < FRAME; ++i) {
            if(block >= 350 && block < 500) after += (double)mic[i] * mic[i];
            if(block >= 600) mixed_output += (double)mic[i] * mic[i];
        }
    }
    double reduction_db = 10 * log10((before + 1) / (after + 1));
    double retained = sqrt(mixed_output / near_energy);
    printf("near gain=%.2f path gain=%.1f; echo-only reduction=%.1f dB; doubletalk retained RMS=%.2f\n",
           near_gain, path_gain, reduction_db, retained);
    assert(reduction_db > 18);
    assert(retained > (near_gain < 1.0f ? 0.50 : 0.55) && retained < 1.6);
    audio_echo_destroy(echo);
    free(far);
}

static void cancellation_with_resampled_playback(void)
{
    audio_echo_t *echo = audio_echo_create(0);
    assert(echo);
    audio_echo_render_t render = {0};
    double before = 0, after = 0;
    const uint64_t start = 10000000000ULL;
    for(unsigned block = 0; block < 650; ++block) {
        int16_t stereo[480 * 2], mic[FRAME];
        for(unsigned i = 0; i < 480; ++i) {
            double t = (double)(block * 480 + i) / 24000;
            stereo[i * 2] = stereo[i * 2 + 1] = (int16_t)(6000 * sin(2 * 3.141592653589793 * 337 * t));
        }
        for(unsigned i = 0; i < FRAME; ++i) {
            double t = (double)(block * FRAME + i) / RATE;
            mic[i] = (int16_t)(2700 * sin(2 * 3.141592653589793 * 337 * (t - 0.045)));
            if(block >= 500) before += (double)mic[i] * mic[i];
        }
        audio_echo_render(echo, &render, stereo, 480, 24000, 2,
            start + (uint64_t)block * 20000000);
        audio_echo_process(echo, mic, start + (uint64_t)block * 20000000);
        if(block >= 500)
            for(unsigned i = 0; i < FRAME; ++i) after += (double)mic[i] * mic[i];
    }
    printf("24 kHz stereo reference reduction=%.1f dB\n", 10 * log10((before + 1) / (after + 1)));
    assert(after < before * 0.02);
    audio_echo_discard(echo, start + 650ULL * 20000000);
    audio_echo_render_close(&render);
    audio_echo_destroy(echo);
}

static void exported_reference_uses_capture_timeline(void)
{
    audio_echo_t *echo = audio_echo_create(40);
    assert(echo);
    const uint64_t start = 10000000000ULL;
    for(unsigned block = 0; block < 4; ++block) {
        int16_t rendered[FRAME], reference[FRAME], mic[FRAME] = {0};
        for(unsigned i = 0; i < FRAME; ++i) rendered[i] = (int16_t)(block * FRAME + i - 700);
        audio_echo_reference(echo, rendered, FRAME, start + block * 20000000ULL);
        int64_t jitter = block ? 200000 : 0;
        audio_echo_process_with_reference(echo, mic,
            start + block * 20000000ULL + 40000000ULL + jitter, reference);
        assert(!memcmp(rendered, reference, sizeof(reference)));
    }
    audio_echo_destroy(echo);
    int16_t mic[FRAME] = {123}, reference[FRAME];
    memset(reference, 1, sizeof(reference));
    audio_echo_process_with_reference(NULL, mic, start, reference);
    for(unsigned i = 0; i < FRAME; ++i) assert(reference[i] == 0);
    assert(mic[0] == 123);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    assert(!audio_echo_create(501));
    cancellation_and_doubletalk(1.0f, 1.0f);
    cancellation_and_doubletalk(0.25f, 1.0f);
    if(!strcmp(audio_echo_engine(), "rockchip_aec_anr"))
        cancellation_and_doubletalk(0.25f, 12.0f);
    cancellation_with_resampled_playback();
    exported_reference_uses_capture_timeline();
    puts("audio echo tests passed");
    return 0;
}
