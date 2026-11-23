#ifndef BINBOT_UI_APP_H
#define BINBOT_UI_APP_H

#include "binbot/app_state.h"
#include "binbot/app_modules.h"
#include "binbot/app_firmware.h"
#include "binbot/app_vocabulary_audio.h"
#include "binbot/audio/client.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct ui_app ui_app_t;

typedef struct {
    uint8_t track_index;
    uint32_t position_seconds;
    bool active;
    bool playing;
} ui_app_music_snapshot_t;

ui_app_t *ui_app_create(app_state_t *state);
void ui_app_destroy(ui_app_t *app);
void ui_app_set_audio_client(ui_app_t *app, binbot_audio_client_t *client);
bool ui_app_pronounce_word(ui_app_t *app, const char *word, app_vocabulary_accent_t accent);
void ui_app_stop_pronunciation(ui_app_t *app);
bool ui_app_studying_vocabulary(const ui_app_t *app);
void ui_app_set_slave_client(ui_app_t *app, binbot_slave_client_t *client);
/* Return to the page that opened the current page, falling back to home. */
void ui_app_go_back(ui_app_t *app);
/* Called by device command polling outside LVGL event callbacks. */
bool ui_app_show_watchface(ui_app_t *app, const char *theme_id);
bool ui_app_show_pomodoro(ui_app_t *app);
/* Queue a timed vibration without waiting for the slave acknowledgement. */
bool ui_app_vibrate(ui_app_t *app, uint16_t duration_ms);
/* Start one voice turn without requiring the keyword detector.  The request
 * is routed through binbot_audio so the dialog service, UI and wake chime all
 * observe the same wake event. */
bool ui_app_request_wake(ui_app_t *app);
bool ui_app_finish_voice_input(ui_app_t *app);
void ui_app_handle_music_state(ui_app_t *app, int state,
                               uint32_t position_ms, uint32_t duration_ms);
void ui_app_handle_wake_detected(ui_app_t *app, int hotword_index);
void ui_app_handle_dialog_state(ui_app_t *app, int state);
void ui_app_handle_dialog_text(ui_app_t *app, const binbot_audio_dialog_text_t *text);
void ui_app_handle_dialog_playback(ui_app_t *app, const binbot_audio_dialog_text_t *text);
void ui_app_handle_noise_state(ui_app_t *app, const binbot_audio_noise_state_t *state);
bool ui_app_handle_calendar_state(ui_app_t *app, const binbot_audio_calendar_state_t *state);
void ui_app_handle_encoder_count(ui_app_t *app, int32_t count, bool input_event);
void ui_app_handle_slave_input(ui_app_t *app, uint8_t adc_key, bool encoder_down);
bool ui_app_apply_remote_music(ui_app_t *app,
                               binbot_audio_remote_action_t action,
                               uint32_t value);
void ui_app_get_music_snapshot(const ui_app_t *app,
                               ui_app_music_snapshot_t *snapshot);
void ui_app_reload_home_gif(ui_app_t *app);
app_modules_t *ui_app_modules(ui_app_t *app);
app_firmware_t *ui_app_firmware(ui_app_t *app);

#endif
