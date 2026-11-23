#include "binbot/component/file_receiver.h"

#include <string.h>

static void abort_transfer(bb_component_file_receiver_t *r)
{
    r->ops.abort(r->context);
    r->state = BB_COMPONENT_IDLE;
    r->file_id = 0;
    r->last_length = 0;
}

void bb_component_file_receiver_init(bb_component_file_receiver_t *r,
                                      const bb_component_file_ops_t *ops,
                                      void *context)
{
    memset(r, 0, sizeof(*r));
    r->ops = *ops;
    r->context = context;
}

uint16_t bb_component_file_receiver_command(
    bb_component_file_receiver_t *r, uint16_t command, const uint8_t *p,
    size_t n, uint8_t *reply, size_t *reply_n)
{
    *reply_n = 4;
    bb_component_put_u32(reply, r->received);
    if (command == BB_COMPONENT_FILE_BEGIN) {
        if (n != BB_COMPONENT_FILE_BEGIN_BYTES ||
            bb_component_get_u64(p) == 0 ||
            !bb_component_file_name(p[8]) ||
            p[9] != BB_COMPONENT_FILE_TYPE_GIF ||
            bb_component_get_u16(p + 10) != 0) return BB_COMPONENT_BAD_REQUEST;
        const uint32_t total = bb_component_get_u32(p + 12);
        if (total == 0 || total > BB_COMPONENT_FILE_MAX_BYTES)
            return BB_COMPONENT_TOO_LARGE;
        if (r->state == BB_COMPONENT_COMMITTED &&
            bb_component_get_u64(p) != r->transfer_id)
            r->state = BB_COMPONENT_IDLE;
        if (r->state != BB_COMPONENT_IDLE) {
            if (bb_component_get_u64(p) != r->transfer_id)
                return BB_COMPONENT_BUSY;
            if (p[8] != r->file_id || total != r->total ||
                memcmp(p + 16, r->sha256, 32) != 0)
                return BB_COMPONENT_SESSION;
            bb_component_put_u32(reply, r->received);
            return BB_COMPONENT_OK;
        }
        const uint16_t status = r->ops.begin(
            r->context, p[8], total, p + 16);
        if (status != BB_COMPONENT_OK) return status;
        r->transfer_id = bb_component_get_u64(p);
        r->file_id = p[8];
        r->file_type = p[9];
        r->total = total;
        r->received = 0;
        r->last_length = 0;
        memcpy(r->sha256, p + 16, 32);
        r->state = BB_COMPONENT_RECEIVING;
        /* A new session must not acknowledge the previous file's offset. */
        bb_component_put_u32(reply, r->received);
        return BB_COMPONENT_OK;
    }
    if (command != BB_COMPONENT_FILE_DATA &&
        command != BB_COMPONENT_FILE_QUERY &&
        command != BB_COMPONENT_FILE_COMMIT &&
        command != BB_COMPONENT_FILE_ABORT)
        return BB_COMPONENT_UNSUPPORTED;
    if (command == BB_COMPONENT_FILE_DATA &&
        (n <= 12 || n > 12 + BB_COMPONENT_CHUNK_BYTES))
        return BB_COMPONENT_BAD_REQUEST;
    if (command != BB_COMPONENT_FILE_DATA && n != 8)
        return BB_COMPONENT_BAD_REQUEST;
    if (bb_component_get_u64(p) != r->transfer_id || r->transfer_id == 0)
        return BB_COMPONENT_SESSION;
    if (command == BB_COMPONENT_FILE_QUERY) {
        memset(reply, 0, BB_COMPONENT_FILE_QUERY_BYTES);
        reply[0] = r->state;
        reply[1] = r->file_id;
        bb_component_put_u32(reply + 4, r->received);
        bb_component_put_u32(reply + 8, r->total);
        bb_component_put_u64(reply + 12, r->transfer_id);
        memcpy(reply + 20, r->sha256, 32);
        *reply_n = BB_COMPONENT_FILE_QUERY_BYTES;
        return BB_COMPONENT_OK;
    }
    if (command == BB_COMPONENT_FILE_ABORT) {
        if (r->state == BB_COMPONENT_COMMITTED) return BB_COMPONENT_BUSY;
        abort_transfer(r);
        return BB_COMPONENT_OK;
    }
    if (command == BB_COMPONENT_FILE_DATA) {
        const uint32_t offset = bb_component_get_u32(p + 8);
        const size_t count = n - 12;
        if (r->last_length && offset == r->last_offset &&
            count == r->last_length && memcmp(p + 12, r->last_chunk, count) == 0)
            return BB_COMPONENT_OK;
        if (r->state != BB_COMPONENT_RECEIVING) return BB_COMPONENT_SESSION;
        if (offset != r->received) return BB_COMPONENT_OFFSET;
        if (count > r->total - r->received) return BB_COMPONENT_TOO_LARGE;
        const uint16_t status = r->ops.write(r->context, p + 12, count);
        if (status != BB_COMPONENT_OK) {
            abort_transfer(r);
            return status;
        }
        r->last_offset = offset;
        r->last_length = count;
        memcpy(r->last_chunk, p + 12, count);
        r->received += (uint32_t)count;
        bb_component_put_u32(reply, r->received);
        return BB_COMPONENT_OK;
    }
    if (r->state == BB_COMPONENT_COMMITTED) return BB_COMPONENT_OK;
    if (r->state != BB_COMPONENT_RECEIVING) return BB_COMPONENT_SESSION;
    if (r->received != r->total) return BB_COMPONENT_OFFSET;
    const uint16_t status = r->ops.finish(r->context, r->file_id, r->sha256);
    if (status != BB_COMPONENT_OK) {
        abort_transfer(r);
        return status;
    }
    r->state = BB_COMPONENT_COMMITTED;
    return BB_COMPONENT_OK;
}
