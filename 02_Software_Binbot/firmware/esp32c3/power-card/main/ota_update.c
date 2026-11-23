#include "ota_update.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board_io.h"
#include "firmware_image.h"
#include "network.h"
#include "protocol.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "ota";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static power_card_ota_status_t s_status = {.state = "idle"};
static QueueHandle_t s_queue;

typedef struct {
    int64_t deadline;
} download_context_t;

static esp_err_t http_event(esp_http_client_event_t *event)
{
    const download_context_t *context = event->user_data;
    // read() may collect many small packets internally; enforce the deadline there too.
    if ((event->event_id == HTTP_EVENT_ON_HEADER || event->event_id == HTTP_EVENT_ON_DATA) &&
        esp_timer_get_time() >= context->deadline) {
        const int socket = esp_http_client_get_socket(event->client);
        if (socket >= 0) shutdown(socket, SHUT_RDWR);
    }
    return ESP_OK;
}

void power_card_ota_status(power_card_ota_status_t *status)
{
    portENTER_CRITICAL(&s_lock);
    *status = s_status;
    portEXIT_CRITICAL(&s_lock);
}

static void set_state(const char *state)
{
    portENTER_CRITICAL(&s_lock);
    snprintf(s_status.state, sizeof(s_status.state), "%s", state);
    portEXIT_CRITICAL(&s_lock);
}

static const char *download(const char *url, esp_err_t *detail)
{
    const char *failure = "no_ota_partition";
    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (!partition) return failure;
    char *buffer = malloc(4096);
    if (!buffer) return "out_of_memory";
    const download_context_t context = {.deadline = esp_timer_get_time() + 300000000};
    const esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 10000,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .disable_auto_redirect = true,
        .transport_type = HTTP_TRANSPORT_OVER_TCP,
        .user_agent = "power_card/0.2",
        .event_handler = http_event,
        .user_data = (void *)&context,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_ota_handle_t handle = 0;
    bool writing = false;
    uint32_t received = 0;
    uint8_t prefix[POWER_CARD_IMAGE_PREFIX_SIZE];
    size_t prefix_used = 0;
    if (!client) {
        failure = "http_init_failed";
        goto cleanup;
    }
    set_state("connecting");
    *detail = esp_http_client_set_header(client, "Accept-Encoding", "identity");
    if (*detail == ESP_OK) *detail = esp_http_client_open(client, 0);
    if (*detail != ESP_OK) {
        failure = "http_connect_failed";
        goto cleanup;
    }
    const int64_t total = esp_http_client_fetch_headers(client);
    if (esp_http_client_get_status_code(client) != 200) {
        failure = "http_status_not_200";
        goto cleanup;
    }
    if (total <= 0 || esp_http_client_is_chunked_response(client)) {
        failure = "content_length_required";
        goto cleanup;
    }
    if (total > partition->size || total < POWER_CARD_IMAGE_PREFIX_SIZE) {
        failure = "invalid_image_size";
        goto cleanup;
    }
    portENTER_CRITICAL(&s_lock);
    s_status.total = (uint32_t)total;
    portEXIT_CRITICAL(&s_lock);
    set_state("downloading");
    for (;;) {
        if (esp_timer_get_time() >= context.deadline) {
            failure = "download_deadline_exceeded";
            goto cleanup;
        }
        power_card_network_status_t network;
        power_card_network_status(&network);
        if (!network.connected) {
            failure = "wifi_disconnected";
            goto cleanup;
        }
        const int count = esp_http_client_read(client, buffer, 4096);
        if (esp_timer_get_time() >= context.deadline) {
            failure = "download_deadline_exceeded";
            goto cleanup;
        }
        if (count < 0) {
            failure = "http_read_failed";
            goto cleanup;
        }
        if (count == 0) {
            if (!esp_http_client_is_complete_data_received(client) || received != total) {
                failure = "incomplete_download";
                goto cleanup;
            }
            break;
        }
        if ((uint64_t)received + count > (uint64_t)total) {
            failure = "image_size_mismatch";
            goto cleanup;
        }
        size_t offset = 0;
        if (!writing) {
            const size_t remaining = sizeof(prefix) - prefix_used;
            const size_t copied = (size_t)count < remaining ? (size_t)count : remaining;
            memcpy(prefix + prefix_used, buffer, copied);
            prefix_used += copied;
            offset = copied;
            if (prefix_used == sizeof(prefix)) {
                esp_app_desc_t description;
                failure = power_card_image_check(prefix, sizeof(prefix), &description);
                if (failure) goto cleanup;
                portENTER_CRITICAL(&s_lock);
                memcpy(s_status.version, description.version, sizeof(s_status.version));
                portEXIT_CRITICAL(&s_lock);
                *detail = esp_ota_begin(partition, OTA_WITH_SEQUENTIAL_WRITES, &handle);
                if (*detail != ESP_OK) {
                    failure = "ota_begin_failed";
                    goto cleanup;
                }
                writing = true;
                *detail = esp_ota_write(handle, prefix, sizeof(prefix));
                if (*detail != ESP_OK) {
                    failure = "ota_write_failed";
                    goto cleanup;
                }
            }
        }
        if (writing && (size_t)count > offset) {
            *detail = esp_ota_write(handle, buffer + offset, (size_t)count - offset);
            if (*detail != ESP_OK) {
                failure = "ota_write_failed";
                goto cleanup;
            }
        }
        received += count;
        portENTER_CRITICAL(&s_lock);
        s_status.received = received;
        portEXIT_CRITICAL(&s_lock);
        vTaskDelay(1);
    }
    if (!writing) {
        failure = "truncated_header";
        goto cleanup;
    }
    set_state("validating");
    *detail = esp_ota_end(handle);
    writing = false;
    if (*detail != ESP_OK) {
        failure = "firmware_validation_failed";
        goto cleanup;
    }
    *detail = esp_ota_set_boot_partition(partition);
    failure = *detail == ESP_OK ? NULL : "activate_firmware_failed";
cleanup:
    if (writing) esp_ota_abort(handle);
    if (client) esp_http_client_cleanup(client);
    free(buffer);
    return failure;
}

static void ota_task(void *arg)
{
    (void)arg;
    char url[POWER_CARD_MAX_URL + 1];
    for (;;) {
        if (xQueueReceive(s_queue, url, portMAX_DELAY) != pdTRUE) continue;
        esp_err_t detail = ESP_OK;
        const char *failure = download(url, &detail);
        memset(url, 0, sizeof(url));
        if (!failure) {
            set_state("rebooting");
            ESP_LOGI(TAG, "Firmware verified; restarting ESP32 in 3 seconds");
            vTaskDelay(pdMS_TO_TICKS(3000));
            esp_restart();
        }
        portENTER_CRITICAL(&s_lock);
        snprintf(s_status.state, sizeof(s_status.state), "failed");
        if (detail == ESP_OK) snprintf(s_status.error, sizeof(s_status.error), "%s", failure);
        else snprintf(s_status.error, sizeof(s_status.error), "%s:%s", failure, esp_err_to_name(detail));
        s_status.busy = false;
        portEXIT_CRITICAL(&s_lock);
        power_card_maintenance_end();
        ESP_LOGE(TAG, "Update failed: %s (%s); original firmware retained", failure, esp_err_to_name(detail));
    }
}

esp_err_t power_card_ota_init(void)
{
    s_queue = xQueueCreate(1, POWER_CARD_MAX_URL + 1);
    if (!s_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreate(ota_task, "http_ota", 8192, NULL, 4, NULL) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

const char *power_card_ota_start(const char *url)
{
    if (!s_queue) return "ota_unavailable";
    if (!power_card_valid_ota_url(url)) return "invalid_url";
    power_card_network_status_t network;
    power_card_network_status(&network);
    if (!network.connected) return "not_connected";
    if (power_card_maintenance_begin() != ESP_OK) return "busy";
    portENTER_CRITICAL(&s_lock);
    s_status = (power_card_ota_status_t){.busy = true, .state = "queued"};
    portEXIT_CRITICAL(&s_lock);
    char queued_url[POWER_CARD_MAX_URL + 1] = {0};
    strcpy(queued_url, url);
    if (xQueueSend(s_queue, queued_url, 0) != pdTRUE) {
        portENTER_CRITICAL(&s_lock);
        s_status.busy = false;
        snprintf(s_status.state, sizeof(s_status.state), "failed");
        snprintf(s_status.error, sizeof(s_status.error), "queue_full");
        portEXIT_CRITICAL(&s_lock);
        power_card_maintenance_end();
        return "busy";
    }
    return NULL;
}

esp_err_t power_card_ota_confirm_boot(void)
{
    esp_ota_img_states_t state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_err_t error = esp_ota_get_state_partition(running, &state);
    if (error == ESP_ERR_NOT_FOUND || error == ESP_ERR_NOT_SUPPORTED) return ESP_OK;
    if (error != ESP_OK) return error;
    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "I/O, network stack and TCP listener initialized; confirming firmware");
        return esp_ota_mark_app_valid_cancel_rollback();
    }
    return ESP_OK;
}
