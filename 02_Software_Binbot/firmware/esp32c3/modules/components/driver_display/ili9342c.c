#include "esp_lcd_panel_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"

esp_err_t ili9342c_register_init(esp_lcd_panel_io_handle_t io)
{
    static const uint8_t extc[] = {0xFF, 0x93, 0x42};
    // Keep BGR color order and use the panel's corrected vertical scan
    // direction; the holographic panel is mounted upside down.
    static const uint8_t madctl[] = {0x48};
    static const uint8_t pixfmt[] = {0x55};
    static const uint8_t pwctr1[] = {0x10, 0x10};
    static const uint8_t pwctr2[] = {0x01};
    static const uint8_t vcom[] = {0xCD};
    static const uint8_t frame_rate[] = {0x00, 0x1B};
    static const uint8_t inversion[] = {0x02};
    static const uint8_t gamma_positive[] = {
        0x0F, 0x14, 0x17, 0x07, 0x16, 0x0A, 0x3F, 0x68,
        0x4C, 0x06, 0x0F, 0x0D, 0x18, 0x1A, 0x00,
    };
    static const uint8_t gamma_negative[] = {
        0x00, 0x29, 0x29, 0x04, 0x0F, 0x04, 0x3C, 0x24,
        0x4B, 0x02, 0x0B, 0x09, 0x32, 0x37, 0x0F,
    };

    esp_err_t error = esp_lcd_panel_io_tx_param(io, 0xC8, extc, sizeof(extc));
    if (error != ESP_OK) {
        return error;
    }
    error = esp_lcd_panel_io_tx_param(io, 0x36, madctl, sizeof(madctl));
    if (error != ESP_OK) {
        return error;
    }
    error = esp_lcd_panel_io_tx_param(io, 0x3A, pixfmt, sizeof(pixfmt));
    if (error != ESP_OK) {
        return error;
    }
    error = esp_lcd_panel_io_tx_param(io, 0xC0, pwctr1, sizeof(pwctr1));
    if (error != ESP_OK) {
        return error;
    }
    error = esp_lcd_panel_io_tx_param(io, 0xC1, pwctr2, sizeof(pwctr2));
    if (error != ESP_OK) {
        return error;
    }
    error = esp_lcd_panel_io_tx_param(io, 0xC5, vcom, sizeof(vcom));
    if (error != ESP_OK) {
        return error;
    }
    error = esp_lcd_panel_io_tx_param(io, 0xB1, frame_rate, sizeof(frame_rate));
    if (error != ESP_OK) {
        return error;
    }
    error = esp_lcd_panel_io_tx_param(io, 0xB4, inversion, sizeof(inversion));
    if (error != ESP_OK) {
        return error;
    }
    error = esp_lcd_panel_io_tx_param(io, 0xE0, gamma_positive,
                                      sizeof(gamma_positive));
    if (error != ESP_OK) {
        return error;
    }
    error = esp_lcd_panel_io_tx_param(io, 0xE1, gamma_negative,
                                      sizeof(gamma_negative));
    if (error != ESP_OK) {
        return error;
    }
    error = esp_lcd_panel_io_tx_param(io, 0x11, NULL, 0);
    if (error != ESP_OK) {
        return error;
    }
    vTaskDelay(pdMS_TO_TICKS(120));
    return esp_lcd_panel_io_tx_param(io, 0x29, NULL, 0);
}
