#include "board_pins.h"

#include <stddef.h>

esp_err_t board_get_spi_pins(board_spi_pins_t *pins)
{
    if (pins == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    pins->host = BOARD_LCD_SPI_HOST;
    pins->sclk = BOARD_LCD_SCLK;
    pins->mosi = BOARD_LCD_MOSI;
    return ESP_OK;
}

esp_err_t board_get_gc9a01a_pins(board_display_pins_t *pins)
{
    if (pins == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    pins->cs = BOARD_GC9A01A_CS;
    pins->dc = BOARD_GC9A01A_DC;
    pins->reset = BOARD_GC9A01A_RST;
    pins->backlight = BOARD_GC9A01A_BL;
    return ESP_OK;
}

esp_err_t board_get_ili9342c_pins(board_display_pins_t *pins)
{
    if (pins == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    pins->cs = BOARD_ILI9342C_CS;
    pins->dc = BOARD_ILI9342C_DC;
    pins->reset = BOARD_ILI9342C_RST;
    pins->backlight = BOARD_ILI9342C_BL;
    return ESP_OK;
}

esp_err_t board_get_turntable_pins(board_turntable_pins_t *pins)
{
    if (pins == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    pins->in1 = BOARD_TURNTABLE_IN1;
    pins->in2 = BOARD_TURNTABLE_IN2;
    return ESP_OK;
}

esp_err_t board_get_uart1_pins(board_uart_pins_t *pins)
{
    if (pins == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    pins->tx = BOARD_UART1_TX;
    pins->rx = BOARD_UART1_RX;
    return ESP_OK;
}
