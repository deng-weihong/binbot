#include "audio/fbank.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

struct audio_fbank {
    audio_fbank_config_t config;
    float *window;
    float *cosine;
    float *sine;
    uint16_t boundaries[AUDIO_FBANK_FEATURE_BINS + 2];
    float history[AUDIO_FBANK_HISTORY_SAMPLES];
    float previous_sample;
    unsigned input_frames;
};

static float mel(float frequency)
{
    return 2595.0f * log10f(1.0f + frequency / 700.0f);
}

static float inverse_mel(float value)
{
    return 700.0f * (powf(10.0f, value / 2595.0f) - 1.0f);
}

audio_fbank_config_t audio_fbank_default_config(void)
{
    return (audio_fbank_config_t){
        .sample_rate_hz = AUDIO_FBANK_SAMPLE_RATE,
        .input_frame_samples = AUDIO_FBANK_INPUT_FRAME_SAMPLES,
        .dft_samples = AUDIO_FBANK_DFT_SAMPLES,
        .feature_bins = AUDIO_FBANK_FEATURE_BINS,
        .low_frequency_hz = 100.0f,
        .high_frequency_hz = 7800.0f,
        .preemphasis = 0.95f,
        .calibration_db = 6.8023f,
    };
}

void audio_fbank_destroy(audio_fbank_t *fbank)
{
    if(!fbank) return;
    free(fbank->window);
    free(fbank->cosine);
    free(fbank->sine);
    free(fbank);
}

audio_fbank_t *audio_fbank_create(
    const audio_fbank_config_t *config)
{
    if(!config || config->sample_rate_hz != 16000 ||
       config->input_frame_samples != 320 || config->dft_samples != 640 ||
       config->feature_bins != 40 || config->low_frequency_hz <= 0.0f ||
       config->high_frequency_hz >= 8000.0f ||
       config->high_frequency_hz <= config->low_frequency_hz ||
       !isfinite(config->low_frequency_hz) ||
       !isfinite(config->high_frequency_hz) ||
       !isfinite(config->preemphasis) ||
       !isfinite(config->calibration_db) ||
       config->preemphasis < 0.0f || config->preemphasis >= 1.0f) {
        return NULL;
    }
    audio_fbank_t *fbank = calloc(1, sizeof(*fbank));
    if(!fbank) return NULL;
    fbank->config = *config;
    const size_t spectrum_size = (size_t)(config->dft_samples / 2U) *
                                 config->dft_samples;
    fbank->window = calloc(config->dft_samples, sizeof(*fbank->window));
    fbank->cosine = calloc(spectrum_size, sizeof(*fbank->cosine));
    fbank->sine = calloc(spectrum_size, sizeof(*fbank->sine));
    if(!fbank->window || !fbank->cosine || !fbank->sine) {
        audio_fbank_destroy(fbank);
        return NULL;
    }
    const float pi = 3.14159265358979323846f;
    for(size_t sample = 0; sample < config->dft_samples; ++sample) {
        fbank->window[sample] = sqrtf(0.5f - 0.5f *
            cosf(2.0f * pi * (float)sample / (float)config->dft_samples));
    }
    for(size_t feature = 0; feature < config->dft_samples / 2U; ++feature) {
        for(size_t sample = 0; sample < config->dft_samples; ++sample) {
            float angle = 2.0f * pi * (float)feature * (float)sample /
                          (float)config->dft_samples;
            fbank->cosine[feature * config->dft_samples + sample] = cosf(angle);
            fbank->sine[feature * config->dft_samples + sample] = sinf(angle);
        }
    }
    const float low_mel = mel(config->low_frequency_hz);
    const float high_mel = mel(config->high_frequency_hz);
    for(size_t index = 0; index < config->feature_bins + 2; ++index) {
        float value = low_mel + (high_mel - low_mel) * (float)index /
                      (float)(config->feature_bins + 1);
        float frequency = inverse_mel(value);
        int boundary = (int)floorf(frequency * (float)config->dft_samples /
                                   (float)config->sample_rate_hz + 0.5f);
        if(boundary < 0) boundary = 0;
        if(boundary >= (int)config->dft_samples / 2) {
            boundary = (int)config->dft_samples / 2 - 1;
        }
        fbank->boundaries[index] = (uint16_t)boundary;
    }
    audio_fbank_reset(fbank);
    return fbank;
}

void audio_fbank_reset(audio_fbank_t *fbank)
{
    if(!fbank) return;
    memset(fbank->history, 0, sizeof(fbank->history));
    fbank->previous_sample = 0.0f;
    fbank->input_frames = 0;
}

bool audio_fbank_process(
    audio_fbank_t *fbank,
    const int16_t *samples,
    size_t sample_count,
    float *features,
    size_t feature_capacity)
{
    if(!fbank || !samples || !features ||
       sample_count != fbank->config.input_frame_samples ||
       feature_capacity < fbank->config.feature_bins) {
        return false;
    }
    memmove(fbank->history, fbank->history + sample_count,
            (AUDIO_FBANK_HISTORY_SAMPLES - sample_count) * sizeof(float));
    for(size_t index = 0; index < sample_count; ++index) {
        float current = (float)samples[index] / 32768.0f;
        fbank->history[AUDIO_FBANK_HISTORY_SAMPLES - sample_count + index] =
            current - fbank->config.preemphasis * fbank->previous_sample;
        fbank->previous_sample = current;
    }
    if((++fbank->input_frames & 1U) != 0U) return false;

    float power[AUDIO_FBANK_DFT_SAMPLES / 2];
    for(size_t bin = 0; bin < fbank->config.dft_samples / 2; ++bin) {
        float real = 0.0f;
        float imaginary = 0.0f;
        /* The current KWS model uses 40 Mel bins; this direct DFT keeps the
         * reference numerically transparent and can later be replaced by an
         * FFT implementation without changing the public feature contract. */
        const float *cosine = &fbank->cosine[bin * fbank->config.dft_samples];
        const float *sine = &fbank->sine[bin * fbank->config.dft_samples];
        for(size_t sample = 0; sample < fbank->config.dft_samples; ++sample) {
            float value = fbank->history[sample] * fbank->window[sample];
            real += value * cosine[sample];
            imaginary -= value * sine[sample];
        }
        power[bin] = real * real + imaginary * imaginary;
    }
    for(size_t feature = 0; feature < fbank->config.feature_bins; ++feature) {
        int left = fbank->boundaries[feature];
        int center = fbank->boundaries[feature + 1];
        int right = fbank->boundaries[feature + 2];
        float energy = 0.0f;
        for(int bin = left; bin < center; ++bin) {
            if(center != left) energy += power[bin] * (float)(bin - left) /
                                             (float)(center - left);
        }
        for(int bin = center; bin < right; ++bin) {
            if(right != center) energy += power[bin] * (float)(right - bin) /
                                             (float)(right - center);
        }
        float value = energy > 0.0f ? 10.0f * log10f(energy) : -75.0f;
        if(value < -75.0f) value = -75.0f;
        features[feature] = value + fbank->config.calibration_db;
    }
    return true;
}
