#include "ui_internal.h"
#include "draw/snapshot/lv_snapshot.h"
#if LV_USE_EVDEV
#include "drivers/evdev/lv_evdev.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static uint32_t flush_start, flush_time;
static lv_point_t pointer_point;
static lv_indev_state_t pointer_state = LV_INDEV_STATE_RELEASED;

static void pointer_read(lv_indev_t *input, lv_indev_data_t *data)
{
    (void)input;
    data->point = pointer_point;
    data->state = pointer_state;
}

static void pointer_sample(lv_indev_t *input, int32_t y, lv_indev_state_t state)
{
    pointer_point = (lv_point_t){360, y};
    pointer_state = state;
    lv_indev_read(input);
    lv_refr_now(NULL);
}

static uint32_t settle_animation(const char *phase)
{
    uint32_t maximum = 0;
    for(int i = 0; i < 30; ++i) {
        uint32_t start = platform_monotonic_ms();
        flush_time = 0;
        lv_timer_handler();
        lv_refr_now(NULL);
        uint32_t elapsed = platform_monotonic_ms() - start;
        if(elapsed > maximum) maximum = elapsed;
        if(elapsed > 40) printf("%s tick=%d ms=%u flush_ms=%u\n", phase, i, elapsed, flush_time);
        usleep(10000);
    }
    printf("%s max_ms=%u\n", phase, maximum);
    return maximum;
}

static void display_event(lv_event_t *event)
{
    if(lv_event_get_code(event) == LV_EVENT_FLUSH_START)
        flush_start = platform_monotonic_ms();
    else if(lv_event_get_code(event) == LV_EVENT_FLUSH_FINISH)
        flush_time += platform_monotonic_ms() - flush_start;
}

static void report_memory(void)
{
    lv_mem_monitor_t memory;
    lv_mem_monitor(&memory);
    printf("memory used=%lu largest_free=%lu\n",
           (unsigned long)(memory.total_size - memory.free_size),
           (unsigned long)memory.free_biggest_size);
}

static bool compare_frames(const lv_draw_buf_t *cached, const lv_draw_buf_t *live)
{
    if(!cached || !live || cached->header.w != live->header.w ||
       cached->header.h != live->header.h) return false;
    unsigned bad_pixels = 0, maximum = 0;
    uint64_t difference = 0;
    for(unsigned y = 0; y < cached->header.h; ++y) {
        const uint8_t *a = cached->data + y * cached->header.stride;
        const uint8_t *b = live->data + y * live->header.stride;
        for(unsigned x = 0; x < cached->header.w; ++x) {
            bool bad = false;
            for(unsigned c = 0; c < 3; ++c) {
                unsigned delta = (unsigned)abs(a[x * 3 + c] - b[x * 3 + c]);
                difference += delta;
                if(delta > maximum) maximum = delta;
                if(delta > 8) bad = true;
            }
            bad_pixels += bad;
        }
    }
    unsigned pixels = cached->header.w * cached->header.h;
    printf("visual max_delta=%u mean_delta=%.4f bad_pixels=%u/%u\n",
            maximum, difference / (pixels * 3.0), bad_pixels, pixels);
    /* Allow small compositing rounding and an asynchronously updated SSID. */
    return bad_pixels < pixels / 200 && difference < (uint64_t)pixels * 3;
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
    lv_display_add_event_cb(lv_display_get_default(), display_event, LV_EVENT_ALL, NULL);
    app_state_t *state = app_state_create();
    ui_app_t *app = ui_app_create(state);
    if(!app) return 2;
    lv_refr_now(NULL);
    if(!getenv("BINBOT_BENCHMARK_COLD")) {
        uint32_t start = platform_monotonic_ms();
        ui_control_center_prepare(app);
        printf("prepare_ms=%u\n", platform_monotonic_ms() - start);
        for(int i = 0; i < 40; ++i) {
            lv_timer_handler();
            usleep(20000);
        }
    }
    const char *cycles_env = getenv("BINBOT_BENCHMARK_CYCLES");
    int cycles = cycles_env ? atoi(cycles_env) : 3;
    for(int cycle = 0; cycle < cycles; ++cycle) {
        uint32_t start = platform_monotonic_ms();
        ui_app_set_control_center_drag_active(app, true);
        if(!ui_app_open_control_center_drag(app, 16)) return 3;
        printf("cycle=%d open_ms=%u\n", cycle, platform_monotonic_ms() - start);
        uint32_t total = 0, maximum = 0;
        for(int frame = 0; frame < 24; ++frame) {
            start = platform_monotonic_ms();
            flush_time = 0;
            ui_app_update_control_center_drag(app, 16 + frame * 724 / 23);
            lv_refr_now(NULL);
            uint32_t elapsed = platform_monotonic_ms() - start;
            total += elapsed;
            if(elapsed > maximum) maximum = elapsed;
            printf("frame=%d ms=%u flush_ms=%u\n", frame, elapsed, flush_time);
        }
        printf("cycle=%d frame_average_ms=%.2f frame_max_ms=%u\n", cycle, total / 24.0, maximum);
        lv_draw_buf_t *cached = lv_snapshot_take(app->root, LV_COLOR_FORMAT_RGB888);
        /* Release partway through travel and compare the settled position
         * against the complete panel, including the animation's final tick. */
        ui_app_update_control_center_drag(app, 180);
        lv_refr_now(NULL);
        ui_app_finish_control_center_drag(app, 180);
        ui_app_set_control_center_drag_active(app, false);
        settle_animation("partial_release");
        /* A model update must restore live controls. Returning to the same
         * value also verifies that the live tree has the cached endpoint. */
        uint8_t volume = app_state_model(state)->volume;
        app_state_set_volume(state, volume == 100 ? 99 : volume + 1);
        app_state_set_volume(state, volume);
        lv_refr_now(NULL);
        lv_draw_buf_t *live = lv_snapshot_take(app->root, LV_COLOR_FORMAT_RGB888);
        bool matches = compare_frames(cached, live);
        if(cached) lv_draw_buf_destroy(cached);
        if(live) lv_draw_buf_destroy(live);
        if(!matches) return 4;
        if(lv_mem_test() != LV_RESULT_OK) return 5;
        report_memory();
        ui_app_close_control_center(app);
        lv_refr_now(NULL);
    }

    /* Exercise the real edge event path: screen coordinates cannot cover
     * the panel's off-screen travel, and releasing must not move its margin. */
    lv_indev_t *pointer = lv_indev_create();
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer, pointer_read);
    lv_indev_set_display(pointer, lv_display_get_default());
    const int32_t starts[] = {7, 36};
    bool endpoints_match = true;
    for(size_t i = 0; i < sizeof(starts) / sizeof(starts[0]); ++i) {
        pointer_sample(pointer, starts[i], LV_INDEV_STATE_PRESSED);
        for(int frame = 1; frame <= 30; ++frame) {
            pointer_sample(pointer, starts[i] + (UI_HEIGHT - 1 - starts[i]) * frame / 30,
                           LV_INDEV_STATE_PRESSED);
            usleep(10000);
        }
        lv_draw_buf_t *held = lv_snapshot_take(app->root, LV_COLOR_FORMAT_RGB888);
        pointer_sample(pointer, UI_HEIGHT - 1, LV_INDEV_STATE_RELEASED);
        settle_animation("bottom_release");
        lv_draw_buf_t *settled = lv_snapshot_take(app->root, LV_COLOR_FORMAT_RGB888);
        printf("edge_start=%ld endpoint ", (long)starts[i]);
        endpoints_match &= compare_frames(held, settled);
        if(held) lv_draw_buf_destroy(held);
        if(settled) lv_draw_buf_destroy(settled);
        ui_app_close_control_center(app);
        lv_refr_now(NULL);
    }
    lv_indev_delete(pointer);
    ui_app_destroy(app);
    app_state_destroy(state);
    platform_shutdown();
    lv_deinit();
    return endpoints_match ? 0 : 6;
}
