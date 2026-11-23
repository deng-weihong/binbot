#ifndef BINBOT_APP_FIRMWARE_H
#define BINBOT_APP_FIRMWARE_H

#include "binbot/slave/ipc.h"

typedef enum {
    APP_FIRMWARE_UNKNOWN,
    APP_FIRMWARE_READING,
    APP_FIRMWARE_READY,
    APP_FIRMWARE_OFFLINE,
    APP_FIRMWARE_BUSY,
    APP_FIRMWARE_UNSUPPORTED,
} app_firmware_status_t;

typedef struct {
    binbot_slave_client_t *client;
    app_firmware_status_t status;
    uint32_t request_id, started_ms, last_attempt_ms;
    uint32_t version, boot_id;
    uint8_t slot;
    bool healthy, requested, started;
} app_firmware_t;

void app_firmware_refresh(app_firmware_t *firmware);
void app_firmware_tick(app_firmware_t *firmware, uint32_t now_ms);
bool app_firmware_handle_packet(app_firmware_t *firmware, const binbot_slave_ipc_packet_t *packet);

#endif
