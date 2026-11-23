#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    OTA_PROGRESS_CHECKING = 0,
    OTA_PROGRESS_NO_UPDATE,
    OTA_PROGRESS_DOWNLOADING,
    OTA_PROGRESS_COMPLETE,
    OTA_PROGRESS_FAILED,
} ota_progress_state_t;

typedef struct {
    ota_progress_state_t state;
    uint8_t percent;
    size_t bytes_written;
    size_t total_bytes;
} ota_progress_t;

typedef void (*ota_progress_callback_t)(const ota_progress_t *progress,
                                        void *user_context);

esp_err_t ota_service_set_progress_callback(ota_progress_callback_t callback,
                                             void *user_context);
esp_err_t ota_service_start(void);
esp_err_t ota_service_confirm_running_image(void);
void ota_service_report_progress(ota_progress_state_t state, uint8_t percent,
                                 size_t bytes_written, size_t total_bytes);
