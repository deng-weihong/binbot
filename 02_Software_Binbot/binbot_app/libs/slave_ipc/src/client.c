#include "binbot/slave/ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static void close_socket(binbot_slave_client_t *client)
{
    if(client->fd >= 0) close(client->fd);
    client->fd = -1;
    client->pending_packet_count = 0;
}

static bool set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if(flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return false;
    int descriptor_flags = fcntl(fd, F_GETFD, 0);
    if(descriptor_flags >= 0) (void)fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC);
    return true;
}

static uint64_t monotonic_ms(void)
{
    struct timespec value;
    if(clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return (uint64_t)value.tv_sec * 1000ULL + (uint64_t)value.tv_nsec / 1000000ULL;
}

static bool wait_fd(int fd, short events, uint32_t timeout_ms)
{
    struct pollfd descriptor = {.fd = fd, .events = events};
    int timeout = timeout_ms > (uint32_t)INT32_MAX ? INT32_MAX : (int)timeout_ms;
    for(;;) {
        int result = poll(&descriptor, 1, timeout);
        if(result > 0) return (descriptor.revents & (events | POLLERR | POLLHUP)) != 0;
        if(result == 0) return false;
        if(errno != EINTR) return false;
    }
}

size_t binbot_slave_ipc_packet_size(const binbot_slave_ipc_packet_t *packet)
{
    if(!packet || packet->payload_length > BINBOT_SLAVE_IPC_MAX_PAYLOAD) return 0;
    return offsetof(binbot_slave_ipc_packet_t, payload) + packet->payload_length;
}

bool binbot_slave_ipc_packet_is_valid(const binbot_slave_ipc_packet_t *packet,
                                      size_t received_size)
{
    size_t header_size = offsetof(binbot_slave_ipc_packet_t, payload);
    if(!packet || received_size < header_size || packet->magic != BINBOT_SLAVE_IPC_MAGIC ||
       packet->version != BINBOT_SLAVE_IPC_VERSION ||
       packet->payload_length > BINBOT_SLAVE_IPC_MAX_PAYLOAD) {
        return false;
    }
    return received_size == binbot_slave_ipc_packet_size(packet);
}

void binbot_slave_ipc_packet_init(binbot_slave_ipc_packet_t *packet,
                                  binbot_slave_ipc_message_type_t type,
                                  uint8_t command,
                                  uint32_t request_id)
{
    if(!packet) return;
    memset(packet, 0, sizeof(*packet));
    packet->magic = BINBOT_SLAVE_IPC_MAGIC;
    packet->version = BINBOT_SLAVE_IPC_VERSION;
    packet->type = (uint16_t)type;
    packet->command = command;
    packet->request_id = request_id;
}

void binbot_slave_client_init(binbot_slave_client_t *client,
                              binbot_slave_ipc_role_t role,
                              const char *socket_path)
{
    if(!client) return;
    memset(client, 0, sizeof(*client));
    client->fd = -1;
    client->next_request_id = 1;
    client->role = role;
    snprintf(client->socket_path, sizeof(client->socket_path), "%s",
             socket_path && socket_path[0] ? socket_path : BINBOT_SLAVE_DEFAULT_SOCKET);
}

void binbot_slave_client_close(binbot_slave_client_t *client)
{
    if(client) close_socket(client);
}

bool binbot_slave_client_connect(binbot_slave_client_t *client,
                                 uint64_t now_ms)
{
    if(!client) return false;
    if(client->fd >= 0) return true;
    if(client->last_connect_attempt_ms != 0 &&
       now_ms - client->last_connect_attempt_ms < 1000U) return false;
    client->last_connect_attempt_ms = now_ms ? now_ms : 1U;

    int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if(fd < 0) return false;
    if(!set_nonblocking(fd)) {
        close(fd);
        return false;
    }
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
    if(connect(fd, (const struct sockaddr *)&address, address_size) != 0) {
        close(fd);
        return false;
    }
    client->fd = fd;

    binbot_slave_ipc_packet_t hello;
    binbot_slave_ipc_packet_init(&hello, BINBOT_SLAVE_IPC_MSG_HELLO, 0, 0);
    hello.payload_length = 1;
    hello.payload[0] = (uint8_t)client->role;
    size_t packet_size = binbot_slave_ipc_packet_size(&hello);
    ssize_t sent = send(fd, &hello, packet_size, MSG_DONTWAIT | MSG_NOSIGNAL);
    if(sent != (ssize_t)packet_size) {
        close_socket(client);
        return false;
    }
    return true;
}

int binbot_slave_client_fd(const binbot_slave_client_t *client)
{
    return client ? client->fd : -1;
}

static int receive_socket_packet(binbot_slave_client_t *client,
                                 binbot_slave_ipc_packet_t *packet)
{
    if(!client || !packet || client->fd < 0) return -1;
    ssize_t received = recv(client->fd, packet, sizeof(*packet), MSG_DONTWAIT | MSG_TRUNC);
    if(received > 0) {
        if((size_t)received > sizeof(*packet) ||
           !binbot_slave_ipc_packet_is_valid(packet, (size_t)received)) {
            close_socket(client);
            return -1;
        }
        return 1;
    }
    if(received == 0) {
        close_socket(client);
        return -1;
    }
    if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
    close_socket(client);
    return -1;
}

int binbot_slave_client_receive(binbot_slave_client_t *client,
                                binbot_slave_ipc_packet_t *packet)
{
    if(!client || !packet || client->fd < 0) return -1;
    if(client->pending_packet_count != 0) {
        *packet = client->pending_packets[0];
        if(client->pending_packet_count > 1) {
            memmove(&client->pending_packets[0], &client->pending_packets[1],
                    (client->pending_packet_count - 1U) * sizeof(client->pending_packets[0]));
        }
        client->pending_packet_count--;
        return 1;
    }
    return receive_socket_packet(client, packet);
}

static void queue_unsolicited_packet(binbot_slave_client_t *client,
                                     const binbot_slave_ipc_packet_t *packet)
{
    if(packet->type != BINBOT_SLAVE_IPC_MSG_STATUS &&
       packet->type != BINBOT_SLAVE_IPC_MSG_EVENT) return;
    if(client->pending_packet_count == BINBOT_SLAVE_CLIENT_EVENT_QUEUE_CAPACITY) {
        memmove(&client->pending_packets[0], &client->pending_packets[1],
                (BINBOT_SLAVE_CLIENT_EVENT_QUEUE_CAPACITY - 1U) *
                    sizeof(client->pending_packets[0]));
        client->pending_packet_count--;
    }
    client->pending_packets[client->pending_packet_count++] = *packet;
}

uint32_t binbot_slave_client_send_request(binbot_slave_client_t *client,
                                         uint8_t command, const uint8_t *payload,
                                         size_t payload_length)
{
    if(!client || client->fd < 0 || payload_length > BINBOT_SLAVE_IPC_MAX_PAYLOAD ||
       (payload_length && !payload)) return 0;
    uint32_t id = client->next_request_id++;
    if(!id) id = client->next_request_id++;
    binbot_slave_ipc_packet_t packet;
    binbot_slave_ipc_packet_init(&packet, BINBOT_SLAVE_IPC_MSG_REQUEST, command, id);
    packet.payload_length = (uint32_t)payload_length;
    if(payload_length) memcpy(packet.payload, payload, payload_length);
    size_t size = binbot_slave_ipc_packet_size(&packet);
    ssize_t sent = send(client->fd, &packet, size, MSG_DONTWAIT | MSG_NOSIGNAL);
    if(sent == (ssize_t)size) return id;
    if(sent >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
        close_socket(client);
    return 0;
}

bool binbot_slave_client_request(binbot_slave_client_t *client,
                                 uint8_t command,
                                 const uint8_t *payload,
                                 size_t payload_length,
                                 uint32_t timeout_ms,
                                 uint8_t *response_payload,
                                 size_t *response_length,
                                 uint8_t *error_code)
{
    if(!client || client->fd < 0 || payload_length > BINBOT_SLAVE_IPC_MAX_PAYLOAD ||
       (payload_length != 0 && !payload) || !response_length || !error_code) return false;
    *response_length = 0;

    binbot_slave_ipc_packet_t request;
    uint32_t request_id = client->next_request_id++;
    if(request_id == 0) request_id = client->next_request_id++;
    binbot_slave_ipc_packet_init(&request, BINBOT_SLAVE_IPC_MSG_REQUEST,
                                 command, request_id);
    request.payload_length = (uint32_t)payload_length;
    if(payload_length != 0) memcpy(request.payload, payload, payload_length);
    size_t request_size = binbot_slave_ipc_packet_size(&request);
    uint64_t deadline = monotonic_ms() + timeout_ms;
    for(;;) {
        ssize_t sent = send(client->fd, &request, request_size, MSG_DONTWAIT | MSG_NOSIGNAL);
        if(sent == (ssize_t)request_size) break;
        if(sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) &&
           monotonic_ms() < deadline && wait_fd(client->fd, POLLOUT,
                                                  (uint32_t)(deadline - monotonic_ms()))) {
            continue;
        }
        close_socket(client);
        return false;
    }

    for(;;) {
        uint64_t now = monotonic_ms();
        if(now >= deadline || !wait_fd(client->fd, POLLIN, (uint32_t)(deadline - now))) {
            close_socket(client);
            return false;
        }
        binbot_slave_ipc_packet_t response;
        int result = receive_socket_packet(client, &response);
        if(result < 0) return false;
        if(result == 0) continue;
        if(response.type != BINBOT_SLAVE_IPC_MSG_RESPONSE ||
           response.request_id != request_id || response.command != command) {
            queue_unsolicited_packet(client, &response);
            continue;
        }
        *error_code = response.error_code;
        *response_length = response.payload_length;
        if(response_payload && response.payload_length != 0) {
            memcpy(response_payload, response.payload, response.payload_length);
        }
        return true;
    }
}
