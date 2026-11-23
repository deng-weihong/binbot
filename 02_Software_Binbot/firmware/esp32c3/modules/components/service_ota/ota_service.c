#include "ota_service.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"
#include "storage.h"

#define OTA_WIFI_CONNECTED BIT0
#define OTA_WIFI_MAX_RETRIES 8
#define OTA_WIFI_MAX_TX_POWER_QDBM 8 /* 2 dBm, in 0.25 dBm units */
#define OTA_CONTROL_LINE_BYTES 384
#define OTA_CONTROL_CONNECT_RETRY_MS 5000
#define OTA_CONTROL_KEEPALIVE_MS 15000
#define OTA_FILE_MAX_SIZE (512 * 1024)
#define OTA_FILE_REQUEST_ID_BYTES 17

#if CONFIG_BINBOT_APP_ROUND_SCREEN
#define OTA_PROFILE_NAME "round_screen"
#elif CONFIG_BINBOT_APP_HOLOGRAPHIC_SCREEN
#define OTA_PROFILE_NAME "holographic_screen"
#else
#define OTA_PROFILE_NAME "unknown"
#endif

static const char *TAG = "service_ota";
static EventGroupHandle_t s_event_group;
static uint8_t s_retry_count;
static ota_progress_callback_t s_progress_callback;
static void *s_progress_context;
static esp_netif_t *s_wifi_netif;
static bool s_wifi_initialized;
static bool s_wifi_started;
static bool s_wifi_event_handlers_registered;

static void ota_report(ota_progress_state_t state, uint8_t percent,
                       size_t bytes_written, size_t total_bytes)
{
    if (s_progress_callback == NULL) {
        return;
    }
    const ota_progress_t progress = {
        .state = state,
        .percent = percent,
        .bytes_written = bytes_written,
        .total_bytes = total_bytes,
    };
    s_progress_callback(&progress, s_progress_context);
}

void ota_service_report_progress(ota_progress_state_t state, uint8_t percent,
                                 size_t written, size_t total)
{
    ota_report(state, percent, written, total);
}

static int version_compare(const char *left, const char *right)
{
    const char *left_cursor = left;
    const char *right_cursor = right;
    while (*left_cursor != '\0' || *right_cursor != '\0') {
        if (!isdigit((unsigned char)*left_cursor) ||
            !isdigit((unsigned char)*right_cursor)) {
            return strcmp(left, right);
        }
        unsigned long left_number = 0;
        unsigned long right_number = 0;
        while (isdigit((unsigned char)*left_cursor)) {
            left_number = left_number * 10 +
                          (unsigned long)(*left_cursor - '0');
            ++left_cursor;
        }
        while (isdigit((unsigned char)*right_cursor)) {
            right_number = right_number * 10 +
                           (unsigned long)(*right_cursor - '0');
            ++right_cursor;
        }
        if (left_number < right_number) {
            return -1;
        }
        if (left_number > right_number) {
            return 1;
        }
        if (*left_cursor == '.') {
            ++left_cursor;
        } else if (*left_cursor != '\0') {
            return strcmp(left, right);
        }
        if (*right_cursor == '.') {
            ++right_cursor;
        } else if (*right_cursor != '\0') {
            return strcmp(left, right);
        }
    }
    return 0;
}

static void ota_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *disconnected =
            (const wifi_event_sta_disconnected_t *)event_data;
        xEventGroupClearBits(s_event_group, OTA_WIFI_CONNECTED);
        ESP_LOGW(TAG, "Wi-Fi disconnected: reason=%d",
                 disconnected != NULL ? (int)disconnected->reason : -1);
        if (s_retry_count++ < OTA_WIFI_MAX_RETRIES) {
            (void)esp_wifi_connect();
        } else {
            s_retry_count = 0;
            ESP_LOGW(TAG, "Wi-Fi retries exhausted; restarting connection");
            (void)esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_retry_count = 0;
        xEventGroupSetBits(s_event_group, OTA_WIFI_CONNECTED);
    }
}

static esp_err_t ota_wifi_start(void)
{
    esp_err_t error = nvs_flash_init();
    if (error == ESP_ERR_NVS_NO_FREE_PAGES ||
        error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        error = nvs_flash_erase();
        if (error != ESP_OK) {
            return error;
        }
        error = nvs_flash_init();
    }
    if (error != ESP_OK) {
        return error;
    }
    error = esp_netif_init();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        return error;
    }
    error = esp_event_loop_create_default();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        return error;
    }
    s_wifi_netif = esp_netif_create_default_wifi_sta();
    if (s_wifi_netif == NULL) {
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    error = esp_wifi_init(&wifi_config);
    if (error != ESP_OK) {
        return error;
    }
    s_wifi_initialized = true;
    error = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                       &ota_event_handler, NULL);
    if (error != ESP_OK) {
        return error;
    }
    s_wifi_event_handlers_registered = true;
    error = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                       &ota_event_handler, NULL);
    if (error != ESP_OK) {
        return error;
    }

    wifi_config_t station_config = {0};
    strncpy((char *)station_config.sta.ssid, CONFIG_BINBOT_OTA_WIFI_SSID,
            sizeof(station_config.sta.ssid) - 1);
    strncpy((char *)station_config.sta.password, CONFIG_BINBOT_OTA_WIFI_PASSWORD,
            sizeof(station_config.sta.password) - 1);
    error = esp_wifi_set_mode(WIFI_MODE_STA);
    if (error == ESP_OK) {
        error = esp_wifi_set_config(WIFI_IF_STA, &station_config);
    }
    if (error == ESP_OK) {
        error = esp_wifi_start();
    }
    if (error == ESP_OK) {
        s_wifi_started = true;
    }
    if (error == ESP_OK) {
        error = esp_wifi_set_max_tx_power(OTA_WIFI_MAX_TX_POWER_QDBM);
    }
    if (error == ESP_OK) {
        ESP_LOGI(TAG, "Wi-Fi maximum transmit power set to 2 dBm");
        error = esp_wifi_connect();
    }
    return error;
}

static void ota_wifi_stop(void)
{
    if (s_wifi_started) {
        (void)esp_wifi_disconnect();
        (void)esp_wifi_stop();
        s_wifi_started = false;
    }
    if (s_wifi_event_handlers_registered) {
        (void)esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                           &ota_event_handler);
        (void)esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                           &ota_event_handler);
        s_wifi_event_handlers_registered = false;
    }
    if (s_wifi_initialized) {
        (void)esp_wifi_deinit();
        s_wifi_initialized = false;
    }
    if (s_wifi_netif != NULL) {
        esp_netif_destroy_default_wifi(s_wifi_netif);
        s_wifi_netif = NULL;
    }
}

static void ota_control_close(int *socket_fd)
{
    if (socket_fd != NULL && *socket_fd >= 0) {
        shutdown(*socket_fd, SHUT_RDWR);
        close(*socket_fd);
        *socket_fd = -1;
    }
}

static int ota_control_connect(void)
{
    char port[8];
    (void)snprintf(port, sizeof(port), "%d", CONFIG_BINBOT_OTA_TCP_PORT);

    struct addrinfo hints = {0};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *results = NULL;
    int error = getaddrinfo(CONFIG_BINBOT_OTA_TCP_HOST, port, &hints,
                            &results);
    if (error != 0 || results == NULL) {
        ESP_LOGW(TAG, "TCP control DNS failed for %s", CONFIG_BINBOT_OTA_TCP_HOST);
        return -1;
    }

    int socket_fd = -1;
    for (struct addrinfo *entry = results; entry != NULL; entry = entry->ai_next) {
        socket_fd = socket(entry->ai_family, entry->ai_socktype,
                           entry->ai_protocol);
        if (socket_fd < 0) {
            continue;
        }
        struct timeval timeout = {
            .tv_sec = 5,
            .tv_usec = 0,
        };
        (void)setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                         sizeof(timeout));
        (void)setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                         sizeof(timeout));
        int keepalive = 1;
        (void)setsockopt(socket_fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive,
                         sizeof(keepalive));
        if (connect(socket_fd, entry->ai_addr, entry->ai_addrlen) == 0) {
            break;
        }
        close(socket_fd);
        socket_fd = -1;
    }
    freeaddrinfo(results);
    return socket_fd;
}

static esp_err_t ota_control_send(int socket_fd, const char *message)
{
    if (socket_fd < 0 || message == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t length = strlen(message);
    size_t sent = 0;
    while (sent < length) {
        const int result = send(socket_fd, message + sent, length - sent, 0);
        if (result <= 0) {
            return ESP_FAIL;
        }
        sent += (size_t)result;
    }
    return ESP_OK;
}

static esp_err_t ota_control_send_hello(int socket_fd)
{
    uint8_t mac[6] = {0};
    esp_err_t error = esp_wifi_get_mac(WIFI_IF_STA, mac);
    if (error != ESP_OK) {
        return error;
    }
    const esp_app_desc_t *app = esp_app_get_description();
    char message[128];
    (void)snprintf(message, sizeof(message),
                   "HELLO %02X%02X%02X%02X%02X%02X %s %s\n",
                   mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                   app->version, OTA_PROFILE_NAME);
    return ota_control_send(socket_fd, message);
}

static esp_err_t ota_control_read_line(int socket_fd, char *line,
                                        size_t line_size, size_t *length)
{
    if (socket_fd < 0 || line == NULL || line_size < 2 || length == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(socket_fd, &read_set);
    struct timeval timeout = {
        .tv_sec = 1,
        .tv_usec = 0,
    };
    const int ready = select(socket_fd + 1, &read_set, NULL, NULL, &timeout);
    if (ready < 0) {
        return ESP_FAIL;
    }
    if (ready == 0) {
        return ESP_ERR_TIMEOUT;
    }

    char value = '\0';
    const int received = recv(socket_fd, &value, 1, 0);
    if (received <= 0) {
        return ESP_FAIL;
    }

    if (value == '\r') {
        return ESP_ERR_TIMEOUT;
    }
    if (value == '\n') {
        line[*length] = '\0';
        *length = 0;
        return ESP_OK;
    }
    if (*length + 1 >= line_size) {
        *length = 0;
        return ESP_ERR_INVALID_SIZE;
    }
    line[(*length)++] = value;
    return ESP_ERR_TIMEOUT;
}

static esp_err_t ota_download_image(const char *firmware_url,
                                    const char *requested_version,
                                    bool *no_update)
{
    if (firmware_url == NULL || no_update == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *no_update = false;

    const esp_http_client_config_t http_config = {
        .url = firmware_url,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
    };
    const esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };
    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t error = esp_https_ota_begin(&ota_config, &ota_handle);
    if (error != ESP_OK) {
        return error;
    }

    esp_app_desc_t new_app = {0};
    error = esp_https_ota_get_img_desc(ota_handle, &new_app);
    const esp_app_desc_t *running_app = esp_app_get_description();
    if (error == ESP_OK && requested_version != NULL &&
        requested_version[0] != '\0' &&
        version_compare(new_app.version, requested_version) < 0) {
        ESP_LOGW(TAG, "TCP command/image version mismatch: command=%s image=%s",
                 requested_version, new_app.version);
        error = ESP_ERR_INVALID_VERSION;
    }
    if (error == ESP_OK &&
        version_compare(new_app.version, running_app->version) <= 0) {
        ESP_LOGI(TAG, "image is not newer: local=%s image=%s",
                 running_app->version, new_app.version);
        *no_update = true;
    }

    if (error == ESP_OK && !*no_update) {
        const int image_size = esp_https_ota_get_image_size(ota_handle);
        ota_report(OTA_PROGRESS_DOWNLOADING, 0, 0,
                   image_size > 0 ? (size_t)image_size : 0);
        do {
            error = esp_https_ota_perform(ota_handle);
            const int image_read = esp_https_ota_get_image_len_read(ota_handle);
            const int total_size = esp_https_ota_get_image_size(ota_handle);
            uint8_t percent = 0;
            if (total_size > 0 && image_read >= 0) {
                const int value = (image_read * 100) / total_size;
                percent = (uint8_t)(value > 100 ? 100 : value);
            }
            ota_report(OTA_PROGRESS_DOWNLOADING, percent,
                       image_read > 0 ? (size_t)image_read : 0,
                       total_size > 0 ? (size_t)total_size : 0);
        } while (error == ESP_ERR_HTTPS_OTA_IN_PROGRESS);
    }

    if (error == ESP_OK && !*no_update) {
        error = esp_https_ota_finish(ota_handle);
        ota_handle = NULL;
    } else {
        (void)esp_https_ota_abort(ota_handle);
        ota_handle = NULL;
    }
    return error;
}

typedef struct {
    esp_http_client_handle_t client;
    size_t expected_size;
    size_t received;
} ota_file_download_t;

static esp_err_t parse_sha256(const char *text, uint8_t digest[32])
{
    if (text == NULL || digest == NULL || strlen(text) != 64) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t index = 0; index < 32; ++index) {
        unsigned int value = 0;
        if (sscanf(text + index * 2, "%2x", &value) != 1) {
            return ESP_ERR_INVALID_ARG;
        }
        digest[index] = (uint8_t)value;
    }
    return ESP_OK;
}

static esp_err_t ota_file_reader(void *context, uint8_t *buffer,
                                 size_t buffer_size, size_t *bytes_read)
{
    if (context == NULL || buffer == NULL || buffer_size == 0 ||
        bytes_read == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ota_file_download_t *download = (ota_file_download_t *)context;
    if (download->received >= download->expected_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    const size_t remaining = download->expected_size - download->received;
    const int request_size = (int)(remaining < buffer_size ? remaining :
                                   buffer_size);
    const int read_size = esp_http_client_read(download->client,
                                               (char *)buffer, request_size);
    if (read_size <= 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    download->received += (size_t)read_size;
    *bytes_read = (size_t)read_size;
    const uint8_t percent = (uint8_t)((download->received * 100) /
                                      download->expected_size);
    ota_report(OTA_PROGRESS_DOWNLOADING, percent, download->received,
               download->expected_size);
    return ESP_OK;
}

static esp_err_t ota_download_file(const char *name, size_t expected_size,
                                   const uint8_t expected_sha256[32],
                                   const char *file_url)
{
    if (name == NULL || expected_size > OTA_FILE_MAX_SIZE ||
        expected_sha256 == NULL || file_url == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_http_client_config_t config = {
        .url = file_url,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t error = esp_http_client_open(client, 0);
    int content_length = -1;
    if (error == ESP_OK) {
        content_length = esp_http_client_fetch_headers(client);
        if (content_length < 0 || (size_t)content_length != expected_size) {
            ESP_LOGW(TAG, "file size mismatch for %s: expected=%u received=%d",
                     name, (unsigned)expected_size, content_length);
            error = ESP_ERR_INVALID_SIZE;
        }
    }
    if (error == ESP_OK) {
        ota_file_download_t download = {
            .client = client,
            .expected_size = expected_size,
        };
        ota_report(OTA_PROGRESS_DOWNLOADING, 0, 0, expected_size);
        error = storage_replace_file(name, expected_size, expected_sha256,
                                     ota_file_reader, &download);
        if (error == ESP_OK && download.received != expected_size) {
            error = ESP_ERR_INVALID_SIZE;
        }
    }
    (void)esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return error;
}

typedef struct {
    int socket_fd;
    char request_id[OTA_FILE_REQUEST_ID_BYTES];
} ota_file_list_context_t;

static esp_err_t ota_list_file_callback(void *context, const char *name,
                                        size_t size)
{
    if (context == NULL || name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ota_file_list_context_t *list = (ota_file_list_context_t *)context;
    char line[128] = {0};
    const int written = snprintf(line, sizeof(line), "FS_FILE %s %s %u\n",
                                 list->request_id, name, (unsigned)size);
    if (written <= 0 || (size_t)written >= sizeof(line)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ota_control_send(list->socket_fd, line);
}

static esp_err_t ota_list_files(int socket_fd, const char *request_id)
{
    if (socket_fd < 0 || request_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char line[64] = {0};
    int written = snprintf(line, sizeof(line), "FS_LIST_BEGIN %s\n",
                           request_id);
    if (written <= 0 || (size_t)written >= sizeof(line)) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t error = ota_control_send(socket_fd, line);
    if (error != ESP_OK) {
        return error;
    }

    ota_file_list_context_t context = {
        .socket_fd = socket_fd,
    };
    (void)snprintf(context.request_id, sizeof(context.request_id), "%s",
                   request_id);
    error = storage_list_files(ota_list_file_callback, &context);
    if (error != ESP_OK) {
        return error;
    }

    written = snprintf(line, sizeof(line), "FS_LIST_END %s\n", request_id);
    if (written <= 0 || (size_t)written >= sizeof(line)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ota_control_send(socket_fd, line);
}

typedef struct {
    esp_http_client_handle_t client;
    size_t total_size;
    size_t sent;
} ota_file_upload_t;

static esp_err_t ota_http_upload_chunk(void *context, const uint8_t *buffer,
                                       size_t length)
{
    if (context == NULL || buffer == NULL || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    ota_file_upload_t *upload = (ota_file_upload_t *)context;
    const int written = esp_http_client_write(upload->client,
                                              (const char *)buffer,
                                              (int)length);
    if (written != (int)length) {
        return ESP_FAIL;
    }
    upload->sent += length;
    const uint8_t percent = upload->total_size == 0
                                ? 100
                                : (uint8_t)((upload->sent * 100) /
                                            upload->total_size);
    ota_report(OTA_PROGRESS_DOWNLOADING, percent, upload->sent,
               upload->total_size);
    return ESP_OK;
}

static esp_err_t ota_upload_file(const char *name, const char *file_url,
                                 size_t *size)
{
    if (name == NULL || file_url == NULL || size == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = storage_get_file_size(name, size);
    if (error != ESP_OK || *size > OTA_FILE_MAX_SIZE) {
        return error == ESP_OK ? ESP_ERR_INVALID_SIZE : error;
    }

    const esp_http_client_config_t config = {
        .url = file_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }
    (void)esp_http_client_set_header(client, "Content-Type",
                                     "application/octet-stream");

    error = esp_http_client_open(client, (int)*size);
    ota_file_upload_t upload = {
        .client = client,
        .total_size = *size,
    };
    if (error == ESP_OK) {
        ota_report(OTA_PROGRESS_DOWNLOADING, 0, 0, *size);
        error = storage_stream_file(name, ota_http_upload_chunk, &upload,
                                    size);
    }
    if (error == ESP_OK) {
        const int content_length = esp_http_client_fetch_headers(client);
        const int status = esp_http_client_get_status_code(client);
        if (content_length < 0 || status < 200 || status >= 300) {
            error = ESP_FAIL;
        }
    }
    (void)esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return error;
}

static esp_err_t ota_send_fs_error(int socket_fd, const char *request_id,
                                   esp_err_t error)
{
    if (socket_fd < 0 || request_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char line[96] = {0};
    const int written = snprintf(line, sizeof(line), "FS_ERROR %s %s\n",
                                 request_id, esp_err_to_name(error));
    if (written <= 0 || (size_t)written >= sizeof(line)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ota_control_send(socket_fd, line);
}

static esp_err_t ota_send_fs_result(int socket_fd, const char *format,
                                    const char *request_id, const char *name,
                                    size_t size)
{
    if (socket_fd < 0 || format == NULL || request_id == NULL || name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char line[128] = {0};
    const int written = snprintf(line, sizeof(line), format, request_id, name,
                                 (unsigned)size);
    if (written <= 0 || (size_t)written >= sizeof(line)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ota_control_send(socket_fd, line);
}

static void ota_task(void *arg)
{
    (void)arg;
#if !CONFIG_BINBOT_OTA_TCP_ENABLE
    ESP_LOGI(TAG, "TCP OTA control disabled");
    ota_report(OTA_PROGRESS_NO_UPDATE, 100, 0, 0);
    vTaskDelete(NULL);
    return;
#else
    if (CONFIG_BINBOT_OTA_WIFI_SSID[0] == '\0') {
        ESP_LOGI(TAG, "Wi-Fi disabled: configure a Wi-Fi SSID");
        ota_report(OTA_PROGRESS_NO_UPDATE, 100, 0, 0);
        vTaskDelete(NULL);
        return;
    }

    esp_err_t error = ota_wifi_start();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi startup failed: %s", esp_err_to_name(error));
        ota_wifi_stop();
        ota_report(OTA_PROGRESS_FAILED, 0, 0, 0);
        vTaskDelete(NULL);
        return;
    }

    int control_socket = -1;
    bool startup_state_reported = false;
    size_t line_length = 0;
    char line[OTA_CONTROL_LINE_BYTES] = {0};
    int64_t last_keepalive_us = esp_timer_get_time();

    ESP_LOGI(TAG, "TCP OTA control connecting to %s:%d",
             CONFIG_BINBOT_OTA_TCP_HOST, CONFIG_BINBOT_OTA_TCP_PORT);
    while (true) {
        EventBits_t bits = xEventGroupGetBits(s_event_group);
        if ((bits & OTA_WIFI_CONNECTED) == 0) {
            if (!startup_state_reported) {
                ota_report(OTA_PROGRESS_FAILED, 0, 0, 0);
                startup_state_reported = true;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (control_socket < 0) {
            control_socket = ota_control_connect();
            if (control_socket < 0) {
                if (!startup_state_reported) {
                    ota_report(OTA_PROGRESS_FAILED, 0, 0, 0);
                    startup_state_reported = true;
                }
                vTaskDelay(pdMS_TO_TICKS(OTA_CONTROL_CONNECT_RETRY_MS));
                continue;
            }
            if (ota_control_send_hello(control_socket) != ESP_OK) {
                ota_control_close(&control_socket);
                vTaskDelay(pdMS_TO_TICKS(OTA_CONTROL_CONNECT_RETRY_MS));
                continue;
            }
            ESP_LOGI(TAG, "TCP OTA control connected");
            ota_report(OTA_PROGRESS_CHECKING, 0, 0, 0);
            ota_report(OTA_PROGRESS_NO_UPDATE, 100, 0, 0);
            startup_state_reported = true;
            line_length = 0;
            last_keepalive_us = esp_timer_get_time();
        }

        error = ota_control_read_line(control_socket, line,
                                      sizeof(line), &line_length);
        if (error == ESP_ERR_TIMEOUT) {
            const int64_t now_us = esp_timer_get_time();
            if ((now_us - last_keepalive_us) / 1000 >=
                OTA_CONTROL_KEEPALIVE_MS) {
                if (ota_control_send(control_socket, "PING\n") != ESP_OK) {
                    ota_control_close(&control_socket);
                    ota_report(OTA_PROGRESS_FAILED, 0, 0, 0);
                    vTaskDelay(pdMS_TO_TICKS(OTA_CONTROL_CONNECT_RETRY_MS));
                }
                last_keepalive_us = now_us;
            }
            continue;
        }
        if (error != ESP_OK) {
            ESP_LOGW(TAG, "TCP OTA control disconnected");
            ota_control_close(&control_socket);
            ota_report(OTA_PROGRESS_FAILED, 0, 0, 0);
            vTaskDelay(pdMS_TO_TICKS(OTA_CONTROL_CONNECT_RETRY_MS));
            continue;
        }

        if (strcmp(line, "PONG") == 0) {
            continue;
        }
        if (strcmp(line, "PING") == 0) {
            (void)ota_control_send(control_socket, "PONG\n");
            continue;
        }
        if (strcmp(line, "OK") == 0) {
            continue;
        }

        if (strncmp(line, "FS_LIST ", 8) == 0) {
            char request_id[OTA_FILE_REQUEST_ID_BYTES] = {0};
            if (sscanf(line, "FS_LIST %16s", request_id) != 1) {
                (void)ota_control_send(control_socket,
                                       "ERROR invalid-fs-command\n");
                continue;
            }
            error = ota_list_files(control_socket, request_id);
            if (error != ESP_OK) {
                (void)ota_send_fs_error(control_socket, request_id, error);
            }
            continue;
        }

        if (strncmp(line, "FS_GET ", 7) == 0) {
            char request_id[OTA_FILE_REQUEST_ID_BYTES] = {0};
            char name[64] = {0};
            char upload_url[192] = {0};
            if (sscanf(line, "FS_GET %16s %63s %191s", request_id, name,
                       upload_url) != 3) {
                (void)ota_control_send(control_socket,
                                       "ERROR invalid-fs-command\n");
                continue;
            }
            ESP_LOGI(TAG, "remote file read requested: %s", name);
            ota_report(OTA_PROGRESS_CHECKING, 0, 0, 0);
            size_t file_size = 0;
            error = ota_upload_file(name, upload_url, &file_size);
            if (error != ESP_OK) {
                ESP_LOGW(TAG, "remote file read failed for %s: %s", name,
                         esp_err_to_name(error));
                ota_report(OTA_PROGRESS_FAILED, 0, 0, file_size);
                (void)ota_send_fs_error(control_socket, request_id, error);
            } else {
                ota_report(OTA_PROGRESS_COMPLETE, 100, file_size, file_size);
                (void)ota_send_fs_result(control_socket,
                                         "FS_READ %s %s %u\n", request_id,
                                         name, file_size);
            }
            continue;
        }

        if (strncmp(line, "FS_DELETE ", 10) == 0) {
            char request_id[OTA_FILE_REQUEST_ID_BYTES] = {0};
            char name[64] = {0};
            if (sscanf(line, "FS_DELETE %16s %63s", request_id, name) != 2) {
                (void)ota_control_send(control_socket,
                                       "ERROR invalid-fs-command\n");
                continue;
            }
            ESP_LOGI(TAG, "remote file delete requested: %s", name);
            error = storage_delete_file(name);
            if (error != ESP_OK) {
                (void)ota_send_fs_error(control_socket, request_id, error);
            } else {
                (void)ota_send_fs_result(control_socket,
                                         "FS_DELETED %s %s %u\n", request_id,
                                         name, 0);
            }
            continue;
        }

        bool legacy_file_update = false;
        if (strncmp(line, "FS_PUT ", 7) == 0 ||
            strncmp(line, "FILE_UPDATE ", 12) == 0) {
            char request_id[OTA_FILE_REQUEST_ID_BYTES] = {0};
            char name[64] = {0};
            char size_text[16] = {0};
            char digest_text[65] = {0};
            char file_url[192] = {0};
            int fields = 0;
            if (strncmp(line, "FS_PUT ", 7) == 0) {
                fields = sscanf(line, "FS_PUT %16s %63s %15s %64s %191s",
                                request_id, name, size_text, digest_text,
                                file_url);
            } else {
                legacy_file_update = true;
                (void)snprintf(request_id, sizeof(request_id), "legacy");
                fields = sscanf(line, "FILE_UPDATE %63s %15s %64s %191s",
                                name, size_text, digest_text, file_url);
            }
            if (fields != (legacy_file_update ? 4 : 5)) {
                ESP_LOGW(TAG, "invalid file update command: %s", line);
                (void)ota_control_send(control_socket,
                                       "ERROR invalid-file-command\n");
                continue;
            }
            char *end = NULL;
            const unsigned long parsed_size = strtoul(size_text, &end, 10);
            uint8_t digest[32] = {0};
            if (end == size_text || *end != '\0' ||
                parsed_size > OTA_FILE_MAX_SIZE ||
                parse_sha256(digest_text, digest) != ESP_OK) {
                ESP_LOGW(TAG, "invalid file update metadata: %s", line);
                (void)ota_control_send(control_socket,
                                       "ERROR invalid-file-metadata\n");
                continue;
            }

            ESP_LOGI(TAG, "remote file write requested: %s (%lu bytes)",
                     name, parsed_size);
            ota_report(OTA_PROGRESS_CHECKING, 0, 0, (size_t)parsed_size);
            error = ota_download_file(name, (size_t)parsed_size, digest,
                                      file_url);
            if (error != ESP_OK) {
                ESP_LOGW(TAG, "file update failed for %s: %s", name,
                         esp_err_to_name(error));
                ota_report(OTA_PROGRESS_FAILED, 0, 0, (size_t)parsed_size);
                if (legacy_file_update) {
                    (void)ota_control_send(control_socket,
                                           "ERROR file-update-failed\n");
                } else {
                    (void)ota_send_fs_error(control_socket, request_id, error);
                }
            } else {
                ota_report(OTA_PROGRESS_COMPLETE, 100, (size_t)parsed_size,
                           (size_t)parsed_size);
                if (legacy_file_update) {
                    (void)ota_control_send(control_socket, "FILE_UPDATED\n");
                } else {
                    (void)ota_send_fs_result(control_socket,
                                             "FS_UPDATED %s %s %u\n",
                                             request_id, name, parsed_size);
                }
            }
            continue;
        }

        char requested_version[32] = {0};
        char firmware_url[192] = {0};
        if (sscanf(line, "UPDATE %31s %191s", requested_version,
                   firmware_url) != 2) {
            ESP_LOGW(TAG, "unknown TCP OTA command: %s", line);
            (void)ota_control_send(control_socket, "ERROR invalid-command\n");
            continue;
        }

        ESP_LOGI(TAG, "active OTA requested: version=%s url=%s",
                 requested_version, firmware_url);
        ota_report(OTA_PROGRESS_CHECKING, 0, 0, 0);
        bool no_update = false;
        error = ota_download_image(firmware_url, requested_version, &no_update);
        if (error == ESP_OK && no_update) {
            ota_report(OTA_PROGRESS_NO_UPDATE, 100, 0, 0);
            (void)ota_control_send(control_socket, "NO_UPDATE\n");
            continue;
        }
        if (error != ESP_OK) {
            ESP_LOGW(TAG, "active OTA failed: %s", esp_err_to_name(error));
            ota_report(OTA_PROGRESS_FAILED, 0, 0, 0);
            (void)ota_control_send(control_socket, "ERROR ota-failed\n");
            continue;
        }

        ota_report(OTA_PROGRESS_COMPLETE, 100, 1, 1);
        (void)ota_control_send(control_socket, "UPDATE_ACCEPTED\n");
        ESP_LOGI(TAG, "OTA image accepted; restarting");
        vTaskDelay(pdMS_TO_TICKS(250));
        esp_restart();
    }
#endif
}

esp_err_t ota_service_set_progress_callback(ota_progress_callback_t callback,
                                             void *user_context)
{
    s_progress_callback = callback;
    s_progress_context = user_context;
    return ESP_OK;
}

esp_err_t ota_service_start(void)
{
#if CONFIG_BINBOT_OTA_ENABLE
    s_event_group = xEventGroupCreate();
    if (s_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }
    return xTaskCreate(ota_task, "ota_service", 6144, NULL, 6, NULL) == pdPASS
               ? ESP_OK
               : ESP_ERR_NO_MEM;
#else
    return ESP_OK;
#endif
}

esp_err_t ota_service_confirm_running_image(void)
{
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    esp_err_t error = esp_ota_mark_app_valid_cancel_rollback();
    if (error == ESP_ERR_NOT_FOUND) {
        return ESP_OK;
    }
    return error;
#else
    return ESP_OK;
#endif
}
