#include "network.h"

#include <stdio.h>
#include <string.h>

#include "bootloader_random.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "network";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static power_card_network_status_t s_status;
static char s_token[POWER_CARD_TOKEN_LENGTH + 1];
static esp_timer_handle_t s_reconnect_timer;

void power_card_network_status(power_card_network_status_t *status)
{
    portENTER_CRITICAL(&s_lock);
    *status = s_status;
    portEXIT_CRITICAL(&s_lock);
}

const char *power_card_network_token(void)
{
    return s_token;
}

esp_err_t power_card_network_save(const char *ssid, const char *password)
{
    if (!ssid || !password || !ssid[0] || strlen(ssid) > 32 ||
        strlen(password) > 63 || (password[0] && strlen(password) < 8))
        return ESP_ERR_INVALID_ARG;
    nvs_handle_t nvs;
    esp_err_t error = nvs_open("power_card", NVS_READWRITE, &nvs);
    if (error != ESP_OK) return error;
    // One blob keeps SSID and password consistent if power is lost during saving.
    wifi_config_t config = {0};
    memcpy(config.sta.ssid, ssid, strlen(ssid));
    memcpy(config.sta.password, password, strlen(password));
    error = nvs_set_blob(nvs, "wifi", &config.sta, sizeof(config.sta));
    if (error == ESP_OK) error = nvs_commit(nvs);
    nvs_close(nvs);
    return error;
}

static void reconnect(void *arg)
{
    (void)arg;
    esp_err_t error = esp_wifi_connect();
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi connect: %s; retrying", esp_err_to_name(error));
        esp_timer_start_once(s_reconnect_timer, 3000000);
    }
}

static void network_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_timer_start_once(s_reconnect_timer, 1000);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        portENTER_CRITICAL(&s_lock);
        s_status.connected = false;
        s_status.ip[0] = '\0';
        portEXIT_CRITICAL(&s_lock);
        const wifi_event_sta_disconnected_t *event = data;
        ESP_LOGW(TAG, "Wi-Fi disconnected (reason=%u); retrying in 3 seconds", event->reason);
        esp_timer_stop(s_reconnect_timer);
        esp_timer_start_once(s_reconnect_timer, 3000000);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        portENTER_CRITICAL(&s_lock);
        s_status.connected = false;
        s_status.ip[0] = '\0';
        portEXIT_CRITICAL(&s_lock);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = data;
        char ip[16] = {0};
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&event->ip_info.ip));
        esp_timer_stop(s_reconnect_timer);
        portENTER_CRITICAL(&s_lock);
        s_status.connected = true;
        memcpy(s_status.ip, ip, sizeof(ip));
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "Connected: %s; TCP port %d", ip, CONFIG_POWER_CARD_TCP_PORT);
    }
}

esp_err_t power_card_network_init(void)
{
    esp_err_t error = nvs_flash_init();
    // Never erase credentials silently when NVS needs recovery.
    if (error != ESP_OK) return error;
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    if (!esp_netif_create_default_wifi_sta()) return ESP_ERR_NO_MEM;
    wifi_config_t config = {0};
    if (strlen(CONFIG_POWER_CARD_WIFI_SSID) > sizeof(config.sta.ssid) ||
        strlen(CONFIG_POWER_CARD_WIFI_PASSWORD) > 63) return ESP_ERR_INVALID_ARG;
    memcpy(config.sta.ssid, CONFIG_POWER_CARD_WIFI_SSID, strlen(CONFIG_POWER_CARD_WIFI_SSID));
    memcpy(config.sta.password, CONFIG_POWER_CARD_WIFI_PASSWORD, strlen(CONFIG_POWER_CARD_WIFI_PASSWORD));

    nvs_handle_t nvs;
    error = nvs_open("power_card", NVS_READWRITE, &nvs);
    if (error != ESP_OK) return error;
    size_t length = sizeof(config.sta);
    error = nvs_get_blob(nvs, "wifi", &config.sta, &length);
    if (error != ESP_OK && error != ESP_ERR_NVS_NOT_FOUND) goto close_nvs;
    if (error == ESP_OK && length != sizeof(config.sta)) {
        error = ESP_ERR_INVALID_SIZE;
        goto close_nvs;
    }
    length = sizeof(s_token);
    error = nvs_get_str(nvs, "token", s_token, &length);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        uint8_t random[POWER_CARD_TOKEN_LENGTH / 2];
        bootloader_random_enable();
        esp_fill_random(random, sizeof(random));
        bootloader_random_disable();
        for (unsigned i = 0; i < sizeof(random); ++i)
            snprintf(s_token + i * 2, 3, "%02x", random[i]);
        error = nvs_set_str(nvs, "token", s_token);
        if (error == ESP_OK) error = nvs_commit(nvs);
    } else if (error == ESP_OK && strlen(s_token) != POWER_CARD_TOKEN_LENGTH) {
        error = ESP_ERR_INVALID_SIZE;
    }
close_nvs:
    nvs_close(nvs);
    if (error != ESP_OK) return error;

    const wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    const esp_timer_create_args_t timer = {.callback = reconnect, .name = "wifi_retry"};
    ESP_ERROR_CHECK(esp_timer_create(&timer, &s_reconnect_timer));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, network_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, network_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, network_event, NULL));
    portENTER_CRITICAL(&s_lock);
    memcpy(s_status.ssid, config.sta.ssid, sizeof(config.sta.ssid));
    s_status.configured = s_status.ssid[0] != '\0';
    portEXIT_CRITICAL(&s_lock);
    if (!s_status.configured) {
        ESP_LOGW(TAG, "Wi-Fi is not configured; use the serial wifi command");
        return ESP_OK;
    }
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    ESP_ERROR_CHECK(esp_wifi_start());
    return esp_wifi_set_ps(WIFI_PS_NONE);
}
