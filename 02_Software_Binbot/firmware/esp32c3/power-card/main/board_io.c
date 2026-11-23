#include "board_io.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "soc/uart_pins.h"

static const char *TAG = "board_io";
static bool s_initialized;
static SemaphoreHandle_t s_output_lock;

#ifdef CONFIG_POWER_CARD_BUTTON_ACTIVE_HIGH
static const int POWER_ACTIVE = 1;
#else
static const int POWER_ACTIVE = 0;
#endif
#ifdef CONFIG_POWER_CARD_RESET_ACTIVE_HIGH
static const int RESET_ACTIVE = 1;
#else
static const int RESET_ACTIVE = 0;
#endif

static esp_err_t validate_pins(void)
{
    const struct { const char *name; int pin; bool output; } pins[] = {
        {"PW", CONFIG_POWER_CARD_BUTTON_GPIO, true},
        {"RST", CONFIG_POWER_CARD_RESET_GPIO, true},
        {"SW2", CONFIG_POWER_CARD_LOCAL_BUTTON_GPIO, false},
        {"PW_LED", CONFIG_POWER_CARD_LED_SENSE_GPIO, false},
    };
    for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        const int pin = pins[i].pin;
        if (pin < 0) continue;
        bool reserved = !GPIO_IS_VALID_GPIO(pin) || (pin >= 12 && pin <= 17) ||
                        (pins[i].output && !GPIO_IS_VALID_OUTPUT_GPIO(pin));
#if CONFIG_ESP_CONSOLE_UART
        int console_tx = U0TXD_GPIO_NUM;
        int console_rx = U0RXD_GPIO_NUM;
#if CONFIG_ESP_CONSOLE_UART_CUSTOM
        if (CONFIG_ESP_CONSOLE_UART_TX_GPIO >= 0) console_tx = CONFIG_ESP_CONSOLE_UART_TX_GPIO;
        if (CONFIG_ESP_CONSOLE_UART_RX_GPIO >= 0) console_rx = CONFIG_ESP_CONSOLE_UART_RX_GPIO;
#endif
        reserved |= pin == console_tx || pin == console_rx;
#endif
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG || CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG
        reserved |= pin == 18 || pin == 19;
#endif
        if (reserved) {
            ESP_LOGE(TAG, "%s: GPIO%d is invalid or reserved by flash/console", pins[i].name, pin);
            return ESP_ERR_INVALID_ARG;
        }
        for (unsigned j = 0; j < i; ++j) {
            if (pin == pins[j].pin) {
                ESP_LOGE(TAG, "%s and %s both use GPIO%d", pins[i].name, pins[j].name, pin);
                return ESP_ERR_INVALID_ARG;
            }
        }
    }
    return ESP_OK;
}

static esp_err_t output_init(int pin, int active)
{
    if (pin < 0) return ESP_OK;
    // Preload the inactive level before enabling the output driver.
    esp_err_t error = gpio_set_level(pin, !active);
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (error == ESP_OK) error = gpio_config(&config);
    return error;
}

static esp_err_t input_init(int pin, bool pull_up)
{
    if (pin < 0) return ESP_OK;
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = pull_up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&config);
}

static esp_err_t press(int pin, int active, unsigned duration_ms)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (pin < 0) return ESP_ERR_NOT_SUPPORTED;
    if (xSemaphoreTake(s_output_lock, 0) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t error = gpio_set_level(pin, active);
    if (error == ESP_OK) vTaskDelay(pdMS_TO_TICKS(duration_ms));
    const esp_err_t release_error = gpio_set_level(pin, !active);
    xSemaphoreGive(s_output_lock);
    return error == ESP_OK ? release_error : error;
}

esp_err_t power_card_press_power(void)
{
    return press(CONFIG_POWER_CARD_BUTTON_GPIO, POWER_ACTIVE, CONFIG_POWER_CARD_BUTTON_PRESS_MS);
}

esp_err_t power_card_press_reset(void)
{
    return press(CONFIG_POWER_CARD_RESET_GPIO, RESET_ACTIVE, CONFIG_POWER_CARD_RESET_PRESS_MS);
}

int power_card_power_led_level(void)
{
    if (!s_initialized || CONFIG_POWER_CARD_LED_SENSE_GPIO < 0) return -1;
    return gpio_get_level(CONFIG_POWER_CARD_LED_SENSE_GPIO);
}

esp_err_t power_card_maintenance_begin(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    return xSemaphoreTake(s_output_lock, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

void power_card_maintenance_end(void)
{
    xSemaphoreGive(s_output_lock);
}

static void local_button_task(void *arg)
{
    (void)arg;
    bool sample = gpio_get_level(CONFIG_POWER_CARD_LOCAL_BUTTON_GPIO) == 0;
    bool stable = sample;
    bool armed = !stable;
    TickType_t changed_at = xTaskGetTickCount();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10));
        const TickType_t now = xTaskGetTickCount();
        const bool current = gpio_get_level(CONFIG_POWER_CARD_LOCAL_BUTTON_GPIO) == 0;
        if (current != sample) {
            sample = current;
            changed_at = now;
        }
        if (sample == stable || now - changed_at < pdMS_TO_TICKS(50)) continue;
        stable = sample;
        if (!stable) {
            armed = true;
        } else if (armed) {
            armed = false;
            const esp_err_t error = power_card_press_power();
            if (error == ESP_OK) ESP_LOGI(TAG, "SW2: power button pulse completed");
            else ESP_LOGW(TAG, "SW2: power button failed: %s", esp_err_to_name(error));
        }
    }
}

esp_err_t power_card_io_init(void)
{
    if (s_initialized) return ESP_OK;
    esp_err_t error = validate_pins();
    if (error == ESP_OK) error = output_init(CONFIG_POWER_CARD_BUTTON_GPIO, POWER_ACTIVE);
    if (error == ESP_OK) error = output_init(CONFIG_POWER_CARD_RESET_GPIO, RESET_ACTIVE);
    if (error == ESP_OK) error = input_init(CONFIG_POWER_CARD_LOCAL_BUTTON_GPIO, true);
    if (error == ESP_OK) error = input_init(CONFIG_POWER_CARD_LED_SENSE_GPIO, false);
    if (error != ESP_OK) return error;
    // A binary semaphore allows the OTA worker to release a reservation made by a client.
    s_output_lock = xSemaphoreCreateBinary();
    if (!s_output_lock) return ESP_ERR_NO_MEM;
    xSemaphoreGive(s_output_lock);
    s_initialized = true;
    if (CONFIG_POWER_CARD_LOCAL_BUTTON_GPIO >= 0 &&
        xTaskCreate(local_button_task, "local_button", 3072, NULL, 4, NULL) != pdPASS) {
        s_initialized = false;
        vSemaphoreDelete(s_output_lock);
        s_output_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Ready: PW=%d RST=%d SW2=%d PW_LED=%d; outputs inactive",
             CONFIG_POWER_CARD_BUTTON_GPIO, CONFIG_POWER_CARD_RESET_GPIO,
             CONFIG_POWER_CARD_LOCAL_BUTTON_GPIO, CONFIG_POWER_CARD_LED_SENSE_GPIO);
    return ESP_OK;
}
