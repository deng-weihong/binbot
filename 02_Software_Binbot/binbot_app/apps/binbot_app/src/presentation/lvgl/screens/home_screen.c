#include "../ui_internal.h"
#include "binbot/app_weather.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    ui_app_t *app;
    ui_route_t route;
} home_route_binding_t;

typedef enum {
    HOME_SURFACE_PAGE,
    HOME_SURFACE_WEATHER,
    HOME_SURFACE_PLAN,
    HOME_SURFACE_QUICK,
    HOME_SURFACE_AI,
    HOME_SURFACE_FOCUS,
    HOME_SURFACE_TODO,
    HOME_SURFACE_REMINDER,
    HOME_SURFACE_SETTINGS,
    HOME_SURFACE_WEATHER_COOL,
    HOME_SURFACE_WEATHER_NIGHT,
    HOME_SURFACE_COUNT,
} home_surface_t;

typedef struct {
    ui_app_t *app;
    lv_obj_t *date;
    lv_obj_t *time;
    lv_obj_t *greeting;
    lv_obj_t *ai_dot;
    lv_obj_t *ai_status;
    lv_obj_t *ai_question;
    lv_obj_t *ai_reply;
    lv_obj_t *ai_empty;
    unsigned chat_revision;
    int chat_state;
    lv_obj_t *plan_percent;
    lv_obj_t *plan_next;
    lv_obj_t *plan_fill;
    lv_obj_t *plan_count;
    lv_obj_t *plan_total;
    lv_obj_t *summary_focus;
    lv_obj_t *summary_focus_suffix;
    lv_obj_t *summary_todo;
    lv_obj_t *summary_todo_total;
    lv_obj_t *summary_next_time;
    lv_obj_t *summary_next_text;
    lv_obj_t *calendar_caption;
    lv_obj_t *calendar_badge;
    lv_obj_t *calendar_badge_text;
    lv_obj_t *focus_arc;
    lv_obj_t *weather_card;
    lv_obj_t *weather_icon;
    lv_obj_t *weather_temperature;
    lv_obj_t *weather_degree;
    lv_obj_t *weather_condition;
    lv_obj_t *weather_range;
    lv_timer_t *weather_timer;
    uint32_t weather_revision;
    bool paused;
    home_route_binding_t routes[18];
    /* Style image sources are borrowed pointers; keep paths for this tree. */
    char surface_paths[HOME_SURFACE_COUNT][384];
} home_context_t;

enum {
    HOME_BACKGROUND = 0xefefef,
    HOME_CARD = 0xfafafa,
    HOME_SOFT = 0xe7e7e7,
    HOME_INK = 0x1a1a1a,
    HOME_MUTED = 0x737373,
    HOME_FAINT = 0xa1a1a1,
    HOME_CLAY = 0xe8643c,
    HOME_WEATHER_HEIGHT = 167,
    HOME_WEATHER_TEXT_TOP = HOME_WEATHER_HEIGHT - 101,
};

static void home_route_clicked(lv_event_t *event);

static void home_chat_update(home_context_t *context)
{
    ui_chat_preview_t preview;
    ui_chat_get_preview(context->app, &preview);
    if(context->chat_state != preview.state) {
        context->chat_state = preview.state;
        lv_label_set_text(context->ai_status, ui_chat_status_text(preview.state));
        lv_obj_set_style_bg_color(context->ai_dot, lv_color_hex(preview.state == 1 ? 0x00bc7d :
                                      preview.state == 0 ? HOME_FAINT : HOME_CLAY), 0);
    }
    if(context->chat_revision == preview.revision) return;
    context->chat_revision = preview.revision;
    lv_label_set_text(context->ai_question, preview.question);
    lv_label_set_text(context->ai_reply, preview.reply);
    lv_obj_t *question = lv_obj_get_parent(context->ai_question);
    lv_obj_t *reply = lv_obj_get_parent(context->ai_reply);
    if(preview.question[0]) lv_obj_remove_flag(question, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(question, LV_OBJ_FLAG_HIDDEN);
    if(preview.reply[0]) lv_obj_remove_flag(reply, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(reply, LV_OBJ_FLAG_HIDDEN);
    if(preview.question[0] || preview.reply[0]) lv_obj_add_flag(context->ai_empty, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(context->ai_empty, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *home_label(lv_obj_t *parent, const char *text, uint16_t size,
                            ui_font_weight_t weight, uint32_t color,
                            int32_t x, int32_t y, int32_t line_height)
{
    const lv_font_t *font = ui_ref_font(size, weight);
    y += (line_height - font->line_height) / 2;
    return ui_label(parent, text, font, color, x, y);
}

static void home_shadow(lv_obj_t *object, uint32_t color, lv_opa_t opacity,
                         int32_t width, int32_t spread, int32_t offset)
{
    lv_obj_set_style_shadow_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_shadow_opa(object, opacity, 0);
    lv_obj_set_style_shadow_width(object, width, 0);
    lv_obj_set_style_shadow_spread(object, spread, 0);
    lv_obj_set_style_shadow_offset_y(object, offset, 0);
}

static lv_obj_t *home_icon(lv_obj_t *parent, const char *name, int32_t size,
                           uint32_t color)
{
    const char *root = getenv("BINBOT_ASSET_ROOT");
    if(!root || !root[0]) root = "/oem/assets";
    char path[384];
    snprintf(path, sizeof(path), "%s%s/home/icons/%ld/%s.png",
             root[0] == 'A' && root[1] == ':' ? "" : "A:", root, (long)size, name);
    lv_obj_t *icon = ui_box(parent, 0, 0, size, size, 0, 0);
    lv_obj_set_style_bg_opa(icon, LV_OPA_TRANSP, 0);
    lv_obj_t *image = lv_image_create(icon);
    lv_obj_remove_flag(image, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_image_set_src(image, path);
    lv_obj_set_style_image_recolor(image, lv_color_hex(color), 0);
    lv_obj_set_style_image_recolor_opa(image, LV_OPA_COVER, 0);
    return icon;
}

static void home_init_surfaces(home_context_t *context)
{
    static const char *names[HOME_SURFACE_COUNT] = {
        "page", "weather", "plan", "quick", "ai", "focus", "todo",
        "reminder", "settings", "weather_cool", "weather_night",
    };
    const char *asset_root = getenv("BINBOT_ASSET_ROOT");
    if(!asset_root || !asset_root[0]) asset_root = "/oem/assets";
    const char *prefix = asset_root[0] == 'A' && asset_root[1] == ':' ? "" : "A:";
    for(size_t i = 0; i < HOME_SURFACE_COUNT; ++i) {
        snprintf(context->surface_paths[i], sizeof(context->surface_paths[i]),
                 "%s%s/home/surfaces/%s.bin", prefix, asset_root, names[i]);
    }
}

static void home_surface(home_context_t *context, lv_obj_t *object,
                          home_surface_t surface)
{
    const char *path = context->surface_paths[surface];
    lv_image_header_t header;
    if(lv_image_decoder_get_info(path, &header) != LV_RESULT_OK) {
        fprintf(stderr, "ui: home surface unavailable: %s\n", path);
        return;
    }
    /* Chrome renders the original CSS at 1:1. LVGL only clips the bitmap to
     * this object's rounded bounds; text and button behavior remain live. */
    lv_obj_set_style_bg_image_src(object, path, 0);
    lv_obj_set_style_bg_image_opa(object, LV_OPA_COVER, 0);
    if(surface == HOME_SURFACE_WEATHER || surface == HOME_SURFACE_FOCUS ||
       surface == HOME_SURFACE_TODO) {
        home_shadow(object, 0x000000, 31, 30, -12, 10);
    }
}

static lv_obj_t *home_route_button(home_context_t *context, size_t index,
                                   ui_route_t route, lv_obj_t *parent,
                                   int32_t x, int32_t y, int32_t width,
                                   int32_t height, uint32_t color, int32_t radius)
{
    context->routes[index].app = context->app;
    context->routes[index].route = route;
    lv_obj_t *button = ui_button(parent, x, y, width, height, color, radius);
    lv_obj_add_event_cb(button, home_route_clicked, LV_EVENT_CLICKED,
                        &context->routes[index]);
    return button;
}

static void home_route_clicked(lv_event_t *event)
{
    home_route_binding_t *binding = lv_event_get_user_data(event);
    if(!binding || !binding->app || binding->app->route_pending) return;
    lv_indev_t *indev = lv_event_get_indev(event);
    if(indev && lv_indev_get_press_moved(indev)) return;
    ui_app_navigate(binding->app, binding->route);
}

static void home_set_text_align(lv_obj_t *label, int32_t width, lv_text_align_t align)
{
    if(!label) return;
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_align(label, align, 0);
}

static lv_obj_t *home_make_arc(lv_obj_t *parent, int32_t x, int32_t y,
                               int32_t size, int32_t width, int32_t value,
                               uint32_t color)
{
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_set_size(arc, size, size);
    lv_obj_set_pos(arc, x, y);
    lv_arc_set_rotation(arc, 270);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_value(arc, value);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(arc, width, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(0xe6e6e6), LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
    return arc;
}

static void home_set_time(home_context_t *context)
{
    char time_text[8];
    ui_format_time(time_text, sizeof(time_text));
    if(context->time) {
        const lv_font_t *font = ui_ref_font(76, UI_FONT_SEMIBOLD);
        lv_point_t bounds;
        lv_text_get_size(&bounds, time_text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        uint16_t size = bounds.x > 195 ? (uint16_t)(76 * 195 / bounds.x) : 76;
        font = ui_ref_font(size, UI_FONT_SEMIBOLD);
        lv_text_get_size(&bounds, time_text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        while(bounds.x > 195 && size > 1) {
            font = ui_ref_font(--size, UI_FONT_SEMIBOLD);
            lv_text_get_size(&bounds, time_text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        }
        ui_resources_set_font(context->time, font, 0);
        lv_obj_set_y(context->time, 39 + (72 - font->line_height) / 2);
        lv_label_set_text(context->time, time_text);
    }
}

static int home_degrees(int tenths)
{
    return (tenths + (tenths < 0 ? -5 : 5)) / 10;
}

static void home_weather_update(home_context_t *context)
{
    bb_component_pen_t weather;
    bool valid = app_modules_weather(context->app->modules, &weather);
    const app_weather_info_t *info = valid ? app_weather_info(weather.condition) : NULL;
    valid = valid && info;
    char temperature[16] = "--", range[96];
    const char *city = getenv("BINBOT_WEATHER_CITY");
    if(!city || !*city) city = "";
    else if(!strcmp(city, "Shenzhen")) city = "深圳";
    if(valid) {
        snprintf(temperature, sizeof(temperature), "%d", home_degrees(weather.temperature));
        snprintf(range, sizeof(range), "%s%s%d°/%d°", city, *city ? " · " : "",
                 home_degrees(weather.low), home_degrees(weather.high));
        lv_label_set_text(context->weather_condition, info->name);
        const char *root = getenv("BINBOT_ASSET_ROOT");
        if(!root || !*root) root = "/oem/assets";
        char path[384];
        snprintf(path, sizeof(path), "%s%s/home/weather/%s.png",
                 root[0] == 'A' && root[1] == ':' ? "" : "A:", root, info->icon);
        lv_image_set_src(context->weather_icon, path);
        lv_obj_remove_flag(context->weather_icon, LV_OBJ_FLAG_HIDDEN);
    } else {
        snprintf(range, sizeof(range), "%s", city);
        lv_label_set_text(context->weather_condition, "暂无天气");
        lv_obj_add_flag(context->weather_icon, LV_OBJ_FLAG_HIDDEN);
    }
    home_surface_t surface = HOME_SURFACE_WEATHER_COOL;
    if(valid) {
        unsigned c = weather.condition;
        if(c == BB_PEN_NIGHT || c == BB_PEN_CLOUD_NIGHT) surface = HOME_SURFACE_WEATHER_NIGHT;
        else if(c == BB_PEN_SUN || c == BB_PEN_CLOUD || c == BB_PEN_STORM ||
                c == BB_PEN_SHOWER || c == BB_PEN_HOT || c == BB_PEN_DUST || c == BB_PEN_HAZE)
            surface = HOME_SURFACE_WEATHER;
    }
    home_surface(context, context->weather_card, surface);
    lv_label_set_text(context->weather_temperature, temperature);
    lv_obj_update_layout(context->weather_temperature);
    lv_obj_set_x(context->weather_degree, 16 + lv_obj_get_width(context->weather_temperature));
    lv_label_set_text(context->weather_range, range);
    uint16_t size = 15;
    lv_point_t bounds;
    const lv_font_t *font;
    do {
        font = ui_ref_font(size, UI_FONT_REGULAR);
        lv_text_get_size(&bounds, range, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    } while(bounds.x > 90 && --size > 1);
    ui_resources_set_font(context->weather_range, font, 0);
    lv_obj_set_y(context->weather_range, HOME_WEATHER_TEXT_TOP + 63 + (23 - font->line_height) / 2);
    context->weather_revision = app_modules_revision(context->app->modules);
}

static void home_weather_timer(lv_timer_t *timer)
{
    home_context_t *context = lv_timer_get_user_data(timer);
    if(context->paused || context->app->content_frozen) return;
    home_chat_update(context);
    if(context->weather_revision != app_modules_revision(context->app->modules)) home_weather_update(context);
}

static size_t home_done_todos(const app_model_t *model)
{
    size_t done = 0;
    if(!model) return done;
    for(size_t i = 0; i < model->todo_count; ++i) {
        if(model->todos[i].done) done++;
    }
    return done;
}

static void home_update_summary(home_context_t *context, const app_model_t *model)
{
    if(!context || !model) return;
    const size_t total = model->todo_count;
    const size_t done = home_done_todos(model);
    const int32_t percent = total ? (int32_t)((done * 100U) / total) : 0;
    const app_todo_t *next = NULL;
    for(size_t i = 0; i < total; ++i) {
        if(!model->todos[i].done) {
            next = &model->todos[i];
            break;
        }
    }

    if(context->plan_percent) {
        char text[24];
        snprintf(text, sizeof(text), "%lu%%", (unsigned long)percent);
        lv_label_set_text(context->plan_percent, text);
    }
    char done_text[16], total_text[16];
    snprintf(done_text, sizeof(done_text), "%lu", (unsigned long)done);
    snprintf(total_text, sizeof(total_text), "/%lu", (unsigned long)total);
    lv_label_set_text(context->plan_count, done_text);
    lv_label_set_text(context->plan_total, total_text);
    lv_obj_update_layout(context->plan_total);
    lv_obj_update_layout(context->plan_count);
    const int32_t total_width = lv_obj_get_width(context->plan_total);
    const int32_t done_width = lv_obj_get_width(context->plan_count);
    lv_obj_set_x(context->plan_total, 295 - 15 - total_width);
    lv_obj_set_x(context->plan_count, 295 - 15 - total_width - done_width);
    if(context->plan_fill) {
        int32_t width = 287 * (percent > 12 ? percent : 12) / 100;
        const int32_t available = 287 - total_width - done_width - 11;
        if(width > available) width = available;
        lv_obj_set_width(context->plan_fill, width);
    }
    if(context->plan_next) {
        char text[128];
        if(next) {
            snprintf(text, sizeof(text), "下一项 · %s%s%s", next->time,
                     next->time[0] ? " " : "", next->text);
        }
        else snprintf(text, sizeof(text), "下一项 · 全部完成");
        lv_label_set_text(context->plan_next, text);
    }
    if(context->summary_todo) {
        lv_label_set_text(context->summary_todo, done_text);
        lv_label_set_text(context->summary_todo_total, total_text);
        lv_obj_update_layout(context->summary_todo);
        lv_obj_set_x(context->summary_todo_total,
                     18 + lv_obj_get_width(context->summary_todo));
    }
}

static void home_calendar_clicked(lv_event_t *event)
{
    home_context_t *context = lv_event_get_user_data(event);
    lv_indev_t *indev = lv_event_get_indev(event);
    if(context->app->route_pending || (indev && lv_indev_get_press_moved(indev))) return;
    const app_calendar_event_t *next = app_calendar_next(&context->app->calendar, time(NULL));
    if(next) ui_calendar_open_event(context->app, next->data.event_id, true);
    else ui_app_navigate(context->app, UI_ROUTE_CALENDAR);
}

static void home_calendar_update(home_context_t *context)
{
    const app_calendar_t *calendar = &context->app->calendar;
    time_t now = time(NULL);
    const app_calendar_event_t *event = app_calendar_next(calendar, now);
    char when[64], badge[80];
    badge[0] = 0;
    if(event) {
        app_calendar_when(when, sizeof(when), event->start, now);
        lv_label_set_text(context->summary_next_time, when);
        lv_label_set_text(context->summary_next_text, event->data.summary);
        lv_obj_remove_flag(context->summary_next_time, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_y(context->calendar_caption, 44);
        lv_obj_set_y(context->summary_next_text, 98);
        bool live = event->start <= now;
        if(live) snprintf(badge, sizeof(badge), "进行中");
        else if(app_calendar_day(event->start) == app_calendar_day(now)) app_calendar_countdown(badge, sizeof(badge), event->start, now);
        const lv_font_t *font = ui_ref_font(13, UI_FONT_MEDIUM);
        lv_point_t bounds;
        lv_text_get_size(&bounds, badge, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if(bounds.x > 84) {
            int minutes = (int)((difftime(event->start, now) + 59) / 60);
            if(minutes % 60) snprintf(badge, sizeof(badge), "%d小时%d分后", minutes / 60, minutes % 60);
            else snprintf(badge, sizeof(badge), "%d小时后", minutes / 60);
        }
        lv_text_get_size(&bounds, badge, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        lv_obj_set_size(context->calendar_badge, bounds.x + 16, 28);
        lv_obj_set_x(context->calendar_badge, 153 - bounds.x - 16);
        lv_obj_set_style_bg_color(context->calendar_badge, lv_color_hex(live ? HOME_CLAY : 0xffffff), 0);
        lv_obj_set_style_bg_opa(context->calendar_badge, live ? 255 : 31, 0);
        lv_label_set_text(context->calendar_badge_text, badge);
        lv_obj_center(context->calendar_badge_text);
        unsigned size = 23;
        do {
            font = ui_ref_font(size, UI_FONT_SEMIBOLD);
            lv_text_get_size(&bounds, when, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        } while(bounds.x > 135 && --size > 12);
        ui_resources_set_font(context->summary_next_time, font, 0);
    }
    else {
        const char *text = calendar->received ? "暂无后续日程" :
                           calendar->connection_known && !calendar->connected ? "暂时无法获取日程" : "正在同步飞书日程…";
        lv_obj_add_flag(context->summary_next_time, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_y(context->calendar_caption, 58);
        lv_obj_set_y(context->summary_next_text, 83);
        lv_label_set_text(context->summary_next_text, text);
    }
    if(badge[0]) lv_obj_remove_flag(context->calendar_badge, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(context->calendar_badge, LV_OBJ_FLAG_HIDDEN);
}

static void create_left_overview(home_context_t *context, lv_obj_t *root)
{
    lv_obj_t *top = ui_box(root, 22, 22, 335, HOME_WEATHER_HEIGHT, HOME_BACKGROUND, 0);
    lv_obj_set_style_bg_opa(top, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(top, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    context->date = home_label(top, "", 19, UI_FONT_MEDIUM, HOME_MUTED, 7, 4, 29);
    context->time = home_label(top, "--:--", 76, UI_FONT_SEMIBOLD, HOME_INK, 7, 39, 72);
    lv_obj_set_width(context->time, 195);
    lv_label_set_long_mode(context->time, LV_LABEL_LONG_CLIP);
    context->greeting = home_label(top, "", 19, UI_FONT_MEDIUM, HOME_INK, 7, 121, 29);
    lv_obj_set_width(context->greeting, 195);

    lv_obj_t *weather = ui_box(top, 213, 0, 122, HOME_WEATHER_HEIGHT, HOME_CARD, 25);
    context->weather_card = weather;
    home_surface(context, weather, HOME_SURFACE_WEATHER);
    context->weather_icon = lv_image_create(weather);
    lv_obj_set_pos(context->weather_icon, -8, -4);
    lv_obj_remove_flag(context->weather_icon, LV_OBJ_FLAG_CLICKABLE);
    context->weather_temperature = home_label(weather, "--", 36, UI_FONT_SEMIBOLD, HOME_INK, 16, HOME_WEATHER_TEXT_TOP, 36);
    context->weather_degree = home_label(weather, "°", 21, UI_FONT_SEMIBOLD, HOME_INK, 55, HOME_WEATHER_TEXT_TOP, 21);
    context->weather_condition = home_label(weather, "", 15, UI_FONT_MEDIUM, HOME_INK, 16, HOME_WEATHER_TEXT_TOP + 40, 23);
    context->weather_range = home_label(weather, "", 15, UI_FONT_REGULAR, HOME_FAINT, 16, HOME_WEATHER_TEXT_TOP + 63, 23);
    lv_obj_set_width(context->weather_range, 90);
    lv_label_set_long_mode(context->weather_range, LV_LABEL_LONG_CLIP);

    lv_obj_t *plan = home_route_button(context, 0, UI_ROUTE_TODO, root,
                                        22, 207, 335, 153, HOME_SOFT, 27);
    home_surface(context, plan, HOME_SURFACE_PLAN);
    home_label(plan, "今日计划已完成", 20, UI_FONT_SEMIBOLD, HOME_INK, 20, 20, 28);
    context->plan_percent = home_label(plan, "0%", 20, UI_FONT_SEMIBOLD,
                                        HOME_CLAY, 164, 20, 28);
    context->plan_next = home_label(plan, "", 16, UI_FONT_REGULAR,
                                     HOME_MUTED, 20, 48, 22);
    lv_obj_set_width(context->plan_next, 235);
    lv_label_set_long_mode(context->plan_next, LV_LABEL_LONG_DOT);
    lv_obj_t *bolt = ui_box(plan, 275, 20, 40, 40, HOME_CLAY, 20);
    home_shadow(bolt, HOME_CLAY, 255, 18, -6, 8);
    lv_obj_t *bolt_icon = home_icon(bolt, "bolt", 21, 0xffffff);
    lv_obj_center(bolt_icon);
    lv_obj_t *track = ui_box(plan, 20, 93, 295, 40, 0xd9d9d9, 20);
    context->plan_fill = ui_box(track, 4, 4, 34, 32, 0xffffff, 16);
    home_shadow(context->plan_fill, 0x000000, 26, 3, 0, 1);
    context->plan_count = home_label(track, "0", 15, UI_FONT_SEMIBOLD,
                                      HOME_INK, 255, 9, 23);
    context->plan_total = home_label(track, "/0", 15, UI_FONT_SEMIBOLD,
                                      HOME_FAINT, 264, 9, 23);

    const struct {
        ui_route_t route;
        const char *icon;
        const char *label;
    } quick[] = {
        {UI_ROUTE_POMODORO, "focus", "专注"},
        {UI_ROUTE_MUSIC, "music", "音乐"},
        {UI_ROUTE_TODO, "bell", "提醒"},
        {UI_ROUTE_CALENDAR, "cal", "日程"},
        {UI_ROUTE_APPS, "more", "更多"},
    };
    for(size_t i = 0; i < sizeof(quick) / sizeof(quick[0]); ++i) {
        const int32_t x = 22 + (int32_t)((275 * i + 2) / 4);
        lv_obj_t *button = home_route_button(context, 1 + i, quick[i].route, root,
                                              x, 392, 60, 108, HOME_SOFT, 30);
        home_surface(context, button, HOME_SURFACE_QUICK);
        lv_obj_t *disc = ui_box(button, 7, 9, 46, 46, 0xffffff, 23);
        home_shadow(disc, 0x000000, 38, 10, -4, 4);
        lv_obj_t *icon = quick[i].route == UI_ROUTE_CALENDAR
                           ? ui_ref_icon(disc, "cal", 22, HOME_INK)
                           : home_icon(disc, quick[i].icon, 22, HOME_INK);
        lv_obj_center(icon);
        lv_obj_t *label = home_label(button, quick[i].label, 15, UI_FONT_MEDIUM,
                                      0x404040, 0, 66, 23);
        home_set_text_align(label, 60, LV_TEXT_ALIGN_CENTER);
    }
}

static void create_ai_card(home_context_t *context, lv_obj_t *root)
{
    lv_obj_t *card = ui_box(root, 375, 22, 323, 478, HOME_SOFT, 31);
    home_surface(context, card, HOME_SURFACE_AI);
    lv_obj_t *title = home_route_button(context, 6, UI_ROUTE_ASSISTANT, card,
                                         90, 18, 144, 36, HOME_CLAY, 18);
    home_shadow(title, HOME_CLAY, 255, 20, -6, 8);
    lv_obj_t *spark = home_icon(title, "spark", 16, 0xffffff);
    lv_obj_set_pos(spark, 14, 10);
    home_label(title, "AI Companion", 15, UI_FONT_MEDIUM, 0xffffff, 37, 7, 23);
    context->ai_dot = ui_box(card, 103, 69, 7, 7, HOME_FAINT, 4);
    context->ai_status = home_label(card, "", 15, UI_FONT_REGULAR, HOME_MUTED, 115, 61, 23);

    lv_obj_t *messages = home_route_button(context, 7, UI_ROUTE_ASSISTANT, card,
                                            18, 84, 287, 267, HOME_SOFT, 0);
    lv_obj_set_style_bg_opa(messages, LV_OPA_TRANSP, 0);
    lv_obj_t *question = ui_box(messages, 109, 74, 178, 45, 0xdcdcdc, 20);
    ui_box(question, 158, 25, 20, 20, 0xdcdcdc, 6);
    lv_obj_t *question_text = home_label(question, "",
                                         15, UI_FONT_MEDIUM, HOME_INK, 14, 11, 23);
    context->ai_question = question_text;
    lv_obj_set_size(question_text, 150, ui_ref_font(15, UI_FONT_MEDIUM)->line_height);
    lv_label_set_long_mode(question_text, LV_LABEL_LONG_DOT);
    lv_obj_t *reply = ui_box(messages, 0, 130, 247, 63, 0xffffff, 20);
    ui_box(reply, 0, 43, 20, 20, 0xffffff, 6);
    home_shadow(reply, 0x000000, 26, 3, 0, 1);
    lv_obj_t *reply_text = home_label(reply, "",
        15, UI_FONT_SEMIBOLD, HOME_INK, 14, 11, 21);
    context->ai_reply = reply_text;
    lv_obj_set_size(reply_text, 219, 42);
    lv_label_set_long_mode(reply_text, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_line_space(reply_text, 21 - ui_ref_font(15, UI_FONT_SEMIBOLD)->line_height, 0);
    lv_obj_add_flag(question, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(reply, LV_OBJ_FLAG_HIDDEN);
    context->ai_empty = home_label(messages, "暂无对话", 15, UI_FONT_REGULAR, HOME_FAINT, 0, 132, 23);
    home_set_text_align(context->ai_empty, 287, LV_TEXT_ALIGN_CENTER);

    const struct {
        const char *label;
        ui_route_t route;
    } actions[] = {
        {"创建提醒", UI_ROUTE_TODO},
        {"查日程", UI_ROUTE_CALENDAR},
        {"专注 25 分", UI_ROUTE_POMODORO},
    };
    for(size_t i = 0; i < 3; ++i) {
        lv_obj_t *action = home_route_button(context, 8 + i, actions[i].route, card,
                                              18 + (int32_t)i * 98, 351, 91, 40,
                                              0xffffff, 20);
        lv_obj_set_style_bg_opa(action, 204, 0);
        home_shadow(action, 0x000000, 26, 3, 0, 1);
        ui_centered_label(action, actions[i].label, ui_ref_font(15, UI_FONT_MEDIUM), HOME_INK);
    }

    lv_obj_t *input = home_route_button(context, 11, UI_ROUTE_ASSISTANT, card,
                                         18, 402, 287, 58, 0xffffff, 29);
    home_shadow(input, 0x000000, 51, 20, -10, 8);
    lv_obj_t *placeholder = home_label(input, "输入消息，或点击麦克风…",
                                        15, UI_FONT_REGULAR, HOME_FAINT, 18, 18, 23);
    lv_obj_set_width(placeholder, 217);
    lv_label_set_long_mode(placeholder, LV_LABEL_LONG_CLIP);
    lv_obj_t *orb = home_route_button(context, 12, UI_ROUTE_ASSISTANT, input,
                                       235, 6, 47, 47, HOME_INK, 24);
    home_shadow(orb, HOME_CLAY, 204, 24, -8, 10);
    lv_obj_t *mic = home_icon(orb, "mic", 20, 0xffffff);
    lv_obj_center(mic);
}

static void create_summary(home_context_t *context, lv_obj_t *root)
{
    lv_obj_t *heading = home_label(root, "Daily Summary", 21, UI_FONT_SEMIBOLD,
                                    HOME_INK, 29, 520, 32);
    lv_obj_update_layout(heading);
    home_label(root, "今日摘要", 15, UI_FONT_REGULAR, HOME_FAINT,
                29 + lv_obj_get_width(heading) + 9, 527, 23);
    lv_obj_t *settings = home_route_button(context, 15, UI_ROUTE_SETTINGS, root,
                                            655, 518, 36, 36, HOME_SOFT, 18);
    home_surface(context, settings, HOME_SURFACE_SETTINGS);
    lv_obj_t *gear = home_icon(settings, "gear", 21, 0x525252);
    lv_obj_center(gear);

    lv_obj_t *focus = home_route_button(context, 16, UI_ROUTE_POMODORO, root,
                                         22, 565, 253, 138, HOME_CARD, 27);
    home_surface(context, focus, HOME_SURFACE_FOCUS);
    context->focus_arc = home_make_arc(focus, 18, 22, 94, 9, 47, HOME_INK);
    home_make_arc(focus, 33, 37, 65, 6, 70, HOME_CLAY);
    ui_box(focus, 126, 38, 5, 5, HOME_INK, 3);
    home_label(focus, "专注时长", 15, UI_FONT_REGULAR, HOME_MUTED, 136, 30, 23);
    context->summary_focus = home_label(focus, "1h25", 26, UI_FONT_SEMIBOLD,
                                         HOME_INK, 126, 56, 26);
    context->summary_focus_suffix = home_label(focus, "m", 15, UI_FONT_MEDIUM,
                                                HOME_INK, 202, 65, 15);
    home_label(focus, "目标 3h", 15, UI_FONT_REGULAR, HOME_FAINT, 126, 86, 23);

    lv_obj_t *todo = home_route_button(context, 17, UI_ROUTE_TODO, root,
                                        289, 565, 224, 138, HOME_CARD, 27);
    home_surface(context, todo, HOME_SURFACE_TODO);
    home_label(todo, "待办完成", 15, UI_FONT_REGULAR, HOME_MUTED, 18, 68, 23);
    context->summary_todo = home_label(todo, "0", 26, UI_FONT_SEMIBOLD,
                                        HOME_INK, 18, 95, 26);
    context->summary_todo_total = home_label(todo, "/0", 26, UI_FONT_SEMIBOLD,
                                              0xd4d4d4, 36, 95, 26);
    const uint8_t bars[] = {30, 55, 20, 70, 45, 85, 60, 100, 40, 65};
    for(size_t i = 0; i < sizeof(bars) / sizeof(bars[0]); ++i) {
        const int32_t height = (bars[i] * 81 + 50) / 100;
        const uint32_t color = i == 7 ? HOME_CLAY : (i % 3 == 1 ? HOME_INK : 0xe3e3e3);
        ui_box(todo, 130 + (int32_t)i * 8, 120 - height, 4, height, color, 2);
    }

    lv_obj_t *next = ui_button(root, 527, 565, 171, 138, 0x2b2b2b, 27);
    lv_obj_add_event_cb(next, home_calendar_clicked, LV_EVENT_CLICKED, context);
    home_surface(context, next, HOME_SURFACE_REMINDER);
    lv_obj_t *bell = ui_ref_icon(next, "cal", 24, HOME_CLAY);
    lv_obj_set_pos(bell, 18, 18);
    context->calendar_badge = ui_box(next, 70, 18, 80, 28, HOME_CLAY, 14);
    context->calendar_badge_text = ui_ref_label(context->calendar_badge, "", 13, UI_FONT_MEDIUM, 0xffffff, 0, 0);
    lv_obj_t *caption = home_label(next, "下一场日程", 15, UI_FONT_REGULAR,
                                    0xffffff, 18, 44, 23);
    context->calendar_caption = caption;
    lv_obj_set_style_text_opa(caption, 153, 0);
    context->summary_next_time = home_label(next, "--", 23, UI_FONT_SEMIBOLD,
                                             0xffffff, 18, 71, 23);
    context->summary_next_text = home_label(next, "", 15, UI_FONT_REGULAR,
                                             0xffffff, 18, 98, 23);
    lv_obj_set_style_text_opa(context->summary_next_text, 204, 0);
    lv_obj_set_size(context->summary_next_text, 135, 23);
    lv_label_set_long_mode(context->summary_next_text, LV_LABEL_LONG_DOT);
}

static void home_update(void *user_data, const app_model_t *model, uint32_t changes)
{
    home_context_t *context = user_data;
    if(!context || !model) return;
    if(changes == APP_CHANGE_ALL || (changes & (APP_CHANGE_CLOCK | APP_CHANGE_CALENDAR)))
        home_calendar_update(context);
    if(changes == APP_CHANGE_ALL || (changes & APP_CHANGE_CLOCK)) {
        char date_text[48];
        static const char *week[] = {"日", "一", "二", "三", "四", "五", "六"};
        time_t now = time(NULL);
        struct tm local;
        if(localtime_r(&now, &local)) {
            snprintf(date_text, sizeof(date_text), "%d月%d日 周%s",
                     local.tm_mon + 1, local.tm_mday, week[local.tm_wday]);
            const char *greeting = local.tm_hour < 5 || local.tm_hour >= 22
                                       ? "夜深了，早点休息"
                                       : local.tm_hour < 9 ? "早上好，开启新一天"
                                       : local.tm_hour < 12 ? "上午好，保持专注"
                                       : local.tm_hour < 18 ? "下午好，保持专注"
                                       : "晚上好，放松一下";
            if(context->greeting) lv_label_set_text(context->greeting, greeting);
        }
        else date_text[0] = '\0';
        if(context->date) lv_label_set_text(context->date, date_text);
        home_set_time(context);
    }
    if(changes == APP_CHANGE_ALL || (changes & (APP_CHANGE_TODOS |
                                                APP_CHANGE_POMODORO |
                                                APP_CHANGE_POMODORO_PROGRESS))) {
        home_update_summary(context, model);
        if(context->focus_arc) {
            const uint32_t minutes = 85U + model->pomodoro.rounds_completed * 25U;
            const int32_t value = minutes >= 180 ? 100 : (int32_t)(minutes * 100 / 180);
            lv_arc_set_value(context->focus_arc, value);
        }
        if(context->summary_focus) {
            const uint32_t minutes = 85U + model->pomodoro.rounds_completed * 25U;
            char text[24];
            snprintf(text, sizeof(text), "%luh%lu", (unsigned long)(minutes / 60U),
                     (unsigned long)(minutes % 60U));
            lv_label_set_text(context->summary_focus, text);
            lv_obj_update_layout(context->summary_focus);
            lv_obj_set_x(context->summary_focus_suffix,
                         126 + lv_obj_get_width(context->summary_focus));
        }
    }
    home_chat_update(context);
}

static void home_destroy(void *user_data)
{
    home_context_t *context = user_data;
    if(context->weather_timer) lv_timer_delete(context->weather_timer);
    free(context);
}

static void home_paused(void *user_data, bool paused)
{
    ((home_context_t *)user_data)->paused = paused;
}

ui_screen_t ui_home_screen_create(ui_app_t *app)
{
    home_context_t *context = calloc(1, sizeof(*context));
    if(!context) return (ui_screen_t){0};
    context->app = app;
    context->chat_state = -1;
    home_init_surfaces(context);
    lv_obj_t *root = ui_box(app->page_layer, 0, 0, UI_WIDTH, UI_HEIGHT,
                            HOME_BACKGROUND, 0);
    home_surface(context, root, HOME_SURFACE_PAGE);
    lv_obj_t *handle = ui_box(root, 317, 7, 86, 5, 0x000000, 3);
    lv_obj_set_style_bg_opa(handle, 38, 0);
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
    create_left_overview(context, root);
    create_ai_card(context, root);
    create_summary(context, root);
    home_weather_update(context);
    context->weather_timer = lv_timer_create(home_weather_timer, 500, context);
    home_update(context, app_state_model(app->state), APP_CHANGE_ALL);
    return (ui_screen_t){
        .root = root,
        .context = context,
        .update = home_update,
        .destroy = home_destroy,
        .set_paused = home_paused,
    };
}
