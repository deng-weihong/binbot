#ifndef BINBOT_APP_CALENDAR_H
#define BINBOT_APP_CALENDAR_H

#include "binbot/audio/protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

typedef struct {
    binbot_audio_calendar_event_t data;
    time_t start;
    time_t end;
    bool all_day;
} app_calendar_event_t;

typedef struct {
    unsigned added;
    unsigned updated;
    unsigned removed;
    app_calendar_event_t event;
} app_calendar_changes_t;

typedef struct {
    app_calendar_event_t events[BINBOT_AUDIO_CALENDAR_EVENT_CAPACITY];
    size_t count;
    app_calendar_changes_t changes;
    uint32_t revision;
    uint32_t generation;
    time_t received_at;
    bool received;
    bool connected;
    bool connection_known;
} app_calendar_t;

typedef struct {
    size_t index;
    int start_minute;
    int end_minute;
    unsigned column;
    unsigned columns;
    bool band;
} app_calendar_segment_t;

bool app_calendar_receive(app_calendar_t *calendar, const binbot_audio_calendar_state_t *snapshot,
                           time_t received_at);
bool app_calendar_set_connected(app_calendar_t *calendar, bool connected);
time_t app_calendar_day(time_t value);
time_t app_calendar_add_days(time_t value, int days);
time_t app_calendar_add_months(time_t value, int months);
unsigned app_calendar_span(const app_calendar_event_t *event);
unsigned app_calendar_day_number(const app_calendar_event_t *event, time_t day);
size_t app_calendar_segments(const app_calendar_t *calendar, time_t day,
                              app_calendar_segment_t segments[BINBOT_AUDIO_CALENDAR_EVENT_CAPACITY]);
const app_calendar_event_t *app_calendar_find(const app_calendar_t *calendar, const char *id);
const app_calendar_event_t *app_calendar_next(const app_calendar_t *calendar, time_t now);
void app_calendar_when(char *text, size_t capacity, time_t value, time_t now);
void app_calendar_countdown(char *text, size_t capacity, time_t start, time_t now);

#endif
