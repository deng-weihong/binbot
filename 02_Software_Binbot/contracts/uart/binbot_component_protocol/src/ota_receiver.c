#include "binbot/component/ota_receiver.h"

#include <string.h>

void bb_component_receiver_init(bb_component_receiver_t *r, uint8_t profile,
                                uint32_t capacity,
                                const bb_component_ota_ops_t *ops, void *context)
{
    memset(r, 0, sizeof(*r));
    r->profile = profile;
    r->capacity = capacity;
    r->ops = *ops;
    r->context = context;
}

static void abort_transfer(bb_component_receiver_t *r)
{
    r->ops.abort(r->context);
    r->state = BB_COMPONENT_IDLE;
    r->last_length = 0;
}

uint16_t bb_component_receiver_command(bb_component_receiver_t *r,
                                       uint16_t command, const uint8_t *p,
                                       size_t n, uint8_t *reply, size_t *reply_n)
{
    *reply_n = 4;
    bb_component_put_u32(reply, r->received);
    if (command == BB_COMPONENT_TRANSFER_BEGIN) {
        if (n != BB_COMPONENT_BEGIN_BYTES || p[8] != 1 ||
            bb_component_get_u16(p + 10) != 0 ||
            bb_component_get_u64(p) == 0) return BB_COMPONENT_BAD_REQUEST;
        if (p[9] != r->profile) return BB_COMPONENT_WRONG_IMAGE;
        const uint32_t total = bb_component_get_u32(p + 12);
        if (total < BB_COMPONENT_IMAGE_PREFIX_BYTES || total > r->capacity)
            return BB_COMPONENT_TOO_LARGE;
        if (r->state != BB_COMPONENT_IDLE) {
            if (bb_component_get_u64(p) != r->transfer_id) return BB_COMPONENT_BUSY;
            if (total != r->total || memcmp(p + 16, r->sha256, 32) != 0)
                return BB_COMPONENT_SESSION;
            return BB_COMPONENT_OK;
        }
        const uint16_t status = r->ops.begin(r->context, total);
        if (status != BB_COMPONENT_OK) return status;
        r->transfer_id = bb_component_get_u64(p);
        r->total = total;
        r->received = 0;
        r->last_length = 0;
        memcpy(r->sha256, p + 16, 32);
        r->state = BB_COMPONENT_RECEIVING;
        bb_component_put_u32(reply, 0);
        return BB_COMPONENT_OK;
    }
    if (command != BB_COMPONENT_TRANSFER_DATA &&
        command != BB_COMPONENT_TRANSFER_QUERY &&
        command != BB_COMPONENT_TRANSFER_COMMIT &&
        command != BB_COMPONENT_TRANSFER_ABORT &&
        command != BB_COMPONENT_OTA_ACTIVATE) return BB_COMPONENT_UNSUPPORTED;
    if ((command == BB_COMPONENT_TRANSFER_DATA &&
         (n <= 12 || n > 12 + BB_COMPONENT_CHUNK_BYTES)) ||
        (command != BB_COMPONENT_TRANSFER_DATA && n != 8))
        return BB_COMPONENT_BAD_REQUEST;
    if (bb_component_get_u64(p) != r->transfer_id || r->transfer_id == 0)
        return BB_COMPONENT_SESSION;
    if (command == BB_COMPONENT_TRANSFER_QUERY) {
        memset(reply, 0, BB_COMPONENT_QUERY_BYTES);
        reply[0] = r->state;
        bb_component_put_u32(reply + 4, r->received);
        bb_component_put_u32(reply + 8, r->total);
        bb_component_put_u64(reply + 12, r->transfer_id);
        memcpy(reply + 20, r->sha256, 32);
        *reply_n = BB_COMPONENT_QUERY_BYTES;
        return BB_COMPONENT_OK;
    }
    if (command == BB_COMPONENT_TRANSFER_ABORT) {
        if (r->state == BB_COMPONENT_ACTIVATED) return BB_COMPONENT_BUSY;
        abort_transfer(r);
        return BB_COMPONENT_OK;
    }
    if (command == BB_COMPONENT_TRANSFER_DATA) {
        const uint32_t offset = bb_component_get_u32(p + 8);
        const size_t count = n - 12;
        if (r->last_length && offset == r->last_offset &&
            count == r->last_length && memcmp(p + 12, r->last_chunk, count) == 0)
            return BB_COMPONENT_OK;
        if (r->state != BB_COMPONENT_RECEIVING) return BB_COMPONENT_SESSION;
        if (offset != r->received) return BB_COMPONENT_OFFSET;
        if (count > r->total - r->received) return BB_COMPONENT_TOO_LARGE;
        if (offset == 0) {
            bb_component_image_t image;
            if (!bb_component_image_parse(p + 12, count, &image) ||
                image.profile != r->profile) {
                abort_transfer(r);
                return BB_COMPONENT_WRONG_IMAGE;
            }
        }
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
    if (command == BB_COMPONENT_TRANSFER_COMMIT) {
        if (r->state == BB_COMPONENT_COMMITTED || r->state == BB_COMPONENT_ACTIVATED)
            return BB_COMPONENT_OK;
        if (r->state != BB_COMPONENT_RECEIVING) return BB_COMPONENT_SESSION;
        if (r->received != r->total) return BB_COMPONENT_OFFSET;
        const uint16_t status = r->ops.finish(r->context, r->sha256);
        if (status != BB_COMPONENT_OK) {
            abort_transfer(r);
            return status;
        }
        r->state = BB_COMPONENT_COMMITTED;
        return BB_COMPONENT_OK;
    }
    if (r->state == BB_COMPONENT_ACTIVATED) return BB_COMPONENT_OK;
    if (r->state != BB_COMPONENT_COMMITTED) return BB_COMPONENT_NOT_READY;
    const uint16_t status = r->ops.activate(r->context);
    if (status == BB_COMPONENT_OK) r->state = BB_COMPONENT_ACTIVATED;
    return status;
}
