#define _DEFAULT_SOURCE
#include "ui_internal.h"
#include "binbot/app_weather.h"
#include "draw/snapshot/lv_snapshot.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static time_t current_time = 1792129500;
time_t __wrap_time(time_t *result)
{
    if(result) *result = current_time;
    return current_time;
}

static lv_obj_t *label(lv_obj_t *root, const char *text)
{
    if(lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), text)) return root;
    for(unsigned i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = label(lv_obj_get_child(root, i), text);
        if(found) return found;
    }
    return NULL;
}

static void settle(void)
{
    lv_tick_inc(600);
    lv_timer_handler();
    lv_obj_update_layout(lv_screen_active());
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    (void)area; (void)pixels; lv_display_flush_ready(display);
}

static void snapshot(ui_app_t *app, unsigned index)
{
    const char *directory = getenv("BINBOT_HOME_SNAPSHOT");
    if(!directory) return;
    char path[512]; snprintf(path, sizeof(path), "%s/home-weather-%u.ppm", directory, index);
    lv_draw_buf_t *frame = lv_snapshot_take(app->root, LV_COLOR_FORMAT_RGB888); assert(frame);
    FILE *file = fopen(path, "wb"); assert(file); fprintf(file, "P6\n720 720\n255\n");
    for(unsigned y = 0; y < 720; ++y) for(unsigned x = 0; x < 720; ++x) {
        const uint8_t *p = frame->data + y * frame->header.stride + x * 3;
        uint8_t rgb[] = {p[2], p[1], p[0]}; assert(fwrite(rgb, 1, 3, file) == 3);
    }
    fclose(file); lv_draw_buf_destroy(frame);
}

int main(void)
{
    char directory[] = "/tmp/binbot-home-weather-XXXXXX"; assert(mkdtemp(directory));
    setenv("BINBOT_MODULE_IMAGE_ROOT", directory, 1);
    setenv("BINBOT_INITIAL_ROUTE", "home", 1);
    setenv("BINBOT_WEATHER_CITY", "Shenzhen", 1);
    setenv("PATH", directory, 1);
    char path[512]; snprintf(path, sizeof(path), "%s/wget", directory);
    FILE *file = fopen(path, "wb"); assert(file);
    fputs("#!/bin/sh\n/bin/cp \"$BINBOT_WEATHER_TEST_JSON\" \"$5\"\n", file);
    fclose(file); assert(!chmod(path, 0700));
    snprintf(path, sizeof(path), "%s/fixture.json", directory);
    setenv("BINBOT_WEATHER_TEST_JSON", path, 1);
    lv_init(); lv_display_t *display = lv_display_create(720, 720); assert(display);
    /* Every protocol condition retains a name and a decodable local icon. */
    assert(BB_PEN_CONDITION_COUNT == 20);
    for(unsigned i = 0; i < BB_PEN_CONDITION_COUNT; ++i) {
        const app_weather_info_t *info = app_weather_info(i); assert(info && info->name[0] && info->icon[0]);
        char icon_path[640];
        snprintf(icon_path, sizeof(icon_path), "A:%s/home/weather/%s.png", getenv("BINBOT_ASSET_ROOT"), info->icon);
        lv_image_header_t header; assert(lv_image_decoder_get_info(icon_path, &header) == LV_RESULT_OK);
    }
    assert(!app_weather_info(BB_PEN_CONDITION_COUNT) && !app_weather_info(255));
    static uint8_t buffer[720 * 64 * 4];
    lv_display_set_buffers(display, buffer, NULL, sizeof(buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    app_state_t *state = app_state_create(); ui_app_t *app = ui_app_create(state); assert(app);
    assert(label(app->current.root, "暂无天气"));
    assert(label(app->current.root, "深圳")); snapshot(app, 99);
    static const struct { unsigned code, day, condition; const char *name, *icon; int temperature; } cases[] = {
        {0,1,BB_PEN_SUN,"晴","sun",22}, {2,1,BB_PEN_CLOUD,"多云","cloudy",20},
        {3,1,BB_PEN_OVERCAST,"阴","overcast",17}, {61,1,BB_PEN_RAIN,"小雨","rain",15},
        {95,1,BB_PEN_STORM,"雷阵雨","storm",24}, {73,1,BB_PEN_SNOW,"小雪","snow",-2},
        {45,1,BB_PEN_FOG,"雾","fog",9}, {0,0,BB_PEN_NIGHT,"晴夜","night",14},
        {2,0,BB_PEN_CLOUD_NIGHT,"多云夜","cloudynight",13}, {80,1,BB_PEN_SHOWER,"阵雨","shower",19},
        {65,1,BB_PEN_HEAVY_RAIN,"大雨","heavyrain",16}, {82,1,BB_PEN_DOWNPOUR,"暴雨","downpour",18},
        {75,1,BB_PEN_HEAVY_SNOW,"大雪","heavysnow",-6}, {99,1,BB_PEN_HAIL,"冰雹","hail",12},
    };
    uint32_t now_ms = 0;
    for(unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        file = fopen(path, "wb"); assert(file);
        fprintf(file, "{\"current\":{\"temperature_2m\":%d,\"weather_code\":%u,\"is_day\":%u,\"time\":%ld},"
                      "\"daily\":{\"temperature_2m_min\":[-9],\"temperature_2m_max\":[38]}}",
                cases[i].temperature, cases[i].code, cases[i].day, (long)current_time);
        fclose(file);
        now_ms = (i + 1) * 1801000U;
        bb_component_pen_t weather;
        bool received = false;
        for(unsigned poll = 0; poll < 200; ++poll) {
            app_modules_tick(app->modules, now_ms + poll * 20);
            if(app_modules_weather(app->modules, &weather) && weather.condition == cases[i].condition &&
               weather.temperature == cases[i].temperature * 10) { received = true; break; }
            usleep(10000);
        }
        assert(received && !app_modules_pen_supported(app->modules));
        assert(app_modules_pen_state(app->modules)->condition == weather.condition);
        settle(); assert(label(app->current.root, cases[i].name));
        char temperature[16]; snprintf(temperature, sizeof(temperature), "%d", cases[i].temperature);
        assert(label(app->current.root, temperature));
        lv_obj_t *range = label(app->current.root, "深圳 · -9°/38°"); assert(range);
        lv_point_t bounds;
        lv_text_get_size(&bounds, lv_label_get_text(range), lv_obj_get_style_text_font(range, 0),
                        0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        assert(bounds.x <= lv_obj_get_width(range));
        lv_obj_t *card = lv_obj_get_parent(range); bool found = false;
        for(unsigned child = 0; child < lv_obj_get_child_count(card); ++child) {
            lv_obj_t *image = lv_obj_get_child(card, child);
            if(!lv_obj_check_type(image, &lv_image_class)) continue;
            const char *source = lv_image_get_src(image);
            char expected[64]; snprintf(expected, sizeof(expected), "/%s.png", cases[i].icon);
            assert(source && strstr(source, expected));
            assert(!lv_obj_has_flag(image, LV_OBJ_FLAG_HIDDEN)); found = true;
        }
        assert(found); snapshot(app, i);
        ui_screensaver_preview(app->screensaver, "shapes"); settle();
        assert(!strcmp(binbot_ambient_current_theme(ui_screensaver_display(app->screensaver)), "shapes"));
        snprintf(temperature, sizeof(temperature), "%d°", cases[i].temperature);
        assert(label(app->root, temperature));
        char weather_line[128]; snprintf(weather_line,sizeof(weather_line),"%s · -9° / 38°",cases[i].name);
        assert(label(app->root, weather_line));
        snapshot(app, i + 100);
        ui_screensaver_wake(app->screensaver); settle();
    }
    current_time += 7201;
    app_modules_tick(app->modules, now_ms + 5000); settle();
    assert(label(app->current.root, "暂无天气")); assert(!label(app->current.root, "冰雹"));
    ui_app_navigate(app, UI_ROUTE_APPS); settle();
    ui_app_navigate(app, UI_ROUTE_HOME); settle();
    assert(label(app->current.root, "暂无天气"));
    ui_app_destroy(app); app_state_destroy(state); lv_deinit();
    const char *files[] = {"wget", "fixture.json", ".weather.json", ".weather.bin"};
    for(unsigned i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        snprintf(path, sizeof(path), "%s/%s", directory, files[i]); unlink(path);
    }
    assert(!rmdir(directory));
    puts("Home live weather without hardware, WMO/day-night mapping, fitted range and stale data passed");
    return 0;
}
