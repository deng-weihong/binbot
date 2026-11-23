#include "application.h"

#if CONFIG_BINBOT_APP_HOLOGRAPHIC_SCREEN

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>

#include "display.h"
#include "component_uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "gif_player.h"
#include "graphics.h"
#include "ota_service.h"
#include "storage.h"

#if !CONFIG_BINBOT_DISPLAY_ILI9342C
#error "holographic_screen requires the ILI9342C display driver"
#endif

static const char *TAG = "app_holographic";

#define GIF_WIDTH 132
#define GIF_HEIGHT 132
#define GIF_RENDER_WIDTH 220
#define GIF_RENDER_HEIGHT 220
#define GIF_RENDER_ROWS 8
#define OTA_ROW_START 96
#define OTA_ROW_WIDTH 240

static uint8_t s_gif_frame[GIF_WIDTH * GIF_HEIGHT * 2];
static uint8_t s_scaled_gif_rows[320 * GIF_RENDER_ROWS * 2];
static QueueHandle_t s_gif_update_queue;

typedef struct {
    char name[64];
} gif_update_event_t;

static void gif_update_callback(void *context, const char *name)
{
    if (context == NULL || name == NULL) return;
    gif_update_event_t event;
    snprintf(event.name, sizeof(event.name), "%s", name);
    (void)xQueueOverwrite((QueueHandle_t)context, &event);
}

#define GIF_PATH BINBOT_STORAGE_PATH "/display.gif"

static void holographic_ota_callback(const ota_progress_t *progress,
                                     void *user_context)
{
    if (progress == NULL || user_context == NULL) {
        return;
    }
    (void)xQueueOverwrite((QueueHandle_t)user_context, progress);
}

static esp_err_t show_ota_progress(display_handle_t display, uint8_t percent,
                                   bool complete)
{
    uint8_t row[OTA_ROW_WIDTH * 2];
    for (uint16_t y = OTA_ROW_START; y < OTA_ROW_START + 48; ++y) {
        esp_err_t error = graphics_render_ota_row(
            row, sizeof(row), OTA_ROW_WIDTH, y, percent, complete);
        if (error != ESP_OK) {
            return error;
        }
        error = display_flush_region(display, row, sizeof(row), 40, y,
                                     OTA_ROW_WIDTH, 1);
        if (error != ESP_OK) {
            return error;
        }
    }
    return ESP_OK;
}

static void handle_ota_events(display_handle_t display, QueueHandle_t queue,
                              bool *active)
{
    ota_progress_t progress;
    while (xQueueReceive(queue, &progress, 0) == pdTRUE) {
        if (progress.state == OTA_PROGRESS_DOWNLOADING) {
            if (!*active) {
                ESP_ERROR_CHECK(display_clear(display, GRAPHICS_BLACK));
            }
            *active = true;
            ESP_ERROR_CHECK(show_ota_progress(display, progress.percent, false));
        } else if (progress.state == OTA_PROGRESS_COMPLETE) {
            ESP_ERROR_CHECK(show_ota_progress(display, 100, true));
            *active = false;
        } else {
            if (*active) {
                ESP_ERROR_CHECK(display_clear(display, GRAPHICS_BLACK));
            }
            *active = false;
        }
    }
}

static esp_err_t display_scaled_gif_frame(display_handle_t display,
                                          uint16_t x, uint16_t y)
{
    uint16_t destination_y = 0;
    while (destination_y < GIF_RENDER_HEIGHT) {
        uint16_t row_count = GIF_RENDER_HEIGHT - destination_y;
        if (row_count > GIF_RENDER_ROWS) {
            row_count = GIF_RENDER_ROWS;
        }
        esp_err_t error = graphics_scale_rgb565_rows(
            s_gif_frame, GIF_WIDTH, GIF_HEIGHT, s_scaled_gif_rows,
            sizeof(s_scaled_gif_rows), GIF_RENDER_WIDTH, GIF_RENDER_HEIGHT,
            destination_y, row_count);
        if (error != ESP_OK) {
            return error;
        }
        error = display_flush_region(
            display, s_scaled_gif_rows,
            (size_t)GIF_RENDER_WIDTH * row_count * 2, x,
            (uint16_t)(y + destination_y), GIF_RENDER_WIDTH, row_count);
        if (error != ESP_OK) {
            return error;
        }
        destination_y += row_count;
    }
    return ESP_OK;
}

typedef struct {
    display_handle_t display;
    uint16_t x, y;
} gif_display_t;

static esp_err_t display_gif_rows(void *context, const uint8_t *rows,
                                  uint16_t width, uint16_t first_row,
                                  uint16_t row_count)
{
    gif_display_t *target = context;
    return display_flush_region(target->display, rows, (size_t)width * row_count * 2,
                                 target->x, target->y + first_row, width, row_count);
}

static esp_err_t play_asset(display_handle_t display, QueueHandle_t ota_queue,
                            bool *ota_active, const char *path)
{
    gif_player_handle_t gif = NULL;
    ESP_LOGI(TAG, "opening GIF asset: %s", path);
    esp_err_t error = gif_player_open(path, &gif);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "cannot open %s: %s", path,
                 esp_err_to_name(error));
        return error;
    }
    ESP_LOGI(TAG, "GIF asset opened: %s", path);
    uint16_t width, height;
    display_info_t info;
    ESP_ERROR_CHECK(gif_player_get_size(gif, &width, &height));
    ESP_ERROR_CHECK(display_get_info(display, &info));
    bool legacy = width == GIF_WIDTH && height == GIF_HEIGHT;
    gif_display_t target = {display, (info.width - width) / 2, (info.height - height) / 2};

    uint16_t index = 0;
    while (true) {
        handle_ota_events(display, ota_queue, ota_active);
        gif_update_event_t update_event;
        if (s_gif_update_queue && xQueueReceive(s_gif_update_queue, &update_event, 0) == pdTRUE) {
            ESP_LOGI(TAG, "GIF updated, reloading: %s", update_event.name);
            gif_player_close(gif);
            return ESP_ERR_INVALID_STATE;
        }
        if (*ota_active) {
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }

        uint32_t delay_ms = 0;
        if (index == 0) {
            ESP_LOGI(TAG, "decoding first GIF frame: %s", path);
        }
        if (legacy) {
            error = gif_player_next_frame(gif, s_gif_frame, sizeof(s_gif_frame), &delay_ms);
        } else {
            error = gif_player_next_frame_rows(gif, s_scaled_gif_rows, sizeof(s_scaled_gif_rows),
                                               display_gif_rows, &target, &delay_ms);
        }
        if (error != ESP_OK) {
            if (index > 0 && gif_player_rewind(gif) == ESP_OK) {
                index = 0;
                continue;
            }
            ESP_LOGW(TAG, "frame failed for %s: %s", path,
                     esp_err_to_name(error));
            break;
        }
        if (index == 0) {
            ESP_LOGI(TAG, "first GIF frame decoded: %s", path);
        }
        if (legacy) error = display_scaled_gif_frame(display,
            (info.width - GIF_RENDER_WIDTH) / 2, (info.height - GIF_RENDER_HEIGHT) / 2);
        if (error != ESP_OK) {
            ESP_LOGW(TAG, "first GIF frame flush failed for %s: %s",
                     path, esp_err_to_name(error));
            break;
        }
        if (index == 0) {
            ESP_LOGI(TAG, "first GIF frame displayed: %s", path);
        }
        ++index;
        vTaskDelay(pdMS_TO_TICKS(delay_ms == 0 ? 1 : delay_ms));
    }

    ESP_ERROR_CHECK(gif_player_close(gif));
    return error;
}

static void wait_for_asset_update(display_handle_t display, QueueHandle_t ota_queue,
                                  bool *ota_active)
{
    ESP_ERROR_CHECK(display_clear(display, GRAPHICS_BLACK));
    while (true) {
        handle_ota_events(display, ota_queue, ota_active);
        gif_update_event_t event;
        if (xQueueReceive(s_gif_update_queue, &event, pdMS_TO_TICKS(100)) == pdTRUE) return;
    }
}

static void holographic_screen_task(void *arg)
{
    (void)arg;
    display_handle_t display = NULL;
    QueueHandle_t ota_queue = xQueueCreate(1, sizeof(ota_progress_t));
    s_gif_update_queue = xQueueCreate(1, sizeof(gif_update_event_t));
    display_info_t info;
    bool ota_active = false;

    if (ota_queue == NULL || s_gif_update_queue == NULL) {
        ESP_LOGE(TAG, "cannot allocate OTA progress queue");
        vTaskDelete(NULL);
        return;
    }

    ESP_ERROR_CHECK(display_init(DISPLAY_DEVICE_ILI9342C, &display));
    ESP_ERROR_CHECK(display_get_info(display, &info));
    ESP_ERROR_CHECK(display_clear(display, GRAPHICS_BLACK));
    ESP_ERROR_CHECK(storage_mount());
    storage_set_gif_update_callback(gif_update_callback, s_gif_update_queue);
    ESP_ERROR_CHECK(ota_service_set_progress_callback(
        holographic_ota_callback, ota_queue));
    ESP_ERROR_CHECK(ota_service_start());
    ESP_ERROR_CHECK(component_uart_start());
    ESP_ERROR_CHECK(component_uart_confirm_ready());

    ESP_LOGI(TAG, "GIF display started: %s", GIF_PATH);
    while (true) {
        esp_err_t error = play_asset(display, ota_queue, &ota_active, GIF_PATH);
        if (error == ESP_ERR_INVALID_STATE) continue;
        if (error != ESP_OK) {
            ESP_LOGW(TAG, "no playable GIF assets found");
            wait_for_asset_update(display, ota_queue, &ota_active);
        }
    }
}

esp_err_t application_start(void)
{
    BaseType_t result = xTaskCreate(holographic_screen_task, "holographic",
                                    4096, NULL, 5, NULL);
    return result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

#endif
