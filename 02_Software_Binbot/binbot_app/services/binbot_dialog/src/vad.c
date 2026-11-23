#include "vad.h"

#include <math.h>
#include <string.h>

bool binbot_vad_init(binbot_vad_t *vad, const binbot_vad_config_t *config)
{
    memset(vad, 0, sizeof(*vad));
    if((config->frame_ms != 10 && config->frame_ms != 20 && config->frame_ms != 30) ||
       config->mode > 3 || !config->start_frames) return false;
    vad->config = *config;
    vad->detector = fvad_new();
    if(!vad->detector) return false;
    if(fvad_set_sample_rate(vad->detector, 16000) ||
       fvad_set_mode(vad->detector, config->mode)) {
        binbot_vad_destroy(vad);
        return false;
    }
    return true;
}

void binbot_vad_destroy(binbot_vad_t *vad)
{
    if(vad->detector) fvad_free(vad->detector);
    vad->detector = NULL;
}

void binbot_vad_reset(binbot_vad_t *vad)
{
    Fvad *detector = vad->detector;
    binbot_vad_config_t config = vad->config;
    memset(vad, 0, sizeof(*vad));
    vad->detector = detector;
    vad->config = config;
}

void binbot_vad_observe_noise(binbot_vad_t *vad, const int16_t *samples, size_t count)
{
    if(vad && vad->detector && samples && count == vad->config.frame_ms * 16)
        (void)fvad_process(vad->detector, samples, count);
}

void binbot_vad_note_transcript(binbot_vad_t *vad)
{
    vad->transcript_received = true;
    vad->speech_started = true;
    vad->last_transcript_ms = vad->elapsed_ms;
}

binbot_vad_result_t binbot_vad_process(binbot_vad_t *vad,
                                       const int16_t *samples, size_t count)
{
    if(!vad || !vad->detector || !samples || count != vad->config.frame_ms * 16)
        return BINBOT_VAD_WAITING;
    double sum = 0;
    for(size_t i = 0; i < count; ++i) {
        double value = samples[i];
        sum += value * value;
    }
    vad->last_rms = (float)sqrt(sum / count);
    if(vad->last_rms > vad->peak_rms) vad->peak_rms = vad->last_rms;
    bool voice = fvad_process(vad->detector, samples, count) == 1;
    vad->elapsed_ms += vad->config.frame_ms;
    if(vad->elapsed_ms >= vad->config.max_utterance_ms) return BINBOT_VAD_MAX_DURATION;

    /* Wait for ASR to confirm quiet speech before ending on local silence.
     * Without a partial, the initial capture is submitted for final ASR. */
    if(!vad->transcript_received && vad->elapsed_ms >= vad->config.start_timeout_ms)
        return BINBOT_VAD_START_TIMEOUT;
    if(!vad->speech_started) {
        vad->consecutive_voice = voice ? vad->consecutive_voice + 1 : 0;
        if(vad->consecutive_voice >= vad->config.start_frames) {
            vad->speech_started = true;
            vad->speech_ms = vad->consecutive_voice * vad->config.frame_ms;
            return BINBOT_VAD_SPEECH_STARTED;
        }
        return BINBOT_VAD_WAITING;
    }

    vad->speech_ms += vad->config.frame_ms;
    vad->silence_ms = voice ? 0 : vad->silence_ms + vad->config.frame_ms;
    /* Quiet syllables can be missed locally, and streaming ASR updates are
     * bursty. Require both detectors to have been inactive before ending. */
    if(vad->transcript_received && vad->silence_ms >= vad->config.end_silence_ms &&
       vad->elapsed_ms - vad->last_transcript_ms >= vad->config.transcript_grace_ms)
        return BINBOT_VAD_SPEECH_ENDED;
    return BINBOT_VAD_SPEECH;
}
