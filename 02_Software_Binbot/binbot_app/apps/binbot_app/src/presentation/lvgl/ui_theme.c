#include "ui_internal.h"

#include "libs/freetype/lv_freetype.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static const char *font_file(bool display, ui_font_weight_t weight)
{
    if(display) {
        if(weight == UI_FONT_EXTRABOLD) return "fonts/Manrope-ExtraBold.ttf";
        if(weight >= UI_FONT_SEMIBOLD) return "fonts/Manrope-Bold.ttf";
        return "fonts/Manrope-SemiBold.ttf";
    }
    if(weight >= UI_FONT_SEMIBOLD) return "fonts/NotoSansSC-Bold.ttf";
    if(weight == UI_FONT_MEDIUM) return "fonts/NotoSansSC-SemiBold.ttf";
    return "fonts/NotoSansSC-Medium.ttf";
}

void ui_theme_initialize(void)
{
    /* Font paths are resolved only when a visible control requests a font. */
}

void ui_theme_shutdown(void) { ui_resources_shutdown(); }

const lv_font_t *ui_font_body(void)
{ return ui_resources_font("fonts/NotoSansSC-Medium.ttf", NULL, 16); }
const lv_font_t *ui_font_body_large(void)
{ return ui_resources_font("fonts/NotoSansSC-Bold.ttf", NULL, 19); }
const lv_font_t *ui_font_status(void)
{ return ui_resources_font("fonts/NotoSansSC-SemiBold.ttf", NULL, 20); }
const lv_font_t *ui_font_number(void)
{ return ui_resources_font("fonts/Manrope-SemiBold.ttf", NULL, 22); }
const lv_font_t *ui_font_number_large(void)
{ return ui_resources_font("fonts/Manrope-Bold.ttf", NULL, 56); }
const lv_font_t *ui_font_clock(void)
{ return ui_resources_font("fonts/Manrope-ExtraBold.ttf", NULL, 104); }
const lv_font_t *ui_font_icon(void) { return &lv_font_montserrat_20; }
const lv_font_t *ui_font_phonetic(void)
{ return ui_resources_font("fonts/NotoSansSC-SemiBold.ttf", "fonts/DejaVuSans-IPA.ttf", 27); }

const lv_font_t *ui_font_text_px(uint16_t size, ui_font_weight_t weight)
{
    /*
     * The Figma Make values are CSS pixels. On the 720 px / 4 inch panel,
     * small CSS text needs an optical scale-up to remain readable at normal
     * desk distance. Keep display numerals unchanged and scale text below
     * 20 px by roughly 1.4x, with a 13 px physical minimum.
     */
    uint16_t physical_size = size < 20 ? (uint16_t)((size * 7U + 2U) / 5U) : size;
    if(physical_size < 13) physical_size = 13;
    return ui_resources_font(font_file(false, weight), NULL, physical_size);
}

const lv_font_t *ui_font_display_px(uint16_t size, ui_font_weight_t weight)
{
    uint16_t physical_size = size < 20 ? (uint16_t)((size * 7U + 2U) / 5U) : size;
    if(physical_size < 13) physical_size = 13;
    return ui_resources_font(font_file(true, weight), NULL, physical_size);
}

lv_obj_t *ui_box(lv_obj_t *parent, int32_t x, int32_t y, int32_t width, int32_t height,
                 uint32_t color, int32_t radius)
{
    lv_obj_t *object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(object, radius, 0);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE |
                       LV_OBJ_FLAG_CLICK_FOCUSABLE |
                       LV_OBJ_FLAG_SCROLLABLE);
    return object;
}

lv_obj_t *ui_button(lv_obj_t *parent, int32_t x, int32_t y, int32_t width, int32_t height,
                    uint32_t color, int32_t radius)
{
    lv_obj_t *object = ui_box(parent, x, y, width, height, color, radius);
    lv_obj_add_flag(object, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_transform_scale(object, 250, LV_STATE_PRESSED);
    lv_obj_set_style_transform_width(object, -2, LV_STATE_PRESSED);
    lv_obj_set_style_transform_height(object, -2, LV_STATE_PRESSED);
    return object;
}

lv_obj_t *ui_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                   uint32_t color, int32_t x, int32_t y)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text ? text : "");
    lv_obj_set_pos(label, x, y);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
    ui_resources_set_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, 0);
    return label;
}

lv_obj_t *ui_centered_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                            uint32_t color)
{
    lv_obj_t *label = ui_label(parent, text, font, color, 0, 0);
    lv_obj_set_width(label, lv_pct(100));
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
    return label;
}

void ui_add_soft_shadow(lv_obj_t *object, uint32_t color, lv_opa_t opacity, int32_t width,
                        int32_t offset_y)
{
    (void)color;
    (void)opacity;
    (void)width;
    (void)offset_y;
    /*
     * Soft alpha shadows look smooth in the 32-bit framebuffer but can form
     * colored bands after the display pipeline converts to the LCD's RGB666
     * physical bus. Keep the API so screen composition stays clean, while
     * panel builds use flat, explicit card borders instead.
     */
    lv_obj_set_style_shadow_opa(object, LV_OPA_TRANSP, 0);
}

void ui_format_time(char *buffer, size_t size)
{
    time_t raw = time(NULL);
    struct tm now;
    localtime_r(&raw, &now);
    snprintf(buffer, size, "%02d:%02d", now.tm_hour, now.tm_min);
}

void ui_format_date(char *buffer, size_t size)
{
    static const char *weekdays[] = {
        "周日", "周一", "周二", "周三", "周四", "周五", "周六",
    };
    time_t raw = time(NULL);
    struct tm now;
    localtime_r(&raw, &now);
    snprintf(buffer, size, "%d月%d日 · %s", now.tm_mon + 1, now.tm_mday,
             weekdays[now.tm_wday]);
}

static void back_clicked(lv_event_t *event)
{
    ui_app_t *app = lv_event_get_user_data(event);
    ui_app_go_back(app);
}

lv_obj_t *ui_create_app_shell(ui_app_t *app, const char *title, uint32_t accent,
                              lv_obj_t **content)
{
    lv_obj_t *root = ui_box(app->page_layer, 0, 0, UI_WIDTH, UI_PAGE_HEIGHT,
                            UI_COLOR_BACKGROUND, 0);
    lv_obj_t *back = ui_button(root, 22, 10, 128, 42, UI_COLOR_BACKGROUND, 21);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
    lv_obj_t *back_icon = ui_icon(back, UI_ICON_CHEVRON_LEFT, 20, accent);
    lv_obj_set_pos(back_icon, 0, 7);
    ui_label(back, "返回", ui_font_body_large(), accent, 36, 9);
    lv_obj_add_event_cb(back, back_clicked, LV_EVENT_CLICKED, app);

    lv_obj_t *heading = ui_label(root, title, ui_font_body_large(), UI_COLOR_TEXT, 0, 20);
    lv_obj_set_width(heading, UI_WIDTH - 28);
    lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_RIGHT, 0);

    *content = ui_box(root, 22, 62, UI_WIDTH - 44, UI_PAGE_HEIGHT - 72,
                      UI_COLOR_BACKGROUND, 0);
    lv_obj_set_style_bg_opa(*content, LV_OPA_TRANSP, 0);
    return root;
}
