#include "../ui_internal.h"

#include <stdlib.h>

enum {
    APP_TILE_COUNT = 13,
};

typedef struct {
    ui_app_t *app;
    ui_route_t route;
} apps_binding_t;

typedef struct {
    ui_app_t *app;
    apps_binding_t tiles[APP_TILE_COUNT];
    lv_obj_t *buttons[APP_TILE_COUNT];
    size_t selected;
} apps_context_t;

typedef struct {
    const char *name;
    const char *en;
    const char *icon;
    ui_route_t route;
} app_tile_t;

static const app_tile_t app_tiles[APP_TILE_COUNT] = {
    {"AI 对话", "Chat", "spark", UI_ROUTE_ASSISTANT},
    {"专注模式", "Focus", "focus", UI_ROUTE_POMODORO},
    {"提醒待办", "Reminder", "bell", UI_ROUTE_TODO},
    {"日程", "Calendar", "cal", UI_ROUTE_CALENDAR},
    {"音乐", "Music", "music", UI_ROUTE_MUSIC},
    {"白噪音", "White Noise", "wave", UI_ROUTE_NOISE},
    {"背单词", "Words", "book", UI_ROUTE_VOCABULARY},
    {"桌面设备", "My Desk", "lightbar", UI_ROUTE_DEVICES},
    {"组件模块", "Modules", "cube", UI_ROUTE_MODULES},
    {"相机", "Camera", "camera", UI_ROUTE_CAMERA},
    {"闹钟", "Alarm", "alarm", UI_ROUTE_ALARM},
    {"吃什么", "Lunch", "bowl", UI_ROUTE_LUNCH},
    {"设置", "Settings", "gear", UI_ROUTE_SETTINGS},
};

static void apps_set_selected(apps_context_t *context, size_t index)
{
    if(!context || index >= APP_TILE_COUNT) return;
    context->selected = index;
    for(size_t i = 0; i < APP_TILE_COUNT; ++i) {
        lv_obj_t *button = context->buttons[i];
        if(!button) continue;
        const bool selected = i == index;
        lv_obj_set_style_outline_width(button, selected ? 2 : 0, 0);
        lv_obj_set_style_outline_pad(button, 2, 0);
        lv_obj_set_style_outline_color(button, lv_color_hex(UI_REF_CLAY), 0);
    }
}

void ui_apps_screen_rotate(ui_app_t *app, int32_t steps)
{
    if(!app || app->route != UI_ROUTE_APPS || !app->current.context || steps == 0) return;
    apps_context_t *context = app->current.context;
    int32_t index = ((int32_t)context->selected + steps % APP_TILE_COUNT +
                     APP_TILE_COUNT) % APP_TILE_COUNT;
    apps_set_selected(context, (size_t)index);
}

static void apps_target_clicked(lv_event_t *event)
{
    apps_binding_t *binding = lv_event_get_user_data(event);
    if(!binding || !binding->app || binding->app->route_pending) return;
    lv_indev_t *indev = lv_event_get_indev(event);
    if(indev && lv_indev_get_press_moved(indev)) return;
    if(binding->route != binding->app->route) {
        ui_app_navigate(binding->app, binding->route);
    }
}

static void apps_gesture(lv_event_t *event)
{
    apps_context_t *context = lv_event_get_user_data(event);
    if(!context || !context->app || context->app->route_pending) return;
    lv_indev_t *indev = lv_event_get_indev(event);
    if(!indev) return;
    lv_dir_t direction = lv_indev_get_gesture_dir(indev);
    if(direction == LV_DIR_RIGHT) {
        lv_indev_wait_release(indev);
        ui_app_navigate(context->app, UI_ROUTE_VOCABULARY);
    }
}

static void create_app_tile(apps_context_t *context, lv_obj_t *root, size_t index)
{
    const app_tile_t *tile = &app_tiles[index];
    const int32_t column = (int32_t)(index % 4U);
    const int32_t row = (int32_t)(index / 4U);
    const int32_t x = 22 + (column * 345 + 1) / 2;
    const int32_t y = 96 + row * 138;

    context->tiles[index] = (apps_binding_t){
        .app = context->app,
        .route = tile->route,
    };
    lv_obj_t *button = ui_ref_button(root, x, y, 159, 124, 27, "noise_apps_tile");
    context->buttons[index] = button;
    lv_obj_add_event_cb(button, apps_target_clicked, LV_EVENT_CLICKED,
                        &context->tiles[index]);

    lv_obj_t *icon_box = ui_box(button, 14, 14, 50, 50, 0xe7e7e7, 25);
    ui_ref_surface(icon_box, "soft50");
    lv_obj_t *icon = ui_ref_icon(icon_box, tile->icon, 23, UI_REF_INK);
    lv_obj_center(icon);

    ui_ref_label(button, tile->en, 15, UI_FONT_REGULAR, UI_REF_FAINT, 14, 61);
    lv_obj_t *name = ui_ref_label(button, tile->name, 20, UI_FONT_SEMIBOLD,
                                  UI_REF_INK, 14, 84);
    lv_obj_set_width(name, 123);
    lv_label_set_long_mode(name, LV_LABEL_LONG_CLIP);
}

static void apps_destroy(void *user_data)
{
    free(user_data);
}

ui_screen_t ui_apps_screen_create(ui_app_t *app)
{
    apps_context_t *context = calloc(1, sizeof(*context));
    if(!context) return (ui_screen_t){0};
    context->app = app;

    lv_obj_t *root = ui_ref_shell_latest(app, "更多功能", "MORE");
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
    for(size_t i = 0; i < APP_TILE_COUNT; ++i) {
        create_app_tile(context, root, i);
    }
    lv_obj_add_event_cb(root, apps_gesture, LV_EVENT_GESTURE, context);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_GESTURE_BUBBLE);
    return (ui_screen_t){
        .root = root,
        .context = context,
        .update = NULL,
        .destroy = apps_destroy,
    };
}
