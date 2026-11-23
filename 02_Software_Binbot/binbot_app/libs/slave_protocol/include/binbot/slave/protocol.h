#ifndef BINBOT_SLAVE_PROTOCOL_H
#define BINBOT_SLAVE_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    BINBOT_SLAVE_PROTOCOL_VERSION = 1,
    BINBOT_SLAVE_DEFAULT_ADDRESS = 1,
    BINBOT_SLAVE_MAX_PAYLOAD = 64,
    BINBOT_SLAVE_FRAME_OVERHEAD = 2 + 6 + 2,
    BINBOT_SLAVE_MAX_FRAME = BINBOT_SLAVE_FRAME_OVERHEAD + BINBOT_SLAVE_MAX_PAYLOAD,
};

#define BINBOT_SLAVE_DEFAULT_DEVICE "/dev/ttyS4"
#define BINBOT_SLAVE_DEFAULT_SOCKET "/tmp/binbot_slave.sock"

typedef enum {
    BINBOT_SLAVE_CMD_PING = 0x01,
    BINBOT_SLAVE_CMD_GET_INFO = 0x02,
    BINBOT_SLAVE_CMD_GET_STATUS_ALL = 0x10,
    BINBOT_SLAVE_CMD_GET_STATUS_ITEMS = 0x11,
    BINBOT_SLAVE_CMD_SET_OUTPUT = 0x20,
    BINBOT_SLAVE_CMD_SET_OUTPUTS = 0x21,
    BINBOT_SLAVE_CMD_MOTOR_PULSE = 0x22,
    BINBOT_SLAVE_CMD_EVENT_ENABLE = 0x30,
    BINBOT_SLAVE_CMD_EVENT_REPORT = 0x31,
    BINBOT_SLAVE_CMD_OTA_INFO = 0x40,
    BINBOT_SLAVE_CMD_OTA_BEGIN = 0x41,
    BINBOT_SLAVE_CMD_OTA_DATA = 0x42,
    BINBOT_SLAVE_CMD_OTA_FINISH = 0x43,
    BINBOT_SLAVE_CMD_OTA_ACTIVATE = 0x44,
    BINBOT_SLAVE_CMD_OTA_ABORT = 0x45,
} binbot_slave_command_t;

enum {
    BINBOT_SLAVE_FLAG_RESPONSE = 1U << 0,
    BINBOT_SLAVE_FLAG_ERROR = 1U << 1,
    BINBOT_SLAVE_FLAG_EVENT = 1U << 2,
};

enum {
    BINBOT_SLAVE_EVENT_INPUT_CHANGED = 1U << 0,
    BINBOT_SLAVE_EVENT_COMPONENT_ADC_CHANGED = 1U << 4,
};

typedef enum {
    BINBOT_SLAVE_ERR_OK = 0x00,
    BINBOT_SLAVE_ERR_CRC = 0x01,
    BINBOT_SLAVE_ERR_UNSUPPORTED_CMD = 0x02,
    BINBOT_SLAVE_ERR_BAD_LENGTH = 0x03,
    BINBOT_SLAVE_ERR_BAD_PARAM = 0x04,
    BINBOT_SLAVE_ERR_BUSY = 0x05,
    BINBOT_SLAVE_ERR_DENIED = 0x06,
} binbot_slave_error_t;

typedef struct {
    uint8_t version;
    uint8_t address;
    uint8_t sequence;
    uint8_t command;
    uint8_t flags;
    uint8_t payload_length;
    uint8_t payload[BINBOT_SLAVE_MAX_PAYLOAD];
} binbot_slave_frame_t;

typedef enum {
    BINBOT_SLAVE_PARSE_NONE = 0,
    BINBOT_SLAVE_PARSE_FRAME = 1,
    BINBOT_SLAVE_PARSE_ERROR = -1,
} binbot_slave_parse_result_t;

typedef struct {
    uint8_t state;
    uint8_t header[6];
    uint8_t header_index;
    uint8_t payload[BINBOT_SLAVE_MAX_PAYLOAD];
    uint8_t payload_index;
    uint8_t crc[2];
    uint8_t crc_index;
} binbot_slave_parser_t;

typedef struct {
    uint16_t status_flags;
    int32_t encoder_count;
    uint8_t adc_key;
    uint16_t adc_key_raw;
    uint16_t component_voltage_mv;
    uint16_t component_current_ma;
    uint16_t component_voltage_raw;
    uint16_t component_current_raw;
    uint16_t component_voltage_adc_mv;
    uint16_t component_current_adc_mv;
    uint8_t event_enable;
} binbot_slave_status_all_t;

typedef struct {
    uint16_t event_flags;
    uint16_t status_flags;
    int32_t encoder_count;
    uint8_t adc_key;
    uint16_t adc_key_raw;
    uint16_t component_voltage_mv;
    uint16_t component_current_ma;
} binbot_slave_event_report_t;

uint16_t binbot_slave_crc16_modbus(const uint8_t *data, size_t length);

/* Returns the encoded frame length, or 0 for invalid arguments/capacity. */
size_t binbot_slave_frame_encode(const binbot_slave_frame_t *frame,
                                 uint8_t *output,
                                 size_t output_capacity);

void binbot_slave_parser_init(binbot_slave_parser_t *parser);
binbot_slave_parse_result_t binbot_slave_parser_feed(binbot_slave_parser_t *parser,
                                                     uint8_t byte,
                                                     binbot_slave_frame_t *frame);

/* Validate a host request before putting it on the UART queue. */
binbot_slave_error_t binbot_slave_validate_request(uint8_t command,
                                                   const uint8_t *payload,
                                                   size_t payload_length);

bool binbot_slave_status_decode(const uint8_t *payload,
                                size_t payload_length,
                                binbot_slave_status_all_t *status);
bool binbot_slave_event_decode(const uint8_t *payload,
                               size_t payload_length,
                               binbot_slave_event_report_t *event_report);
size_t binbot_slave_status_encode(const binbot_slave_status_all_t *status,
                                  uint8_t *payload,
                                  size_t payload_capacity);
bool binbot_slave_status_equal(const binbot_slave_status_all_t *left,
                               const binbot_slave_status_all_t *right);

#ifdef __cplusplus
}
#endif

#endif
