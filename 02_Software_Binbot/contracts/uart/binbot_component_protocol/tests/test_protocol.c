#ifdef NDEBUG
#undef NDEBUG
#endif
#include "binbot/component/ota_receiver.h"
#include "binbot/component/file_receiver.h"
#include "binbot/component/light.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    unsigned begins, writes, finishes, aborts, activations;
    uint16_t write_error, finish_error;
} mock_t;

static uint16_t begin(void *p, uint32_t total)
{
    assert(total > 0);
    ++((mock_t *)p)->begins;
    return BB_COMPONENT_OK;
}
static uint16_t write_chunk(void *p, const uint8_t *bytes, size_t n)
{
    assert(bytes && n);
    mock_t *m = p;
    ++m->writes;
    return m->write_error;
}
static uint16_t finish(void *p, const uint8_t digest[32])
{
    assert(digest);
    mock_t *m = p;
    ++m->finishes;
    return m->finish_error;
}
static void abort_transfer(void *p) { ++((mock_t *)p)->aborts; }
static uint16_t activate(void *p)
{
    ++((mock_t *)p)->activations;
    return BB_COMPONENT_OK;
}

static void make_image(uint8_t bytes[512], uint8_t profile)
{
    memset(bytes, 0, 512);
    bytes[0] = 0xe9;
    bb_component_put_u16(bytes + 12, 5);
    bb_component_put_u32(bytes + 32, 0xabcd5432);
    strcpy((char *)bytes + 48, "25");
    strcpy((char *)bytes + 80, bb_component_project(profile));
}

static void test_wire(void)
{
    assert(bb_component_crc32((const uint8_t *)"123456789", 9) == 0xcbf43926);
    bb_component_frame_t input = {0}, output;
    input.type = BB_COMPONENT_REQUEST;
    input.sequence = 0xabcd;
    input.command = BB_COMPONENT_TRANSFER_DATA;
    input.length = BB_COMPONENT_MAX_PAYLOAD;
    for (unsigned i = 0; i < input.length; ++i) input.payload[i] = (uint8_t)i;
    uint8_t bytes[BB_COMPONENT_FRAME_BYTES];
    size_t length = bb_component_encode(&input, bytes, sizeof(bytes));
    assert(length == sizeof(bytes));
    assert(bb_component_encode(&input, bytes, 10) == 0);
    bb_component_parser_t parser = {0};
    for (size_t i = 0; i < length; ++i)
        assert(bb_component_parser_push(&parser, bytes[i], &output) == (i == length - 1));
    assert(output.sequence == input.sequence && output.command == input.command);
    assert(output.length == input.length && memcmp(output.payload, input.payload, input.length) == 0);
    /* Bad CRC, overlapping magic, invalid version, and oversized length. */
    bytes[length - 1] ^= 1;
    for (size_t i = 0; i < length; ++i)
        assert(!bb_component_parser_push(&parser, bytes[i], &output));
    uint8_t noise[] = {0x42, 0x42, 0x43, 9, 1, 0, 0, 0, 0, 0xff, 0xff};
    for (size_t i = 0; i < sizeof(noise); ++i)
        assert(!bb_component_parser_push(&parser, noise[i], &output));
    input.length = 8;
    length = bb_component_encode(&input, bytes, sizeof(bytes));
    for (size_t i = 0; i < length; ++i)
        assert(bb_component_parser_push(&parser, bytes[i], &output) == (i == length - 1));
    assert(output.length == 8);
    input.type = BB_COMPONENT_EVENT;
    input.command = BB_COMPONENT_ANNOUNCE;
    input.length = BB_COMPONENT_ANNOUNCE_BYTES;
    length = bb_component_encode(&input, bytes, sizeof(bytes));
    for (size_t i = 0; i < length; ++i)
        assert(bb_component_parser_push(&parser, bytes[i], &output) == (i == length - 1));
    assert(output.type == BB_COMPONENT_EVENT && output.command == BB_COMPONENT_ANNOUNCE);
    uint8_t integer[8];
    bb_component_put_u64(integer, UINT64_C(0xfedcba9876543210));
    assert(bb_component_get_u64(integer) == UINT64_C(0xfedcba9876543210));
}

static void test_announcements(void)
{
    bb_component_announcement_t state;
    bb_component_announcement_init(&state, 42);
    uint16_t sequence;
    assert(bb_component_announcement_due(&state, false, 0, &sequence));
    uint16_t initial = sequence;
    assert(!bb_component_announcement_due(&state, false, 249, &sequence));
    assert(bb_component_announcement_due(&state, false, 250, &sequence));
    assert(sequence == initial);
    bb_component_frame_t ack = {.type = BB_COMPONENT_RESPONSE,
        .command = BB_COMPONENT_ANNOUNCE, .sequence = sequence,
        .length = BB_COMPONENT_ANNOUNCE_ACK_BYTES};
    bb_component_put_u64(ack.payload + 2, 41);
    assert(!bb_component_announcement_ack(&state, &ack));
    bb_component_put_u64(ack.payload + 2, 42);
    assert(bb_component_announcement_ack(&state, &ack));
    assert(!bb_component_announcement_due(&state, false, 1000, &sequence));
    assert(bb_component_announcement_due(&state, true, 1001, &sequence));
    assert(sequence != initial);
    assert(!bb_component_announcement_ack(&state, &ack));
    ack.sequence = sequence;
    ack.length--;
    assert(!bb_component_announcement_ack(&state, &ack));
    ack.length++;
    bb_component_put_u16(ack.payload, BB_COMPONENT_BUSY);
    assert(!bb_component_announcement_ack(&state, &ack));
    bb_component_put_u16(ack.payload, BB_COMPONENT_OK);
    assert(bb_component_announcement_ack(&state, &ack));
    assert(!bb_component_announcement_due(&state, true, 1500, &sequence));
    bb_component_announcement_init(&state, 43);
    assert(bb_component_announcement_due(&state, false, 0, &sequence));
    bb_component_announcement_observed(&state, true);
    assert(bb_component_announcement_due(&state, false, 250, &sequence));
    bb_component_announcement_observed(&state, false);
    assert(!bb_component_announcement_due(&state, false, 500, &sequence));
    assert(bb_component_announcement_due(&state, true, 501, &sequence));
}

static void test_receiver(void)
{
    mock_t m = {0};
    bb_component_receiver_t r;
    bb_component_ota_ops_t ops = {begin, write_chunk, finish, abort_transfer, activate};
    bb_component_receiver_init(&r, BB_COMPONENT_ROUND_SCREEN, 4096, &ops, &m);
    uint8_t payload[1024] = {0}, reply[1024];
    size_t reply_n;
    bb_component_put_u64(payload, 123);
    payload[8] = 1;
    payload[9] = BB_COMPONENT_ROUND_SCREEN;
    bb_component_put_u32(payload + 12, 515);
    assert(bb_component_receiver_command(&r, BB_COMPONENT_TRANSFER_BEGIN, payload, 47,
                                          reply, &reply_n) == BB_COMPONENT_BAD_REQUEST);
    payload[9] = BB_COMPONENT_HOLOGRAPHIC_SCREEN;
    assert(bb_component_receiver_command(&r, BB_COMPONENT_TRANSFER_BEGIN, payload, 48,
                                          reply, &reply_n) == BB_COMPONENT_WRONG_IMAGE);
    payload[9] = BB_COMPONENT_ROUND_SCREEN;
    bb_component_put_u32(payload + 12, 4097);
    assert(bb_component_receiver_command(&r, BB_COMPONENT_TRANSFER_BEGIN, payload, 48,
                                          reply, &reply_n) == BB_COMPONENT_TOO_LARGE);
    bb_component_put_u32(payload + 12, 515);
    for (unsigned retry = 0; retry < 3; ++retry)
        assert(bb_component_receiver_command(&r, BB_COMPONENT_TRANSFER_BEGIN, payload, 48,
                                              reply, &reply_n) == BB_COMPONENT_OK);
    assert(m.begins == 1);
    bb_component_put_u64(payload, 124);
    assert(bb_component_receiver_command(&r, BB_COMPONENT_TRANSFER_BEGIN, payload, 48,
                                          reply, &reply_n) == BB_COMPONENT_BUSY);
    bb_component_put_u64(payload, 123);
    assert(bb_component_receiver_command(&r, BB_COMPONENT_OTA_ACTIVATE, payload, 8,
                                          reply, &reply_n) == BB_COMPONENT_NOT_READY);
    assert(bb_component_receiver_command(&r, BB_COMPONENT_TRANSFER_COMMIT, payload, 8,
                                          reply, &reply_n) == BB_COMPONENT_OFFSET);
    bb_component_put_u32(payload + 8, 512);
    assert(bb_component_receiver_command(&r, BB_COMPONENT_TRANSFER_DATA, payload, 15,
                                          reply, &reply_n) == BB_COMPONENT_OFFSET);
    bb_component_put_u32(payload + 8, 0);
    make_image(payload + 12, BB_COMPONENT_ROUND_SCREEN);
    for (unsigned retry = 0; retry < 3; ++retry)
        assert(bb_component_receiver_command(&r, BB_COMPONENT_TRANSFER_DATA, payload, 524,
                                              reply, &reply_n) == BB_COMPONENT_OK);
    assert(m.writes == 1 && r.received == 512);
    payload[511] ^= 1;
    assert(bb_component_receiver_command(&r, BB_COMPONENT_TRANSFER_DATA, payload, 524,
                                          reply, &reply_n) == BB_COMPONENT_OFFSET);
    bb_component_put_u32(payload + 8, 512);
    assert(bb_component_receiver_command(&r, BB_COMPONENT_TRANSFER_DATA, payload, 16,
                                          reply, &reply_n) == BB_COMPONENT_TOO_LARGE);
    assert(bb_component_receiver_command(&r, BB_COMPONENT_TRANSFER_DATA, payload, 15,
                                          reply, &reply_n) == BB_COMPONENT_OK);
    assert(m.writes == 2 && r.received == 515);
    assert(bb_component_receiver_command(&r, BB_COMPONENT_TRANSFER_QUERY, payload, 8,
                                          reply, &reply_n) == BB_COMPONENT_OK);
    assert(reply_n == BB_COMPONENT_QUERY_BYTES && bb_component_get_u32(reply + 4) == 515);
    for (unsigned retry = 0; retry < 3; ++retry)
        assert(bb_component_receiver_command(&r, BB_COMPONENT_TRANSFER_COMMIT, payload, 8,
                                              reply, &reply_n) == BB_COMPONENT_OK);
    assert(m.finishes == 1 && m.activations == 0);
    for (unsigned retry = 0; retry < 3; ++retry)
        assert(bb_component_receiver_command(&r, BB_COMPONENT_OTA_ACTIVATE, payload, 8,
                                              reply, &reply_n) == BB_COMPONENT_OK);
    assert(m.activations == 1);
    assert(bb_component_receiver_command(&r, BB_COMPONENT_TRANSFER_ABORT, payload, 8,
                                          reply, &reply_n) == BB_COMPONENT_BUSY);

    /* Cross-profile and chip checks are performed on actual image bytes too. */
    bb_component_receiver_init(&r, BB_COMPONENT_ROUND_SCREEN, 4096, &ops, &m);
    memset(payload, 0, sizeof(payload));
    bb_component_put_u64(payload, 5);
    payload[8] = 1;
    payload[9] = BB_COMPONENT_ROUND_SCREEN;
    bb_component_put_u32(payload + 12, 512);
    assert(bb_component_receiver_command(&r, BB_COMPONENT_TRANSFER_BEGIN, payload, 48,
                                          reply, &reply_n) == BB_COMPONENT_OK);
    bb_component_put_u32(payload + 8, 0);
    make_image(payload + 12, BB_COMPONENT_HOLOGRAPHIC_SCREEN);
    assert(bb_component_receiver_command(&r, BB_COMPONENT_TRANSFER_DATA, payload, 524,
                                          reply, &reply_n) == BB_COMPONENT_WRONG_IMAGE);
    assert(r.state == BB_COMPONENT_IDLE && m.aborts == 1);
    bb_component_image_t image;
    make_image(payload, BB_COMPONENT_ROUND_SCREEN);
    assert(bb_component_image_parse(payload, 512, &image));
    payload[12] = 9;
    assert(!bb_component_image_parse(payload, 512, &image));
    make_image(payload, BB_COMPONENT_ROUND_SCREEN);
    memset(payload + 80, 'x', 32);
    assert(!bb_component_image_parse(payload, 512, &image));
}

static void test_ambient_profile(void)
{
    uint8_t payload[12 + 512] = {0}, reply[BB_COMPONENT_QUERY_BYTES];
    bb_component_image_t image;
    make_image(payload, BB_COMPONENT_AMBIENT_LIGHT);
    assert(bb_component_image_parse(payload, 512, &image));
    assert(image.profile == BB_COMPONENT_AMBIENT_LIGHT);
    assert(strcmp(image.project, "binbot_ambient_light") == 0);
    assert(bb_component_project(4) == NULL);
    strcpy((char *)payload + 80, "binbot_slave");
    assert(!bb_component_image_parse(payload, 512, &image));

    const uint8_t image_profiles[] = {BB_COMPONENT_AMBIENT_LIGHT, BB_COMPONENT_ROUND_SCREEN};
    for (unsigned i = 0; i < sizeof(image_profiles); ++i) {
        mock_t m = {0};
        bb_component_receiver_t receiver;
        const bb_component_ota_ops_t ops = {begin, write_chunk, finish, abort_transfer, activate};
        bb_component_receiver_init(&receiver, BB_COMPONENT_AMBIENT_LIGHT, 4096, &ops, &m);
        memset(payload, 0, sizeof(payload));
        bb_component_put_u64(payload, 1);
        payload[8] = 1;
        payload[9] = BB_COMPONENT_AMBIENT_LIGHT;
        bb_component_put_u32(payload + 12, 512);
        size_t reply_n;
        assert(bb_component_receiver_command(&receiver, BB_COMPONENT_TRANSFER_BEGIN, payload, 48,
                                              reply, &reply_n) == BB_COMPONENT_OK);
        bb_component_put_u32(payload + 8, 0);
        make_image(payload + 12, image_profiles[i]);
        const uint16_t expected = i == 0 ? BB_COMPONENT_OK : BB_COMPONENT_WRONG_IMAGE;
        assert(bb_component_receiver_command(&receiver, BB_COMPONENT_TRANSFER_DATA, payload,
                                              sizeof(payload), reply, &reply_n) == expected);
        assert(m.writes == (i == 0 ? 1u : 0u));
        assert(m.aborts == (i == 0 ? 0u : 1u));
    }
}

static uint16_t file_begin(void *p, uint8_t id, uint32_t total, const uint8_t sha[32])
{
    (void)id; (void)total; (void)sha;
    ++((mock_t *)p)->begins;
    return BB_COMPONENT_OK;
}
static uint16_t file_finish(void *p, uint8_t id, const uint8_t sha[32])
{
    (void)id; (void)sha;
    ++((mock_t *)p)->finishes;
    return BB_COMPONENT_OK;
}

static void send_file(bb_component_file_receiver_t *r, uint64_t transfer,
                      uint8_t file_id, uint32_t total)
{
    uint8_t payload[12 + 8], reply[BB_COMPONENT_FILE_QUERY_BYTES];
    uint8_t begin_payload[BB_COMPONENT_FILE_BEGIN_BYTES] = {0};
    size_t reply_n = 0;
    bb_component_put_u64(begin_payload, transfer);
    begin_payload[8] = file_id;
    begin_payload[9] = BB_COMPONENT_FILE_TYPE_GIF;
    assert(total > 0 && total <= 8);
    bb_component_put_u32(begin_payload + 12, total);
    assert(bb_component_file_receiver_command(r, BB_COMPONENT_FILE_BEGIN, begin_payload,
                                               sizeof(begin_payload), reply, &reply_n) == BB_COMPONENT_OK);
    assert(reply_n == 4 && bb_component_get_u32(reply) == 0);
    bb_component_put_u64(payload, transfer);
    bb_component_put_u32(payload + 8, 0);
    memcpy(payload + 12, "GIF89aXX", total);
    assert(bb_component_file_receiver_command(r, BB_COMPONENT_FILE_DATA, payload, 12 + total,
                                               reply, &reply_n) == BB_COMPONENT_OK);
    assert(reply_n == 4 && bb_component_get_u32(reply) == total);
    assert(bb_component_file_receiver_command(r, BB_COMPONENT_FILE_COMMIT, payload, 8,
                                               reply, &reply_n) == BB_COMPONENT_OK);
    /* A retried commit for the same transfer stays idempotent. */
    assert(bb_component_file_receiver_command(r, BB_COMPONENT_FILE_COMMIT, payload, 8,
                                               reply, &reply_n) == BB_COMPONENT_OK);
}

static void test_file_receiver_accepts_consecutive_files(void)
{
    mock_t m = {0};
    const bb_component_file_ops_t ops = {file_begin, write_chunk, file_finish, abort_transfer};
    bb_component_file_receiver_t r;
    bb_component_file_receiver_init(&r, &ops, &m);
    send_file(&r, 1, BB_COMPONENT_FILE_DISPLAY_GIF, 8);
    send_file(&r, 2, BB_COMPONENT_FILE_DISPLAY_GIF, 4);
    send_file(&r, 3, BB_COMPONENT_FILE_DISPLAY_GIF, 8);
    assert(m.begins == 3 && m.finishes == 3);
}

static void test_file_receiver_resume_and_restart(void)
{
    mock_t m = {0};
    const bb_component_file_ops_t ops = {file_begin, write_chunk, file_finish, abort_transfer};
    bb_component_file_receiver_t r;
    bb_component_file_receiver_init(&r, &ops, &m);
    uint8_t begin_payload[BB_COMPONENT_FILE_BEGIN_BYTES] = {0};
    uint8_t payload[16] = {0}, reply[BB_COMPONENT_FILE_QUERY_BYTES];
    size_t reply_n;
    bb_component_put_u64(begin_payload, 7);
    begin_payload[8] = BB_COMPONENT_FILE_DISPLAY_GIF;
    begin_payload[9] = BB_COMPONENT_FILE_TYPE_GIF;
    bb_component_put_u32(begin_payload + 12, 8);
    assert(bb_component_file_receiver_command(&r, BB_COMPONENT_FILE_BEGIN, begin_payload,
                                               sizeof(begin_payload), reply, &reply_n) == BB_COMPONENT_OK);
    bb_component_put_u64(payload, 7);
    memcpy(payload + 12, "GIF8", 4);
    assert(bb_component_file_receiver_command(&r, BB_COMPONENT_FILE_DATA, payload,
                                               sizeof(payload), reply, &reply_n) == BB_COMPONENT_OK);
    /* Retrying the current session resumes buffered bytes without reopening storage. */
    assert(bb_component_file_receiver_command(&r, BB_COMPONENT_FILE_BEGIN, begin_payload,
                                               sizeof(begin_payload), reply, &reply_n) == BB_COMPONENT_OK);
    assert(reply_n == 4 && bb_component_get_u32(reply) == 4 && m.begins == 1);
    assert(bb_component_file_receiver_command(&r, BB_COMPONENT_FILE_ABORT, payload, 8,
                                               reply, &reply_n) == BB_COMPONENT_OK);
    /* A replacement after an abort starts at zero, even if the old session had data. */
    send_file(&r, 8, BB_COMPONENT_FILE_DISPLAY_GIF, 4);
    assert(m.begins == 2 && m.aborts == 1 && m.finishes == 1);
}

static void test_light(void)
{
    bb_component_light_t light = {0};
    bb_component_light_settings_t settings = {1, BB_COMPONENT_LIGHT_SOLID, 255, 160, 80, 50, 50, 1}, decoded;
    uint8_t wire[BB_COMPONENT_LIGHT_BYTES], pixels[24], other[24];
    bb_component_light_encode(&settings, wire);
    assert(bb_component_light_decode(wire, sizeof(wire), &decoded));
    wire[6] = 4; assert(!bb_component_light_decode(wire, sizeof(wire), &decoded));
    wire[6] = 50; wire[1] = 5; assert(!bb_component_light_decode(wire, sizeof(wire), &decoded));
    assert(bb_component_light_set(&light, &settings, 1000));
    assert(!bb_component_light_render(&light, 1000, pixels, 8));
    assert(pixels[0] == 127 && pixels[1] == 80 && pixels[2] == 40);
    assert(bb_component_light_remaining(&light, 1000) == 1);
    settings.brightness = 60;
    assert(bb_component_light_set(&light, &settings, 30000));
    assert(light.deadline_ms == 61000);
    assert(bb_component_light_set(&light, &settings, 30001));
    assert(light.deadline_ms == 61000);
    assert(bb_component_light_render(&light, 61000, pixels, 8));
    assert(!light.settings.enabled && !light.settings.timer_minutes && !bb_component_light_remaining(&light, 61000));
    for(unsigned i = 0; i < sizeof(pixels); ++i) assert(!pixels[i]);
    settings.timer_minutes = 0;
    for(unsigned effect = BB_COMPONENT_LIGHT_BREATHE; effect <= BB_COMPONENT_LIGHT_BEAT; ++effect) {
        settings.effect = effect;
        assert(bb_component_light_set(&light, &settings, 100000));
        bb_component_light_render(&light, 100000, pixels, 8);
        bb_component_light_render(&light, effect == BB_COMPONENT_LIGHT_BLINK ? 102500 : 100450, other, 8);
        assert(memcmp(pixels, other, sizeof(pixels)));
        for(unsigned i = 0; i < sizeof(pixels); ++i) assert(pixels[i] <= 153);
    }
    settings.effect = BB_COMPONENT_LIGHT_SOLID; settings.brightness = 0;
    assert(bb_component_light_set(&light, &settings, 120000));
    bb_component_light_render(&light, 120000, pixels, 8);
    for(unsigned i = 0; i < sizeof(pixels); ++i) assert(!pixels[i]);
    settings.brightness = 101; assert(!bb_component_light_set(&light, &settings, 120000));
}

int main(int argc, char **argv)
{
    test_light();
    test_wire();
    test_announcements();
    test_receiver();
    test_ambient_profile();
    test_file_receiver_accepts_consecutive_files();
    test_file_receiver_resume_and_restart();
    for (int arg = 1; arg < argc; ++arg) {
        FILE *file = fopen(argv[arg], "rb");
        assert(file);
        uint8_t prefix[BB_COMPONENT_IMAGE_PREFIX_BYTES];
        assert(fread(prefix, 1, sizeof(prefix), file) == sizeof(prefix));
        fclose(file);
        bb_component_image_t image;
        assert(bb_component_image_parse(prefix, sizeof(prefix), &image));
        printf("Validated %s: profile=%u project=%s version=%s\n", argv[arg],
               image.profile, image.project, image.version);
    }
    puts("component protocol tests passed");
    return 0;
}
