#ifndef BINBOT_AUDIO_ECHO_H
#define BINBOT_AUDIO_ECHO_H

#include "binbot/audio/protocol.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct audio_echo audio_echo_t;
typedef struct SpeexResamplerState_ SpeexResamplerState;

typedef struct {
    SpeexResamplerState *resampler;
    unsigned rate;
    uint64_t next_tick;
    bool started;
} audio_echo_render_t;

audio_echo_t *audio_echo_create(unsigned delay_ms);
const char *audio_echo_engine(void);
void audio_echo_destroy(audio_echo_t *echo);
void audio_echo_reference(audio_echo_t *echo, const int16_t *samples,
                           size_t count, uint64_t first_sample_ns);
void audio_echo_render(audio_echo_t *echo, audio_echo_render_t *render,
                        const int16_t *samples, size_t frames, unsigned rate,
                        unsigned channels, uint64_t first_sample_ns);
void audio_echo_render_close(audio_echo_render_t *render);
void audio_echo_discard(audio_echo_t *echo, uint64_t from_ns);
void audio_echo_process(audio_echo_t *echo, int16_t *microphone,
                         uint64_t first_sample_ns);
/* Same capture timeline as audio_echo_process; optionally export the aligned
 * render reference for independent multichannel DSP. Never modifies raw mics. */
void audio_echo_process_with_reference(audio_echo_t *echo, int16_t *microphone,
                                        uint64_t first_sample_ns,
                                        int16_t *reference_output);

#endif
