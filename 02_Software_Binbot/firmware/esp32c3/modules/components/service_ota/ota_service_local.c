#include "ota_service.h"

#include "esp_ota_ops.h"

static ota_progress_callback_t s_callback;
static void *s_context;

esp_err_t ota_service_set_progress_callback(ota_progress_callback_t callback,
                                           void *context)
{
    s_callback = callback;
    s_context = context;
    return ESP_OK;
}

void ota_service_report_progress(ota_progress_state_t state, uint8_t percent,
                                 size_t written, size_t total)
{
    if (s_callback) {
        const ota_progress_t progress = {state, percent, written, total};
        s_callback(&progress, s_context);
    }
}

esp_err_t ota_service_start(void)
{
    return ESP_OK;
}

esp_err_t ota_service_confirm_running_image(void)
{
    esp_ota_img_states_t state;
    esp_err_t error = esp_ota_get_state_partition(esp_ota_get_running_partition(), &state);
    if (error == ESP_ERR_NOT_FOUND) return ESP_OK;
    if (error != ESP_OK) return error;
    if (state == ESP_OTA_IMG_PENDING_VERIFY)
        return esp_ota_mark_app_valid_cancel_rollback();
    return ESP_OK;
}
