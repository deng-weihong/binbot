#include "binbot/slave/protocol.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_round_trip(void)
{
    binbot_slave_frame_t input = {
        .version = BINBOT_SLAVE_PROTOCOL_VERSION,
        .address = 1,
        .sequence = 0x10,
        .command = BINBOT_SLAVE_CMD_PING,
        .flags = 0,
        .payload_length = 0,
    };
    uint8_t encoded[BINBOT_SLAVE_MAX_FRAME];
    size_t length = binbot_slave_frame_encode(&input, encoded, sizeof(encoded));
    assert(length == 10);
    assert(encoded[0] == 0xAA && encoded[1] == 0x55);
    assert(memcmp(encoded, (const uint8_t[]){0xAA, 0x55, 0x01, 0x01, 0x10,
                                            0x01, 0x00, 0x00, 0x69, 0x0A},
                  length) == 0);

    binbot_slave_parser_t parser;
    binbot_slave_parser_init(&parser);
    binbot_slave_frame_t output;
    int result = BINBOT_SLAVE_PARSE_NONE;
    for(size_t i = 0; i < length; ++i) {
        result = binbot_slave_parser_feed(&parser, encoded[i], &output);
    }
    assert(result == BINBOT_SLAVE_PARSE_FRAME);
    assert(memcmp(&input, &output, sizeof(input)) == 0);
}

static void test_noise_and_crc(void)
{
    binbot_slave_frame_t input = {
        .version = 1, .address = 1, .sequence = 4,
        .command = BINBOT_SLAVE_CMD_SET_OUTPUT,
        .flags = 0, .payload_length = 2, .payload = {0x03, 0x01},
    };
    uint8_t encoded[BINBOT_SLAVE_MAX_FRAME];
    size_t length = binbot_slave_frame_encode(&input, encoded, sizeof(encoded));
    encoded[length - 1] ^= 0x80;
    binbot_slave_parser_t parser;
    binbot_slave_parser_init(&parser);
    binbot_slave_frame_t output;
    assert(binbot_slave_parser_feed(&parser, 0x12, &output) == BINBOT_SLAVE_PARSE_NONE);
    int result = BINBOT_SLAVE_PARSE_NONE;
    for(size_t i = 0; i < length; ++i) result = binbot_slave_parser_feed(&parser, encoded[i], &output);
    assert(result == BINBOT_SLAVE_PARSE_ERROR);
}

static void test_status_codec(void)
{
    binbot_slave_status_all_t input = {
        .status_flags = 0x1234,
        .encoder_count = -123,
        .adc_key = 2,
        .adc_key_raw = 456,
        .component_voltage_mv = 5000,
        .component_current_ma = 321,
        .component_voltage_raw = 11,
        .component_current_raw = 22,
        .component_voltage_adc_mv = 33,
        .component_current_adc_mv = 44,
        .event_enable = 1,
    };
    uint8_t payload[BINBOT_SLAVE_MAX_PAYLOAD];
    size_t length = binbot_slave_status_encode(&input, payload, sizeof(payload));
    assert(length == 22);
    binbot_slave_status_all_t output;
    assert(binbot_slave_status_decode(payload, length, &output));
    assert(binbot_slave_status_equal(&input, &output));
}

static void test_event_codec(void)
{
    const uint8_t payload[] = {
        0x11, 0x00, 0x34, 0x12, 0x85, 0xFF, 0xFF, 0xFF,
        0x03, 0x78, 0x56, 0x88, 0x13, 0x2C, 0x01,
    };
    binbot_slave_event_report_t event_report;
    assert(binbot_slave_event_decode(payload, sizeof(payload), &event_report));
    assert(event_report.event_flags == 0x0011);
    assert(event_report.status_flags == 0x1234);
    assert(event_report.encoder_count == -123);
    assert(event_report.adc_key == 3 && event_report.adc_key_raw == 0x5678);
    assert(event_report.component_voltage_mv == 5000);
    assert(event_report.component_current_ma == 300);
}

static void test_request_validation(void)
{
    uint8_t output[] = {0x08, 1};
    assert(binbot_slave_validate_request(BINBOT_SLAVE_CMD_SET_OUTPUT,
                                         output, sizeof(output)) == BINBOT_SLAVE_ERR_OK);
    output[0] = 0x09;
    assert(binbot_slave_validate_request(BINBOT_SLAVE_CMD_SET_OUTPUT,
                                         output, sizeof(output)) == BINBOT_SLAVE_ERR_BAD_PARAM);
    assert(binbot_slave_validate_request(BINBOT_SLAVE_CMD_MOTOR_PULSE,
                                         (const uint8_t[]){0x89, 0x13}, 2) == BINBOT_SLAVE_ERR_BAD_PARAM);
}

int main(void)
{
    test_round_trip();
    test_noise_and_crc();
    test_status_codec();
    test_event_codec();
    test_request_validation();
    puts("slave protocol tests passed");
    return 0;
}
