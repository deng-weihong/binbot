#include "ambient_internal.h"
#include <stdio.h>
#include <string.h>

lv_obj_t *ambient_box(lv_obj_t *parent, int x, int y, int width, int height,
                       uint32_t color, int radius)
{
    lv_obj_t *box = lv_obj_create(parent);
    if(!box) return NULL;
    lv_obj_remove_style_all(box);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE |
                            LV_OBJ_FLAG_CLICK_FOCUSABLE | LV_OBJ_FLAG_EVENT_BUBBLE |
                            LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, width, height);
    lv_obj_set_style_bg_color(box, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, radius, 0);
    return box;
}

lv_obj_t *ambient_text(binbot_ambient_display_t *display, lv_obj_t *parent,
                        const char *text, unsigned size, unsigned weight,
                        uint32_t color, int x, int y)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_label_set_text(label, text);
    const lv_font_t *font = display->config.font ?
        display->config.font(display->config.user_data, label, (uint16_t)size, weight) : LV_FONT_DEFAULT;
    lv_obj_set_style_text_font(label, font ? font : LV_FONT_DEFAULT, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_pos(label, x, y);
    return label;
}

void ambient_text_set(lv_obj_t *label, const char *text)
{
    if(strcmp(lv_label_get_text(label), text)) lv_label_set_text(label, text);
}

void ambient_asset_path(const binbot_ambient_display_t *display, const char *relative,
                         char *path, size_t capacity)
{
    const char *root = display->asset_root;
    snprintf(path, capacity, "%s%s/%s", root[0] == 'A' && root[1] == ':' ? "" : "A:", root, relative);
}

