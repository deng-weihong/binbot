#define _DEFAULT_SOURCE
#include "ui_internal.h"
#include "draw/snapshot/lv_snapshot.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

enum { PREVIEW_SIZE = 152 };

static uint8_t display_buffer[720 * 64 * 4];

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    (void)area;
    (void)pixels;
    lv_display_flush_ready(display);
}

static void advance(unsigned milliseconds)
{
    while(milliseconds) {
        unsigned step = milliseconds > 33 ? 33 : milliseconds;
        lv_tick_inc(step);
        lv_timer_handler();
        milliseconds -= step;
    }
    lv_obj_update_layout(lv_screen_active());
}

static const lv_font_t *preview_font(void *user_data, lv_obj_t *owner, uint16_t size, unsigned weight)
{
    (void)user_data;
    const lv_font_t *font = ui_ref_font(size, weight >= 600 ? UI_FONT_SEMIBOLD :
                                        weight >= 500 ? UI_FONT_MEDIUM : UI_FONT_REGULAR);
    return ui_resources_retain_font(owner, font) ? font : LV_FONT_DEFAULT;
}

static void sample_snapshot(binbot_ambient_snapshot_t *data)
{
    memset(data, 0, sizeof(*data));
    snprintf(data->time, sizeof(data->time), "20:00");
    snprintf(data->date, sizeof(data->date), "11月4日  星期三");
    data->year = 2026;
    data->month = 11;
    data->day = 4;
    data->weekday = 3;
    data->hour = 20;
    data->weather_valid = true;
    snprintf(data->city, sizeof(data->city), "深圳");
    snprintf(data->weather, sizeof(data->weather), "多云");
    snprintf(data->weather_icon, sizeof(data->weather_icon), "home/weather/cloudy.png");
    snprintf(data->weather_id, sizeof(data->weather_id), "cloudy");
    snprintf(data->weather_english, sizeof(data->weather_english), "CLOUDY");
    data->temperature = 22;
    data->temperature_low = 18;
    data->temperature_high = 25;
    snprintf(data->track, sizeof(data->track), "晚风");
    snprintf(data->artist, sizeof(data->artist), "小伴");
    data->todo_total = 3;
    data->todo_done = 1;
    snprintf(data->next_todo, sizeof(data->next_todo), "整理书桌");
    snprintf(data->todos[0].text, sizeof(data->todos[0].text), "整理书桌");
    snprintf(data->todos[0].time, sizeof(data->todos[0].time), "20:30");
    snprintf(data->todos[1].text, sizeof(data->todos[1].text), "准备明天的东西");
    snprintf(data->todos[1].time, sizeof(data->todos[1].time), "21:00");
    snprintf(data->todos[2].text, sizeof(data->todos[2].text), "阅读十分钟");
    snprintf(data->todos[2].time, sizeof(data->todos[2].time), "21:30");
    data->todos[2].done = true;
    data->focus_running = true;
    data->focus_seconds = 15 * 60;
    data->focus_minutes = 25;
    data->uptime_seconds = 60 * 60 * 5;
    snprintf(data->world_time[0], sizeof(data->world_time[0]), "12:00");
    snprintf(data->world_time[1], sizeof(data->world_time[1]), "07:00");
    snprintf(data->world_time[2], sizeof(data->world_time[2]), "21:00");
    data->event_count = 2;
    snprintf(data->events[0].title, sizeof(data->events[0].title), "散步");
    snprintf(data->events[0].time, sizeof(data->events[0].time), "20:30");
    snprintf(data->events[1].title, sizeof(data->events[1].title), "睡前阅读");
    snprintf(data->events[1].time, sizeof(data->events[1].time), "21:00");
    struct tm local = {.tm_year = data->year - 1900, .tm_mon = data->month - 1,
                       .tm_mday = data->day, .tm_hour = data->hour, .tm_isdst = -1};
    data->unix_time = mktime(&local);
    data->events[0].start = data->unix_time + 30 * 60;
    data->events[0].end = data->unix_time + 60 * 60;
    data->events[1].start = data->unix_time + 60 * 60;
    data->events[1].end = data->unix_time + 90 * 60;
}

static bool save_preview(const char *directory, const char *id, lv_obj_t *clip)
{
    lv_draw_buf_t *frame = lv_snapshot_take(clip, LV_COLOR_FORMAT_RGB565);
    if(!frame) return false;
    bool valid = frame->header.w == PREVIEW_SIZE && frame->header.h == PREVIEW_SIZE &&
                 frame->header.cf == LV_COLOR_FORMAT_RGB565 && frame->header.stride >= PREVIEW_SIZE * 2;
    char path[512];
    int length = snprintf(path, sizeof(path), "%s/%s.rgb565", directory, id);
    if(length < 0 || (size_t)length >= sizeof(path)) valid = false;
    FILE *file = valid ? fopen(path, "wb") : NULL;
    if(!file) valid = false;
    for(unsigned y = 0; valid && y < PREVIEW_SIZE; ++y)
        valid = fwrite(frame->data + y * frame->header.stride, 1, PREVIEW_SIZE * 2, file) == PREVIEW_SIZE * 2;
    if(file && fclose(file) != 0) valid = false;
    lv_draw_buf_destroy(frame);
    if(!valid) fprintf(stderr, "preview export failed: %s\n", id);
    return valid;
}

int main(int argc, char **argv)
{
    if(argc != 3) {
        fprintf(stderr, "usage: %s <assets-root> <output-directory>\n", argv[0]);
        return 2;
    }
    const char *assets = argv[1];
    const char *output = argv[2];
    if(mkdir(output, 0755) != 0 && errno != EEXIST) {
        perror(output);
        return 1;
    }
    setenv("BINBOT_ASSET_ROOT", assets, 1);
    setenv("TZ", "Asia/Shanghai", 1);
    tzset();

    lv_init();
    lv_rand_set_seed(7);
    lv_display_t *display = lv_display_create(720, 720);
    if(!display) { lv_deinit(); return 1; }
    lv_display_set_buffers(display, display_buffer, NULL, sizeof(display_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_obj_t *screen = lv_screen_active();
    lv_obj_remove_style_all(screen);
    lv_obj_t *clip = lv_obj_create(screen);
    lv_obj_remove_style_all(clip);
    lv_obj_set_pos(clip, 0, 0);
    lv_obj_set_size(clip, PREVIEW_SIZE, PREVIEW_SIZE);
    lv_obj_set_style_bg_color(clip, lv_color_hex(0x141414), 0);
    lv_obj_set_style_bg_opa(clip, LV_OPA_COVER, 0);
    lv_obj_set_style_clip_corner(clip, true, 0);
    lv_obj_t *layer = lv_obj_create(clip);
    lv_obj_remove_style_all(layer);
    lv_obj_set_pos(layer, 0, 0);
    lv_obj_set_size(layer, 720, 720);
    lv_obj_set_style_transform_pivot_x(layer, 0, 0);
    lv_obj_set_style_transform_pivot_y(layer, 0, 0);
    lv_obj_set_style_transform_scale(layer, 54, 0);

    binbot_ambient_config_t config = {
        .parent = layer,
        .asset_root = assets,
        .font = preview_font,
        .preview = true,
    };
    binbot_ambient_display_t *preview = binbot_ambient_create(&config);
    binbot_ambient_snapshot_t data;
    sample_snapshot(&data);
    bool ok = preview != NULL;
    size_t count = preview ? binbot_ambient_theme_count(preview) : 0;
    size_t rendered = 0;
    for(size_t i = 0; ok && i < count; ++i) {
        const binbot_ambient_theme_t *theme = binbot_ambient_theme_at(preview, i);
        ok = theme && binbot_ambient_show(preview, theme->id, &data) &&
             !strcmp(binbot_ambient_current_theme(preview), theme->id);
        if(ok) {
            advance(66);
            ok = save_preview(output, theme->id, clip);
            if(ok) ++rendered;
        }
    }
    if(preview) binbot_ambient_destroy(preview);
    lv_obj_delete(clip);
    advance(66);
    ui_resources_shutdown();
    lv_deinit();
    if(!ok || !count) {
        fprintf(stderr, "expected %zu previews; rendered %zu\n", count, rendered);
        return 1;
    }
    printf("generated %zu fixed standby previews in %s\n", count, output);
    return 0;
}
