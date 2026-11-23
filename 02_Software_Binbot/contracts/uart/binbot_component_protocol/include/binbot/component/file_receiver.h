#ifndef BINBOT_COMPONENT_FILE_RECEIVER_H
#define BINBOT_COMPONENT_FILE_RECEIVER_H

#include "binbot/component/protocol.h"

typedef struct {
    uint16_t (*begin)(void *context, uint8_t file_id, uint32_t total,
                      const uint8_t expected_sha256[32]);
    uint16_t (*write)(void *context, const uint8_t *bytes, size_t length);
    uint16_t (*finish)(void *context, uint8_t file_id,
                       const uint8_t expected_sha256[32]);
    void (*abort)(void *context);
} bb_component_file_ops_t;

typedef struct {
    uint8_t state;
    uint8_t file_id;
    uint8_t file_type;
    uint32_t total;
    uint32_t received;
    uint64_t transfer_id;
    uint8_t sha256[32];
    uint32_t last_offset;
    size_t last_length;
    uint8_t last_chunk[BB_COMPONENT_CHUNK_BYTES];
    bb_component_file_ops_t ops;
    void *context;
} bb_component_file_receiver_t;

void bb_component_file_receiver_init(bb_component_file_receiver_t *receiver,
                                      const bb_component_file_ops_t *ops,
                                      void *context);
uint16_t bb_component_file_receiver_command(
    bb_component_file_receiver_t *receiver, uint16_t command,
    const uint8_t *payload, size_t length, uint8_t *reply,
    size_t *reply_length);

#endif
