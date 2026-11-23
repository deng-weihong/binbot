#include "binbot/app_calendar.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool timestamp(const char *text, time_t *value)
{
    if(!text[0]) return false;
    char *end;
    errno = 0;
    long long number = strtoll(text, &end, 10);
    if(errno || end == text || *end || number < 0 || (long long)(time_t)number != number)
        return false;
    *value = (time_t)number;
    return true;
}

static bool date(const char *text, time_t *value)
{
    int year, month, day, used = 0;
    if(sscanf(text, "%4d-%2d-%2d%n", &year, &month, &day, &used) != 3 ||
       used != 10 || text[used] || year < 1970 || month < 1 || month > 12 || day < 1 || day > 31)
        return false;
    struct tm tm = {.tm_year = year - 1900, .tm_mon = month - 1, .tm_mday = day, .tm_isdst = -1};
    time_t result = mktime(&tm);
    if(result == (time_t)-1 || tm.tm_year != year - 1900 || tm.tm_mon != month - 1 || tm.tm_mday != day)
        return false;
    *value = result;
    return true;
}

static bool valid_strings(const binbot_audio_calendar_event_t *event)
{
#define TERMINATED(field) (memchr(event->field, '\0', sizeof(event->field)) != NULL)
    return TERMINATED(event_id) && TERMINATED(summary) && TERMINATED(start_timestamp) &&
           TERMINATED(end_timestamp) && TERMINATED(start_date) && TERMINATED(end_date) &&
           TERMINATED(timezone) && TERMINATED(location) && TERMINATED(meeting_url) && TERMINATED(status);
#undef TERMINATED
}

static int compare_events(const void *a, const void *b)
{
    const app_calendar_event_t *x = a, *y = b;
    if(x->start != y->start) return x->start < y->start ? -1 : 1;
    return strcmp(x->data.event_id, y->data.event_id);
}

static void trim_utf8_tail(char *text)
{
    size_t length = strlen(text);
    if(!length) return;
    size_t start = length - 1;
    while(start && ((unsigned char)text[start] & 0xc0) == 0x80) --start;
    unsigned char lead = (unsigned char)text[start];
    size_t bytes = lead < 0x80 ? 1 : lead >= 0xc2 && lead <= 0xdf ? 2 :
                   lead >= 0xe0 && lead <= 0xef ? 3 : lead >= 0xf0 && lead <= 0xf4 ? 4 : 0;
    if(!bytes || length - start != bytes) text[start] = 0;
}

static bool same_event(const app_calendar_event_t *a, const app_calendar_event_t *b)
{
    return a->start == b->start && a->end == b->end && a->all_day == b->all_day &&
           !strcmp(a->data.summary, b->data.summary) &&
           !strcmp(a->data.location, b->data.location) &&
           !strcmp(a->data.meeting_url, b->data.meeting_url) &&
           !strcmp(a->data.timezone, b->data.timezone) &&
           !strcmp(a->data.status, b->data.status);
}

bool app_calendar_receive(app_calendar_t *calendar, const binbot_audio_calendar_state_t *snapshot,
                           time_t received_at)
{
    if(!calendar || !snapshot || snapshot->event_count > BINBOT_AUDIO_CALENDAR_EVENT_CAPACITY)
        return false;
    app_calendar_t next = {0};
    for(size_t i = 0; i < snapshot->event_count; ++i) {
        const binbot_audio_calendar_event_t *source = &snapshot->events[i];
        if(!valid_strings(source)) return false;
        if(!strcmp(source->status, "cancelled") || !strcmp(source->status, "canceled")) continue;
        app_calendar_event_t event = {.data = *source, .all_day = !source->start_timestamp[0]};
        bool valid = event.all_day ? date(source->start_date, &event.start) && date(source->end_date, &event.end)
                                   : timestamp(source->start_timestamp, &event.start) && timestamp(source->end_timestamp, &event.end);
        if(!source->event_id[0] || !valid || event.end <= event.start) continue;
        /* IPC strings can end in the middle of a UTF-8 character. */
        trim_utf8_tail(event.data.summary);
        trim_utf8_tail(event.data.location);
        if(!event.data.summary[0]) snprintf(event.data.summary, sizeof(event.data.summary), "未命名日程");
        next.events[next.count++] = event;
    }
    qsort(next.events, next.count, sizeof(next.events[0]), compare_events);
    /* The first snapshot establishes a baseline; replayed revisions stay quiet. */
    if(calendar->received) {
        const app_calendar_event_t *focus = NULL;
        const app_calendar_event_t *added = NULL, *updated = NULL, *removed = NULL;
        for(size_t i = 0; i < next.count; ++i) {
            const app_calendar_event_t *event = &next.events[i];
            const app_calendar_event_t *previous = app_calendar_find(calendar, event->data.event_id);
            if(!previous) {
                ++next.changes.added;
                if(!added) added = event;
            } else if(!same_event(previous, event)) {
                ++next.changes.updated;
                if(!updated) updated = event;
            }
            else continue;
            if(!focus) focus = event;
        }
        for(size_t i = 0; i < calendar->count; ++i) {
            const app_calendar_event_t *event = &calendar->events[i];
            if(app_calendar_find(&next, event->data.event_id)) continue;
            ++next.changes.removed;
            if(!removed) removed = event;
            if(!focus) focus = event;
        }
        if(snapshot->changes_added & BINBOT_AUDIO_CALENDAR_CHANGES_PRESENT) {
            /* Snapshot eviction is not cancellation; use the server's delta. */
            next.changes = (app_calendar_changes_t){0};
            if(snapshot->revision != calendar->revision) {
                next.changes.added = snapshot->changes_added & ~BINBOT_AUDIO_CALENDAR_CHANGES_PRESENT;
                next.changes.updated = snapshot->changes_updated;
                next.changes.removed = snapshot->changes_removed;
            }
            focus = next.changes.added ? added : next.changes.updated ? (updated ? updated : added) :
                    next.changes.removed ? removed : NULL;
        }
        if(focus) next.changes.event = *focus;
    }
    next.revision = snapshot->revision;
    next.generation = calendar->generation + 1;
    next.received = true;
    next.connected = calendar->connected;
    next.connection_known = calendar->connection_known;
    next.received_at = received_at;
    *calendar = next;
    return true;
}

bool app_calendar_set_connected(app_calendar_t *calendar, bool connected)
{
    if(!calendar || (calendar->connection_known && calendar->connected == connected)) return false;
    calendar->connected = connected;
    calendar->connection_known = true;
    ++calendar->generation;
    return true;
}

time_t app_calendar_day(time_t value)
{
    struct tm tm;
    if(!localtime_r(&value, &tm)) return (time_t)-1;
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    tm.tm_isdst = -1;
    return mktime(&tm);
}

time_t app_calendar_add_days(time_t value, int days)
{
    struct tm tm;
    if(!localtime_r(&value, &tm)) return (time_t)-1;
    tm.tm_mday += days;
    tm.tm_isdst = -1;
    return mktime(&tm);
}

time_t app_calendar_add_months(time_t value, int months)
{
    struct tm tm;
    if(!localtime_r(&value, &tm)) return (time_t)-1;
    tm.tm_mon += months;
    tm.tm_mday = 1;
    tm.tm_isdst = -1;
    return mktime(&tm);
}

unsigned app_calendar_span(const app_calendar_event_t *event)
{
    return event ? (unsigned)(difftime(app_calendar_day(event->end - 1), app_calendar_day(event->start)) / 86400) + 1 : 0;
}

unsigned app_calendar_day_number(const app_calendar_event_t *event, time_t day)
{
    return event ? (unsigned)(difftime(app_calendar_day(day), app_calendar_day(event->start)) / 86400) + 1 : 0;
}

size_t app_calendar_segments(const app_calendar_t *calendar, time_t day,
                              app_calendar_segment_t segments[BINBOT_AUDIO_CALENDAR_EVENT_CAPACITY])
{
    if(!calendar || !segments) return 0;
    const time_t start = app_calendar_day(day), end = app_calendar_add_days(start, 1);
    size_t count = 0;
    for(size_t i = 0; i < calendar->count; ++i) {
        const app_calendar_event_t *event = &calendar->events[i];
        if(event->start >= end || event->end <= start) continue;
        segments[count++] = (app_calendar_segment_t){.index = i,
            .start_minute = event->start > start ? (int)(difftime(event->start, start) / 60) : 0,
            .end_minute = event->end < end ? (int)((difftime(event->end, start) + 59) / 60) : 1440,
            .columns = 1, .band = event->all_day || (event->start <= start && event->end >= end)};
    }
    /* Connected overlap groups share a column count; unrelated later events use the full width. */
    size_t group[BINBOT_AUDIO_CALENDAR_EVENT_CAPACITY], length = 0;
    int group_end = -1;
    unsigned columns = 1;
    for(size_t i = 0; i <= count; ++i) {
        if(i < count && segments[i].band) continue;
        if(length && (i == count || segments[i].start_minute >= group_end)) {
            for(size_t j = 0; j < length; ++j) segments[group[j]].columns = columns;
            length = 0;
            group_end = -1;
            columns = 1;
        }
        if(i == count) break;
        bool used[BINBOT_AUDIO_CALENDAR_EVENT_CAPACITY] = {0};
        for(size_t j = 0; j < length; ++j) {
            const app_calendar_segment_t *previous = &segments[group[j]];
            if(previous->end_minute > segments[i].start_minute) used[previous->column] = true;
        }
        unsigned column = 0;
        while(column < BINBOT_AUDIO_CALENDAR_EVENT_CAPACITY && used[column]) ++column;
        segments[i].column = column;
        if(columns < column + 1) columns = column + 1;
        group[length++] = i;
        if(group_end < segments[i].end_minute) group_end = segments[i].end_minute;
    }
    return count;
}

const app_calendar_event_t *app_calendar_find(const app_calendar_t *calendar, const char *id)
{
    if(calendar && id) for(size_t i = 0; i < calendar->count; ++i)
        if(!strcmp(calendar->events[i].data.event_id, id)) return &calendar->events[i];
    return NULL;
}

const app_calendar_event_t *app_calendar_next(const app_calendar_t *calendar, time_t now)
{
    if(calendar) for(size_t i = 0; i < calendar->count; ++i)
        if(!calendar->events[i].all_day && calendar->events[i].end > now) return &calendar->events[i];
    return NULL;
}

void app_calendar_when(char *text, size_t capacity, time_t value, time_t now)
{
    struct tm tm;
    if(!localtime_r(&value, &tm)) { if(capacity) text[0] = 0; return; }
    time_t day = app_calendar_day(value), today = app_calendar_day(now);
    if(day == today) snprintf(text, capacity, "%02d:%02d", tm.tm_hour, tm.tm_min);
    else if(day == app_calendar_add_days(today, 1)) snprintf(text, capacity, "明天 %02d:%02d", tm.tm_hour, tm.tm_min);
    else snprintf(text, capacity, "%d月%d日 %02d:%02d", tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
}

void app_calendar_countdown(char *text, size_t capacity, time_t start, time_t now)
{
    int minutes = (int)((difftime(start, now) + 59) / 60);
    if(minutes < 1) minutes = 1;
    if(minutes < 60) snprintf(text, capacity, "还有 %d 分钟", minutes);
    else if(minutes % 60) snprintf(text, capacity, "还有 %d 小时 %d 分", minutes / 60, minutes % 60);
    else snprintf(text, capacity, "还有 %d 小时", minutes / 60);
}
