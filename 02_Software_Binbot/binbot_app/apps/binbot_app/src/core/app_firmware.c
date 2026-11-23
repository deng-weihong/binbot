#include "binbot/app_firmware.h"

static uint32_t read_u32(const uint8_t *data)
{
    return (uint32_t)data[0] | (uint32_t)data[1] << 8 |
           (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
}

void app_firmware_refresh(app_firmware_t *firmware)
{
    if(!firmware || firmware->requested) return;
    firmware->requested = true;
    firmware->started = false;
    firmware->request_id = 0;
    firmware->status = APP_FIRMWARE_READING;
}

void app_firmware_tick(app_firmware_t *firmware, uint32_t now_ms)
{
    if(!firmware || !firmware->requested) return;
    if(!firmware->started) {
        firmware->started = true;
        firmware->started_ms = now_ms;
        firmware->last_attempt_ms = now_ms - 250;
    }
    if((uint32_t)(now_ms - firmware->started_ms) >= 5000) {
        firmware->requested = false;
        firmware->request_id = 0;
        if(firmware->status != APP_FIRMWARE_BUSY) firmware->status = APP_FIRMWARE_OFFLINE;
        return;
    }
    if(firmware->request_id || !firmware->client ||
       (uint32_t)(now_ms - firmware->last_attempt_ms) < 250) return;
    firmware->last_attempt_ms = now_ms;
    firmware->request_id = binbot_slave_client_send_request(firmware->client,
        BINBOT_SLAVE_CMD_OTA_INFO, NULL, 0);
}

bool app_firmware_handle_packet(app_firmware_t *firmware, const binbot_slave_ipc_packet_t *packet)
{
    if(!firmware || !packet || !firmware->requested || !firmware->request_id ||
       packet->type != BINBOT_SLAVE_IPC_MSG_RESPONSE ||
       packet->command != BINBOT_SLAVE_CMD_OTA_INFO ||
       packet->request_id != firmware->request_id) return false;
    firmware->request_id = 0;
    if(packet->error_code == BINBOT_SLAVE_ERR_BUSY) {
        firmware->status = APP_FIRMWARE_BUSY;
        return true;
    }
    firmware->requested = false;
    if(packet->error_code) {
        firmware->status = packet->error_code == BINBOT_SLAVE_IPC_ERR_TIMEOUT
                               ? APP_FIRMWARE_OFFLINE : APP_FIRMWARE_UNSUPPORTED;
        return true;
    }
    const uint8_t *data = packet->payload;
    if(packet->payload_length != 56 || data[0] != 1 || data[1] > 1 || data[2] > 1 ||
       read_u32(data + 8) != 0x35000) {
        firmware->status = APP_FIRMWARE_UNSUPPORTED;
        return true;
    }
    firmware->slot = data[1];
    firmware->healthy = data[2] == 1;
    firmware->boot_id = read_u32(data + 4);
    firmware->version = read_u32(data + 12);
    firmware->status = APP_FIRMWARE_READY;
    return true;
}
