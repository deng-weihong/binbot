#define _POSIX_C_SOURCE 200809L
#include "binbot/app_modules.h"
#include <assert.h>
#include <cjson/cJSON.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char *id = "11111111-2222-4333-8444-555555555555";

static void write_bytes(const char *path, const void *data, size_t size)
{
    FILE *file = fopen(path, "wb"); assert(file);
    assert(fwrite(data, 1, size, file) == size);
    assert(fclose(file) == 0);
}

static void fixture(const char *root, bool legacy)
{
    const char *keys[] = {"round", "holo", "thumbnail"};
    cJSON *json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "schema", 1);
    cJSON_AddStringToObject(json, "id", id);
    cJSON_AddStringToObject(json, "name", "Uploaded picture.png");
    for(unsigned i = 0; i < 3; ++i) {
        unsigned char bytes[128], digest[32];
        memset(bytes, 0x40 + i, sizeof(bytes));
        unsigned count;
        assert(EVP_Digest(bytes, sizeof(bytes), digest, &count, EVP_sha256(), NULL) == 1);
        char sha[65], path[512];
        for(size_t n = 0; n < 32; ++n) snprintf(sha + n * 2, 3, "%02x", digest[n]);
        snprintf(path, sizeof(path), "%s/%s.%s", root, keys[i], i == 2 ? "png" : "gif");
        write_bytes(path, bytes, sizeof(bytes));
        cJSON *item = cJSON_AddObjectToObject(json, keys[i]);
        cJSON_AddNumberToObject(item, "size", sizeof(bytes));
        cJSON_AddStringToObject(item, "sha256", sha);
        if(i < 2) {
            cJSON_AddNumberToObject(item, "width", i ? legacy ? 132 : 320 : 240);
            cJSON_AddNumberToObject(item, "height", i && legacy ? 132 : 240);
        }
    }
    char *text = cJSON_PrintUnformatted(json), path[512];
    snprintf(path, sizeof(path), "%s/manifest.json", root);
    write_bytes(path, text, strlen(text));
    cJSON_free(text); cJSON_Delete(json);
}

static int await_receive(app_modules_t *m)
{
    for(unsigned i = 0; i < 400; ++i) {
        int received = app_modules_poll_receive(m);
        if(received) return received;
        struct timespec pause = {0, 10000000}; nanosleep(&pause, NULL);
    }
    assert(!"image receive timed out"); return 0;
}

int main(void)
{
    char root[] = "/tmp/binbot-modules-test-XXXXXX";
    assert(mkdtemp(root));
    char source[128], gallery[128], executable[128], worker_dir[128];
    snprintf(source, sizeof(source), "%s/source", root); assert(!mkdir(source, 0700));
    snprintf(gallery, sizeof(gallery), "%s/gallery", root);
    snprintf(worker_dir, sizeof(worker_dir), "%s/worker", root);
    fixture(source, false);
    snprintf(executable, sizeof(executable), "%s/wget", root);
    const char *script = "#!/bin/sh\n/bin/cp \"$BINBOT_MODULE_TEST_SOURCE/${6##*/}\" \"$5\"\n";
    write_bytes(executable, script, strlen(script)); assert(!chmod(executable, 0700));
    assert(!setenv("BINBOT_MODULE_TEST_SOURCE", source, 1));
    assert(!setenv("PATH", root, 1));
    app_modules_t *m = app_modules_create(gallery); assert(m);
    char url[128]; snprintf(url, sizeof(url), "http://test/module-images/%s", id);
    assert(!app_modules_receive(m, "file:///untrusted"));
    assert(!app_modules_receive(m, "http://test/not-a-uuid"));
    assert(app_modules_receive(m, url));
    assert(!app_modules_receive(m, url));
    assert(await_receive(m) == 1);
    assert(app_modules_image_count(m) == 1);
    assert(!strcmp(app_modules_image(m, 0)->name, "Uploaded picture.png"));
    assert(!app_modules_set_current(m, id, 0));
    assert(!app_modules_set_current(m, id, 3));
    assert(!app_modules_set_current(m, "bad-id", 1));
    assert(!app_modules_set_current(m, "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee", 1));
    uint32_t revision = app_modules_revision(m);
    assert(app_modules_set_current(m, id, 2));
    assert(app_modules_revision(m) > revision && !strcmp(app_modules_current(m, 2), id));
    revision = app_modules_revision(m);
    assert(app_modules_set_current(m, id, 2));
    assert(app_modules_revision(m) == revision);
    assert(app_modules_receive(m, url)); assert(await_receive(m) == 1);
    assert(app_modules_image_count(m) == 1);
    assert(!app_modules_push(m, 0, 1));
    assert(!setenv("BINBOT_COMPONENT_STATE_DIR", worker_dir, 1));
    assert(!setenv("BINBOT_COMPONENT_UART", "/nonexistent-binbot-test-uart", 1));
    bb_component_worker_t worker; bb_component_worker_init(&worker);
    app_modules_set_worker(m, &worker);
    binbot_component_event_t event = {.status = BINBOT_COMPONENT_PROBED, .has_info = 1,
                                       .healthy = 1, .profile = 1};
    app_modules_handle_event(m, &event);
    assert(app_modules_profile(m) == 1 && app_modules_healthy(m));
    binbot_component_event_t slave_event = event;
    slave_event.profile = 4;
    revision = app_modules_revision(m);
    assert(!app_modules_handle_event(m, &slave_event));
    assert(app_modules_profile(m) == 1 && app_modules_healthy(m));
    assert(app_modules_revision(m) == revision);
    app_modules_request_probe(m);
    app_modules_tick(m, 1500);
    assert(worker.pid > 0);
    assert(!app_modules_push(m, 0, 2));
    assert(app_modules_push(m, 0, 1));
    assert(!app_modules_push(m, 0, 1));
    assert(app_modules_push_status(m)->status == BINBOT_COMPONENT_PREPARING);
    for(unsigned i = 0; worker.pid > 0 && i < 400; ++i) {
        binbot_component_event_t incoming;
        bb_component_worker_poll(&worker, &incoming, 1600 + i * 10);
        struct timespec pause = {0, 10000000}; nanosleep(&pause, NULL);
    }
    assert(worker.pid == 0);
    app_modules_tick(m, 12000);
    assert(worker.pid > 0);
    memcpy(event.request_id, worker.latest.request_id, 16);
    event.status = BINBOT_COMPONENT_FAILED;
    app_modules_handle_event(m, &event);
    assert(!app_modules_current(m, 1)[0]);
    event.status = BINBOT_COMPONENT_SUCCEEDED;
    app_modules_handle_event(m, &event);
    assert(!strcmp(app_modules_current(m, 1), id));
    bb_component_worker_close(&worker);
    worker.pid = 0;
    assert(!app_modules_motor_set(m, 1, 40));
    event = (binbot_component_event_t){.status = BINBOT_COMPONENT_PROBED, .has_info = 1,
        .healthy = 1, .profile = 3, .motor_supported = 1, .motor_speed = 40};
    app_modules_handle_event(m, &event);
    assert(app_modules_motor_supported(m) && app_modules_motor_speed(m) == 40);
    assert(!app_modules_motor_set(m, 3, 40));
    assert(!app_modules_motor_set(m, 1, 101));
    worker.pid = 1; worker.active_action = BINBOT_COMPONENT_OTA;
    assert(!app_modules_motor_set(m, 1, 40));
    worker.active_action = BINBOT_COMPONENT_PROBE;
    assert(app_modules_motor_set(m, 2, 65));
    assert(app_modules_motor_busy(m));
    assert(!app_modules_motor_set(m, 0, 40));
    assert(app_modules_motor_direction(m) == 0 && app_modules_motor_speed(m) == 40);
    worker.pid = 0;
    app_modules_tick(m, 6500);
    assert(worker.active_action == BINBOT_COMPONENT_MOTOR_CONTROL && worker.pid > 0);
    memcpy(event.request_id, worker.latest.request_id, 16);
    event.status = BINBOT_COMPONENT_SUCCEEDED;
    event.motor_direction = 2; event.motor_speed = 65;
    app_modules_handle_event(m, &event);
    assert(!app_modules_motor_busy(m));
    assert(app_modules_motor_direction(m) == 2 && app_modules_motor_speed(m) == 65);
    bb_component_worker_close(&worker);
    event = (binbot_component_event_t){.status = BINBOT_COMPONENT_PROBED};
    app_modules_handle_event(m, &event);
    assert(!app_modules_motor_supported(m));
    app_modules_tick(m, 23000);
    assert(worker.pid > 0 && worker.active_action == BINBOT_COMPONENT_PROBE);
    bb_component_worker_close(&worker);
    app_modules_destroy(m);
    m = app_modules_create(gallery); assert(m);
    assert(app_modules_image_count(m) == 1 && !strcmp(app_modules_current(m, 1), id));
    assert(!strcmp(app_modules_current(m, 2), id));
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", gallery, id);
    fixture(path, true);
    app_modules_destroy(m);
    m = app_modules_create(gallery); assert(m);
    assert(app_modules_image_count(m) == 1 && !strcmp(app_modules_current(m, 1), id));
    assert(app_modules_receive(m, url)); assert(await_receive(m) == 1);
    assert(app_modules_image_count(m) == 1 && !strcmp(app_modules_current(m, 1), id));
    snprintf(path, sizeof(path), "%s/%s/manifest.json", gallery, id);
    FILE *manifest_file = fopen(path, "rb"); assert(manifest_file);
    char manifest[2048]; size_t bytes = fread(manifest, 1, sizeof(manifest) - 1, manifest_file);
    assert(!ferror(manifest_file)); manifest[bytes] = 0; fclose(manifest_file);
    cJSON *json = cJSON_Parse(manifest); assert(json);
    cJSON *holo = cJSON_GetObjectItemCaseSensitive(json, "holo");
    assert(cJSON_GetObjectItemCaseSensitive(holo, "width")->valueint == 320);
    assert(cJSON_GetObjectItemCaseSensitive(holo, "height")->valueint == 240);
    cJSON_Delete(json);
    snprintf(path, sizeof(path), "%s/%s/thumbnail.png", gallery, id);
    unsigned char corrupt[128]; memset(corrupt, 0xff, sizeof(corrupt));
    write_bytes(path, corrupt, sizeof(corrupt));
    assert(app_modules_receive(m, url));
    assert(await_receive(m) == 1);
    assert(app_modules_image_count(m) == 1);
    snprintf(path, sizeof(path), "%s/thumbnail.png", source); write_bytes(path, corrupt, sizeof(corrupt));
    snprintf(path, sizeof(path), "%s/%s/thumbnail.png", gallery, id);
    write_bytes(path, corrupt, sizeof(corrupt));
    assert(app_modules_receive(m, url));
    assert(await_receive(m) == -1);
    app_modules_destroy(m);
    const char *files[] = {"manifest.json", "round.gif", "holo.gif", "thumbnail.png"};
    for(size_t i = 0; i < 4; ++i) {
        snprintf(path, sizeof(path), "%s/%s", source, files[i]); unlink(path);
        snprintf(path, sizeof(path), "%s/%s/%s", gallery, id, files[i]); unlink(path);
    }
    snprintf(path, sizeof(path), "%s/%s", gallery, id); rmdir(path);
    snprintf(path, sizeof(path), "%s/display.json", gallery); unlink(path);
    snprintf(path, sizeof(path), "%s/job.state", worker_dir); unlink(path);
    snprintf(path, sizeof(path), "%s/worker.lock", worker_dir); unlink(path);
    unlink(executable); rmdir(source); rmdir(gallery); rmdir(worker_dir); assert(!rmdir(root));
    puts("module gallery integrity, deduplication, profile and persistence tests passed");
    return 0;
}
