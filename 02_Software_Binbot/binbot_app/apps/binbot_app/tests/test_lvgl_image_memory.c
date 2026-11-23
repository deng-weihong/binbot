#include "lvgl.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t pixels[40 * 40 * 3];
static uint32_t frame[160 * 16];

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *data)
{
    (void)area;
    (void)data;
    lv_display_flush_ready(display);
}

int main(void)
{
    lv_init();
    lv_display_t *display = lv_display_create(160, 80);
    assert(display);
    lv_display_set_buffers(display, frame, NULL, sizeof(frame), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    memset(pixels, 220, sizeof(pixels));
    const lv_image_dsc_t surface = {
        .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB888,
                   .w = 40, .h = 40, .stride = 120},
        .data_size = sizeof(pixels),
        .data = pixels,
    };
    lv_obj_t *screen = lv_screen_active();
    /* More radii than the circle cache holds exercises temporary mask ownership. */
    for(int i = 0; i < 8; ++i) {
        lv_obj_t *card = lv_obj_create(screen);
        lv_obj_remove_style_all(card);
        lv_obj_set_pos(card, (i % 4) * 40, (i / 4) * 40);
        lv_obj_set_size(card, 40, 40);
        lv_obj_set_style_radius(card, 5 + i, 0);
        lv_obj_set_style_bg_image_src(card, &surface, 0);
        lv_obj_set_style_bg_image_opa(card, LV_OPA_COVER, 0);
    }
    for(int i = 0; i < 10; ++i) {
        lv_obj_invalidate(screen);
        lv_refr_now(display);
    }
    lv_mem_monitor_t before, after;
    lv_mem_monitor(&before);
    for(int i = 0; i < 40; ++i) {
        lv_obj_invalidate(screen);
        lv_refr_now(display);
    }
    lv_mem_monitor(&after);
    printf("Rounded image redraws: free before=%zu after=%zu\n", before.free_size, after.free_size);
    fflush(stdout);
    assert(after.free_size + 4096 >= before.free_size);
    assert(lv_mem_test() == LV_RESULT_OK);
    lv_display_delete(display);
    lv_deinit();
    return 0;
}
