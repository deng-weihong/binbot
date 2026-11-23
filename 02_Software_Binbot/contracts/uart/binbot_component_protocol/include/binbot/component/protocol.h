#ifndef BINBOT_COMPONENT_PROTOCOL_H
#define BINBOT_COMPONENT_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BB_COMPONENT_VERSION 1
#define BB_COMPONENT_MAX_PAYLOAD 1024
#define BB_COMPONENT_FRAME_BYTES (14 + BB_COMPONENT_MAX_PAYLOAD)
#define BB_COMPONENT_CHUNK_BYTES 512
#define BB_COMPONENT_IMAGE_PREFIX_BYTES 288
#define BB_COMPONENT_BEGIN_BYTES 48
#define BB_COMPONENT_INFO_BYTES 128
#define BB_COMPONENT_CAP_MOTOR 0x01
#define BB_COMPONENT_CAP_LIGHT 0x02
#define BB_COMPONENT_CAP_PEN 0x04
#define BB_COMPONENT_MOTOR_BYTES 2
#define BB_COMPONENT_ANNOUNCE_BYTES (BB_COMPONENT_INFO_BYTES + BB_COMPONENT_MOTOR_BYTES)
#define BB_COMPONENT_ANNOUNCE_ACK_BYTES 10
#define BB_COMPONENT_ANNOUNCE_RETRY_MS 250
#define BB_COMPONENT_QUERY_BYTES 52
#define BB_COMPONENT_FILE_BEGIN_BYTES 48
#define BB_COMPONENT_FILE_QUERY_BYTES 52
#define BB_COMPONENT_FILE_MAX_BYTES (512 * 1024)
#define BB_COMPONENT_UART_BAUD 460800

enum bb_component_type {
    BB_COMPONENT_REQUEST = 1, BB_COMPONENT_RESPONSE = 2, BB_COMPONENT_EVENT = 3
};
enum bb_component_command {
    BB_COMPONENT_HELLO = 0x0001,
    BB_COMPONENT_GET_STATUS = 0x0002,
    BB_COMPONENT_ANNOUNCE = 0x0003,
    BB_COMPONENT_TRANSFER_BEGIN = 0x0010,
    BB_COMPONENT_TRANSFER_DATA = 0x0011,
    BB_COMPONENT_TRANSFER_QUERY = 0x0012,
    BB_COMPONENT_TRANSFER_COMMIT = 0x0013,
    BB_COMPONENT_TRANSFER_ABORT = 0x0014,
    BB_COMPONENT_OTA_ACTIVATE = 0x0040,
    BB_COMPONENT_FILE_BEGIN = 0x0050,
    BB_COMPONENT_FILE_DATA = 0x0051,
    BB_COMPONENT_FILE_QUERY = 0x0052,
    BB_COMPONENT_FILE_COMMIT = 0x0053,
    BB_COMPONENT_FILE_ABORT = 0x0054,
    BB_COMPONENT_MOTOR_GET = 0x0060,
    BB_COMPONENT_MOTOR_SET = 0x0061,
    BB_COMPONENT_LIGHT_GET = 0x0070,
    BB_COMPONENT_LIGHT_SET = 0x0071,
    BB_COMPONENT_PEN_GET = 0x0080,
    BB_COMPONENT_PEN_SET = 0x0081
};
enum bb_component_status {
    BB_COMPONENT_OK = 0,
    BB_COMPONENT_BAD_REQUEST = 1,
    BB_COMPONENT_UNSUPPORTED = 2,
    BB_COMPONENT_BUSY = 3,
    BB_COMPONENT_SESSION = 4,
    BB_COMPONENT_OFFSET = 5,
    BB_COMPONENT_TOO_LARGE = 6,
    BB_COMPONENT_WRONG_IMAGE = 7,
    BB_COMPONENT_HASH = 8,
    BB_COMPONENT_IO = 9,
    BB_COMPONENT_NOT_READY = 10
};
enum bb_component_profile {
    BB_COMPONENT_ROUND_SCREEN = 1,
    BB_COMPONENT_HOLOGRAPHIC_SCREEN = 2,
    BB_COMPONENT_SHOWCASE_TURNTABLE = 3,
    /* Profile 4 is reserved for the CH582 slave MCU. */
    BB_COMPONENT_AMBIENT_LIGHT = 5
};
enum bb_component_transfer_state {
    BB_COMPONENT_IDLE = 0,
    BB_COMPONENT_RECEIVING = 1,
    BB_COMPONENT_COMMITTED = 2,
    BB_COMPONENT_ACTIVATED = 3
};

enum bb_component_file_id {
    BB_COMPONENT_FILE_DISPLAY_GIF = 1
};

enum bb_component_file_type {
    BB_COMPONENT_FILE_TYPE_GIF = 1
};

enum bb_component_motor_direction {
    BB_COMPONENT_MOTOR_STOP = 0,
    BB_COMPONENT_MOTOR_CCW = 1,
    BB_COMPONENT_MOTOR_CW = 2
};

typedef struct {
    uint8_t type;
    uint16_t sequence;
    uint16_t command;
    uint16_t length;
    uint8_t payload[BB_COMPONENT_MAX_PAYLOAD];
} bb_component_frame_t;

typedef struct {
    size_t length;
    uint8_t bytes[BB_COMPONENT_FRAME_BYTES];
} bb_component_parser_t;

typedef struct {
    uint8_t profile;
    char version[32];
    char project[32];
    uint8_t elf_sha256[32];
} bb_component_image_t;

typedef struct {
    uint64_t boot_id;
    uint16_t sequence;
    bool state_known, healthy, confirmed;
    int64_t next_send_ms;
} bb_component_announcement_t;

void bb_component_announcement_init(bb_component_announcement_t *state, uint64_t boot_id);
bool bb_component_announcement_due(bb_component_announcement_t *state, bool healthy,
                                    int64_t now_ms, uint16_t *sequence);
bool bb_component_announcement_ack(bb_component_announcement_t *state,
                                    const bb_component_frame_t *frame);
void bb_component_announcement_observed(bb_component_announcement_t *state, bool healthy);

uint16_t bb_component_get_u16(const uint8_t *bytes);
uint32_t bb_component_get_u32(const uint8_t *bytes);
uint64_t bb_component_get_u64(const uint8_t *bytes);
void bb_component_put_u16(uint8_t *bytes, uint16_t value);
void bb_component_put_u32(uint8_t *bytes, uint32_t value);
void bb_component_put_u64(uint8_t *bytes, uint64_t value);
uint32_t bb_component_crc32(const uint8_t *bytes, size_t length);
size_t bb_component_encode(const bb_component_frame_t *frame, uint8_t *bytes,
                           size_t capacity);
bool bb_component_parser_push(bb_component_parser_t *parser, uint8_t byte,
                              bb_component_frame_t *frame);
/* Drain a complete frame already buffered after CRC/length resynchronization. */
bool bb_component_parser_next(bb_component_parser_t *parser,
                              bb_component_frame_t *frame);
const char *bb_component_project(uint8_t profile);
const char *bb_component_status_name(uint16_t status);
uint8_t bb_component_file_id_from_name(const char *name);
const char *bb_component_file_name(uint8_t file_id);
bool bb_component_image_parse(const uint8_t *bytes, size_t length,
                              bb_component_image_t *image);

#ifdef __cplusplus
}
#endif
#endif
