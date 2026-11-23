#include "binbot/audio/playback_queue.h"

#include <string.h>

static bool stream_matches(const binbot_playback_queue_t *queue,
                           const uint8_t stream_id[16])
{
    return queue->active && memcmp(queue->stream_id, stream_id, 16) == 0;
}

void binbot_playback_queue_init(binbot_playback_queue_t *queue)
{
    memset(queue, 0, sizeof(*queue));
}

void binbot_playback_queue_begin(binbot_playback_queue_t *queue,
                                 const uint8_t stream_id[16])
{
    queue->head = 0;
    queue->count = 0;
    queue->active = true;
    queue->ended = false;
    memcpy(queue->stream_id, stream_id, sizeof(queue->stream_id));
}

bool binbot_playback_queue_push(binbot_playback_queue_t *queue,
                                const binbot_audio_playback_frame_t *frame)
{
    if(!stream_matches(queue, frame->stream_id) || queue->ended ||
       frame->sample_count == 0 ||
       frame->sample_count > BINBOT_AUDIO_PLAYBACK_FRAME_SAMPLES ||
       queue->count >= BINBOT_PLAYBACK_QUEUE_CAPACITY) {
        return false;
    }
    size_t tail = (queue->head + queue->count) % BINBOT_PLAYBACK_QUEUE_CAPACITY;
    binbot_playback_queue_frame_t *destination = &queue->frames[tail];
    memcpy(destination->stream_id, frame->stream_id, sizeof(destination->stream_id));
    destination->sample_count = frame->sample_count;
    memcpy(destination->samples, frame->samples,
           (size_t)frame->sample_count * sizeof(frame->samples[0]));
    queue->count++;
    return true;
}

bool binbot_playback_queue_pop(binbot_playback_queue_t *queue,
                               binbot_playback_queue_frame_t *frame)
{
    if(queue->count == 0) return false;
    *frame = queue->frames[queue->head];
    queue->head = (queue->head + 1U) % BINBOT_PLAYBACK_QUEUE_CAPACITY;
    queue->count--;
    return true;
}

bool binbot_playback_queue_end(binbot_playback_queue_t *queue,
                               const uint8_t stream_id[16])
{
    if(!stream_matches(queue, stream_id)) return false;
    queue->ended = true;
    return true;
}

bool binbot_playback_queue_cancel(binbot_playback_queue_t *queue,
                                  const uint8_t stream_id[16])
{
    if(!queue->active || (stream_id && !stream_matches(queue, stream_id))) return false;
    queue->head = 0;
    queue->count = 0;
    queue->active = false;
    queue->ended = false;
    memset(queue->stream_id, 0, sizeof(queue->stream_id));
    return true;
}

bool binbot_playback_queue_should_finish(const binbot_playback_queue_t *queue)
{
    return queue->active && queue->ended && queue->count == 0;
}

void binbot_playback_queue_finish(binbot_playback_queue_t *queue)
{
    (void)binbot_playback_queue_cancel(queue, NULL);
}

size_t binbot_playback_queue_count(const binbot_playback_queue_t *queue)
{
    return queue->count;
}
