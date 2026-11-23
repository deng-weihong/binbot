#include "binbot/control/service.h"
#include "binbot/control/ipc.h"
#include <cJSON.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum { JOURNAL_LIMIT = 128 };
struct bb_control_service {
    bb_control_socket_t socket;
    bb_control_hooks_t hooks;
    cJSON *journal, *snapshot;
    char *snapshot_text;
    char journal_path[384], boot[33], pending[37];
    uint64_t sequence;
    uint32_t revision, last_send, pending_at, last_capture, pending_timeout;
    bool pending_volume;
    uint8_t brightness, volume;
    bool volume_valid, dirty, journal_valid;
};

static const char *string(const cJSON *root, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(item) ? item->valuestring : "";
}

static bool integer(const cJSON *root, const char *key, double low, double high, double *value)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if(!cJSON_IsNumber(item) || !isfinite(item->valuedouble) || floor(item->valuedouble) != item->valuedouble ||
       item->valuedouble < low || item->valuedouble > high) return false;
    *value = item->valuedouble; return true;
}

static bool identifier(const char *id)
{
    if(strlen(id) != 36) return false;
    for(size_t i = 0; i < 36; ++i) {
        if(i == 8 || i == 13 || i == 18 || i == 23) { if(id[i] != '-') return false; }
        else if(!((id[i] >= 'a' && id[i] <= 'f') || (id[i] >= '0' && id[i] <= '9'))) return false;
    }
    return true;
}

static bool save_journal(bb_control_service_t *s)
{
    char temporary[400];
    snprintf(temporary, sizeof(temporary), "%s.tmp", s->journal_path);
    char *json = cJSON_PrintUnformatted(s->journal);
    if(!json) return false;
    FILE *file = fopen(temporary, "w");
    if(!file) { free(json); return false; }
    bool ok = fchmod(fileno(file), 0600) == 0 && fputs(json, file) >= 0 &&
              fflush(file) == 0 && fsync(fileno(file)) == 0;
    if(fclose(file) != 0) ok = false;
    free(json);
    if(ok && rename(temporary, s->journal_path) != 0) ok = false;
    if(ok) {
        char parent[384]; snprintf(parent, sizeof(parent), "%s", s->journal_path);
        char *slash = strrchr(parent, '/');
        if(slash) { *slash = '\0'; int fd = open(parent[0] ? parent : "/", O_RDONLY | O_DIRECTORY);
                    if(fd < 0) ok = false; else { ok = fsync(fd) == 0; close(fd); } }
    }
    if(!ok) unlink(temporary);
    return ok;
}

static cJSON *find_record(bb_control_service_t *s, const char *id)
{
    cJSON *record;
    cJSON_ArrayForEach(record, s->journal)
        if(strcmp(string(record, "id"), id) == 0) return record;
    return NULL;
}

static void reply(bb_control_service_t *s, const char *id, bool success, const char *message)
{
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "type", "device.control.result");
    cJSON_AddNumberToObject(result, "schema_version", 2);
    cJSON_AddStringToObject(result, "boot_id", s->boot);
    cJSON_AddStringToObject(result, "command_id", id);
    cJSON_AddBoolToObject(result, "success", success);
    cJSON_AddStringToObject(result, "message", message);
    char *json = cJSON_PrintUnformatted(result);
    if(json) { bb_control_send(s->socket.peer, json); free(json); }
    cJSON_Delete(result);
}

static void finish(bb_control_service_t *s, const char *id, bool success, const char *message)
{
    cJSON *record = find_record(s, id);
    if(record) {
        cJSON_AddBoolToObject(record, "done", true);
        cJSON_AddBoolToObject(record, "success", success);
        cJSON_AddStringToObject(record, "message", message);
        if(!save_journal(s)) { s->journal_valid = false; success = false; message = "执行结果存储失败，需要核实设备状态"; }
    }
    reply(s, id, success, message);
    s->dirty = true;
}

static void command(bb_control_service_t *s, const cJSON *root, uint32_t now)
{
    const char *id = string(root, "command_id"), *name = string(root, "name");
    if(!identifier(id)) return;
    const cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
    bool brightness = strcmp(name, "screen.brightness.set") == 0;
    bool volume = strcmp(name, "audio.volume.set") == 0;
    double value = 0, version = 0, issued = 0, expires = 0;
    bool generic = !brightness && !volume && s->hooks.command;
    if(!cJSON_IsObject(root) || cJSON_GetArraySize(root) != 8 ||
       strcmp(string(root, "type"), "device.command") || (!brightness && !volume && !generic) ||
       !integer(root, "schema_version", 2, 2, &version) || !cJSON_IsObject(payload) ||
       (!generic && (cJSON_GetArraySize(payload) != 1 ||
       !integer(payload, brightness ? "brightness" : "volume", brightness ? 1 : 0, 100, &value)))) {
        reply(s, id, false, "控制命令格式或能力不支持"); return;
    }
    cJSON *existing = find_record(s, id);
    if(existing) {
        double saved = -1;
        integer(existing, "value", 0, 100, &saved);
        const cJSON *saved_payload = cJSON_GetObjectItemCaseSensitive(existing, "payload");
        bool same = saved_payload ? cJSON_Compare(saved_payload, payload, true) : (!generic && saved == value);
        if(strcmp(string(existing, "name"), name) || !same) {
            reply(s, id, false, "命令 ID 与已有操作不一致"); return;
        }
        if(!strcmp(s->pending, id)) return;
        bool done = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(existing, "done"));
        reply(s, id, done && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(existing, "success")),
              done ? string(existing, "message") : "上次执行未确认，不重复执行");
        return;
    }
    time_t clock = time(NULL);
    if(strcmp(string(root, "boot_id"), s->boot) || !integer(root,"issued_at",946684800,4102444800.0,&issued) ||
       !integer(root,"expires_at",946684800,4102444800.0,&expires) ||
       clock < issued - 5 || clock >= expires || expires <= issued || expires - issued > 60) {
        reply(s, id, false, "命令已过期、设备已重启或设备时间未校准"); return;
    }
    if(!s->journal_valid || s->pending[0]) { reply(s, id, false, "控制服务繁忙或去重存储不可用"); return; }
    while(cJSON_GetArraySize(s->journal) >= JOURNAL_LIMIT) {
        double deadline = 0;
        integer(cJSON_GetArrayItem(s->journal, 0), "expires_at", 0, 4102444800.0, &deadline);
        if(deadline >= clock) { reply(s,id,false,"控制命令过多，请稍后重试"); return; }
        cJSON_DeleteItemFromArray(s->journal, 0);
    }
    cJSON *record = cJSON_CreateObject();
    cJSON_AddStringToObject(record, "id", id);
    cJSON_AddStringToObject(record, "name", name);
    cJSON_AddNumberToObject(record, "value", value);
    cJSON_AddItemToObject(record, "payload", cJSON_Duplicate(payload, true));
    cJSON_AddNumberToObject(record, "expires_at", expires);
    cJSON_AddItemToArray(s->journal, record);
    if(!save_journal(s)) { s->journal_valid = false; reply(s,id,false,"无法保存命令，未执行操作"); return; }
    if(brightness) {
        bool ok = s->hooks.brightness && s->hooks.brightness(s->hooks.context, (uint8_t)value);
        if(ok) s->brightness = (uint8_t)value;
        finish(s, id, ok, ok ? "亮度已设置并保存" : "背光设置或保存失败");
    } else if(volume) {
        s->sequence++;
        if(s->volume_valid && s->hooks.volume && s->hooks.volume(s->hooks.context, (uint8_t)value, s->sequence)) {
            snprintf(s->pending, sizeof(s->pending), "%s", id); s->pending_at = now;
            s->pending_timeout = 3000U; s->pending_volume = true;
        } else finish(s, id, false, "音频服务尚未就绪");
    } else {
        char message[256] = "设备不支持该操作";
        s->sequence++;
        int result = s->hooks.command(s->hooks.context, name, payload, id, s->sequence, message, sizeof(message));
        if(result == 2) {
            snprintf(s->pending, sizeof(s->pending), "%s", id); s->pending_at = now;
            s->pending_timeout = !strcmp(name, "display.gif") || !strcmp(name, "modules.image") || !strcmp(name, "module.display.set") ? 50000U : 10000U;
            s->pending_volume = false;
        } else finish(s, id, result == 1, message);
    }
}

bb_control_service_t *bb_control_service_create(const char *socket, const char *journal, bb_control_hooks_t hooks)
{
    if(!journal || strlen(journal) >= 384) return NULL;
    bb_control_service_t *s = calloc(1, sizeof(*s));
    if(!s) return NULL;
    s->hooks = hooks; s->dirty = true;
    snprintf(s->journal_path, sizeof(s->journal_path), "%s", journal);
    unsigned char random[16];
    FILE *rng = fopen("/dev/urandom", "rb");
    bool random_ok = rng && fread(random, 1, sizeof(random), rng) == sizeof(random);
    if(rng) fclose(rng);
    if(!random_ok) { free(s); return NULL; }
    for(size_t i = 0; i < sizeof(random); ++i) snprintf(s->boot + i*2, 3, "%02x", random[i]);
    s->journal = cJSON_CreateArray(); s->journal_valid = true;
    FILE *file = fopen(journal, "r");
    if(file) {
        const size_t capacity = 4U * 1024U * 1024U;
        char *data = malloc(capacity);
        size_t count = data ? fread(data, 1, capacity - 1, file) : 0;
        if(data) data[count] = '\0';
        bool bounded = data && !ferror(file) && feof(file); fclose(file);
        cJSON *loaded = bounded ? cJSON_ParseWithOpts(data, NULL, true) : NULL;
        free(data);
        if(bounded && cJSON_IsArray(loaded) && cJSON_GetArraySize(loaded) <= JOURNAL_LIMIT) {
            cJSON_Delete(s->journal); s->journal = loaded;
        } else { cJSON_Delete(loaded); s->journal_valid = false; }
    } else if(access(journal,F_OK) == 0) s->journal_valid = false;
    if(!bb_control_socket_open(&s->socket, socket)) { cJSON_Delete(s->journal); free(s); return NULL; }
    return s;
}

void bb_control_service_destroy(bb_control_service_t *s)
{
    if(!s) return;
    bb_control_socket_close(&s->socket); cJSON_Delete(s->journal);
    cJSON_Delete(s->snapshot); free(s->snapshot_text); free(s);
}

void bb_control_service_tick(bb_control_service_t *s, uint32_t now, uint8_t brightness)
{
    if(!s) return;
    if(s->brightness != brightness) { s->brightness = brightness; s->dirty = true; }
    int old_peer = s->socket.peer;
    bb_control_socket_accept(&s->socket);
    if(s->socket.peer != old_peer) s->dirty = true;
    char message[BB_CONTROL_MAX_MESSAGE];
    if(s->socket.peer >= 0) {
        int count = bb_control_receive(s->socket.peer, message, sizeof(message));
        if(count < 0) { close(s->socket.peer); s->socket.peer = -1; }
        else if(count > 0) {
            cJSON *root = cJSON_ParseWithOpts(message, NULL, true);
            if(root) command(s, root, now);
            cJSON_Delete(root);
        }
    }
    if(s->pending[0] && now - s->pending_at >= s->pending_timeout) {
        finish(s, s->pending, false, "设备执行结果未确认，请核对实际状态"); s->pending[0] = '\0';
    }
    if(s->hooks.snapshot && (s->dirty || now - s->last_capture >= 500U)) {
        cJSON *snapshot = s->hooks.snapshot(s->hooks.context);
        char *text = snapshot ? cJSON_PrintUnformatted(snapshot) : NULL;
        if(text && (!s->snapshot_text || strcmp(text, s->snapshot_text))) {
            cJSON_Delete(s->snapshot); free(s->snapshot_text);
            s->snapshot = snapshot; s->snapshot_text = text; s->dirty = true;
        } else { cJSON_Delete(snapshot); free(text); }
        s->last_capture = now;
    }
    if(s->socket.peer < 0 || (!s->dirty && now - s->last_send < 5000U)) return;
    if(s->dirty) { s->revision++; s->dirty = false; }
    cJSON *state = cJSON_CreateObject();
    cJSON_AddStringToObject(state,"type","device.control.state");
    cJSON_AddNumberToObject(state,"schema_version",2);
    cJSON_AddStringToObject(state,"boot_id",s->boot);
    cJSON_AddNumberToObject(state,"revision",s->revision);
    cJSON_AddBoolToObject(state,"ready",s->journal_valid);
    if(s->brightness) cJSON_AddNumberToObject(state,"brightness",s->brightness);
    else cJSON_AddNullToObject(state,"brightness");
    if(s->volume_valid) cJSON_AddNumberToObject(state,"volume",s->volume);
    else cJSON_AddNullToObject(state,"volume");
    cJSON *caps=cJSON_AddArrayToObject(state,"capabilities");
    cJSON_AddItemToArray(caps,cJSON_CreateString("control_v2"));
    cJSON_AddItemToArray(caps,cJSON_CreateString("brightness_v1"));
    if(s->volume_valid) cJSON_AddItemToArray(caps,cJSON_CreateString("volume_v1"));
    if(s->hooks.command && s->hooks.snapshot) cJSON_AddItemToArray(caps,cJSON_CreateString("workspace_v1"));
    const cJSON *features = cJSON_GetObjectItemCaseSensitive(s->snapshot, "features");
    if(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(features, "cameraAvailable")))
        cJSON_AddItemToArray(caps,cJSON_CreateString("camera_publish_v2"));
    for(const cJSON *item = s->snapshot ? s->snapshot->child : NULL; item; item = item->next)
        if(item->string) cJSON_AddItemToObject(state, item->string, cJSON_Duplicate(item, true));
    char *json = cJSON_PrintUnformatted(state);
    if(json) { if(bb_control_send(s->socket.peer,json)) s->last_send=now; else s->dirty=true; free(json); }
    cJSON_Delete(state);
}

bool bb_control_volume_pending(const bb_control_service_t *s) { return s && s->pending[0]; }

void bb_control_service_volume_result(bb_control_service_t *s, uint64_t request, uint8_t value, bool success)
{
    if(!s) return;
    if(success) { s->volume=value; s->volume_valid=true; s->dirty=true; }
    else { s->volume_valid=false; s->dirty=true; }
    if(s->pending[0] && s->pending_volume && request == s->sequence) {
        finish(s,s->pending,success,success ? "音量已设置并保存" : "声卡设置或保存失败"); s->pending[0]='\0';
    }
}

void bb_control_service_audio_disconnected(bb_control_service_t *s)
{
    if(!s) return;
    if(s->volume_valid) { s->volume_valid=false; s->dirty=true; }
    if(s->pending[0]) { finish(s,s->pending,false,"音频服务已断开"); s->pending[0]='\0'; }
}

void bb_control_service_complete(bb_control_service_t *s, uint64_t request,
                                 bool success, const char *message)
{
    if(!s || !s->pending[0] || s->pending_volume || request != s->sequence) return;
    finish(s, s->pending, success, message); s->pending[0] = '\0';
}

void bb_control_service_complete_id(bb_control_service_t *s, const char *id,
                                    bool success, const char *message)
{
    if(!s || !s->pending[0] || strcmp(s->pending, id)) return;
    finish(s, id, success, message); s->pending[0] = '\0';
}
