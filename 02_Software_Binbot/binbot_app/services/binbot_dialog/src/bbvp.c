#include "bbvp.h"

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static void write_u32(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)(value >> 24);
    output[1] = (uint8_t)(value >> 16);
    output[2] = (uint8_t)(value >> 8);
    output[3] = (uint8_t)value;
}

static void write_u64(uint8_t *output, uint64_t value)
{
    write_u32(output, (uint32_t)(value >> 32));
    write_u32(output + 4, (uint32_t)value);
}

static uint32_t read_u32(const uint8_t *input)
{
    return ((uint32_t)input[0] << 24) | ((uint32_t)input[1] << 16) |
           ((uint32_t)input[2] << 8) | (uint32_t)input[3];
}

static uint64_t read_u64(const uint8_t *input)
{
    return ((uint64_t)read_u32(input) << 32) | read_u32(input + 4);
}

bool bbvp_audio_encode(uint8_t *output, size_t capacity,
                       const bbvp_audio_frame_t *frame, size_t *output_size)
{
    if(!output || !frame || !output_size || !frame->payload ||
       (frame->kind != BBVP_AUDIO_UPLINK_OPUS &&
        frame->kind != BBVP_AUDIO_DOWNLINK_OPUS) ||
       frame->payload_size == 0 || frame->payload_size > BBVP_MAX_AUDIO_PAYLOAD ||
       capacity < BBVP_AUDIO_HEADER_SIZE + frame->payload_size) {
        return false;
    }
    memcpy(output, "BBV1", 4);
    output[4] = BBVP_VERSION;
    output[5] = (uint8_t)frame->kind;
    output[6] = 0;
    output[7] = 0;
    memcpy(output + 8, frame->stream_id, 16);
    write_u32(output + 24, frame->sequence);
    write_u64(output + 28, frame->timestamp_ms);
    write_u32(output + 36, frame->payload_size);
    memcpy(output + BBVP_AUDIO_HEADER_SIZE, frame->payload, frame->payload_size);
    *output_size = BBVP_AUDIO_HEADER_SIZE + frame->payload_size;
    return true;
}

bool bbvp_audio_decode(const uint8_t *input, size_t input_size,
                       bbvp_audio_frame_t *frame)
{
    if(!input || !frame || input_size < BBVP_AUDIO_HEADER_SIZE ||
       memcmp(input, "BBV1", 4) != 0 || input[4] != BBVP_VERSION ||
       (input[5] != BBVP_AUDIO_UPLINK_OPUS &&
        input[5] != BBVP_AUDIO_DOWNLINK_OPUS) ||
       input[6] != 0 || input[7] != 0) {
        return false;
    }
    uint32_t payload_size = read_u32(input + 36);
    if(payload_size == 0 || payload_size > BBVP_MAX_AUDIO_PAYLOAD ||
       input_size != BBVP_AUDIO_HEADER_SIZE + payload_size) {
        return false;
    }
    frame->kind = (bbvp_audio_kind_t)input[5];
    memcpy(frame->stream_id, input + 8, 16);
    frame->sequence = read_u32(input + 24);
    frame->timestamp_ms = read_u64(input + 28);
    frame->payload = input + BBVP_AUDIO_HEADER_SIZE;
    frame->payload_size = payload_size;
    return true;
}

bool bbvp_uuid_generate(uint8_t uuid[16])
{
    if(!uuid) return false;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if(fd < 0) return false;
    size_t offset = 0;
    while(offset < 16) {
        ssize_t received = read(fd, uuid + offset, 16 - offset);
        if(received <= 0) {
            close(fd);
            return false;
        }
        offset += (size_t)received;
    }
    close(fd);
    uuid[6] = (uint8_t)((uuid[6] & 0x0fU) | 0x40U);
    uuid[8] = (uint8_t)((uuid[8] & 0x3fU) | 0x80U);
    return true;
}

static int hex_value(char value)
{
    if(value >= '0' && value <= '9') return value - '0';
    if(value >= 'a' && value <= 'f') return value - 'a' + 10;
    if(value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

void bbvp_uuid_format(const uint8_t uuid[16], char output[37])
{
    static const char hex[] = "0123456789abcdef";
    size_t position = 0;
    for(size_t i = 0; i < 16; ++i) {
        if(i == 4 || i == 6 || i == 8 || i == 10) output[position++] = '-';
        output[position++] = hex[uuid[i] >> 4];
        output[position++] = hex[uuid[i] & 0x0fU];
    }
    output[position] = '\0';
}

bool bbvp_uuid_parse(const char *text, uint8_t uuid[16])
{
    if(!text || !uuid || strlen(text) != 36) return false;
    size_t position = 0;
    for(size_t i = 0; i < 16; ++i) {
        if(position == 8 || position == 13 || position == 18 || position == 23) {
            if(text[position++] != '-') return false;
        }
        int high = hex_value(text[position++]);
        int low = hex_value(text[position++]);
        if(high < 0 || low < 0) return false;
        uuid[i] = (uint8_t)((high << 4) | low);
    }
    return text[position] == '\0';
}
