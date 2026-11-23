#define _DEFAULT_SOURCE

#include "binbot/slave/ipc.h"
#include "binbot/slave/protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

enum {
    SLAVE_CLIENT_CAPACITY = 8,
    SLAVE_REQUEST_QUEUE_CAPACITY = 8,
    SLAVE_DEFAULT_TIMEOUT_MS = 100,
    SLAVE_DEFAULT_RETRIES = 3,
    SLAVE_DEFAULT_POLL_MS = 500,
    SLAVE_POLL_GRANULARITY_MS = 50,
    STARTUP_STEP_PING = 0,
    STARTUP_STEP_INFO = 1,
    STARTUP_STEP_EVENT_ENABLE = 2,
    STARTUP_STEP_READY = 3,
};

typedef struct {
    int fd;
    bool ready;
    uint8_t role;
} client_slot_t;

typedef struct {
    bool used;
    int client_index;
    uint32_t request_id;
    uint8_t command;
    uint8_t payload_length;
    uint8_t payload[BINBOT_SLAVE_MAX_PAYLOAD];
} queued_request_t;

typedef struct {
    bool active;
    bool internal;
    int client_index;
    uint32_t request_id;
    uint8_t command;
    uint8_t attempts;
    uint64_t deadline_ms;
    binbot_slave_frame_t frame;
    uint8_t encoded[BINBOT_SLAVE_MAX_FRAME];
    size_t encoded_size;
} pending_request_t;

typedef struct {
    int serial_fd;
    int listen_fd;
    char device_path[256];
    char socket_path[108];
    uint8_t address;
    uint8_t next_sequence;
    uint8_t startup_step;
    uint32_t timeout_ms;
    uint32_t max_retries;
    uint32_t poll_ms;
    bool event_enable;
    uint16_t event_mask;
    uint64_t next_serial_attempt_ms;
    uint64_t next_handshake_ms;
    uint64_t next_status_ms;
    client_slot_t clients[SLAVE_CLIENT_CAPACITY];
    queued_request_t queue[SLAVE_REQUEST_QUEUE_CAPACITY];
    pending_request_t pending;
    binbot_slave_parser_t parser;
    bool status_valid;
    int ota_owner;
    binbot_slave_status_all_t status;
} slave_server_t;

static volatile sig_atomic_t running = 1;

static void stop_requested(int signal_number)
{
    (void)signal_number;
    running = 0;
}

static uint64_t monotonic_ms(void)
{
    struct timespec value;
    if(clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return (uint64_t)value.tv_sec * 1000ULL + (uint64_t)value.tv_nsec / 1000000ULL;
}

static uint32_t env_u32(const char *name, uint32_t fallback, uint32_t maximum)
{
    const char *value = getenv(name);
    if(!value || !value[0]) return fallback;
    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 0);
    if(end == value || *end != '\0' || parsed > maximum) return fallback;
    return (uint32_t)parsed;
}

static bool set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if(flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return false;
    int descriptor_flags = fcntl(fd, F_GETFD, 0);
    if(descriptor_flags >= 0) (void)fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC);
    return true;
}

static int serial_open(const char *path)
{
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if(fd < 0) return -1;
    struct termios settings;
    if(tcgetattr(fd, &settings) != 0) {
        close(fd);
        return -1;
    }
    cfmakeraw(&settings);
    (void)cfsetispeed(&settings, B115200);
    (void)cfsetospeed(&settings, B115200);
    settings.c_cflag |= CLOCAL | CREAD;
    settings.c_cflag &= (tcflag_t)~(CSTOPB | PARENB | CSIZE);
    settings.c_cflag |= CS8;
#ifdef CRTSCTS
    settings.c_cflag &= (tcflag_t)~CRTSCTS;
#endif
    settings.c_cc[VMIN] = 0;
    settings.c_cc[VTIME] = 0;
    if(tcsetattr(fd, TCSANOW, &settings) != 0) {
        close(fd);
        return -1;
    }
    (void)tcflush(fd, TCIOFLUSH);
    return fd;
}

static void close_serial(slave_server_t *server, const char *reason)
{
    if(server->serial_fd >= 0) {
        fprintf(stderr, "slave: UART closed (%s)\n", reason ? reason : "error");
        close(server->serial_fd);
    }
    server->serial_fd = -1;
    server->next_serial_attempt_ms = monotonic_ms() + 1000U;
    server->next_handshake_ms = server->next_serial_attempt_ms;
    binbot_slave_parser_init(&server->parser);
    server->startup_step = STARTUP_STEP_PING;
}

static bool write_serial(slave_server_t *server, const uint8_t *data, size_t length)
{
    if(server->serial_fd < 0 || !data || length == 0) return false;
    size_t offset = 0;
    while(offset < length) {
        ssize_t written = write(server->serial_fd, &data[offset], length - offset);
        if(written > 0) {
            offset += (size_t)written;
            continue;
        }
        if(written < 0 && errno == EINTR) continue;
        if(written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd descriptor = {.fd = server->serial_fd, .events = POLLOUT};
            if(poll(&descriptor, 1, 20) > 0 && (descriptor.revents & POLLOUT) != 0) {
                continue;
            }
        }
        return false;
    }
    return true;
}

static bool server_start_ipc(slave_server_t *server)
{
    int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if(fd < 0 || !set_nonblocking(fd)) {
        if(fd >= 0) close(fd);
        fprintf(stderr, "slave: cannot create IPC socket: %s\n", strerror(errno));
        return false;
    }
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", server->socket_path);
    unlink(server->socket_path);
    socklen_t address_size = (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
                                        strlen(address.sun_path) + 1U);
    if(bind(fd, (const struct sockaddr *)&address, address_size) != 0 ||
       listen(fd, SLAVE_CLIENT_CAPACITY) != 0) {
        fprintf(stderr, "slave: cannot listen on %s: %s\n",
                server->socket_path, strerror(errno));
        close(fd);
        unlink(server->socket_path);
        return false;
    }
    (void)chmod(server->socket_path, 0660);
    server->listen_fd = fd;
    return true;
}

static void close_client(slave_server_t *server, size_t index)
{
    if(index >= SLAVE_CLIENT_CAPACITY) return;
    if(server->ota_owner == (int)index) server->ota_owner = -1;
    if(server->clients[index].fd >= 0) close(server->clients[index].fd);
    server->clients[index] = (client_slot_t){.fd = -1};
    for(size_t i = 0; i < SLAVE_REQUEST_QUEUE_CAPACITY; ++i) {
        if(server->queue[i].used && server->queue[i].client_index == (int)index) {
            server->queue[i].used = false;
        }
    }
    if(server->pending.active && server->pending.client_index == (int)index) {
        server->pending.client_index = -1;
    }
}

static bool send_packet(slave_server_t *server, size_t index,
                        const binbot_slave_ipc_packet_t *packet)
{
    if(index >= SLAVE_CLIENT_CAPACITY || server->clients[index].fd < 0 || !packet) return false;
    size_t length = binbot_slave_ipc_packet_size(packet);
    ssize_t sent = send(server->clients[index].fd, packet, length,
                        MSG_DONTWAIT | MSG_NOSIGNAL);
    if(sent == (ssize_t)length) return true;
    if(sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return false;
    close_client(server, index);
    return false;
}

static void send_response(slave_server_t *server, size_t index, uint32_t request_id,
                          uint8_t command, uint8_t error_code,
                          const uint8_t *payload, size_t payload_length)
{
    if(index >= SLAVE_CLIENT_CAPACITY || payload_length > BINBOT_SLAVE_IPC_MAX_PAYLOAD) return;
    binbot_slave_ipc_packet_t packet;
    binbot_slave_ipc_packet_init(&packet, BINBOT_SLAVE_IPC_MSG_RESPONSE,
                                 command, request_id);
    packet.error_code = error_code;
    packet.payload_length = (uint32_t)payload_length;
    if(payload_length != 0 && payload) memcpy(packet.payload, payload, payload_length);
    (void)send_packet(server, index, &packet);
}

static void broadcast_status(slave_server_t *server)
{
    if(!server->status_valid) return;
    binbot_slave_ipc_packet_t packet;
    binbot_slave_ipc_packet_init(&packet, BINBOT_SLAVE_IPC_MSG_STATUS,
                                 BINBOT_SLAVE_CMD_GET_STATUS_ALL, 0);
    packet.payload_length = (uint32_t)binbot_slave_status_encode(
        &server->status, packet.payload, sizeof(packet.payload));
    for(size_t i = 0; i < SLAVE_CLIENT_CAPACITY; ++i) {
        if(server->clients[i].fd >= 0 && server->clients[i].ready) {
            (void)send_packet(server, i, &packet);
        }
    }
}

static void broadcast_event(slave_server_t *server, const binbot_slave_frame_t *frame)
{
    binbot_slave_ipc_packet_t packet;
    binbot_slave_ipc_packet_init(&packet, BINBOT_SLAVE_IPC_MSG_EVENT,
                                 frame->command, 0);
    packet.payload_length = frame->payload_length;
    memcpy(packet.payload, frame->payload, frame->payload_length);
    for(size_t i = 0; i < SLAVE_CLIENT_CAPACITY; ++i) {
        if(server->clients[i].fd >= 0 && server->clients[i].ready) {
            (void)send_packet(server, i, &packet);
        }
    }
}

static bool queue_request(slave_server_t *server, int client_index,
                          const binbot_slave_ipc_packet_t *packet)
{
    for(size_t i = 0; i < SLAVE_REQUEST_QUEUE_CAPACITY; ++i) {
        if(!server->queue[i].used) {
            queued_request_t *request = &server->queue[i];
            request->used = true;
            request->client_index = client_index;
            request->request_id = packet->request_id;
            request->command = packet->command;
            request->payload_length = (uint8_t)packet->payload_length;
            memcpy(request->payload, packet->payload, packet->payload_length);
            return true;
        }
    }
    return false;
}

static bool pop_request(slave_server_t *server, queued_request_t *request)
{
    for(size_t i = 0; i < SLAVE_REQUEST_QUEUE_CAPACITY; ++i) {
        if(server->queue[i].used) {
            *request = server->queue[i];
            server->queue[i].used = false;
            return true;
        }
    }
    return false;
}

static void handle_client_packet(slave_server_t *server, size_t index,
                                 const binbot_slave_ipc_packet_t *packet,
                                 size_t received_size)
{
    if(!binbot_slave_ipc_packet_is_valid(packet, received_size)) {
        close_client(server, index);
        return;
    }
    if(packet->type == BINBOT_SLAVE_IPC_MSG_HELLO) {
        if(packet->payload_length != 1 || packet->payload[0] == BINBOT_SLAVE_IPC_ROLE_UNKNOWN) {
            close_client(server, index);
            return;
        }
        server->clients[index].ready = true;
        server->clients[index].role = packet->payload[0];
        if(packet->payload[0] == BINBOT_SLAVE_IPC_ROLE_OTA) {
            if(server->ota_owner >= 0 && server->ota_owner != (int)index) {
                close_client(server, index);
                return;
            }
            server->ota_owner = (int)index;
            for(size_t i = 0; i < SLAVE_REQUEST_QUEUE_CAPACITY; ++i) {
                queued_request_t *q = &server->queue[i];
                if(q->used && q->client_index != (int)index) {
                    send_response(server, (size_t)q->client_index, q->request_id, q->command,
                                  BINBOT_SLAVE_ERR_BUSY, NULL, 0);
                    q->used = false;
                }
            }
        }
        if(server->status_valid) {
            binbot_slave_ipc_packet_t status_packet;
            binbot_slave_ipc_packet_init(&status_packet, BINBOT_SLAVE_IPC_MSG_STATUS,
                                         BINBOT_SLAVE_CMD_GET_STATUS_ALL, 0);
            status_packet.payload_length = (uint32_t)binbot_slave_status_encode(
                &server->status, status_packet.payload, sizeof(status_packet.payload));
            (void)send_packet(server, index, &status_packet);
        }
        return;
    }
    if(packet->type != BINBOT_SLAVE_IPC_MSG_REQUEST || !server->clients[index].ready) {
        close_client(server, index);
        return;
    }
    if(server->ota_owner >= 0 && server->ota_owner != (int)index) {
        send_response(server, index, packet->request_id, packet->command,
                      BINBOT_SLAVE_ERR_BUSY, NULL, 0);
        return;
    }
    if(packet->command >= BINBOT_SLAVE_CMD_OTA_BEGIN && packet->command <= BINBOT_SLAVE_CMD_OTA_ABORT &&
       server->clients[index].role != BINBOT_SLAVE_IPC_ROLE_OTA) {
        send_response(server, index, packet->request_id, packet->command,
                      BINBOT_SLAVE_ERR_DENIED, NULL, 0);
        return;
    }
    binbot_slave_error_t validation = binbot_slave_validate_request(
        packet->command, packet->payload, packet->payload_length);
    if(validation != BINBOT_SLAVE_ERR_OK) {
        send_response(server, index, packet->request_id, packet->command,
                      (uint8_t)validation, NULL, 0);
        return;
    }
    if(packet->request_id == 0 || !queue_request(server, (int)index, packet)) {
        send_response(server, index, packet->request_id, packet->command,
                      BINBOT_SLAVE_ERR_BUSY, NULL, 0);
    }
}

static void read_client(slave_server_t *server, size_t index)
{
    while(server->clients[index].fd >= 0) {
        binbot_slave_ipc_packet_t packet;
        ssize_t received = recv(server->clients[index].fd, &packet, sizeof(packet),
                                MSG_DONTWAIT | MSG_TRUNC);
        if(received > 0) {
            if((size_t)received > sizeof(packet)) {
                close_client(server, index);
                return;
            }
            handle_client_packet(server, index, &packet, (size_t)received);
            continue;
        }
        if(received == 0) {
            close_client(server, index);
            return;
        }
        if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return;
        close_client(server, index);
        return;
    }
}

static void accept_clients(slave_server_t *server)
{
    for(;;) {
        int fd = accept(server->listen_fd, NULL, NULL);
        if(fd < 0) {
            if(errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                fprintf(stderr, "slave: accept failed: %s\n", strerror(errno));
            }
            return;
        }
        if(!set_nonblocking(fd)) {
            close(fd);
            continue;
        }
        size_t slot = SLAVE_CLIENT_CAPACITY;
        for(size_t i = 0; i < SLAVE_CLIENT_CAPACITY; ++i) {
            if(server->clients[i].fd < 0) {
                slot = i;
                break;
            }
        }
        if(slot == SLAVE_CLIENT_CAPACITY) {
            close(fd);
            continue;
        }
        server->clients[slot] = (client_slot_t){.fd = fd, .ready = false};
    }
}

static void complete_pending(slave_server_t *server, uint8_t error_code,
                             const uint8_t *payload, size_t payload_length)
{
    pending_request_t completed = server->pending;
    server->pending.active = false;
    if(completed.client_index >= 0 && completed.client_index < SLAVE_CLIENT_CAPACITY) {
        send_response(server, (size_t)completed.client_index, completed.request_id,
                      completed.command, error_code, payload, payload_length);
    }

    if(error_code == BINBOT_SLAVE_ERR_OK &&
       completed.command == BINBOT_SLAVE_CMD_GET_STATUS_ALL) {
        binbot_slave_status_all_t status;
        if(binbot_slave_status_decode(payload, payload_length, &status)) {
            bool changed = !server->status_valid ||
                           !binbot_slave_status_equal(&server->status, &status);
            server->status = status;
            server->status_valid = true;
            if(changed) broadcast_status(server);
        }
        else {
            fprintf(stderr, "slave: invalid status payload length %zu\n", payload_length);
        }
    }
    if(error_code == BINBOT_SLAVE_ERR_OK &&
       completed.command == BINBOT_SLAVE_CMD_EVENT_ENABLE && payload_length >= 4 &&
       payload[3] == BINBOT_SLAVE_ERR_OK) {
        server->event_enable = payload[0] != 0;
        server->event_mask = (uint16_t)payload[1] | ((uint16_t)payload[2] << 8);
    }
    if(completed.internal) {
        if(error_code != BINBOT_SLAVE_ERR_OK) {
            fprintf(stderr, "slave: internal command 0x%02x failed, error 0x%02x\n",
                    completed.command, error_code);
            server->startup_step = STARTUP_STEP_PING;
            server->next_handshake_ms = monotonic_ms() + 1000U;
        }
        else if(completed.command == BINBOT_SLAVE_CMD_PING &&
                server->startup_step == STARTUP_STEP_PING) {
            server->startup_step = STARTUP_STEP_INFO;
        }
        else if(completed.command == BINBOT_SLAVE_CMD_GET_INFO &&
                server->startup_step == STARTUP_STEP_INFO) {
            server->startup_step = server->event_enable ? STARTUP_STEP_EVENT_ENABLE
                                                        : STARTUP_STEP_READY;
            if(!server->event_enable) {
                fprintf(stdout, "slave: UART4 protocol handshake complete (events disabled)\n");
            }
        }
        else if(completed.command == BINBOT_SLAVE_CMD_EVENT_ENABLE &&
                server->startup_step == STARTUP_STEP_EVENT_ENABLE) {
            server->startup_step = STARTUP_STEP_READY;
            fprintf(stdout, "slave: UART4 protocol handshake complete\n");
        }
    }
}

static void handle_slave_frame(slave_server_t *server, const binbot_slave_frame_t *frame)
{
    if(frame->version != BINBOT_SLAVE_PROTOCOL_VERSION || frame->address != server->address) return;
    if((frame->flags & BINBOT_SLAVE_FLAG_EVENT) != 0) {
        if(frame->command == BINBOT_SLAVE_CMD_EVENT_REPORT) {
            binbot_slave_event_report_t event_report;
            if(binbot_slave_event_decode(frame->payload, frame->payload_length, &event_report)) {
                binbot_slave_status_all_t status = server->status;
                status.status_flags = event_report.status_flags;
                status.encoder_count = event_report.encoder_count;
                status.adc_key = event_report.adc_key;
                status.adc_key_raw = event_report.adc_key_raw;
                status.component_voltage_mv = event_report.component_voltage_mv;
                status.component_current_ma = event_report.component_current_ma;
                bool changed = !server->status_valid ||
                               !binbot_slave_status_equal(&server->status, &status);
                server->status = status;
                server->status_valid = true;
                if(changed) broadcast_status(server);
            }
            else {
                fprintf(stderr, "slave: invalid event payload length %u\n",
                        (unsigned)frame->payload_length);
            }
        }
        broadcast_event(server, frame);
        return;
    }
    if(!server->pending.active || (frame->flags & BINBOT_SLAVE_FLAG_RESPONSE) == 0 ||
       frame->sequence != server->pending.frame.sequence ||
       frame->command != server->pending.command) {
        return;
    }
    if((frame->flags & BINBOT_SLAVE_FLAG_ERROR) != 0) {
        uint8_t error = frame->payload_length != 0 ? frame->payload[0] : BINBOT_SLAVE_ERR_BAD_LENGTH;
        complete_pending(server, error, NULL, 0);
    }
    else {
        complete_pending(server, BINBOT_SLAVE_ERR_OK,
                         frame->payload, frame->payload_length);
    }
}

static void read_serial(slave_server_t *server)
{
    uint8_t buffer[256];
    for(;;) {
        ssize_t received = read(server->serial_fd, buffer, sizeof(buffer));
        if(received > 0) {
            for(ssize_t i = 0; i < received; ++i) {
                binbot_slave_frame_t frame;
                binbot_slave_parse_result_t result = binbot_slave_parser_feed(
                    &server->parser, buffer[i], &frame);
                if(result == BINBOT_SLAVE_PARSE_FRAME) handle_slave_frame(server, &frame);
            }
            continue;
        }
        /* With VMIN=0 on a non-blocking tty, an empty read is normal when
         * poll reported readable data that was consumed by the previous read.
         * The main loop handles POLLHUP/POLLERR separately. */
        if(received == 0) return;
        if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return;
        fprintf(stderr, "slave: UART read failed: %s\n", strerror(errno));
        close_serial(server, "read error");
        return;
    }
}

static void start_pending(slave_server_t *server, uint64_t now)
{
    if(server->serial_fd < 0 || server->pending.active) return;
    queued_request_t queued;
    bool internal = false;
    if(pop_request(server, &queued)) {
        if(queued.client_index < 0 || queued.client_index >= SLAVE_CLIENT_CAPACITY ||
           server->clients[queued.client_index].fd < 0 || !server->clients[queued.client_index].ready) {
            return;
        }
        memset(&server->pending, 0, sizeof(server->pending));
        server->pending.client_index = queued.client_index;
        server->pending.request_id = queued.request_id;
        server->pending.command = queued.command;
        server->pending.frame.payload_length = queued.payload_length;
        memcpy(server->pending.frame.payload, queued.payload, queued.payload_length);
    }
    else if(server->ota_owner < 0 &&
            ((server->startup_step < STARTUP_STEP_READY && now >= server->next_handshake_ms) ||
            (server->startup_step >= STARTUP_STEP_READY && now >= server->next_status_ms))) {
        internal = true;
        memset(&server->pending, 0, sizeof(server->pending));
        server->pending.client_index = -1;
        if(server->startup_step == STARTUP_STEP_PING) {
            server->pending.command = BINBOT_SLAVE_CMD_PING;
            server->pending.frame.payload_length = 0;
        }
        else if(server->startup_step == STARTUP_STEP_INFO) {
            server->pending.command = BINBOT_SLAVE_CMD_GET_INFO;
            server->pending.frame.payload_length = 0;
        }
        else if(server->startup_step == STARTUP_STEP_EVENT_ENABLE) {
            server->pending.command = BINBOT_SLAVE_CMD_EVENT_ENABLE;
            server->pending.frame.payload_length = 3;
            server->pending.frame.payload[0] = server->event_enable ? 1U : 0U;
            server->pending.frame.payload[1] = (uint8_t)server->event_mask;
            server->pending.frame.payload[2] = (uint8_t)(server->event_mask >> 8);
        }
        else {
            server->pending.command = BINBOT_SLAVE_CMD_GET_STATUS_ALL;
            server->pending.frame.payload_length = 0;
        }
        if(server->pending.command == BINBOT_SLAVE_CMD_GET_STATUS_ALL) {
            server->next_status_ms = now + server->poll_ms;
        }
    }
    else {
        return;
    }

    server->pending.active = true;
    server->pending.internal = internal;
    server->pending.frame.version = BINBOT_SLAVE_PROTOCOL_VERSION;
    server->pending.frame.address = server->address;
    server->pending.frame.sequence = server->next_sequence++;
    if(server->pending.frame.sequence == 0) server->pending.frame.sequence = server->next_sequence++;
    server->pending.frame.command = server->pending.command;
    server->pending.frame.flags = 0;
    server->pending.encoded_size = binbot_slave_frame_encode(
        &server->pending.frame, server->pending.encoded, sizeof(server->pending.encoded));
    server->pending.attempts = 1;
    if(!write_serial(server, server->pending.encoded, server->pending.encoded_size)) {
        close_serial(server, "write error");
    }
    uint32_t timeout = server->pending.command == BINBOT_SLAVE_CMD_OTA_BEGIN ? 5000 :
                       server->pending.command == BINBOT_SLAVE_CMD_OTA_FINISH ? 3000 : server->timeout_ms;
    server->pending.deadline_ms = now + timeout;
}

static void retry_or_timeout(slave_server_t *server, uint64_t now)
{
    if(!server->pending.active || now < server->pending.deadline_ms) return;
    if(server->pending.attempts <= server->max_retries) {
        server->pending.attempts++;
        if(server->serial_fd >= 0 &&
           !write_serial(server, server->pending.encoded, server->pending.encoded_size)) {
            close_serial(server, "retry write error");
        }
        uint32_t timeout = server->pending.command == BINBOT_SLAVE_CMD_OTA_BEGIN ? 5000 :
                           server->pending.command == BINBOT_SLAVE_CMD_OTA_FINISH ? 3000 : server->timeout_ms;
        server->pending.deadline_ms = now + timeout;
        return;
    }
    complete_pending(server, BINBOT_SLAVE_IPC_ERR_TIMEOUT, NULL, 0);
}

static int build_pollfds(slave_server_t *server, struct pollfd *pollfds,
                         size_t *client_indices, int *serial_index)
{
    int count = 0;
    pollfds[count++] = (struct pollfd){.fd = server->listen_fd, .events = POLLIN};
    *serial_index = -1;
    if(server->serial_fd >= 0) {
        *serial_index = count;
        pollfds[count++] = (struct pollfd){.fd = server->serial_fd, .events = POLLIN};
    }
    for(size_t i = 0; i < SLAVE_CLIENT_CAPACITY; ++i) {
        if(server->clients[i].fd >= 0) {
            client_indices[count] = i;
            pollfds[count++] = (struct pollfd){.fd = server->clients[i].fd, .events = POLLIN};
        }
    }
    return count;
}

static int poll_timeout_ms(const slave_server_t *server, uint64_t now)
{
    uint64_t deadline = now + SLAVE_POLL_GRANULARITY_MS;
    if(server->pending.active && server->pending.deadline_ms < deadline) deadline = server->pending.deadline_ms;
    if(server->serial_fd < 0 && server->next_serial_attempt_ms < deadline) {
        deadline = server->next_serial_attempt_ms;
    }
    if(server->ota_owner < 0 && !server->pending.active && server->serial_fd >= 0 && server->startup_step >= STARTUP_STEP_READY &&
       server->next_status_ms < deadline) deadline = server->next_status_ms;
    if(server->ota_owner < 0 && !server->pending.active && server->serial_fd >= 0 && server->startup_step < STARTUP_STEP_READY &&
       server->next_handshake_ms < deadline) deadline = server->next_handshake_ms;
    if(deadline <= now) return 0;
    uint64_t wait = deadline - now;
    return wait > (uint64_t)INT32_MAX ? INT32_MAX : (int)wait;
}

int main(void)
{
    signal(SIGINT, stop_requested);
    signal(SIGTERM, stop_requested);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    slave_server_t server;
    memset(&server, 0, sizeof(server));
    server.serial_fd = -1;
    server.listen_fd = -1;
    server.ota_owner = -1;
    server.address = (uint8_t)env_u32("BINBOT_SLAVE_ADDRESS", BINBOT_SLAVE_DEFAULT_ADDRESS, 255);
    server.timeout_ms = env_u32("BINBOT_SLAVE_TIMEOUT_MS", SLAVE_DEFAULT_TIMEOUT_MS, 10000);
    server.max_retries = env_u32("BINBOT_SLAVE_RETRIES", SLAVE_DEFAULT_RETRIES, 10);
    server.poll_ms = env_u32("BINBOT_SLAVE_POLL_MS", SLAVE_DEFAULT_POLL_MS, 60000);
    server.event_enable = env_u32("BINBOT_SLAVE_EVENT_ENABLE", 1, 1) != 0;
    server.event_mask = (uint16_t)env_u32("BINBOT_SLAVE_EVENT_MASK", 0x001FU, 0xFFFFU);
    snprintf(server.device_path, sizeof(server.device_path), "%s",
             getenv("BINBOT_SLAVE_DEVICE") && getenv("BINBOT_SLAVE_DEVICE")[0]
                 ? getenv("BINBOT_SLAVE_DEVICE") : BINBOT_SLAVE_DEFAULT_DEVICE);
    snprintf(server.socket_path, sizeof(server.socket_path), "%s",
             getenv("BINBOT_SLAVE_SOCKET") && getenv("BINBOT_SLAVE_SOCKET")[0]
                 ? getenv("BINBOT_SLAVE_SOCKET") : BINBOT_SLAVE_DEFAULT_SOCKET);
    for(size_t i = 0; i < SLAVE_CLIENT_CAPACITY; ++i) server.clients[i].fd = -1;
    binbot_slave_parser_init(&server.parser);
    server.next_sequence = 1;
    server.next_serial_attempt_ms = 0;
    server.next_status_ms = 0;

    if(strlen(server.socket_path) >= sizeof(((struct sockaddr_un *)0)->sun_path) ||
       !server_start_ipc(&server)) {
        return EXIT_FAILURE;
    }
    fprintf(stdout, "slave: IPC listening on %s, UART device %s\n",
            server.socket_path, server.device_path);

    while(running) {
        uint64_t now = monotonic_ms();
        if(server.serial_fd < 0 && now >= server.next_serial_attempt_ms) {
            server.serial_fd = serial_open(server.device_path);
            if(server.serial_fd >= 0) {
                fprintf(stdout, "slave: UART4 opened at 115200 8N1\n");
                binbot_slave_parser_init(&server.parser);
                server.startup_step = STARTUP_STEP_PING;
                server.next_handshake_ms = now;
                server.next_status_ms = now;
            }
            else {
                fprintf(stderr, "slave: cannot open %s: %s\n",
                        server.device_path, strerror(errno));
                server.next_serial_attempt_ms = now + 1000U;
            }
        }
        start_pending(&server, now);

        struct pollfd pollfds[2 + SLAVE_CLIENT_CAPACITY];
        size_t client_indices[2 + SLAVE_CLIENT_CAPACITY];
        int serial_index;
        int poll_count = build_pollfds(&server, pollfds, client_indices, &serial_index);
        int result = poll(pollfds, (nfds_t)poll_count, poll_timeout_ms(&server, now));
        if(result < 0 && errno != EINTR) {
            fprintf(stderr, "slave: poll failed: %s\n", strerror(errno));
            break;
        }
        if(result > 0) {
            if((pollfds[0].revents & POLLIN) != 0) accept_clients(&server);
            if(serial_index >= 0 && (pollfds[serial_index].revents & POLLIN) != 0) {
                read_serial(&server);
            }
            if(serial_index >= 0 &&
               (pollfds[serial_index].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                close_serial(&server, "poll error");
            }
            for(int i = 1; i < poll_count; ++i) {
                if(i == serial_index) continue;
                if((pollfds[i].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)) != 0) {
                    read_client(&server, client_indices[i]);
                }
            }
        }
        retry_or_timeout(&server, monotonic_ms());
    }

    server.pending.active = false;
    for(size_t i = 0; i < SLAVE_CLIENT_CAPACITY; ++i) close_client(&server, i);
    if(server.serial_fd >= 0) close(server.serial_fd);
    if(server.listen_fd >= 0) close(server.listen_fd);
    unlink(server.socket_path);
    fprintf(stdout, "slave: stopped cleanly\n");
    return EXIT_SUCCESS;
}
