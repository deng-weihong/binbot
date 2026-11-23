#include "application.h"

#if CONFIG_BINBOT_APP_AMBIENT_LIGHT

#include "ambient_light.h"
#include "component_uart.h"
#include "esp_log.h"

#if !CONFIG_BINBOT_DISPLAY_NONE
#error "ambient_light must not select a display driver"
#endif

esp_err_t application_start(void)
{
    ESP_ERROR_CHECK(ambient_light_init());
    ESP_ERROR_CHECK(ambient_light_set(255, 160, 80, 20));
    ESP_ERROR_CHECK(component_uart_start());
    ESP_ERROR_CHECK(component_uart_confirm_ready());
    ESP_LOGI("ambient_light", "Ready: GPIO%d, %d WS2812 LEDs, warm white at 20%%",
             CONFIG_BINBOT_LIGHT_GPIO, CONFIG_BINBOT_LIGHT_LED_COUNT);
    return ESP_OK;
}

#endif
