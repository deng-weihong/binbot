#include "application.h"
#include "esp_err.h"
#include "component_uart.h"

void app_main(void)
{
    ESP_ERROR_CHECK(component_uart_start_boot_watchdog());
    ESP_ERROR_CHECK(component_uart_start());
    ESP_ERROR_CHECK(application_start());
}
