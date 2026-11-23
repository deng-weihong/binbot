#include "binbot/audio/protocol.h"
#include "playback.h"
#include "pcm_clock.h"

#include <alsa/asoundlib.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

enum {
    AUDIO_CAPTURE_CHANNELS = 2,
    AUDIO_CLIENT_CAPACITY = 8,
    AUDIO_PREROLL_FRAMES = BINBOT_AUDIO_DEFAULT_PREROLL_MS / BINBOT_AUDIO_FRAME_MS,
    AUDIO_LEVEL_INTERVAL_FRAMES = 100 / BINBOT_AUDIO_FRAME_MS,
};

typedef struct {
    int fd;
    binbot_audio_client_role_t role;
    uint32_t capabilities;
    uint32_t dropped_frames;
    bool ready;
} audio_client_slot_t;

typedef struct {
    int listen_fd;
    char socket_path[108];
    audio_playback_t *playback;
    audio_client_slot_t clients[AUDIO_CLIENT_CAPACITY];
    binbot_audio_packet_t preroll[AUDIO_PREROLL_FRAMES];
    size_t preroll_count;
    size_t preroll_next;
    bool dialog_connected;
    bool microphone_muted;
    binbot_audio_dialog_state_t dialog_state;
    bool calendar_state_valid;
    binbot_audio_calendar_state_t calendar_state;
    binbot_audio_noise_state_t noise_state;
    uint64_t noise_last_check_ns;
    bool diagnostic_playback;
    uint8_t diagnostic_stream_id[16];
} audio_server_t;

typedef struct {
    float previous_input;
    float previous_output;
} audio_channel_filter_t;

typedef struct {
    enum {
        AUDIO_SOURCE_MIX = 0,
        AUDIO_SOURCE_LEFT,
        AUDIO_SOURCE_RIGHT,
    } source;
    float gain;
    bool highpass;
    audio_channel_filter_t channels[AUDIO_CAPTURE_CHANNELS];
} audio_frontend_t;

static volatile sig_atomic_t running = 1;

static bool configure_output_volume(const char *card, const char *control_name,
                                    uint8_t percent);

static const char *microphone_state_path(void)
{
    const char *path = getenv("BINBOT_MICROPHONE_STATE");
    return path && *path ? path : "/oem/data/microphone.state";
}

static bool save_microphone_state(bool muted)
{
    const char *path = microphone_state_path();
    char temporary[512];
    int length = snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    if(length < 0 || (size_t)length >= sizeof(temporary)) return false;
    int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if(fd < 0) return false;
    const char value = muted ? '1' : '0';
    bool success = fchmod(fd, 0600) == 0 && write(fd, &value, 1) == 1 && fsync(fd) == 0;
    if(close(fd) != 0) success = false;
    if(success) success = rename(temporary, path) == 0;
    if(success) {
        char directory[512]; snprintf(directory, sizeof(directory), "%s", path);
        char *slash = strrchr(directory, '/');
        if(slash) { *slash = '\0'; int parent = open(directory[0] ? directory : "/", O_RDONLY | O_DIRECTORY);
            if(parent < 0) success = false;
            else { success = fsync(parent) == 0; close(parent); }
        }
    }
    if(!success) unlink(temporary);
    return success;
}

static void stop_requested(int signal_number)
{
    (void)signal_number;
    running = 0;
}

static uint64_t monotonic_ns(void)
{
    struct timespec value;
    if(clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return (uint64_t)value.tv_sec * 1000000000ULL + (uint64_t)value.tv_nsec;
}

static bool set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if(flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return false;
    int descriptor_flags = fcntl(fd, F_GETFD, 0);
    if(descriptor_flags >= 0) fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC);
    return true;
}

static void client_close(audio_client_slot_t *client)
{
    if(client->fd >= 0) close(client->fd);
    *client = (audio_client_slot_t){.fd = -1};
}

static bool server_start(audio_server_t *server, const char *socket_path)
{
    memset(server, 0, sizeof(*server));
    FILE *microphone_state = fopen(microphone_state_path(), "r");
    if(microphone_state) { server->microphone_muted = fgetc(microphone_state) == '1'; fclose(microphone_state); }
    server->listen_fd = -1;
    for(size_t i = 0; i < AUDIO_CLIENT_CAPACITY; ++i) server->clients[i].fd = -1;
    snprintf(server->socket_path, sizeof(server->socket_path), "%s",
             socket_path && socket_path[0] ? socket_path : BINBOT_AUDIO_DEFAULT_SOCKET);

    int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if(fd < 0) {
        fprintf(stderr, "audio: socket failed: %s\n", strerror(errno));
        return false;
    }
    if(!set_nonblocking(fd)) {
        fprintf(stderr, "audio: cannot configure server socket: %s\n", strerror(errno));
        close(fd);
        return false;
    }

    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if(strlen(server->socket_path) >= sizeof(address.sun_path)) {
        fprintf(stderr, "audio: socket path is too long\n");
        close(fd);
        return false;
    }
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", server->socket_path);
    unlink(server->socket_path);
    socklen_t address_size = (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
                                        strlen(address.sun_path) + 1U);
    if(bind(fd, (const struct sockaddr *)&address, address_size) != 0 ||
       listen(fd, AUDIO_CLIENT_CAPACITY) != 0) {
        fprintf(stderr, "audio: cannot listen on %s: %s\n",
                server->socket_path, strerror(errno));
        close(fd);
        unlink(server->socket_path);
        return false;
    }
    chmod(server->socket_path, 0660);
    server->listen_fd = fd;
    fprintf(stdout, "audio: IPC listening on %s\n", server->socket_path);
    return true;
}

static void server_stop(audio_server_t *server)
{
    for(size_t i = 0; i < AUDIO_CLIENT_CAPACITY; ++i) client_close(&server->clients[i]);
    if(server->listen_fd >= 0) close(server->listen_fd);
    if(server->socket_path[0]) unlink(server->socket_path);
    server->listen_fd = -1;
}

static void server_accept_clients(audio_server_t *server)
{
    while(running) {
        int fd = accept(server->listen_fd, NULL, NULL);
        if(fd < 0) {
            if(errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                fprintf(stderr, "audio: accept failed: %s\n", strerror(errno));
            }
            return;
        }
        if(!set_nonblocking(fd)) {
            close(fd);
            continue;
        }
        size_t slot = AUDIO_CLIENT_CAPACITY;
        for(size_t i = 0; i < AUDIO_CLIENT_CAPACITY; ++i) {
            if(server->clients[i].fd < 0) {
                slot = i;
                break;
            }
        }
        if(slot == AUDIO_CLIENT_CAPACITY) {
            fprintf(stderr, "audio: rejected client, capacity reached\n");
            close(fd);
            continue;
        }
        server->clients[slot] = (audio_client_slot_t){.fd = fd};
    }
}

static bool client_send(audio_client_slot_t *client,
                        const binbot_audio_packet_t *packet,
                        bool is_pcm)
{
    uint32_t packet_size = binbot_audio_packet_size(packet);
    ssize_t sent = send(client->fd, packet, packet_size, MSG_DONTWAIT | MSG_NOSIGNAL);
    if(sent == (ssize_t)packet_size) {
        if(is_pcm) client->dropped_frames = 0;
        return true;
    }
    if(sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        if(is_pcm) {
            client->dropped_frames++;
            if(client->dropped_frames == 1 || client->dropped_frames % 100U == 0) {
                fprintf(stderr, "audio: role %u is slow, dropped %u PCM frames\n",
                        (unsigned)client->role, client->dropped_frames);
            }
        }
        return false;
    }
    fprintf(stderr, "audio: role %u disconnected: %s\n",
            (unsigned)client->role, sent < 0 ? strerror(errno) : "short send");
    client_close(client);
    return false;
}

static void server_send_preroll(audio_server_t *server,
                                audio_client_slot_t *client,
                                uint32_t requested_ms)
{
    size_t requested_frames = requested_ms / BINBOT_AUDIO_FRAME_MS;
    if(requested_frames > AUDIO_PREROLL_FRAMES) requested_frames = AUDIO_PREROLL_FRAMES;
    if(requested_frames > server->preroll_count) requested_frames = server->preroll_count;
    size_t start = (server->preroll_next + AUDIO_PREROLL_FRAMES - requested_frames) %
                   AUDIO_PREROLL_FRAMES;
    for(size_t i = 0; i < requested_frames && client->fd >= 0; ++i) {
        size_t index = (start + i) % AUDIO_PREROLL_FRAMES;
        if(!client_send(client, &server->preroll[index], true)) break;
    }
}

static void server_broadcast(audio_server_t *server,
                             const binbot_audio_packet_t *packet,
                             uint32_t required_capability,
                             bool is_pcm)
{
    for(size_t i = 0; i < AUDIO_CLIENT_CAPACITY; ++i) {
        audio_client_slot_t *client = &server->clients[i];
        if(client->fd >= 0 && client->ready &&
           (client->capabilities & required_capability) != 0) {
            if(is_pcm && client->role == BINBOT_AUDIO_ROLE_WAKE &&
               (audio_playback_active(server->playback) ||
                (server->dialog_state.state >= BINBOT_DIALOG_STATE_LISTENING &&
                 server->dialog_state.state <= BINBOT_DIALOG_STATE_CANCELLING))) {
                continue;
            }
            client_send(client, packet, is_pcm);
        }
    }
}

static bool diagnostics_connected(const audio_server_t *server)
{
    for(size_t i = 0; i < AUDIO_CLIENT_CAPACITY; ++i) {
        const audio_client_slot_t *client = &server->clients[i];
        if(client->fd >= 0 && client->ready && client->role == BINBOT_AUDIO_ROLE_APP &&
           (client->capabilities & BINBOT_AUDIO_CAP_DIAGNOSTICS)) return true;
    }
    return false;
}

static void server_emit_wake(audio_server_t *server, int hotword_index,
                             uint64_t sequence, uint64_t timestamp_ns)
{
    if(!server) return;
    if(diagnostics_connected(server)) {
        fprintf(stdout, "audio: wake ignored during hardware diagnostics\n");
        return;
    }
    /* Do not let a repeated hotword or a UI click cancel an answer that is
     * already being played.  The dialog service ignores the corresponding
     * event while its turn is active, so cancelling the queue here would only
     * make the answer stop abruptly. */
    if(audio_playback_dialog_active(server->playback)) {
        fprintf(stdout, "audio: wake ignored while dialog playback is active\n");
        return;
    }
    binbot_audio_packet_t event;
    binbot_audio_packet_init(&event, BINBOT_AUDIO_MSG_WAKE_DETECTED,
                             sizeof(event.payload.wake), sequence,
                             timestamp_ns);
    event.payload.wake.hotword_index = hotword_index > 0 ? hotword_index : 0;
    server_broadcast(server, &event, BINBOT_AUDIO_CAP_EVENTS, false);
    audio_playback_request(server->playback);
}

static void server_handle_packet(audio_server_t *server,
                                 audio_client_slot_t *client,
                                 const binbot_audio_packet_t *packet)
{
    if(packet->header.type == BINBOT_AUDIO_MSG_HELLO &&
       packet->header.payload_size == sizeof(packet->payload.hello)) {
        if(packet->payload.hello.role <= BINBOT_AUDIO_ROLE_UNKNOWN ||
           packet->payload.hello.role > BINBOT_AUDIO_ROLE_DIALOG) {
            fprintf(stderr, "audio: rejected invalid client role\n");
            client_close(client);
            return;
        }
        client->role = (binbot_audio_client_role_t)packet->payload.hello.role;
        client->capabilities = packet->payload.hello.capabilities;
        client->ready = true;
        fprintf(stdout, "audio: role %u connected, capabilities=0x%x\n",
                (unsigned)client->role, client->capabilities);
        if((client->capabilities & BINBOT_AUDIO_CAP_PCM_PREROLL) != 0) {
            server_send_preroll(server, client, packet->payload.hello.preroll_ms);
        }
        if((client->capabilities & BINBOT_AUDIO_CAP_DIALOG_STATE) != 0) {
            binbot_audio_packet_t state;
            binbot_audio_packet_init(&state, BINBOT_AUDIO_MSG_DIALOG_STATE,
                                     sizeof(state.payload.dialog_state), 0, monotonic_ns());
            state.payload.dialog_state = server->dialog_state;
            (void)client_send(client, &state, false);
        }
        if((client->capabilities & BINBOT_AUDIO_CAP_CALENDAR_STATE) != 0 &&
           server->calendar_state_valid) {
            binbot_audio_packet_t state;
            binbot_audio_packet_init(&state, BINBOT_AUDIO_MSG_CALENDAR_STATE,
                                     sizeof(state.payload.calendar_state), 0, monotonic_ns());
            state.payload.calendar_state = server->calendar_state;
            (void)client_send(client, &state, false);
        }
        if(client->role == BINBOT_AUDIO_ROLE_APP && server->playback &&
           (client->capabilities & BINBOT_AUDIO_CAP_NOISE_STATE) != 0) {
            binbot_audio_packet_t state;
            binbot_audio_packet_init(&state, BINBOT_AUDIO_MSG_NOISE_STATE,
                                     sizeof(state.payload.noise), 0, monotonic_ns());
            audio_playback_noise_snapshot(server->playback, &state.payload.noise);
            (void)client_send(client, &state, false);
        }
        return;
    }

    if(!client->ready) {
        fprintf(stderr, "audio: client sent data before HELLO\n");
        client_close(client);
        return;
    }
    if(packet->header.type == BINBOT_AUDIO_MSG_WAKE_DETECTED &&
       packet->header.payload_size == sizeof(packet->payload.wake) &&
       client->role == BINBOT_AUDIO_ROLE_WAKE) {
        if(server->microphone_muted) return;
        fprintf(stdout, "audio: hotword %d detected at sequence %llu\n",
                packet->payload.wake.hotword_index,
                (unsigned long long)packet->header.sequence);
        server_emit_wake(server, packet->payload.wake.hotword_index,
                         packet->header.sequence, packet->header.timestamp_ns);
        return;
    }

    if(packet->header.type == BINBOT_AUDIO_MSG_WAKE_REQUEST &&
       packet->header.payload_size == sizeof(packet->payload.wake) &&
       (client->role == BINBOT_AUDIO_ROLE_APP ||
        client->role == BINBOT_AUDIO_ROLE_DIALOG)) {
        if(server->microphone_muted) return;
        fprintf(stdout, "audio: software wake requested by role %u\n",
                (unsigned)client->role);
        server_emit_wake(server, packet->payload.wake.hotword_index,
                         packet->header.sequence, packet->header.timestamp_ns);
        return;
    }

    if(packet->header.type == BINBOT_AUDIO_MSG_DIALOG_INPUT_END &&
       packet->header.payload_size == 0 && client->role == BINBOT_AUDIO_ROLE_APP) {
        server_broadcast(server, packet, BINBOT_AUDIO_CAP_EVENTS, false);
        return;
    }

    if(packet->header.type == BINBOT_AUDIO_MSG_MUSIC_CONTROL &&
       packet->header.payload_size == sizeof(packet->payload.music_control) &&
       client->role == BINBOT_AUDIO_ROLE_APP) {
        if(packet->payload.music_control.reserved == BINBOT_AUDIO_MEDIA_PRONUNCIATION &&
           packet->payload.music_control.command == BINBOT_AUDIO_MUSIC_PLAY &&
           server->dialog_state.state >= BINBOT_DIALOG_STATE_LISTENING &&
           server->dialog_state.state <= BINBOT_DIALOG_STATE_CANCELLING) return;
        if(!audio_playback_music_control(server->playback,
                                         &packet->payload.music_control)) {
            fprintf(stderr, "audio: rejected music command %u\n",
                    (unsigned)packet->payload.music_control.command);
        }
        return;
    }

    if(packet->header.type == BINBOT_AUDIO_MSG_NOISE_CONTROL &&
       packet->header.payload_size == sizeof(packet->payload.noise) &&
       client->role == BINBOT_AUDIO_ROLE_APP) {
        bool success = audio_playback_noise_control(server->playback, &packet->payload.noise);
        if(!success)
            fprintf(stderr, "audio: rejected white noise control\n");
        binbot_audio_packet_t result;
        binbot_audio_packet_init(&result, BINBOT_AUDIO_MSG_NOISE_RESULT, sizeof(result.payload.noise),
                                 packet->header.sequence, monotonic_ns());
        audio_playback_noise_snapshot(server->playback, &result.payload.noise);
        result.payload.noise.reserved = success ? 1 : 0;
        if(send(client->fd, &result, sizeof(result.header) + sizeof(result.payload.noise),
                MSG_DONTWAIT | MSG_NOSIGNAL) < 0) client_close(client);
        return;
    }

    if(packet->header.type == BINBOT_AUDIO_MSG_MICROPHONE_SET &&
       packet->header.payload_size == sizeof(packet->payload.volume_set) &&
       client->role == BINBOT_AUDIO_ROLE_APP) {
        bool success = packet->payload.volume_set.volume <= 1 &&
                       save_microphone_state(packet->payload.volume_set.volume == 0);
        if(success) {
            server->microphone_muted = packet->payload.volume_set.volume == 0;
            server->preroll_count = 0;
            server->preroll_next = 0;
        }
        binbot_audio_packet_t result;
        binbot_audio_packet_init(&result, BINBOT_AUDIO_MSG_MICROPHONE_RESULT,
                                 sizeof(result.payload.volume_result), packet->header.sequence, monotonic_ns());
        result.payload.volume_result.volume = server->microphone_muted ? 0 : 1;
        result.payload.volume_result.success = success ? 1 : 0;
        if(send(client->fd, &result, sizeof(result.header) + sizeof(result.payload.volume_result),
                MSG_DONTWAIT | MSG_NOSIGNAL) < 0) client_close(client);
        return;
    }

    if(packet->header.type == BINBOT_AUDIO_MSG_VOLUME_SET &&
       packet->header.payload_size == sizeof(packet->payload.volume_set) &&
       client->role == BINBOT_AUDIO_ROLE_APP) {
        const char *card = getenv("BINBOT_AUDIO_MIXER_CARD");
        if(!card || !card[0]) card = "hw:0";
        const char *control = getenv("BINBOT_AUDIO_VOLUME_CONTROL");
        if(!control || !control[0]) control = "DAC LINEOUT";
        bool success = packet->payload.volume_set.volume <= 100U &&
                       configure_output_volume(card, control, packet->payload.volume_set.volume);
        if(!success) {
            fprintf(stderr, "audio: rejected volume %u%%\n",
                    (unsigned)packet->payload.volume_set.volume);
        }
        binbot_audio_packet_t result;
        binbot_audio_packet_init(&result, BINBOT_AUDIO_MSG_VOLUME_RESULT,
                                 sizeof(result.payload.volume_result), packet->header.sequence,
                                 monotonic_ns());
        result.payload.volume_result.volume = packet->payload.volume_set.volume;
        result.payload.volume_result.success = success ? 1 : 0;
        if(send(client->fd, &result, sizeof(result.header) + sizeof(result.payload.volume_result),
                MSG_DONTWAIT | MSG_NOSIGNAL) < 0) client_close(client);
        return;
    }

    if(packet->header.type == BINBOT_AUDIO_MSG_COMPONENT_COMMAND &&
       packet->header.payload_size == sizeof(packet->payload.component_command) &&
       client->role == BINBOT_AUDIO_ROLE_DIALOG) {
        server_broadcast(server, packet, BINBOT_AUDIO_CAP_REMOTE_COMMAND, false);
        return;
    }
    if(packet->header.type == BINBOT_AUDIO_MSG_COMPONENT_EVENT &&
       packet->header.payload_size == sizeof(packet->payload.component_event) &&
       client->role == BINBOT_AUDIO_ROLE_APP) {
        server_broadcast(server, packet, BINBOT_AUDIO_CAP_REMOTE_EVENTS, false);
        return;
    }
    if(packet->header.type == BINBOT_AUDIO_MSG_REMOTE_COMMAND &&
       packet->header.payload_size == sizeof(packet->payload.remote_command) &&
       client->role == BINBOT_AUDIO_ROLE_DIALOG) {
        server_broadcast(server, packet, BINBOT_AUDIO_CAP_REMOTE_COMMAND, false);
        return;
    }

    if((packet->header.type == BINBOT_AUDIO_MSG_REMOTE_STATE ||
        packet->header.type == BINBOT_AUDIO_MSG_REMOTE_RESULT) &&
       client->role == BINBOT_AUDIO_ROLE_APP) {
        const uint32_t expected = packet->header.type == BINBOT_AUDIO_MSG_REMOTE_STATE
            ? sizeof(packet->payload.remote_state)
            : sizeof(packet->payload.remote_result);
        if(packet->header.payload_size == expected) {
            server_broadcast(server, packet, BINBOT_AUDIO_CAP_REMOTE_EVENTS, false);
        }
        return;
    }

    bool diagnostic = client->role == BINBOT_AUDIO_ROLE_APP &&
                      (client->capabilities & BINBOT_AUDIO_CAP_DIAGNOSTICS);
    if(client->role != BINBOT_AUDIO_ROLE_DIALOG && !diagnostic) return;

    if(packet->header.type == BINBOT_AUDIO_MSG_CALENDAR_STATE &&
       client->role == BINBOT_AUDIO_ROLE_DIALOG &&
       packet->header.payload_size == sizeof(packet->payload.calendar_state) &&
       packet->payload.calendar_state.event_count <= BINBOT_AUDIO_CALENDAR_EVENT_CAPACITY) {
        server->calendar_state = packet->payload.calendar_state;
        server->calendar_state_valid = true;
        server_broadcast(server, packet, BINBOT_AUDIO_CAP_CALENDAR_STATE, false);
        return;
    }

    if(packet->header.type == BINBOT_AUDIO_MSG_PLAYBACK_BEGIN &&
       packet->header.payload_size == sizeof(packet->payload.playback_begin)) {
        if(diagnostic && server->dialog_state.state >= BINBOT_DIALOG_STATE_LISTENING &&
           server->dialog_state.state <= BINBOT_DIALOG_STATE_CANCELLING) return;
        if(!audio_playback_begin(server->playback, &packet->payload.playback_begin)) {
            fprintf(stderr, "audio: rejected invalid playback begin\n");
        }
        else if(diagnostic) {
            server->diagnostic_playback = true;
            memcpy(server->diagnostic_stream_id, packet->payload.playback_begin.stream_id, 16);
        }
        return;
    }
    if(packet->header.type == BINBOT_AUDIO_MSG_PLAYBACK_FRAME &&
       packet->header.payload_size == sizeof(packet->payload.playback_frame)) {
        if(!audio_playback_enqueue(server->playback, &packet->payload.playback_frame)) {
            fprintf(stderr, "audio: rejected playback frame (stale or queue full, queued=%zu)\n",
                    audio_playback_queued(server->playback));
        }
        return;
    }
    if(packet->header.type == BINBOT_AUDIO_MSG_PLAYBACK_END &&
       packet->header.payload_size == sizeof(packet->payload.playback_control)) {
        (void)audio_playback_end(server->playback,
                                 packet->payload.playback_control.stream_id);
        return;
    }
    if(packet->header.type == BINBOT_AUDIO_MSG_PLAYBACK_CANCEL &&
       packet->header.payload_size == sizeof(packet->payload.playback_control)) {
        (void)audio_playback_cancel(server->playback,
                                    packet->payload.playback_control.stream_id);
        return;
    }
    if(client->role != BINBOT_AUDIO_ROLE_DIALOG) return;
    if(packet->header.type == BINBOT_AUDIO_MSG_DIALOG_STATE &&
       packet->header.payload_size == sizeof(packet->payload.dialog_state) &&
       packet->payload.dialog_state.state <= BINBOT_DIALOG_STATE_ERROR) {
        server->dialog_state = packet->payload.dialog_state;
        server_broadcast(server, packet, BINBOT_AUDIO_CAP_DIALOG_STATE, false);
    }
    if(packet->header.type == BINBOT_AUDIO_MSG_DIALOG_TEXT &&
       packet->header.payload_size == sizeof(packet->payload.dialog_text) &&
       binbot_audio_dialog_text_is_valid(&packet->payload.dialog_text)) {
        server_broadcast(server, packet, BINBOT_AUDIO_CAP_DIALOG_TEXT, false);
    }
}

static void server_receive_messages(audio_server_t *server)
{
    for(size_t i = 0; i < AUDIO_CLIENT_CAPACITY; ++i) {
        audio_client_slot_t *client = &server->clients[i];
        while(client->fd >= 0) {
            binbot_audio_packet_t packet;
            ssize_t size = recv(client->fd, &packet, sizeof(packet), MSG_DONTWAIT);
            if(size > 0) {
                if(!binbot_audio_packet_is_valid(&packet, (uint32_t)size)) {
                    fprintf(stderr, "audio: malformed packet, closing client\n");
                    client_close(client);
                    break;
                }
                server_handle_packet(server, client, &packet);
                continue;
            }
            if(size == 0) {
                client_close(client);
                break;
            }
            if(errno == EAGAIN || errno == EWOULDBLOCK) break;
            if(errno == EINTR) continue;
            client_close(client);
            break;
        }
    }
}

static void server_service(audio_server_t *server)
{
    server_accept_clients(server);
    server_receive_messages(server);
    if(server->diagnostic_playback && !diagnostics_connected(server)) {
        (void)audio_playback_cancel(server->playback, server->diagnostic_stream_id);
        server->diagnostic_playback = false;
    }
    uint64_t now = monotonic_ns();
    if(server->playback && now - server->noise_last_check_ns >= 100000000ULL) {
        server->noise_last_check_ns = now;
        binbot_audio_packet_t packet;
        binbot_audio_packet_init(&packet, BINBOT_AUDIO_MSG_NOISE_STATE,
                                 sizeof(packet.payload.noise), 0, now);
        audio_playback_noise_snapshot(server->playback, &packet.payload.noise);
        if(memcmp(&packet.payload.noise, &server->noise_state, sizeof(server->noise_state))) {
            server->noise_state = packet.payload.noise;
            for(size_t i = 0; i < AUDIO_CLIENT_CAPACITY; ++i) {
                audio_client_slot_t *client = &server->clients[i];
                if(client->fd >= 0 && client->ready && client->role == BINBOT_AUDIO_ROLE_APP &&
                   (client->capabilities & BINBOT_AUDIO_CAP_NOISE_STATE) != 0)
                    (void)client_send(client, &packet, false);
            }
        }
    }
    bool dialog_connected = false;
    for(size_t i = 0; i < AUDIO_CLIENT_CAPACITY; ++i) {
        audio_client_slot_t *client = &server->clients[i];
        if(client->fd >= 0 && client->ready &&
           client->role == BINBOT_AUDIO_ROLE_DIALOG) {
            dialog_connected = true;
            break;
        }
    }
    if(server->dialog_connected && !dialog_connected) {
        (void)audio_playback_cancel(server->playback, NULL);
        binbot_audio_packet_t state;
        binbot_audio_packet_init(&state, BINBOT_AUDIO_MSG_DIALOG_STATE,
                                 sizeof(state.payload.dialog_state), 0, monotonic_ns());
        server->dialog_state = state.payload.dialog_state;
        server_broadcast(server, &state, BINBOT_AUDIO_CAP_DIALOG_STATE, false);
    }
    server->dialog_connected = dialog_connected;
}

static void preroll_push(audio_server_t *server, const binbot_audio_packet_t *packet)
{
    server->preroll[server->preroll_next] = *packet;
    server->preroll_next = (server->preroll_next + 1U) % AUDIO_PREROLL_FRAMES;
    if(server->preroll_count < AUDIO_PREROLL_FRAMES) server->preroll_count++;
}

static snd_pcm_t *capture_open(const char *device)
{
    snd_pcm_t *pcm = NULL;
    int error = snd_pcm_open(&pcm, device, SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
    if(error < 0) {
        fprintf(stderr, "audio: cannot open capture device %s: %s\n",
                device, snd_strerror(error));
        return NULL;
    }

    snd_pcm_hw_params_t *params;
    snd_pcm_hw_params_alloca(&params);
    unsigned rate = BINBOT_AUDIO_SAMPLE_RATE;
    snd_pcm_uframes_t period = BINBOT_AUDIO_FRAME_SAMPLES;
    snd_pcm_uframes_t buffer = BINBOT_AUDIO_FRAME_SAMPLES * 4U;

    if((error = snd_pcm_hw_params_any(pcm, params)) < 0 ||
       (error = snd_pcm_hw_params_set_access(pcm, params,
                                             SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
       (error = snd_pcm_hw_params_set_format(pcm, params, SND_PCM_FORMAT_S16_LE)) < 0 ||
       (error = snd_pcm_hw_params_set_channels(pcm, params, AUDIO_CAPTURE_CHANNELS)) < 0 ||
       (error = snd_pcm_hw_params_set_rate_near(pcm, params, &rate, NULL)) < 0 ||
       (error = snd_pcm_hw_params_set_period_size_near(pcm, params, &period, NULL)) < 0 ||
       (error = snd_pcm_hw_params_set_buffer_size_near(pcm, params, &buffer)) < 0 ||
       (error = snd_pcm_hw_params(pcm, params)) < 0) {
        fprintf(stderr, "audio: cannot configure %s: %s\n", device, snd_strerror(error));
        snd_pcm_close(pcm);
        return NULL;
    }
    if(rate != BINBOT_AUDIO_SAMPLE_RATE) {
        fprintf(stderr, "audio: device selected unsupported rate %u\n", rate);
        snd_pcm_close(pcm);
        return NULL;
    }
    (void)audio_pcm_clock_enable(pcm);
    if((error = snd_pcm_prepare(pcm)) < 0) {
        fprintf(stderr, "audio: cannot prepare %s: %s\n", device, snd_strerror(error));
        snd_pcm_close(pcm);
        return NULL;
    }

    fprintf(stdout, "audio: capturing %u Hz S16_LE stereo from %s, period=%lu\n",
            rate, device, (unsigned long)period);
    return pcm;
}

static int capture_recover(snd_pcm_t *pcm, int error)
{
    if(error == -EPIPE) {
        fprintf(stderr, "audio: capture overrun, recovering\n");
        return snd_pcm_prepare(pcm);
    }
    if(error == -ESTRPIPE) {
        while(running && (error = snd_pcm_resume(pcm)) == -EAGAIN) usleep(10000);
        if(error < 0) error = snd_pcm_prepare(pcm);
        return error;
    }
    return error;
}

static int16_t clamp_sample(float value)
{
    if(value > 32767.0f) return 32767;
    if(value < -32768.0f) return -32768;
    return (int16_t)value;
}

static void frontend_process(audio_frontend_t *frontend,
                             const int16_t *stereo,
                             int16_t *processed_stereo,
                             int16_t *mono)
{
    const float highpass_alpha = 0.97f;
    for(size_t i = 0; i < BINBOT_AUDIO_FRAME_SAMPLES; ++i) {
        const int16_t *frame = &stereo[i * AUDIO_CAPTURE_CHANNELS];
        float filtered[AUDIO_CAPTURE_CHANNELS];
        for(size_t channel = 0; channel < AUDIO_CAPTURE_CHANNELS; ++channel) {
            float input = (float)frame[channel];
            float output = input;
            if(frontend->highpass) {
                audio_channel_filter_t *filter = &frontend->channels[channel];
                output = input - filter->previous_input +
                         highpass_alpha * filter->previous_output;
                filter->previous_input = input;
                filter->previous_output = output;
            }
            filtered[channel] = output;
            processed_stereo[i * AUDIO_CAPTURE_CHANNELS + channel] =
                clamp_sample(output * frontend->gain);
        }

        float input;
        if(frontend->source == AUDIO_SOURCE_LEFT) {
            input = filtered[0];
        } else if(frontend->source == AUDIO_SOURCE_RIGHT) {
            input = filtered[1];
        } else {
            input = (filtered[0] + filtered[1]) * 0.5f;
        }
        mono[i] = clamp_sample(input * frontend->gain);
    }
}

static binbot_audio_level_t measure_level(const int16_t *samples)
{
    uint64_t sum_squares = 0;
    uint32_t peak = 0;
    for(size_t i = 0; i < BINBOT_AUDIO_FRAME_SAMPLES; ++i) {
        int32_t value = samples[i];
        uint32_t magnitude = (uint32_t)(value < 0 ? -value : value);
        if(magnitude > peak) peak = magnitude;
        sum_squares += (uint64_t)((int64_t)value * value);
    }
    double mean = (double)sum_squares / BINBOT_AUDIO_FRAME_SAMPLES;
    uint32_t rms = (uint32_t)sqrt(mean);
    if(rms > 32767U) rms = 32767U;
    if(peak > 32767U) peak = 32767U;
    return (binbot_audio_level_t){
        .rms = (uint16_t)rms,
        .peak = (uint16_t)peak,
        .scale = 32767,
    };
}

static float environment_float(const char *name, float fallback)
{
    const char *text = getenv(name);
    if(!text || !text[0]) return fallback;
    char *end = NULL;
    float value = strtof(text, &end);
    return end && *end == '\0' ? value : fallback;
}

static int environment_int(const char *name, int fallback)
{
    const char *text = getenv(name);
    if(!text || !text[0]) return fallback;
    char *end = NULL;
    long value = strtol(text, &end, 10);
    return end && *end == '\0' ? (int)value : fallback;
}

static const char *audio_source_name(const audio_frontend_t *frontend)
{
    if(frontend->source == AUDIO_SOURCE_LEFT) return "left";
    if(frontend->source == AUDIO_SOURCE_RIGHT) return "right";
    return "mix";
}

static bool configure_adc_mode(const char *card, const char *mode)
{
    if(!mode || !mode[0] || strcmp(mode, "keep") == 0) return true;

    snd_mixer_t *mixer = NULL;
    int error = snd_mixer_open(&mixer, 0);
    if(error < 0) {
        fprintf(stderr, "audio: cannot open mixer: %s\n", snd_strerror(error));
        return false;
    }

    bool configured = false;
    error = snd_mixer_attach(mixer, card);
    if(error < 0) {
        fprintf(stderr, "audio: cannot attach mixer %s: %s\n",
                card, snd_strerror(error));
        goto done;
    }
    if((error = snd_mixer_selem_register(mixer, NULL, NULL)) < 0 ||
       (error = snd_mixer_load(mixer)) < 0) {
        fprintf(stderr, "audio: cannot load mixer %s: %s\n",
                card, snd_strerror(error));
        goto done;
    }

    snd_mixer_selem_id_t *identifier;
    snd_mixer_selem_id_alloca(&identifier);
    snd_mixer_selem_id_set_index(identifier, 0);
    snd_mixer_selem_id_set_name(identifier, "ADC Mode");
    snd_mixer_elem_t *element = snd_mixer_find_selem(mixer, identifier);
    if(!element || !snd_mixer_selem_is_enumerated(element)) {
        fprintf(stderr, "audio: mixer %s has no enumerated ADC Mode control\n", card);
        goto done;
    }

    int item_count = snd_mixer_selem_get_enum_items(element);
    for(int item = 0; item < item_count; ++item) {
        char name[64] = {0};
        if(snd_mixer_selem_get_enum_item_name(
               element, (unsigned)item, sizeof(name), name) < 0 ||
           strcmp(name, mode) != 0) {
            continue;
        }
        error = snd_mixer_selem_set_enum_item(
            element, SND_MIXER_SCHN_FRONT_LEFT, (unsigned)item);
        if(error < 0) {
            fprintf(stderr, "audio: cannot set ADC Mode=%s: %s\n",
                    mode, snd_strerror(error));
            goto done;
        }
        fprintf(stdout, "audio: configured ADC Mode=%s on %s\n", mode, card);
        configured = true;
        break;
    }
    if(!configured) {
        fprintf(stderr, "audio: ADC Mode does not support value '%s'\n", mode);
    }

done:
    snd_mixer_close(mixer);
    return configured;
}

static bool configure_mic_gain(const char *card, const char *control_name, long gain)
{
    snd_mixer_t *mixer = NULL;
    int error = snd_mixer_open(&mixer, 0);
    if(error < 0) {
        fprintf(stderr, "audio: cannot open mixer: %s\n", snd_strerror(error));
        return false;
    }

    bool configured = false;
    error = snd_mixer_attach(mixer, card);
    if(error < 0 ||
       (error = snd_mixer_selem_register(mixer, NULL, NULL)) < 0 ||
       (error = snd_mixer_load(mixer)) < 0) {
        fprintf(stderr, "audio: cannot load mixer %s: %s\n",
                card, snd_strerror(error));
        goto done;
    }

    snd_mixer_selem_id_t *identifier;
    snd_mixer_selem_id_alloca(&identifier);
    snd_mixer_selem_id_set_index(identifier, 0);
    snd_mixer_selem_id_set_name(identifier, control_name);
    snd_mixer_elem_t *element = snd_mixer_find_selem(mixer, identifier);
    if(!element) {
        fprintf(stderr, "audio: mixer %s has no %s control\n", card, control_name);
        goto done;
    }

    if(snd_mixer_selem_has_common_volume(element) ||
       snd_mixer_selem_has_playback_volume(element)) {
        error = snd_mixer_selem_set_playback_volume_all(element, gain);
    } else if(snd_mixer_selem_has_capture_volume(element)) {
        error = snd_mixer_selem_set_capture_volume_all(element, gain);
    } else {
        fprintf(stderr, "audio: mixer control %s has no writable volume\n", control_name);
        goto done;
    }
    if(error < 0) {
        fprintf(stderr, "audio: cannot set %s=%ld: %s\n",
                control_name, gain, snd_strerror(error));
        goto done;
    }
    fprintf(stdout, "audio: configured %s=%ld on %s\n",
            control_name, gain, card);
    configured = true;

done:
    snd_mixer_close(mixer);
    return configured;
}

static bool configure_output_volume(const char *card, const char *control_name,
                                    uint8_t percent)
{
    snd_mixer_t *mixer = NULL;
    int error = snd_mixer_open(&mixer, 0);
    if(error < 0) {
        fprintf(stderr, "audio: cannot open mixer for volume: %s\n",
                snd_strerror(error));
        return false;
    }

    bool configured = false;
    error = snd_mixer_attach(mixer, card);
    if(error < 0 ||
       (error = snd_mixer_selem_register(mixer, NULL, NULL)) < 0 ||
       (error = snd_mixer_load(mixer)) < 0) {
        fprintf(stderr, "audio: cannot load mixer %s for volume: %s\n",
                card, snd_strerror(error));
        goto done;
    }

    snd_mixer_selem_id_t *identifier;
    snd_mixer_selem_id_alloca(&identifier);
    snd_mixer_selem_id_set_index(identifier, 0);
    snd_mixer_selem_id_set_name(identifier, control_name);
    snd_mixer_elem_t *element = snd_mixer_find_selem(mixer, identifier);
    if(!element || !snd_mixer_selem_has_playback_volume(element)) {
        fprintf(stderr, "audio: mixer %s has no playback volume control %s\n",
                card, control_name);
        goto done;
    }

    long minimum = 0;
    long maximum = 0;
    if(snd_mixer_selem_get_playback_volume_range(element, &minimum, &maximum) < 0 ||
       maximum < minimum) {
        fprintf(stderr, "audio: cannot read playback volume range for %s\n",
                control_name);
        goto done;
    }
    long gain = minimum + (maximum - minimum) * (long)percent / 100L;
    error = snd_mixer_selem_set_playback_volume_all(element, gain);
    if(error < 0) {
        fprintf(stderr, "audio: cannot set %s=%u%%: %s\n",
                control_name, (unsigned)percent, snd_strerror(error));
        goto done;
    }
    fprintf(stdout, "audio: configured %s=%u%% (%ld..%ld) on %s\n",
            control_name, (unsigned)percent, minimum, maximum, card);
    configured = true;

done:
    snd_mixer_close(mixer);
    return configured;
}

static void frontend_configure_source(audio_frontend_t *frontend)
{
    const char *source = getenv("BINBOT_AUDIO_SOURCE");
    if(source && source[0]) {
        if(strcmp(source, "mix") == 0) {
            frontend->source = AUDIO_SOURCE_MIX;
        } else if(strcmp(source, "left") == 0) {
            frontend->source = AUDIO_SOURCE_LEFT;
        } else if(strcmp(source, "right") == 0) {
            frontend->source = AUDIO_SOURCE_RIGHT;
        } else {
            fprintf(stderr, "audio: invalid source '%s', using mix\n", source);
            frontend->source = AUDIO_SOURCE_MIX;
        }
        return;
    }

    /* Keep the old channel selector working for existing deployments. */
    const char *legacy_channel = getenv("BINBOT_AUDIO_CHANNEL");
    if(!legacy_channel || !legacy_channel[0]) {
        frontend->source = AUDIO_SOURCE_MIX;
        return;
    }

    int channel = environment_int("BINBOT_AUDIO_CHANNEL", -1);
    if(channel == 0) {
        frontend->source = AUDIO_SOURCE_LEFT;
    } else if(channel == 1) {
        frontend->source = AUDIO_SOURCE_RIGHT;
    } else {
        fprintf(stderr, "audio: invalid legacy channel '%s', using mix\n", legacy_channel);
        frontend->source = AUDIO_SOURCE_MIX;
    }
}

int main(void)
{
    signal(SIGINT, stop_requested);
    signal(SIGTERM, stop_requested);
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    const char *socket_path = getenv("BINBOT_AUDIO_SOCKET");
    const char *capture_device = getenv("BINBOT_AUDIO_DEVICE");
    if(!capture_device || !capture_device[0]) capture_device = "hw:0,0";
    const char *mixer_card = getenv("BINBOT_AUDIO_MIXER_CARD");
    if(!mixer_card || !mixer_card[0]) mixer_card = "hw:0";
    const char *adc_mode = getenv("BINBOT_AUDIO_ADC_MODE");
    if(!adc_mode || !adc_mode[0]) adc_mode = "SingadcLR";
    if(!configure_adc_mode(mixer_card, adc_mode)) return EXIT_FAILURE;
    long mic_gain = environment_int("BINBOT_AUDIO_MIC_GAIN", 1);
    if(mic_gain < 1 || mic_gain > 3) {
        fprintf(stderr, "audio: BINBOT_AUDIO_MIC_GAIN must be in range 1..3\n");
        return EXIT_FAILURE;
    }
    if(!configure_mic_gain(mixer_card, "ADC MIC Left Gain", mic_gain) ||
       !configure_mic_gain(mixer_card, "ADC MIC Right Gain", mic_gain)) {
        return EXIT_FAILURE;
    }
    audio_frontend_t frontend = {
        .source = AUDIO_SOURCE_MIX,
        .gain = environment_float("BINBOT_AUDIO_GAIN", 1.0f),
        .highpass = environment_int("BINBOT_AUDIO_HIGHPASS", 1) != 0,
    };
    frontend_configure_source(&frontend);
    if(frontend.gain <= 0.0f || frontend.gain > 16.0f) {
        fprintf(stderr, "audio: invalid gain %.2f, using 1.0\n", frontend.gain);
        frontend.gain = 1.0f;
    }

    audio_server_t server;
    if(!server_start(&server, socket_path)) return EXIT_FAILURE;
    audio_playback_t playback;
    const char *playback_device = getenv("BINBOT_AUDIO_PLAYBACK_DEVICE");
    const char *wake_sound = getenv("BINBOT_WAKE_SOUND");
    float wake_volume = environment_float("BINBOT_WAKE_SOUND_VOLUME", 0.35f);
    if(!isfinite(wake_volume) || wake_volume < 0.0f || wake_volume > 1.0f) {
        fprintf(stderr, "audio: invalid wake sound volume %.2f, using 0.35\n", wake_volume);
        wake_volume = 0.35f;
    }
    long wake_max_ms = environment_int("BINBOT_WAKE_SOUND_MAX_MS", 800);
    if(wake_max_ms < 100 || wake_max_ms > 5000) {
        fprintf(stderr,
                "audio: BINBOT_WAKE_SOUND_MAX_MS must be in range 100..5000\n");
        return EXIT_FAILURE;
    }
    float dialog_volume = environment_float("BINBOT_AUDIO_DIALOG_VOLUME", 0.25f);
    if(!isfinite(dialog_volume) || dialog_volume < 0.0f || dialog_volume > 1.0f) {
        fprintf(stderr, "audio: invalid dialog volume %.2f, using 0.25\n", dialog_volume);
        dialog_volume = 0.25f;
    }
    int echo_delay_ms = environment_int("BINBOT_AUDIO_AEC_DELAY_MS", 40);
    if(echo_delay_ms < 0 || echo_delay_ms > 500) {
        fprintf(stderr, "audio: BINBOT_AUDIO_AEC_DELAY_MS must be in range 0..500\n");
        server_stop(&server);
        return EXIT_FAILURE;
    }
    audio_echo_t *echo = NULL;
    if(environment_int("BINBOT_AUDIO_AEC", 1)) {
        echo = audio_echo_create((unsigned)echo_delay_ms);
        if(!echo) {
            fprintf(stderr, "audio: cannot initialize acoustic echo cancellation\n");
            server_stop(&server);
            return EXIT_FAILURE;
        }
    }
    if(!audio_playback_start(&playback, playback_device, wake_sound, wake_volume,
                             (uint32_t)wake_max_ms, dialog_volume, echo)) {
        fprintf(stderr, "audio: failed to start playback worker\n");
        audio_echo_destroy(echo);
        server_stop(&server);
        return EXIT_FAILURE;
    }
    server.playback = &playback;
    fprintf(stdout, "audio: frontend source=%s gain=%.2f highpass=%s\n",
            audio_source_name(&frontend), frontend.gain,
            frontend.highpass ? "on" : "off");
    fprintf(stdout, "audio: wake sound volume=%.2f max_ms=%ld\n",
            wake_volume, wake_max_ms);
    fprintf(stdout, "audio: dialog volume=%.2f\n", dialog_volume);
    fprintf(stdout, "audio: AEC=%s delay_ms=%d\n",
            echo ? audio_echo_engine() : "off", echo_delay_ms);

    snd_pcm_t *pcm = NULL;
    int16_t stereo[BINBOT_AUDIO_FRAME_SAMPLES * AUDIO_CAPTURE_CHANNELS];
    int16_t processed_stereo[BINBOT_AUDIO_FRAME_SAMPLES * AUDIO_CAPTURE_CHANNELS];
    size_t captured_frames = 0;
    uint64_t sequence = 0;

    while(running) {
        server_service(&server);
        if(!pcm) {
            pcm = capture_open(capture_device);
            if(!pcm) {
                for(unsigned i = 0; running && i < 100U; ++i) {
                    server_service(&server);
                    usleep(10000);
                }
                continue;
            }
            captured_frames = 0;
        }

        snd_pcm_sframes_t frames = snd_pcm_readi(
            pcm,
            stereo + captured_frames * AUDIO_CAPTURE_CHANNELS,
            BINBOT_AUDIO_FRAME_SAMPLES - captured_frames);
        if(frames == -EAGAIN) {
            usleep(2000);
            continue;
        }
        if(frames < 0) {
            int recovery = capture_recover(pcm, (int)frames);
            if(recovery < 0) {
                fprintf(stderr, "audio: capture failed: %s; reopening\n",
                        snd_strerror(recovery));
                snd_pcm_close(pcm);
                pcm = NULL;
            }
            captured_frames = 0;
            continue;
        }
        if(frames == 0) continue;
        captured_frames += (size_t)frames;
        if(captured_frames < BINBOT_AUDIO_FRAME_SAMPLES) continue;
        captured_frames = 0;

        const uint64_t frame_sequence = sequence++;
        if(server.microphone_muted) memset(stereo, 0, sizeof(stereo));
        const uint64_t timestamp_ns = monotonic_ns();
        binbot_audio_packet_t mono_packet;
        binbot_audio_packet_init(&mono_packet, BINBOT_AUDIO_MSG_PCM,
                                 sizeof(mono_packet.payload.pcm),
                                 frame_sequence, timestamp_ns);
        frontend_process(&frontend, stereo, processed_stereo,
                         mono_packet.payload.pcm.samples);
        snd_pcm_sframes_t capture_delay = 0;
        uint64_t position_ns;
        (void)audio_pcm_clock_read(pcm, monotonic_ns(), &position_ns, &capture_delay);
        if(capture_delay < 0) capture_delay = 0;
        uint64_t captured_ns = ((uint64_t)capture_delay + BINBOT_AUDIO_FRAME_SAMPLES) *
            1000000000ULL / BINBOT_AUDIO_SAMPLE_RATE;
        int16_t render_reference[BINBOT_AUDIO_FRAME_SAMPLES];
        audio_echo_process_with_reference(echo, mono_packet.payload.pcm.samples,
            position_ns > captured_ns ? position_ns - captured_ns : 0,
            render_reference);
        preroll_push(&server, &mono_packet);
        server_broadcast(&server, &mono_packet, BINBOT_AUDIO_CAP_PCM_LIVE, true);

        binbot_audio_packet_t stereo_packet;
        binbot_audio_packet_init(&stereo_packet, BINBOT_AUDIO_MSG_PCM_STEREO,
                                 sizeof(stereo_packet.payload.pcm_stereo),
                                 frame_sequence, timestamp_ns);
        memcpy(stereo_packet.payload.pcm_stereo.samples, processed_stereo,
               sizeof(stereo_packet.payload.pcm_stereo.samples));
        server_broadcast(&server, &stereo_packet,
                         BINBOT_AUDIO_CAP_PCM_STEREO_LIVE, true);

        binbot_audio_packet_t frontend_packet;
        binbot_audio_packet_init(&frontend_packet, BINBOT_AUDIO_MSG_PCM_FRONTEND,
                                 sizeof(frontend_packet.payload.pcm_frontend),
                                 frame_sequence, timestamp_ns);
        for(size_t i = 0; i < BINBOT_AUDIO_FRAME_SAMPLES; ++i) {
            frontend_packet.payload.pcm_frontend.samples[i * 3] = stereo[i * AUDIO_CAPTURE_CHANNELS];
            frontend_packet.payload.pcm_frontend.samples[i * 3 + 1] = stereo[i * AUDIO_CAPTURE_CHANNELS + 1];
            frontend_packet.payload.pcm_frontend.samples[i * 3 + 2] = render_reference[i];
        }
        server_broadcast(&server, &frontend_packet, BINBOT_AUDIO_CAP_PCM_FRONTEND, true);

        if(sequence % AUDIO_LEVEL_INTERVAL_FRAMES == 0) {
            binbot_audio_packet_t level;
            binbot_audio_packet_init(&level, BINBOT_AUDIO_MSG_LEVEL,
                                     sizeof(level.payload.level), sequence,
                                     mono_packet.header.timestamp_ns);
            level.payload.level = measure_level(mono_packet.payload.pcm.samples);
            server_broadcast(&server, &level, BINBOT_AUDIO_CAP_LEVEL, false);
        }
        server_service(&server);
    }

    if(pcm) snd_pcm_close(pcm);
    audio_playback_stop(&playback);
    audio_echo_destroy(echo);
    server_stop(&server);
    fprintf(stdout, "audio: stopped cleanly\n");
    return EXIT_SUCCESS;
}
