#include "bbvp.h"
#include "retry_policy.h"
#include "vad.h"

#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t load_vector(uint8_t *output, size_t capacity)
{
    FILE *file = fopen(BBVP_VECTOR_PATH, "r");
    assert(file);
    char line[256];
    size_t count = 0;
    while(fgets(line, sizeof(line), file)) {
        if(line[0] == '#') continue;
        for(size_t i = 0; line[i];) {
            if(isspace((unsigned char)line[i])) {
                i++;
                continue;
            }
            assert(isxdigit((unsigned char)line[i]));
            assert(isxdigit((unsigned char)line[i + 1]));
            char value[3] = {line[i], line[i + 1], '\0'};
            assert(count < capacity);
            output[count++] = (uint8_t)strtoul(value, NULL, 16);
            i += 2;
        }
    }
    fclose(file);
    return count;
}

static void test_bbvp_vector(void)
{
    const uint8_t stream_id[16] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
    };
    const uint8_t payload[] = {0xaa, 0xbb, 0xcc};
    bbvp_audio_frame_t frame = {
        .kind = BBVP_AUDIO_UPLINK_OPUS,
        .sequence = 0x01020304,
        .timestamp_ms = 0x0102030405060708ULL,
        .payload = payload,
        .payload_size = sizeof(payload),
    };
    memcpy(frame.stream_id, stream_id, 16);
    uint8_t encoded[128];
    size_t encoded_size = 0;
    assert(bbvp_audio_encode(encoded, sizeof(encoded), &frame, &encoded_size));
    uint8_t vector[128];
    size_t vector_size = load_vector(vector, sizeof(vector));
    assert(encoded_size == vector_size);
    assert(memcmp(encoded, vector, vector_size) == 0);

    bbvp_audio_frame_t decoded;
    assert(bbvp_audio_decode(vector, vector_size, &decoded));
    assert(decoded.sequence == frame.sequence);
    assert(decoded.timestamp_ms == frame.timestamp_ms);
    assert(memcmp(decoded.stream_id, stream_id, 16) == 0);
    assert(decoded.payload_size == sizeof(payload));
    assert(memcmp(decoded.payload, payload, sizeof(payload)) == 0);
    assert(!bbvp_audio_decode(vector, BBVP_AUDIO_HEADER_SIZE - 1, &decoded));
}

static void fill_samples(int16_t samples[320], int16_t value)
{
    for(size_t i = 0; i < 320; ++i) samples[i] = value;
}

static const binbot_vad_config_t vad_config = {
    .start_timeout_ms = 5000, .end_silence_ms = 1200, .transcript_grace_ms = 2400,
    .max_utterance_ms = 30000,
    .frame_ms = 20, .mode = 3, .start_frames = 2,
};

static void test_vad(void)
{
    binbot_vad_t vad;
    assert(binbot_vad_init(&vad, &vad_config));
    int16_t samples[320]; fill_samples(samples, 0);
    assert(binbot_vad_process(&vad, samples, 160) == BINBOT_VAD_WAITING);
    assert(!vad.elapsed_ms);
    for(unsigned i = 0; i < 249; ++i)
        assert(binbot_vad_process(&vad, samples, 320) == BINBOT_VAD_WAITING);
    assert(binbot_vad_process(&vad, samples, 320) == BINBOT_VAD_START_TIMEOUT);
    Fvad *detector = vad.detector;
    binbot_vad_reset(&vad);
    assert(vad.detector == detector && !vad.transcript_received && !vad.elapsed_ms);
    binbot_vad_note_transcript(&vad);
    for(unsigned i = 0; i < 119; ++i)
        assert(binbot_vad_process(&vad, samples, 320) == BINBOT_VAD_SPEECH);
    assert(binbot_vad_process(&vad, samples, 320) == BINBOT_VAD_SPEECH_ENDED);
    binbot_vad_reset(&vad);
    for(unsigned i = 0; i < 1500; ++i) {
        if(i % 90 == 0) binbot_vad_note_transcript(&vad);
        assert(binbot_vad_process(&vad, samples, 320) ==
               (i == 1499 ? BINBOT_VAD_MAX_DURATION : BINBOT_VAD_SPEECH));
    }
    binbot_vad_destroy(&vad);
}

static void test_vad_endpoint_clocks(void)
{
    binbot_vad_t vad;
    int16_t samples[320] = {0};
    assert(binbot_vad_init(&vad, &vad_config));
    binbot_vad_note_transcript(&vad);
    for(unsigned i = 0; i < 60; ++i)
        assert(binbot_vad_process(&vad, samples, 320) == BINBOT_VAD_SPEECH);
    assert(vad.silence_ms == 1200);
    binbot_vad_note_transcript(&vad);
    assert(vad.silence_ms == 1200 && vad.last_transcript_ms == 1200);
    for(unsigned i = 0; i < 119; ++i)
        assert(binbot_vad_process(&vad, samples, 320) == BINBOT_VAD_SPEECH);
    assert(binbot_vad_process(&vad, samples, 320) == BINBOT_VAD_SPEECH_ENDED);
    assert(vad.elapsed_ms == 3600);
    binbot_vad_destroy(&vad);

    binbot_vad_config_t local_clock = vad_config;
    local_clock.transcript_grace_ms = 600;
    assert(binbot_vad_init(&vad, &local_clock));
    binbot_vad_note_transcript(&vad);
    for(unsigned i = 0; i < 59; ++i)
        assert(binbot_vad_process(&vad, samples, 320) == BINBOT_VAD_SPEECH);
    assert(binbot_vad_process(&vad, samples, 320) == BINBOT_VAD_SPEECH_ENDED);
    assert(vad.elapsed_ms == 1200);
    binbot_vad_destroy(&vad);
}

static void test_retry_policy(void)
{
    assert(binbot_retry_delay_ms(0) == 1000);
    assert(binbot_retry_delay_ms(4) == 16000);
    assert(binbot_retry_delay_ms(5) == 30000);
    assert(binbot_retry_delay_ms(100) == 30000);
}

static void noise_frame(int16_t samples[320], uint32_t *seed)
{
    for(unsigned i = 0; i < 320; ++i) {
        *seed ^= *seed << 13; *seed ^= *seed >> 17; *seed ^= *seed << 5;
        samples[i] = (int16_t)(*seed % 2001) - 1000;
    }
}

static void test_vad_quiet_speech(bool provider_updates)
{
    FILE *speech = fopen(VAD_SPEECH_PATH, "rb"); assert(speech);
    int16_t samples[320], noise[320];
    uint32_t seed = 17;
    binbot_vad_t vad; assert(binbot_vad_init(&vad, &vad_config));
    for(unsigned i = 0; i < 100; ++i) {
        noise_frame(samples, &seed);
        binbot_vad_observe_noise(&vad, samples, 320);
    }
    binbot_vad_reset(&vad);
    unsigned frames = 0, quiet_frames = 0;
    bool detected = false;
    while(fread(samples, sizeof(*samples), 320, speech) == 320) {
        noise_frame(noise, &seed);
        for(unsigned i = 0; i < 320; ++i) samples[i] += noise[i];
        if(provider_updates && frames >= 60 && (frames - 60) % 80 == 0)
            binbot_vad_note_transcript(&vad);
        binbot_vad_result_t result = binbot_vad_process(&vad, samples, 320);
        detected |= result == BINBOT_VAD_SPEECH_STARTED;
        quiet_frames += vad.last_rms < 850;
        assert(result == BINBOT_VAD_WAITING || result == BINBOT_VAD_SPEECH_STARTED ||
               result == BINBOT_VAD_SPEECH);
        ++frames;
    }
    fclose(speech);
    assert(frames == 150 && quiet_frames > 100);
    assert(detected || vad.speech_started);
    binbot_vad_result_t result = BINBOT_VAD_SPEECH;
    for(unsigned i = 0; i < 175; ++i) {
        noise_frame(samples, &seed);
        result = binbot_vad_process(&vad, samples, 320);
        if(result == BINBOT_VAD_SPEECH_ENDED || result == BINBOT_VAD_START_TIMEOUT) break;
    }
    assert(result == (provider_updates ? BINBOT_VAD_SPEECH_ENDED : BINBOT_VAD_START_TIMEOUT));
    assert(vad.elapsed_ms <= (provider_updates ? 6000U : 5000U));
    binbot_vad_destroy(&vad);
}

static void test_vad_quiet_speech_with_pause(void)
{
    FILE *speech = fopen(VAD_SPEECH_PATH, "rb"); assert(speech);
    int16_t samples[320], noise[320];
    uint32_t seed = 17;
    binbot_vad_t vad; assert(binbot_vad_init(&vad, &vad_config));
    for(unsigned i = 0; i < 100; ++i) {
        noise_frame(samples, &seed);
        binbot_vad_observe_noise(&vad, samples, 320);
    }
    binbot_vad_reset(&vad);
    for(unsigned frame = 0; frame < 360; ++frame) {
        if(frame == 210) rewind(speech);
        noise_frame(noise, &seed);
        if(frame >= 150 && frame < 210) memcpy(samples, noise, sizeof(samples));
        else {
            assert(fread(samples, sizeof(*samples), 320, speech) == 320);
            for(unsigned i = 0; i < 320; ++i) samples[i] += noise[i];
        }
        if(frame == 60 || frame == 140 || frame == 240 || frame == 320)
            binbot_vad_note_transcript(&vad);
        binbot_vad_result_t result = binbot_vad_process(&vad, samples, 320);
        assert(result == BINBOT_VAD_WAITING || result == BINBOT_VAD_SPEECH_STARTED ||
               result == BINBOT_VAD_SPEECH);
    }
    fclose(speech);
    binbot_vad_result_t result = BINBOT_VAD_SPEECH;
    for(unsigned i = 0; i < 175 && result != BINBOT_VAD_SPEECH_ENDED; ++i) {
        noise_frame(samples, &seed);
        result = binbot_vad_process(&vad, samples, 320);
        assert(result == BINBOT_VAD_SPEECH || result == BINBOT_VAD_SPEECH_ENDED);
    }
    assert(result == BINBOT_VAD_SPEECH_ENDED);
    binbot_vad_destroy(&vad);
}

int main(void)
{
    test_bbvp_vector();
    test_vad();
    test_vad_endpoint_clocks();
    test_vad_quiet_speech(false);
    test_vad_quiet_speech(true);
    test_vad_quiet_speech_with_pause();
    test_retry_policy();
    puts("dialog core tests passed");
    return 0;
}
