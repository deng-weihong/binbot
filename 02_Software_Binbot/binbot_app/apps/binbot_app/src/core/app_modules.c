#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "binbot/app_modules.h"
#include <cjson/cJSON.h>
#include <dirent.h>
#include <errno.h>
#include <openssl/evp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct app_modules {
    char directory[256], staging[384];
    app_module_image_t *images;
    size_t count;
    uint32_t revision, last_probe;
    bb_component_worker_t *worker;
    bool probe_pending, has_info, healthy;
    unsigned profile;
    unsigned connected_profile, missing_probes;
    uint8_t connection_request_id[16];
    uint64_t connection_event_seq;
    bool connection_event_valid;
    app_module_connection_fn connection_observer;
    void *connection_user_data;
    uint8_t probe_id[16], push_id[16];
    char current[2][37], pushing[37];
    unsigned pushing_profile;
    binbot_component_event_t push_status;
    binbot_component_command_t pending_push;
    bool push_pending;
    bool motor_supported, motor_pending, motor_busy, motor_requested;
    unsigned motor_direction, motor_speed;
    uint8_t motor_id[16];
    binbot_component_command_t pending_motor;
    binbot_component_event_t motor_status;
    bool light_supported, light_state_valid, light_busy, light_requested;
    bb_component_light_settings_t light;
    uint16_t light_remaining;
    uint64_t boot_id;
    uint8_t light_id[16];
    binbot_component_event_t light_status;
    bool pen_supported, pen_state_valid, pen_busy, pen_requested;
    bb_component_pen_t pen, weather;
    uint8_t pen_id[16];
    binbot_component_event_t pen_status;
    uint32_t now_ms, last_pen_sync, random_interval[2], random_due[2];
    pid_t weather_pid;
    uint32_t weather_due;
    bool weather_started;
    pid_t download_pid;
    int receipt;
};

enum { MODULE_PROBE_INTERVAL_MS = 10000 };

static void save_display(app_modules_t *m);
static void weather_tick(app_modules_t *m);
static void pen_clock(app_modules_t *m, bb_component_pen_t *state);

static bool uuid_valid(const char *id)
{
    if(!id || strlen(id) != 36) return false;
    for(size_t i = 0; i < 36; ++i) {
        if(i == 8 || i == 13 || i == 18 || i == 23) { if(id[i] != '-') return false; }
        else if(!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) return false;
    }
    return true;
}

static bool mkdir_parents(const char *directory)
{
    char path[384];
    if(strlen(directory) >= sizeof(path)) return false;
    snprintf(path, sizeof(path), "%s", directory);
    for(char *p = path + 1; *p; ++p) {
        if(*p != '/') continue;
        *p = '\0';
        if(mkdir(path, 0700) != 0 && errno != EEXIST) return false;
        *p = '/';
    }
    return mkdir(path, 0700) == 0 || errno == EEXIST;
}

static cJSON *read_json(const char *path)
{
    FILE *file = fopen(path, "rb");
    if(!file) return NULL;
    char bytes[8193];
    size_t count = fread(bytes, 1, sizeof(bytes), file);
    bool ok = !ferror(file) && count < sizeof(bytes);
    fclose(file);
    if(!ok) return NULL;
    bytes[count] = 0;
    return cJSON_ParseWithLength(bytes, count + 1);
}

static bool digest_valid(const char *value)
{
    if(!value || strlen(value) != 64) return false;
    for(const char *p = value; *p; ++p)
        if(!((*p >= 'a' && *p <= 'f') || (*p >= '0' && *p <= '9'))) return false;
    return true;
}

static bool file_matches(const char *path, uint32_t size, const char *expected)
{
    struct stat info;
    if(stat(path, &info) || info.st_size != (off_t)size) return false;
    FILE *file = fopen(path, "rb");
    if(!file) return false;
    EVP_MD_CTX *hash = EVP_MD_CTX_new();
    bool ok = hash && EVP_DigestInit_ex(hash, EVP_sha256(), NULL) == 1;
    unsigned char buffer[4096], digest[32];
    unsigned int count = 0;
    size_t n;
    while(ok && (n = fread(buffer, 1, sizeof(buffer), file)) > 0)
        ok = EVP_DigestUpdate(hash, buffer, n) == 1;
    ok = ok && !ferror(file) && EVP_DigestFinal_ex(hash, digest, &count) == 1 && count == 32;
    if(hash) EVP_MD_CTX_free(hash);
    fclose(file);
    char actual[65];
    if(ok) for(size_t i = 0; i < 32; ++i) snprintf(actual + i * 2, 3, "%02x", digest[i]);
    return ok && strcmp(actual, expected) == 0;
}

static bool parse_image(const char *directory, app_module_image_t *image, bool verify)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/manifest.json", directory);
    cJSON *json = read_json(path);
    if(!json) return false;
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "id"));
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "name"));
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(json, "schema");
    bool ok = uuid_valid(id) && name && name[0] && strlen(name) < sizeof(image->name) &&
              cJSON_IsNumber(schema) && schema->valuedouble == 1;
    memset(image, 0, sizeof(*image));
    if(ok) {
        snprintf(image->id, sizeof(image->id), "%s", id);
        snprintf(image->name, sizeof(image->name), "%s", name);
    }
    const char *keys[] = {"round", "holo", "thumbnail"};
    for(size_t i = 0; ok && i < 3; ++i) {
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, keys[i]);
        const cJSON *size = cJSON_GetObjectItemCaseSensitive(item, "size");
        const char *sha = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "sha256"));
        uint32_t limit = i == 2 ? 256 * 1024 : 512 * 1024;
        ok = cJSON_IsNumber(size) && size->valuedouble >= 10 && size->valuedouble <= limit &&
             size->valuedouble == (uint32_t)size->valuedouble && digest_valid(sha);
        if(!ok) break;
        snprintf(path, sizeof(path), "%s/%s.%s", directory, keys[i], i == 2 ? "png" : "gif");
        if(verify) ok = file_matches(path, (uint32_t)size->valuedouble, sha);
        else {
            struct stat info;
            ok = stat(path, &info) == 0 && info.st_size == (off_t)size->valuedouble;
        }
        if(i < 2) {
            const cJSON *w = cJSON_GetObjectItemCaseSensitive(item, "width");
            const cJSON *h = cJSON_GetObjectItemCaseSensitive(item, "height");
            unsigned width = i == 0 ? 240 : 320;
            bool legacy = !verify && i == 1 && cJSON_IsNumber(w) && cJSON_IsNumber(h) &&
                          w->valuedouble == 132 && h->valuedouble == 132;
            ok = ok && cJSON_IsNumber(w) && cJSON_IsNumber(h) &&
                 ((w->valuedouble == width && h->valuedouble == 240) || legacy);
            image->sizes[i] = (uint32_t)size->valuedouble;
            snprintf(image->sha256[i], sizeof(image->sha256[i]), "%s", sha);
            snprintf(image->files[i], sizeof(image->files[i]), "%s", path);
        } else snprintf(image->thumbnail, sizeof(image->thumbnail), "%s", path);
    }
    cJSON_Delete(json);
    return ok;
}

static void scan_images(app_modules_t *m)
{
    DIR *dir = opendir(m->directory);
    if(!dir) return;
    app_module_image_t *images = NULL;
    size_t count = 0;
    struct dirent *entry;
    while((entry = readdir(dir))) {
        if(!uuid_valid(entry->d_name)) continue;
        char path[384];
        snprintf(path, sizeof(path), "%s/%.36s", m->directory, entry->d_name);
        app_module_image_t image;
        if(!parse_image(path, &image, false) || strcmp(image.id, entry->d_name)) continue;
        app_module_image_t *next = realloc(images, (count + 1) * sizeof(*images));
        if(!next) break;
        images = next;
        images[count++] = image;
    }
    closedir(dir);
    free(m->images);
    m->images = images;
    m->count = count;
    m->revision++;
}

static void remove_staging(const char *directory)
{
    const char *files[] = {"manifest.json", "thumbnail.png", "round.gif", "holo.gif"};
    for(size_t i = 0; i < 4; ++i) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", directory, files[i]);
        unlink(path);
    }
    rmdir(directory);
}

static bool fetch_file(const char *url, const char *path, off_t limit)
{
    pid_t child = fork();
    if(child < 0) return false;
    if(child == 0) {
        execlp("wget", "wget", "-q", "-T", "15", "-O", path, url, (char *)NULL);
        _exit(127);
    }
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    time_t deadline = now.tv_sec + 30;
    int status = 0;
    for(;;) {
        pid_t waited = waitpid(child, &status, WNOHANG);
        if(waited == child) return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if(waited < 0 && errno != EINTR) return false;
        struct stat info;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if(now.tv_sec >= deadline || (!stat(path, &info) && info.st_size > limit)) break;
        struct timespec pause = {0, 50000000};
        nanosleep(&pause, NULL);
    }
    kill(child, SIGTERM);
    while(waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    return false;
}

app_modules_t *app_modules_create(const char *directory)
{
    app_modules_t *m = calloc(1, sizeof(*m));
    if(!m) return NULL;
    if(!directory || !directory[0]) directory = getenv("BINBOT_MODULE_IMAGE_ROOT");
    if(!directory || !directory[0]) {
#ifdef BINBOT_HOST_SIMULATOR
        directory = "/tmp/binbot-module-images";
#else
        directory = "/oem/data/module-images";
#endif
    }
    if(strlen(directory) >= sizeof(m->directory) || !mkdir_parents(directory)) { free(m); return NULL; }
    snprintf(m->directory, sizeof(m->directory), "%s", directory);
    scan_images(m);
    char path[384];
    snprintf(path, sizeof(path), "%s/display.json", directory);
    cJSON *json = read_json(path);
    for(size_t i = 0; i < 2; ++i) {
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, i ? "holo" : "round"));
        if(uuid_valid(id)) snprintf(m->current[i], sizeof(m->current[i]), "%s", id);
        const cJSON *iv = cJSON_GetObjectItemCaseSensitive(json, i ? "holo_random" : "round_random");
        if(cJSON_IsNumber(iv) && (iv->valueint == 10 || iv->valueint == 60 || iv->valueint == 300 || iv->valueint == 1800))
            m->random_interval[i] = iv->valueint;
    }
    cJSON_Delete(json);
    return m;
}

void app_modules_destroy(app_modules_t *m)
{
    if(!m) return;
    if(m->download_pid > 0) {
        if(kill(-m->download_pid, SIGTERM)) kill(m->download_pid, SIGTERM);
        while(waitpid(m->download_pid, NULL, 0) < 0 && errno == EINTR) {}
        remove_staging(m->staging);
    }
    if(m->weather_pid > 0) {
        if(kill(-m->weather_pid, SIGTERM)) kill(m->weather_pid, SIGTERM);
        while(waitpid(m->weather_pid, NULL, 0) < 0 && errno == EINTR) {}
    }
    free(m->images);
    free(m);
}

void app_modules_set_worker(app_modules_t *m, bb_component_worker_t *worker)
{
    if(!m) return;
    m->worker = worker;
    m->probe_pending = worker != NULL;
}
void app_modules_set_connection_observer(app_modules_t *m,
                                         app_module_connection_fn observer, void *user_data)
{
    if(!m) return;
    m->connection_observer = observer;
    m->connection_user_data = user_data;
}
size_t app_modules_image_count(const app_modules_t *m) { return m ? m->count : 0; }
const app_module_image_t *app_modules_image(const app_modules_t *m, size_t i) { return m && i < m->count ? &m->images[i] : NULL; }
const char *app_modules_current(const app_modules_t *m, unsigned profile) { return m && profile >= 1 && profile <= 2 ? m->current[profile - 1] : ""; }
uint32_t app_modules_revision(const app_modules_t *m) { return m ? m->revision : 0; }
unsigned app_modules_profile(const app_modules_t *m) { return m && m->has_info ? m->profile : 0; }
bool app_modules_healthy(const app_modules_t *m) { return m && m->has_info && m->healthy; }
void app_modules_request_probe(app_modules_t *m) { if(m) m->probe_pending = true; }
bool app_modules_pen_supported(const app_modules_t *m) { return app_modules_healthy(m) && m->profile == 1 && m->pen_supported && m->pen_state_valid; }
bool app_modules_pen_busy(const app_modules_t *m) { return m && m->pen_busy; }
const bb_component_pen_t *app_modules_pen_state(const app_modules_t *m) { return m ? &m->pen : NULL; }
const binbot_component_event_t *app_modules_pen_status(const app_modules_t *m) { return m && m->pen_requested ? &m->pen_status : NULL; }
unsigned app_modules_random_interval(const app_modules_t *m, unsigned p) { return m && p >= 1 && p <= 2 ? m->random_interval[p-1] : 0; }
unsigned app_modules_random_remaining(const app_modules_t *m, unsigned p)
{
    if(!app_modules_random_interval(m, p)) return 0;
    int32_t left = (int32_t)(m->random_due[p-1] - m->now_ms);
    return left > 0 ? ((uint32_t)left + 999) / 1000 : 0;
}
bool app_modules_random_set(app_modules_t *m, unsigned p, unsigned seconds)
{
    if(!m || p < 1 || p > 2 || (seconds && seconds != 10 && seconds != 60 && seconds != 300 && seconds != 1800)) return false;
    m->random_interval[p-1] = seconds;
    m->random_due[p-1] = m->now_ms + seconds*1000;
    save_display(m); m->revision++;
    return true;
}
bool app_modules_motor_supported(const app_modules_t *m) { return app_modules_healthy(m) && m->profile == 3 && m->motor_supported; }
bool app_modules_motor_busy(const app_modules_t *m) { return m && m->motor_busy; }
unsigned app_modules_motor_direction(const app_modules_t *m) { return m ? m->motor_direction : 0; }
unsigned app_modules_motor_speed(const app_modules_t *m) { return m ? m->motor_speed : 0; }
const binbot_component_event_t *app_modules_motor_status(const app_modules_t *m) { return m && m->motor_requested ? &m->motor_status : NULL; }
bool app_modules_light_supported(const app_modules_t *m) { return app_modules_healthy(m) && m->profile == BB_COMPONENT_AMBIENT_LIGHT && m->light_supported && m->light_state_valid; }
bool app_modules_light_busy(const app_modules_t *m) { return m && m->light_busy; }
const bb_component_light_settings_t *app_modules_light_state(const app_modules_t *m) { return m && m->light_state_valid ? &m->light : NULL; }
unsigned app_modules_light_remaining(const app_modules_t *m) { return m ? m->light_remaining : 0; }
const binbot_component_event_t *app_modules_light_status(const app_modules_t *m) { return m && m->light_requested ? &m->light_status : NULL; }

static bool request_id(uint8_t id[16])
{
    FILE *file = fopen("/dev/urandom", "rb");
    if(!file) return false;
    bool ok = fread(id, 1, 16, file) == 16;
    fclose(file);
    return ok;
}

void app_modules_tick(app_modules_t *m, uint32_t now_ms)
{
    if(!m) return;
    m->now_ms = now_ms;
    weather_tick(m);
    if(!m->worker || m->worker->pid > 0 || m->worker->command_pending) return;
    if(m->motor_pending) {
        m->motor_pending = false;
        if(!bb_component_worker_submit(m->worker, &m->pending_motor)) {
            m->motor_busy = false;
            m->motor_status.status = BINBOT_COMPONENT_FAILED;
            snprintf(m->motor_status.message, sizeof(m->motor_status.message), "Could not start motor control");
            m->revision++;
        }
        return;
    }
    if(m->push_pending) {
        m->push_pending = false;
        if(!bb_component_worker_submit(m->worker, &m->pending_push)) {
            m->push_status.status = BINBOT_COMPONENT_FAILED;
            snprintf(m->push_status.message, sizeof(m->push_status.message), "Could not start component update");
            m->revision++;
        }
        return;
    }
    if(app_modules_pen_supported(m) && !m->pen_busy &&
       (!m->last_pen_sync || (uint32_t)(now_ms - m->last_pen_sync) >= 60000)) {
        if(app_modules_pen_set(m, m->pen.mode, m->pen.face)) return;
    }
    unsigned p = app_modules_profile(m);
    if(app_modules_healthy(m) && p >= 1 && p <= 2 && m->random_interval[p-1] &&
       !(p == 1 && m->pen.mode == BB_PEN_INFO)) {
        if(!m->random_due[p-1]) m->random_due[p-1] = now_ms + m->random_interval[p-1]*1000;
        if((int32_t)(now_ms - m->random_due[p-1]) >= 0) {
            size_t choices = 0;
            for(size_t i=0;i<m->count;++i) if(strcmp(m->images[i].id, m->current[p-1])) ++choices;
            uint8_t entropy[16];
            m->random_due[p-1] = now_ms + m->random_interval[p-1]*1000;
            if(choices && request_id(entropy)) {
                size_t next = bb_component_get_u32(entropy) % choices;
                for(size_t i=0;i<m->count;++i) if(strcmp(m->images[i].id, m->current[p-1])) {
                    if(!next--) { if(app_modules_push(m, i, p)) return; break; }
                }
            }
        }
    }
    /* Plug-in announcements trigger an immediate probe.  Keep the periodic
     * fallback at 10 s so an absent component does not continuously spawn a
     * UART worker and compete with the UI on RV1106. */
    if((uint32_t)(now_ms - m->last_probe) >= MODULE_PROBE_INTERVAL_MS)
        m->probe_pending = true;
    if(!m->probe_pending ||
       (uint32_t)(now_ms - m->last_probe) < 250) return;
    binbot_component_command_t command = {.action = BINBOT_COMPONENT_PROBE};
    if(!request_id(command.request_id)) return;
    if(bb_component_worker_submit(m->worker, &command)) {
        memcpy(m->probe_id, command.request_id, 16);
        m->probe_pending = false;
        m->last_probe = now_ms;
    }
}

static void save_display(app_modules_t *m)
{
    char path[384], temporary[384];
    snprintf(path, sizeof(path), "%s/display.json", m->directory);
    snprintf(temporary, sizeof(temporary), "%s/.display.tmp", m->directory);
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "round", m->current[0]);
    cJSON_AddStringToObject(json, "holo", m->current[1]);
    cJSON_AddNumberToObject(json, "round_random", m->random_interval[0]);
    cJSON_AddNumberToObject(json, "holo_random", m->random_interval[1]);
    char *text = cJSON_PrintUnformatted(json);
    if(text) {
        FILE *file = fopen(temporary, "wb");
        if(file) {
            bool ok = fwrite(text, 1, strlen(text), file) == strlen(text);
            if(fflush(file) != 0 || fsync(fileno(file)) != 0) ok = false;
            if(fclose(file) != 0) ok = false;
            if(ok) rename(temporary, path);
            else unlink(temporary);
        }
        cJSON_free(text);
    }
    cJSON_Delete(json);
}

bool app_modules_set_current(app_modules_t *m, const char *id, unsigned profile)
{
    if(!m || !uuid_valid(id) || profile < 1 || profile > 2) return false;
    for(size_t i = 0; i < m->count; ++i) {
        if(strcmp(m->images[i].id, id)) continue;
        m->random_due[profile-1] = m->now_ms + m->random_interval[profile-1]*1000;
        if(profile == 1 && m->pen.mode != BB_PEN_IMAGE) {
            m->pen.mode = BB_PEN_IMAGE; m->revision++;
        }
        if(strcmp(m->current[profile - 1], id)) {
            snprintf(m->current[profile - 1], sizeof(m->current[0]), "%s", id);
            save_display(m);
            m->revision++;
        }
        return true;
    }
    return false;
}

static void connection_changed(app_modules_t *m, const binbot_component_event_t *event)
{
    unsigned profile = event->has_info ? event->profile : 0;
    if(!profile && event->status != BINBOT_COMPONENT_PROBED) return;
    /* The worker republishes its latest result until another job starts. */
    if(event->event_seq && m->connection_event_valid &&
       !memcmp(m->connection_request_id, event->request_id, 16) &&
       event->event_seq <= m->connection_event_seq) return;
    memcpy(m->connection_request_id, event->request_id, 16);
    m->connection_event_seq = event->event_seq;
    m->connection_event_valid = event->event_seq != 0;
    if(!profile) {
        if(m->missing_probes < 2) ++m->missing_probes;
        if(m->missing_probes < 2) return;
    }
    if(profile) m->missing_probes = 0;
    if(profile == m->connected_profile) return;
    unsigned previous = m->connected_profile;
    m->connected_profile = profile;
    if(m->connection_observer) {
        if(previous) m->connection_observer(previous, false, m->connection_user_data);
        if(profile) m->connection_observer(profile, true, m->connection_user_data);
    }
}

static void light_info(app_modules_t *m, const binbot_component_event_t *event)
{
    cJSON *json = cJSON_ParseWithLength(event->message, sizeof(event->message));
    const cJSON *light = cJSON_GetObjectItemCaseSensitive(json, "light");
    const cJSON *supported = cJSON_GetObjectItemCaseSensitive(light, "supported");
    if(cJSON_IsBool(supported)) {
        bool next = cJSON_IsTrue(supported);
        if(next != m->light_supported) { m->light_supported = next; m->revision++; }
    }
    const char *keys[] = {"on", "fx", "r", "g", "b", "v", "sp", "off", "left"};
    unsigned values[9];
    bool valid = cJSON_IsObject(light);
    for(unsigned i = 0; i < 9 && valid; ++i) {
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(light, keys[i]);
        valid = cJSON_IsNumber(value) && value->valuedouble >= 0 && value->valuedouble <= 255 &&
                value->valuedouble == value->valueint;
        if(valid) values[i] = (unsigned)value->valueint;
    }
    if(valid) {
        bb_component_light_settings_t next = {values[0], values[1], values[2], values[3], values[4], values[5], values[6], values[7]};
        if(bb_component_light_valid(&next) && values[8] <= next.timer_minutes && (!values[8] || next.enabled)) {
            uint8_t before[BB_COMPONENT_LIGHT_BYTES], after[BB_COMPONENT_LIGHT_BYTES];
            bb_component_light_encode(&m->light, before); bb_component_light_encode(&next, after);
            if(!m->light_state_valid || memcmp(before, after, sizeof(before)) || m->light_remaining != values[8])
                m->revision++;
            m->light = next; m->light_state_valid = true; m->light_remaining = values[8];
        }
    }
    cJSON_Delete(json);
}

static void pen_info(app_modules_t *m, const binbot_component_event_t *event)
{
    cJSON *json = cJSON_ParseWithLength(event->message, sizeof(event->message));
    const cJSON *pen = cJSON_GetObjectItemCaseSensitive(json, "pen");
    const cJSON *supported = cJSON_GetObjectItemCaseSensitive(pen, "supported");
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(pen, "mode");
    const cJSON *face = cJSON_GetObjectItemCaseSensitive(pen, "face");
    if(cJSON_IsBool(supported)) {
        bool next = cJSON_IsTrue(supported);
        if(m->pen_supported != next) { m->pen_supported = next; m->revision++; }
        if(next && cJSON_IsNumber(mode) && cJSON_IsNumber(face) &&
           (mode->valuedouble == 0 || mode->valuedouble == 1) &&
           (face->valuedouble == 0 || face->valuedouble == 1 || face->valuedouble == 2)) {
            if(!m->pen_state_valid || m->pen.mode != mode->valueint || m->pen.face != face->valueint) {
                m->pen.mode = mode->valueint; m->pen.face = face->valueint;
                m->random_due[0] = m->now_ms + m->random_interval[0]*1000;
                m->revision++;
            }
            m->pen_state_valid = true;
            uint32_t minute = m->pen.unix_time/60;
            uint8_t flags = m->pen.flags;
            pen_clock(m, &m->pen);
            if(minute != m->pen.unix_time/60 || flags != m->pen.flags) m->revision++;
        }
    }
    cJSON_Delete(json);
}

bool app_modules_handle_event(app_modules_t *m, const binbot_component_event_t *event)
{
    if(!m || !event) return false;
    if(event->profile == 4) return false;
    connection_changed(m, event);
    bool local = !memcmp(m->probe_id, event->request_id, 16) || !memcmp(m->push_id, event->request_id, 16) ||
                 (m->motor_requested && !memcmp(m->motor_id, event->request_id, 16)) ||
                 (m->pen_requested && !memcmp(m->pen_id, event->request_id, 16)) ||
                 (m->light_requested && !memcmp(m->light_id, event->request_id, 16));
    if(event->has_info || event->status == BINBOT_COMPONENT_PROBED) {
        bool changed = m->has_info != (event->has_info != 0) ||
                       m->healthy != (event->healthy != 0) || m->profile != event->profile ||
                       m->boot_id != event->boot_id ||
                       m->motor_supported != (event->motor_supported != 0) ||
                       m->motor_direction != event->motor_direction || m->motor_speed != event->motor_speed;
        if(!event->has_info || event->profile != m->profile || event->boot_id != m->boot_id) {
            m->light_supported = m->light_state_valid = false;
            m->pen_supported = m->pen_state_valid = false;
            m->last_pen_sync = 0;
        }
        m->boot_id = event->boot_id;
        m->has_info = event->has_info != 0;
        m->healthy = event->healthy != 0;
        m->profile = event->profile;
        m->motor_supported = event->motor_supported != 0;
        m->motor_direction = event->motor_direction;
        m->motor_speed = event->motor_speed;
        if(changed) m->revision++;
        if(event->has_info && event->profile == BB_COMPONENT_AMBIENT_LIGHT) light_info(m, event);
        if(event->has_info && event->profile == BB_COMPONENT_ROUND_SCREEN) pen_info(m, event);
    }
    if(m->pen_requested && !memcmp(m->pen_id, event->request_id, 16)) {
        bool changed = memcmp(&m->pen_status, event, sizeof(*event)) != 0;
        m->pen_status = *event;
        if(event->status >= BINBOT_COMPONENT_SUCCEEDED) m->pen_busy = false;
        if(event->status == BINBOT_COMPONENT_FAILED) m->probe_pending = true;
        if(changed) m->revision++;
    }
    if(m->light_requested && !memcmp(m->light_id, event->request_id, 16)) {
        bool changed = memcmp(&m->light_status, event, sizeof(*event)) != 0;
        m->light_status = *event;
        if(event->status >= BINBOT_COMPONENT_SUCCEEDED) m->light_busy = false;
        if(event->status == BINBOT_COMPONENT_FAILED) m->probe_pending = true;
        if(changed) m->revision++;
    }
    if(m->motor_requested && !memcmp(m->motor_id, event->request_id, 16)) {
        bool changed = memcmp(&m->motor_status, event, sizeof(*event)) != 0;
        m->motor_status = *event;
        if(event->status >= BINBOT_COMPONENT_SUCCEEDED) m->motor_busy = false;
        if(event->status == BINBOT_COMPONENT_FAILED) m->probe_pending = true;
        if(changed) m->revision++;
    }
    if(m->pushing_profile && !memcmp(m->push_id, event->request_id, 16)) {
        bool first_success = event->status == BINBOT_COMPONENT_SUCCEEDED &&
                             m->push_status.status != BINBOT_COMPONENT_SUCCEEDED;
        bool changed = memcmp(&m->push_status, event, sizeof(*event)) != 0;
        m->push_status = *event;
        if(first_success) {
            snprintf(m->current[m->pushing_profile - 1], sizeof(m->current[0]), "%s", m->pushing);
            save_display(m);
            m->random_due[m->pushing_profile-1] = m->now_ms + m->random_interval[m->pushing_profile-1]*1000;
            if(m->pushing_profile == 1) m->pen.mode = BB_PEN_IMAGE;
        }
        if(changed) m->revision++;
    }
    return local;
}

static bool weather_number(const cJSON *object, const char *key, double *value)
{
    const cJSON *number = cJSON_GetObjectItemCaseSensitive(object, key);
    if(!cJSON_IsNumber(number) || number->valuedouble != number->valuedouble) return false;
    *value = number->valuedouble;
    return true;
}

static bool weather_condition(unsigned code, bool night, unsigned *condition)
{
    switch(code) {
        case 0: *condition = night ? BB_PEN_NIGHT : BB_PEN_SUN; break;
        case 1: case 2: *condition = night ? BB_PEN_CLOUD_NIGHT : BB_PEN_CLOUD; break;
        case 3: *condition = BB_PEN_OVERCAST; break;
        case 45: case 48: *condition = BB_PEN_FOG; break;
        case 51: case 53: case 55: case 56: case 57:
        case 61: case 63: case 66: *condition = BB_PEN_RAIN; break;
        case 65: case 67: *condition = BB_PEN_HEAVY_RAIN; break;
        case 71: case 73: case 77: case 85: *condition = BB_PEN_SNOW; break;
        case 75: case 86: *condition = BB_PEN_HEAVY_SNOW; break;
        case 80: case 81: *condition = BB_PEN_SHOWER; break;
        case 82: *condition = BB_PEN_DOWNPOUR; break;
        case 95: *condition = BB_PEN_STORM; break;
        case 96: case 99: *condition = BB_PEN_HAIL; break;
        default: return false;
    }
    return true;
}

static bool weather_download(app_modules_t *m, const char *city, const char *latitude, const char *longitude)
{
    char path[384], url[1024];
    snprintf(path, sizeof(path), "%s/.weather.json", m->directory);
    char location[400];
    if(latitude && longitude) {
        char *end_lat, *end_lon;
        double lat = strtod(latitude, &end_lat), lon = strtod(longitude, &end_lon);
        if(!*latitude || !*longitude || *end_lat || *end_lon ||
           !(lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180)) return false;
        snprintf(location, sizeof(location), "latitude=%.5f&longitude=%.5f", lat, lon);
    } else if(city && *city && strlen(city) <= 120) {
        char encoded[361], *next = encoded;
        for(const unsigned char *p=(const unsigned char *)city; *p; ++p) {
            snprintf(next, 4, "%%%02X", *p); next += 3;
        }
        *next = 0;
        snprintf(location, sizeof(location), "city=%s", encoded);
    } else return false;
    const char *base = getenv("BINBOT_WEATHER_BASE_URL");
    if(!base || !*base) base = getenv("BINBOT_DIALOG_URL");
    if(!base || !*base) base = "http://127.0.0.1:8080";
    const char *authority, *scheme;
    if(!strncmp(base,"http://",7)) { authority=base+7; scheme="http://"; }
    else if(!strncmp(base,"https://",8)) { authority=base+8; scheme="https://"; }
    else if(!strncmp(base,"ws://",5)) { authority=base+5; scheme="http://"; }
    else if(!strncmp(base,"wss://",6)) { authority=base+6; scheme="https://"; }
    else return false;
    size_t length = strcspn(authority,"/?#");
    if(!length || length > 250 || memchr(authority,'@',length)) return false;
    snprintf(url, sizeof(url), "%s%.*s/v1/device/weather?%s", scheme, (int)length, authority, location);
    if(!fetch_file(url, path, 8192)) return false;
    cJSON *json = read_json(path);
    const cJSON *current = cJSON_GetObjectItemCaseSensitive(json, "current");
    const cJSON *daily = cJSON_GetObjectItemCaseSensitive(json, "daily");
    const cJSON *low = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_min"), 0);
    const cJSON *high = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_max"), 0);
    double temp, code, observed;
    bool ok = weather_number(current, "temperature_2m", &temp) && weather_number(current, "weather_code", &code) &&
        weather_number(current, "time", &observed) && cJSON_IsNumber(low) && cJSON_IsNumber(high) &&
        temp >= -100 && temp <= 70 && low->valuedouble >= -100 && high->valuedouble <= 70 &&
        low->valuedouble <= high->valuedouble && observed >= 1577836800 && observed <= (double)time(NULL);
    if(ok) {
        const cJSON *day = cJSON_GetObjectItemCaseSensitive(current, "is_day");
        unsigned condition;
        if(code < 0 || code > 99 || code != (unsigned)code ||
           !weather_condition((unsigned)code, cJSON_IsNumber(day) && day->valuedouble == 0, &condition)) {
            cJSON_Delete(json);
            return false;
        }
        bb_component_pen_t weather = {.flags=3, .unix_time=(uint32_t)time(NULL), .weather_time=(uint32_t)observed,
            .temperature=(int16_t)(temp*10), .low=(int16_t)(low->valuedouble*10),
            .high=(int16_t)(high->valuedouble*10), .condition=condition};
        uint8_t bytes[BB_COMPONENT_PEN_BYTES]; bb_component_pen_encode(&weather, bytes);
        snprintf(path, sizeof(path), "%s/.weather.bin", m->directory);
        FILE *file = fopen(path, "wb");
        ok = file && fwrite(bytes, 1, sizeof(bytes), file) == sizeof(bytes);
        if(file && fclose(file)) ok = false;
    }
    cJSON_Delete(json);
    return ok;
}

static void weather_tick(app_modules_t *m)
{
    if(m->weather_pid > 0) {
        int status;
        pid_t done = waitpid(m->weather_pid, &status, WNOHANG);
        if(done > 0 || (done < 0 && errno != EINTR)) {
            m->weather_pid = 0;
            bool ok = done > 0 && WIFEXITED(status) && !WEXITSTATUS(status);
            m->weather_due = m->now_ms + (ok ? 1800000 : 60000);
            if(ok) {
                char path[384]; snprintf(path, sizeof(path), "%s/.weather.bin", m->directory);
                FILE *file = fopen(path, "rb"); uint8_t bytes[BB_COMPONENT_PEN_BYTES];
                if(file) {
                    if(fread(bytes, 1, sizeof(bytes), file) == sizeof(bytes) &&
                       bb_component_pen_decode(bytes, sizeof(bytes), &m->weather)) {
                        m->last_pen_sync = 0; m->revision++;
                    }
                    fclose(file);
                }
            }
        }
    }
    unsigned minute = m->pen.unix_time/60, flags = m->pen.flags;
    pen_clock(m, &m->pen);
    if(minute != m->pen.unix_time/60 || flags != m->pen.flags) m->revision++;
    if(m->weather_pid > 0 || (m->weather_started && (int32_t)(m->now_ms - m->weather_due) < 0)) return;
    const char *city = getenv("BINBOT_WEATHER_CITY");
    const char *latitude = getenv("BINBOT_WEATHER_LATITUDE"), *longitude = getenv("BINBOT_WEATHER_LONGITUDE");
    if((!city || !*city) && (!latitude || !longitude)) return;
    pid_t child = fork();
    if(child < 0) return;
    if(!child) { setpgid(0, 0); _exit(weather_download(m, city, latitude, longitude) ? 0 : 1); }
    setpgid(child, child);
    m->weather_pid = child; m->weather_started = true;
}

bool app_modules_weather(const app_modules_t *m, bb_component_pen_t *weather)
{
    if(!m || !weather) return false;
    *weather = m->weather;
    time_t now = time(NULL);
    if(now < 1577836800 || (uint64_t)now > UINT32_MAX) return false;
    weather->unix_time = (uint32_t)now;
    bb_component_pen_advance(weather, 0);
    return (weather->flags & BB_PEN_WEATHER_VALID) != 0;
}

static void pen_weather(app_modules_t *m, bb_component_pen_t *state)
{
    bb_component_pen_t weather;
    if(app_modules_weather(m, &weather)) {
        state->flags |= BB_PEN_WEATHER_VALID;
        state->temperature = weather.temperature; state->low = weather.low; state->high = weather.high;
        state->condition = weather.condition;
        state->weather_time = weather.weather_time;
    }
}

static void pen_clock(app_modules_t *m, bb_component_pen_t *state)
{
    time_t now = time(NULL);
    struct tm local;
    state->flags = 0;
    if(now < 1577836800 || (uint64_t)now > UINT32_MAX || !localtime_r(&now, &local)) return;
    state->unix_time = (uint32_t)now;
    state->utc_offset = local.tm_gmtoff;
    state->flags = BB_PEN_TIME_VALID;
    pen_weather(m, state);
}

bool app_modules_pen_set(app_modules_t *m, unsigned mode, unsigned face)
{
    if(!m || !m->worker || !app_modules_pen_supported(m) || m->pen_busy || mode > 1 || face > 2 ||
       m->push_pending || m->worker->command_pending ||
       (m->worker->pid > 0 && m->worker->active_action != BINBOT_COMPONENT_PROBE)) return false;
    bb_component_pen_t state = m->pen;
    state.mode = mode; state.face = face;
    pen_clock(m, &state);
    binbot_component_command_t command = {.action=BINBOT_COMPONENT_PEN_CONTROL, .before_boot_id=m->boot_id};
    command.reserved[0] = BB_COMPONENT_ROUND_SCREEN;
    bb_component_pen_encode(&state, (uint8_t *)command.url);
    if(!request_id(command.request_id) || !bb_component_worker_submit(m->worker, &command)) return false;
    memcpy(m->pen_id, command.request_id, 16);
    m->pen_requested = m->pen_busy = true;
    m->last_pen_sync = m->now_ms ? m->now_ms : 1;
    memset(&m->pen_status, 0, sizeof(m->pen_status));
    m->pen_status.status = BINBOT_COMPONENT_PREPARING;
    m->revision++;
    return true;
}

bool app_modules_light_set(app_modules_t *m, const bb_component_light_settings_t *settings)
{
    if(!m || !m->worker || !app_modules_light_supported(m) || m->light_busy ||
       !bb_component_light_valid(settings)) return false;
    if(m->worker->pid > 0 && m->worker->active_action != BINBOT_COMPONENT_PROBE) return false;
    binbot_component_command_t command = {.action = BINBOT_COMPONENT_LIGHT_CONTROL,
                                           .before_boot_id = m->boot_id};
    command.reserved[0] = BB_COMPONENT_AMBIENT_LIGHT;
    bb_component_light_encode(settings, (uint8_t *)command.file_name);
    if(!request_id(command.request_id) || !bb_component_worker_submit(m->worker, &command)) return false;
    memcpy(m->light_id, command.request_id, 16);
    m->light_requested = m->light_busy = true;
    memset(&m->light_status, 0, sizeof(m->light_status));
    m->light_status.status = BINBOT_COMPONENT_PREPARING;
    m->revision++;
    return true;
}

bool app_modules_motor_set(app_modules_t *m, unsigned direction, unsigned speed)
{
    if(!m || !m->worker || !app_modules_motor_supported(m) || m->motor_busy ||
       m->push_pending || direction > 2 || speed > 100) return false;
    bool probing = m->worker->pid > 0 && m->worker->active_action == BINBOT_COMPONENT_PROBE;
    if(m->worker->pid > 0 && !probing) return false;
    binbot_component_command_t command = {.action = BINBOT_COMPONENT_MOTOR_CONTROL};
    if(!request_id(command.request_id)) return false;
    command.reserved[0] = 3;
    command.reserved[1] = (uint8_t)direction;
    command.reserved[2] = (uint8_t)speed;
    if(probing) { m->pending_motor = command; m->motor_pending = true; }
    else if(!bb_component_worker_submit(m->worker, &command)) return false;
    memcpy(m->motor_id, command.request_id, 16);
    m->motor_requested = m->motor_busy = true;
    memset(&m->motor_status, 0, sizeof(m->motor_status));
    m->motor_status.status = BINBOT_COMPONENT_PREPARING;
    m->revision++;
    return true;
}

bool app_modules_push(app_modules_t *m, size_t image_index, unsigned profile)
{
    if(!m || !m->worker || m->push_pending || image_index >= m->count ||
       profile < 1 || profile > 2 || !app_modules_healthy(m) || m->profile != profile) return false;
    bool probing = m->worker->pid > 0 && m->worker->active_action == BINBOT_COMPONENT_PROBE;
    if(m->worker->pid > 0 && !probing) return false;
    const app_module_image_t *image = &m->images[image_index];
    binbot_component_command_t command = {.action = BINBOT_COMPONENT_FILE_UPDATE,
                                          .size = image->sizes[profile - 1]};
    if(!request_id(command.request_id)) return false;
    command.reserved[0] = (uint8_t)profile;
    snprintf(command.sha256, sizeof(command.sha256), "%s", image->sha256[profile - 1]);
    snprintf(command.url, sizeof(command.url), "file://%s", image->files[profile - 1]);
    snprintf(command.file_name, sizeof(command.file_name), "display.gif");
    if(probing) {
        m->pending_push = command;
        m->push_pending = true;
    } else if(!bb_component_worker_submit(m->worker, &command)) return false;
    memcpy(m->push_id, command.request_id, 16);
    snprintf(m->pushing, sizeof(m->pushing), "%s", image->id);
    m->pushing_profile = profile;
    memset(&m->push_status, 0, sizeof(m->push_status));
    memcpy(m->push_status.request_id, command.request_id, sizeof(command.request_id));
    m->push_status.status = BINBOT_COMPONENT_PREPARING;
    m->push_status.total_bytes = command.size;
    m->revision++;
    return true;
}

const binbot_component_event_t *app_modules_push_status(const app_modules_t *m)
{
    return m && m->pushing_profile ? &m->push_status : NULL;
}
unsigned app_modules_push_profile(const app_modules_t *m) { return m ? m->pushing_profile : 0; }
const char *app_modules_push_image(const app_modules_t *m) { return m ? m->pushing : ""; }

bool app_modules_receive(app_modules_t *m, const char *url)
{
    if(!m || !url || m->download_pid > 0 || m->receipt ||
       (strncmp(url, "http://", 7) && strncmp(url, "https://", 8))) return false;
    const char *id = strrchr(url, '/');
    if(!id || !uuid_valid(++id) || strlen(url) > 512) return false;
    char final[384];
    snprintf(final, sizeof(final), "%s/%s", m->directory, id);
    app_module_image_t existing;
    if(parse_image(final, &existing, true) && !strcmp(existing.id, id)) { m->receipt = 1; return true; }
    snprintf(m->staging, sizeof(m->staging), "%s/.incoming-%s-%ld", m->directory, id, (long)getpid());
    remove_staging(m->staging);
    if(mkdir(m->staging, 0700)) return false;
    pid_t child = fork();
    if(child < 0) { remove_staging(m->staging); return false; }
    if(child == 0) {
        setpgid(0, 0);
        const char *files[] = {"manifest.json", "thumbnail.png", "round.gif", "holo.gif"};
        bool ok = true;
        for(size_t i = 0; ok && i < 4; ++i) {
            char path[512], resource[576];
            snprintf(path, sizeof(path), "%s/%s", m->staging, files[i]);
            snprintf(resource, sizeof(resource), "%s/%s", url, files[i]);
            ok = fetch_file(resource, path, i == 0 ? 8192 : i == 1 ? 256 * 1024 : 512 * 1024);
        }
        app_module_image_t image;
        ok = ok && parse_image(m->staging, &image, true) && !strcmp(image.id, id);
        if(ok && rename(m->staging, final) != 0) {
            if(errno == EEXIST || errno == ENOTEMPTY) {
                remove_staging(final);
                ok = rename(m->staging, final) == 0;
            } else ok = false;
        }
        if(!ok) remove_staging(m->staging);
        _exit(ok ? 0 : 1);
    }
    setpgid(child, child);
    m->download_pid = child;
    return true;
}

int app_modules_poll_receive(app_modules_t *m)
{
    if(!m) return 0;
    if(m->receipt) { int result = m->receipt; m->receipt = 0; scan_images(m); return result; }
    if(m->download_pid <= 0) return 0;
    int status;
    pid_t waited = waitpid(m->download_pid, &status, WNOHANG);
    if(waited == 0 || (waited < 0 && errno == EINTR)) return 0;
    m->download_pid = 0;
    bool ok = waited > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if(ok) scan_images(m);
    else remove_staging(m->staging);
    return ok ? 1 : -1;
}
