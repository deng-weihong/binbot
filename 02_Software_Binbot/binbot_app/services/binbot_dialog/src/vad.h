#ifndef BINBOT_DIALOG_VAD_H
#define BINBOT_DIALOG_VAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "fvad.h"

typedef struct {
    uint32_t start_timeout_ms;
    uint32_t end_silence_ms;
    uint32_t transcript_grace_ms;
    uint32_t max_utterance_ms;
    uint32_t frame_ms;
    uint16_t mode;
    uint16_t start_frames;
} binbot_vad_config_t;

typedef enum {
    BINBOT_VAD_WAITING = 0,
    BINBOT_VAD_SPEECH_STARTED,
    BINBOT_VAD_SPEECH,
    BINBOT_VAD_SPEECH_ENDED,
    BINBOT_VAD_START_TIMEOUT,
    BINBOT_VAD_MAX_DURATION,
} binbot_vad_result_t;

typedef struct {
    binbot_vad_config_t config;
    Fvad *detector;
    uint32_t elapsed_ms;
    uint32_t speech_ms;
    uint32_t silence_ms;
    uint32_t last_transcript_ms;
    uint16_t consecutive_voice;
    float last_rms;
    float peak_rms;
    bool speech_started;
    bool transcript_received;
} binbot_vad_t;

bool binbot_vad_init(binbot_vad_t *vad, const binbot_vad_config_t *config);
void binbot_vad_destroy(binbot_vad_t *vad);
void binbot_vad_reset(binbot_vad_t *vad);
void binbot_vad_observe_noise(binbot_vad_t *vad, const int16_t *samples, size_t count);
void binbot_vad_note_transcript(binbot_vad_t *vad);
binbot_vad_result_t binbot_vad_process(binbot_vad_t *vad,
                                       const int16_t *samples, size_t count);

#endif
