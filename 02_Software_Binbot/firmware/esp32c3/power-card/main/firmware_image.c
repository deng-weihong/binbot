#include "firmware_image.h"

#include <string.h>

const char *power_card_image_check(const void *data, size_t length, esp_app_desc_t *description)
{
    if (length < POWER_CARD_IMAGE_PREFIX_SIZE) return "truncated_header";
    esp_image_header_t header;
    esp_image_segment_header_t segment;
    memcpy(&header, data, sizeof(header));
    memcpy(&segment, (const char *)data + sizeof(header), sizeof(segment));
    memcpy(description, (const char *)data + sizeof(header) + sizeof(segment), sizeof(*description));
    if (header.magic != ESP_IMAGE_HEADER_MAGIC || header.segment_count == 0 ||
        header.segment_count > ESP_IMAGE_MAX_SEGMENTS || segment.data_len < sizeof(*description) ||
        description->magic_word != ESP_APP_DESC_MAGIC_WORD) return "invalid_firmware";
    if (header.chip_id != ESP_CHIP_ID_ESP32C3) return "wrong_chip";
    if (header.hash_appended != 1) return "missing_image_digest";
    if (!memchr(description->project_name, 0, sizeof(description->project_name)) ||
        strcmp(description->project_name, "power_card") != 0) return "wrong_project";
    if (!description->version[0] || !memchr(description->version, 0, sizeof(description->version)))
        return "invalid_version";
    return NULL;
}
