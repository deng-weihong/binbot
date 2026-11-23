#include "binbot/app_workspace.h"
#include "binbot/build_info.h"
#include <cJSON.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <math.h>
#include <netinet/in.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

enum { OFFLINE_REVIEWS = 4096, REPORT_REVIEWS = 8 };
struct app_workspace {
    app_runtime_t *runtime;
    ui_app_t *ui;
    binbot_audio_client_t *audio;
    bb_control_service_t *control;
    cJSON *document, *system;
    app_vocabulary_card_t cards[APP_VOCABULARY_CAPACITY];
    char path[512];
    binbot_audio_noise_state_t noise;
    uint32_t system_at;
    uint64_t pending_request;
    enum { PENDING_NONE, PENDING_NOISE, PENDING_MIC, PENDING_MOTOR, PENDING_LIGHT, PENDING_PEN, PENDING_DISPLAY } pending;
    unsigned wanted_direction, wanted_speed;
    bb_component_light_settings_t wanted_light;
    unsigned wanted_pen_mode, wanted_pen_face;
    bool audio_initialized, restore_mix, noise_received;
};

static const char *get_string(const cJSON *object, const char *key)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(value) ? value->valuestring : "";
}

static int number(const cJSON *object, const char *key, int minimum, int maximum, int fallback)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsNumber(value) && isfinite(value->valuedouble) &&
           floor(value->valuedouble) == value->valuedouble &&
           value->valuedouble >= minimum && value->valuedouble <= maximum ? value->valueint : fallback;
}

static void replace(cJSON *object, const char *key, cJSON *value)
{
    cJSON_DeleteItemFromObjectCaseSensitive(object, key);
    cJSON_AddItemToObject(object, key, value);
}

static bool save_document(app_workspace_t *w, const cJSON *document)
{
    char temporary[528];
    snprintf(temporary, sizeof(temporary), "%s.tmp", w->path);
    char *json = cJSON_PrintUnformatted(document);
    if(!json) return false;
    int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    FILE *file = fd < 0 ? NULL : fdopen(fd, "w");
    if(!file) { if(fd >= 0) close(fd); free(json); return false; }
    bool ok = fchmod(fd, 0600) == 0 && fputs(json, file) >= 0 && fflush(file) == 0 && fsync(fd) == 0;
    if(fclose(file) != 0) ok = false;
    free(json);
    if(ok) ok = rename(temporary, w->path) == 0;
    if(!ok) unlink(temporary);
    if(ok) {
        char directory[512]; snprintf(directory, sizeof(directory), "%s", w->path);
        char *slash = strrchr(directory, '/');
        if(slash) {
            *slash = '\0'; int parent = open(directory[0] ? directory : "/", O_RDONLY | O_DIRECTORY);
            if(parent < 0) ok = false;
            else { ok = fsync(parent) == 0; close(parent); }
        }
    }
    return ok;
}

static bool bind_cards(app_workspace_t *w)
{
    cJSON *cards = cJSON_GetObjectItemCaseSensitive(w->document, "cards");
    if(!cJSON_IsArray(cards) || cJSON_GetArraySize(cards) > APP_VOCABULARY_CAPACITY) return false;
    memset(w->cards, 0, sizeof(w->cards));
    for(int i = 0; i < cJSON_GetArraySize(cards); ++i) {
        cJSON *card = cJSON_GetArrayItem(cards, i);
        if(!get_string(card, "word")[0] || !get_string(card, "meaning")[0]) return false;
        w->cards[i] = (app_vocabulary_card_t){get_string(card, "word"), get_string(card, "phonetic"),
            get_string(card, "meaning"), get_string(card, "example"), get_string(card, "translation")};
    }
    return true;
}

static bool install_document(app_workspace_t *w, cJSON *next)
{
    if(!save_document(w, next)) return false;
    cJSON *previous = w->document;
    w->document = next;
    (void)bind_cards(w);
    cJSON_Delete(previous);
    return true;
}

static cJSON *progress_json(const app_vocabulary_t *progress)
{
    cJSON *value = cJSON_CreateObject();
    cJSON_AddNumberToObject(value, "index", progress->current_index);
    cJSON_AddNumberToObject(value, "known", progress->known_mask);
    cJSON_AddNumberToObject(value, "attempted", progress->attempted_mask);
    cJSON_AddNumberToObject(value, "finished", progress->finished_mask);
    cJSON_AddNumberToObject(value, "reviewed", progress->reviewed_count);
    cJSON_AddNumberToObject(value, "correct", progress->correct_count);
    cJSON_AddNumberToObject(value, "cycles", progress->cycles_completed);
    return value;
}

static bool read_learning_plan(const cJSON *value, size_t count, app_vocabulary_plan_t *plan)
{
    if(!value) return true;
    if(!cJSON_IsObject(value)) return false;
    int due = number(value, "due", 0, 25000, -1);
    int new_count = number(value, "new", 0, 25000, -1);
    int learned = number(value, "learned_today", 0, 25000, -1);
    int new_mask = number(value, "new_mask", 0, (int)((1U << count) - 1U), -1);
    double next = cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(value, "next_review_at"));
    if(due < 0 || new_count < 0 || learned < 0 || new_mask < 0 || !isfinite(next) ||
       next < 0 || next > 4102444800.0 || floor(next) != next) return false;
    *plan = (app_vocabulary_plan_t){true, (uint16_t)due, (uint16_t)new_count,
                                  (uint16_t)learned, (uint32_t)next, (uint32_t)new_mask};
    return true;
}

static bool record_review(const app_vocabulary_card_t *card, app_vocabulary_answer_t answer, bool retry,
                          const app_vocabulary_t *progress, void *context)
{
    app_workspace_t *w = context;
    cJSON *next = cJSON_Duplicate(w->document, true);
    cJSON *events = cJSON_GetObjectItemCaseSensitive(next, "events");
    time_t now = time(NULL);
    if(!next || !cJSON_IsArray(events) || cJSON_GetArraySize(events) >= OFFLINE_REVIEWS || now < 946684800) {
        cJSON_Delete(next); fprintf(stderr, "workspace: cannot persist learning event\n"); return false;
    }
    double sequence = cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(next, "sequence")) + 1;
    char id[96]; snprintf(id, sizeof(id), "%s:%.0f", get_string(next, "namespace"), sequence);
    cJSON *event = cJSON_CreateObject();
    cJSON_AddStringToObject(event, "id", id);
    cJSON_AddStringToObject(event, "book_id", get_string(next, "book_id"));
    cJSON_AddStringToObject(event, "word", card->word);
    cJSON_AddNumberToObject(event, "at", (double)now);
    cJSON_AddBoolToObject(event, "remembered", answer == APP_VOCABULARY_ANSWER_KNOWN);
    cJSON_AddStringToObject(event, "grade", answer == APP_VOCABULARY_ANSWER_KNOWN ? "known" :
                           answer == APP_VOCABULARY_ANSWER_FUZZY ? "fuzzy" : "forgot");
    cJSON_AddBoolToObject(event, "retry", retry);
    cJSON_AddItemToArray(events, event);
    replace(next, "sequence", cJSON_CreateNumber(sequence));
    replace(next, "progress", progress_json(progress));
    if(!install_document(w, next)) { cJSON_Delete(next); return false; }
    return true;
}

app_workspace_t *app_workspace_create(app_runtime_t *runtime, ui_app_t *ui,
                                       binbot_audio_client_t *audio, const char *state_path)
{
    app_workspace_t *w = calloc(1, sizeof(*w));
    if(!w) return NULL;
    w->runtime = runtime; w->ui = ui; w->audio = audio;
    snprintf(w->path, sizeof(w->path), "%s.workspace.json", state_path && *state_path ? state_path : "/oem/data/binbot_state.ini");
    FILE *file = fopen(w->path, "rb");
    if(file) {
        if(fseek(file, 0, SEEK_END) != 0) { fclose(file); free(w); return NULL; }
        long size = ftell(file); rewind(file);
        if(size < 2 || size > 8 * 1024 * 1024) { fclose(file); free(w); return NULL; }
        char *json = malloc((size_t)size + 1);
        if(!json) { fclose(file); free(w); return NULL; }
        size_t read = fread(json, 1, (size_t)size, file); fclose(file); json[read] = '\0';
        w->document = cJSON_ParseWithOpts(json, NULL, true); free(json);
        if(!cJSON_IsObject(w->document) || !get_string(w->document, "namespace")[0] ||
           !cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(w->document, "events")) ||
           !isfinite(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(w->document, "sequence"))) || !bind_cards(w)) {
            cJSON_Delete(w->document); free(w); return NULL;
        }
        cJSON *saved = cJSON_GetObjectItemCaseSensitive(w->document, "progress");
        app_vocabulary_t progress = {0};
        progress.current_index = (size_t)number(saved, "index", 0, 30, 0);
        int max_mask = (1U << APP_VOCABULARY_CAPACITY) - 1U;
        progress.known_mask = (uint32_t)number(saved, "known", 0, max_mask, 0);
        progress.attempted_mask = (uint32_t)number(saved, "attempted", 0, max_mask, 0);
        progress.finished_mask = (uint32_t)number(saved, "finished", 0, max_mask, 0);
        progress.reviewed_count = (uint16_t)number(saved, "reviewed", 0, UINT16_MAX, 0);
        progress.correct_count = (uint16_t)number(saved, "correct", 0, UINT16_MAX, 0);
        progress.cycles_completed = (uint16_t)number(saved, "cycles", 0, UINT16_MAX, 0);
        size_t count = (size_t)cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(w->document, "cards"));
        (void)read_learning_plan(cJSON_GetObjectItemCaseSensitive(w->document, "plan"), count, &progress.plan);
        (void)app_state_set_vocabulary(app_runtime_state(runtime), w->cards, count, &progress);
    } else {
        if(errno != ENOENT) { free(w); return NULL; }
        const app_vocabulary_t *v = &app_state_model(app_runtime_state(runtime))->vocabulary;
        w->document = cJSON_CreateObject();
        unsigned char random[16]; char namespace_id[33];
        file = fopen("/dev/urandom", "rb");
        bool random_ok = file && fread(random, 1, sizeof(random), file) == sizeof(random);
        if(file) fclose(file);
        if(!random_ok) { cJSON_Delete(w->document); free(w); return NULL; }
        for(unsigned i = 0; i < 16; ++i) snprintf(namespace_id + i * 2, 3, "%02x", random[i]);
        cJSON_AddStringToObject(w->document, "namespace", namespace_id);
        cJSON_AddStringToObject(w->document, "book_id", "daily-english");
        cJSON_AddStringToObject(w->document, "deck_id", "builtin");
        cJSON_AddNumberToObject(w->document, "sequence", 0);
        cJSON_AddStringToObject(w->document, "timezone", "Asia/Shanghai");
        cJSON_AddNumberToObject(w->document, "legacy_reviewed", v->reviewed_count);
        cJSON_AddNumberToObject(w->document, "legacy_correct", v->correct_count);
        cJSON_AddNumberToObject(w->document, "legacy_cycles", v->cycles_completed);
        cJSON_AddItemToObject(w->document, "progress", progress_json(v));
        cJSON_AddArrayToObject(w->document, "events");
        cJSON *cards = cJSON_AddArrayToObject(w->document, "cards");
        cJSON *known = cJSON_AddArrayToObject(w->document, "legacy_known");
        for(size_t i = 0; i < v->card_count; ++i) {
            const app_vocabulary_card_t *card = &v->cards[i];
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "word", card->word); cJSON_AddStringToObject(item, "phonetic", card->phonetic);
            cJSON_AddStringToObject(item, "meaning", card->meaning); cJSON_AddStringToObject(item, "example", card->example);
            cJSON_AddStringToObject(item, "translation", card->translation); cJSON_AddItemToArray(cards, item);
            if(v->known_mask & (1U << i)) cJSON_AddItemToArray(known, cJSON_CreateString(card->word));
        }
        if(!save_document(w, w->document)) { cJSON_Delete(w->document); free(w); return NULL; }
        (void)bind_cards(w);
        app_vocabulary_t progress = *v;
        (void)app_state_set_vocabulary(app_runtime_state(runtime), w->cards, v->card_count, &progress);
    }
    int utc_offset = number(w->document, "utc_offset", -86400, 86400, 28800) / 60;
    char timezone[32]; snprintf(timezone, sizeof(timezone), "UTC%s%02d:%02d", utc_offset >= 0 ? "-" : "+", abs(utc_offset) / 60, abs(utc_offset) % 60);
    setenv("TZ", timezone, 1); tzset();
    w->restore_mix = cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(w->document, "noise"));
    app_state_set_review_handler(app_runtime_state(runtime), record_review, w);
    (void)app_runtime_flush(runtime);
    return w;
}

void app_workspace_set_control(app_workspace_t *w, bb_control_service_t *control)
{ if(w) w->control = control; }

void app_workspace_destroy(app_workspace_t *w)
{
    if(!w) return;
    app_state_set_review_handler(app_runtime_state(w->runtime), NULL, NULL);
    cJSON_Delete(w->document); cJSON_Delete(w->system); free(w);
}

static void update_system(app_workspace_t *w)
{
    cJSON_Delete(w->system); w->system = cJSON_CreateObject();
    cJSON_AddStringToObject(w->system, "app_version", BINBOT_APP_VERSION);
    cJSON_AddStringToObject(w->system, "build_date", BINBOT_APP_BUILD_DATE);
    struct utsname name;
    if(uname(&name) == 0) {
        cJSON_AddStringToObject(w->system, "architecture", name.machine);
        cJSON_AddStringToObject(w->system, "kernel", name.release);
        cJSON_AddStringToObject(w->system, "hostname", name.nodename);
    }
    struct sysinfo info;
    if(sysinfo(&info) == 0) {
        cJSON_AddNumberToObject(w->system, "uptime_seconds", info.uptime);
        cJSON_AddNumberToObject(w->system, "memory_total", (double)info.totalram * info.mem_unit);
        cJSON_AddNumberToObject(w->system, "memory_free", (double)(info.freeram + info.bufferram) * info.mem_unit);
        cJSON_AddNumberToObject(w->system, "load_average", (double)info.loads[0] / 65536.0);
    }
    struct statvfs disk;
    if(statvfs("/userdata", &disk) == 0) {
        cJSON_AddNumberToObject(w->system, "storage_total", (double)disk.f_blocks * disk.f_frsize);
        cJSON_AddNumberToObject(w->system, "storage_free", (double)disk.f_bavail * disk.f_frsize);
    }
    struct ifaddrs *interfaces = NULL;
    if(getifaddrs(&interfaces) == 0) {
        for(struct ifaddrs *item = interfaces; item; item = item->ifa_next) {
            if(!item->ifa_addr || item->ifa_addr->sa_family != AF_INET || (item->ifa_flags & IFF_LOOPBACK)) continue;
            char address[INET_ADDRSTRLEN];
            if(inet_ntop(AF_INET, &((struct sockaddr_in *)item->ifa_addr)->sin_addr, address, sizeof(address))) {
                cJSON_AddStringToObject(w->system, "local_ip", address); break;
            }
        }
        freeifaddrs(interfaces);
    }
}

cJSON *app_workspace_snapshot(app_workspace_t *w)
{
    if(!w) return NULL;
    const app_model_t *model = app_state_model(app_runtime_state(w->runtime));
    cJSON *root = cJSON_CreateObject(), *features = cJSON_AddObjectToObject(root, "features");
    cJSON_AddBoolToObject(features, "microphone", !model->mic_muted);
    cJSON_AddBoolToObject(features, "notifications", !model->dnd);
    cJSON_AddStringToObject(features, "timezone", get_string(w->document, "timezone"));
    cJSON_AddNumberToObject(features, "utcOffset", number(w->document, "utc_offset", -86400, 86400, 28800));
    cJSON *face = cJSON_AddObjectToObject(features, "watchFace");
    cJSON_AddStringToObject(face, "themeId", model->screensaver.theme_id);
    cJSON_AddStringToObject(face, "standbyId", model->screensaver.standby_id);
    cJSON_AddNumberToObject(face, "idleSeconds", model->screensaver.delay_seconds);
    cJSON_AddBoolToObject(face, "shuffle", model->screensaver.shuffle);
    static const char *gesture_ids[] = {"ok", "thumb", "palm", "victory", "fist"};
    static const char *actions[] = {"pc-on", "pc-sleep", "lamp", "play", "next", "previous", "photo", "focus", "wake", "noise", "mute", "nothing"};
    cJSON *gestures = cJSON_AddObjectToObject(features, "gestures");
    cJSON_AddBoolToObject(gestures, "enabled", model->gestures.enabled);
    cJSON_AddBoolToObject(gestures, "confirmSensitive", model->gestures.confirm_sensitive);
    static const char *sensitivity[] = {"low", "medium", "high"};
    cJSON_AddStringToObject(gestures, "sensitivity", sensitivity[model->gestures.sensitivity <= 2 ? model->gestures.sensitivity : 1]);
    cJSON *mappings = cJSON_AddObjectToObject(gestures, "mappings");
    for(unsigned i = 0; i < APP_GESTURE_COUNT; ++i) {
        cJSON *mapping = cJSON_AddObjectToObject(mappings, gesture_ids[i]);
        cJSON_AddBoolToObject(mapping, "enabled", (model->gestures.enabled_mask & (1U << i)) != 0);
        cJSON_AddStringToObject(mapping, "action", actions[model->gestures.actions[i] < APP_GESTURE_ACTION_COUNT ? model->gestures.actions[i] : APP_GESTURE_NOTHING]);
    }
    static const char *sounds[] = {"rain", "waves", "fire", "forest", "cafe", "wind", "storm", "stream", "typing"};
    cJSON *noise = cJSON_AddObjectToObject(features, "whiteNoise"), *levels = cJSON_AddObjectToObject(noise, "levels");
    for(unsigned i = 0; i < 9; ++i) cJSON_AddNumberToObject(levels, sounds[i], w->noise.levels[i]);
    cJSON_AddNumberToObject(noise, "master", w->noise.master);
    cJSON_AddNumberToObject(noise, "timerSeconds", w->noise.timer_seconds);
    cJSON_AddNumberToObject(noise, "remainingSeconds", w->noise.remaining_seconds);
    cJSON_AddBoolToObject(noise, "playing", w->noise.playing != 0);
    cJSON *focus = cJSON_AddObjectToObject(features, "focus");
    cJSON_AddStringToObject(focus, "mode", model->pomodoro.mode == APP_POMODORO_FOCUS ? "focus" : "break");
    cJSON_AddNumberToObject(focus, "secondsLeft", model->pomodoro.seconds_left);
    cJSON_AddNumberToObject(focus, "focusSeconds", model->pomodoro.focus_seconds);
    cJSON_AddNumberToObject(focus, "roundsCompleted", model->pomodoro.rounds_completed);
    cJSON_AddBoolToObject(focus, "running", model->pomodoro.running);
    cJSON *todos = cJSON_AddArrayToObject(features, "todos");
    for(size_t i = 0; i < model->todo_count; ++i) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", model->todos[i].id);
        cJSON_AddStringToObject(item, "text", model->todos[i].text);
        cJSON_AddStringToObject(item, "time", model->todos[i].time);
        cJSON_AddBoolToObject(item, "done", model->todos[i].done); cJSON_AddItemToArray(todos, item);
    }
    const app_vocabulary_t *v = &model->vocabulary;
    cJSON *vocabulary = cJSON_AddObjectToObject(features, "vocabulary"), *known = cJSON_AddArrayToObject(vocabulary, "knownWords");
    for(size_t i = 0; i < v->card_count; ++i)
        if(v->known_mask & (1U << i)) cJSON_AddItemToArray(known, cJSON_CreateString(v->cards[i].word));
    cJSON_AddStringToObject(vocabulary, "bookId", get_string(w->document, "book_id"));
    cJSON_AddStringToObject(vocabulary, "deckId", get_string(w->document, "deck_id"));
    cJSON_AddStringToObject(vocabulary, "currentWord", !v->complete && v->card_count && v->current_index < v->card_count ? v->cards[v->current_index].word : "");
    cJSON_AddNumberToObject(vocabulary, "reviewedCount", v->reviewed_count);
    cJSON_AddNumberToObject(vocabulary, "correctCount", v->correct_count);
    cJSON_AddNumberToObject(vocabulary, "cyclesCompleted", v->cycles_completed);
    cJSON_AddBoolToObject(vocabulary, "complete", v->complete);
    cJSON_AddNumberToObject(vocabulary, "studyPlanVersion", 1);
    cJSON_AddBoolToObject(vocabulary, "studying", ui_app_studying_vocabulary(w->ui));
    cJSON_AddItemToObject(vocabulary, "legacyKnownWords", cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(w->document, "legacy_known"), true));
    cJSON_AddNumberToObject(vocabulary, "legacyReviewedCount", number(w->document, "legacy_reviewed", 0, UINT16_MAX, 0));
    cJSON_AddNumberToObject(vocabulary, "legacyCorrectCount", number(w->document, "legacy_correct", 0, UINT16_MAX, 0));
    cJSON_AddNumberToObject(vocabulary, "legacyCyclesCompleted", number(w->document, "legacy_cycles", 0, UINT16_MAX, 0));
    cJSON *events = cJSON_AddArrayToObject(root, "learning_events");
    const cJSON *pending = cJSON_GetObjectItemCaseSensitive(w->document, "events");
    for(int i = 0; i < cJSON_GetArraySize(pending) && i < REPORT_REVIEWS; ++i)
        cJSON_AddItemToArray(events, cJSON_Duplicate(cJSON_GetArrayItem(pending, i), true));
    app_modules_t *modules = ui_app_modules(w->ui);
    unsigned profile = app_modules_profile(modules);
    cJSON *module = cJSON_AddObjectToObject(features, "modules");
    const char *connected = profile == 1 ? "pen" : profile == 2 ? "holo" : profile == 3 ? "turntable" : profile == 5 ? "light" : NULL;
    if(connected && app_modules_healthy(modules)) cJSON_AddStringToObject(module, "connected", connected);
    else cJSON_AddNullToObject(module, "connected");
    cJSON_AddNumberToObject(module, "profile", profile);
    cJSON_AddBoolToObject(module, "healthy", app_modules_healthy(modules));
    cJSON_AddBoolToObject(module, "motorSupported", app_modules_motor_supported(modules));
    cJSON_AddBoolToObject(module, "lightSupported", app_modules_light_supported(modules));
    cJSON_AddBoolToObject(module, "penSupported", app_modules_pen_supported(modules));
    const binbot_component_event_t *push = app_modules_push_status(modules);
    if(push && push->status >= BINBOT_COMPONENT_PREPARING && push->status <= BINBOT_COMPONENT_CANCELLED) {
        static const char *phases[] = {
            "preparing", "downloading", "transferring", "validating", "activating",
            "verifying", "reconciling", "succeeded", "failed", "cancelled",
        };
        char transfer_id[33];
        for(size_t i = 0; i < sizeof(push->request_id); ++i)
            snprintf(transfer_id + 2 * i, 3, "%02x", (unsigned)push->request_id[i]);
        cJSON *transfer = cJSON_AddObjectToObject(module, "imageTransfer");
        cJSON_AddStringToObject(transfer, "id", transfer_id);
        cJSON_AddStringToObject(transfer, "imageId", app_modules_push_image(modules));
        cJSON_AddNumberToObject(transfer, "profile", app_modules_push_profile(modules));
        cJSON_AddStringToObject(transfer, "status", phases[push->status - BINBOT_COMPONENT_PREPARING]);
        cJSON_AddNumberToObject(transfer, "bytesWritten", push->bytes_written);
        cJSON_AddNumberToObject(transfer, "totalBytes", push->total_bytes);
    }
    const bb_component_pen_t *pen_state = app_modules_pen_state(modules);
    for(unsigned current = 1; current <= 2; ++current) {
        cJSON *display = cJSON_AddObjectToObject(module, current == 1 ? "pen" : "holo");
        const char *image = app_modules_current(modules, current);
        cJSON_AddStringToObject(display, "imageId", image ? image : "");
        cJSON_AddNumberToObject(display, "intervalSeconds", app_modules_random_interval(modules, current));
        if(current == 1) {
            cJSON_AddStringToObject(display, "mode", pen_state && pen_state->mode ? "info" : "image");
            cJSON_AddStringToObject(display, "face", !pen_state || pen_state->face == 0 ? "both" : pen_state->face == 1 ? "time" : "weather");
        }
    }
    cJSON *motor = cJSON_AddObjectToObject(module, "turntable");
    unsigned direction = app_modules_motor_direction(modules);
    cJSON_AddStringToObject(motor, "direction", direction == 1 ? "clockwise" : direction == 2 ? "counterclockwise" : "stop");
    cJSON_AddNumberToObject(motor, "speed", app_modules_motor_speed(modules));
    cJSON *light = cJSON_AddObjectToObject(module, "light");
    const bb_component_light_settings_t *light_state = app_modules_light_state(modules);
    static const char *effects[] = {"常亮", "呼吸", "流光", "闪烁", "心跳"};
    char color[8]; snprintf(color, sizeof(color), "#%02x%02x%02x", light_state ? light_state->red : 0, light_state ? light_state->green : 0, light_state ? light_state->blue : 0);
    cJSON_AddStringToObject(light, "color", color);
    cJSON_AddStringToObject(light, "effect", effects[light_state && light_state->effect <= 4 ? light_state->effect : 0]);
    cJSON_AddBoolToObject(light, "enabled", light_state && light_state->enabled);
    cJSON_AddNumberToObject(light, "brightness", light_state ? light_state->brightness : 0);
    cJSON_AddNumberToObject(light, "speed", light_state ? light_state->speed : 0);
    cJSON *images = cJSON_AddArrayToObject(module, "images");
    for(size_t i = 0; i < app_modules_image_count(modules) && i < 32; ++i) {
        const app_module_image_t *image = app_modules_image(modules, i);
        if(!image) continue;
        cJSON *item = cJSON_CreateObject(); cJSON_AddStringToObject(item, "id", image->id);
        cJSON_AddStringToObject(item, "name", image->name); cJSON_AddItemToArray(images, item);
    }
    if(!w->system) update_system(w);
    cJSON_AddItemToObject(root, "system", cJSON_Duplicate(w->system, true));
    size_t known_count = (size_t)cJSON_GetArraySize(known);
    cJSON_AddNumberToObject(vocabulary, "knownCount", known_count);
    char *wire = cJSON_PrintUnformatted(root);
    bool trimmed = false;
    while(wire && strlen(wire) > 13500 && cJSON_GetArraySize(known) > 0) {
        cJSON_DeleteItemFromArray(known, cJSON_GetArraySize(known) - 1);
        trimmed = true; free(wire); wire = cJSON_PrintUnformatted(root);
    }
    if(trimmed) cJSON_AddBoolToObject(vocabulary, "knownWordsTruncated", true);
    while(wire && strlen(wire) > 13500 && cJSON_GetArraySize(images) > 0) {
        cJSON_DeleteItemFromArray(images, cJSON_GetArraySize(images) - 1);
        free(wire); wire = cJSON_PrintUnformatted(root);
    }
    free(wire);
    return root;
}

static bool send_audio(app_workspace_t *w, unsigned type, uint64_t request, const void *data, size_t size)
{
    binbot_audio_packet_t packet;
    binbot_audio_packet_init(&packet, (binbot_audio_message_type_t)type, (uint32_t)size, request, 0);
    memcpy(&packet.payload, data, size);
    return binbot_audio_client_send(w->audio, &packet);
}

static void set_timezone_offset(int offset)
{
    char zone[40]; int absolute = offset < 0 ? -offset : offset;
    snprintf(zone, sizeof(zone), "UTC%c%d:%02d", offset < 0 ? '+' : '-', absolute / 3600, (absolute % 3600) / 60);
    if(setenv("TZ", zone, 1) == 0) tzset();
}

int app_workspace_command(app_workspace_t *w, const char *name, const cJSON *p,
                           const char *command_id, uint64_t request, char *message, size_t capacity)
{
    (void)command_id;
    if(!w || !cJSON_IsObject(p)) return 0;
    snprintf(message, capacity, "设备不支持该操作或参数无效");
    app_state_t *state = app_runtime_state(w->runtime);
    const app_model_t *model = app_state_model(state);
    app_modules_t *modules = ui_app_modules(w->ui);
    if(!strcmp(name, "watchface.set")) {
        app_screensaver_settings_t next = {0}, previous = model->screensaver;
        const char *theme = get_string(p, "themeId"), *standby = get_string(p, "standbyId");
        int seconds = number(p, "idleSeconds", 0, 3600, -1);
        const cJSON *shuffle = cJSON_GetObjectItemCaseSensitive(p, "shuffle");
        const cJSON *show_now = cJSON_GetObjectItemCaseSensitive(p, "showNow");
        if(strlen(theme) >= sizeof(next.theme_id) || strlen(standby) >= sizeof(next.standby_id) ||
           !*theme || !*standby || seconds < 0 || !cJSON_IsBool(shuffle) ||
           (show_now && !cJSON_IsBool(show_now))) return 0;
        snprintf(next.theme_id, sizeof(next.theme_id), "%s", theme);
        snprintf(next.standby_id, sizeof(next.standby_id), "%s", standby);
        next.delay_seconds = (uint16_t)seconds; next.shuffle = cJSON_IsTrue(shuffle);
        if(!app_state_set_screensaver(state, &next)) return 0;
        if(!app_runtime_flush(w->runtime)) { (void)app_state_set_screensaver(state, &previous); return 0; }
        if(cJSON_IsTrue(show_now) || (!show_now && strcmp(next.theme_id, previous.theme_id))) {
            if(!ui_app_show_watchface(w->ui, next.theme_id)) {
                (void)app_state_set_screensaver(state, &previous);
                (void)app_runtime_flush(w->runtime);
                snprintf(message, capacity, "设备未能显示所选表盘");
                return 0;
            }
            snprintf(message, capacity, "表盘已显示");
            return 1;
        }
    } else if(!strcmp(name, "gestures.set")) {
        static const char *ids[] = {"ok", "thumb", "palm", "victory", "fist"};
        static const char *actions[] = {"pc-on", "pc-sleep", "lamp", "play", "next", "previous", "photo", "focus", "wake", "noise", "mute", "nothing"};
        app_gesture_settings_t next = {0}, previous = model->gestures;
        const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(p, "enabled");
        const cJSON *confirm = cJSON_GetObjectItemCaseSensitive(p, "confirmSensitive");
        const cJSON *mappings = cJSON_GetObjectItemCaseSensitive(p, "mappings");
        const char *sensitivity = get_string(p, "sensitivity");
        if(!cJSON_IsBool(enabled) || !cJSON_IsBool(confirm) || !cJSON_IsObject(mappings)) return 0;
        next.enabled = cJSON_IsTrue(enabled); next.confirm_sensitive = cJSON_IsTrue(confirm);
        if(!strcmp(sensitivity, "low")) next.sensitivity = 0;
        else if(!strcmp(sensitivity, "medium")) next.sensitivity = 1;
        else if(!strcmp(sensitivity, "high")) next.sensitivity = 2;
        else return 0;
        for(unsigned i = 0; i < APP_GESTURE_COUNT; ++i) {
            const cJSON *binding = cJSON_GetObjectItemCaseSensitive(mappings, ids[i]);
            const cJSON *on = cJSON_GetObjectItemCaseSensitive(binding, "enabled");
            if(!cJSON_IsBool(on)) return 0;
            if(cJSON_IsTrue(on)) next.enabled_mask |= (uint8_t)(1U << i);
            unsigned action;
            for(action = 0; action < APP_GESTURE_ACTION_COUNT; ++action)
                if(!strcmp(get_string(binding, "action"), actions[action])) break;
            if(action == APP_GESTURE_ACTION_COUNT) return 0;
            next.actions[i] = (uint8_t)action;
        }
        if(!app_state_set_gestures(state, &next)) return 0;
        if(!app_runtime_flush(w->runtime)) { (void)app_state_set_gestures(state, &previous); return 0; }
    } else if(!strcmp(name, "preferences.set")) {
        const cJSON *notifications = cJSON_GetObjectItemCaseSensitive(p, "notifications");
        const char *zone = get_string(p, "timezone");
        int offset = number(p, "utcOffset", -86400, 86400, 8 * 3600);
        if(!cJSON_IsBool(notifications) || !*zone || strlen(zone) > 80) return 0;
        cJSON *next = cJSON_Duplicate(w->document, true);
        replace(next, "timezone", cJSON_CreateString(zone));
        replace(next, "utc_offset", cJSON_CreateNumber(offset));
        if(!install_document(w, next)) { cJSON_Delete(next); return 0; }
        set_timezone_offset(offset);
        if(model->dnd == cJSON_IsTrue(notifications)) app_state_toggle_control(state, APP_CONTROL_DND);
        if(!app_runtime_flush(w->runtime)) return 0;
    } else if(!strcmp(name, "microphone.set")) {
        const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(p, "enabled");
        if(!cJSON_IsBool(enabled)) return 0;
        binbot_audio_volume_set_t setting = {.volume = cJSON_IsTrue(enabled) ? 1 : 0};
        if(!send_audio(w, BINBOT_AUDIO_MSG_MICROPHONE_SET, request, &setting, sizeof(setting))) {
            snprintf(message, capacity, "音频服务尚未连接"); return 0;
        }
        w->pending_request = request; w->pending = PENDING_MIC; return 2;
    } else if(!strcmp(name, "noise.set")) {
        binbot_audio_noise_state_t setting = {0};
        const cJSON *levels = cJSON_GetObjectItemCaseSensitive(p, "levels");
        const cJSON *playing = cJSON_GetObjectItemCaseSensitive(p, "playing");
        int volume = number(p, "volume", 0, 100, -1), minutes = number(p, "timerMinutes", 0, 180, -1);
        if(!cJSON_IsBool(playing) || !cJSON_IsArray(levels) || cJSON_GetArraySize(levels) != 9 || volume < 0 || minutes < 0) return 0;
        for(unsigned i = 0; i < 9; ++i) {
            const cJSON *level = cJSON_GetArrayItem(levels, (int)i);
            if(!cJSON_IsNumber(level) || !isfinite(level->valuedouble) || floor(level->valuedouble) != level->valuedouble || level->valueint < 0 || level->valueint > 100) return 0;
            setting.levels[i] = (uint8_t)level->valueint;
        }
        setting.master = (uint8_t)volume; setting.playing = cJSON_IsTrue(playing) ? 1 : 0;
        setting.timer_seconds = (uint32_t)minutes * 60;
        if(!send_audio(w, BINBOT_AUDIO_MSG_NOISE_CONTROL, request, &setting, sizeof(setting))) return 0;
        w->pending_request = request; w->pending = PENDING_NOISE; return 2;
    } else if(!strcmp(name, "focus.set")) {
        int minutes = number(p, "minutes", 5, 120, 0);
        if(cJSON_HasObjectItem(p, "minutes") && !minutes) return 0;
        const char *action = get_string(p, "action"), *mode = get_string(p, "mode");
        if(*mode && strcmp(mode, "focus") && strcmp(mode, "break")) return 0;
        if(strcmp(action, "start") && strcmp(action, "pause") && strcmp(action, "reset") && strcmp(action, "configure")) return 0;
        if(minutes && (uint32_t)minutes * 60 != model->pomodoro.focus_seconds) app_state_set_pomodoro_duration(state, (uint32_t)minutes);
        if(*mode) {
            app_pomodoro_mode_t target = !strcmp(mode, "focus") ? APP_POMODORO_FOCUS : APP_POMODORO_BREAK;
            if(model->pomodoro.mode != target) app_state_set_pomodoro_mode(state, target);
        }
        if(!strcmp(action, "start")) app_state_start_pomodoro(state);
        else if(!strcmp(action, "pause")) app_state_pause_pomodoro(state);
        else if(!strcmp(action, "reset")) app_state_reset_pomodoro(state);
        if(!app_runtime_flush(w->runtime)) return 0;
        if(!strcmp(action, "start")) {
            bool shown = ui_app_show_pomodoro(w->ui);
            snprintf(message, capacity, "%s", shown ? "已开始计时并打开专注页面" : "计时已开始，专注页面打开失败");
            return shown ? 1 : 0;
        }
    } else if(!strcmp(name, "todo.replace")) {
        const cJSON *items = cJSON_GetObjectItemCaseSensitive(p, "items");
        if(!cJSON_IsArray(items) || cJSON_GetArraySize(items) > APP_TODO_CAPACITY) return 0;
        app_todo_t next[APP_TODO_CAPACITY] = {0}, old[APP_TODO_CAPACITY];
        size_t old_count = model->todo_count; memcpy(old, model->todos, sizeof(old));
        int count = cJSON_GetArraySize(items);
        for(int i = 0; i < count; ++i) {
            const cJSON *item = cJSON_GetArrayItem(items, i), *done = cJSON_GetObjectItemCaseSensitive(item, "done");
            const char *id = get_string(item, "id"), *content = get_string(item, "text"), *time_value = get_string(item, "time");
            if(!*id || !*content || strlen(id) >= sizeof(next[i].id) || strlen(content) >= sizeof(next[i].text) || strlen(time_value) >= sizeof(next[i].time) || !cJSON_IsBool(done)) return 0;
            bool existing = false;
            for(size_t j = 0; j < model->todo_count; ++j) if(!strcmp(model->todos[j].id, id)) {
                next[i] = model->todos[j]; existing = true; break;
            }
            snprintf(next[i].id, sizeof(next[i].id), "%s", id);
            snprintf(next[i].text, sizeof(next[i].text), "%s", content);
            if(cJSON_HasObjectItem(item, "time")) snprintf(next[i].time, sizeof(next[i].time), "%s", time_value);
            if(!existing) { snprintf(next[i].tag, sizeof(next[i].tag), "待办"); next[i].tone = 0x30b0c7; }
            next[i].done = cJSON_IsTrue(done);
        }
        if(!app_state_replace_todos(state, next, (size_t)count)) return 0;
        if(!app_runtime_flush(w->runtime)) { (void)app_state_replace_todos(state, old, old_count); return 0; }
    } else if(!strncmp(name, "todo.", 5)) {
        int index = number(p, "index", 0, APP_TODO_CAPACITY - 1, -1);
        const char *content = get_string(p, "text"); bool ok = false;
        if(!strcmp(name, "todo.add") && *content && strlen(content) < 72) ok = app_state_add_todo(state, content);
        else if(!strcmp(name, "todo.edit") && index >= 0 && *content && strlen(content) < 72) ok = app_state_edit_todo(state, (size_t)index, content);
        else if(!strcmp(name, "todo.delete") && index >= 0) ok = app_state_delete_todo(state, (size_t)index);
        else if(!strcmp(name, "todo.set_done") && index >= 0 && cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(p, "done"))) ok = app_state_set_todo_done(state, (size_t)index, cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(p, "done")));
        if(!ok || !app_runtime_flush(w->runtime)) return 0;
    } else if(!strcmp(name, "learning.deck.set")) {
        const cJSON *cards = cJSON_GetObjectItemCaseSensitive(p, "cards");
        const cJSON *plan = cJSON_GetObjectItemCaseSensitive(p, "plan");
        const char *book = get_string(p, "book_id"), *deck = get_string(p, "deck_id");
        int count = cJSON_GetArraySize(cards);
        app_vocabulary_t progress = {0};
        if(!*book || !*deck || strlen(book) > 64 || strlen(deck) > 64 || !cJSON_IsArray(cards) ||
           count > APP_VOCABULARY_CAPACITY || !read_learning_plan(plan, (size_t)count, &progress.plan)) return 0;
        if(!strcmp(book, get_string(w->document, "book_id")) && !strcmp(deck, get_string(w->document, "deck_id"))) {
            snprintf(message, capacity, "学习词卡已同步"); return 1;
        }
        if(!strcmp(book, get_string(w->document, "book_id")) && ui_app_studying_vocabulary(w->ui)) {
            snprintf(message, capacity, "本批词卡正在学习"); return 0;
        }
        for(int i = 0; i < count; ++i) {
            const cJSON *card = cJSON_GetArrayItem(cards, i);
            if(!*get_string(card, "word") || !*get_string(card, "meaning")) return 0;
        }
        cJSON *next = cJSON_Duplicate(w->document, true);
        replace(next, "cards", cJSON_Duplicate(cards, true));
        replace(next, "book_id", cJSON_CreateString(book)); replace(next, "deck_id", cJSON_CreateString(deck));
        cJSON_DeleteItemFromObjectCaseSensitive(next, "plan");
        if(plan) cJSON_AddItemToObject(next, "plan", cJSON_Duplicate(plan, true));
        progress.cycles_completed = model->vocabulary.cycles_completed;
        replace(next, "progress", progress_json(&progress));
        if(!install_document(w, next)) { cJSON_Delete(next); return 0; }
        if(!app_state_set_vocabulary(state, w->cards, (size_t)count, &progress) || !app_runtime_flush(w->runtime)) return 0;
    } else if(!strcmp(name, "learning.events.ack")) {
        const cJSON *ids = cJSON_GetObjectItemCaseSensitive(p, "event_ids");
        if(!cJSON_IsArray(ids) || cJSON_GetArraySize(ids) > 32) return 0;
        cJSON *next = cJSON_Duplicate(w->document, true), *events = cJSON_GetObjectItemCaseSensitive(next, "events");
        for(int i = cJSON_GetArraySize(events) - 1; i >= 0; --i) {
            const char *id = get_string(cJSON_GetArrayItem(events, i), "id");
            for(int j = 0; j < cJSON_GetArraySize(ids); ++j) {
                const cJSON *wanted = cJSON_GetArrayItem(ids, j);
                if(cJSON_IsString(wanted) && !strcmp(id, wanted->valuestring)) { cJSON_DeleteItemFromArray(events, i); break; }
            }
        }
        if(!install_document(w, next)) { cJSON_Delete(next); return 0; }
    } else if(!strcmp(name, "module.motor.set")) {
        const char *direction = get_string(p, "direction");
        unsigned target = !strcmp(direction, "stop") ? 0 : !strcmp(direction, "clockwise") ? 1 : !strcmp(direction, "counterclockwise") ? 2 : 99;
        int speed = number(p, "speed", 0, 100, -1);
        if(target > 2 || speed < 0 || !app_modules_motor_set(modules, target, (unsigned)speed)) {
            snprintf(message, capacity, "转盘未连接或设备正忙"); return 0;
        }
        w->wanted_direction = target; w->wanted_speed = (unsigned)speed;
        w->pending = PENDING_MOTOR; w->pending_request = request; return 2;
    } else if(!strcmp(name, "module.light.set")) {
        const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(p, "enabled");
        static const char *effects[] = {"常亮", "呼吸", "流光", "闪烁", "心跳"};
        const char *effect = get_string(p, "effect"), *color = get_string(p, "color");
        int brightness = number(p, "brightness", 0, 100, -1), speed = number(p, "speed", 0, 100, -1);
        unsigned red, green, blue, index;
        for(index = 0; index < 5; ++index) if(!strcmp(effect, effects[index])) break;
        if(!cJSON_IsBool(enabled) || index >= 5 || strlen(color) != 7 || *color != '#' ||
           strspn(color + 1, "0123456789abcdefABCDEF") != 6 || sscanf(color + 1, "%2x%2x%2x", &red, &green, &blue) != 3 || brightness < 0 || speed < 0) return 0;
        bb_component_light_settings_t target = {(uint8_t)cJSON_IsTrue(enabled), (uint8_t)index,
            (uint8_t)red, (uint8_t)green, (uint8_t)blue, (uint8_t)brightness, (uint8_t)speed, 0};
        if(!app_modules_light_set(modules, &target)) { snprintf(message, capacity, "氛围灯未连接或设备正忙"); return 0; }
        w->wanted_light = target; w->pending = PENDING_LIGHT; w->pending_request = request; return 2;
    } else if(!strcmp(name, "module.pen.set")) {
        const char *mode = get_string(p, "mode"), *face = get_string(p, "face");
        unsigned target_mode = !strcmp(mode, "image") ? 0 : !strcmp(mode, "info") ? 1 : 99;
        unsigned target_face = !strcmp(face, "both") ? 0 : !strcmp(face, "time") ? 1 : !strcmp(face, "weather") ? 2 : 99;
        if(target_mode > 1 || target_face > 2 || !app_modules_pen_set(modules, target_mode, target_face)) { snprintf(message, capacity, "笔筒未连接或设备正忙"); return 0; }
        w->wanted_pen_mode = target_mode; w->wanted_pen_face = target_face;
        w->pending = PENDING_PEN; w->pending_request = request; return 2;
    } else if(!strcmp(name, "module.rotation.set")) {
        int profile = number(p, "profile", 1, 2, -1), seconds = number(p, "seconds", 0, 86400, -1);
        if(profile < 0 || seconds < 0 || !app_modules_random_set(modules, (unsigned)profile, (unsigned)seconds)) return 0;
    } else if(!strcmp(name, "module.display.set")) {
        int profile = number(p, "profile", 1, 2, -1);
        const char *image_id = get_string(p, "image_id");
        if(profile < 0) return 0;
        size_t index;
        for(index = 0; index < app_modules_image_count(modules); ++index)
            if(!strcmp(app_modules_image(modules, index)->id, image_id)) break;
        if(index >= app_modules_image_count(modules) || !app_modules_push(modules, index, (unsigned)profile)) {
            snprintf(message, capacity, "组件未连接、图片不存在或设备正忙"); return 0;
        }
        w->pending = PENDING_DISPLAY; w->pending_request = request; return 2;
    } else if(!strcmp(name, "calendar.set")) {
        const cJSON *events = cJSON_GetObjectItemCaseSensitive(p, "events");
        if(!cJSON_IsArray(events) || cJSON_GetArraySize(events) > BINBOT_AUDIO_CALENDAR_EVENT_CAPACITY) return 0;
        binbot_audio_calendar_state_t calendar = {0};
        const cJSON *revision = cJSON_GetObjectItemCaseSensitive(p, "revision");
        if(!cJSON_IsNumber(revision) || revision->valuedouble < 0 || revision->valuedouble > UINT32_MAX) return 0;
        calendar.revision = (uint32_t)revision->valuedouble; calendar.event_count = (uint8_t)cJSON_GetArraySize(events);
        for(unsigned i = 0; i < calendar.event_count; ++i) {
            const cJSON *event = cJSON_GetArrayItem(events, (int)i);
            const char *id = get_string(event, "id"), *title = get_string(event, "title");
            double start = cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(event, "start")), end = cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(event, "end"));
            if(!*id || strlen(id) >= sizeof(calendar.events[i].event_id) || !*title || strlen(title) >= sizeof(calendar.events[i].summary) || !isfinite(start) || !isfinite(end) || start < 946684800 || end < start || end > 4102444800.0) return 0;
            snprintf(calendar.events[i].event_id, sizeof(calendar.events[i].event_id), "%s", id);
            snprintf(calendar.events[i].summary, sizeof(calendar.events[i].summary), "%s", title);
            snprintf(calendar.events[i].start_timestamp, sizeof(calendar.events[i].start_timestamp), "%.0f", start);
            snprintf(calendar.events[i].end_timestamp, sizeof(calendar.events[i].end_timestamp), "%.0f", end);
            snprintf(calendar.events[i].timezone, sizeof(calendar.events[i].timezone), "%s", get_string(w->document, "timezone"));
            snprintf(calendar.events[i].status, sizeof(calendar.events[i].status), "confirmed");
        }
        if(!ui_app_handle_calendar_state(w->ui, &calendar)) return 0;
    } else if(!strcmp(name, "device.wake")) {
        if(model->mic_muted || !ui_app_request_wake(w->ui)) { snprintf(message, capacity, "麦克风已关闭或音频服务未连接"); return 0; }
    } else if(!strncmp(name, "music.", 6)) {
        static const char *names[] = {"music.play", "music.pause", "music.stop", "music.previous", "music.next", "music.seek", "music.select"};
        unsigned action;
        for(action = 0; action < 7; ++action) if(!strcmp(name, names[action])) break;
        int value = action == 5 ? number(p, "position_seconds", 0, 86400, -1) : action == 6 ? number(p, "track_index", 0, 255, -1) : 0;
        if(action == 7 || value < 0 || !ui_app_apply_remote_music(w->ui, (binbot_audio_remote_action_t)(action + 1), (uint32_t)value)) return 0;
    } else return 0;
    snprintf(message, capacity, "设备已执行并保存");
    return 1;
}

static bool persist_noise(app_workspace_t *w)
{
    cJSON *saved = cJSON_GetObjectItemCaseSensitive(w->document, "noise");
    cJSON *next_mix = cJSON_CreateObject(), *levels = cJSON_AddArrayToObject(next_mix, "levels");
    cJSON_AddNumberToObject(next_mix, "master", w->noise.master);
    cJSON_AddNumberToObject(next_mix, "timer_seconds", w->noise.timer_seconds);
    for(unsigned i = 0; i < 9; ++i) cJSON_AddItemToArray(levels, cJSON_CreateNumber(w->noise.levels[i]));
    if(cJSON_Compare(saved, next_mix, true)) { cJSON_Delete(next_mix); return true; }
    cJSON *next = cJSON_Duplicate(w->document, true);
    replace(next, "noise", next_mix);
    if(!install_document(w, next)) { cJSON_Delete(next); return false; }
    return true;
}

void app_workspace_audio(app_workspace_t *w, const binbot_audio_packet_t *packet)
{
    if(!w || !packet) return;
    if((packet->header.type == BINBOT_AUDIO_MSG_NOISE_STATE || packet->header.type == BINBOT_AUDIO_MSG_NOISE_RESULT) &&
       packet->header.payload_size == sizeof(packet->payload.noise)) {
        w->noise = packet->payload.noise; w->noise.reserved = 0; w->noise_received = true;
        if(w->noise.playing) w->restore_mix = false;
        bool durable = w->restore_mix || persist_noise(w);
        if(packet->header.type == BINBOT_AUDIO_MSG_NOISE_RESULT && w->pending == PENDING_NOISE && packet->header.sequence == w->pending_request) {
            bool success = packet->payload.noise.reserved == 1 && durable;
            if(success) ui_app_handle_noise_state(w->ui, &w->noise);
            bb_control_service_complete(w->control, w->pending_request, success, success ? "白噪音已应用" : "音频服务未能播放白噪音");
            w->pending = PENDING_NONE;
        }
    } else if(packet->header.type == BINBOT_AUDIO_MSG_MICROPHONE_RESULT && packet->header.payload_size == sizeof(packet->payload.volume_result)) {
        bool success = packet->payload.volume_result.success != 0;
        if(success) {
            app_state_set_microphone(app_runtime_state(w->runtime), packet->payload.volume_result.volume != 0);
            success = app_runtime_flush(w->runtime);
        }
        if(w->pending == PENDING_MIC && packet->header.sequence == w->pending_request) {
            bb_control_service_complete(w->control, w->pending_request, success, success ? "麦克风设置已生效" : "麦克风设置或保存失败");
            w->pending = PENDING_NONE;
        }
    }
}

void app_workspace_tick(app_workspace_t *w, uint32_t now)
{
    if(!w) return;
    if(!w->system || now - w->system_at > 10000U) { update_system(w); w->system_at = now; }
    int fd = binbot_audio_client_fd(w->audio);
    if(fd < 0) { w->audio_initialized = false; w->pending = PENDING_NONE; w->noise_received = false;
        w->restore_mix = cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(w->document, "noise")); }
    else if(!w->audio_initialized) {
        binbot_audio_volume_set_t setting = {.volume = app_state_model(app_runtime_state(w->runtime))->mic_muted ? 0 : 1};
        w->audio_initialized = send_audio(w, BINBOT_AUDIO_MSG_MICROPHONE_SET, 0, &setting, sizeof(setting));
    }
    if(fd >= 0 && w->restore_mix && w->noise_received) {
        const cJSON *saved = cJSON_GetObjectItemCaseSensitive(w->document, "noise");
        const cJSON *levels = cJSON_GetObjectItemCaseSensitive(saved, "levels");
        binbot_audio_noise_state_t mix = {0};
        mix.master = (uint8_t)number(saved, "master", 0, 100, 40);
        mix.timer_seconds = (uint32_t)number(saved, "timer_seconds", 0, 10800, 0);
        for(unsigned i = 0; i < 9; ++i) {
            const cJSON *level = cJSON_GetArrayItem(levels, (int)i);
            mix.levels[i] = cJSON_IsNumber(level) && level->valueint >= 0 && level->valueint <= 100 ? (uint8_t)level->valueint : 0;
        }
        if(send_audio(w, BINBOT_AUDIO_MSG_NOISE_CONTROL, 0, &mix, sizeof(mix))) w->restore_mix = false;
    }
    app_modules_t *modules = ui_app_modules(w->ui);
    const binbot_component_event_t *status = NULL;
    bool finished = false, matches = false;
    if(w->pending == PENDING_DISPLAY) {
        status = app_modules_push_status(modules);
        finished = status && status->status >= BINBOT_COMPONENT_SUCCEEDED;
        matches = true;
    } else if(w->pending == PENDING_MOTOR && !app_modules_motor_busy(modules)) {
        finished = true; status = app_modules_motor_status(modules);
        matches = app_modules_motor_direction(modules) == w->wanted_direction && app_modules_motor_speed(modules) == w->wanted_speed;
    } else if(w->pending == PENDING_PEN && !app_modules_pen_busy(modules)) {
        const bb_component_pen_t *pen = app_modules_pen_state(modules);
        finished = true; status = app_modules_pen_status(modules);
        matches = pen && pen->mode == w->wanted_pen_mode && pen->face == w->wanted_pen_face;
    } else if(w->pending == PENDING_LIGHT && !app_modules_light_busy(modules)) {
        const bb_component_light_settings_t *light = app_modules_light_state(modules), *target = &w->wanted_light;
        finished = true; status = app_modules_light_status(modules);
        matches = light && light->enabled == target->enabled && light->effect == target->effect &&
            light->red == target->red && light->green == target->green && light->blue == target->blue &&
            light->brightness == target->brightness && light->speed == target->speed;
    }
    if(finished) {
        bool success = status && status->status == BINBOT_COMPONENT_SUCCEEDED && matches;
        bb_control_service_complete(w->control, w->pending_request, success, success ? "组件已确认设置" : "组件执行失败或已断开，请核对连接");
        w->pending = PENDING_NONE;
    }
}
