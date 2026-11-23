#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protocol.h"
#include "firmware_image.h"

static const char *TOKEN = "0123456789abcdef0123456789abcdef";

static void check_request(const char *line, const char *expected)
{
    power_card_request_t request;
    const char *error = power_card_parse_request(line, TOKEN, &request);
    assert(expected ? error && strcmp(error, expected) == 0 : error == NULL);
}

static void test_requests(void)
{
    power_card_request_t request;
    const char *line = "{\"id\":\"a-1\",\"cmd\":\"ota\",\"token\":\"0123456789abcdef0123456789abcdef\","
                       "\"url\":\"http://192.168.1.2:8000/power_card.bin?v=2\"}\r";
    assert(power_card_parse_request(line, TOKEN, &request) == NULL);
    assert(request.has_id && strcmp(request.id, "a-1") == 0);
    assert(request.command == POWER_CARD_CMD_OTA);
    assert(strcmp(request.url, "http://192.168.1.2:8000/power_card.bin?v=2") == 0);
    const char *commands[] = {"status", "power", "reset", "ota_status"};
    for (unsigned i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
        char buffer[256];
        snprintf(buffer, sizeof(buffer), "{\"cmd\":\"%s\",\"token\":\"%s\"}", commands[i], TOKEN);
        assert(power_card_parse_request(buffer, TOKEN, &request) == NULL);
        assert(request.command == (i == 3 ? POWER_CARD_CMD_OTA_STATUS : (power_card_command_t)i));
        assert(!request.has_id);
    }
    check_request("", "invalid_json");
    check_request("[]", "invalid_json");
    check_request("{\"cmd\":\"power\"}", "unauthorized");
    check_request("{\"cmd\":\"power\",\"token\":null}", "unauthorized");
    check_request("{\"cmd\":\"power\",\"token\":\"wrong\"}", "unauthorized");
    check_request("{\"cmd\":\"power\\u0000reset\",\"token\":\"0123456789abcdef0123456789abcdef\"}", "invalid_request");
    check_request("{\"cmd\":\"power\",\"cmd\":\"reset\",\"token\":\"0123456789abcdef0123456789abcdef\"}", "invalid_request");
    check_request("{\"cmd\":\"power\",\"token\":\"0123456789abcdef0123456789abcdef\",\"token\":\"wrong\"}", "invalid_request");
    check_request("{\"cmd\":\"power\",\"token\":\"0123456789abcdef0123456789abcdef\",\"duration\":10000}", "invalid_request");
    check_request("{\"cmd\":\"power\",\"token\":\"0123456789abcdef0123456789abcdef\",\"url\":\"http://a/b\"}", "invalid_request");
    check_request("{\"cmd\":\"x\",\"token\":\"0123456789abcdef0123456789abcdef\"}", "unknown_command");
    check_request("{\"cmd\":\"ota\",\"token\":\"0123456789abcdef0123456789abcdef\"}", "invalid_url");
    check_request("{\"cmd\":\"power\",\"id\":1,\"token\":\"0123456789abcdef0123456789abcdef\"}", "invalid_request");
    check_request("{}{}", "invalid_json");
    check_request("{\"a\":[[[[[[[[[[[[[]]]]]]]]]]]]]}", "invalid_json");
    check_request("{\"cmd\":\"power\",\"token\":\"0123456789abcdef0123456789abcdef\"} garbage", "invalid_json");
    char buffer[1400];
    snprintf(buffer, sizeof(buffer), "{\"cmd\":\"status\",\"token\":\"%s\",\"id\":\"%065d\"}", TOKEN, 0);
    check_request(buffer, "invalid_request");
    memset(buffer, ' ', sizeof(buffer));
    buffer[sizeof(buffer) - 1] = 0;
    check_request(buffer, "invalid_request");
}

static void test_urls(void)
{
    const char *valid[] = {"http://a/b.bin", "http://192.168.1.2:8000/file.bin", "http://server/a%20b.bin?x=y"};
    const char *invalid[] = {"", "https://a/b", "ftp://a/b", "http:///a", "http://a:0/b", "http://a:65536/b",
                             "http://user:pass@a/b", "http://a/b#fragment", "http://a/b\nX:header", "http://a/a b"};
    for (unsigned i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i) assert(power_card_valid_ota_url(valid[i]));
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) assert(!power_card_valid_ota_url(invalid[i]));
    char long_url[POWER_CARD_MAX_URL + 2];
    memset(long_url, 'a', sizeof(long_url));
    memcpy(long_url, "http://a/", 9);
    long_url[sizeof(long_url) - 1] = 0;
    assert(!power_card_valid_ota_url(long_url));
}

static void test_streams(void)
{
    const char *stream = "{\"cmd\":\"status\",\"token\":\"0123456789abcdef0123456789abcdef\"}\n"
                         "{\"cmd\":\"power\",\"token\":\"0123456789abcdef0123456789abcdef\"}\r\n";
    const size_t length = strlen(stream);
    // Exercise every two-packet split, including two requests coalesced into one packet.
    for (size_t split = 0; split <= length; ++split) {
        power_card_frame_t frame = {0};
        unsigned completed = 0;
        const size_t boundaries[] = {0, split, length};
        for (int chunk = 0; chunk < 2; ++chunk) {
            for (size_t i = boundaries[chunk]; i < boundaries[chunk + 1]; ++i) {
                const int result = power_card_frame_feed(&frame, (unsigned char)stream[i]);
                assert(result >= 0);
                if (result == 1) {
                    power_card_request_t request;
                    assert(power_card_parse_request(frame.data, TOKEN, &request) == NULL);
                    assert(request.command == (completed == 0 ? POWER_CARD_CMD_STATUS : POWER_CARD_CMD_POWER));
                    frame.used = 0;
                    completed++;
                }
            }
        }
        assert(completed == 2 && frame.used == 0);
    }
    power_card_frame_t frame = {0};
    for (int i = 0; i < POWER_CARD_MAX_FRAME; ++i) assert(power_card_frame_feed(&frame, ' ') == 0);
    assert(power_card_frame_feed(&frame, '\n') == 1);
    assert(strlen(frame.data) == POWER_CARD_MAX_FRAME);
    assert(power_card_frame_feed(&frame, 'x') == -1);
    frame.used = 0;
    assert(power_card_frame_feed(&frame, 0) == -1);
}

static void test_firmware(const char *path)
{
    FILE *file = fopen(path, "rb");
    assert(file);
    unsigned char prefix[POWER_CARD_IMAGE_PREFIX_SIZE];
    assert(fread(prefix, 1, sizeof(prefix), file) == sizeof(prefix));
    fclose(file);
    esp_app_desc_t description;
    assert(power_card_image_check(prefix, sizeof(prefix), &description) == NULL);
    assert(strcmp(description.project_name, "power_card") == 0);
    for (size_t length = 0; length < sizeof(prefix); ++length)
        assert(strcmp(power_card_image_check(prefix, length, &description), "truncated_header") == 0);
    unsigned char modified[sizeof(prefix)];
    memcpy(modified, prefix, sizeof(prefix));
    modified[0] = 0;
    assert(strcmp(power_card_image_check(modified, sizeof(modified), &description), "invalid_firmware") == 0);
    memcpy(modified, prefix, sizeof(prefix));
    modified[offsetof(esp_image_header_t, chip_id)] = ESP_CHIP_ID_ESP32S3;
    assert(strcmp(power_card_image_check(modified, sizeof(modified), &description), "wrong_chip") == 0);
    memcpy(modified, prefix, sizeof(prefix));
    modified[offsetof(esp_image_header_t, hash_appended)] = 0;
    assert(strcmp(power_card_image_check(modified, sizeof(modified), &description), "missing_image_digest") == 0);
    memcpy(modified, prefix, sizeof(prefix));
    const size_t description_offset = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    modified[description_offset + offsetof(esp_app_desc_t, project_name)] = 'x';
    assert(strcmp(power_card_image_check(modified, sizeof(modified), &description), "wrong_project") == 0);
    memcpy(modified, prefix, sizeof(prefix));
    memset(modified + description_offset + offsetof(esp_app_desc_t, version), 'x', sizeof(description.version));
    assert(strcmp(power_card_image_check(modified, sizeof(modified), &description), "invalid_version") == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    test_requests();
    test_urls();
    test_streams();
    test_firmware(argv[1]);
    puts("PASS: command validation, authentication, URL parsing, TCP framing, firmware identity");
    return 0;
}
