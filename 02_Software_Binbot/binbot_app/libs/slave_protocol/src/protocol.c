#include "binbot/slave/protocol.h"

#include <string.h>

enum {
    PARSER_WAIT_SOF0 = 0,
    PARSER_WAIT_SOF1 = 1,
    PARSER_HEADER = 2,
    PARSER_PAYLOAD = 3,
    PARSER_CRC = 4,
    STATUS_PAYLOAD_LENGTH = 22,
    EVENT_PAYLOAD_LENGTH = 15,
};

static uint16_t read_u16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t read_u32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void write_u16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
}

static void write_u32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
    data[2] = (uint8_t)(value >> 16);
    data[3] = (uint8_t)(value >> 24);
}

uint16_t binbot_slave_crc16_modbus(const uint8_t *data, size_t length)
{
    uint16_t crc = 0xFFFFU;
    if(!data && length != 0) return 0;
    for(size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for(unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc & 1U) != 0 ? (uint16_t)((crc >> 1) ^ 0xA001U)
                                  : (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

size_t binbot_slave_frame_encode(const binbot_slave_frame_t *frame,
                                 uint8_t *output,
                                 size_t output_capacity)
{
    if(!frame || !output || frame->payload_length > BINBOT_SLAVE_MAX_PAYLOAD ||
       output_capacity < (size_t)BINBOT_SLAVE_FRAME_OVERHEAD + frame->payload_length) {
        return 0;
    }

    output[0] = 0xAA;
    output[1] = 0x55;
    output[2] = frame->version;
    output[3] = frame->address;
    output[4] = frame->sequence;
    output[5] = frame->command;
    output[6] = frame->flags;
    output[7] = frame->payload_length;
    if(frame->payload_length != 0) {
        memcpy(&output[8], frame->payload, frame->payload_length);
    }
    uint16_t crc = binbot_slave_crc16_modbus(&output[2],
            (size_t)6 + frame->payload_length);
    size_t crc_offset = (size_t)8 + frame->payload_length;
    output[crc_offset] = (uint8_t)crc;
    output[crc_offset + 1] = (uint8_t)(crc >> 8);
    return crc_offset + 2;
}

void binbot_slave_parser_init(binbot_slave_parser_t *parser)
{
    if(parser) memset(parser, 0, sizeof(*parser));
}

static void parser_reset(binbot_slave_parser_t *parser)
{
    parser->state = PARSER_WAIT_SOF0;
    parser->header_index = 0;
    parser->payload_index = 0;
    parser->crc_index = 0;
}

binbot_slave_parse_result_t binbot_slave_parser_feed(binbot_slave_parser_t *parser,
                                                     uint8_t byte,
                                                     binbot_slave_frame_t *frame)
{
    if(!parser || !frame) return BINBOT_SLAVE_PARSE_ERROR;

    switch(parser->state) {
    case PARSER_WAIT_SOF0:
        if(byte == 0xAA) parser->state = PARSER_WAIT_SOF1;
        return BINBOT_SLAVE_PARSE_NONE;
    case PARSER_WAIT_SOF1:
        if(byte == 0x55) {
            parser->state = PARSER_HEADER;
            parser->header_index = 0;
        }
        else if(byte != 0xAA) {
            parser->state = PARSER_WAIT_SOF0;
        }
        return BINBOT_SLAVE_PARSE_NONE;
    case PARSER_HEADER:
        parser->header[parser->header_index++] = byte;
        if(parser->header_index < sizeof(parser->header)) return BINBOT_SLAVE_PARSE_NONE;
        if(parser->header[5] > BINBOT_SLAVE_MAX_PAYLOAD) {
            parser_reset(parser);
            return BINBOT_SLAVE_PARSE_ERROR;
        }
        parser->payload_index = 0;
        parser->crc_index = 0;
        parser->state = parser->header[5] == 0 ? PARSER_CRC : PARSER_PAYLOAD;
        return BINBOT_SLAVE_PARSE_NONE;
    case PARSER_PAYLOAD:
        parser->payload[parser->payload_index++] = byte;
        if(parser->payload_index >= parser->header[5]) {
            parser->state = PARSER_CRC;
            parser->crc_index = 0;
        }
        return BINBOT_SLAVE_PARSE_NONE;
    case PARSER_CRC:
        parser->crc[parser->crc_index++] = byte;
        if(parser->crc_index < 2) return BINBOT_SLAVE_PARSE_NONE;
        {
            uint8_t crc_data[6 + BINBOT_SLAVE_MAX_PAYLOAD];
            size_t crc_length = (size_t)6 + parser->header[5];
            memcpy(crc_data, parser->header, 6);
            if(parser->header[5] != 0) {
                memcpy(&crc_data[6], parser->payload, parser->header[5]);
            }
            uint16_t expected = binbot_slave_crc16_modbus(crc_data, crc_length);
            uint16_t received = read_u16(parser->crc);
            if(expected != received) {
                parser_reset(parser);
                return BINBOT_SLAVE_PARSE_ERROR;
            }
            memset(frame, 0, sizeof(*frame));
            frame->version = parser->header[0];
            frame->address = parser->header[1];
            frame->sequence = parser->header[2];
            frame->command = parser->header[3];
            frame->flags = parser->header[4];
            frame->payload_length = parser->header[5];
            if(frame->payload_length != 0) {
                memcpy(frame->payload, parser->payload, frame->payload_length);
            }
            parser_reset(parser);
            return BINBOT_SLAVE_PARSE_FRAME;
        }
    default:
        parser_reset(parser);
        return BINBOT_SLAVE_PARSE_ERROR;
    }
}

static bool valid_output_id(uint8_t output_id)
{
    return output_id >= 0x01U && output_id <= 0x08U;
}

binbot_slave_error_t binbot_slave_validate_request(uint8_t command,
                                                   const uint8_t *payload,
                                                   size_t payload_length)
{
    if(payload_length > BINBOT_SLAVE_MAX_PAYLOAD || (payload_length != 0 && !payload)) {
        return BINBOT_SLAVE_ERR_BAD_LENGTH;
    }
    switch(command) {
    case BINBOT_SLAVE_CMD_PING:
    case BINBOT_SLAVE_CMD_GET_INFO:
    case BINBOT_SLAVE_CMD_GET_STATUS_ALL:
        return payload_length == 0 ? BINBOT_SLAVE_ERR_OK : BINBOT_SLAVE_ERR_BAD_LENGTH;
    case BINBOT_SLAVE_CMD_GET_STATUS_ITEMS:
        if(payload_length < 1 || payload_length != (size_t)payload[0] + 1U) {
            return BINBOT_SLAVE_ERR_BAD_LENGTH;
        }
        return BINBOT_SLAVE_ERR_OK;
    case BINBOT_SLAVE_CMD_SET_OUTPUT:
        if(payload_length != 2) return BINBOT_SLAVE_ERR_BAD_LENGTH;
        return valid_output_id(payload[0]) && payload[1] <= 1U
                   ? BINBOT_SLAVE_ERR_OK : BINBOT_SLAVE_ERR_BAD_PARAM;
    case BINBOT_SLAVE_CMD_SET_OUTPUTS:
        if(payload_length < 1 || (payload_length - 1U) % 2U != 0 ||
           payload_length != (size_t)payload[0] * 2U + 1U) {
            return BINBOT_SLAVE_ERR_BAD_LENGTH;
        }
        for(size_t i = 1; i < payload_length; i += 2) {
            if(!valid_output_id(payload[i]) || payload[i + 1] > 1U) {
                return BINBOT_SLAVE_ERR_BAD_PARAM;
            }
        }
        return BINBOT_SLAVE_ERR_OK;
    case BINBOT_SLAVE_CMD_MOTOR_PULSE: {
        if(payload_length != 2) return BINBOT_SLAVE_ERR_BAD_LENGTH;
        uint16_t duration = read_u16(payload);
        return duration > 0 && duration <= 5000U ? BINBOT_SLAVE_ERR_OK
                                                : BINBOT_SLAVE_ERR_BAD_PARAM;
    }
    case BINBOT_SLAVE_CMD_OTA_INFO:
    case BINBOT_SLAVE_CMD_OTA_FINISH:
    case BINBOT_SLAVE_CMD_OTA_ACTIVATE:
    case BINBOT_SLAVE_CMD_OTA_ABORT:
        return payload_length == 0 ? BINBOT_SLAVE_ERR_OK : BINBOT_SLAVE_ERR_BAD_LENGTH;
    case BINBOT_SLAVE_CMD_OTA_BEGIN:
        return payload_length == 48 ? BINBOT_SLAVE_ERR_OK : BINBOT_SLAVE_ERR_BAD_LENGTH;
    case BINBOT_SLAVE_CMD_OTA_DATA:
        return payload_length >= 8 && payload_length <= 64 && payload_length % 4 == 0 ?
               BINBOT_SLAVE_ERR_OK : BINBOT_SLAVE_ERR_BAD_LENGTH;
    case BINBOT_SLAVE_CMD_EVENT_ENABLE:
        if(payload_length != 3) return BINBOT_SLAVE_ERR_BAD_LENGTH;
        return payload[0] <= 1U ? BINBOT_SLAVE_ERR_OK : BINBOT_SLAVE_ERR_BAD_PARAM;
    default:
        return BINBOT_SLAVE_ERR_UNSUPPORTED_CMD;
    }
}

bool binbot_slave_status_decode(const uint8_t *payload,
                                size_t payload_length,
                                binbot_slave_status_all_t *status)
{
    if(!payload || !status || payload_length != STATUS_PAYLOAD_LENGTH) return false;
    memset(status, 0, sizeof(*status));
    status->status_flags = read_u16(&payload[0]);
    status->encoder_count = (int32_t)read_u32(&payload[2]);
    status->adc_key = payload[6];
    status->adc_key_raw = read_u16(&payload[7]);
    status->component_voltage_mv = read_u16(&payload[9]);
    status->component_current_ma = read_u16(&payload[11]);
    status->component_voltage_raw = read_u16(&payload[13]);
    status->component_current_raw = read_u16(&payload[15]);
    status->component_voltage_adc_mv = read_u16(&payload[17]);
    status->component_current_adc_mv = read_u16(&payload[19]);
    status->event_enable = payload[21];
    return true;
}

bool binbot_slave_event_decode(const uint8_t *payload,
                               size_t payload_length,
                               binbot_slave_event_report_t *event_report)
{
    if(!payload || !event_report || payload_length != EVENT_PAYLOAD_LENGTH) return false;
    memset(event_report, 0, sizeof(*event_report));
    event_report->event_flags = read_u16(&payload[0]);
    event_report->status_flags = read_u16(&payload[2]);
    event_report->encoder_count = (int32_t)read_u32(&payload[4]);
    event_report->adc_key = payload[8];
    event_report->adc_key_raw = read_u16(&payload[9]);
    event_report->component_voltage_mv = read_u16(&payload[11]);
    event_report->component_current_ma = read_u16(&payload[13]);
    return true;
}

size_t binbot_slave_status_encode(const binbot_slave_status_all_t *status,
                                  uint8_t *payload,
                                  size_t payload_capacity)
{
    if(!status || !payload || payload_capacity < STATUS_PAYLOAD_LENGTH) return 0;
    write_u16(&payload[0], status->status_flags);
    write_u32(&payload[2], (uint32_t)status->encoder_count);
    payload[6] = status->adc_key;
    write_u16(&payload[7], status->adc_key_raw);
    write_u16(&payload[9], status->component_voltage_mv);
    write_u16(&payload[11], status->component_current_ma);
    write_u16(&payload[13], status->component_voltage_raw);
    write_u16(&payload[15], status->component_current_raw);
    write_u16(&payload[17], status->component_voltage_adc_mv);
    write_u16(&payload[19], status->component_current_adc_mv);
    payload[21] = status->event_enable;
    return STATUS_PAYLOAD_LENGTH;
}

bool binbot_slave_status_equal(const binbot_slave_status_all_t *left,
                               const binbot_slave_status_all_t *right)
{
    return left && right && left->status_flags == right->status_flags &&
           left->encoder_count == right->encoder_count && left->adc_key == right->adc_key &&
           left->adc_key_raw == right->adc_key_raw &&
           left->component_voltage_mv == right->component_voltage_mv &&
           left->component_current_ma == right->component_current_ma &&
           left->component_voltage_raw == right->component_voltage_raw &&
           left->component_current_raw == right->component_current_raw &&
           left->component_voltage_adc_mv == right->component_voltage_adc_mv &&
           left->component_current_adc_mv == right->component_current_adc_mv &&
           left->event_enable == right->event_enable;
}
