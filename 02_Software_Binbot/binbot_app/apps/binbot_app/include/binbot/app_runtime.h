#ifndef BINBOT_APP_RUNTIME_H
#define BINBOT_APP_RUNTIME_H

#include "binbot/app_state.h"

#include <stdint.h>

typedef struct app_runtime app_runtime_t;

app_runtime_t *app_runtime_create(const char *state_path);
void app_runtime_destroy(app_runtime_t *runtime);
app_state_t *app_runtime_state(app_runtime_t *runtime);
void app_runtime_tick(app_runtime_t *runtime, uint32_t monotonic_ms);
bool app_runtime_flush(app_runtime_t *runtime);

#endif
