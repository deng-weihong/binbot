#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "binbot/component/light.h"

esp_err_t ambient_light_init(void);
esp_err_t ambient_light_set(uint8_t red, uint8_t green, uint8_t blue, uint8_t brightness);
esp_err_t ambient_light_get(uint8_t *red, uint8_t *green, uint8_t *blue, uint8_t *brightness);
esp_err_t ambient_light_configure(const bb_component_light_settings_t *settings);
esp_err_t ambient_light_status(bb_component_light_settings_t *settings, uint16_t *remaining_minutes);
