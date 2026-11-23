#include <stdio.h>

#include "esp_app_desc.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "board_io.h"
#include "network.h"
#include "ota_update.h"
#include "tcp_server.h"
#include "sdkconfig.h"

static const char *TAG = "power_card";

static int status_command(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) return 1;
    printf("project=%s version=%s idf=%s uptime_ms=%lld\n",
           esp_app_get_description()->project_name, esp_app_get_description()->version,
           esp_get_idf_version(), (long long)(esp_timer_get_time() / 1000));
    printf("power_gpio=%d power_press_ms=%d reset_gpio=%d reset_press_ms=%d\n",
           CONFIG_POWER_CARD_BUTTON_GPIO, CONFIG_POWER_CARD_BUTTON_PRESS_MS,
           CONFIG_POWER_CARD_RESET_GPIO, CONFIG_POWER_CARD_RESET_PRESS_MS);
    printf("local_button_gpio=%d pw_led_gpio=%d pw_led_level=%d\n",
           CONFIG_POWER_CARD_LOCAL_BUTTON_GPIO, CONFIG_POWER_CARD_LED_SENSE_GPIO,
           power_card_power_led_level());
    puts("pc_power_state=unknown");
    power_card_network_status_t network;
    power_card_network_status(&network);
    printf("wifi_configured=%d wifi_connected=%d ssid=%s ip=%s tcp_port=%d\n",
           network.configured, network.connected, network.ssid, network.ip,
           CONFIG_POWER_CARD_TCP_PORT);
    power_card_ota_status_t ota;
    power_card_ota_status(&ota);
    printf("ota_state=%s received=%lu total=%lu version=%s error=%s\n", ota.state,
           (unsigned long)ota.received, (unsigned long)ota.total, ota.version, ota.error);
    return 0;
}

static int power_command(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) return 1;
    const esp_err_t error = power_card_press_power();
    if (error != ESP_OK) {
        printf("Power button failed: %s\n", esp_err_to_name(error));
        return 1;
    }
    puts("Power button pulse completed");
    return 0;
}

static int reset_command(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) return 1;
    const esp_err_t error = power_card_press_reset();
    if (error != ESP_OK) {
        printf("Reset button failed: %s\n", esp_err_to_name(error));
        return 1;
    }
    puts("Motherboard reset button pulse completed");
    return 0;
}

static int wifi_command(int argc, char **argv)
{
    if (argc != 2 && argc != 3) {
        puts("Usage: wifi <ssid> [password]; saves credentials and restarts ESP32");
        return 1;
    }
    if (power_card_maintenance_begin() != ESP_OK) {
        puts("Busy: a pulse or OTA is in progress");
        return 1;
    }
    const esp_err_t error = power_card_network_save(argv[1], argc == 3 ? argv[2] : "");
    if (error != ESP_OK) {
        power_card_maintenance_end();
        printf("Wi-Fi configuration failed: %s\n", esp_err_to_name(error));
        return 1;
    }
    puts("Wi-Fi credentials saved; restarting ESP32");
    fflush(stdout);
    esp_restart();
    return 0;
}

static int token_command(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) return 1;
    printf("tcp_token=%s\n", power_card_network_token());
    return 0;
}

static int ota_command(int argc, char **argv)
{
    if (argc != 2) {
        puts("Usage: ota http://host/path/power_card.bin");
        return 1;
    }
    const char *error = power_card_ota_start(argv[1]);
    if (error) {
        printf("OTA rejected: %s\n", error);
        return 1;
    }
    puts("OTA accepted; use status to check progress");
    return 0;
}

void app_main(void)
{
    ESP_ERROR_CHECK(power_card_io_init());
    ESP_ERROR_CHECK(power_card_network_init());
    ESP_ERROR_CHECK(power_card_ota_init());
    ESP_ERROR_CHECK(power_card_tcp_server_start());

    const esp_console_cmd_t commands[] = {
        {.command = "status", .help = "Show firmware and button configuration", .func = status_command},
        {.command = "power", .help = "Send one short PWR_SW button press", .func = power_command},
        {.command = "reset", .help = "Send one motherboard RESET_SW button press", .func = reset_command},
        {.command = "wifi", .help = "wifi <ssid> [password]: save Wi-Fi and restart ESP32", .func = wifi_command},
        {.command = "token", .help = "Show the TCP access token over serial", .func = token_command},
        {.command = "ota", .help = "ota <http-url>: download and install firmware", .func = ota_command},
    };
    ESP_ERROR_CHECK(esp_console_register_help_command());
    for (unsigned i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i)
        ESP_ERROR_CHECK(esp_console_cmd_register(&commands[i]));

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    config.prompt = "power_card> ";
    config.max_cmdline_length = 600;
    ESP_ERROR_CHECK(esp_console_new_repl_stdio(&config, &repl));
    ESP_ERROR_CHECK(power_card_ota_confirm_boot());
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    ESP_LOGI(TAG, "Standalone power card ready; no button press on boot");
}
