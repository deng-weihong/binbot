#include "ambient_light.h"

#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "led_strip_encoder.h"
#include "sdkconfig.h"

static rmt_channel_handle_t s_channel;
static rmt_encoder_handle_t s_encoder;
static SemaphoreHandle_t s_lock;
static uint8_t s_pixels[CONFIG_BINBOT_LIGHT_LED_COUNT * 3];
static uint8_t s_rgb[CONFIG_BINBOT_LIGHT_LED_COUNT * 3];
static bb_component_light_t s_light;

static esp_err_t render_locked(void)
{
    bb_component_light_render(&s_light, esp_timer_get_time() / 1000,
                               s_rgb, CONFIG_BINBOT_LIGHT_LED_COUNT);
    for(unsigned i = 0; i < CONFIG_BINBOT_LIGHT_LED_COUNT; ++i) {
        s_pixels[i * 3] = s_rgb[i * 3 + 1];
        s_pixels[i * 3 + 1] = s_rgb[i * 3];
        s_pixels[i * 3 + 2] = s_rgb[i * 3 + 2];
    }
    const rmt_transmit_config_t transmit = {.loop_count = 0, .flags.eot_level = 0};
    esp_err_t error = rmt_transmit(s_channel, s_encoder, s_pixels, sizeof(s_pixels), &transmit);
    return error == ESP_OK ? rmt_tx_wait_all_done(s_channel, 1000) : error;
}

static void light_task(void *unused)
{
    (void)unused;
    for(;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        esp_err_t error = render_locked();
        xSemaphoreGive(s_lock);
        if(error != ESP_OK) ESP_LOGW("ambient_light", "LED refresh failed: %s", esp_err_to_name(error));
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}

esp_err_t ambient_light_init(void)
{
    if (s_lock) return ESP_OK;
    const gpio_config_t gpio = {
        .pin_bit_mask = 1ULL << CONFIG_BINBOT_LIGHT_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    esp_err_t error = gpio_config(&gpio);
    if (error != ESP_OK) return error;
    error = gpio_set_level(CONFIG_BINBOT_LIGHT_GPIO, 0);
    if (error != ESP_OK) return error;
    const rmt_tx_channel_config_t channel = {
        .gpio_num = CONFIG_BINBOT_LIGHT_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10000000,
        .mem_block_symbols = 48,
        .trans_queue_depth = 1,
    };
    error = rmt_new_tx_channel(&channel, &s_channel);
    if (error != ESP_OK) return error;
    const led_strip_encoder_config_t encoder = {.resolution = 10000000};
    error = rmt_new_led_strip_encoder(&encoder, &s_encoder);
    if (error == ESP_OK) error = rmt_enable(s_channel);
    if (error != ESP_OK) {
        if (s_encoder) rmt_del_encoder(s_encoder);
        rmt_del_channel(s_channel);
        s_encoder = NULL;
        s_channel = NULL;
        return error;
    }
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        rmt_disable(s_channel);
        rmt_del_encoder(s_encoder);
        rmt_del_channel(s_channel);
        s_encoder = NULL;
        s_channel = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_light.settings.speed = 50;
    error = ambient_light_set(0, 0, 0, 0);
    if(error != ESP_OK) return error;
    return xTaskCreate(light_task, "ambient_light", 3072, NULL, 4, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t ambient_light_set(uint8_t red, uint8_t green, uint8_t blue, uint8_t brightness)
{
    const bb_component_light_settings_t settings = {brightness != 0, BB_COMPONENT_LIGHT_SOLID,
        red, green, blue, brightness, 50, 0};
    return ambient_light_configure(&settings);
}

esp_err_t ambient_light_get(uint8_t *red, uint8_t *green, uint8_t *blue, uint8_t *brightness)
{
    if (!red || !green || !blue || !brightness) return ESP_ERR_INVALID_ARG;
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *red = s_light.settings.red; *green = s_light.settings.green;
    *blue = s_light.settings.blue; *brightness = s_light.settings.brightness;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t ambient_light_configure(const bb_component_light_settings_t *settings)
{
    if(!bb_component_light_valid(settings)) return ESP_ERR_INVALID_ARG;
    if(!s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bb_component_light_t before = s_light;
    bb_component_light_set(&s_light, settings, esp_timer_get_time() / 1000);
    esp_err_t error = render_locked();
    if(error != ESP_OK) s_light = before;
    xSemaphoreGive(s_lock);
    return error;
}

esp_err_t ambient_light_status(bb_component_light_settings_t *settings, uint16_t *remaining)
{
    if(!settings || !remaining) return ESP_ERR_INVALID_ARG;
    if(!s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *settings = s_light.settings;
    *remaining = bb_component_light_remaining(&s_light, esp_timer_get_time() / 1000);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}
