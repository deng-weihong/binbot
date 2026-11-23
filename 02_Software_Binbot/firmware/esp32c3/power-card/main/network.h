#pragma once

#include <stdbool.h>
#include "esp_err.h"

#define POWER_CARD_TOKEN_LENGTH 32

typedef struct {
    bool configured;
    bool connected;
    char ssid[33];
    char ip[16];
} power_card_network_status_t;

esp_err_t power_card_network_init(void);
void power_card_network_status(power_card_network_status_t *status);
/* Saves credentials for the next boot. Caller must restart after success. */
esp_err_t power_card_network_save(const char *ssid, const char *password);
/* Token is immutable until NVS is erased; only expose it over the serial console. */
const char *power_card_network_token(void);
