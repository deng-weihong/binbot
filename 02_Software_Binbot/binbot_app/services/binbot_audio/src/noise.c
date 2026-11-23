#include "noise.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

enum { WHITE, PINK, BROWN, LOWPASS, BANDPASS, HIGHPASS };
static const struct {
    unsigned kind, filter;
    float hz, q, lfo, depth;
} sounds[BINBOT_AUDIO_NOISE_COUNT] = {
    {WHITE, BANDPASS, 2600, .45f, 0, 0},
    {PINK, LOWPASS, 700, .707f, .09f, .85f},
    {BROWN, LOWPASS, 900, .707f, 2.7f, .25f},
    {PINK, BANDPASS, 1800, .9f, .22f, .4f},
    {PINK, BANDPASS, 650, .7f, .6f, .2f},
    {PINK, BANDPASS, 420, 1.6f, .15f, .7f},
    {BROWN, LOWPASS, 260, .707f, .05f, .9f},
    {WHITE, BANDPASS, 1400, .8f, 1.3f, .2f},
    {WHITE, HIGHPASS, 3200, .707f, 7, .7f},
};

void audio_noise_init(audio_noise_t *noise)
{
    memset(noise, 0, sizeof(*noise));
    for(unsigned i = 0; i < BINBOT_AUDIO_NOISE_COUNT; ++i) {
        audio_noise_voice_t *v = &noise->voices[i];
        v->random = 0x9e3779b9U ^ (0x85ebca6bU * (i + 1U));
        float w = 6.283185307f * sounds[i].hz / BINBOT_AUDIO_PLAYBACK_SAMPLE_RATE;
        float c = cosf(w), alpha = sinf(w) / (2 * sounds[i].q), a0 = 1 + alpha;
        /* RBJ biquad coefficients, matching the prototype's Web Audio filters. */
        if(sounds[i].filter == BANDPASS) {
            v->b0 = alpha / a0; v->b1 = 0; v->b2 = -alpha / a0;
        }
        else {
            bool high = sounds[i].filter == HIGHPASS;
            v->b0 = (1 + (high ? c : -c)) / (2 * a0);
            v->b1 = (high ? -(1 + c) : 1 - c) / a0;
            v->b2 = v->b0;
        }
        v->a1 = -2 * c / a0; v->a2 = (1 - alpha) / a0;
    }
}

void audio_noise_render(audio_noise_t *noise, const binbot_audio_noise_state_t *state,
                         int16_t *samples, size_t count)
{
    if(!noise || !state || !samples || !count) return;
    float modulation[BINBOT_AUDIO_NOISE_COUNT], step[BINBOT_AUDIO_NOISE_COUNT];
    for(unsigned i = 0; i < BINBOT_AUDIO_NOISE_COUNT; ++i) {
        audio_noise_voice_t *v = &noise->voices[i];
        float next = v->phase + 6.283185307f * sounds[i].lfo * count /
                                  BINBOT_AUDIO_PLAYBACK_SAMPLE_RATE;
        float depth = sounds[i].depth / 2;
        modulation[i] = 1 - depth + depth * sinf(v->phase);
        step[i] = depth * (sinf(next) - sinf(v->phase)) / count;
        v->phase = fmodf(next, 6.283185307f);
    }
    for(size_t s = 0; s < count; ++s) {
        float mixed = 0;
        for(unsigned i = 0; i < BINBOT_AUDIO_NOISE_COUNT; ++i) {
            audio_noise_voice_t *v = &noise->voices[i];
            v->gain += (state->levels[i] / 100.0f - v->gain) * .0002083f;
            modulation[i] += step[i];
            if(v->gain < .00001f) continue;
            uint32_t r = v->random;
            r ^= r << 13; r ^= r >> 17; r ^= r << 5; v->random = r;
            float w = (r >> 8) / 8388608.0f - 1, value;
            if(sounds[i].kind == WHITE) value = w * .5f;
            else if(sounds[i].kind == PINK) {
                v->pink[0] = .99765f * v->pink[0] + w * .099046f;
                v->pink[1] = .963f * v->pink[1] + w * .2965164f;
                v->pink[2] = .57f * v->pink[2] + w * 1.0526913f;
                value = (v->pink[0] + v->pink[1] + v->pink[2] + w * .1848f) * .16f;
            }
            else { v->brown = (v->brown + .02f * w) / 1.02f; value = v->brown * 3.2f; }
            float output = v->b0 * value + v->z1;
            v->z1 = v->b1 * value - v->a1 * output + v->z2;
            v->z2 = v->b2 * value - v->a2 * output;
            mixed += output * v->gain * modulation[i];
        }
        float target = state->playing ? state->master / 100.0f : 0;
        noise->master += (target - noise->master) * .0002778f;
        mixed *= noise->master * .65f;
        samples[s] = (int16_t)(fmaxf(-1, fminf(1, mixed)) * 32767);
    }
}
