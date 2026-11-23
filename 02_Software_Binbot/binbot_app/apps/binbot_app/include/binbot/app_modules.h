#ifndef BINBOT_APP_MODULES_H
#define BINBOT_APP_MODULES_H

#include "binbot/component/worker.h"
#include <stddef.h>
#include <stdint.h>

typedef struct app_modules app_modules_t;
typedef void (*app_module_connection_fn)(unsigned profile, bool inserted, void *user_data);
typedef struct {
    char id[37];
    char name[72];
    char thumbnail[512];
    char files[2][512];
    char sha256[2][65];
    uint32_t sizes[2];
} app_module_image_t;

app_modules_t *app_modules_create(const char *directory);
void app_modules_destroy(app_modules_t *modules);
void app_modules_set_worker(app_modules_t *modules, bb_component_worker_t *worker);
void app_modules_set_connection_observer(app_modules_t *modules,
                                         app_module_connection_fn observer, void *user_data);
size_t app_modules_image_count(const app_modules_t *modules);
const app_module_image_t *app_modules_image(const app_modules_t *modules, size_t index);
const char *app_modules_current(const app_modules_t *modules, unsigned profile);
bool app_modules_set_current(app_modules_t *modules, const char *image_id, unsigned profile);
uint32_t app_modules_revision(const app_modules_t *modules);
unsigned app_modules_profile(const app_modules_t *modules);
bool app_modules_healthy(const app_modules_t *modules);
void app_modules_request_probe(app_modules_t *modules);
bool app_modules_pen_supported(const app_modules_t *modules);
bool app_modules_pen_busy(const app_modules_t *modules);
const bb_component_pen_t *app_modules_pen_state(const app_modules_t *modules);
/* Shared live weather, available even when no component is connected. */
bool app_modules_weather(const app_modules_t *modules, bb_component_pen_t *weather);
bool app_modules_pen_set(app_modules_t *modules, unsigned mode, unsigned face);
const binbot_component_event_t *app_modules_pen_status(const app_modules_t *modules);
bool app_modules_random_set(app_modules_t *modules, unsigned profile, unsigned seconds);
unsigned app_modules_random_interval(const app_modules_t *modules, unsigned profile);
unsigned app_modules_random_remaining(const app_modules_t *modules, unsigned profile);
bool app_modules_motor_supported(const app_modules_t *modules);
bool app_modules_motor_busy(const app_modules_t *modules);
unsigned app_modules_motor_direction(const app_modules_t *modules);
unsigned app_modules_motor_speed(const app_modules_t *modules);
bool app_modules_motor_set(app_modules_t *modules, unsigned direction, unsigned speed);
const binbot_component_event_t *app_modules_motor_status(const app_modules_t *modules);
bool app_modules_light_supported(const app_modules_t *modules);
bool app_modules_light_busy(const app_modules_t *modules);
const bb_component_light_settings_t *app_modules_light_state(const app_modules_t *modules);
unsigned app_modules_light_remaining(const app_modules_t *modules);
bool app_modules_light_set(app_modules_t *modules, const bb_component_light_settings_t *settings);
const binbot_component_event_t *app_modules_light_status(const app_modules_t *modules);
void app_modules_tick(app_modules_t *modules, uint32_t now_ms);
bool app_modules_handle_event(app_modules_t *modules, const binbot_component_event_t *event);
bool app_modules_push(app_modules_t *modules, size_t image_index, unsigned profile);
const binbot_component_event_t *app_modules_push_status(const app_modules_t *modules);
unsigned app_modules_push_profile(const app_modules_t *modules);
const char *app_modules_push_image(const app_modules_t *modules);
bool app_modules_receive(app_modules_t *modules, const char *base_url);
int app_modules_poll_receive(app_modules_t *modules);

#endif
