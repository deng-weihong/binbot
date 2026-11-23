#include "../ui_internal.h"
#include "misc/cache/instance/lv_image_cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct modules_context modules_context_t;
typedef struct { modules_context_t *context; size_t index; } image_binding_t;
typedef struct { modules_context_t *context; unsigned index; } module_binding_t;
struct modules_context {
    ui_app_t *app;
    lv_obj_t *root, *panel, *slot, *slot_dot, *buttons[4], *names[4], *statuses[4], *dots[4];
    lv_obj_t *gallery;
    lv_obj_t *progress_label, *progress_value, *progress_fill;
    int progress_width;
    lv_timer_t *timer;
    module_binding_t bindings[4];
    module_binding_t motor_bindings[3];
    lv_obj_t *speed_value, *speed_slider;
    lv_obj_t *light_brightness, *light_speed, *light_brightness_value, *light_speed_value, *light_preview;
    module_binding_t light_scenes[4], light_effects[5], light_colors[8];
    module_binding_t pen_choices[5], intervals[4];
    lv_image_dsc_t faces[4];
    uint8_t *face_pixels;
    lv_obj_t *random_label;
    bb_component_light_t light_animation;
    image_binding_t *images;
    char picked[37];
    unsigned selected;
    uint32_t revision;
    bool paused;
};

static const unsigned profiles[] = {2, 1, 3, 5};
static const char *names[] = {"全息屏", "笔筒", "转盘", "氛围灯"};
static const char *english[] = {"HOLO", "PEN CUP", "TURNTABLE", "LIGHT"};
static const char *icons[] = {"holo", "pen", "dial", "bulb"};
static const uint32_t light_colors[] = {0xe8643c, 0xf6b26b, 0xffd166, 0x7ed38f,
                                        0x4cc9f0, 0x7b5cff, 0xff5fa2, 0xffffff};
static const char *effect_names[] = {"常亮", "呼吸", "流光", "闪烁", "心跳"};
static const char *effect_icons[] = {"sun", "spark", "shuffle", "bolt", "heart"};
static const char *scene_names[] = {"阅读", "专注", "助眠", "派对"};
static const bb_component_light_settings_t light_scenes[] = {
    {1, BB_COMPONENT_LIGHT_SOLID, 255, 209, 102, 80, 50, 0},
    {1, BB_COMPONENT_LIGHT_BREATHE, 76, 201, 240, 60, 50, 0},
    {1, BB_COMPONENT_LIGHT_BREATHE, 123, 92, 255, 20, 30, 0},
    {1, BB_COMPONENT_LIGHT_FLOW, 255, 95, 162, 80, 75, 0}
};
static void modules_render(modules_context_t *c);

static void pen_choice_clicked(lv_event_t *event)
{
    module_binding_t *b = lv_event_get_user_data(event);
    modules_context_t *c = b->context;
    const bb_component_pen_t *pen = app_modules_pen_state(c->app->modules);
    unsigned mode = b->index < 2 ? b->index : BB_PEN_INFO;
    unsigned face = b->index < 2 ? pen->face : b->index - 2;
    if(!app_modules_pen_set(c->app->modules, mode, face))
        ui_app_show_toast(c->app, "组件忙碌或连接已改变，请稍后重试");
    c->picked[0] = 0;
    modules_render(c);
}

static void random_clicked(lv_event_t *event)
{
    modules_context_t *c = lv_event_get_user_data(event);
    unsigned p = profiles[c->selected];
    app_modules_random_set(c->app->modules, p, app_modules_random_interval(c->app->modules, p) ? 0 : 60);
    c->picked[0] = 0; modules_render(c);
}

static void interval_clicked(lv_event_t *event)
{
    module_binding_t *b = lv_event_get_user_data(event);
    static const unsigned seconds[] = {10,60,300,1800};
    app_modules_random_set(b->context->app->modules, profiles[b->context->selected], seconds[b->index]);
    modules_render(b->context);
}

static void random_label_sync(modules_context_t *c)
{
    if(!c->random_label) return;
    unsigned p = profiles[c->selected]; char text[96];
    if(app_modules_random_interval(c->app->modules, p)) {
        unsigned left = app_modules_random_remaining(c->app->modules, p);
        snprintf(text, sizeof(text), "随机切换中 · 下次 %u:%02u", left/60, left%60);
    } else snprintf(text, sizeof(text), "已上传的图片 · %zu 张", app_modules_image_count(c->app->modules));
    lv_label_set_text(c->random_label, text);
}

static void pen_preview(modules_context_t *c, lv_obj_t *parent, unsigned index, unsigned face,
                         int x, int y, int size)
{
    if(!c->face_pixels) return;
    bb_component_pen_t state = *app_modules_pen_state(c->app->modules);
    state.face = face;
    uint8_t *pixels = c->face_pixels + index*240*240*2;
    bb_component_pen_render_face(&state, pixels, index == 0);
    for(unsigned i=0;i<240*240*2;i+=2) { uint8_t byte=pixels[i]; pixels[i]=pixels[i+1]; pixels[i+1]=byte; }
    c->faces[index] = (lv_image_dsc_t){.header={.magic=LV_IMAGE_HEADER_MAGIC, .cf=LV_COLOR_FORMAT_RGB565,
        .w=240,.h=240,.stride=480}, .data_size=240*240*2, .data=pixels};
    lv_obj_t *frame = ui_box(parent, x, y, size, size, UI_REF_INK, size/2);
    lv_obj_set_style_clip_corner(frame, true, 0);
    lv_obj_t *picture = lv_image_create(frame);
    lv_image_set_src(picture, &c->faces[index]);
    lv_image_set_scale(picture, size*256/240);
    lv_obj_center(picture);
}

static void render_pen_controls(modules_context_t *c, bool busy)
{
    app_modules_t *m = c->app->modules;
    const bb_component_pen_t *pen = app_modules_pen_state(m);
    bool enabled = app_modules_pen_supported(m) && !app_modules_pen_busy(m) && !busy;
    lv_obj_t *track = ui_box(c->panel, 20, 216, 424, 52, 0xe6e6e6, 26);
    ui_ref_surface(track, "modules_pen_modes");
    const char *labels[] = {"显示图片", "时间与天气"};
    const char *icons[] = {"camera", "sun"};
    for(unsigned i=0;i<2;++i) {
        bool on = pen->mode == i;
        lv_obj_t *b = ui_button(track, 4+i*210, 4, 206, 44, on ? UI_REF_INK : 0xe6e6e6, 22);
        if(!on) lv_obj_set_style_bg_opa(b, 0, 0);
        uint32_t fg = on ? 0xffffff : UI_REF_MUTED;
        lv_obj_t *icon = ui_ref_icon(b, icons[i], 17, fg); lv_obj_set_pos(icon, i ? 54 : 61, 14);
        ui_ref_label(b, labels[i], 15, UI_FONT_MEDIUM, fg, i ? 77 : 84, 11);
        c->pen_choices[i] = (module_binding_t){c,i};
        lv_obj_set_state(b, LV_STATE_DISABLED, !enabled);
        lv_obj_add_event_cb(b, pen_choice_clicked, LV_EVENT_CLICKED, &c->pen_choices[i]);
    }
    if(pen->mode != BB_PEN_INFO) return;
    ui_ref_label(c->panel, "表盘样式 Face", 15, UI_FONT_REGULAR, UI_REF_FAINT, 20, 284);
    const char *faces[] = {"时间 + 天气", "仅时间", "仅天气"};
    for(unsigned i=0;i<3;++i) {
        bool on = pen->face == i;
        lv_obj_t *b = ui_button(c->panel, 20+i*145, 318, 133, 123, on ? 0xffffff : 0xe6e6e6, 20);
        if(!on) ui_ref_surface(b, "modules_pen_face");
        if(on) {
            lv_obj_set_style_outline_width(b, 3, 0); lv_obj_set_style_outline_pad(b, 0, 0);
            lv_obj_set_style_outline_color(b, lv_color_hex(UI_REF_CLAY), 0);
        }
        lv_obj_t *rim = ui_box(b, 27, 6, 80, 80, UI_REF_INK, 40);
        pen_preview(c, rim, i+1, i, 4, 4, 72);
        lv_obj_t *label = ui_ref_label(b, faces[i], 15, UI_FONT_MEDIUM, UI_REF_INK, 0, 90);
        lv_obj_set_width(label, 133); lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        c->pen_choices[i+2] = (module_binding_t){c,i+2};
        lv_obj_set_state(b, LV_STATE_DISABLED, !enabled);
        lv_obj_add_event_cb(b, pen_choice_clicked, LV_EVENT_CLICKED, &c->pen_choices[i+2]);
    }
    const binbot_component_event_t *result = app_modules_pen_status(m);
    const char *status = result && result->status == BINBOT_COMPONENT_FAILED ? "设置失败，请重试" :
        pen->flags & BB_PEN_WEATHER_VALID ? "时间每分钟自动刷新，天气每 30 分钟同步一次" :
        "时间每分钟自动刷新 · 天气暂无数据";
    ui_ref_label(c->panel, status, 13, UI_FONT_REGULAR, UI_REF_FAINT, 20, 453);
}

static void light_apply(modules_context_t *c, const bb_component_light_settings_t *settings)
{
    if(!app_modules_light_set(c->app->modules, settings))
        ui_app_show_toast(c->app, "组件忙碌或连接已改变，请稍后重试");
    modules_render(c);
}

static void light_power_clicked(lv_event_t *event)
{
    modules_context_t *c = lv_event_get_user_data(event);
    const bb_component_light_settings_t *current = app_modules_light_state(c->app->modules);
    if(!current) return;
    bb_component_light_settings_t next = *current; next.enabled = !next.enabled;
    light_apply(c, &next);
}

static void light_timer_clicked(lv_event_t *event)
{
    modules_context_t *c = lv_event_get_user_data(event);
    const bb_component_light_settings_t *current = app_modules_light_state(c->app->modules);
    if(!current) return;
    static const unsigned timers[] = {0, 15, 30, 60, 120};
    bb_component_light_settings_t next = *current;
    unsigned i = 0; while(i < 4 && timers[i] != next.timer_minutes) ++i;
    next.timer_minutes = timers[(i + 1) % 5];
    light_apply(c, &next);
}

static void light_option_clicked(lv_event_t *event)
{
    module_binding_t *binding = lv_event_get_user_data(event);
    modules_context_t *c = binding->context;
    const bb_component_light_settings_t *current = app_modules_light_state(c->app->modules);
    if(!current) return;
    bb_component_light_settings_t next = *current;
    unsigned choice = binding->index;
    if(choice < 4) {
        next = light_scenes[choice]; next.timer_minutes = current->timer_minutes; next.speed = current->speed;
    } else if(choice < 9) next.effect = choice - 4;
    else {
        uint32_t rgb = light_colors[choice - 9];
        next.red = rgb >> 16; next.green = rgb >> 8; next.blue = rgb;
    }
    next.enabled = 1;
    light_apply(c, &next);
}

static void light_slider_changed(lv_event_t *event)
{
    modules_context_t *c = lv_event_get_user_data(event);
    lv_obj_t *slider = lv_event_get_target(event);
    bool brightness = slider == c->light_brightness;
    unsigned value = lv_slider_get_value(slider);
    char text[40]; snprintf(text, sizeof(text), "%s %u%%", brightness ? "亮度" : "速度", value);
    lv_label_set_text(brightness ? c->light_brightness_value : c->light_speed_value, text);
    if(lv_event_get_code(event) != LV_EVENT_RELEASED) return;
    const bb_component_light_settings_t *current = app_modules_light_state(c->app->modules);
    if(!current) return;
    bb_component_light_settings_t next = *current;
    if(brightness) next.brightness = value; else next.speed = value;
    light_apply(c, &next);
}

static lv_obj_t *light_button(modules_context_t *c, int x, int y, int w, int h,
                               const char *name, const char *icon, uint32_t color, bool enabled,
                               lv_event_cb_t callback, void *binding)
{
    lv_obj_t *button = ui_button(c->panel, x, y, w, h, color,
                                callback == light_power_clicked || callback == light_timer_clicked ? 24 : 16);
    uint32_t foreground = color == UI_REF_INK || color == UI_REF_CLAY ? 0xffffff : 0x525252;
    lv_obj_t *label = ui_ref_label(button, name, 15, UI_FONT_MEDIUM, foreground, 0, 0);
    lv_point_t text_size;
    lv_text_get_size(&text_size, name, lv_obj_get_style_text_font(label, 0), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int left = (w - text_size.x - (icon ? 22 : 0)) / 2;
    if(icon) {
        lv_obj_t *image = ui_ref_icon(button, icon, 16, foreground);
        lv_obj_set_pos(image, left, (h - 16) / 2);
    }
    lv_obj_set_pos(label, left + (icon ? 22 : 0), (h - text_size.y) / 2);
    lv_obj_set_state(button, LV_STATE_DISABLED, !enabled);
    if(!enabled) lv_obj_set_style_opa(button, 120, LV_STATE_DISABLED);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, binding);
    return button;
}

static void render_light(modules_context_t *c)
{
    app_modules_t *m = c->app->modules;
    const bb_component_light_settings_t defaults = {1, 0, 255, 160, 80, 20, 50, 0};
    const bb_component_light_settings_t *s = app_modules_light_state(m);
    if(!s) s = &defaults;
    bool supported = app_modules_light_supported(m), busy = app_modules_light_busy(m), enabled = supported && !busy;
    uint32_t rgb = (uint32_t)s->red << 16 | (uint32_t)s->green << 8 | s->blue;
    lv_obj_t *preview = ui_box(c->panel, 20, 84, 150, 244, UI_REF_INK, 24);
    c->light_preview = ui_box(preview, 39, 86, 72, 72, s->enabled ? rgb : 0x3a3a3a, 36);
    lv_obj_set_style_border_width(c->light_preview, 4, 0);
    lv_obj_set_style_border_color(c->light_preview, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_border_opa(c->light_preview, 204, 0);
    ui_ref_shadow(c->light_preview, rgb, s->enabled ? s->brightness * 2 : 0, 96, 12, 0);
    lv_obj_t *bulb = ui_ref_icon(c->light_preview, "bulb", 28, 0xffffff); lv_obj_center(bulb);
    if(s->enabled && app_modules_light_remaining(m)) {
        char text[48]; snprintf(text, sizeof(text), "%u 分钟后关灯", app_modules_light_remaining(m));
        lv_obj_t *label = ui_ref_label(preview, text, 13, UI_FONT_REGULAR, 0xaaaaaa, 3, 213);
        lv_obj_set_width(label, 144); lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    }
    bb_component_light_set(&c->light_animation, s, lv_tick_get());
    light_button(c, 186, 129, 140, 48, s->enabled ? "已开启" : "已关闭", "power",
                 s->enabled ? UI_REF_CLAY : 0xe6e6e6, enabled, light_power_clicked, c);
    char timer[24];
    if(s->timer_minutes) snprintf(timer, sizeof(timer), "%u分", s->timer_minutes);
    else snprintf(timer, sizeof(timer), "定时");
    light_button(c, 334, 129, 110, 48, timer, "alarm", s->timer_minutes ? UI_REF_INK : 0xe6e6e6,
                 enabled, light_timer_clicked, c);
    for(unsigned i = 0; i < 4; ++i) {
        c->light_scenes[i] = (module_binding_t){c, i};
        lv_obj_t *button = light_button(c, 186 + (i % 2) * 133, 187 + (i / 2) * 54, 125, 44,
                                        scene_names[i], NULL, 0xe6e6e6, enabled, light_option_clicked, &c->light_scenes[i]);
        ui_box(button, 12, 15, 14, 14, (uint32_t)light_scenes[i].red << 16 |
               (uint32_t)light_scenes[i].green << 8 | light_scenes[i].blue, 7);
    }
    const binbot_component_event_t *result = app_modules_light_status(m);
    const char *status = !supported ? "请升级灯组件固件" : busy ? "正在设置…" :
                         result && result->status == BINBOT_COMPONENT_FAILED ? "设置失败，请重试" : "";
    ui_ref_label(c->panel, status, 13, UI_FONT_REGULAR, UI_REF_MUTED, 186, 305);
    ui_ref_label(c->panel, "灯效 Effect", 15, UI_FONT_REGULAR, UI_REF_FAINT, 20, 344);
    for(unsigned i = 0; i < 5; ++i) {
        c->light_effects[i] = (module_binding_t){c, i + 4};
        light_button(c, 20 + i * 87, 373, 76, 48, effect_names[i], effect_icons[i],
                     s->effect == i ? UI_REF_INK : 0xe6e6e6, enabled, light_option_clicked, &c->light_effects[i]);
    }
    for(unsigned i = 0; i < 8; ++i) {
        lv_obj_t *button = ui_button(c->panel, 20 + i * 54, 437, 44, 44, light_colors[i], 22);
        lv_obj_set_style_border_width(button, 1, 0);
        lv_obj_set_style_border_color(button, lv_color_hex(0xd4d4d4), 0);
        if(rgb == light_colors[i]) {
            lv_obj_set_style_outline_width(button, 3, 0); lv_obj_set_style_outline_pad(button, 2, 0);
            lv_obj_set_style_outline_color(button, lv_color_hex(UI_REF_INK), 0);
        }
        lv_obj_set_state(button, LV_STATE_DISABLED, !enabled);
        c->light_colors[i] = (module_binding_t){c, i + 9};
        lv_obj_add_event_cb(button, light_option_clicked, LV_EVENT_CLICKED, &c->light_colors[i]);
    }
    for(unsigned i = 0; i < 2; ++i) {
        int x = 20 + i * 220;
        unsigned value = i ? s->speed : s->brightness;
        char text[40]; snprintf(text, sizeof(text), "%s %u%%", i ? "速度" : "亮度", value);
        lv_obj_t *label = ui_ref_label(c->panel, text, 15, UI_FONT_REGULAR, UI_REF_FAINT, x, 497);
        lv_obj_t *slider = lv_slider_create(c->panel);
        lv_obj_set_pos(slider, x, 526); lv_obj_set_size(slider, 204, 56);
        lv_slider_set_range(slider, i ? 5 : 0, 100); lv_slider_set_value(slider, value, LV_ANIM_OFF);
        lv_obj_set_style_radius(slider, 20, LV_PART_MAIN);
        lv_obj_set_style_radius(slider, 20, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(slider, lv_color_hex(0xececec), LV_PART_MAIN);
        lv_obj_set_style_bg_color(slider, lv_color_hex(i ? 0x2b2b2b : rgb), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(slider, 0, LV_PART_KNOB); lv_obj_set_style_pad_all(slider, 0, LV_PART_KNOB);
        lv_obj_set_state(slider, LV_STATE_DISABLED, !enabled || (i && s->effect == BB_COMPONENT_LIGHT_SOLID));
        lv_obj_add_event_cb(slider, light_slider_changed, LV_EVENT_VALUE_CHANGED, c);
        lv_obj_add_event_cb(slider, light_slider_changed, LV_EVENT_RELEASED, c);
        if(i) { c->light_speed = slider; c->light_speed_value = label; }
        else { c->light_brightness = slider; c->light_brightness_value = label; }
    }
}

static void progress_sync(modules_context_t *c)
{
    const binbot_component_event_t *push = app_modules_push_status(c->app->modules);
    if(!push || !c->progress_label) return;
    bool success = push->status == BINBOT_COMPONENT_SUCCEEDED;
    unsigned percent = success ? 100 : push->total_bytes ?
                       (unsigned)((uint64_t)push->bytes_written * 95 / push->total_bytes) : 0;
    if(!success && percent > 99) percent = 99;
    lv_label_set_text(c->progress_label, success ? "推送完成" :
                      push->status >= BINBOT_COMPONENT_VALIDATING ? "正在写入显示…" : "正在传输到屏幕…");
    lv_obj_set_style_text_color(c->progress_label, lv_color_hex(success ? 0x059669 : UI_REF_CLAY), 0);
    char value[16]; snprintf(value, sizeof(value), "%u%%", percent);
    lv_label_set_text(c->progress_value, value);
    lv_obj_set_width(c->progress_fill, c->progress_width * (int)percent / 100);
    lv_obj_set_flag(c->progress_fill, LV_OBJ_FLAG_HIDDEN, !percent);
    lv_obj_set_style_bg_color(c->progress_fill, lv_color_hex(success ? 0x10b981 : UI_REF_CLAY), 0);
}

static const app_module_image_t *find_image(modules_context_t *c, const char *id, size_t *index)
{
    size_t count = app_modules_image_count(c->app->modules);
    for(size_t i = 0; id && id[0] && i < count; ++i) {
        const app_module_image_t *image = app_modules_image(c->app->modules, i);
        if(!strcmp(id, image->id)) { if(index) *index = i; return image; }
    }
    return NULL;
}

static void select_module(lv_event_t *event)
{
    module_binding_t *binding = lv_event_get_user_data(event);
    modules_context_t *c = binding->context;
    c->selected = binding->index;
    c->picked[0] = 0;
    modules_render(c);
}

static void pick_image(lv_event_t *event)
{
    image_binding_t *binding = lv_event_get_user_data(event);
    modules_context_t *c = binding->context;
    lv_indev_t *indev = lv_event_get_indev(event);
    if(indev && lv_indev_get_press_moved(indev)) return;
    const app_module_image_t *image = app_modules_image(c->app->modules, binding->index);
    if(!image) return;
    if(app_modules_random_interval(c->app->modules, profiles[c->selected])) return;
    const char *current = app_modules_current(c->app->modules, profiles[c->selected]);
    snprintf(c->picked, sizeof(c->picked), "%s", !strcmp(current, image->id) ? "" : image->id);
    modules_render(c);
}

static void cancel_pick(lv_event_t *event)
{
    modules_context_t *c = lv_event_get_user_data(event);
    c->picked[0] = 0;
    modules_render(c);
}

static void push_image(lv_event_t *event)
{
    modules_context_t *c = lv_event_get_user_data(event);
    size_t index = 0;
    if(!find_image(c, c->picked, &index)) return;
    if(!app_modules_push(c->app->modules, index, profiles[c->selected])) {
        ui_app_show_toast(c->app, "组件忙碌或连接已改变，请稍后重试");
        app_modules_request_probe(c->app->modules);
    }
    modules_render(c);
}

static void probe_clicked(lv_event_t *event)
{
    modules_context_t *c = lv_event_get_user_data(event);
    app_modules_request_probe(c->app->modules);
    ui_app_show_toast(c->app, "正在检测组件插槽");
}

static void motor_direction_clicked(lv_event_t *event)
{
    module_binding_t *binding = lv_event_get_user_data(event);
    modules_context_t *c = binding->context;
    static const unsigned directions[] = {1, 0, 2};
    unsigned speed = app_modules_motor_speed(c->app->modules);
    if(binding->index != 1 && !speed) speed = 40;
    if(!app_modules_motor_set(c->app->modules, directions[binding->index], speed))
        ui_app_show_toast(c->app, "组件忙碌或连接已改变，请稍后重试");
    modules_render(c);
}

static void motor_speed_changed(lv_event_t *event)
{
    modules_context_t *c = lv_event_get_user_data(event);
    unsigned speed = (unsigned)lv_slider_get_value(lv_event_get_target(event));
    char text[16]; snprintf(text, sizeof(text), "%u%%", speed);
    lv_label_set_text(c->speed_value, text);
    if(lv_event_get_code(event) != LV_EVENT_RELEASED) return;
    if(!app_modules_motor_set(c->app->modules, app_modules_motor_direction(c->app->modules), speed))
        ui_app_show_toast(c->app, "组件忙碌或连接已改变，请稍后重试");
    modules_render(c);
}

static lv_obj_t *image_preview(lv_obj_t *parent, const app_module_image_t *image,
                                int x, int y, int w, int h, int radius)
{
    lv_obj_t *frame = ui_box(parent, x, y, w, h, UI_REF_INK, radius);
    lv_obj_set_style_clip_corner(frame, true, 0);
    if(image) {
        lv_obj_t *picture = lv_image_create(frame);
        char path[520];
        snprintf(path, sizeof(path), "A:%s", image->thumbnail);
        lv_image_set_src(picture, path);
        lv_image_set_scale(picture, (uint32_t)(w > h ? w : h) * 256 / 240);
        lv_obj_center(picture);
        lv_obj_remove_flag(picture, LV_OBJ_FLAG_CLICKABLE);
    }
    return frame;
}

static void render_gallery(modules_context_t *c, bool busy, int scroll_y)
{
    size_t count = app_modules_image_count(c->app->modules);
    unsigned iv = app_modules_random_interval(c->app->modules, profiles[c->selected]);
    int top = profiles[c->selected] == 1 ? 284 : 216;
    c->random_label = ui_ref_label(c->panel, "", 15, UI_FONT_REGULAR, UI_REF_FAINT, 20, top+9);
    lv_obj_set_width(c->random_label, 275); random_label_sync(c);
    lv_obj_t *toggle = ui_button(c->panel, 301, top, 143, 40, iv ? 0xfaeae5 : 0xe6e6e6, 20);
    if(!iv) ui_ref_surface(toggle, "modules_random");
    lv_obj_t *icon = ui_ref_icon(toggle, "shuffle", 17, iv ? UI_REF_CLAY : 0x525252); lv_obj_set_pos(icon, 12, 12);
    ui_ref_label(toggle, "随机切换", 14, UI_FONT_MEDIUM, iv ? UI_REF_CLAY : 0x525252, 37, 9);
    lv_obj_t *rail = ui_box(toggle, 101, 9, 36, 22, iv ? UI_REF_CLAY : 0xd4d4d4, 11);
    ui_box(rail, iv ? 16 : 2, 2, 18, 18, 0xffffff, 9);
    lv_obj_set_state(toggle, LV_STATE_DISABLED, busy);
    lv_obj_add_event_cb(toggle, random_clicked, LV_EVENT_CLICKED, c);
    top += 48;
    if(iv) {
        static const unsigned seconds[] = {10,60,300,1800};
        static const char *labels[] = {"每 10 秒", "每 1 分钟", "每 5 分钟", "每 30 分钟"};
        for(unsigned i=0;i<4;++i) {
            bool on = seconds[i] == iv;
            lv_obj_t *b = ui_button(c->panel, 20+i*108, top, 100, 40, on ? UI_REF_INK : 0xe6e6e6, 20);
            if(!on) ui_ref_surface(b, "modules_interval");
            ui_centered_label(b, labels[i], ui_ref_font(14, UI_FONT_REGULAR), on ? 0xffffff : 0x525252);
            c->intervals[i] = (module_binding_t){c,i};
            lv_obj_set_state(b, LV_STATE_DISABLED, busy);
            lv_obj_add_event_cb(b, interval_clicked, LV_EVENT_CLICKED, &c->intervals[i]);
        }
        top += 50;
    }
    c->gallery = ui_box(c->panel, 20, top, 424, 582-top, 0xffffff, 0);
    lv_obj_set_style_bg_opa(c->gallery, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(c->gallery, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(c->gallery, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(c->gallery, LV_SCROLLBAR_MODE_AUTO);
    if(!count) {
        lv_obj_t *empty = ui_ref_label(c->gallery, "暂无上传图片", 17, UI_FONT_REGULAR,
                                        UI_REF_FAINT, 0, 68);
        lv_obj_set_width(empty, 422);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }
    c->images = calloc(count, sizeof(*c->images));
    if(!c->images) return;
    bool round = profiles[c->selected] == 1;
    const char *current = app_modules_current(c->app->modules, profiles[c->selected]);
    for(size_t i = 0; i < count; ++i) {
        const app_module_image_t *image = app_modules_image(c->app->modules, i);
        int x = (int)(i % 4) * 108, y = (int)(i / 4) * (round ? 131 : 107);
        bool picked = !strcmp(c->picked, image->id), on = !strcmp(current, image->id);
        lv_obj_t *button = ui_button(c->gallery, x, y, 98, round ? 124 : 100, 0xffffff, 0);
        lv_obj_set_style_bg_opa(button, 0, 0);
        lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
        lv_obj_set_state(button, LV_STATE_DISABLED, busy || iv);
        if(busy || iv) lv_obj_set_style_opa(button, 128, 0);
        lv_obj_t *preview = image_preview(button, image, 0, 0, 98, round ? 98 : 73, round ? 49 : 12);
        if(picked) {
            lv_obj_set_style_outline_width(preview, 3, 0);
            lv_obj_set_style_outline_pad(preview, 2, 0);
            lv_obj_set_style_outline_color(preview, lv_color_hex(UI_REF_CLAY), 0);
            lv_obj_t *check = ui_box(button, 78, 0, 24, 24, UI_REF_CLAY, 12);
            lv_obj_t *icon = ui_ref_icon(check, "check", 14, 0xffffff); lv_obj_center(icon);
        }
        if(on) {
            lv_obj_t *tag = ui_box(preview, 23, (round ? 98 : 73) - 24, 52, 20, 0, 10);
            lv_obj_set_style_bg_opa(tag, 115, 0);
            ui_centered_label(tag, "显示中", ui_ref_font(11, UI_FONT_REGULAR), 0xffffff);
        }
        lv_obj_t *label = ui_ref_label(button, image->name, 13,
                                        picked ? UI_FONT_MEDIUM : UI_FONT_REGULAR,
                                        picked ? UI_REF_CLAY : UI_REF_MUTED, 0, round ? 102 : 77);
        lv_obj_set_width(label, 98);
        lv_obj_set_height(label, 18);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        c->images[i] = (image_binding_t){c, i};
        lv_obj_add_event_cb(button, pick_image, LV_EVENT_CLICKED, &c->images[i]);
    }
    lv_obj_scroll_to_y(c->gallery, scroll_y, LV_ANIM_OFF);
}

static void modules_render(modules_context_t *c)
{
    int scroll_y = c->gallery ? lv_obj_get_scroll_y(c->gallery) : 0;
    c->gallery = NULL;
    c->random_label = NULL;
    c->progress_label = c->progress_value = c->progress_fill = NULL;
    c->speed_value = c->speed_slider = NULL;
    c->light_brightness = c->light_speed = c->light_preview = NULL;
    lv_obj_clean(c->panel);
    for(unsigned i=0;i<4;++i) lv_image_cache_drop(&c->faces[i]);
    free(c->images); c->images = NULL;
    app_modules_t *m = c->app->modules;
    unsigned profile = app_modules_profile(m), selected = profiles[c->selected];
    bool inserted = profile == selected && app_modules_healthy(m);
    char text[96];
    const char *slot = "未连接";
    for(unsigned i = 0; i < 4; ++i) if(profile == profiles[i]) slot = names[i];
    snprintf(text, sizeof(text), "插槽 · %s", slot);
    lv_label_set_text(c->slot, text);
    lv_obj_set_style_bg_color(c->slot_dot, lv_color_hex(app_modules_healthy(m) ? 0x7ed38f : 0xd4d4d4), 0);
    for(unsigned i = 0; i < 4; ++i) {
        bool on = i == c->selected;
        lv_obj_set_style_bg_image_opa(c->buttons[i], on ? 0 : 255, 0);
        lv_obj_set_style_bg_color(c->buttons[i], lv_color_hex(on ? UI_REF_INK : 0xffffff), 0);
        lv_obj_set_style_text_color(c->names[i], lv_color_hex(on ? 0xffffff : UI_REF_INK), 0);
        lv_obj_set_style_text_color(c->statuses[i], lv_color_hex(on ? 0xaaaaaa : UI_REF_FAINT), 0);
        lv_obj_set_flag(c->dots[i], LV_OBJ_FLAG_HIDDEN, profile != profiles[i] || !app_modules_healthy(m));
        const app_module_image_t *current = find_image(c, app_modules_current(m, profiles[i]), NULL);
        lv_label_set_text(c->statuses[i], profile == profiles[i] && app_modules_healthy(m) ?
                          (current ? current->name : "已插入") : "未插入");
        if(i == 3 && profile == BB_COMPONENT_AMBIENT_LIGHT && app_modules_light_supported(m)) {
            const bb_component_light_settings_t *light = app_modules_light_state(m);
            snprintf(text, sizeof(text), light->enabled ? "亮度 %u%%" : "已关闭", light->brightness);
            lv_label_set_text(c->statuses[i], text);
        }
        if(i == 1 && profile == 1 && app_modules_pen_state(m)->mode == BB_PEN_INFO)
            lv_label_set_text(c->statuses[i], "时间与天气");
        lv_obj_t *icon_box = lv_obj_get_child(c->buttons[i], 0);
        lv_obj_set_style_bg_color(icon_box, lv_color_hex(on ? UI_REF_CLAY : 0xe6e6e6), 0);
        lv_obj_set_style_image_recolor(lv_obj_get_child(lv_obj_get_child(icon_box, 0), 0),
                                       lv_color_hex(on ? 0xffffff : 0x525252), 0);
    }
    ui_ref_label(c->panel, english[c->selected], 14, UI_FONT_REGULAR, UI_REF_FAINT, 20, 20);
    ui_ref_label(c->panel, names[c->selected], 22, UI_FONT_SEMIBOLD, UI_REF_INK, 20, 41);
    lv_obj_t *status = ui_ref_button(c->panel, 356, 24, 88, 40, 20, "modules_inserted");
    ui_box(status, 16, 16, 8, 8, inserted ? 0x7ed38f : 0xd4d4d4, 4);
    ui_ref_label(status, inserted ? "已插入" : "未插入", 14, UI_FONT_REGULAR, 0x525252, 31, 10);
    if(!inserted) {
        lv_obj_t *frame = ui_box(c->panel, 184, 201, 96, 96, 0xffffff, 28);
        lv_obj_set_style_bg_opa(frame, 0, 0);
        lv_obj_set_style_border_color(frame, lv_color_hex(0xd4d4d4), 0);
        lv_obj_set_style_border_width(frame, 2, 0);
        lv_obj_t *icon = ui_ref_icon(frame, icons[c->selected], 44, 0xd4d4d4); lv_obj_center(icon);
        snprintf(text, sizeof(text), "%s未插入", names[c->selected]);
        lv_obj_t *label = ui_ref_label(c->panel, text, 19, UI_FONT_SEMIBOLD, UI_REF_INK, 20, 313);
        lv_obj_set_width(label, 424); lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        snprintf(text, sizeof(text), "请将%s插入组件插槽", names[c->selected]);
        label = ui_ref_label(c->panel, text, 15, UI_FONT_REGULAR, UI_REF_FAINT, 20, 345);
        lv_obj_set_width(label, 424); lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_t *probe = ui_button(c->panel, 176, 398, 112, 48, UI_REF_CLAY, 24);
        ui_centered_label(probe, "检测组件", ui_ref_font(15, UI_FONT_MEDIUM), 0xffffff);
        lv_obj_add_event_cb(probe, probe_clicked, LV_EVENT_CLICKED, c);
    } else if(selected <= 2) {
        const bb_component_pen_t *pen = app_modules_pen_state(m);
        bool info = selected == 1 && pen->mode == BB_PEN_INFO;
        const binbot_component_event_t *push = app_modules_push_status(m);
        bool busy = push && push->status < BINBOT_COMPONENT_SUCCEEDED;
        bool same_profile = app_modules_push_profile(m) == selected;
        bool success = !info && push && same_profile && push->status == BINBOT_COMPONENT_SUCCEEDED &&
                       (!c->picked[0] || !strcmp(c->picked, app_modules_current(m, selected)));
        const app_module_image_t *image = find_image(c, c->picked[0] ? c->picked : app_modules_current(m, selected), NULL);
        if(success && image && !strcmp(image->id, app_modules_current(m, selected))) c->picked[0] = 0;
        int preview_w = selected == 1 ? 116 : 148, tx = preview_w + 40;
        lv_obj_t *frame = ui_box(c->panel, 20, 84, preview_w, 116, UI_REF_INK, selected == 1 ? 58 : 18);
        ui_ref_shadow(frame, 0, 140, 30, -16, 16);
        if(info) pen_preview(c, frame, 0, pen->face, 8, 8, 100);
        else image_preview(frame, image, 8, 8, preview_w - 16, 100, selected == 1 ? 50 : 11);
        ui_ref_label(c->panel, busy ? "正在推送" : success ? "已更新" : c->picked[0] ? "待推送" : "当前显示",
                       15, UI_FONT_REGULAR, UI_REF_FAINT, tx, 90);
        lv_obj_t *name = ui_ref_label(c->panel, info ? "时间与天气" : image ? image->name : "暂无图片", 20,
                                       UI_FONT_SEMIBOLD, UI_REF_INK, tx, 113);
        lv_obj_set_height(name, 28);
        lv_obj_set_width(name, 444 - tx); lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        ui_ref_label(c->panel, selected == 1 ? "圆形屏 · 240×240" : "全息屏 · 320×240",
                       14, UI_FONT_REGULAR, UI_REF_FAINT, tx, 145);
        if((busy && same_profile) || success) {
            c->progress_label = ui_ref_label(c->panel, "", 13, UI_FONT_MEDIUM, UI_REF_CLAY, tx, 177);
            c->progress_value = ui_ref_label(c->panel, "", 13, UI_FONT_REGULAR, UI_REF_MUTED, 399, 177);
            lv_obj_set_width(c->progress_value, 45);
            lv_obj_set_style_text_align(c->progress_value, LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_t *bar = ui_box(c->panel, tx, 201, 444 - tx, 10, 0xf5f5f5, 5);
            c->progress_width = 444 - tx;
            c->progress_fill = ui_box(bar, 0, 0, 1, 10, UI_REF_CLAY, 5);
            progress_sync(c);
        } else if(info) {
            ui_ref_label(c->panel, "实时显示中，随机换图已暂停", 13, UI_FONT_REGULAR, UI_REF_FAINT, tx, 177);
        } else if(c->picked[0]) {
            lv_obj_t *cancel = ui_ref_button(c->panel, tx, 176, 62, 44, 22, "desk_dismiss");
            ui_centered_label(cancel, "取消", ui_ref_font(15, UI_FONT_REGULAR), 0x525252);
            lv_obj_add_event_cb(cancel, cancel_pick, LV_EVENT_CLICKED, c);
            lv_obj_t *confirm = ui_button(c->panel, tx + 70, 176, 100, 44, UI_REF_CLAY, 22);
            ui_centered_label(confirm, "确认推送", ui_ref_font(15, UI_FONT_MEDIUM), 0xffffff);
            lv_obj_set_state(confirm, LV_STATE_DISABLED, busy);
            lv_obj_add_event_cb(confirm, push_image, LV_EVENT_CLICKED, c);
            if(push && push->status >= BINBOT_COMPONENT_FAILED) {
                lv_obj_t *error = ui_ref_label(c->panel, "推送失败，可重试", 13, UI_FONT_REGULAR, UI_REF_CLAY, tx, 222);
                lv_obj_set_width(error, 444 - tx);
            }
        } else {
            const binbot_component_event_t *pen_result = app_modules_pen_status(m);
            const char *hint_text = selected == 1 && !app_modules_pen_supported(m) ? "请升级笔筒固件以使用时间与天气" :
                selected == 1 && pen_result && pen_result->status == BINBOT_COMPONENT_FAILED ? "设置失败，请重试" :
                app_modules_random_interval(m, selected) ?
                "随机切换已开启，将自动从图库中换图" : "从下方选一张图片，确认后推送到屏幕";
            lv_obj_t *hint = ui_ref_label(c->panel, hint_text, 13, UI_FONT_REGULAR, UI_REF_FAINT, tx, 177);
            lv_obj_set_width(hint, 444 - tx);
        }
        if(selected == 1) render_pen_controls(c, busy);
        if(!info) render_gallery(c, busy || (selected == 1 && app_modules_pen_busy(m)), scroll_y);
    } else if(selected == 3) {
        lv_obj_t *dial = ui_box(c->panel, 147, 180, 170, 170, 0xf1eee8, 85);
        lv_obj_t *wheel = ui_box(dial, 15, 15, 140, 140, 0xffffff, 70);
        ui_box(wheel, 65, 12, 10, 24, UI_REF_CLAY, 5);
        ui_box(dial, 73, 73, 24, 24, UI_REF_INK, 12);
        const char *commands[] = {"逆时针", "停止", "顺时针"};
        const unsigned directions[] = {1, 0, 2};
        bool supported = app_modules_motor_supported(m), busy = app_modules_motor_busy(m);
        const binbot_component_event_t *motor = app_modules_motor_status(m);
        const char *state = !supported ? "请升级转盘固件" : busy ? "正在设置…" :
                            motor && motor->status == BINBOT_COMPONENT_FAILED ? "设置失败，请重试" :
                            app_modules_motor_direction(m) == 1 ? "逆时针旋转" :
                            app_modules_motor_direction(m) == 2 ? "顺时针旋转" : "已停止";
        lv_obj_t *state_label = ui_ref_label(c->panel, state, 16, UI_FONT_MEDIUM, UI_REF_MUTED, 20, 382);
        lv_obj_set_width(state_label, 424);
        lv_obj_set_style_text_align(state_label, LV_TEXT_ALIGN_CENTER, 0);
        for(int i = 0; i < 3; ++i) {
            lv_obj_t *button = ui_ref_button(c->panel, 20 + i * 144, 440, 136, 52, 26, "modules_direction");
            bool active = supported && directions[i] == app_modules_motor_direction(m);
            if(active) {
                lv_obj_set_style_bg_image_opa(button, 0, 0);
                lv_obj_set_style_bg_color(button, lv_color_hex(UI_REF_CLAY), 0);
            }
            ui_centered_label(button, commands[i], ui_ref_font(16, UI_FONT_MEDIUM), active ? 0xffffff : 0x525252);
            lv_obj_set_state(button, LV_STATE_DISABLED, !supported || busy);
            c->motor_bindings[i] = (module_binding_t){c, (unsigned)i};
            lv_obj_add_event_cb(button, motor_direction_clicked, LV_EVENT_CLICKED, &c->motor_bindings[i]);
        }
        ui_ref_label(c->panel, "转速 Speed", 15, UI_FONT_REGULAR, UI_REF_FAINT, 20, 514);
        snprintf(text, sizeof(text), "%u%%", app_modules_motor_speed(m));
        c->speed_value = ui_ref_label(c->panel, text, 15, UI_FONT_MEDIUM, UI_REF_INK, 384, 514);
        lv_obj_set_width(c->speed_value, 60);
        lv_obj_set_style_text_align(c->speed_value, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_t *speed = lv_slider_create(c->panel);
        c->speed_slider = speed;
        lv_obj_set_pos(speed, 20, 547); lv_obj_set_size(speed, 424, 35);
        lv_slider_set_range(speed, 0, 100);
        lv_slider_set_value(speed, app_modules_motor_speed(m), LV_ANIM_OFF);
        lv_obj_set_style_bg_color(speed, lv_color_hex(0xe6e6e6), LV_PART_MAIN);
        lv_obj_set_style_bg_color(speed, lv_color_hex(UI_REF_CLAY), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(speed, lv_color_hex(UI_REF_CLAY), LV_PART_KNOB);
        lv_obj_set_state(speed, LV_STATE_DISABLED, !supported || busy);
        lv_obj_add_event_cb(speed, motor_speed_changed, LV_EVENT_VALUE_CHANGED, c);
        lv_obj_add_event_cb(speed, motor_speed_changed, LV_EVENT_RELEASED, c);
    } else render_light(c);
    c->revision = app_modules_revision(m);
}

static void modules_timer(lv_timer_t *timer)
{
    modules_context_t *c = lv_timer_get_user_data(timer);
    if(c->paused || c->app->content_frozen) return;
    random_label_sync(c);
    if(c->light_preview) {
        uint8_t pixel[3];
        bb_component_light_render(&c->light_animation, lv_tick_get(), pixel, 1);
        uint32_t color = (uint32_t)pixel[0] << 16 | (uint32_t)pixel[1] << 8 | pixel[2];
        lv_obj_set_style_bg_color(c->light_preview, lv_color_hex(c->light_animation.settings.enabled ? color : 0x3a3a3a), 0);
        lv_obj_set_style_shadow_color(c->light_preview, lv_color_hex(color), 0);
        lv_obj_set_style_shadow_opa(c->light_preview, c->light_animation.settings.enabled ? 180 : 0, 0);
    }
    if(c->revision != app_modules_revision(c->app->modules) &&
       (!c->speed_slider || !lv_slider_is_dragged(c->speed_slider)) &&
       (!c->light_brightness || !lv_slider_is_dragged(c->light_brightness)) &&
       (!c->light_speed || !lv_slider_is_dragged(c->light_speed))) {
        const binbot_component_event_t *push = app_modules_push_status(c->app->modules);
        if(c->progress_label && push && push->status < BINBOT_COMPONENT_SUCCEEDED &&
           app_modules_healthy(c->app->modules) &&
           app_modules_profile(c->app->modules) == profiles[c->selected]) {
            progress_sync(c);
            c->revision = app_modules_revision(c->app->modules);
        } else modules_render(c);
    }
}

static void modules_paused(void *data, bool paused) { ((modules_context_t *)data)->paused = paused; }
static void modules_destroy(void *data)
{
    modules_context_t *c = data;
    if(c->timer) lv_timer_delete(c->timer);
    for(unsigned i=0;i<4;++i) lv_image_cache_drop(&c->faces[i]);
    free(c->face_pixels); free(c->images); free(c);
}

ui_screen_t ui_modules_screen_create(ui_app_t *app)
{
    modules_context_t *c = calloc(1, sizeof(*c));
    if(!c) return (ui_screen_t){0};
    c->app = app;
    c->face_pixels = malloc(4*240*240*2);
    c->root = ui_ref_shell_latest(app, "组件模块", "MODULES");
    lv_obj_t *slot = ui_box(c->root, 553, 31, 138, 40, 0xffffff, 20);
    ui_ref_surface(slot, "modules_slot");
    c->slot_dot = ui_box(slot, 14, 16, 8, 8, 0xd4d4d4, 4);
    c->slot = ui_ref_label(slot, "", 15, UI_FONT_REGULAR, 0x525252, 30, 10);
    for(unsigned i = 0; i < 4; ++i) {
        c->buttons[i] = ui_ref_button(c->root, 22, 96 + (int)i * 98, 200, 88, 22, "modules_tile");
        lv_obj_t *box = ui_box(c->buttons[i], 14, 22, 44, 44, 0xe6e6e6, 12);
        lv_obj_t *icon = ui_ref_icon(box, icons[i], 21, 0x525252); lv_obj_center(icon);
        c->names[i] = ui_ref_label(c->buttons[i], names[i], 17, UI_FONT_SEMIBOLD, UI_REF_INK, 70, 23);
        c->statuses[i] = ui_ref_label(c->buttons[i], "未插入", 13, UI_FONT_REGULAR, UI_REF_FAINT, 70, 47);
        lv_obj_set_width(c->statuses[i], 102); lv_label_set_long_mode(c->statuses[i], LV_LABEL_LONG_DOT);
        lv_obj_set_height(c->statuses[i], 18);
        c->dots[i] = ui_box(c->buttons[i], 176, 39, 10, 10, 0x7ed38f, 5);
        c->bindings[i] = (module_binding_t){c, i};
        lv_obj_add_event_cb(c->buttons[i], select_module, LV_EVENT_CLICKED, &c->bindings[i]);
    }
    lv_obj_t *note = ui_box(c->root, 22, 488, 200, 210, 0xe6e6e6, 22);
    ui_ref_surface(note, "modules_note");
    lv_obj_t *label = ui_ref_label(note, "只有一个组件插槽，插入新组件会自动替换当前组件", 13,
                                    UI_FONT_REGULAR, UI_REF_MUTED, 14, 86);
    lv_obj_set_width(label, 172);
    c->panel = ui_box(c->root, 234, 96, 464, 602, 0xffffff, 28);
    ui_ref_surface(c->panel, "modules_panel");
    app_modules_request_probe(app->modules);
    modules_render(c);
    c->timer = lv_timer_create(modules_timer, 250, c);
    return (ui_screen_t){c->root, c, NULL, modules_destroy, modules_paused};
}
