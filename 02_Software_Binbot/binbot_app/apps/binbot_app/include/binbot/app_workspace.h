#ifndef BINBOT_APP_WORKSPACE_H
#define BINBOT_APP_WORKSPACE_H
#include "binbot/app_runtime.h"
#include "binbot/ui_app.h"
#include "binbot/control/service.h"

typedef struct app_workspace app_workspace_t;
app_workspace_t *app_workspace_create(app_runtime_t *runtime, ui_app_t *ui,
                                       binbot_audio_client_t *audio, const char *state_path);
void app_workspace_destroy(app_workspace_t *workspace);
void app_workspace_set_control(app_workspace_t *workspace, bb_control_service_t *control);
cJSON *app_workspace_snapshot(app_workspace_t *workspace);
int app_workspace_command(app_workspace_t *workspace, const char *name, const cJSON *payload,
                           const char *command_id, uint64_t request, char *message, size_t capacity);
void app_workspace_audio(app_workspace_t *workspace, const binbot_audio_packet_t *packet);
void app_workspace_tick(app_workspace_t *workspace, uint32_t now);
#endif
