#ifndef BINBOT_AUDIO_NOISE_H
#define BINBOT_AUDIO_NOISE_H

#include "binbot/audio/protocol.h"
#include <stddef.h>

typedef struct {
    uint32_t random;
    float pink[3];
    float brown;
    float b0, b1, b2, a1, a2, z1, z2;
    float phase;
    float gain;
} audio_noise_voice_t;

typedef struct {
    audio_noise_voice_t voices[BINBOT_AUDIO_NOISE_COUNT];
    float master;
} audio_noise_t;

void audio_noise_init(audio_noise_t *noise);
void audio_noise_render(audio_noise_t *noise, const binbot_audio_noise_state_t *state,
                         int16_t *samples, size_t count);

#endif
