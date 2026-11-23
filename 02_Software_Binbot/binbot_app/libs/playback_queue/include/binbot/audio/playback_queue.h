#ifndef BINBOT_AUDIO_PLAYBACK_QUEUE_H
#define BINBOT_AUDIO_PLAYBACK_QUEUE_H

#include "binbot/audio/protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { BINBOT_PLAYBACK_QUEUE_CAPACITY = 50 };

typedef struct {
    uint8_t stream_id[16];
    uint16_t sample_count;
    int16_t samples[BINBOT_AUDIO_PLAYBACK_FRAME_SAMPLES];
} binbot_playback_queue_frame_t;

typedef struct {
    binbot_playback_queue_frame_t frames[BINBOT_PLAYBACK_QUEUE_CAPACITY];
    size_t head;
    size_t count;
    bool active;
    bool ended;
    uint8_t stream_id[16];
} binbot_playback_queue_t;

void binbot_playback_queue_init(binbot_playback_queue_t *queue);
void binbot_playback_queue_begin(binbot_playback_queue_t *queue,
                                 const uint8_t stream_id[16]);
bool binbot_playback_queue_push(binbot_playback_queue_t *queue,
                                const binbot_audio_playback_frame_t *frame);
bool binbot_playback_queue_pop(binbot_playback_queue_t *queue,
                               binbot_playback_queue_frame_t *frame);
bool binbot_playback_queue_end(binbot_playback_queue_t *queue,
                               const uint8_t stream_id[16]);
bool binbot_playback_queue_cancel(binbot_playback_queue_t *queue,
                                  const uint8_t stream_id[16]);
bool binbot_playback_queue_should_finish(const binbot_playback_queue_t *queue);
void binbot_playback_queue_finish(binbot_playback_queue_t *queue);
size_t binbot_playback_queue_count(const binbot_playback_queue_t *queue);

#ifdef __cplusplus
}
#endif

#endif
