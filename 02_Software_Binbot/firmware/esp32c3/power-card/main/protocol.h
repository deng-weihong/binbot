#pragma once

#include <stdbool.h>
#include <stddef.h>

#define POWER_CARD_MAX_FRAME 1024
#define POWER_CARD_MAX_URL 512
#define POWER_CARD_MAX_ID 64

typedef enum {
    POWER_CARD_CMD_STATUS,
    POWER_CARD_CMD_POWER,
    POWER_CARD_CMD_RESET,
    POWER_CARD_CMD_OTA,
    POWER_CARD_CMD_OTA_STATUS,
} power_card_command_t;

typedef struct {
    power_card_command_t command;
    bool has_id;
    char id[POWER_CARD_MAX_ID + 1];
    char url[POWER_CARD_MAX_URL + 1];
} power_card_request_t;

typedef struct {
    size_t used;
    char data[POWER_CARD_MAX_FRAME + 1];
} power_card_frame_t;

/* Feed one byte: 1 means a complete frame, 0 means more data, -1 means close. */
int power_card_frame_feed(power_card_frame_t *frame, unsigned char byte);
/* Returns NULL on success, otherwise a stable protocol error code. */
const char *power_card_parse_request(const char *line, const char *token, power_card_request_t *request);
bool power_card_valid_ota_url(const char *url);
