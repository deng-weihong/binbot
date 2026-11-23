#include "binbot/agent/ipc.h"

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

enum {
    AGENT_IPC_MAGIC = 0x42414754U, /* BAGT */
    AGENT_IPC_HEADER_SIZE = 12,
};

static void set_error(char *error, size_t capacity, const char *format, ...)
{
    if(!error || capacity == 0) return;
    va_list arguments;
    va_start(arguments, format);
    (void)vsnprintf(error, capacity, format, arguments);
    va_end(arguments);
}

static uint64_t monotonic_ms(void)
{
    struct timespec value;
    if(clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return (uint64_t)value.tv_sec * 1000ULL +
           (uint64_t)value.tv_nsec / 1000000ULL;
}

static bool wait_fd(int fd, short events, uint64_t deadline_ms,
                    char *error, size_t error_capacity)
{
    for(;;) {
        uint64_t now = monotonic_ms();
        if(now >= deadline_ms) {
            set_error(error, error_capacity, "agent IPC timed out");
            return false;
        }
        uint64_t remaining = deadline_ms - now;
        int timeout = remaining > 2147483647ULL ? 2147483647 : (int)remaining;
        struct pollfd descriptor = {.fd = fd, .events = events};
        int result = poll(&descriptor, 1, timeout);
        if(result > 0) {
            if(descriptor.revents & events) return true;
            set_error(error, error_capacity, "agent IPC connection closed");
            return false;
        }
        if(result == 0) {
            set_error(error, error_capacity, "agent IPC timed out");
            return false;
        }
        if(errno != EINTR) {
            set_error(error, error_capacity, "agent IPC poll failed: %s",
                      strerror(errno));
            return false;
        }
    }
}

static bool transfer(int fd, void *buffer, size_t size, bool writing,
                     uint64_t deadline_ms, char *error, size_t error_capacity)
{
    uint8_t *bytes = buffer;
    size_t offset = 0;
    while(offset < size) {
        if(!wait_fd(fd, writing ? POLLOUT : POLLIN, deadline_ms,
                    error, error_capacity)) return false;
        ssize_t count = writing
            ? write(fd, bytes + offset, size - offset)
            : read(fd, bytes + offset, size - offset);
        if(count > 0) {
            offset += (size_t)count;
            continue;
        }
        if(count == 0) {
            set_error(error, error_capacity, "agent IPC connection closed");
            return false;
        }
        if(errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            set_error(error, error_capacity, "agent IPC %s failed: %s",
                      writing ? "write" : "read", strerror(errno));
            return false;
        }
    }
    return true;
}

bool binbot_agent_ipc_write(int fd, binbot_agent_message_type_t type,
                            const char *payload, size_t payload_size,
                            uint32_t timeout_ms, char *error,
                            size_t error_capacity)
{
    if(fd < 0 || !payload || payload_size == 0 ||
       payload_size > BINBOT_AGENT_RESPONSE_MAX ||
       type < BINBOT_AGENT_MSG_REQUEST || type > BINBOT_AGENT_MSG_ERROR) {
        set_error(error, error_capacity, "invalid agent IPC message");
        return false;
    }
    uint8_t header[AGENT_IPC_HEADER_SIZE];
    uint32_t magic = htonl(AGENT_IPC_MAGIC);
    uint16_t version = htons(BINBOT_AGENT_IPC_VERSION);
    uint16_t wire_type = htons((uint16_t)type);
    uint32_t length = htonl((uint32_t)payload_size);
    memcpy(header, &magic, sizeof(magic));
    memcpy(header + 4, &version, sizeof(version));
    memcpy(header + 6, &wire_type, sizeof(wire_type));
    memcpy(header + 8, &length, sizeof(length));
    uint64_t deadline = monotonic_ms() + (timeout_ms ? timeout_ms : 1U);
    return transfer(fd, header, sizeof(header), true, deadline,
                    error, error_capacity) &&
           transfer(fd, (void *)payload, payload_size, true, deadline,
                    error, error_capacity);
}

bool binbot_agent_ipc_read(int fd, binbot_agent_message_type_t *type,
                           char *payload, size_t capacity, size_t *payload_size,
                           uint32_t timeout_ms, char *error, size_t error_capacity)
{
    if(fd < 0 || !type || !payload || capacity < 2 || !payload_size) {
        set_error(error, error_capacity, "invalid agent IPC receive buffer");
        return false;
    }
    uint64_t deadline = monotonic_ms() + (timeout_ms ? timeout_ms : 1U);
    uint8_t header[AGENT_IPC_HEADER_SIZE];
    if(!transfer(fd, header, sizeof(header), false, deadline,
                 error, error_capacity)) return false;
    uint32_t magic;
    uint16_t version;
    uint16_t wire_type;
    uint32_t length;
    memcpy(&magic, header, sizeof(magic));
    memcpy(&version, header + 4, sizeof(version));
    memcpy(&wire_type, header + 6, sizeof(wire_type));
    memcpy(&length, header + 8, sizeof(length));
    magic = ntohl(magic);
    version = ntohs(version);
    wire_type = ntohs(wire_type);
    length = ntohl(length);
    if(magic != AGENT_IPC_MAGIC || version != BINBOT_AGENT_IPC_VERSION ||
       wire_type < BINBOT_AGENT_MSG_REQUEST ||
       wire_type > BINBOT_AGENT_MSG_ERROR || length == 0 ||
       length >= capacity || length > BINBOT_AGENT_RESPONSE_MAX) {
        set_error(error, error_capacity, "invalid agent IPC header");
        return false;
    }
    if(!transfer(fd, payload, length, false, deadline,
                 error, error_capacity)) return false;
    payload[length] = '\0';
    if(memchr(payload, '\0', length) != NULL) {
        set_error(error, error_capacity, "agent IPC payload contains NUL");
        return false;
    }
    *type = (binbot_agent_message_type_t)wire_type;
    *payload_size = length;
    return true;
}

bool binbot_agent_request(const char *socket_path, const char *request,
                          char *response, size_t response_capacity,
                          uint32_t timeout_ms, char *error,
                          size_t error_capacity)
{
    if(!socket_path || !socket_path[0] || !request || !request[0] ||
       !response || response_capacity < 2) {
        set_error(error, error_capacity, "invalid agent request");
        return false;
    }
    size_t request_size = strlen(request);
    if(request_size > BINBOT_AGENT_REQUEST_MAX) {
        set_error(error, error_capacity, "agent request is too long");
        return false;
    }
    if(strlen(socket_path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        set_error(error, error_capacity, "agent socket path is too long");
        return false;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if(fd < 0) {
        set_error(error, error_capacity, "cannot create agent socket: %s",
                  strerror(errno));
        return false;
    }
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", socket_path);
    if(connect(fd, (const struct sockaddr *)&address, sizeof(address)) != 0) {
        set_error(error, error_capacity, "cannot connect to device agent: %s",
                  strerror(errno));
        close(fd);
        return false;
    }
    bool ok = binbot_agent_ipc_write(
        fd, BINBOT_AGENT_MSG_REQUEST, request, request_size, timeout_ms,
        error, error_capacity);
    binbot_agent_message_type_t response_type = BINBOT_AGENT_MSG_ERROR;
    size_t response_size = 0;
    if(ok) {
        ok = binbot_agent_ipc_read(fd, &response_type, response,
                                   response_capacity, &response_size,
                                   timeout_ms, error, error_capacity);
    }
    close(fd);
    if(!ok) return false;
    if(response_type != BINBOT_AGENT_MSG_RESPONSE) {
        set_error(error, error_capacity, "%s", response);
        return false;
    }
    return response_size > 0;
}
