#ifndef BINBOT_SLAVE_OTA_CLIENT_H
#define BINBOT_SLAVE_OTA_CLIENT_H

#include "binbot/component/client.h"
#include "binbot/slave/ipc.h"

int bb_slave_ota_open(binbot_slave_client_t *client, char error[160]);
int bb_slave_ota_status(binbot_slave_client_t *client, bb_component_info_t *info,
                         char error[160]);
int bb_slave_ota_abort(binbot_slave_client_t *client, char error[160]);
int bb_slave_ota_update(binbot_slave_client_t *client, const char *path,
                         bb_component_progress_fn progress, bb_component_phase_fn phase,
                         void *context, char error[160]);

#endif
