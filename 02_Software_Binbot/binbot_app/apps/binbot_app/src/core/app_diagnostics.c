#include "binbot/app_diagnostics.h"
#include "binbot/audio/client.h"

#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { RECORD_SAMPLES = BINBOT_AUDIO_SAMPLE_RATE * 5 };

struct app_diagnostics {
    pthread_t thread;
    pthread_mutex_t mutex;
    binbot_audio_client_t client;
    app_diag_status_t status;
    bool quit, microphone, stop;
    app_diag_play_t requested;
    int16_t recording[RECORD_SAMPLES * 2];
    unsigned captured, sent, total;
    uint64_t last_pcm, record_start, sequence, next_frame, finish_at;
    uint8_t stream_id[16];
    double phase, frequency;
};

static uint64_t now_ms(void)
{
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (uint64_t)time.tv_sec * 1000 + (uint64_t)time.tv_nsec / 1000000;
}

static bool control(app_diagnostics_t *d, binbot_audio_message_type_t type)
{
    binbot_audio_packet_t packet;
    binbot_audio_packet_init(&packet, type, sizeof(packet.payload.playback_control), 0, 0);
    memcpy(packet.payload.playback_control.stream_id, d->stream_id, 16);
    return binbot_audio_client_send(&d->client, &packet);
}

static void stop_playback(app_diagnostics_t *d)
{
    if(d->status.playback != APP_DIAG_PLAY_NONE)
        (void)control(d, BINBOT_AUDIO_MSG_PLAYBACK_CANCEL);
    d->status.playback = APP_DIAG_PLAY_NONE;
    d->status.playback_ms = 0;
    d->finish_at = 0;
}

static void capture(app_diagnostics_t *d, const binbot_audio_packet_t *packet, uint64_t now)
{
    if(packet->header.type == BINBOT_AUDIO_MSG_DIALOG_STATE &&
       packet->header.payload_size == sizeof(packet->payload.dialog_state)) {
        unsigned state = packet->payload.dialog_state.state;
        d->status.busy = state >= BINBOT_DIALOG_STATE_LISTENING &&
                         state <= BINBOT_DIALOG_STATE_CANCELLING;
        if(!d->status.busy && d->status.error == APP_DIAG_ERROR_BUSY)
            d->status.error = APP_DIAG_ERROR_NONE;
        if(d->status.busy && d->status.playback) {
            stop_playback(d);
            d->status.error = APP_DIAG_ERROR_BUSY;
        }
        return;
    }
    if(packet->header.type != BINBOT_AUDIO_MSG_PCM_STEREO ||
       packet->header.payload_size != sizeof(packet->payload.pcm_stereo)) return;
    d->last_pcm = now;
    if(!d->microphone) return;
    d->status.capture_ready = true;
    if(d->status.error == APP_DIAG_ERROR_OFFLINE || d->status.error == APP_DIAG_ERROR_CAPTURE)
        d->status.error = APP_DIAG_ERROR_NONE;
    const int16_t *samples = packet->payload.pcm_stereo.samples;
    for(unsigned channel = 0; channel < 2; ++channel) {
        double sum = 0;
        for(unsigned i = 0; i < BINBOT_AUDIO_FRAME_SAMPLES; ++i) {
            double sample = samples[i * 2 + channel];
            sum += sample * sample;
        }
        double level = sqrt(sum / BINBOT_AUDIO_FRAME_SAMPLES) * 600 / 32768;
        d->status.level[channel] = (uint8_t)(level > 100 ? 100 : lround(level));
        if(d->status.level[channel] > d->status.peak[channel])
            d->status.peak[channel] = d->status.level[channel];
    }
    if(!d->status.recording) return;
    if(d->captured && packet->header.sequence != d->sequence + 1) {
        d->status.recording = false;
        d->captured = 0;
        d->status.record_ms = 0;
        d->status.error = APP_DIAG_ERROR_GAP;
        return;
    }
    d->sequence = packet->header.sequence;
    memcpy(d->recording + d->captured * 2, samples, sizeof(packet->payload.pcm_stereo.samples));
    d->captured += BINBOT_AUDIO_FRAME_SAMPLES;
    d->status.record_ms = d->captured * 1000 / BINBOT_AUDIO_SAMPLE_RATE;
    if(d->captured == RECORD_SAMPLES) {
        d->status.recording = false;
        d->status.recorded = true;
    }
}

static void begin_playback(app_diagnostics_t *d, uint64_t now)
{
    app_diag_play_t kind = d->requested;
    d->requested = APP_DIAG_PLAY_NONE;
    if(d->status.busy) { d->status.error = APP_DIAG_ERROR_BUSY; return; }
    binbot_audio_packet_t packet;
    binbot_audio_packet_init(&packet, BINBOT_AUDIO_MSG_PLAYBACK_BEGIN,
                             sizeof(packet.payload.playback_begin), 0, 0);
    uint64_t identity = (uint64_t)getpid() << 32 | ++d->sequence;
    memcpy(d->stream_id, &now, 8);
    memcpy(d->stream_id + 8, &identity, 8);
    memcpy(packet.payload.playback_begin.stream_id, d->stream_id, 16);
    packet.payload.playback_begin.sample_rate = BINBOT_AUDIO_PLAYBACK_SAMPLE_RATE;
    packet.payload.playback_begin.channels = 1;
    packet.payload.playback_begin.frame_ms = BINBOT_AUDIO_FRAME_MS;
    if(!binbot_audio_client_send(&d->client, &packet)) {
        d->status.error = APP_DIAG_ERROR_PLAYBACK;
        return;
    }
    d->status.playback = kind;
    d->status.playback_ms = 0;
    d->status.error = APP_DIAG_ERROR_NONE;
    d->sent = 0;
    d->total = BINBOT_AUDIO_PLAYBACK_SAMPLE_RATE * (kind == APP_DIAG_PLAY_TONE ? 3 : 5);
    d->phase = 0;
    d->frequency = kind == APP_DIAG_PLAY_TONE ? 1000 : 200;
    d->next_frame = now;
    d->finish_at = 0;
}

static void playback_frame(app_diagnostics_t *d, uint64_t now)
{
    if(d->finish_at) {
        if(now >= d->finish_at) {
            d->status.playback = APP_DIAG_PLAY_NONE;
            d->finish_at = 0;
        }
        return;
    }
    if(now < d->next_frame) return;
    binbot_audio_packet_t packet;
    binbot_audio_packet_init(&packet, BINBOT_AUDIO_MSG_PLAYBACK_FRAME,
                             sizeof(packet.payload.playback_frame), 0, 0);
    binbot_audio_playback_frame_t *frame = &packet.payload.playback_frame;
    memcpy(frame->stream_id, d->stream_id, 16);
    frame->sample_count = BINBOT_AUDIO_PLAYBACK_FRAME_SAMPLES;
    double phase = d->phase, frequency = d->frequency;
    const double sweep_step = pow(40.0, 1.0 / (5 * BINBOT_AUDIO_PLAYBACK_SAMPLE_RATE));
    for(unsigned i = 0; i < frame->sample_count; ++i) {
        unsigned index = d->sent + i;
        if(d->status.playback == APP_DIAG_PLAY_LEFT || d->status.playback == APP_DIAG_PLAY_RIGHT) {
            /* The capture is 16 kHz; the shared speaker stream is 24 kHz mono. */
            unsigned numerator = index * 2, pos = numerator / 3, fraction = numerator % 3;
            unsigned next = pos + 1 < RECORD_SAMPLES ? pos + 1 : pos;
            unsigned ch = d->status.playback == APP_DIAG_PLAY_RIGHT;
            frame->samples[i] = (int16_t)((d->recording[pos * 2 + ch] * (int)(3 - fraction) +
                                           d->recording[next * 2 + ch] * (int)fraction) / 3);
        }
        else {
            double fade_in = (double)index / (BINBOT_AUDIO_PLAYBACK_SAMPLE_RATE * .05);
            double fade_out = (double)(d->total - index) / (BINBOT_AUDIO_PLAYBACK_SAMPLE_RATE * .1);
            double gain = fmin(1, fmin(fade_in, fade_out)) * .35;
            frame->samples[i] = (int16_t)lround(sin(phase) * gain * 32767);
            phase = fmod(phase + 6.283185307179586 * frequency / BINBOT_AUDIO_PLAYBACK_SAMPLE_RATE,
                         6.283185307179586);
            if(d->status.playback == APP_DIAG_PLAY_SWEEP) frequency *= sweep_step;
        }
    }
    if(!binbot_audio_client_send(&d->client, &packet)) {
        stop_playback(d);
        d->status.error = APP_DIAG_ERROR_PLAYBACK;
        return;
    }
    d->phase = phase;
    d->frequency = frequency;
    d->sent += frame->sample_count;
    d->status.playback_ms = d->sent * 1000 / BINBOT_AUDIO_PLAYBACK_SAMPLE_RATE;
    /* Keep capture wakeups and signal generation time from accumulating drift. */
    d->next_frame += BINBOT_AUDIO_FRAME_MS;
    if(d->sent == d->total) {
        if(!control(d, BINBOT_AUDIO_MSG_PLAYBACK_END)) {
            stop_playback(d);
            d->status.error = APP_DIAG_ERROR_PLAYBACK;
        }
        else d->finish_at = now + 100;
    }
}

static void *worker(void *data)
{
    app_diagnostics_t *d = data;
    for(;;) {
        pthread_mutex_lock(&d->mutex);
        if(d->quit) { stop_playback(d); pthread_mutex_unlock(&d->mutex); break; }
        uint64_t now = now_ms();
        d->status.connected = binbot_audio_client_connect(&d->client, now);
        if(d->stop) { stop_playback(d); d->stop = false; }
        if(d->status.connected) {
            binbot_audio_packet_t packet;
            for(unsigned i = 0; i < 64; ++i) {
                int result = binbot_audio_client_receive(&d->client, &packet);
                if(result <= 0) break;
                capture(d, &packet, now);
            }
        }
        if(d->client.fd < 0) {
            d->status.connected = d->status.capture_ready = d->status.recording = false;
            d->status.error = APP_DIAG_ERROR_OFFLINE;
            d->status.playback = APP_DIAG_PLAY_NONE;
            d->requested = APP_DIAG_PLAY_NONE;
            memset(d->status.level, 0, sizeof(d->status.level));
        }
        else {
            if(d->requested) begin_playback(d, now);
            if(d->status.playback) playback_frame(d, now);
            if(d->microphone && (!d->last_pcm || now - d->last_pcm > 500)) {
                d->status.capture_ready = false;
                memset(d->status.level, 0, sizeof(d->status.level));
                d->status.error = APP_DIAG_ERROR_CAPTURE;
            }
            if(d->status.recording && now - d->record_start > 7000) {
                d->status.recording = false;
                d->captured = 0;
                d->status.error = APP_DIAG_ERROR_CAPTURE;
            }
        }
        struct pollfd descriptor = {.fd = d->client.fd, .events = POLLIN};
        pthread_mutex_unlock(&d->mutex);
        poll(&descriptor, 1, 10);
    }
    binbot_audio_client_close(&d->client);
    return NULL;
}

app_diagnostics_t *app_diagnostics_create(const char *socket_path)
{
    app_diagnostics_t *d = calloc(1, sizeof(*d));
    if(!d) return NULL;
    if(pthread_mutex_init(&d->mutex, NULL)) { free(d); return NULL; }
    binbot_audio_client_init(&d->client, BINBOT_AUDIO_ROLE_APP,
        BINBOT_AUDIO_CAP_PCM_STEREO_LIVE | BINBOT_AUDIO_CAP_DIALOG_STATE |
        BINBOT_AUDIO_CAP_DIAGNOSTICS, 0, socket_path);
    if(pthread_create(&d->thread, NULL, worker, d)) {
        pthread_mutex_destroy(&d->mutex); free(d); return NULL;
    }
    return d;
}

void app_diagnostics_destroy(app_diagnostics_t *d)
{
    if(!d) return;
    pthread_mutex_lock(&d->mutex); d->quit = true; pthread_mutex_unlock(&d->mutex);
    pthread_join(d->thread, NULL);
    pthread_mutex_destroy(&d->mutex);
    free(d);
}

void app_diagnostics_set_microphone(app_diagnostics_t *d, bool enabled)
{
    if(!d) return;
    pthread_mutex_lock(&d->mutex);
    if(d->microphone != enabled) {
        d->microphone = enabled;
        d->status.capture_ready = false;
        d->status.recording = false;
        d->status.recorded = false;
        d->status.error = d->status.connected ? APP_DIAG_ERROR_NONE : APP_DIAG_ERROR_OFFLINE;
        d->captured = d->status.record_ms = 0;
        d->stop = true;
        d->requested = APP_DIAG_PLAY_NONE;
        memset(d->status.level, 0, sizeof(d->status.level));
        memset(d->status.peak, 0, sizeof(d->status.peak));
    }
    pthread_mutex_unlock(&d->mutex);
}

bool app_diagnostics_record(app_diagnostics_t *d)
{
    if(!d) return false;
    pthread_mutex_lock(&d->mutex);
    bool ready = d->microphone && d->status.capture_ready && !d->status.recording;
    if(ready) {
        d->stop = true; d->requested = APP_DIAG_PLAY_NONE;
        d->status.recording = true; d->status.recorded = false;
        d->status.record_ms = d->captured = 0;
        d->status.error = APP_DIAG_ERROR_NONE;
        d->record_start = now_ms();
    }
    pthread_mutex_unlock(&d->mutex);
    return ready;
}

bool app_diagnostics_play(app_diagnostics_t *d, app_diag_play_t kind)
{
    if(!d || kind < APP_DIAG_PLAY_LEFT || kind > APP_DIAG_PLAY_SWEEP) return false;
    pthread_mutex_lock(&d->mutex);
    bool ready = d->status.connected && !d->status.busy && !d->status.recording &&
        ((kind >= APP_DIAG_PLAY_TONE) || d->status.recorded);
    if(ready) { d->stop = true; d->requested = kind; }
    else if(d->status.busy) d->status.error = APP_DIAG_ERROR_BUSY;
    pthread_mutex_unlock(&d->mutex);
    return ready;
}

void app_diagnostics_stop(app_diagnostics_t *d)
{
    if(!d) return;
    pthread_mutex_lock(&d->mutex);
    d->stop = true; d->requested = APP_DIAG_PLAY_NONE;
    pthread_mutex_unlock(&d->mutex);
}

app_diag_status_t app_diagnostics_status(app_diagnostics_t *d)
{
    if(!d) return (app_diag_status_t){.error = APP_DIAG_ERROR_OFFLINE};
    pthread_mutex_lock(&d->mutex);
    app_diag_status_t status = d->status;
    pthread_mutex_unlock(&d->mutex);
    return status;
}
