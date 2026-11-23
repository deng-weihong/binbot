#include "binbot/audio/client.h"

#include <errno.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int main(int argc, char **argv)
{
    if(argc != 4) {
        fprintf(stderr, "usage: audio_echo_probe reference-24k-mono.pcm capture-16k-3ch.pcm seconds\n");
        return 1;
    }
    unsigned seconds = (unsigned)strtoul(argv[3], NULL, 10);
    if(seconds < 5 || seconds > 120) return 1;
    FILE *source = fopen(argv[1], "rb"), *capture = fopen(argv[2], "wb");
    if(!source || !capture) return 1;
    binbot_audio_client_t client;
    binbot_audio_client_init(&client, BINBOT_AUDIO_ROLE_APP,
        BINBOT_AUDIO_CAP_DIAGNOSTICS | BINBOT_AUDIO_CAP_PCM_LIVE |
        BINBOT_AUDIO_CAP_PCM_STEREO_LIVE, 0, NULL);
    if(!binbot_audio_client_connect(&client, now_ns() / 1000000)) return 1;
    uint8_t stream[16] = {0xAE, 0xC1, 0x01};
    binbot_audio_packet_t packet;
    binbot_audio_packet_init(&packet, BINBOT_AUDIO_MSG_PLAYBACK_BEGIN,
        sizeof(packet.payload.playback_begin), 0, now_ns());
    memcpy(packet.payload.playback_begin.stream_id, stream, sizeof(stream));
    packet.payload.playback_begin.sample_rate = 24000;
    packet.payload.playback_begin.channels = 1;
    packet.payload.playback_begin.frame_ms = 20;
    if(!binbot_audio_client_send(&client, &packet)) return 1;
    uint64_t start = now_ns(), next_play = start + 1200000000ULL;
    uint64_t stop_play = start + (uint64_t)(seconds - 2) * 1000000000ULL;
    uint64_t stop_capture = start + (uint64_t)seconds * 1000000000ULL;
    uint64_t clean_sequence = UINT64_MAX, previous_sequence = UINT64_MAX;
    uint64_t blocks = 0, gaps = 0, play_blocks = 0;
    int16_t clean[BINBOT_AUDIO_FRAME_SAMPLES] = {0};
    double raw_energy = 0, clean_energy = 0;
    bool ended = false;
    while(now_ns() < stop_capture) {
        uint64_t now = now_ns();
        if(!ended && now >= stop_play) {
            binbot_audio_packet_init(&packet, BINBOT_AUDIO_MSG_PLAYBACK_END,
                sizeof(packet.payload.playback_control), play_blocks, now);
            memcpy(packet.payload.playback_control.stream_id, stream, sizeof(stream));
            if(!binbot_audio_client_send(&client, &packet)) return 1;
            ended = true;
        }
        if(!ended && now >= next_play) {
            binbot_audio_packet_init(&packet, BINBOT_AUDIO_MSG_PLAYBACK_FRAME,
                sizeof(packet.payload.playback_frame), play_blocks, now);
            memcpy(packet.payload.playback_frame.stream_id, stream, sizeof(stream));
            packet.payload.playback_frame.sample_count = BINBOT_AUDIO_PLAYBACK_FRAME_SAMPLES;
            size_t count = fread(packet.payload.playback_frame.samples, sizeof(int16_t), 480, source);
            if(count < 480) {
                rewind(source);
                count += fread(packet.payload.playback_frame.samples + count, sizeof(int16_t), 480 - count, source);
                if(count < 480) return 1;
            }
            if(!binbot_audio_client_send(&client, &packet)) return 1;
            play_blocks++;
            next_play += 20000000;
        }
        while(binbot_audio_client_receive(&client, &packet) > 0) {
            if(packet.header.type == BINBOT_AUDIO_MSG_PCM) {
                memcpy(clean, packet.payload.pcm.samples, sizeof(clean));
                clean_sequence = packet.header.sequence;
            } else if(packet.header.type == BINBOT_AUDIO_MSG_PCM_STEREO &&
                      clean_sequence == packet.header.sequence) {
                if(previous_sequence != UINT64_MAX && clean_sequence != previous_sequence + 1) gaps++;
                previous_sequence = clean_sequence;
                int16_t output[BINBOT_AUDIO_FRAME_SAMPLES * 3];
                for(unsigned i = 0; i < BINBOT_AUDIO_FRAME_SAMPLES; ++i) {
                    output[i * 3] = packet.payload.pcm_stereo.samples[i * 2];
                    output[i * 3 + 1] = packet.payload.pcm_stereo.samples[i * 2 + 1];
                    output[i * 3 + 2] = clean[i];
                    if(now - start > 8000000000ULL && now < stop_play) {
                        double raw = ((double)output[i * 3] + output[i * 3 + 1]) / 2;
                        raw_energy += raw * raw;
                        clean_energy += (double)clean[i] * clean[i];
                    }
                }
                if(fwrite(output, sizeof(output), 1, capture) != 1) return 1;
                blocks++;
            }
        }
        struct pollfd fd = {.fd = binbot_audio_client_fd(&client), .events = POLLIN};
        if(poll(&fd, 1, 2) < 0 && errno != EINTR) return 1;
    }
    binbot_audio_client_close(&client);
    fclose(source); fclose(capture);
    printf("blocks=%llu gaps=%llu playback_blocks=%llu attenuation_db=%.1f\n",
        (unsigned long long)blocks, (unsigned long long)gaps, (unsigned long long)play_blocks,
        10 * log10((raw_energy + 1) / (clean_energy + 1)));
    return gaps ? 1 : 0;
}
