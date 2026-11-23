#include "application.h"

#if CONFIG_BINBOT_APP_SHOWCASE_TURNTABLE

#include <stdbool.h>

#include "esp_err.h"
#include "component_uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "turntable_motor.h"

#if !CONFIG_BINBOT_DISPLAY_NONE
#error "showcase_turntable must not select a display driver"
#endif

static void showcase_turntable_task(void *arg)
{
    (void)arg;
    ESP_ERROR_CHECK(turntable_motor_init());
    ESP_ERROR_CHECK(component_uart_start());
    ESP_ERROR_CHECK(component_uart_confirm_ready());
    vTaskDelete(NULL);
}

esp_err_t application_start(void)
{
    BaseType_t result = xTaskCreate(showcase_turntable_task, "turntable",
                                    3072, NULL, 5, NULL);
    return result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

#endif
