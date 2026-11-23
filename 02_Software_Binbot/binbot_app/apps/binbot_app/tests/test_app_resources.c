#define _DEFAULT_SOURCE
#include "ui_internal.h"
#include "draw/snapshot/lv_snapshot.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{ (void)area; (void)pixels; lv_display_flush_ready(display); }
static void advance(unsigned ms)
{
    while(ms) { unsigned step = ms > 33 ? 33 : ms; lv_tick_inc(step); lv_timer_handler(); ms -= step; }
    lv_obj_update_layout(lv_screen_active());
}
static void draw(lv_obj_t *root)
{
    lv_draw_buf_t *image = lv_snapshot_take(root, LV_COLOR_FORMAT_RGB888);
    assert(image); lv_draw_buf_destroy(image);
}

static lv_point_t pointer_point;
static lv_indev_state_t pointer_state;
static void pointer_read(lv_indev_t *input, lv_indev_data_t *data)
{ (void)input; data->point = pointer_point; data->state = pointer_state; }
static void pointer_sample(lv_indev_t *input, int32_t y, lv_indev_state_t state)
{
    pointer_point = (lv_point_t){360, y}; pointer_state = state;
    lv_indev_read(input); advance(33);
}

static void settings_edge_drag(ui_app_t *app)
{
    ui_app_navigate(app, UI_ROUTE_SETTINGS); advance(200);
    lv_indev_t *pointer = lv_indev_create();
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer, pointer_read);
    lv_indev_set_display(pointer, lv_display_get_default());
    const int32_t starts[] = {7, 25};
    for(size_t i = 0; i < sizeof(starts) / sizeof(starts[0]); ++i) {
        /* This page's header is not clickable. Its background press must
         * survive creation of the control center's clickable overlay. */
        pointer_sample(pointer, starts[i], LV_INDEV_STATE_PRESSED);
        for(int frame = 1; frame <= 30; ++frame)
            pointer_sample(pointer, starts[i] + (710 - starts[i]) * frame / 30,
                           LV_INDEV_STATE_PRESSED);
        pointer_sample(pointer, 710, LV_INDEV_STATE_RELEASED); advance(400);
        assert(app->control_center && ui_control_center_is_active(app->control_center));
        ui_app_close_control_center(app); advance(100); assert(!app->control_center);
    }
    lv_indev_delete(pointer);
    ui_app_navigate(app, UI_ROUTE_HOME); advance(200);
}

static void font_ownership(void)
{
    ui_theme_initialize(); assert(ui_resources_font_count() == 0);
    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    const lv_font_t *shared = ui_ref_font(23, UI_FONT_MEDIUM);
    lv_obj_t *a = ui_label(parent, "共享字体 A", shared, 0, 0, 0);
    lv_obj_t *b = ui_label(parent, "共享字体 B", shared, 0, 0, 40);
    /* The selected part and an inactive state must also retain their font. */
    lv_obj_t *roller = lv_roller_create(parent);
    const lv_font_t *large = ui_ref_font(44, UI_FONT_SEMIBOLD);
    ui_resources_set_font(roller, large, LV_PART_SELECTED | LV_STATE_PRESSED);
    advance(66); assert(ui_resources_font_count() == 2);
    lv_obj_delete(a); advance(66); assert(ui_resources_font_count() == 2);
    assert(lv_obj_get_style_text_font(b, 0) == shared);
    ui_resources_set_font(b, ui_ref_font(29, UI_FONT_REGULAR), 0);
    advance(66); assert(ui_resources_font_count() == 2);
    /* Measurement requests have no widget owner and are released next turn. */
    for(unsigned i = 0; i < 16; ++i) {
        const lv_font_t *font = ui_ref_font((uint16_t)(70 + i), UI_FONT_MEDIUM);
        lv_point_t bounds; lv_text_get_size(&bounds, "12:34", font, 0, 0, 720, 0);
        assert(bounds.x > 0);
    }
    advance(66); assert(ui_resources_font_count() == 2);
    lv_obj_add_state(roller, LV_STATE_PRESSED); draw(parent);
    assert(lv_obj_get_style_text_font(roller, LV_PART_SELECTED) == large);
    lv_obj_delete(parent); advance(66); assert(ui_resources_font_count() == 0);
    ui_theme_shutdown();
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setenv("BINBOT_INITIAL_ROUTE", "home", 1);
    setenv("BINBOT_GESTURE_CAPTURE", "0", 1);
    lv_init(); lv_display_t *display = lv_display_create(720, 720); assert(display);
    static uint8_t buffer[720 * 64 * 4];
    lv_display_set_buffers(display, buffer, NULL, sizeof(buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    font_ownership();
    app_state_t *state = app_state_create(); assert(state);
    ui_app_t *app = ui_app_create(state); assert(app);
    advance(1000); assert(!app->control_center);
    draw(app->root); advance(66);
    size_t baseline = ui_resources_font_count(); assert(baseline > 0);
    settings_edge_drag(app);
    assert(ui_resources_font_count() <= baseline + 1);
    const ui_route_t routes[] = {
        UI_ROUTE_SETTINGS, UI_ROUTE_SCREENSAVER_SETTINGS, UI_ROUTE_APPS,
        UI_ROUTE_MUSIC, UI_ROUTE_NOISE, UI_ROUTE_ALARM, UI_ROUTE_POMODORO,
        UI_ROUTE_VOCABULARY, UI_ROUTE_TODO, UI_ROUTE_CALENDAR, UI_ROUTE_DEVICES,
        UI_ROUTE_ASSISTANT, UI_ROUTE_CAMERA, UI_ROUTE_LUNCH, UI_ROUTE_MODULES,
        UI_ROUTE_ABOUT, UI_ROUTE_DIAGNOSTICS, UI_ROUTE_GESTURES,
        UI_ROUTE_WIFI, UI_ROUTE_BLUETOOTH,
    };
    for(unsigned cycle = 0; cycle < 3; ++cycle) {
        for(size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
            ui_app_navigate(app, routes[i]); advance(200); assert(app->route == routes[i]);
            assert(!app->control_center);
            draw(app->root); advance(66);
            printf("cycle=%u route=%d fonts=%zu\n", cycle, routes[i], ui_resources_font_count());
            assert(ui_resources_font_count() < 48);
            assert(lv_mem_test() == LV_RESULT_OK);
        }
        ui_app_navigate(app, UI_ROUTE_HOME); advance(300); draw(app->root); advance(66);
        size_t returned = ui_resources_font_count();
        printf("home fonts: baseline=%zu returned=%zu\n", baseline, returned);
        assert(returned <= baseline + 1);
        ui_app_open_control_center(app); advance(400); assert(app->control_center);
        draw(app->root);
        ui_app_close_control_center(app); advance(100);
        assert(!app->control_center);
        assert(ui_resources_font_count() <= baseline + 1);
        /* A pending close must not destroy a control center reopened this turn. */
        ui_app_open_control_center(app); ui_app_close_control_center(app);
        ui_app_open_control_center(app); advance(100);
        assert(app->control_center && ui_control_center_is_active(app->control_center));
        ui_app_close_control_center(app); advance(100); assert(!app->control_center);
        ui_screensaver_show(app->screensaver); advance(400); draw(app->root);
        ui_screensaver_wake(app->screensaver); advance(100);
        assert(ui_resources_font_count() <= baseline + 1);
    }
    ui_app_destroy(app); app_state_destroy(state); advance(100);
    assert(ui_resources_font_count() == 0 && lv_mem_test() == LV_RESULT_OK);
    lv_deinit();
    puts("On-demand resources: widget ownership, state fonts, measurements, all routes, overlays, theme wake and cleanup passed");
    return 0;
}
