#include "ui_internal.h"
#include "draw/snapshot/lv_snapshot.h"
#if LV_USE_EVDEV
#include "drivers/evdev/lv_evdev.h"
#endif

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static lv_point_t point;
static lv_indev_state_t pointer_state;

static void pointer_read(lv_indev_t *input, lv_indev_data_t *data)
{
    (void)input;
    data->point = point;
    data->state = pointer_state;
}

static void settle(void)
{
    for(int i = 0; i < 4; ++i) {
        lv_timer_handler();
        lv_refr_now(NULL);
        usleep(20000);
    }
}

static void tap(lv_indev_t *input, int x, int y)
{
    point = (lv_point_t){x, y};
    pointer_state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(input);
    lv_refr_now(NULL);
    pointer_state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(input);
    settle();
}

static void check_memory(int cycle, int route)
{
    lv_mem_monitor_t memory;
    lv_mem_monitor(&memory);
    printf("cycle=%d route=%d used=%lu largest_free=%lu fragmentation=%u\n", cycle, route,
           (unsigned long)(memory.total_size - memory.free_size),
           (unsigned long)memory.free_biggest_size, memory.frag_pct);
    assert(lv_mem_test() == LV_RESULT_OK);
    assert(memory.free_biggest_size >= 2 * 1024 * 1024);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    lv_init();
    if(!platform_initialize()) return 1;
#if LV_USE_EVDEV
    lv_evdev_discovery_stop();
#endif
    lv_indev_t *input = NULL;
    while((input = lv_indev_get_next(input))) lv_indev_enable(input, false);
    input = lv_indev_create();
    lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(input, pointer_read);
    lv_indev_set_display(input, lv_display_get_default());
    app_state_t *state = app_state_create();
    ui_app_t *app = ui_app_create(state);
    assert(app);
    const ui_route_t routes[] = {
        UI_ROUTE_HOME, UI_ROUTE_POMODORO, UI_ROUTE_ALARM, UI_ROUTE_ASSISTANT,
        UI_ROUTE_SETTINGS, UI_ROUTE_MUSIC, UI_ROUTE_VOCABULARY, UI_ROUTE_TODO,
        UI_ROUTE_WIFI, UI_ROUTE_APPS, UI_ROUTE_DEVICES, UI_ROUTE_LUNCH, UI_ROUTE_CAMERA, UI_ROUTE_MODULES,
    };
    for(int cycle = 0; cycle < 3; ++cycle) {
        for(size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
            ui_app_navigate(app, routes[i]);
            settle();
            assert(app->route == routes[i]);
            if(routes[i] == UI_ROUTE_ALARM) {
                for(int editor = 0; editor < 3; ++editor) {
                    tap(input, 636, 53);
                    tap(input, 652, 68);
                    settle();
                }
                tap(input, 220, 656);
                tap(input, 487, 563);
                settle();
            }
            else if(routes[i] == UI_ROUTE_ASSISTANT) {
                tap(input, 120, 665);
                tap(input, 77, 664);
            }
            ui_control_center_prepare(app);
            ui_app_set_control_center_drag_active(app, true);
            assert(ui_app_open_control_center_drag(app, 740));
            lv_refr_now(NULL);
            ui_app_finish_control_center_drag(app, 740);
            ui_app_set_control_center_drag_active(app, false);
            ui_app_close_control_center(app);
            settle();
            lv_draw_buf_t *frame = lv_snapshot_take(app->root, LV_COLOR_FORMAT_RGB888);
            assert(frame);
            lv_draw_buf_destroy(frame);
            check_memory(cycle, routes[i]);
        }
    }
    lv_indev_delete(input);
    ui_app_destroy(app);
    app_state_destroy(state);
    platform_shutdown();
    lv_deinit();
    return 0;
}
