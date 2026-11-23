#ifndef AUDIO_FBANK_H
#define AUDIO_FBANK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    AUDIO_FBANK_SAMPLE_RATE = 16000,
    AUDIO_FBANK_INPUT_FRAME_SAMPLES = 320,
    AUDIO_FBANK_DFT_SAMPLES = 640,
    AUDIO_FBANK_HISTORY_SAMPLES = 800,
    AUDIO_FBANK_FEATURE_BINS = 40,
};

typedef struct {
    uint32_t sample_rate_hz;
    uint32_t input_frame_samples;
    uint32_t dft_samples;
    uint32_t feature_bins;
    float low_frequency_hz;
    float high_frequency_hz;
    float preemphasis;
    float calibration_db;
} audio_fbank_config_t;

typedef struct audio_fbank audio_fbank_t;

audio_fbank_config_t audio_fbank_default_config(void);
audio_fbank_t *audio_fbank_create(
    const audio_fbank_config_t *config);
void audio_fbank_destroy(audio_fbank_t *fbank);
void audio_fbank_reset(audio_fbank_t *fbank);

/*
 * Consume one 20 ms mono S16_LE frame. Returns true and writes exactly 40
 * floats after every second input frame (one 40 ms analysis frame). Before the
 * first result, the analysis window is left-padded with 160 zeros. The
 * 800-sample streaming history retains a 160-sample look-ahead beyond each
 * 640-sample analysis window, matching the training reference timing.
 * Features are log10 Mel-band energies clamped to -75 dB, plus calibration_db.
 */
bool audio_fbank_process(
    audio_fbank_t *fbank,
    const int16_t *samples,
    size_t sample_count,
    float *features,
    size_t feature_capacity);

#ifdef __cplusplus
}
#endif

#endif
