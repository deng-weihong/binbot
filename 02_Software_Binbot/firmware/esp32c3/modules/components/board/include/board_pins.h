#pragma once

#include "driver/gpio.h"
#include "esp_err.h"
#include "sdkconfig.h"

#define BOARD_LCD_SPI_HOST 1
#define BOARD_LCD_SCLK GPIO_NUM_6
#define BOARD_LCD_MOSI GPIO_NUM_7

#define BOARD_ILI9342C_CS GPIO_NUM_10
#define BOARD_ILI9342C_DC GPIO_NUM_0
#define BOARD_ILI9342C_RST GPIO_NUM_3
#define BOARD_ILI9342C_BL GPIO_NUM_4

#define BOARD_GC9A01A_CS GPIO_NUM_10
#define BOARD_GC9A01A_DC GPIO_NUM_0
#define BOARD_GC9A01A_RST GPIO_NUM_3
#define BOARD_GC9A01A_BL GPIO_NUM_4

#define BOARD_TURNTABLE_IN1 GPIO_NUM_18
#define BOARD_TURNTABLE_IN2 GPIO_NUM_19

/* Both display boards expose UART1 with RX on GPIO1 and TX on GPIO5. */
#define BOARD_UART1_TX GPIO_NUM_5
#define BOARD_UART1_RX GPIO_NUM_1

typedef struct {
    int host;
    gpio_num_t sclk;
    gpio_num_t mosi;
} board_spi_pins_t;

typedef struct {
    gpio_num_t cs;
    gpio_num_t dc;
    gpio_num_t reset;
    gpio_num_t backlight;
} board_display_pins_t;

typedef struct {
    gpio_num_t in1;
    gpio_num_t in2;
} board_turntable_pins_t;

typedef struct {
    gpio_num_t tx;
    gpio_num_t rx;
} board_uart_pins_t;

esp_err_t board_get_spi_pins(board_spi_pins_t *pins);
esp_err_t board_get_gc9a01a_pins(board_display_pins_t *pins);
esp_err_t board_get_ili9342c_pins(board_display_pins_t *pins);
esp_err_t board_get_turntable_pins(board_turntable_pins_t *pins);
esp_err_t board_get_uart1_pins(board_uart_pins_t *pins);
