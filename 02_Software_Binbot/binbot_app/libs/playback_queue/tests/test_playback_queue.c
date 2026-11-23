#include "binbot/audio/playback_queue.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void make_stream(uint8_t stream[16], uint8_t value)
{
    memset(stream, value, 16);
}

static binbot_audio_playback_frame_t make_frame(const uint8_t stream[16], int16_t value)
{
    binbot_audio_playback_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    memcpy(frame.stream_id, stream, 16);
    frame.sample_count = BINBOT_AUDIO_PLAYBACK_FRAME_SAMPLES;
    for(size_t i = 0; i < frame.sample_count; ++i) frame.samples[i] = value;
    return frame;
}

int main(void)
{
    uint8_t first[16];
    uint8_t second[16];
    make_stream(first, 0x11);
    make_stream(second, 0x22);

    binbot_playback_queue_t queue;
    binbot_playback_queue_init(&queue);
    binbot_playback_queue_begin(&queue, first);
    binbot_audio_playback_frame_t frame = make_frame(first, 7);
    for(size_t i = 0; i < BINBOT_PLAYBACK_QUEUE_CAPACITY; ++i) {
        assert(binbot_playback_queue_push(&queue, &frame));
    }
    assert(!binbot_playback_queue_push(&queue, &frame));
    assert(binbot_playback_queue_count(&queue) == BINBOT_PLAYBACK_QUEUE_CAPACITY);

    assert(binbot_playback_queue_cancel(&queue, first));
    assert(binbot_playback_queue_count(&queue) == 0);
    assert(!binbot_playback_queue_push(&queue, &frame));

    binbot_playback_queue_begin(&queue, second);
    assert(!binbot_playback_queue_push(&queue, &frame));
    frame = make_frame(second, 9);
    assert(binbot_playback_queue_push(&queue, &frame));
    assert(binbot_playback_queue_end(&queue, second));
    assert(!binbot_playback_queue_push(&queue, &frame));
    binbot_playback_queue_frame_t output;
    assert(binbot_playback_queue_pop(&queue, &output));
    assert(output.samples[0] == 9);
    assert(binbot_playback_queue_should_finish(&queue));
    binbot_playback_queue_finish(&queue);
    assert(!queue.active);

    puts("playback queue tests passed");
    return 0;
}
