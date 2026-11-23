#include "assistant_orb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

enum { ORB_SIZE = 464, ORB_FRAME_BYTES = ORB_SIZE * ORB_SIZE * 3, ORB_MAX_FRAMES = 120 };

struct ui_assistant_orb {
    lv_obj_t *image;
    lv_timer_t *timer;
    lv_image_dsc_t descriptor;
    FILE *file;
    uint8_t *pixels, *compressed;
    uint32_t offsets[ORB_MAX_FRAMES], lengths[ORB_MAX_FRAMES];
    unsigned state, count, frame;
    bool reverse, paused;
};

static uint32_t little32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static bool render_frame(ui_assistant_orb_t *orb)
{
    uLongf size = ORB_FRAME_BYTES;
    uint32_t length = orb->lengths[orb->frame];
    if(fseek(orb->file, orb->offsets[orb->frame], SEEK_SET) ||
       fread(orb->compressed, 1, length, orb->file) != length ||
       uncompress(orb->pixels, &size, orb->compressed, length) != Z_OK || size != ORB_FRAME_BYTES) {
        lv_timer_pause(orb->timer);
        return false;
    }
    lv_obj_invalidate(orb->image);
    return true;
}

static void animate(lv_timer_t *timer)
{
    ui_assistant_orb_t *orb = lv_timer_get_user_data(timer);
    if(!orb->file || orb->paused) return;
    /* Reflect at each end so the original non-periodic canvas mesh breathes
     * continuously without a visible jump or a large in-memory frame cache. */
    if(orb->reverse) {
        if(orb->frame) --orb->frame;
        if(!orb->frame) orb->reverse = false;
    } else {
        if(orb->frame + 1 < orb->count) ++orb->frame;
        if(orb->frame + 1 >= orb->count) orb->reverse = true;
    }
    (void)render_frame(orb);
}

void ui_assistant_orb_set_state(ui_assistant_orb_t *orb, unsigned state)
{
    if(!orb || state > 3 || state == orb->state) return;
    static const char *const names[] = {"idle", "listening", "thinking", "reply"};
    const char *root = getenv("BINBOT_ASSET_ROOT");
    if(!root || !root[0]) root = "/oem/assets";
    if(root[0] == 'A' && root[1] == ':') root += 2;
    char path[512];
    snprintf(path, sizeof(path), "%s/reference/assistant/%s.orb", root, names[state]);
    FILE *file = fopen(path, "rb");
    uint8_t header[20], entry[8];
    uint32_t offsets[ORB_MAX_FRAMES], lengths[ORB_MAX_FRAMES];
    if(!file) goto invalid;
    if(fread(header, 1, sizeof(header), file) != sizeof(header) ||
       memcmp(header, "BBORB002", 8) ||
       (header[8] | header[9] << 8) != ORB_SIZE ||
       (header[10] | header[11] << 8) != ORB_SIZE) goto invalid;
    unsigned count = header[12] | header[13] << 8, fps = header[14] | header[15] << 8;
    if(count < 2 || count > ORB_MAX_FRAMES || fps < 1 || fps > 30) goto invalid;
    for(unsigned i = 0; i < count; ++i) {
        if(fread(entry, 1, sizeof(entry), file) != sizeof(entry)) goto invalid;
        uint32_t offset = little32(entry), length = little32(entry + 4);
        if(offset < 20 + count * 8 || !length || length > ORB_FRAME_BYTES + 1024) goto invalid;
        offsets[i] = offset;
        lengths[i] = length;
    }
    if(orb->file) fclose(orb->file);
    orb->file = file;
    memcpy(orb->offsets, offsets, count * sizeof(offsets[0]));
    memcpy(orb->lengths, lengths, count * sizeof(lengths[0]));
    orb->count = count;
    orb->frame = 0;
    orb->reverse = false;
    orb->state = state;
    lv_timer_set_period(orb->timer, 1000 / fps);
    if(render_frame(orb) && !orb->paused) lv_timer_resume(orb->timer);
    lv_timer_reset(orb->timer);
    return;
invalid:
    if(file) fclose(file);
    fprintf(stderr, "ui: AI orb asset unavailable: %s\n", path);
    lv_timer_pause(orb->timer);
}

void ui_assistant_orb_set_paused(ui_assistant_orb_t *orb, bool paused)
{
    if(!orb) return;
    orb->paused = paused;
    if(paused) lv_timer_pause(orb->timer);
    else if(orb->file) { lv_timer_resume(orb->timer); lv_timer_reset(orb->timer); }
}

ui_assistant_orb_t *ui_assistant_orb_create(lv_obj_t *parent)
{
    ui_assistant_orb_t *orb = calloc(1, sizeof(*orb));
    if(!orb) return NULL;
    orb->pixels = calloc(1, ORB_FRAME_BYTES);
    orb->compressed = malloc(ORB_FRAME_BYTES + 1024);
    if(!orb->pixels || !orb->compressed) { ui_assistant_orb_destroy(orb); return NULL; }
    orb->descriptor = (lv_image_dsc_t){.header = {.magic = LV_IMAGE_HEADER_MAGIC,
        .cf = LV_COLOR_FORMAT_RGB888, .w = ORB_SIZE, .h = ORB_SIZE, .stride = ORB_SIZE * 3},
        .data_size = ORB_FRAME_BYTES, .data = orb->pixels};
    orb->image = lv_image_create(parent);
    lv_obj_remove_flag(orb->image, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_pos(orb->image, 128, 30);
    orb->timer = lv_timer_create(animate, 83, orb);
    orb->state = 4;
    ui_assistant_orb_set_state(orb, 0);
    if(orb->file) lv_image_set_src(orb->image, &orb->descriptor);
    return orb;
}

void ui_assistant_orb_destroy(ui_assistant_orb_t *orb)
{
    if(!orb) return;
    if(orb->timer) lv_timer_delete(orb->timer);
    if(orb->image) lv_obj_delete(orb->image);
    if(orb->file) fclose(orb->file);
    free(orb->pixels);
    free(orb->compressed);
    free(orb);
}
