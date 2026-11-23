#ifndef BINBOT_APP_DIAGNOSTICS_H
#define BINBOT_APP_DIAGNOSTICS_H

#include <stdbool.h>
#include <stdint.h>

typedef struct app_diagnostics app_diagnostics_t;

typedef enum {
    APP_DIAG_PLAY_NONE,
    APP_DIAG_PLAY_LEFT,
    APP_DIAG_PLAY_RIGHT,
    APP_DIAG_PLAY_TONE,
    APP_DIAG_PLAY_SWEEP,
} app_diag_play_t;

typedef enum {
    APP_DIAG_ERROR_NONE,
    APP_DIAG_ERROR_OFFLINE,
    APP_DIAG_ERROR_CAPTURE,
    APP_DIAG_ERROR_GAP,
    APP_DIAG_ERROR_PLAYBACK,
    APP_DIAG_ERROR_BUSY,
} app_diag_error_t;

typedef struct {
    bool connected, capture_ready, recording, recorded, busy;
    uint8_t level[2], peak[2];
    uint32_t record_ms, playback_ms;
    app_diag_play_t playback;
    app_diag_error_t error;
} app_diag_status_t;

app_diagnostics_t *app_diagnostics_create(const char *socket_path);
void app_diagnostics_destroy(app_diagnostics_t *diagnostics);
void app_diagnostics_set_microphone(app_diagnostics_t *diagnostics, bool enabled);
bool app_diagnostics_record(app_diagnostics_t *diagnostics);
bool app_diagnostics_play(app_diagnostics_t *diagnostics, app_diag_play_t kind);
void app_diagnostics_stop(app_diagnostics_t *diagnostics);
app_diag_status_t app_diagnostics_status(app_diagnostics_t *diagnostics);

#endif
