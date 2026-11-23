#include "ui_internal.h"

#include <stdio.h>
#include <stdlib.h>

static const char *const icon_files[UI_ICON_COUNT] = {
    [UI_ICON_ALARM_CLOCK] = "alarm-clock.png",
    [UI_ICON_BATTERY_FULL] = "battery-full.png",
    [UI_ICON_BELL] = "bell.png",
    [UI_ICON_BLUETOOTH] = "bluetooth.png",
    [UI_ICON_BRAIN] = "brain.png",
    [UI_ICON_CAMERA] = "camera.png",
    [UI_ICON_CHECK] = "check.png",
    [UI_ICON_CHEVRON_DOWN] = "chevron-down.png",
    [UI_ICON_CHEVRON_LEFT] = "chevron-left.png",
    [UI_ICON_CHEVRON_RIGHT] = "chevron-right.png",
    [UI_ICON_CLOUD_SUN] = "cloud-sun.png",
    [UI_ICON_COFFEE] = "coffee.png",
    [UI_ICON_DROPLETS] = "droplets.png",
    [UI_ICON_FLOWER_2] = "flower-2.png",
    [UI_ICON_HEART] = "heart.png",
    [UI_ICON_HAND] = "hand.png",
    [UI_ICON_LEAF] = "leaf.png",
    [UI_ICON_LIGHTBULB] = "lightbulb.png",
    [UI_ICON_LIST_TODO] = "list-todo.png",
    [UI_ICON_LIST_MUSIC] = "list-music.png",
    [UI_ICON_MIC] = "mic.png",
    [UI_ICON_MONITOR] = "monitor.png",
    [UI_ICON_MOON] = "moon.png",
    [UI_ICON_MORE_HORIZONTAL] = "more-horizontal.png",
    [UI_ICON_PAUSE] = "pause.png",
    [UI_ICON_PLAY] = "play.png",
    [UI_ICON_PLUS] = "plus.png",
    [UI_ICON_POWER] = "power.png",
    [UI_ICON_ROTATE_CCW] = "rotate-ccw.png",
    [UI_ICON_REPEAT_2] = "repeat-2.png",
    [UI_ICON_SIGNAL] = "signal.png",
    [UI_ICON_SETTINGS] = "settings.png",
    [UI_ICON_SPARKLES] = "sparkles.png",
    [UI_ICON_SPEAKER] = "speaker.png",
    [UI_ICON_SKIP_BACK] = "skip-back.png",
    [UI_ICON_SKIP_FORWARD] = "skip-forward.png",
    [UI_ICON_SUN] = "sun.png",
    [UI_ICON_THERMOMETER] = "thermometer.png",
    [UI_ICON_TIMER] = "timer.png",
    [UI_ICON_VOLUME_2] = "volume-2.png",
    [UI_ICON_WIFI] = "wifi.png",
    [UI_ICON_WIND] = "wind.png",
    [UI_ICON_ZAP] = "zap.png",
};

static lv_obj_t *icon_image(lv_obj_t *icon)
{
    return icon && lv_obj_get_child_count(icon) > 0 ? lv_obj_get_child(icon, 0) : NULL;
}

/*
 * The LCD uses an RGB666 physical bus. Scaling a 48 px alpha bitmap at
 * runtime still creates several nearly-identical edge colors which can
 * collapse into visible stair steps after display-pipeline quantization.
 * Use a small set of optical sizes, all rendered directly from the Lucide
 * SVG sources, so LVGL only has to composite 1:1.
 */
static int32_t physical_icon_size(int32_t requested_size)
{
    if(requested_size <= 14) return 18;
    if(requested_size <= 18) return 24;
    if(requested_size <= 22) return 28;
    if(requested_size <= 28) return 32;
    if(requested_size <= 34) return 38;
    return 44;
}

static void set_source(lv_obj_t *image, ui_icon_name_t name, int32_t size)
{
    if(!image || name < 0 || name >= UI_ICON_COUNT || !icon_files[name]) return;
    const char *asset_root = getenv("BINBOT_ASSET_ROOT");
    if(!asset_root || !asset_root[0]) asset_root = "/oem/assets";
    char path[384];
    if(asset_root[0] == 'A' && asset_root[1] == ':') {
        snprintf(path, sizeof(path), "%s/icons/lucide/png/%d/%s",
                 asset_root, (int)size, icon_files[name]);
    }
    else {
        snprintf(path, sizeof(path), "A:%s/icons/lucide/png/%d/%s",
                 asset_root, (int)size, icon_files[name]);
    }
    lv_image_set_src(image, path);
}

lv_obj_t *ui_icon(lv_obj_t *parent, ui_icon_name_t name, int32_t size, uint32_t color)
{
    const int32_t physical_size = physical_icon_size(size);
    lv_obj_t *holder = ui_box(parent, 0, 0, physical_size, physical_size, 0x000000, 0);
    lv_obj_set_style_bg_opa(holder, LV_OPA_TRANSP, 0);
    lv_obj_t *image = lv_image_create(holder);
    lv_obj_remove_flag(image, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_image_set_antialias(image, false);
    lv_obj_set_style_image_recolor(image, lv_color_hex(color), 0);
    lv_obj_set_style_image_recolor_opa(image, LV_OPA_COVER, 0);
    set_source(image, name, physical_size);
    lv_obj_center(image);
    return holder;
}

void ui_icon_set_name(lv_obj_t *icon, ui_icon_name_t name)
{
    if(icon) {
        int32_t size = lv_obj_get_width(icon);
        /* A name can be changed from an event/update callback before LVGL has
         * resolved the holder's layout.  Never construct a /0/ asset path. */
        if(size <= 0) size = 24;
        set_source(icon_image(icon), name, physical_icon_size(size));
    }
}

void ui_icon_set_color(lv_obj_t *icon, uint32_t color)
{
    lv_obj_t *image = icon_image(icon);
    if(image) lv_obj_set_style_image_recolor(image, lv_color_hex(color), 0);
}

void ui_icon_set_visible(lv_obj_t *icon, bool visible)
{
    if(!icon) return;
    if(visible) lv_obj_remove_flag(icon, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(icon, LV_OBJ_FLAG_HIDDEN);
}
