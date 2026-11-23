#ifndef BINBOT_SLAVE_IPC_H
#define BINBOT_SLAVE_IPC_H

#include "binbot/slave/protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    BINBOT_SLAVE_IPC_MAGIC = 0x42534950U, /* "BSIP" */
    BINBOT_SLAVE_IPC_VERSION = 1,
    BINBOT_SLAVE_IPC_MAX_PAYLOAD = BINBOT_SLAVE_MAX_PAYLOAD,
    /* Local transport errors are outside the CH582 protocol error namespace. */
    BINBOT_SLAVE_IPC_ERR_TIMEOUT = 0x80,
    BINBOT_SLAVE_CLIENT_EVENT_QUEUE_CAPACITY = 8,
};

typedef enum {
    BINBOT_SLAVE_IPC_MSG_HELLO = 1,
    BINBOT_SLAVE_IPC_MSG_REQUEST = 2,
    BINBOT_SLAVE_IPC_MSG_RESPONSE = 3,
    BINBOT_SLAVE_IPC_MSG_STATUS = 4,
    BINBOT_SLAVE_IPC_MSG_EVENT = 5,
} binbot_slave_ipc_message_type_t;

typedef enum {
    BINBOT_SLAVE_IPC_ROLE_UNKNOWN = 0,
    BINBOT_SLAVE_IPC_ROLE_APP = 1,
    BINBOT_SLAVE_IPC_ROLE_DIAGNOSTIC = 2,
    BINBOT_SLAVE_IPC_ROLE_OTA = 3,
} binbot_slave_ipc_role_t;

/* A SOCK_SEQPACKET message. Only header + payload_length bytes are sent. */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint8_t command;
    uint8_t error_code;
    uint16_t reserved;
    uint32_t request_id;
    uint32_t payload_length;
    uint8_t payload[BINBOT_SLAVE_IPC_MAX_PAYLOAD];
} binbot_slave_ipc_packet_t;

size_t binbot_slave_ipc_packet_size(const binbot_slave_ipc_packet_t *packet);
bool binbot_slave_ipc_packet_is_valid(const binbot_slave_ipc_packet_t *packet,
                                      size_t received_size);
void binbot_slave_ipc_packet_init(binbot_slave_ipc_packet_t *packet,
                                  binbot_slave_ipc_message_type_t type,
                                  uint8_t command,
                                  uint32_t request_id);

typedef struct {
    int fd;
    uint32_t next_request_id;
    uint64_t last_connect_attempt_ms;
    binbot_slave_ipc_role_t role;
    char socket_path[108];
    binbot_slave_ipc_packet_t pending_packets[BINBOT_SLAVE_CLIENT_EVENT_QUEUE_CAPACITY];
    size_t pending_packet_count;
} binbot_slave_client_t;

void binbot_slave_client_init(binbot_slave_client_t *client,
                              binbot_slave_ipc_role_t role,
                              const char *socket_path);
void binbot_slave_client_close(binbot_slave_client_t *client);
bool binbot_slave_client_connect(binbot_slave_client_t *client,
                                 uint64_t monotonic_ms);
int binbot_slave_client_fd(const binbot_slave_client_t *client);

/* Nonblocking request. Match the returned ID in client_receive responses;
 * zero means the packet was not sent. The caller owns the response deadline. */
uint32_t binbot_slave_client_send_request(binbot_slave_client_t *client,
                                         uint8_t command, const uint8_t *payload,
                                         size_t payload_length);

/* Return true when a complete response was received. */
bool binbot_slave_client_request(binbot_slave_client_t *client,
                                 uint8_t command,
                                 const uint8_t *payload,
                                 size_t payload_length,
                                 uint32_t timeout_ms,
                                 uint8_t *response_payload,
                                 size_t *response_length,
                                 uint8_t *error_code);

/* Read a RESPONSE, STATUS or EVENT packet. Return 0 for no packet. */
int binbot_slave_client_receive(binbot_slave_client_t *client,
                                binbot_slave_ipc_packet_t *packet);

#ifdef __cplusplus
}
#endif

#endif
