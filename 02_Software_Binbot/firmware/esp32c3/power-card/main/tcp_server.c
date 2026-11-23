#include "tcp_server.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "board_io.h"
#include "network.h"
#include "ota_update.h"
#include "protocol.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

#define MAX_CLIENTS 4
#define CLIENT_TIMEOUT_US 120000000

static const char *TAG = "tcp_server";
static int s_listener = -1;
typedef struct {
    int socket;
    int64_t last_request;
    power_card_frame_t frame;
} client_t;
static client_t s_clients[MAX_CLIENTS];

static cJSON *ota_status_json(void)
{
    power_card_ota_status_t status;
    power_card_ota_status(&status);
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "state", status.state);
    cJSON_AddBoolToObject(json, "busy", status.busy);
    cJSON_AddNumberToObject(json, "received", status.received);
    cJSON_AddNumberToObject(json, "total", status.total);
    cJSON_AddNumberToObject(json, "progress", status.total ? (double)status.received * 100 / status.total : 0);
    cJSON_AddStringToObject(json, "version", status.version);
    cJSON_AddStringToObject(json, "error", status.error);
    return json;
}

static cJSON *status_json(void)
{
    power_card_network_status_t network;
    power_card_network_status(&network);
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *partition = esp_ota_get_running_partition();
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "project", app->project_name);
    cJSON_AddStringToObject(json, "version", app->version);
    cJSON_AddNumberToObject(json, "uptime_ms", (double)(esp_timer_get_time() / 1000));
    cJSON_AddStringToObject(json, "ip", network.ip);
    cJSON_AddNumberToObject(json, "port", CONFIG_POWER_CARD_TCP_PORT);
    cJSON_AddNumberToObject(json, "pw_led_level", power_card_power_led_level());
    cJSON_AddStringToObject(json, "pc_power_state", "unknown");
    cJSON_AddStringToObject(json, "partition", partition ? partition->label : "unknown");
    cJSON_AddItemToObject(json, "ota", ota_status_json());
    return json;
}

static bool send_all(int socket, const char *data, size_t length)
{
    const int64_t deadline = esp_timer_get_time() + 2000000;
    while (length) {
        if (esp_timer_get_time() > deadline) return false;
        const int sent = send(socket, data, length, 0);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return false;
        data += sent;
        length -= sent;
    }
    return true;
}

static bool send_response(int socket, const power_card_request_t *request, const char *error, cJSON *data)
{
    cJSON *response = cJSON_CreateObject();
    if (!response) {
        cJSON_Delete(data);
        return false;
    }
    if (request && request->has_id) cJSON_AddStringToObject(response, "id", request->id);
    cJSON_AddBoolToObject(response, "ok", error == NULL);
    if (error) cJSON_AddStringToObject(response, "error", error);
    if (data) cJSON_AddItemToObject(response, "data", data);
    char *encoded = cJSON_PrintUnformatted(response);
    const bool sent = encoded && send_all(socket, encoded, strlen(encoded)) && send_all(socket, "\n", 1);
    cJSON_free(encoded);
    cJSON_Delete(response);
    return sent;
}

static bool handle_request(int socket, const char *line)
{
    power_card_request_t request;
    const char *error = power_card_parse_request(line, power_card_network_token(), &request);
    cJSON *data = NULL;
    if (!error) {
        switch (request.command) {
        case POWER_CARD_CMD_STATUS:
            data = status_json();
            break;
        case POWER_CARD_CMD_OTA_STATUS:
            data = ota_status_json();
            break;
        case POWER_CARD_CMD_POWER:
        case POWER_CARD_CMD_RESET: {
            const esp_err_t result = request.command == POWER_CARD_CMD_POWER ?
                                     power_card_press_power() : power_card_press_reset();
            if (result == ESP_ERR_TIMEOUT) error = "busy";
            else if (result == ESP_ERR_NOT_SUPPORTED) error = "output_disabled";
            else if (result != ESP_OK) error = "output_failed";
            break;
        }
        case POWER_CARD_CMD_OTA:
            error = power_card_ota_start(request.url);
            if (!error) {
                data = cJSON_CreateObject();
                cJSON_AddBoolToObject(data, "accepted", true);
            }
            break;
        }
    }
    return send_response(socket, &request, error, data);
}

static int create_listener(void)
{
    const int socket_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (socket_fd < 0) return -1;
    const int reuse = 1;
    const struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(CONFIG_POWER_CARD_TCP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0 ||
        bind(socket_fd, (const struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(socket_fd, MAX_CLIENTS) != 0) {
        close(socket_fd);
        return -1;
    }
    return socket_fd;
}

static void close_client(client_t *client)
{
    close(client->socket);
    client->socket = -1;
    client->frame.used = 0;
}

static void accept_client(void)
{
    const int socket_fd = accept(s_listener, NULL, NULL);
    if (socket_fd < 0) return;
    const struct timeval timeout = {.tv_sec = 2};
    const int keepalive = 1;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0 ||
        setsockopt(socket_fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive)) != 0) {
        close(socket_fd);
        return;
    }
    for (unsigned i = 0; i < MAX_CLIENTS; ++i) {
        if (s_clients[i].socket >= 0) continue;
        s_clients[i] = (client_t){.socket = socket_fd, .last_request = esp_timer_get_time()};
        return;
    }
    send_response(socket_fd, NULL, "too_many_clients", NULL);
    close(socket_fd);
}

static void read_client(client_t *client)
{
    unsigned char data[256];
    const int received = recv(client->socket, data, sizeof(data), 0);
    if (received < 0 && (errno == EINTR || errno == EAGAIN)) return;
    if (received <= 0) {
        close_client(client);
        return;
    }
    for (int i = 0; i < received; ++i) {
        const int result = power_card_frame_feed(&client->frame, data[i]);
        if (result < 0) {
            send_response(client->socket, NULL, "invalid_frame", NULL);
            close_client(client);
            return;
        }
        if (result == 1) {
            if (!handle_request(client->socket, client->frame.data)) {
                close_client(client);
                return;
            }
            client->frame.used = 0;
            client->last_request = esp_timer_get_time();
        }
    }
}

static void server_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (s_listener < 0) {
            s_listener = create_listener();
            if (s_listener < 0) {
                vTaskDelay(pdMS_TO_TICKS(3000));
                continue;
            }
        }
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(s_listener, &readfds);
        int max_fd = s_listener;
        for (unsigned i = 0; i < MAX_CLIENTS; ++i) {
            if (s_clients[i].socket < 0) continue;
            if (esp_timer_get_time() - s_clients[i].last_request > CLIENT_TIMEOUT_US) {
                close_client(&s_clients[i]);
                continue;
            }
            FD_SET(s_clients[i].socket, &readfds);
            if (s_clients[i].socket > max_fd) max_fd = s_clients[i].socket;
        }
        struct timeval timeout = {.tv_sec = 1};
        const int ready = select(max_fd + 1, &readfds, NULL, NULL, &timeout);
        if (ready < 0) {
            if (errno == EINTR) continue;
            ESP_LOGW(TAG, "Socket error %d; recreating listener", errno);
            for (unsigned i = 0; i < MAX_CLIENTS; ++i)
                if (s_clients[i].socket >= 0) close_client(&s_clients[i]);
            close(s_listener);
            s_listener = -1;
            continue;
        }
        if (!ready) continue;
        // Process existing clients first: a newly accepted fd was not in this select set.
        for (unsigned i = 0; i < MAX_CLIENTS; ++i)
            if (s_clients[i].socket >= 0 && FD_ISSET(s_clients[i].socket, &readfds))
                read_client(&s_clients[i]);
        if (FD_ISSET(s_listener, &readfds)) accept_client();
    }
}

esp_err_t power_card_tcp_server_start(void)
{
    for (unsigned i = 0; i < MAX_CLIENTS; ++i) s_clients[i].socket = -1;
    s_listener = create_listener();
    if (s_listener < 0) return ESP_FAIL;
    if (xTaskCreate(server_task, "tcp_server", 8192, NULL, 4, NULL) != pdPASS) {
        close(s_listener);
        s_listener = -1;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Listening on 0.0.0.0:%d (max %d clients)", CONFIG_POWER_CARD_TCP_PORT, MAX_CLIENTS);
    return ESP_OK;
}
