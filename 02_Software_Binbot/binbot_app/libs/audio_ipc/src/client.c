#include "binbot/audio/client.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static void close_socket(binbot_audio_client_t *client)
{
    if(client->fd >= 0) close(client->fd);
    client->fd = -1;
}

static bool set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if(flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return false;
    int descriptor_flags = fcntl(fd, F_GETFD, 0);
    if(descriptor_flags >= 0) fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC);
    return true;
}

void binbot_audio_client_init(binbot_audio_client_t *client,
                              binbot_audio_client_role_t role,
                              uint32_t capabilities,
                              uint32_t preroll_ms,
                              const char *socket_path)
{
    memset(client, 0, sizeof(*client));
    client->fd = -1;
    client->role = role;
    client->capabilities = capabilities;
    client->preroll_ms = preroll_ms;
    snprintf(client->socket_path, sizeof(client->socket_path), "%s",
             socket_path && socket_path[0] ? socket_path : BINBOT_AUDIO_DEFAULT_SOCKET);
}

void binbot_audio_client_close(binbot_audio_client_t *client)
{
    if(client) close_socket(client);
}

bool binbot_audio_client_connect(binbot_audio_client_t *client, uint64_t monotonic_ms)
{
    if(!client) return false;
    if(client->fd >= 0) return true;
    if(client->last_connect_attempt_ms != 0 &&
       monotonic_ms - client->last_connect_attempt_ms < 1000U) {
        return false;
    }
    client->last_connect_attempt_ms = monotonic_ms ? monotonic_ms : 1U;

    int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if(fd < 0) return false;

    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if(strlen(client->socket_path) >= sizeof(address.sun_path)) {
        close(fd);
        return false;
    }
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", client->socket_path);
    socklen_t address_size = (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
                                        strlen(address.sun_path) + 1U);
    if(connect(fd, (const struct sockaddr *)&address, address_size) != 0 ||
       !set_nonblocking(fd)) {
        close(fd);
        return false;
    }

    client->fd = fd;
    binbot_audio_packet_t hello;
    binbot_audio_packet_init(&hello, BINBOT_AUDIO_MSG_HELLO,
                             sizeof(hello.payload.hello), 0, 0);
    hello.payload.hello.role = (uint16_t)client->role;
    hello.payload.hello.capabilities = client->capabilities;
    hello.payload.hello.preroll_ms = client->preroll_ms;
    if(!binbot_audio_client_send(client, &hello)) {
        close_socket(client);
        return false;
    }
    return true;
}

int binbot_audio_client_receive(binbot_audio_client_t *client,
                                binbot_audio_packet_t *packet)
{
    if(!client || !packet || client->fd < 0) return -1;
    ssize_t size = recv(client->fd, packet, sizeof(*packet), MSG_DONTWAIT);
    if(size > 0) {
        if(!binbot_audio_packet_is_valid(packet, (uint32_t)size)) {
            fprintf(stderr, "audio-ipc: rejected malformed packet\n");
            close_socket(client);
            return -1;
        }
        return 1;
    }
    if(size == 0) {
        close_socket(client);
        return -1;
    }
    if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
    close_socket(client);
    return -1;
}

bool binbot_audio_client_send(binbot_audio_client_t *client,
                              const binbot_audio_packet_t *packet)
{
    if(!client || !packet || client->fd < 0) return false;
    uint32_t packet_size = binbot_audio_packet_size(packet);
    ssize_t sent = send(client->fd, packet, packet_size, MSG_DONTWAIT | MSG_NOSIGNAL);
    if(sent == (ssize_t)packet_size) return true;
    if(sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        return false;
    }
    close_socket(client);
    return false;
}

bool binbot_audio_client_music_control(binbot_audio_client_t *client,
                                       binbot_audio_music_command_t command,
                                       uint32_t position_ms,
                                       const char *path)
{
    if(!client || client->fd < 0 || command < BINBOT_AUDIO_MUSIC_PLAY ||
       command > BINBOT_AUDIO_MUSIC_SEEK) return false;

    binbot_audio_packet_t packet;
    binbot_audio_packet_init(&packet, BINBOT_AUDIO_MSG_MUSIC_CONTROL,
                             sizeof(packet.payload.music_control), 0, 0);
    packet.payload.music_control.command = (uint16_t)command;
    packet.payload.music_control.position_ms = position_ms;
    if(path && path[0]) {
        snprintf(packet.payload.music_control.path,
                 sizeof(packet.payload.music_control.path), "%s", path);
    }
    return binbot_audio_client_send(client, &packet);
}

bool binbot_audio_client_pronunciation(binbot_audio_client_t *client,
                                       bool play, const char *path)
{
    if(!client || client->fd < 0 || (play && (!path || !path[0] || strlen(path) >= 256)))
        return false;
    binbot_audio_packet_t packet;
    binbot_audio_packet_init(&packet, BINBOT_AUDIO_MSG_MUSIC_CONTROL,
                             sizeof(packet.payload.music_control), 0, 0);
    packet.payload.music_control.command = play ? BINBOT_AUDIO_MUSIC_PLAY : BINBOT_AUDIO_MUSIC_STOP;
    packet.payload.music_control.reserved = BINBOT_AUDIO_MEDIA_PRONUNCIATION;
    if(play) snprintf(packet.payload.music_control.path, sizeof(packet.payload.music_control.path), "%s", path);
    return binbot_audio_client_send(client, &packet);
}

bool binbot_audio_client_set_volume(binbot_audio_client_t *client, uint8_t volume)
{
    return binbot_audio_client_request_volume(client, volume, 0);
}

bool binbot_audio_client_request_volume(binbot_audio_client_t *client, uint8_t volume,
                                        uint64_t request_sequence)
{
    if(!client || client->fd < 0 || volume > 100U) return false;

    binbot_audio_packet_t packet;
    binbot_audio_packet_init(&packet, BINBOT_AUDIO_MSG_VOLUME_SET,
                             sizeof(packet.payload.volume_set), request_sequence, 0);
    packet.payload.volume_set.volume = volume;
    return binbot_audio_client_send(client, &packet);
}

int binbot_audio_client_fd(const binbot_audio_client_t *client)
{
    return client ? client->fd : -1;
}
