#ifndef BINBOT_AUDIO_PLAYBACK_H
#define BINBOT_AUDIO_PLAYBACK_H

#include "binbot/audio/playback_queue.h"
#include "echo.h"

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    pthread_t thread;
    pthread_t music_thread;
    pthread_mutex_t mutex;
    pthread_mutex_t device_mutex;
    pthread_cond_t condition;
    bool initialized;
    bool stop;
    bool wake_pending;
    bool wake_playing;
    bool music_thread_started;
    bool music_stop;
    bool music_playing;
    bool music_paused;
    bool music_active;
    uint16_t music_kind;
    uint64_t music_generation;
    uint32_t music_position_ms;
    uint32_t music_seek_ms;
    bool noise_mode;
    binbot_audio_noise_state_t noise;
    uint64_t noise_remaining_ns;
    uint64_t dialog_epoch;
    binbot_playback_queue_t dialog_queue;
    audio_echo_t *echo;
    char device[64];
    char sound_path[256];
    char music_path[256];
    float wake_volume;
    float dialog_volume;
    uint32_t wake_max_ms;
} audio_playback_t;

bool audio_playback_start(audio_playback_t *playback,
                          const char *device,
                          const char *sound_path,
                          float wake_volume,
                          uint32_t wake_max_ms,
                          float dialog_volume,
                          audio_echo_t *echo);
void audio_playback_request(audio_playback_t *playback);
bool audio_playback_music_control(audio_playback_t *playback,
                                  const binbot_audio_music_control_t *control);
bool audio_playback_noise_control(audio_playback_t *playback,
                                  const binbot_audio_noise_state_t *state);
void audio_playback_noise_snapshot(audio_playback_t *playback,
                                   binbot_audio_noise_state_t *state);
bool audio_playback_active(audio_playback_t *playback);
bool audio_playback_dialog_active(audio_playback_t *playback);
bool audio_playback_begin(audio_playback_t *playback,
                          const binbot_audio_playback_begin_t *begin);
bool audio_playback_enqueue(audio_playback_t *playback,
                            const binbot_audio_playback_frame_t *frame);
bool audio_playback_end(audio_playback_t *playback,
                        const uint8_t stream_id[16]);
bool audio_playback_cancel(audio_playback_t *playback,
                           const uint8_t stream_id[16]);
size_t audio_playback_queued(audio_playback_t *playback);
void audio_playback_stop(audio_playback_t *playback);

#endif
