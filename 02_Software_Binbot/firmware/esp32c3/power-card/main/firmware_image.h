#pragma once

#include <stddef.h>
#include "esp_app_desc.h"
#include "esp_app_format.h"

#define POWER_CARD_IMAGE_PREFIX_SIZE (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))

/* Validate the complete prefix before erasing an OTA partition. */
const char *power_card_image_check(const void *data, size_t length, esp_app_desc_t *description);
