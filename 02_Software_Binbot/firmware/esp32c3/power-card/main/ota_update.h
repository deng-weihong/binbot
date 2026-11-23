#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    bool busy;
    uint32_t received;
    uint32_t total;
    char state[16];
    char version[32];
    char error[80];
} power_card_ota_status_t;

esp_err_t power_card_ota_init(void);
/* Returns NULL when accepted; downloading continues in a background task. */
const char *power_card_ota_start(const char *url);
void power_card_ota_status(power_card_ota_status_t *status);
esp_err_t power_card_ota_confirm_boot(void);
