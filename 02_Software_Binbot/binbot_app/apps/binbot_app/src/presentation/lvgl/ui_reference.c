#include "ui_internal.h"
#include "libs/freetype/lv_freetype.h"
#include "draw/snapshot/lv_snapshot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char surfaces[96][384];
static char surface_names[96][40];

static const char *asset_root(void)
{
    const char *root = getenv("BINBOT_ASSET_ROOT");
    return root && root[0] ? root : "/oem/assets";
}

const lv_font_t *ui_ref_font(uint16_t size, ui_font_weight_t weight)
{
    const char *face = weight >= UI_FONT_SEMIBOLD ? "SemiBold" :
                       weight == UI_FONT_MEDIUM ? "Medium" : "Regular";
    char primary[96], fallback[96];
    snprintf(primary, sizeof(primary), "fonts/InterTight-%s.ttf", face);
    snprintf(fallback, sizeof(fallback), "fonts/NotoSansSC-%s.ttf", face);
    return ui_resources_font(primary, fallback, size);
}

static void backdrop_deleted(lv_event_t *event)
{
    lv_draw_buf_destroy(lv_event_get_user_data(event));
}

lv_obj_t *ui_ref_backdrop(lv_obj_t *parent, lv_obj_t *source, uint32_t tint,
                         uint8_t opacity, uint8_t blur_radius)
{
    lv_draw_buf_t *snapshot = lv_snapshot_take(source, LV_COLOR_FORMAT_RGB565);
    if(!snapshot) return NULL;
    const int scale = blur_radius < 8 ? 2 : 8;
    const int width = (snapshot->header.w + scale - 1) / scale;
    const int height = (snapshot->header.h + scale - 1) / scale;
    lv_draw_buf_t *blur = lv_draw_buf_create(width, height, LV_COLOR_FORMAT_XRGB8888, LV_STRIDE_AUTO);
    uint8_t *work = malloc((size_t)width * height * 3);
    if(!blur || !work) {
        if(blur) lv_draw_buf_destroy(blur);
        lv_draw_buf_destroy(snapshot);
        free(work);
        return NULL;
    }
    /* Downsample before the blur so each overlay retains only a small bitmap. */
    for(int y = 0; y < height; ++y) {
        for(int x = 0; x < width; ++x) {
            unsigned sum[3] = {0}, count = 0;
            for(int sy = y * scale; sy < (y + 1) * scale && sy < (int)snapshot->header.h; ++sy) {
                const uint16_t *row = (const uint16_t *)(snapshot->data + sy * snapshot->header.stride);
                for(int sx = x * scale; sx < (x + 1) * scale && sx < (int)snapshot->header.w; ++sx) {
                    uint16_t color = row[sx];
                    sum[0] += (color & 31) * 255 / 31;
                    sum[1] += ((color >> 5) & 63) * 255 / 63;
                    sum[2] += (color >> 11) * 255 / 31;
                    ++count;
                }
            }
            uint8_t *pixel = blur->data + y * blur->header.stride + x * 4;
            for(int channel = 0; channel < 3; ++channel) pixel[channel] = sum[channel] / count;
            pixel[3] = 255;
        }
    }
    lv_draw_buf_destroy(snapshot);

    /* Three separable box passes approximate the supplied CSS backdrop blur. */
    const int radius = blur_radius / scale;
    for(int pass = 0; pass < 3; ++pass) {
        for(int y = 0; y < height; ++y) {
            for(int x = 0; x < width; ++x) {
                for(int channel = 0; channel < 3; ++channel) {
                    unsigned sum = 0;
                    for(int offset = -radius; offset <= radius; ++offset) {
                        int sx = x + offset;
                        if(sx < 0) sx = 0;
                        if(sx >= width) sx = width - 1;
                        sum += blur->data[y * blur->header.stride + sx * 4 + channel];
                    }
                    work[(y * width + x) * 3 + channel] = sum / (radius * 2 + 1);
                }
            }
        }
        for(int y = 0; y < height; ++y) {
            for(int x = 0; x < width; ++x) {
                for(int channel = 0; channel < 3; ++channel) {
                    unsigned sum = 0;
                    for(int offset = -radius; offset <= radius; ++offset) {
                        int sy = y + offset;
                        if(sy < 0) sy = 0;
                        if(sy >= height) sy = height - 1;
                        sum += work[(sy * width + x) * 3 + channel];
                    }
                    blur->data[y * blur->header.stride + x * 4 + channel] = sum / (radius * 2 + 1);
                }
            }
        }
    }
    free(work);
    for(int y = 0; y < height; ++y) {
        for(int x = 0; x < width; ++x) {
            uint8_t *pixel = blur->data + y * blur->header.stride + x * 4;
            for(int channel = 0; channel < 3; ++channel) {
                unsigned color = (tint >> (channel * 8)) & 255;
                pixel[channel] = (pixel[channel] * (255 - opacity) + color * opacity) / 255;
            }
        }
    }
    lv_obj_t *image = lv_image_create(parent);
    lv_image_set_src(image, blur);
    lv_image_set_pivot(image, 0, 0);
    lv_image_set_scale(image, scale * 256);
    lv_obj_remove_flag(image, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_pos(image, 0, lv_obj_get_y(source));
    lv_obj_move_background(image);
    lv_obj_add_event_cb(image, backdrop_deleted, LV_EVENT_DELETE, blur);
    return image;
}

void ui_ref_shadow(lv_obj_t *object, uint32_t color, lv_opa_t opacity,
                    int32_t width, int32_t spread, int32_t offset_y)
{
    lv_obj_set_style_shadow_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_shadow_opa(object, opacity, 0);
    lv_obj_set_style_shadow_width(object, width, 0);
    lv_obj_set_style_shadow_spread(object, spread, 0);
    lv_obj_set_style_shadow_offset_y(object, offset_y, 0);
}

void ui_ref_surface(lv_obj_t *object, const char *name)
{
    if(!object || !name) return;
    static const char *cards[] = {
        "circle50", "setting_slider", "setting_network", "privacy", "todo_row",
        "todo_entry", "word", "word_counter", "word_answer", "word_complete",
        "control_tile", "control_music", "playlist",
        "apps_tile", "settings_slider_latest", "settings_network_latest", "privacy_latest",
        "wifi_password", "alarm_row",
        "focus_dial", "focus_reset", "chat_input", "alarm_dial",
        "alarm_row_latest", "alarm_editor", "chat_status",
        "desk_tip", "desk_pc", "desk_lamp", "desk_status", "desk_scene",
        "camera_album", "camera_modes", "lunch_category", "lunch_budget", "apps_tile_extended",
        "modules_panel", "modules_tile", "modules_slot", "desk_charge",
        "settings_network_modules", "settings_voice_modules", "settings_about_modules",
        "about_summary", "about_info", "about_storage", "about_links", "about_action", "module_notice",
        "bluetooth_wait", "bluetooth_list", "bluetooth_list_connected", "noise_apps_tile",
    };
    for(size_t i = 0; i < sizeof(cards) / sizeof(cards[0]); ++i) {
        if(strcmp(name, cards[i]) != 0) continue;
        lv_obj_set_style_shadow_color(object, lv_color_hex(0x000000), 0);
        lv_obj_set_style_shadow_opa(object, 31, 0);
        lv_obj_set_style_shadow_width(object, 30, 0);
        lv_obj_set_style_shadow_spread(object, -12, 0);
        lv_obj_set_style_shadow_offset_y(object, 10, 0);
        break;
    }
    static const char *companion_cards[] = {
        "focus_dial", "focus_reset", "chat_input", "chat_status",
        "alarm_dial", "alarm_row_latest", "alarm_editor",
        "bluetooth_wait", "bluetooth_list", "bluetooth_list_connected",
    };
    for(size_t i = 0; i < sizeof(companion_cards) / sizeof(companion_cards[0]); ++i) {
        if(strcmp(name, companion_cards[i]) == 0) {
            ui_ref_shadow(object, 0x000000, 31, 60, -12, 10);
            break;
        }
    }
    if(strncmp(name, "cover", 5) == 0) {
        lv_obj_set_style_shadow_color(object, lv_color_hex(0x000000), 0);
        lv_obj_set_style_shadow_opa(object, 128, 0);
        lv_obj_set_style_shadow_width(object, 60, 0);
        lv_obj_set_style_shadow_spread(object, -24, 0);
        lv_obj_set_style_shadow_offset_y(object, 30, 0);
    }
    if(strcmp(name, "control_panel") == 0) {
        lv_obj_set_style_shadow_color(object, lv_color_hex(0x000000), 0);
        lv_obj_set_style_shadow_opa(object, 89, 0);
        lv_obj_set_style_shadow_width(object, 70, 0);
        lv_obj_set_style_shadow_spread(object, -20, 0);
        lv_obj_set_style_shadow_offset_y(object, 30, 0);
    }
    for(size_t i = 0; i < sizeof(surfaces) / sizeof(surfaces[0]); ++i) {
        if(surface_names[i][0] && strcmp(surface_names[i], name) != 0) continue;
        if(!surface_names[i][0]) {
            snprintf(surface_names[i], sizeof(surface_names[i]), "%s", name);
            const char *root = asset_root();
            snprintf(surfaces[i], sizeof(surfaces[i]), "%s%s/reference/surfaces/%s.bin",
                     root[0] == 'A' && root[1] == ':' ? "" : "A:", root, name);
        }
        lv_obj_set_style_bg_image_src(object, surfaces[i], 0);
        lv_obj_set_style_bg_image_opa(object, LV_OPA_COVER, 0);
        return;
    }
}

lv_obj_t *ui_ref_label(lv_obj_t *parent, const char *text, uint16_t size,
                       ui_font_weight_t weight, uint32_t color, int32_t x, int32_t y)
{
    return ui_label(parent, text, ui_ref_font(size, weight), color, x, y);
}

void ui_ref_icon_set_name(lv_obj_t *icon, const char *name)
{
    if(!icon || !name || lv_obj_get_child_count(icon) == 0) return;
    int32_t size = lv_obj_get_style_width(icon, 0);
    char path[384];
    const char *root = asset_root();
    snprintf(path, sizeof(path), "%s%s/reference/icons/%ld/%s.png",
             root[0] == 'A' && root[1] == ':' ? "" : "A:", root, (long)size, name);
    lv_image_set_src(lv_obj_get_child(icon, 0), path);
}

lv_obj_t *ui_ref_icon(lv_obj_t *parent, const char *name, int32_t size, uint32_t color)
{
    lv_obj_t *icon = ui_box(parent, 0, 0, size, size, 0, 0);
    lv_obj_set_style_bg_opa(icon, LV_OPA_TRANSP, 0);
    lv_obj_t *image = lv_image_create(icon);
    lv_obj_remove_flag(image, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_style_image_recolor(image, lv_color_hex(color), 0);
    lv_obj_set_style_image_recolor_opa(image, LV_OPA_COVER, 0);
    ui_ref_icon_set_name(icon, name);
    return icon;
}

lv_obj_t *ui_ref_button(lv_obj_t *parent, int32_t x, int32_t y, int32_t width,
                        int32_t height, int32_t radius, const char *surface)
{
    lv_obj_t *button = ui_button(parent, x, y, width, height, 0xffffff, radius);
    if(surface) ui_ref_surface(button, surface);
    return button;
}

static void reference_back(lv_event_t *event)
{
    ui_app_t *app = lv_event_get_user_data(event);
    ui_app_go_back(app);
}

lv_obj_t *ui_ref_page(ui_app_t *app)
{
    lv_obj_t *root = ui_box(app->page_layer, 0, 0, UI_WIDTH, UI_HEIGHT, 0xefefef, 0);
    ui_ref_surface(root, "page");
    lv_obj_t *handle = ui_box(root, 317, 7, 86, 5, 0x000000, 3);
    lv_obj_set_style_bg_opa(handle, 38, 0);
    return root;
}

lv_obj_t *ui_ref_shell(ui_app_t *app, const char *title, const char *en)
{
    lv_obj_t *root = ui_ref_page(app);
    lv_obj_t *back = ui_ref_button(root, 29, 25, 50, 50, 25, "circle50");
    lv_obj_t *icon = ui_ref_icon(back, "back", 22, UI_REF_INK);
    lv_obj_center(icon);
    lv_obj_add_event_cb(back, reference_back, LV_EVENT_CLICKED, app);
    ui_ref_label(root, en, 11, UI_FONT_REGULAR, UI_REF_FAINT, 93, 27);
    ui_ref_label(root, title, 22, UI_FONT_SEMIBOLD, UI_REF_INK, 93, 45);
    return root;
}

lv_obj_t *ui_ref_shell_latest(ui_app_t *app, const char *title, const char *en)
{
    lv_obj_t *root = ui_ref_page(app);
    lv_obj_t *back = ui_ref_button(root, 29, 26, 50, 50, 25, "circle50");
    lv_obj_t *icon = ui_ref_icon(back, "back", 25, UI_REF_INK);
    lv_obj_center(icon);
    lv_obj_add_event_cb(back, reference_back, LV_EVENT_CLICKED, app);
    ui_ref_label(root, en, 15, UI_FONT_REGULAR, UI_REF_FAINT, 93, 27);
    ui_ref_label(root, title, 24, UI_FONT_SEMIBOLD, UI_REF_INK, 93, 48);
    return root;
}

lv_obj_t *ui_ref_ring(lv_obj_t *parent, int32_t x, int32_t y, int32_t size, int32_t width)
{
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_remove_style_all(arc);
    lv_obj_set_pos(arc, x, y);
    lv_obj_set_size(arc, size, size);
    lv_arc_set_rotation(arc, 270);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_range(arc, 0, 100);
    lv_obj_set_style_arc_width(arc, width, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(0xe6e6e6), LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(UI_REF_CLAY), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
    return arc;
}
