#include "display.h"

#include <stdbool.h>
#include <string.h>

#include "board_pins.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define DISPLAY_SPI_CLOCK_HZ (40 * 1000 * 1000)
#define DISPLAY_DMA_STAGING_BYTES 4096
#define DISPLAY_MAX_TRANSFER_BYTES DISPLAY_DMA_STAGING_BYTES

struct display_context_t {
    display_device_t device;
    spi_host_device_t spi_host;
    bool spi_bus_initialized;
    esp_lcd_panel_io_handle_t io;
    uint8_t *dma_staging;
    board_display_pins_t pins;
    display_info_t info;
};

#if CONFIG_BINBOT_DISPLAY_GC9A01A
esp_err_t gc9a01a_register_init(esp_lcd_panel_io_handle_t io);
#endif
#if CONFIG_BINBOT_DISPLAY_ILI9342C
esp_err_t ili9342c_register_init(esp_lcd_panel_io_handle_t io);
#endif

static const char *TAG = "driver_display";

static esp_err_t configure_output(gpio_num_t pin, uint32_t level)
{
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t error = gpio_config(&config);
    if (error != ESP_OK) {
        return error;
    }
    return gpio_set_level(pin, (uint32_t)level);
}

static esp_err_t reset_panel(const board_display_pins_t *pins)
{
    esp_err_t error = gpio_set_level(pins->reset, 1);
    if (error != ESP_OK) {
        return error;
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    error = gpio_set_level(pins->reset, 0);
    if (error != ESP_OK) {
        return error;
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    error = gpio_set_level(pins->reset, 1);
    if (error != ESP_OK) {
        return error;
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    return ESP_OK;
}

static esp_err_t set_window_region(display_handle_t handle, uint16_t x,
                                   uint16_t y, uint16_t width,
                                   uint16_t height)
{
    const uint8_t column_address[] = {
        (uint8_t)(x >> 8), (uint8_t)x,
        (uint8_t)((x + width - 1) >> 8), (uint8_t)(x + width - 1),
    };
    const uint8_t page_address[] = {
        (uint8_t)(y >> 8), (uint8_t)y,
        (uint8_t)((y + height - 1) >> 8), (uint8_t)(y + height - 1),
    };
    esp_err_t error = esp_lcd_panel_io_tx_param(handle->io, 0x2A,
                                                 column_address,
                                                 sizeof(column_address));
    if (error != ESP_OK) {
        return error;
    }
    return esp_lcd_panel_io_tx_param(handle->io, 0x2B, page_address,
                                     sizeof(page_address));
}

static void release_context(display_handle_t context)
{
    if (context->io != NULL) {
        (void)esp_lcd_panel_io_del(context->io);
        context->io = NULL;
    }
    heap_caps_free(context->dma_staging);
    context->dma_staging = NULL;
    if (context->spi_bus_initialized) {
        (void)spi_bus_free(context->spi_host);
        context->spi_bus_initialized = false;
    }
    heap_caps_free(context);
}

esp_err_t display_init(display_device_t device, display_handle_t *handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *handle = NULL;

    if (device != DISPLAY_DEVICE_GC9A01A &&
        device != DISPLAY_DEVICE_ILI9342C) {
        return ESP_ERR_INVALID_ARG;
    }
#if CONFIG_BINBOT_DISPLAY_GC9A01A
    if (device != DISPLAY_DEVICE_GC9A01A) {
        return ESP_ERR_NOT_SUPPORTED;
    }
#elif CONFIG_BINBOT_DISPLAY_ILI9342C
    if (device != DISPLAY_DEVICE_ILI9342C) {
        return ESP_ERR_NOT_SUPPORTED;
    }
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif

    display_handle_t context = heap_caps_calloc(1, sizeof(*context),
                                                MALLOC_CAP_INTERNAL);
    if (context == NULL) {
        return ESP_ERR_NO_MEM;
    }
    context->device = device;
    context->info.width = device == DISPLAY_DEVICE_GC9A01A ? 240 : 320;
    context->info.height = 240;
    context->info.frame_bytes = (size_t)context->info.width *
                                context->info.height * 2;

    board_spi_pins_t spi_pins;
    esp_err_t error;
    if (device == DISPLAY_DEVICE_GC9A01A) {
        error = board_get_gc9a01a_pins(&context->pins);
    } else if (device == DISPLAY_DEVICE_ILI9342C) {
        error = board_get_ili9342c_pins(&context->pins);
    } else {
        error = ESP_ERR_INVALID_ARG;
    }
    if (error != ESP_OK) {
        heap_caps_free(context);
        return error;
    }
    error = board_get_spi_pins(&spi_pins);
    if (error != ESP_OK) {
        release_context(context);
        return error;
    }
    context->spi_host = (spi_host_device_t)spi_pins.host;

    ESP_LOGI(TAG, "init device=%d, %ux%u, SPI=%d Hz", device,
             context->info.width, context->info.height, DISPLAY_SPI_CLOCK_HZ);
    error = configure_output(context->pins.reset, 1);
    if (error != ESP_OK) {
        release_context(context);
        return error;
    }
    error = configure_output(context->pins.backlight, 0);
    if (error != ESP_OK) {
        release_context(context);
        return error;
    }

    const spi_bus_config_t bus_config = {
        .mosi_io_num = spi_pins.mosi,
        .miso_io_num = -1,
        .sclk_io_num = spi_pins.sclk,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DISPLAY_MAX_TRANSFER_BYTES,
    };
    error = spi_bus_initialize((spi_host_device_t)spi_pins.host, &bus_config,
                               SPI_DMA_CH_AUTO);
    if (error != ESP_OK) {
        release_context(context);
        return error;
    }
    context->spi_bus_initialized = true;

    const esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = context->pins.cs,
        .dc_gpio_num = context->pins.dc,
        .spi_mode = 0,
        .pclk_hz = DISPLAY_SPI_CLOCK_HZ,
        .trans_queue_depth = 1,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    error = esp_lcd_new_panel_io_spi(
                                     (esp_lcd_spi_bus_handle_t)spi_pins.host,
                                     &io_config, &context->io);
    if (error != ESP_OK) {
        release_context(context);
        return error;
    }

    context->dma_staging = heap_caps_malloc(DISPLAY_DMA_STAGING_BYTES,
                                            MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (context->dma_staging == NULL) {
        release_context(context);
        return ESP_ERR_NO_MEM;
    }

    error = reset_panel(&context->pins);
    if (error != ESP_OK) {
        release_context(context);
        return error;
    }
#if CONFIG_BINBOT_DISPLAY_GC9A01A
    if (device == DISPLAY_DEVICE_GC9A01A) {
        error = gc9a01a_register_init(context->io);
    }
#elif CONFIG_BINBOT_DISPLAY_ILI9342C
    if (device == DISPLAY_DEVICE_ILI9342C) {
        error = ili9342c_register_init(context->io);
    }
#endif
    if (error != ESP_OK) {
        release_context(context);
        return error;
    }
    error = gpio_set_level(context->pins.backlight, 1);
    if (error != ESP_OK) {
        release_context(context);
        return error;
    }
    *handle = context;
    return ESP_OK;
}

esp_err_t display_deinit(display_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = gpio_set_level(handle->pins.backlight, 0);
    if (error != ESP_OK) {
        return error;
    }
    if (handle->io != NULL) {
        error = esp_lcd_panel_io_del(handle->io);
        if (error != ESP_OK) {
            return error;
        }
        handle->io = NULL;
    }
    if (handle->spi_bus_initialized) {
        error = spi_bus_free(handle->spi_host);
        if (error != ESP_OK) {
            return error;
        }
        handle->spi_bus_initialized = false;
    }
    heap_caps_free(handle);
    return ESP_OK;
}

esp_err_t display_get_info(display_handle_t handle, display_info_t *info)
{
    if (handle == NULL || info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *info = handle->info;
    return ESP_OK;
}

esp_err_t display_clear(display_handle_t handle, uint16_t color)
{
    if (handle == NULL || handle->dma_staging == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t row_bytes = (size_t)handle->info.width * 2;
    const uint16_t rows_per_chunk =
        (uint16_t)(DISPLAY_DMA_STAGING_BYTES / row_bytes);
    if (rows_per_chunk == 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    for (size_t offset = 0; offset < DISPLAY_DMA_STAGING_BYTES;
         offset += 2) {
        handle->dma_staging[offset] = (uint8_t)(color >> 8);
        handle->dma_staging[offset + 1] = (uint8_t)color;
    }

    uint16_t row = 0;
    while (row < handle->info.height) {
        uint16_t chunk_rows = handle->info.height - row;
        if (chunk_rows > rows_per_chunk) {
            chunk_rows = rows_per_chunk;
        }
        const size_t chunk_bytes = (size_t)chunk_rows * row_bytes;
        esp_err_t error = set_window_region(handle, 0, row,
                                            handle->info.width, chunk_rows);
        if (error != ESP_OK) {
            return error;
        }
        error = esp_lcd_panel_io_tx_color(handle->io, 0x2C,
                                          handle->dma_staging, chunk_bytes);
        if (error != ESP_OK) {
            return error;
        }
        row += chunk_rows;
    }
    return ESP_OK;
}

esp_err_t display_flush(display_handle_t handle, const uint8_t *frame,
                        size_t frame_bytes)
{
    if (handle == NULL || frame == NULL || frame_bytes != handle->info.frame_bytes) {
        return ESP_ERR_INVALID_ARG;
    }
    return display_flush_region(handle, frame, frame_bytes, 0, 0,
                                handle->info.width, handle->info.height);
}

esp_err_t display_flush_region(display_handle_t handle, const uint8_t *frame,
                               size_t frame_bytes, uint16_t x, uint16_t y,
                               uint16_t width, uint16_t height)
{
    if (handle == NULL || frame == NULL || width == 0 || height == 0 ||
        x >= handle->info.width || y >= handle->info.height ||
        width > handle->info.width - x || height > handle->info.height - y ||
        frame_bytes != (size_t)width * height * 2) {
        return ESP_ERR_INVALID_ARG;
    }
    // Graphics buffers live in ordinary internal RAM. Copy small chunks into
    // the DMA-capable staging buffer so OTA and display do not need a large
    // contiguous DMA allocation at the same time.
    const size_t row_bytes = (size_t)width * 2;
    const uint16_t rows_per_chunk = (uint16_t)(DISPLAY_DMA_STAGING_BYTES /
                                               row_bytes);
    if (rows_per_chunk == 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t offset = 0;
    uint16_t row = 0;
    while (row < height) {
        uint16_t chunk_rows = (uint16_t)(height - row);
        if (chunk_rows > rows_per_chunk) {
            chunk_rows = rows_per_chunk;
        }
        const size_t chunk_bytes = (size_t)chunk_rows * row_bytes;
        esp_err_t error = set_window_region(handle, x, y + row, width,
                                            chunk_rows);
        if (error != ESP_OK) {
            return error;
        }
        // set_window_region waits for the previous DMA transaction. Reuse
        // the staging buffer only after that transaction has completed.
        memcpy(handle->dma_staging, frame + offset, chunk_bytes);
        error = esp_lcd_panel_io_tx_color(handle->io, 0x2C,
                                          handle->dma_staging, chunk_bytes);
        if (error != ESP_OK) {
            return error;
        }
        offset += chunk_bytes;
        row += chunk_rows;
    }
    return ESP_OK;
}
