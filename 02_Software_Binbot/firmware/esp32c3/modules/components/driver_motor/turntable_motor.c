#include "turntable_motor.h"

#include "binbot/component/protocol.h"
#include "board_pins.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define MOTOR_PWM_HZ 20000
#define MOTOR_PWM_MAX 1023

static const char *TAG = "driver_motor";
static SemaphoreHandle_t s_lock;
static uint8_t s_direction;
static uint8_t s_speed = 40;

static esp_err_t set_duty(ledc_channel_t channel, uint32_t duty)
{
    esp_err_t error = ledc_set_duty(LEDC_LOW_SPEED_MODE, channel, duty);
    return error == ESP_OK ? ledc_update_duty(LEDC_LOW_SPEED_MODE, channel) : error;
}

esp_err_t turntable_motor_init(void)
{
    board_turntable_pins_t pins;
    esp_err_t error = board_get_turntable_pins(&pins);
    if (error != ESP_OK) return error;
    const gpio_config_t config = {
        .pin_bit_mask = (1ULL << pins.in1) | (1ULL << pins.in2),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    error = gpio_config(&config);
    if (error != ESP_OK) return error;
    gpio_set_level(pins.in1, 0);
    gpio_set_level(pins.in2, 0);
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .freq_hz = MOTOR_PWM_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    error = ledc_timer_config(&timer);
    if (error != ESP_OK) return error;
    const int outputs[] = {pins.in1, pins.in2};
    for (unsigned i = 0; i < 2; ++i) {
        const ledc_channel_config_t channel = {
            .gpio_num = outputs[i],
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = i == 0 ? LEDC_CHANNEL_0 : LEDC_CHANNEL_1,
            .timer_sel = LEDC_TIMER_0,
            .intr_type = LEDC_INTR_DISABLE,
            .duty = 0,
        };
        error = ledc_channel_config(&channel);
        if (error != ESP_OK) return error;
    }
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "PWM initialized IN1=%d IN2=%d frequency=%d Hz; stopped",
             pins.in1, pins.in2, MOTOR_PWM_HZ);
    return ESP_OK;
}

esp_err_t turntable_motor_set(uint8_t direction, uint8_t speed)
{
    if (direction > BB_COMPONENT_MOTOR_CW || speed > 100) return ESP_ERR_INVALID_ARG;
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    if (!speed) direction = BB_COMPONENT_MOTOR_STOP;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t error = ESP_OK;
    if (direction != s_direction || direction == BB_COMPONENT_MOTOR_STOP) {
        error = set_duty(LEDC_CHANNEL_0, 0);
        esp_err_t other = set_duty(LEDC_CHANNEL_1, 0);
        if (error == ESP_OK) error = other;
        /* Let the motor coast before energizing the opposite input. */
        if (error == ESP_OK && s_direction && direction && s_direction != direction)
            vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (error == ESP_OK && direction) {
        error = set_duty(direction == BB_COMPONENT_MOTOR_CCW ? LEDC_CHANNEL_0 : LEDC_CHANNEL_1,
                         (uint32_t)speed * MOTOR_PWM_MAX / 100);
    }
    if (error == ESP_OK) {
        s_direction = direction;
        s_speed = speed;
        ESP_LOGI(TAG, "direction=%u speed=%u%%", direction, speed);
    } else {
        (void)set_duty(LEDC_CHANNEL_0, 0);
        (void)set_duty(LEDC_CHANNEL_1, 0);
        s_direction = BB_COMPONENT_MOTOR_STOP;
    }
    xSemaphoreGive(s_lock);
    return error;
}

esp_err_t turntable_motor_get(uint8_t *direction, uint8_t *speed)
{
    if (!direction || !speed) return ESP_ERR_INVALID_ARG;
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *direction = s_direction;
    *speed = s_speed;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t turntable_motor_stop(void) { return turntable_motor_set(BB_COMPONENT_MOTOR_STOP, 40); }
esp_err_t turntable_motor_left(void) { return turntable_motor_set(BB_COMPONENT_MOTOR_CCW, 100); }
esp_err_t turntable_motor_right(void) { return turntable_motor_set(BB_COMPONENT_MOTOR_CW, 100); }
