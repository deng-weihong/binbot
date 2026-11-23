#include "component_uart.h"

#include <string.h>
#include <stdatomic.h>

#include "binbot/component/ota_receiver.h"
#include "binbot/component/file_receiver.h"
#include "board_pins.h"
#include "driver/uart.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mbedtls/md.h"
#include "ota_service.h"
#include "nvs.h"
#if CONFIG_BINBOT_APP_SHOWCASE_TURNTABLE
#include "turntable_motor.h"
#elif CONFIG_BINBOT_APP_AMBIENT_LIGHT
#include "ambient_light.h"
#elif !CONFIG_BINBOT_APP_AMBIENT_LIGHT
#include "storage.h"
#endif

#define COMPONENT_UART UART_NUM_1

#if CONFIG_BINBOT_APP_ROUND_SCREEN
#define COMPONENT_PROFILE BB_COMPONENT_ROUND_SCREEN
#elif CONFIG_BINBOT_APP_HOLOGRAPHIC_SCREEN
#define COMPONENT_PROFILE BB_COMPONENT_HOLOGRAPHIC_SCREEN
#elif CONFIG_BINBOT_APP_AMBIENT_LIGHT
#define COMPONENT_PROFILE BB_COMPONENT_AMBIENT_LIGHT
#else
#define COMPONENT_PROFILE BB_COMPONENT_SHOWCASE_TURNTABLE
#endif

static const char *TAG = "component_uart";
static bool s_started;
static QueueHandle_t s_uart_events;
static atomic_bool s_ready;
static uint64_t s_boot_id;
static bb_component_announcement_t s_announcement;
static int64_t s_restart_at;
static int64_t s_last_byte_at;
static bb_component_receiver_t s_receiver;
#if !CONFIG_BINBOT_APP_SHOWCASE_TURNTABLE && !CONFIG_BINBOT_APP_AMBIENT_LIGHT
static bb_component_file_receiver_t s_file_receiver;
#endif
static bb_component_parser_t s_parser;
static bb_component_frame_t s_request;
static bb_component_frame_t s_reply;
static uint8_t s_wire[BB_COMPONENT_FRAME_BYTES];
static const esp_partition_t *s_target;
static esp_ota_handle_t s_ota;
static bool s_ota_open;
static mbedtls_md_context_t s_sha;

#if CONFIG_BINBOT_APP_ROUND_SCREEN
static portMUX_TYPE s_pen_lock = portMUX_INITIALIZER_UNLOCKED;
static bb_component_pen_t s_pen;
static int64_t s_pen_synced_at;

void component_pen_snapshot(bb_component_pen_t *state)
{
    portENTER_CRITICAL(&s_pen_lock);
    *state = s_pen;
    int64_t at = s_pen_synced_at;
    portEXIT_CRITICAL(&s_pen_lock);
    bb_component_pen_advance(state, (uint32_t)((esp_timer_get_time() - at) / 1000000));
}

static void pen_configure(const bb_component_pen_t *state)
{
    portENTER_CRITICAL(&s_pen_lock);
    bool changed = s_pen.mode != state->mode || s_pen.face != state->face;
    s_pen = *state;
    s_pen_synced_at = esp_timer_get_time();
    portEXIT_CRITICAL(&s_pen_lock);
    if(changed) {
        nvs_handle_t handle;
        if(nvs_open("pen_display", NVS_READWRITE, &handle) == ESP_OK) {
            nvs_set_u8(handle, "mode", state->mode);
            nvs_set_u8(handle, "face", state->face);
            nvs_commit(handle);
            nvs_close(handle);
        }
    }
}

void component_pen_show_image(void)
{
    bb_component_pen_t state;
    component_pen_snapshot(&state);
    state.mode = BB_PEN_IMAGE;
    pen_configure(&state);
}
#endif

#if !CONFIG_BINBOT_APP_SHOWCASE_TURNTABLE && !CONFIG_BINBOT_APP_AMBIENT_LIGHT
static uint16_t storage_status(esp_err_t error)
{
    if (error == ESP_OK) return BB_COMPONENT_OK;
    if (error == ESP_ERR_INVALID_CRC) return BB_COMPONENT_WRONG_IMAGE;
    if (error == ESP_ERR_INVALID_STATE) return BB_COMPONENT_BUSY;
    if (error == ESP_ERR_INVALID_SIZE || error == ESP_ERR_INVALID_ARG)
        return BB_COMPONENT_BAD_REQUEST;
    return BB_COMPONENT_IO;
}

static uint16_t file_begin(void *context, uint8_t file_id, uint32_t total,
                           const uint8_t expected_sha256[32])
{
    (void)context;
    if (!atomic_load(&s_ready)) return BB_COMPONENT_NOT_READY;
    const char *name = bb_component_file_name(file_id);
    if (!name) return BB_COMPONENT_BAD_REQUEST;
    return storage_status(storage_gif_update_begin(name, total, expected_sha256));
}

static uint16_t file_write(void *context, const uint8_t *bytes, size_t length)
{
    (void)context;
    return storage_status(storage_gif_update_write(bytes, length));
}

static uint16_t file_finish(void *context, uint8_t file_id,
                            const uint8_t expected_sha256[32])
{
    (void)context;
    (void)expected_sha256;
    uint16_t width = 0;
    uint16_t height = 0;
#if CONFIG_BINBOT_APP_ROUND_SCREEN
    if (file_id == BB_COMPONENT_FILE_DISPLAY_GIF) {
        width = 240;
        height = 240;
    }
#elif CONFIG_BINBOT_APP_HOLOGRAPHIC_SCREEN
    if (file_id == BB_COMPONENT_FILE_DISPLAY_GIF) {
        width = 320;
        height = 240;
    }
#endif
    if (width == 0 || height == 0) return BB_COMPONENT_BAD_REQUEST;
    return storage_status(storage_gif_update_finish(width, height));
}

static void file_abort(void *context)
{
    (void)context;
    storage_gif_update_abort();
}
#endif

static uint16_t ota_begin(void *context, uint32_t total)
{
    (void)context;
    if (!atomic_load(&s_ready)) return BB_COMPONENT_NOT_READY;
    mbedtls_md_free(&s_sha);
#if CONFIG_BINBOT_APP_SHOWCASE_TURNTABLE
    if (turntable_motor_stop() != ESP_OK) return BB_COMPONENT_IO;
#endif
    mbedtls_md_init(&s_sha);
    if (mbedtls_md_setup(&s_sha, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0) ||
        mbedtls_md_starts(&s_sha)) return BB_COMPONENT_IO;
    esp_err_t error = esp_ota_begin(s_target, total, &s_ota);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(error));
        mbedtls_md_free(&s_sha);
        return BB_COMPONENT_IO;
    }
    s_ota_open = true;
    ota_service_report_progress(OTA_PROGRESS_DOWNLOADING, 0, 0, total);
    return BB_COMPONENT_OK;
}

static uint16_t ota_write(void *context, const uint8_t *bytes, size_t length)
{
    (void)context;
    esp_err_t error = esp_ota_write(s_ota, bytes, length);
    if (error != ESP_OK || mbedtls_md_update(&s_sha, bytes, length)) {
        ESP_LOGE(TAG, "OTA write failed: %s", esp_err_to_name(error));
        return BB_COMPONENT_IO;
    }
    const size_t written = s_receiver.received + length;
    ota_service_report_progress(OTA_PROGRESS_DOWNLOADING,
                                (uint8_t)(written * 100 / s_receiver.total),
                                written, s_receiver.total);
    return BB_COMPONENT_OK;
}

static uint16_t ota_finish(void *context, const uint8_t expected[32])
{
    (void)context;
    uint8_t actual[32];
    if (mbedtls_md_finish(&s_sha, actual)) return BB_COMPONENT_IO;
    mbedtls_md_free(&s_sha);
    if (memcmp(actual, expected, sizeof(actual))) return BB_COMPONENT_HASH;
    const esp_err_t error = esp_ota_end(s_ota);
    s_ota_open = false;
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "OTA image validation failed: %s", esp_err_to_name(error));
        return BB_COMPONENT_WRONG_IMAGE;
    }
    esp_app_desc_t description;
    if (esp_ota_get_partition_description(s_target, &description) != ESP_OK ||
        strncmp(description.project_name,
                bb_component_project(COMPONENT_PROFILE),
                sizeof(description.project_name)) != 0)
        return BB_COMPONENT_WRONG_IMAGE;
    ota_service_report_progress(OTA_PROGRESS_COMPLETE, 100,
                                s_receiver.total, s_receiver.total);
    return BB_COMPONENT_OK;
}

static void ota_abort(void *context)
{
    (void)context;
    if (s_ota_open) (void)esp_ota_abort(s_ota);
    s_ota_open = false;
    mbedtls_md_free(&s_sha);
    ota_service_report_progress(OTA_PROGRESS_FAILED, 0,
                                s_receiver.received, s_receiver.total);
}

static uint16_t ota_activate(void *context)
{
    (void)context;
    const esp_err_t error = esp_ota_set_boot_partition(s_target);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "OTA activation failed: %s", esp_err_to_name(error));
        return BB_COMPONENT_IO;
    }
    /* Give the ACK time to leave the UART before resetting. */
    s_restart_at = esp_timer_get_time() + 250000;
    return BB_COMPONENT_OK;
}

static void fill_info(uint8_t *body, bool healthy)
{
    const esp_app_desc_t *description = esp_app_get_description();
    memset(body, 0, BB_COMPONENT_INFO_BYTES);
    body[0] = COMPONENT_PROFILE;
    body[1] = s_receiver.state;
    body[2] = healthy ? 1 : 0;
#if CONFIG_BINBOT_APP_SHOWCASE_TURNTABLE
    body[3] = BB_COMPONENT_CAP_MOTOR;
#elif CONFIG_BINBOT_APP_AMBIENT_LIGHT
    body[3] = BB_COMPONENT_CAP_LIGHT;
#elif CONFIG_BINBOT_APP_ROUND_SCREEN
    body[3] = BB_COMPONENT_CAP_PEN;
#endif
    bb_component_put_u64(body + 4, s_boot_id);
    bb_component_put_u32(body + 12, s_receiver.capacity);
    bb_component_put_u32(body + 16, s_receiver.received);
    bb_component_put_u32(body + 20, s_receiver.total);
    bb_component_put_u64(body + 24, s_receiver.transfer_id);
    memcpy(body + 32, description->version, 32);
    memcpy(body + 64, description->project_name, 32);
    memcpy(body + 96, description->app_elf_sha256, 32);
}

static void announce(int64_t now_ms)
{
    const bool healthy = atomic_load(&s_ready);
    uint16_t sequence;
    if (!bb_component_announcement_due(&s_announcement, healthy, now_ms, &sequence)) return;
    s_reply.type = BB_COMPONENT_EVENT;
    s_reply.command = BB_COMPONENT_ANNOUNCE;
    s_reply.sequence = sequence;
    s_reply.length = BB_COMPONENT_ANNOUNCE_BYTES;
    fill_info(s_reply.payload, healthy);
    memset(s_reply.payload + BB_COMPONENT_INFO_BYTES, 0, BB_COMPONENT_MOTOR_BYTES);
#if CONFIG_BINBOT_APP_SHOWCASE_TURNTABLE
    if (healthy && turntable_motor_get(s_reply.payload + BB_COMPONENT_INFO_BYTES,
                                       s_reply.payload + BB_COMPONENT_INFO_BYTES + 1) != ESP_OK)
        return;
#endif
    const size_t length = bb_component_encode(&s_reply, s_wire, sizeof(s_wire));
    if (uart_write_bytes(COMPONENT_UART, s_wire, length) != (int)length)
        ESP_LOGW(TAG, "UART announcement write failed");
}

static void dispatch(void)
{
    if (bb_component_announcement_ack(&s_announcement, &s_request)) return;
    if (s_request.type != BB_COMPONENT_REQUEST) return;
    s_reply.type = BB_COMPONENT_RESPONSE;
    s_reply.sequence = s_request.sequence;
    s_reply.command = s_request.command;
    size_t body_length = 0;
    uint16_t status = BB_COMPONENT_OK;
    uint8_t *body = s_reply.payload + 2;
    if (s_request.command == BB_COMPONENT_HELLO ||
        s_request.command == BB_COMPONENT_GET_STATUS) {
        if (s_request.length != 0) {
            status = BB_COMPONENT_BAD_REQUEST;
        } else {
            const bool healthy = atomic_load(&s_ready);
            fill_info(body, healthy);
            bb_component_announcement_observed(&s_announcement, healthy);
            body_length = BB_COMPONENT_INFO_BYTES;
        }
    } else if(s_request.command == BB_COMPONENT_PEN_GET || s_request.command == BB_COMPONENT_PEN_SET) {
#if CONFIG_BINBOT_APP_ROUND_SCREEN
        bool setting = s_request.command == BB_COMPONENT_PEN_SET;
        bb_component_pen_t state;
        if(s_request.length != (setting ? BB_COMPONENT_PEN_BYTES : 0) ||
           (setting && !bb_component_pen_decode(s_request.payload, s_request.length, &state)))
            status = BB_COMPONENT_BAD_REQUEST;
        else if(!atomic_load(&s_ready)) status = BB_COMPONENT_NOT_READY;
        else if(setting && (s_restart_at || s_receiver.state != BB_COMPONENT_IDLE)) status = BB_COMPONENT_BUSY;
        else {
            if(setting) pen_configure(&state);
            component_pen_snapshot(&state);
            bb_component_pen_encode(&state, body);
            body_length = BB_COMPONENT_PEN_BYTES;
        }
#else
        status = BB_COMPONENT_UNSUPPORTED;
#endif
    } else if (s_request.command == BB_COMPONENT_MOTOR_GET ||
               s_request.command == BB_COMPONENT_MOTOR_SET) {
#if CONFIG_BINBOT_APP_SHOWCASE_TURNTABLE
        const bool setting = s_request.command == BB_COMPONENT_MOTOR_SET;
        if (s_request.length != (setting ? BB_COMPONENT_MOTOR_BYTES : 0) ||
            (setting && (s_request.payload[0] > BB_COMPONENT_MOTOR_CW ||
                         s_request.payload[1] > 100))) {
            status = BB_COMPONENT_BAD_REQUEST;
        } else if (!atomic_load(&s_ready)) {
            status = BB_COMPONENT_NOT_READY;
        } else if (setting && (s_restart_at || s_receiver.state != BB_COMPONENT_IDLE)) {
            status = BB_COMPONENT_BUSY;
        } else {
            esp_err_t error = setting ? turntable_motor_set(s_request.payload[0], s_request.payload[1]) : ESP_OK;
            if (error == ESP_OK) error = turntable_motor_get(body, body + 1);
            status = error == ESP_OK ? BB_COMPONENT_OK : BB_COMPONENT_IO;
            if (status == BB_COMPONENT_OK) body_length = BB_COMPONENT_MOTOR_BYTES;
        }
#else
        status = BB_COMPONENT_UNSUPPORTED;
#endif
    } else if (s_request.command == BB_COMPONENT_LIGHT_GET || s_request.command == BB_COMPONENT_LIGHT_SET) {
#if CONFIG_BINBOT_APP_AMBIENT_LIGHT
        const bool setting = s_request.command == BB_COMPONENT_LIGHT_SET;
        bb_component_light_settings_t settings;
        uint16_t remaining;
        if(s_request.length != (setting ? BB_COMPONENT_LIGHT_BYTES : 0) ||
           (setting && !bb_component_light_decode(s_request.payload, s_request.length, &settings))) {
            status = BB_COMPONENT_BAD_REQUEST;
        } else if(!atomic_load(&s_ready)) {
            status = BB_COMPONENT_NOT_READY;
        } else if(setting && (s_restart_at || s_receiver.state != BB_COMPONENT_IDLE)) {
            status = BB_COMPONENT_BUSY;
        } else {
            esp_err_t error = setting ? ambient_light_configure(&settings) : ESP_OK;
            if(error == ESP_OK) error = ambient_light_status(&settings, &remaining);
            status = error == ESP_OK ? BB_COMPONENT_OK : BB_COMPONENT_IO;
            if(status == BB_COMPONENT_OK) {
                bb_component_light_encode(&settings, body);
                bb_component_put_u16(body + BB_COMPONENT_LIGHT_BYTES, remaining);
                body_length = BB_COMPONENT_LIGHT_STATE_BYTES;
            }
        }
#else
        status = BB_COMPONENT_UNSUPPORTED;
#endif
    } else if (s_request.command >= BB_COMPONENT_FILE_BEGIN &&
               s_request.command <= BB_COMPONENT_FILE_ABORT) {
#if CONFIG_BINBOT_APP_SHOWCASE_TURNTABLE || CONFIG_BINBOT_APP_AMBIENT_LIGHT
        status = BB_COMPONENT_UNSUPPORTED;
#else
        status = bb_component_file_receiver_command(&s_file_receiver,
                                                     s_request.command,
                                                     s_request.payload,
                                                     s_request.length, body,
                                                     &body_length);
#endif
    } else {
        status = bb_component_receiver_command(&s_receiver, s_request.command,
                                               s_request.payload,
                                               s_request.length, body,
                                               &body_length);
    }
    bb_component_put_u16(s_reply.payload, status);
    s_reply.length = (uint16_t)(2 + body_length);
    const size_t length = bb_component_encode(&s_reply, s_wire, sizeof(s_wire));
    if (uart_write_bytes(COMPONENT_UART, s_wire, length) != (int)length)
        ESP_LOGW(TAG, "UART reply write failed");
}

static void uart_task(void *arg)
{
    (void)arg;
    uint8_t bytes[256];
    while (true) {
        uart_event_t event;
        if (xQueueReceive(s_uart_events, &event, pdMS_TO_TICKS(50)) == pdTRUE &&
            (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL ||
             event.type == UART_FRAME_ERR || event.type == UART_PARITY_ERR)) {
            (void)uart_flush_input(COMPONENT_UART);
            (void)xQueueReset(s_uart_events);
            s_parser.length = 0;
        }
        /* Read only buffered bytes so a short frame tail is dispatched without
         * waiting for a fixed-size uart_read_bytes() call to time out. */
        while (true) {
            size_t available = 0;
            if (uart_get_buffered_data_len(COMPONENT_UART, &available) != ESP_OK ||
                !available) break;
            const uint32_t count = available < sizeof(bytes) ? available : sizeof(bytes);
            const int received = uart_read_bytes(COMPONENT_UART, bytes, count, 0);
            if (received <= 0) break;
            const int64_t now = esp_timer_get_time();
            if (now - s_last_byte_at > 200000) s_parser.length = 0;
            s_last_byte_at = now;
            for (int i = 0; i < received; ++i) {
                if (bb_component_parser_push(&s_parser, bytes[i], &s_request)) {
                    dispatch();
                    while (bb_component_parser_next(&s_parser, &s_request))
                        dispatch();
                }
            }
        }
        const int64_t now = esp_timer_get_time();
        announce(now / 1000);
        if (now - s_last_byte_at > 200000) s_parser.length = 0;
        if (s_restart_at && now >= s_restart_at) {
            (void)uart_wait_tx_done(COMPONENT_UART, pdMS_TO_TICKS(100));
            esp_restart();
        }
    }
}

esp_err_t component_uart_start(void)
{
#if !CONFIG_BINBOT_UART_OTA_ENABLE
    return ESP_OK;
#else
    if (s_started) return ESP_OK;
#if CONFIG_BINBOT_APP_ROUND_SCREEN
    nvs_handle_t pen_handle;
    if(nvs_open("pen_display", NVS_READONLY, &pen_handle) == ESP_OK) {
        uint8_t mode = 0, face = 0;
        nvs_get_u8(pen_handle, "mode", &mode); nvs_get_u8(pen_handle, "face", &face);
        nvs_close(pen_handle);
        if(mode <= BB_PEN_INFO && face <= BB_PEN_WEATHER) { s_pen.mode = mode; s_pen.face = face; }
    }
#endif
    board_uart_pins_t pins;
    esp_err_t error = board_get_uart1_pins(&pins);
    if (error != ESP_OK) return error;
    const uart_config_t config = {
        .baud_rate = BB_COMPONENT_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT
    };
    error = uart_param_config(COMPONENT_UART, &config);
    if (error != ESP_OK) return error;
    error = uart_set_pin(COMPONENT_UART, pins.tx, pins.rx,
                         UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (error != ESP_OK) return error;
    error = uart_driver_install(COMPONENT_UART, 4096, 2048, 16, &s_uart_events, 0);
    if (error != ESP_OK) return error;
    s_target = esp_ota_get_next_update_partition(NULL);
    const bb_component_ota_ops_t ops = {
        ota_begin, ota_write, ota_finish, ota_abort, ota_activate
    };
    bb_component_receiver_init(&s_receiver, COMPONENT_PROFILE,
                               s_target ? s_target->size : 0, &ops, NULL);
#if !CONFIG_BINBOT_APP_SHOWCASE_TURNTABLE && !CONFIG_BINBOT_APP_AMBIENT_LIGHT
    const bb_component_file_ops_t file_ops = {
        file_begin, file_write, file_finish, file_abort
    };
    bb_component_file_receiver_init(&s_file_receiver, &file_ops, NULL);
#endif
    esp_fill_random(&s_boot_id, sizeof(s_boot_id));
    if (!s_boot_id) s_boot_id = 1;
    bb_component_announcement_init(&s_announcement, s_boot_id);
    if (xTaskCreate(uart_task, "component_uart", 6144, NULL, 6, NULL) != pdPASS) {
        (void)uart_driver_delete(COMPONENT_UART);
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    ESP_LOGI(TAG, "UART OTA: TX=%d RX=%d baud=%d slot=%lu", pins.tx, pins.rx,
             BB_COMPONENT_UART_BAUD, (unsigned long)s_receiver.capacity);
    return ESP_OK;
#endif
}

esp_err_t component_uart_confirm_ready(void)
{
#if CONFIG_BINBOT_UART_OTA_ENABLE
    if (!s_started) return ESP_ERR_INVALID_STATE;
#endif
    const esp_err_t error = ota_service_confirm_running_image();
    if (error == ESP_OK) atomic_store(&s_ready, true);
    return error;
}

static void boot_watchdog_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(30000));
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGE(TAG, "New firmware did not become healthy; rebooting for rollback");
        esp_restart();
    }
    vTaskDelete(NULL);
}

esp_err_t component_uart_start_boot_watchdog(void)
{
    return xTaskCreate(boot_watchdog_task, "ota_boot_check", 2048, NULL, 7, NULL) == pdPASS
               ? ESP_OK : ESP_ERR_NO_MEM;
}
