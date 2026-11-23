#pragma once

#include "esp_err.h"

esp_err_t power_card_io_init(void);
/* These calls block for the pulse duration; concurrent presses return ESP_ERR_TIMEOUT. */
esp_err_t power_card_press_power(void);
esp_err_t power_card_press_reset(void);
/* Returns the raw PW_LED sense level, or -1 if unavailable. */
int power_card_power_led_level(void);
/* Excludes all button pulses while firmware/configuration is being updated. */
esp_err_t power_card_maintenance_begin(void);
void power_card_maintenance_end(void);
