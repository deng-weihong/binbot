#ifndef BINBOT_AGENT_IPC_H
#define BINBOT_AGENT_IPC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    BINBOT_AGENT_IPC_VERSION = 1,
    BINBOT_AGENT_REQUEST_MAX = 2048,
    BINBOT_AGENT_RESPONSE_MAX = 3072,
};

typedef enum {
    BINBOT_AGENT_MSG_REQUEST = 1,
    BINBOT_AGENT_MSG_RESPONSE = 2,
    BINBOT_AGENT_MSG_ERROR = 3,
} binbot_agent_message_type_t;

/* Read or write one bounded message on an already connected Unix socket. */
bool binbot_agent_ipc_read(int fd, binbot_agent_message_type_t *type,
                           char *payload, size_t capacity, size_t *payload_size,
                           uint32_t timeout_ms, char *error, size_t error_capacity);
bool binbot_agent_ipc_write(int fd, binbot_agent_message_type_t type,
                            const char *payload, size_t payload_size,
                            uint32_t timeout_ms, char *error,
                            size_t error_capacity);

/* Blocking client helper used from binbot_dialog's worker thread. */
bool binbot_agent_request(const char *socket_path, const char *request,
                          char *response, size_t response_capacity,
                          uint32_t timeout_ms, char *error,
                          size_t error_capacity);

#ifdef __cplusplus
}
#endif

#endif
