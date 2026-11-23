#include "application.h"

#if CONFIG_BINBOT_APP_ROUND_SCREEN

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

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

#if !CONFIG_BINBOT_DISPLAY_GC9A01A
#error "round_screen requires the GC9A01A display driver"
#endif

static const char *TAG = "app_round_screen";

#define GIF_WIDTH 240
#define GIF_HEIGHT 240
#define OTA_ROW_WIDTH 240
#define OTA_ROW_START 96
#define OTA_ROW_COUNT 48
#define DEFAULT_GIF_NAME "display.gif"
#define GIF_PATH BINBOT_STORAGE_PATH "/" DEFAULT_GIF_NAME

static uint8_t s_gif_frame[GIF_WIDTH * GIF_HEIGHT * 2];
static QueueHandle_t s_gif_update_queue;

typedef struct {
    char name[64];
} gif_update_event_t;

static void round_ota_callback(const ota_progress_t *progress,
                               void *user_context)
{
    if (progress == NULL || user_context == NULL) {
        return;
    }
    (void)xQueueOverwrite((QueueHandle_t)user_context, progress);
}

static void gif_update_callback(void *context, const char *name)
{
    if (context == NULL || name == NULL) {
        return;
    }
    gif_update_event_t event;
    (void)snprintf(event.name, sizeof(event.name), "%s", name);
    (void)xQueueOverwrite((QueueHandle_t)context, &event);
    component_pen_show_image();
    ESP_LOGI(TAG, "GIF updated: %s", name);
}

static esp_err_t show_ota_progress(display_handle_t display, uint8_t percent,
                                   bool complete)
{
    uint8_t row[OTA_ROW_WIDTH * 2];
    for (uint16_t y = OTA_ROW_START; y < OTA_ROW_START + OTA_ROW_COUNT; ++y) {
        esp_err_t error = graphics_render_ota_row(
            row, sizeof(row), OTA_ROW_WIDTH, y, percent, complete);
        if (error != ESP_OK) {
            return error;
        }
        error = display_flush_region(display, row, sizeof(row), 0, y,
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

static esp_err_t play_asset(display_handle_t display, QueueHandle_t ota_queue,
                            bool *ota_active, const char *path,
                            uint16_t x, uint16_t y)
{
    gif_player_handle_t gif = NULL;
    ESP_LOGI(TAG, "opening GIF: %s", path);
    esp_err_t error = gif_player_open(path, &gif);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "cannot open %s: %s", path,
                 esp_err_to_name(error));
        return error;
    }
    ESP_LOGI(TAG, "GIF opened: %s", path);

    uint16_t frame_index = 0;
    bool first_frame = true;
    while (true) {
        handle_ota_events(display, ota_queue, ota_active);
        bb_component_pen_t pen;
        component_pen_snapshot(&pen);
        if(pen.mode == BB_PEN_INFO) { gif_player_close(gif); return ESP_ERR_INVALID_STATE; }

        // Check for GIF update event
        gif_update_event_t update_event;
        if (s_gif_update_queue && xQueueReceive(s_gif_update_queue, &update_event, 0) == pdTRUE) {
            ESP_LOGI(TAG, "GIF update detected, reloading...");
            gif_player_close(gif);
            return ESP_ERR_INVALID_STATE; // Signal to reload
        }

        if (*ota_active) {
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }

        uint32_t delay_ms = 0;
        if (first_frame) {
            ESP_LOGI(TAG, "decoding first GIF frame: %s", path);
        }
        error = gif_player_next_frame(gif, s_gif_frame, sizeof(s_gif_frame),
                                      &delay_ms);
        if (error != ESP_OK) {
            // End of GIF or error - rewind and restart
            if (frame_index > 0) {
                ESP_LOGI(TAG, "GIF ended at frame %u, rewinding: %s", frame_index, path);
                error = gif_player_rewind(gif);
                if (error != ESP_OK) {
                    ESP_LOGW(TAG, "cannot rewind %s: %s", path,
                             esp_err_to_name(error));
                    break;
                }
                frame_index = 0;
                first_frame = true;
                continue;
            } else {
                ESP_LOGW(TAG, "frame failed for %s: %s", path,
                         esp_err_to_name(error));
                break;
            }
        }
        if (first_frame) {
            ESP_LOGI(TAG, "first GIF frame decoded: %s", path);
        }
        error = display_flush_region(display, s_gif_frame, sizeof(s_gif_frame),
                                     x, y, GIF_WIDTH, GIF_HEIGHT);
        if (error != ESP_OK) {
            ESP_LOGW(TAG, "GIF frame flush failed for %s: %s",
                     path, esp_err_to_name(error));
            break;
        }
        if (first_frame) {
            ESP_LOGI(TAG, "first GIF frame displayed: %s", path);
            first_frame = false;
        }
        frame_index++;
        /* Keep mode changes and OTA responsive even for a long GIF frame. */
        if(!delay_ms) delay_ms = 1;
        while(delay_ms) {
            uint32_t slice = delay_ms > 30 ? 30 : delay_ms;
            vTaskDelay(pdMS_TO_TICKS(slice) ? pdMS_TO_TICKS(slice) : 1);
            delay_ms -= slice;
            component_pen_snapshot(&pen);
            if(pen.mode == BB_PEN_INFO || uxQueueMessagesWaiting(s_gif_update_queue) ||
               uxQueueMessagesWaiting(ota_queue)) break;
        }
    }

    ESP_ERROR_CHECK(gif_player_close(gif));
    return error;
}

static void round_screen_task(void *arg)
{
    (void)arg;
    display_handle_t display = NULL;
    QueueHandle_t ota_queue = xQueueCreate(1, sizeof(ota_progress_t));
    s_gif_update_queue = xQueueCreate(1, sizeof(gif_update_event_t));
    display_info_t info;
    bool ota_active = false;

    if (ota_queue == NULL || s_gif_update_queue == NULL) {
        ESP_LOGE(TAG, "cannot allocate queues");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "starting GC9A01A display initialization");
    ESP_ERROR_CHECK(display_init(DISPLAY_DEVICE_GC9A01A, &display));
    ESP_ERROR_CHECK(display_get_info(display, &info));
    ESP_ERROR_CHECK(display_clear(display, GRAPHICS_BLACK));
    ESP_ERROR_CHECK(storage_mount());
    storage_set_gif_update_callback(gif_update_callback, s_gif_update_queue);
    ESP_ERROR_CHECK(ota_service_set_progress_callback(round_ota_callback,
                                                       ota_queue));
    ESP_ERROR_CHECK(ota_service_start());
    ESP_ERROR_CHECK(component_uart_start());
    ESP_ERROR_CHECK(component_uart_confirm_ready());

    const uint16_t x = (info.width - GIF_WIDTH) / 2;
    const uint16_t y = (info.height - GIF_HEIGHT) / 2;
    ESP_LOGI(TAG, "Starting GIF player: %s", DEFAULT_GIF_NAME);

    uint8_t previous[BB_COMPONENT_PEN_BYTES] = {255};
    while (true) {
        if(ota_active) previous[0] = 255;
        handle_ota_events(display, ota_queue, &ota_active);
        if(ota_active) { vTaskDelay(pdMS_TO_TICKS(30)); continue; }
        bb_component_pen_t pen, stamp;
        component_pen_snapshot(&pen);
        if(pen.mode == BB_PEN_INFO) {
            stamp = pen; stamp.unix_time /= 60;
            uint8_t bytes[BB_COMPONENT_PEN_BYTES];
            bb_component_pen_encode(&stamp, bytes);
            if(memcmp(previous, bytes, sizeof(bytes))) {
                bb_component_pen_render(&pen, s_gif_frame);
                ESP_ERROR_CHECK(display_flush_region(display, s_gif_frame, sizeof(s_gif_frame), x, y, GIF_WIDTH, GIF_HEIGHT));
                memcpy(previous, bytes, sizeof(bytes));
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        previous[0] = 255;
        esp_err_t error = play_asset(display, ota_queue, &ota_active,
                                     GIF_PATH, x, y);
        if (error == ESP_ERR_INVALID_STATE) {
            // GIF update detected, reload
            ESP_LOGI(TAG, "GIF updated, reloading...");
            continue;
        } else if (error != ESP_OK) {
            ESP_LOGW(TAG, "Failed to play %s: %s", DEFAULT_GIF_NAME,
                     esp_err_to_name(error));
            ESP_ERROR_CHECK(display_clear(display, GRAPHICS_BLACK));
            vTaskDelay(pdMS_TO_TICKS(250));
        }
    }
}

esp_err_t application_start(void)
{
    BaseType_t result = xTaskCreate(round_screen_task, "round_screen", 4096,
                                    NULL, 5, NULL);
    return result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

#endif
