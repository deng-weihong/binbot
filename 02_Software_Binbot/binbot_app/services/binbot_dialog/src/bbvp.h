#ifndef BINBOT_DIALOG_BBVP_H
#define BINBOT_DIALOG_BBVP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    BBVP_VERSION = 1,
    BBVP_AUDIO_HEADER_SIZE = 40,
    BBVP_MAX_AUDIO_PAYLOAD = 4096,
};

typedef enum {
    BBVP_AUDIO_UPLINK_OPUS = 1,
    BBVP_AUDIO_DOWNLINK_OPUS = 2,
} bbvp_audio_kind_t;

typedef struct {
    bbvp_audio_kind_t kind;
    uint8_t stream_id[16];
    uint32_t sequence;
    uint64_t timestamp_ms;
    const uint8_t *payload;
    uint32_t payload_size;
} bbvp_audio_frame_t;

bool bbvp_audio_encode(uint8_t *output, size_t capacity,
                       const bbvp_audio_frame_t *frame, size_t *output_size);
bool bbvp_audio_decode(const uint8_t *input, size_t input_size,
                       bbvp_audio_frame_t *frame);
bool bbvp_uuid_generate(uint8_t uuid[16]);
void bbvp_uuid_format(const uint8_t uuid[16], char output[37]);
bool bbvp_uuid_parse(const char *text, uint8_t uuid[16]);

#endif
