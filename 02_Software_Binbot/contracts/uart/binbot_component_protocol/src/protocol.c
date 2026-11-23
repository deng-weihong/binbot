#include "binbot/component/protocol.h"

#include <string.h>

uint16_t bb_component_get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}

uint32_t bb_component_get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

uint64_t bb_component_get_u64(const uint8_t *p)
{
    return bb_component_get_u32(p) | (uint64_t)bb_component_get_u32(p + 4) << 32;
}

void bb_component_put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

void bb_component_put_u32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i));
}

void bb_component_put_u64(uint8_t *p, uint64_t v)
{
    for (unsigned i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (8 * i));
}

uint32_t bb_component_crc32(const uint8_t *p, size_t n)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

size_t bb_component_encode(const bb_component_frame_t *f, uint8_t *p, size_t cap)
{
    if (!f || !p || f->length > BB_COMPONENT_MAX_PAYLOAD ||
        cap < (size_t)f->length + 14) return 0;
    p[0] = 0x42;
    p[1] = 0x43;
    p[2] = BB_COMPONENT_VERSION;
    p[3] = f->type;
    bb_component_put_u16(p + 4, f->sequence);
    bb_component_put_u16(p + 6, f->command);
    bb_component_put_u16(p + 8, f->length);
    memcpy(p + 10, f->payload, f->length);
    bb_component_put_u32(p + 10 + f->length,
                         bb_component_crc32(p + 2, 8 + f->length));
    return 14 + f->length;
}

static void discard(bb_component_parser_t *p, size_t n)
{
    p->length -= n;
    memmove(p->bytes, p->bytes + n, p->length);
}

bool bb_component_parser_next(bb_component_parser_t *p, bb_component_frame_t *f)
{
    while (p->length >= 2) {
        if (p->bytes[0] != 0x42 || p->bytes[1] != 0x43) {
            discard(p, 1);
            continue;
        }
        if (p->length < 10) return false;
        const uint16_t n = bb_component_get_u16(p->bytes + 8);
        if (p->bytes[2] != BB_COMPONENT_VERSION ||
            (p->bytes[3] != BB_COMPONENT_REQUEST &&
             p->bytes[3] != BB_COMPONENT_RESPONSE &&
             p->bytes[3] != BB_COMPONENT_EVENT) ||
            n > BB_COMPONENT_MAX_PAYLOAD) {
            discard(p, 1);
            continue;
        }
        if (p->length < (size_t)n + 14) return false;
        if (bb_component_get_u32(p->bytes + 10 + n) !=
            bb_component_crc32(p->bytes + 2, 8 + n)) {
            discard(p, 1);
            continue;
        }
        f->type = p->bytes[3];
        f->sequence = bb_component_get_u16(p->bytes + 4);
        f->command = bb_component_get_u16(p->bytes + 6);
        f->length = n;
        memcpy(f->payload, p->bytes + 10, n);
        discard(p, n + 14);
        return true;
    }
    return false;
}

bool bb_component_parser_push(bb_component_parser_t *p, uint8_t byte,
                              bb_component_frame_t *f)
{
    if (p->length == sizeof(p->bytes)) discard(p, 1);
    p->bytes[p->length++] = byte;
    return bb_component_parser_next(p, f);
}

void bb_component_announcement_init(bb_component_announcement_t *state, uint64_t boot_id)
{
    memset(state, 0, sizeof(*state));
    state->boot_id = boot_id;
}

bool bb_component_announcement_due(bb_component_announcement_t *state, bool healthy,
                                    int64_t now_ms, uint16_t *sequence)
{
    if (!state->state_known || state->healthy != healthy) {
        state->state_known = true;
        state->healthy = healthy;
        state->confirmed = false;
        state->next_send_ms = now_ms;
        if (++state->sequence == 0) ++state->sequence;
    }
    if (state->confirmed || now_ms < state->next_send_ms) return false;
    state->next_send_ms = now_ms + BB_COMPONENT_ANNOUNCE_RETRY_MS;
    *sequence = state->sequence;
    return true;
}

bool bb_component_announcement_ack(bb_component_announcement_t *state,
                                    const bb_component_frame_t *frame)
{
    if (!state->state_known || frame->type != BB_COMPONENT_RESPONSE ||
        frame->command != BB_COMPONENT_ANNOUNCE || frame->sequence != state->sequence ||
        frame->length != BB_COMPONENT_ANNOUNCE_ACK_BYTES ||
        bb_component_get_u16(frame->payload) != BB_COMPONENT_OK ||
        bb_component_get_u64(frame->payload + 2) != state->boot_id) return false;
    state->confirmed = true;
    return true;
}

void bb_component_announcement_observed(bb_component_announcement_t *state, bool healthy)
{
    if (state->state_known && state->healthy == healthy) state->confirmed = true;
}

const char *bb_component_project(uint8_t profile)
{
    switch (profile) {
    case BB_COMPONENT_ROUND_SCREEN: return "binbot_round_screen";
    case BB_COMPONENT_HOLOGRAPHIC_SCREEN: return "binbot_holographic_screen";
    case BB_COMPONENT_SHOWCASE_TURNTABLE: return "binbot_showcase_turntable";
    case BB_COMPONENT_AMBIENT_LIGHT: return "binbot_ambient_light";
    default: return NULL;
    }
}

const char *bb_component_status_name(uint16_t status)
{
    static const char *const names[] = {
        "ok", "bad-request", "unsupported", "busy", "session-mismatch",
        "offset-mismatch", "too-large", "wrong-image", "hash-mismatch",
        "storage-error", "not-ready"
    };
    return status < sizeof(names) / sizeof(names[0]) ? names[status] : "unknown";
}

uint8_t bb_component_file_id_from_name(const char *name)
{
    if (!name) return 0;
    if (strcmp(name, "display.gif") == 0) return BB_COMPONENT_FILE_DISPLAY_GIF;
    return 0;
}

const char *bb_component_file_name(uint8_t file_id)
{
    switch (file_id) {
    case BB_COMPONENT_FILE_DISPLAY_GIF: return "display.gif";
    default: return NULL;
    }
}

bool bb_component_image_parse(const uint8_t *p, size_t n, bb_component_image_t *i)
{
    if (!p || !i || n < BB_COMPONENT_IMAGE_PREFIX_BYTES || p[0] != 0xe9 ||
        bb_component_get_u16(p + 12) != 5 ||
        bb_component_get_u32(p + 32) != 0xabcd5432 ||
        !memchr(p + 48, 0, 32) || !memchr(p + 80, 0, 32)) return false;
    memset(i, 0, sizeof(*i));
    memcpy(i->version, p + 48, 32);
    memcpy(i->project, p + 80, 32);
    memcpy(i->elf_sha256, p + 176, 32);
    for (uint8_t profile = BB_COMPONENT_ROUND_SCREEN; profile <= BB_COMPONENT_AMBIENT_LIGHT; ++profile) {
        const char *project = bb_component_project(profile);
        if (project && strcmp(i->project, project) == 0) {
            i->profile = profile;
            return true;
        }
    }
    return false;
}
