#ifndef BINBOT_AMBIENT_DISPLAY_H
#define BINBOT_AMBIENT_DISPLAY_H

#include "lvgl.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* This library has no dependency on application state, networking or audio.
 * Call all methods from the LVGL thread. Snapshots and configuration are copied. */
enum { BINBOT_AMBIENT_ID_SIZE = 32, BINBOT_AMBIENT_MAX_THEMES = 32 };
typedef struct binbot_ambient_display binbot_ambient_display_t;

typedef struct {
    char time[16], date[64];
    int year, month, day, weekday, hour, minute, second;
    bool weather_valid;
    char city[48], weather[48];
    char weather_icon[96];
    int temperature, temperature_low, temperature_high;
    bool playing;
    char track[128], artist[128];
    unsigned todo_total, todo_done;
    char next_todo[128];
    bool focus_running;
    uint32_t focus_seconds;
    uint32_t focus_minutes, uptime_seconds;
    int64_t unix_time;
    char weather_id[32], weather_english[24];
    struct { char text[128], time[16]; bool done; } todos[5];
    struct { char title[128], time[16]; int64_t start, end; } events[5];
    unsigned event_count;
    char world_time[3][16];
} binbot_ambient_snapshot_t;

typedef struct {
    char id[BINBOT_AMBIENT_ID_SIZE];
    char name[64], english[48], description[128];
    uint32_t background, foreground, accent;
} binbot_ambient_theme_t;

typedef const lv_font_t *(*binbot_ambient_font_fn)(
    void *user_data, lv_obj_t *owner, uint16_t pixels, unsigned weight);

typedef struct {
    lv_obj_t *parent;
    const char *asset_root;
    /* The returned font must outlive owner. A host cache can use owner deletion
     * to release its loan, rather than retaining every font until shutdown. */
    binbot_ambient_font_fn font;
    /* Called on touch. The host decides what waking means and calls hide().
     * Hide/destroy and showing another theme are safe from this callback. */
    void (*wake)(void *user_data);
    void (*selected)(void *user_data, const char *theme_id);
    bool preview; /* One still frame, no input, entry transition or running timer. */
    void *user_data;
} binbot_ambient_config_t;

binbot_ambient_display_t *binbot_ambient_create(const binbot_ambient_config_t *config);
void binbot_ambient_destroy(binbot_ambient_display_t *display);
size_t binbot_ambient_theme_count(const binbot_ambient_display_t *display);
const binbot_ambient_theme_t *binbot_ambient_theme_at(const binbot_ambient_display_t *display, size_t index);
const binbot_ambient_theme_t *binbot_ambient_theme_find(const binbot_ambient_display_t *display, const char *id);
bool binbot_ambient_show(binbot_ambient_display_t *display, const char *theme_id,
                         const binbot_ambient_snapshot_t *snapshot);
void binbot_ambient_update(binbot_ambient_display_t *display, const binbot_ambient_snapshot_t *snapshot);
void binbot_ambient_hide(binbot_ambient_display_t *display);
void binbot_ambient_raise(binbot_ambient_display_t *display);
bool binbot_ambient_active(const binbot_ambient_display_t *display);
/* Reload an atomically installed resource package while hidden. */
bool binbot_ambient_reload(binbot_ambient_display_t *display);
const char *binbot_ambient_current_theme(const binbot_ambient_display_t *display);
/* Snapshot strings use UTF-8. Templates are bounded and unknown keys are empty. */
void binbot_ambient_format(const binbot_ambient_snapshot_t *snapshot, const char *format,
                           char *output, size_t capacity);

#endif

