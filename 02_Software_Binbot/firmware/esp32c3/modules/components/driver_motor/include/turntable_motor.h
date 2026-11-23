#pragma once

#include "esp_err.h"
#include <stdint.h>

esp_err_t turntable_motor_init(void);
esp_err_t turntable_motor_stop(void);
esp_err_t turntable_motor_left(void);
esp_err_t turntable_motor_right(void);
esp_err_t turntable_motor_set(uint8_t direction, uint8_t speed);
esp_err_t turntable_motor_get(uint8_t *direction, uint8_t *speed);
