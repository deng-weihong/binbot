#define _DEFAULT_SOURCE
#include "binbot/app_diagnostics.h"
#include "binbot/audio/protocol.h"

#include <assert.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static double monotonic_seconds(void)
{
    struct timespec now;
    assert(!clock_gettime(CLOCK_MONOTONIC, &now));
    return now.tv_sec + now.tv_nsec / 1000000000.0;
}

static void send_packet(int fd, binbot_audio_packet_t *packet)
{
    assert(send(fd, packet, binbot_audio_packet_size(packet), MSG_NOSIGNAL) ==
           (ssize_t)binbot_audio_packet_size(packet));
}

static binbot_audio_packet_t receive_type(int fd, unsigned type)
{
    binbot_audio_packet_t packet;
    for(unsigned i = 0; i < 500; ++i) {
        struct pollfd descriptor = {.fd = fd, .events = POLLIN};
        assert(poll(&descriptor, 1, 2000) == 1);
        ssize_t size = recv(fd, &packet, sizeof(packet), 0);
        assert(size > 0 && binbot_audio_packet_is_valid(&packet, (uint32_t)size));
        if(packet.header.type == type) return packet;
    }
    abort();
}

static app_diag_status_t wait_status(app_diagnostics_t *d, bool recorded, app_diag_error_t error)
{
    for(unsigned i = 0; i < 300; ++i) {
        app_diag_status_t status = app_diagnostics_status(d);
        if(status.recorded == recorded && status.error == error) return status;
        usleep(5000);
    }
    assert(!"timed out waiting for diagnostic state");
    return (app_diag_status_t){0};
}

static void pcm(int fd, uint64_t sequence)
{
    binbot_audio_packet_t packet;
    binbot_audio_packet_init(&packet, BINBOT_AUDIO_MSG_PCM_STEREO,
                             sizeof(packet.payload.pcm_stereo), sequence, 0);
    for(unsigned i = 0; i < BINBOT_AUDIO_FRAME_SAMPLES; ++i) {
        packet.payload.pcm_stereo.samples[i * 2] = 1000;
        packet.payload.pcm_stereo.samples[i * 2 + 1] = -2000;
    }
    send_packet(fd, &packet);
}

int main(void)
{
    char path[] = "/tmp/binbot-diagnostics-core-XXXXXX";
    int temporary = mkstemp(path); assert(temporary >= 0); close(temporary); unlink(path);
    int server = socket(AF_UNIX, SOCK_SEQPACKET, 0); assert(server >= 0);
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
    assert(!bind(server, (struct sockaddr *)&address, sizeof(address)) && !listen(server, 1));
    app_diagnostics_t *d = app_diagnostics_create(path); assert(d);
    app_diagnostics_set_microphone(d, true);
    int peer = accept(server, NULL, NULL); assert(peer >= 0);
    binbot_audio_packet_t packet = receive_type(peer, BINBOT_AUDIO_MSG_HELLO);
    assert(packet.payload.hello.role == BINBOT_AUDIO_ROLE_APP);
    assert(packet.payload.hello.capabilities & BINBOT_AUDIO_CAP_DIAGNOSTICS);
    assert(packet.payload.hello.capabilities & BINBOT_AUDIO_CAP_PCM_STEREO_LIVE);
    pcm(peer, 1);
    app_diag_status_t status = wait_status(d, false, APP_DIAG_ERROR_NONE);
    assert(status.capture_ready && status.level[0] == 18 && status.level[1] == 37);
    assert(!app_diagnostics_play(d, APP_DIAG_PLAY_LEFT));
    assert(app_diagnostics_record(d));
    assert(!app_diagnostics_record(d));
    for(unsigned i = 0; i < 250; ++i) pcm(peer, i + 2);
    status = wait_status(d, true, APP_DIAG_ERROR_NONE);
    assert(status.record_ms == 5000 && !status.recording);

    for(unsigned ch = 0; ch < 2; ++ch) {
        assert(app_diagnostics_play(d, ch ? APP_DIAG_PLAY_RIGHT : APP_DIAG_PLAY_LEFT));
        packet = receive_type(peer, BINBOT_AUDIO_MSG_PLAYBACK_BEGIN);
        assert(packet.payload.playback_begin.sample_rate == 24000 && packet.payload.playback_begin.channels == 1);
        uint8_t id[16]; memcpy(id, packet.payload.playback_begin.stream_id, 16);
        packet = receive_type(peer, BINBOT_AUDIO_MSG_PLAYBACK_FRAME);
        assert(packet.payload.playback_frame.sample_count == 480);
        for(unsigned i = 0; i < 480; ++i) assert(packet.payload.playback_frame.samples[i] == (ch ? -2000 : 1000));
        app_diagnostics_stop(d);
        packet = receive_type(peer, BINBOT_AUDIO_MSG_PLAYBACK_CANCEL);
        assert(!memcmp(id, packet.payload.playback_control.stream_id, 16));
    }
    pcm(peer, 252); usleep(20000);
    assert(app_diagnostics_record(d));
    pcm(peer, 253); pcm(peer, 255);
    status = wait_status(d, false, APP_DIAG_ERROR_GAP);
    assert(!status.recording && !status.record_ms);
    assert(!app_diagnostics_play(d, APP_DIAG_PLAY_RIGHT));

    app_diagnostics_set_microphone(d, false);
    uint64_t sequence = 255;
    for(unsigned kind = APP_DIAG_PLAY_TONE; kind <= APP_DIAG_PLAY_SWEEP; ++kind) {
        assert(app_diagnostics_play(d, (app_diag_play_t)kind));
        receive_type(peer, BINBOT_AUDIO_MSG_PLAYBACK_BEGIN);
        double started = monotonic_seconds();
        unsigned count = 0, crossings = 0; int previous = 0, max = 0;
        for(;;) {
            struct pollfd descriptor = {.fd = peer, .events = POLLIN};
            assert(poll(&descriptor, 1, 2000) == 1);
            ssize_t size = recv(peer, &packet, sizeof(packet), 0);
            assert(size > 0 && binbot_audio_packet_is_valid(&packet, (uint32_t)size));
            if(packet.header.type == BINBOT_AUDIO_MSG_PLAYBACK_END) break;
            assert(packet.header.type == BINBOT_AUDIO_MSG_PLAYBACK_FRAME);
            for(unsigned i = 0; i < packet.payload.playback_frame.sample_count; ++i) {
                int value = packet.payload.playback_frame.samples[i];
                if(value > 0 && previous <= 0) ++crossings;
                if(abs(value) > max) max = abs(value);
                previous = value; ++count;
            }
            /* Live capture interrupts playback's wait between frame deadlines. */
            usleep(4000);
            pcm(peer, ++sequence);
        }
        double elapsed = monotonic_seconds() - started;
        double duration = kind == APP_DIAG_PLAY_TONE ? 3 : 5;
        assert(elapsed >= duration - .1 && elapsed <= duration + .25);
        assert(count == (kind == APP_DIAG_PLAY_TONE ? 72000U : 120000U));
        double expected = kind == APP_DIAG_PLAY_TONE ? 3000 : 200 * 5 * 39 / log(40);
        assert(fabs(crossings - expected) < 5 && max <= 11469 && max > 11000);
        usleep(150000);
        assert(app_diagnostics_status(d).playback == APP_DIAG_PLAY_NONE);
    }

    binbot_audio_packet_init(&packet, BINBOT_AUDIO_MSG_DIALOG_STATE, sizeof(packet.payload.dialog_state), 0, 0);
    packet.payload.dialog_state.state = BINBOT_DIALOG_STATE_SPEAKING;
    send_packet(peer, &packet); usleep(20000);
    assert(app_diagnostics_status(d).busy && !app_diagnostics_play(d, APP_DIAG_PLAY_TONE));
    close(peer);
    status = wait_status(d, false, APP_DIAG_ERROR_OFFLINE);
    assert(!status.connected && !status.capture_ready);
    app_diagnostics_destroy(d);
    close(server); assert(!unlink(path));
    puts("Diagnostic stereo levels, lossless capture, channel playback, tone/sweep, gap and disconnect passed");
    return 0;
}
