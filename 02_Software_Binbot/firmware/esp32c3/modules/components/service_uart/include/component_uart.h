#pragma once

#include "esp_err.h"
#include "binbot/component/pen.h"

esp_err_t component_uart_start(void);
esp_err_t component_uart_confirm_ready(void);
esp_err_t component_uart_start_boot_watchdog(void);
#if CONFIG_BINBOT_APP_ROUND_SCREEN
void component_pen_snapshot(bb_component_pen_t *state);
void component_pen_show_image(void);
#endif
