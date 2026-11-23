#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "binbot/component/worker.h"
#include "binbot/component/client.h"
#include "binbot/component/slave_ota.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <openssl/evp.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    uint32_t magic;
    uint32_t size;
    binbot_component_command_t command;
    binbot_component_event_t event;
    uint8_t activated;
    uint32_t crc;
} worker_record_t;

_Static_assert(sizeof(binbot_component_event_t) == 344,
               "Component event must retain the deployed IPC size");
_Static_assert(offsetof(binbot_component_event_t, motor_supported) == 341,
               "Motor state must only occupy the original tail padding");

typedef struct {
    worker_record_t record;
    const char *directory;
    int fd;
    uint32_t last_saved_bytes;
} work_t;

static void path_for(char *path, size_t size, const char *directory, const char *name)
{
    snprintf(path, size, "%s/%s", directory, name);
}

static bool read_record(const char *directory, worker_record_t *record)
{
    char path[320];
    path_for(path, sizeof(path), directory, "job.state");
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    const bool ok = fread(record, 1, sizeof(*record), file) == sizeof(*record) &&
                    record->magic == 0x42434f32 && record->size == sizeof(*record) &&
                    record->crc == bb_component_crc32((const uint8_t *)record, offsetof(worker_record_t, crc));
    fclose(file);
    return ok;
}

static bool save_record(work_t *work)
{
    work->record.crc = bb_component_crc32((const uint8_t *)&work->record, offsetof(worker_record_t, crc));
    char path[320], temporary[320];
    path_for(path, sizeof(path), work->directory, "job.state");
    path_for(temporary, sizeof(temporary), work->directory, "job.tmp");
    int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;
    const ssize_t n = write(fd, &work->record, sizeof(work->record));
    const bool synced = n == sizeof(work->record) && fsync(fd) == 0;
    close(fd);
    if (!synced || rename(temporary, path)) return false;
    fd = open(work->directory, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return false;
    const bool ok = fsync(fd) == 0;
    close(fd);
    return ok;
}

static bool cancelled(work_t *work)
{
    char path[320];
    path_for(path, sizeof(path), work->directory, "cancel");
    uint8_t id[16];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return work->record.command.cancel;
    const ssize_t n = read(fd, id, sizeof(id));
    close(fd);
    return n == sizeof(id) && !memcmp(id, work->record.command.request_id, sizeof(id));
}

static void hex_digest(char *output, const uint8_t *digest)
{
    for (size_t i = 0; i < 32; ++i) snprintf(output + i * 2, 3, "%02x", digest[i]);
}

static void set_info(work_t *work, const bb_component_info_t *info)
{
    binbot_component_event_t *event = &work->record.event;
    event->has_info = 1;
    event->profile = info->profile;
    event->state = info->state;
    event->healthy = info->healthy;
    event->boot_id = info->boot_id;
    event->capacity = info->capacity;
    memcpy(event->version, info->version, sizeof(event->version));
    memcpy(event->project, info->project, sizeof(event->project));
    hex_digest(event->elf_sha256, info->elf_sha256);
    event->motor_supported = info->motor_supported;
    event->motor_direction = info->motor_direction;
    event->motor_speed = info->motor_speed;
    if(info->profile == BB_COMPONENT_ROUND_SCREEN) {
        if(info->pen_state_valid)
            snprintf(event->message, sizeof(event->message),
                     "{\"pen\":{\"supported\":true,\"mode\":%u,\"face\":%u}}", info->pen.mode, info->pen.face);
        else snprintf(event->message, sizeof(event->message), "{\"pen\":{\"supported\":%s}}",
                      info->pen_supported ? "true" : "false");
    }
    if(info->profile == BB_COMPONENT_AMBIENT_LIGHT) {
        /* Keep the deployed event ABI and persistent OTA record size unchanged. */
        if(info->light_state_valid) {
            const bb_component_light_settings_t *s = &info->light;
            snprintf(event->message, sizeof(event->message),
                     "{\"light\":{\"supported\":true,\"on\":%u,\"fx\":%u,\"r\":%u,\"g\":%u,\"b\":%u,\"v\":%u,\"sp\":%u,\"off\":%u,\"left\":%u}}",
                     s->enabled, s->effect, s->red, s->green, s->blue, s->brightness,
                     s->speed, s->timer_minutes, info->light_remaining_minutes);
        } else snprintf(event->message, sizeof(event->message), "{\"light\":{\"supported\":%s}}",
                        info->light_supported ? "true" : "false");
    }
}

static bool emit(work_t *work, uint32_t status, const char *message)
{
    work->record.event.status = status;
    work->record.event.event_seq++;
    if(message) snprintf(work->record.event.message, sizeof(work->record.event.message), "%s", message);
    const bool persistent = work->record.command.action == BINBOT_COMPONENT_OTA ||
                            work->record.command.action == BINBOT_COMPONENT_FILE_UPDATE;
    if (persistent && !save_record(work)) {
        work->record.event.status = work->record.activated ? BINBOT_COMPONENT_RECONCILING : BINBOT_COMPONENT_FAILED;
        snprintf(work->record.event.message, sizeof(work->record.event.message), "cannot persist component job state");
        (void)send(work->fd, &work->record.event, sizeof(work->record.event), MSG_DONTWAIT | MSG_NOSIGNAL);
        return false;
    }
    (void)send(work->fd, &work->record.event, sizeof(work->record.event), MSG_DONTWAIT | MSG_NOSIGNAL);
    return true;
}

static void progress(uint32_t received, uint32_t total, void *context)
{
    work_t *work = context;
    work->record.event.bytes_written = received;
    work->record.event.total_bytes = total;
    /* Keep flash writes bounded; receiver QUERY supplies exact resume offset. */
    if (received - work->last_saved_bytes >= 4096 || received == total) {
        work->last_saved_bytes = received;
        if (!emit(work, BINBOT_COMPONENT_TRANSFERRING, "")) _exit(2);
    }
}

static int phase(bb_component_phase_t value, void *context)
{
    work_t *work = context;
    if (value != BB_COMPONENT_PHASE_VERIFYING && cancelled(work)) return 1;
    if (value == BB_COMPONENT_PHASE_ACTIVATING) work->record.activated = 1;
    if (work->record.event.status != (uint32_t)value && !emit(work, value, "")) _exit(2);
    return 0;
}

static int file_phase(bb_component_phase_t value, void *context)
{
    work_t *work = context;
    if (cancelled(work)) return 1;
    if (work->record.event.status != (uint32_t)value && !emit(work, value, "")) _exit(2);
    return 0;
}

static bool matches(const work_t *work, const bb_component_info_t *info)
{
    char elf[65];
    hex_digest(elf, info->elf_sha256);
    return info->healthy && !strcmp(info->version, work->record.command.version) &&
           !strcmp(elf, work->record.command.elf_sha256);
}

static int check_file(work_t *work, const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) return -errno;
    EVP_MD_CTX *hash = EVP_MD_CTX_new();
    bool ok = hash && EVP_DigestInit_ex(hash, EVP_sha256(), NULL) == 1;
    uint8_t buffer[4096], digest[32];
    unsigned int digest_size;
    size_t size = 0, count;
    while (ok && (count = fread(buffer, 1, sizeof(buffer), file)) != 0) {
        size += count;
        ok = EVP_DigestUpdate(hash, buffer, count) == 1;
    }
    ok = ok && !ferror(file) && size == work->record.command.size &&
         EVP_DigestFinal_ex(hash, digest, &digest_size) == 1 && digest_size == 32;
    if (hash) EVP_MD_CTX_free(hash);
    fclose(file);
    if (!ok) return -EINVAL;
    char actual[65];
    hex_digest(actual, digest);
    return strcmp(actual, work->record.command.sha256) ? -EINVAL : 0;
}

static int download(work_t *work, const char *path)
{
    if (!emit(work, BINBOT_COMPONENT_DOWNLOADING, "")) return -EIO;
    /* Never inspect a previous job's cache while wget is still starting. In
     * particular, a smaller replacement must not trip the size limit before
     * wget has truncated its output. Publish only a verified temporary file. */
    char temporary[384];
    const int length = snprintf(temporary, sizeof(temporary), "%s.download-XXXXXX", path);
    if (length < 0 || (size_t)length >= sizeof(temporary)) return -ENAMETOOLONG;
    const int output = mkstemp(temporary);
    if (output < 0) return -errno;
    close(output);
    if (!strncmp(work->record.command.url, "file://", 7)) {
        FILE *source = fopen(work->record.command.url + 7, "rb");
        FILE *destination = fopen(temporary, "wb");
        bool ok = source && destination;
        uint8_t buffer[4096];
        size_t count, total = 0;
        while (ok && (count = fread(buffer, 1, sizeof(buffer), source)) != 0) {
            total += count;
            ok = total <= work->record.command.size && !cancelled(work) &&
                 fwrite(buffer, 1, count, destination) == count;
        }
        if (source) { if (ferror(source)) ok = false; fclose(source); }
        if (destination) {
            if (fflush(destination) || fsync(fileno(destination))) ok = false;
            if (fclose(destination)) ok = false;
        }
        const int result = ok ? check_file(work, temporary) : -EIO;
        const bool committed = !result && rename(temporary, path) == 0;
        unlink(temporary);
        return committed ? 0 : result ? result : -errno;
    }
    pid_t child = fork();
    if (child < 0) {
        const int result = -errno;
        unlink(temporary);
        return result;
    }
    if (!child) {
        /* Download capability is restricted to this job and configured Server. */
        execlp("wget", "wget", "-q", "-T", "15", "-O", temporary, work->record.command.url, (char *)NULL);
        _exit(127);
    }
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    const time_t deadline = now.tv_sec + 90;
    int status = 0, result = -EIO;
    pid_t waited;
    while ((waited = waitpid(child, &status, WNOHANG)) == 0 || (waited < 0 && errno == EINTR)) {
        struct stat info;
        if (cancelled(work)) { result = -ECANCELED; break; }
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec >= deadline) { result = -ETIMEDOUT; break; }
        if (!stat(temporary, &info) && info.st_size > (off_t)work->record.command.size) { result = -EFBIG; break; }
        const struct timespec pause = {0, 100000000};
        nanosleep(&pause, NULL);
    }
    const int wait_error = waited < 0 ? errno : 0;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (waited == child && WIFEXITED(status) && result == -EIO &&
        now.tv_sec < deadline && !cancelled(work)) {
        /* Accept complete, matching bytes even if the downloader reports an
         * error after writing them. This never substitutes for SHA checking. */
        const int verified = check_file(work, temporary);
        if (verified == 0 || WEXITSTATUS(status) == 0) result = verified;
    }
    if (waited == 0) {
        kill(child, SIGTERM);
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    }
    if (!result && rename(temporary, path)) result = -errno;
    struct stat downloaded;
    const long bytes = stat(result ? temporary : path, &downloaded) == 0 ?
                           (long)downloaded.st_size : -1L;
    fprintf(stderr, "component download: exit=%d signal=%d wait_errno=%d "
            "result=%d bytes=%ld expected=%u\n",
            WIFEXITED(status) ? WEXITSTATUS(status) : -1,
            WIFSIGNALED(status) ? WTERMSIG(status) : 0,
            wait_error, result, bytes, work->record.command.size);
    unlink(temporary);
    return result;
}

static const char *file_failure_message(int result, const char *client_error)
{
    if (client_error != NULL && client_error[0] != '\0') return client_error;
    if (result == -EINVAL) return "GIF size or SHA-256 verification failed";
    if (result == -ETIMEDOUT) return "GIF download timed out";
    if (result == -ECANCELED) return "GIF transfer cancelled";
    return "GIF download failed; check Server address and device network";
}

static void run(work_t *work)
{
    const bool slave_target = work->record.command.reserved[3] == 1;
    bb_component_client_t client = {.fd = -1};
    binbot_slave_client_t slave = {.fd = -1};
    const char *uart = getenv("BINBOT_COMPONENT_UART");
    int result = slave_target ? bb_slave_ota_open(&slave, client.error) :
                 bb_component_client_open(&client, uart && *uart ? uart : BB_COMPONENT_DEFAULT_UART);
    if (result) {
        emit(work, work->record.command.action == BINBOT_COMPONENT_PROBE ? BINBOT_COMPONENT_PROBED :
             work->record.activated ? BINBOT_COMPONENT_RECONCILING : BINBOT_COMPONENT_FAILED, client.error);
        return;
    }
    bb_component_info_t info;
    result = slave_target ? bb_slave_ota_status(&slave, &info, client.error) :
             work->record.command.action == BINBOT_COMPONENT_PROBE ?
                           bb_component_client_probe(&client, &info) :
                           bb_component_client_status(&client, &info);
    if (!result) set_info(work, &info);
    if (work->record.command.action == BINBOT_COMPONENT_PROBE) {
        emit(work, BINBOT_COMPONENT_PROBED, result ? client.error : NULL);
        goto done;
    }
    if(work->record.command.action == BINBOT_COMPONENT_PEN_CONTROL) {
        bb_component_pen_t settings;
        if(slave_target || !bb_component_pen_decode((const uint8_t *)work->record.command.url,
                                                     BB_COMPONENT_PEN_BYTES, &settings)) {
            emit(work, BINBOT_COMPONENT_FAILED, "invalid pen display command"); goto done;
        }
        if(!result && (info.profile != BB_COMPONENT_ROUND_SCREEN ||
                       work->record.command.before_boot_id != info.boot_id)) result = BB_COMPONENT_SESSION;
        if(!result) result = bb_component_client_pen_set(&client, &settings, &info);
        if(!result) set_info(work, &info);
        emit(work, result ? BINBOT_COMPONENT_FAILED : BINBOT_COMPONENT_SUCCEEDED,
             result ? client.error[0] ? client.error : "pen connection changed" : NULL);
        goto done;
    }
    if(work->record.command.action == BINBOT_COMPONENT_LIGHT_CONTROL) {
        bb_component_light_settings_t settings;
        if(slave_target || !bb_component_light_decode((const uint8_t *)work->record.command.file_name,
                                                       BB_COMPONENT_LIGHT_BYTES, &settings)) {
            emit(work, BINBOT_COMPONENT_FAILED, "invalid light control command"); goto done;
        }
        if(!result && work->record.command.reserved[0] != info.profile) result = BB_COMPONENT_WRONG_IMAGE;
        if(!result && work->record.command.before_boot_id != info.boot_id) result = BB_COMPONENT_SESSION;
        if(!result) result = bb_component_client_light_set(&client, &settings, &info);
        if(!result) set_info(work, &info);
        emit(work, result ? BINBOT_COMPONENT_FAILED : BINBOT_COMPONENT_SUCCEEDED,
             result ? client.error[0] ? client.error : "component changed since light selection" : NULL);
        goto done;
    }
    if (work->record.command.action == BINBOT_COMPONENT_MOTOR_CONTROL) {
        if(slave_target) { emit(work, BINBOT_COMPONENT_FAILED, "unsupported slave action"); goto done; }
        if (!result && work->record.command.reserved[0] != info.profile)
            result = BB_COMPONENT_WRONG_IMAGE;
        if (!result) result = bb_component_client_motor_set(&client,
            work->record.command.reserved[1], work->record.command.reserved[2], &info);
        if (!result) set_info(work, &info);
        emit(work, result ? BINBOT_COMPONENT_FAILED : BINBOT_COMPONENT_SUCCEEDED,
             result ? client.error[0] ? client.error : "component changed since control selection" : "motor state confirmed");
        goto done;
    }
    if (work->record.command.action == BINBOT_COMPONENT_FILE_UPDATE) {
        if(slave_target) { emit(work, BINBOT_COMPONENT_FAILED, "unsupported slave action"); goto done; }
        if (result) {
            emit(work, BINBOT_COMPONENT_FAILED, client.error);
            goto done;
        }
        if (!info.healthy || info.profile == BB_COMPONENT_SHOWCASE_TURNTABLE) {
            emit(work, BINBOT_COMPONENT_FAILED, "component does not provide a display asset");
            goto done;
        }
        if (work->record.command.reserved[0] && work->record.command.reserved[0] != info.profile) {
            emit(work, BINBOT_COMPONENT_FAILED, "component changed since image selection; select the inserted component");
            goto done;
        }
        if (cancelled(work)) {
            emit(work, BINBOT_COMPONENT_CANCELLED, "cancelled before GIF transfer");
            goto done;
        }
        char asset[320];
        path_for(asset, sizeof(asset), work->directory, "component.gif");
        if (check_file(work, asset)) result = download(work, asset);
        if (!result) {
            result = bb_component_client_file_update_ex(
                &client, work->record.command.file_name, asset, progress,
                file_phase, work);
        }
        if (!result) {
            emit(work, BINBOT_COMPONENT_SUCCEEDED, "display asset replaced");
        } else {
            fprintf(stderr, "component GIF update: result=%d error=%s\n",
                    result, file_failure_message(result, client.error));
            emit(work, result == -ECANCELED ? BINBOT_COMPONENT_CANCELLED : BINBOT_COMPONENT_FAILED,
                 file_failure_message(result, client.error));
        }
        goto done;
    }
    if (work->record.activated) {
        if (!result && info.boot_id != work->record.command.before_boot_id && info.healthy) {
            emit(work, matches(work, &info) ? BINBOT_COMPONENT_SUCCEEDED : BINBOT_COMPONENT_FAILED,
                 matches(work, &info) ? "healthy target boot confirmed" : "another healthy image booted (rollback)");
        } else {
            emit(work, BINBOT_COMPONENT_RECONCILING, "activation intent saved; waiting for a healthy new boot");
        }
        goto done;
    }
    if (result) { emit(work, BINBOT_COMPONENT_FAILED, client.error); goto done; }
    char before_elf[65];
    hex_digest(before_elf, info.elf_sha256);
    if (!info.healthy || strcmp(info.version, work->record.command.before_version) ||
        strcmp(before_elf, work->record.command.before_elf_sha256)) {
        emit(work, BINBOT_COMPONENT_FAILED, "component changed since Server preflight; query and start a new job");
        goto done;
    }
    work->record.command.before_boot_id = info.boot_id;
    if (!work->record.command.allow_downgrade &&
        strtoul(work->record.command.version, NULL, 10) < strtoul(info.version, NULL, 10)) {
        emit(work, BINBOT_COMPONENT_FAILED, "downgrade rejected by worker");
        goto done;
    }
    if (cancelled(work)) {
        result = slave_target ? bb_slave_ota_abort(&slave, client.error) : bb_component_client_abort(&client);
        emit(work, result ? BINBOT_COMPONENT_FAILED : BINBOT_COMPONENT_CANCELLED,
             result ? client.error : "cancelled before activation");
        goto done;
    }
    char firmware[320];
    path_for(firmware, sizeof(firmware), work->directory, "firmware.bin");
    if (check_file(work, firmware)) result = download(work, firmware);
    if (result == -ECANCELED) {
        const int aborted = slave_target ? bb_slave_ota_abort(&slave, client.error) : bb_component_client_abort(&client);
        if (aborted) result = aborted;
    }
    if (!result) result = slave_target ? bb_slave_ota_update(&slave, firmware, progress, phase, work, client.error) :
                                        bb_component_client_ota_ex(&client, firmware, false, progress, phase, work);
    if (!result) {
        result = slave_target ? bb_slave_ota_status(&slave, &info, client.error) : bb_component_client_status(&client, &info);
        if (!result) set_info(work, &info);
    }
    if (!result) emit(work, BINBOT_COMPONENT_SUCCEEDED, "healthy target boot confirmed");
    else if (work->record.activated && result < 0) emit(work, BINBOT_COMPONENT_RECONCILING, client.error);
    else emit(work, result == -ECANCELED ? BINBOT_COMPONENT_CANCELLED : BINBOT_COMPONENT_FAILED,
              result == -ECANCELED ? "cancelled before activation" : client.error[0] ? client.error : "download or firmware verification failed");
done:
    if(slave_target) binbot_slave_client_close(&slave);
    else bb_component_client_close(&client);
}

static int run_worker(int fd, const char *directory, const binbot_component_command_t *command)
{
    char path[320];
    path_for(path, sizeof(path), directory, "worker.lock");
    int lock = open(path, O_RDWR | O_CREAT, 0600);
    if(lock < 0) return 2;
    if(flock(lock, LOCK_EX | LOCK_NB)) { close(lock); return 2; }
    work_t work = {.directory = directory, .fd = fd};
    const bool valid_record = read_record(directory, &work.record);
    path_for(path, sizeof(path), directory, "job.state");
    const bool corrupt_record = !valid_record && access(path, F_OK) == 0;
    if((command->action != BINBOT_COMPONENT_OTA && command->action != BINBOT_COMPONENT_FILE_UPDATE) || !valid_record ||
       memcmp(work.record.command.request_id, command->request_id, 16)) {
        memset(&work.record, 0, sizeof(work.record));
        work.record.magic = 0x42434f32;
        work.record.size = sizeof(work.record);
        work.record.command = *command;
        work.record.activated = command->action == BINBOT_COMPONENT_OTA &&
                                (command->reconcile_only || corrupt_record);
        memcpy(work.record.event.request_id, command->request_id, 16);
        work.record.event.total_bytes = command->size;
    }
    if(command->reserved[3] == 1) work.record.event.profile = 4;
    int result = 0;
    if((command->action == BINBOT_COMPONENT_OTA || command->action == BINBOT_COMPONENT_FILE_UPDATE) &&
       !emit(&work, work.record.activated ? BINBOT_COMPONENT_RECONCILING : BINBOT_COMPONENT_PREPARING, "")) result = 2;
    else run(&work);
    close(lock);
    return result;
}

int bb_component_worker_main(int argc, char **argv)
{
    if(argc != 4 || strcmp(argv[1], "--component-worker")) return 2;
    char *end;
    errno = 0;
    long number = strtol(argv[2], &end, 10);
    if(errno || end == argv[2] || *end || number < 3 || number > INT_MAX || !argv[3][0]) return 2;
    int fd = (int)number;
    binbot_component_command_t command;
    ssize_t count;
    do { count = recv(fd, &command, sizeof(command), MSG_TRUNC); } while(count < 0 && errno == EINTR);
    if(count != sizeof(command)) { close(fd); return 2; }
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    int result = run_worker(fd, argv[3], &command);
    close(fd);
    return result;
}

static bool launch(bb_component_worker_t *worker, const binbot_component_command_t *command)
{
    bb_component_client_close(&worker->listener);
    worker->listen_retry_ms = 0;
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets)) return false;
    const long limit = sysconf(_SC_OPEN_MAX);
    char descriptor[24];
    snprintf(descriptor, sizeof(descriptor), "%d", sockets[1]);
    char *arguments[] = {"binbot_app", "--component-worker", descriptor, worker->directory, NULL};
    pid_t pid = fork();
    if (pid < 0) { close(sockets[0]); close(sockets[1]); return false; }
    if (!pid) {
        (void)setpgid(0, 0);
        /* Component probing and OTA are background work.  RV1106 has one CPU
         * core, so keep a probe from delaying LVGL and audio delivery. */
        (void)setpriority(PRIO_PROCESS, 0, 10);
        close(sockets[0]);
        /* Do not keep GUI, audio or network descriptors alive in the worker. */
        for (int fd = 3; fd < limit; ++fd) if (fd != sockets[1]) close(fd);
        /* exec drops inherited camera/NPU mappings and locks from other threads. */
        if(worker->reexec) {
            execv("/proc/self/exe", arguments);
            _exit(127);
        }
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        _exit(run_worker(sockets[1], worker->directory, command));
    }
    (void)setpgid(pid, pid);
    close(sockets[1]);
    ssize_t sent = 0;
    if(worker->reexec) {
        do { sent = send(sockets[0], command, sizeof(*command), MSG_NOSIGNAL); } while(sent < 0 && errno == EINTR);
    }
    if(worker->reexec && sent != sizeof(*command)) {
        kill(pid, SIGTERM);
        while(waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
        close(sockets[0]);
        return false;
    }
    if (worker->fd >= 0) close(worker->fd);
    worker->fd = sockets[0];
    worker->pid = pid;
    worker->active_action = command->action;
    return true;
}

static void initialize_worker(bb_component_worker_t *worker, bool reexec)
{
    memset(worker, 0, sizeof(*worker));
    worker->reexec = reexec;
    worker->fd = -1;
    worker->listener.fd = -1;
    const char *directory = getenv("BINBOT_COMPONENT_STATE_DIR");
    snprintf(worker->directory, sizeof(worker->directory), "%s", directory && *directory ? directory : "/oem/data/component-ota");
    char parent[256];
    snprintf(parent, sizeof(parent), "%s", worker->directory);
    for (char *p = parent + 1; *p; ++p) {
        if (*p == '/') { *p = '\0'; mkdir(parent, 0700); *p = '/'; }
    }
    mkdir(parent, 0700);
    worker_record_t saved;
    if (read_record(worker->directory, &saved)) {
        worker->latest = saved.event;
        worker->active_action = saved.command.action;
        worker->pending = true;
        if (saved.event.status < BINBOT_COMPONENT_SUCCEEDED) (void)launch(worker, &saved.command);
    }
}

void bb_component_worker_init(bb_component_worker_t *worker)
{
    initialize_worker(worker, false);
}

void bb_component_worker_init_exec(bb_component_worker_t *worker)
{
    initialize_worker(worker, true);
}

bool bb_component_worker_submit(bb_component_worker_t *worker,
                                 const binbot_component_command_t *command)
{
    if (command->cancel) {
        char path[320];
        path_for(path, sizeof(path), worker->directory, "cancel");
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0) return false;
        bool ok = write(fd, command->request_id, 16) == 16 && fsync(fd) == 0;
        close(fd);
        fd = open(worker->directory, O_RDONLY | O_DIRECTORY);
        ok = ok && fd >= 0 && fsync(fd) == 0;
        if (fd >= 0) close(fd);
        if (!ok) return false;
    }
    if (worker->pid > 0 && !memcmp(worker->latest.request_id, command->request_id, 16)) return true;
    if (worker->command_pending) {
        if (memcmp(worker->pending_command.request_id, command->request_id, 16)) return false;
        worker->pending_command = *command;
        return true;
    }
    if (worker->pid > 0) {
        if (worker->active_action != BINBOT_COMPONENT_PROBE) return false;
        worker->pending_command = *command;
        worker->command_pending = true;
        return true;
    }
    worker_record_t saved;
    if ((command->action == BINBOT_COMPONENT_OTA || command->action == BINBOT_COMPONENT_FILE_UPDATE) && read_record(worker->directory, &saved) &&
        !memcmp(saved.command.request_id, command->request_id, 16) && saved.event.status >= BINBOT_COMPONENT_SUCCEEDED) {
        worker->latest = saved.event;
        worker->pending = true;
        return true;
    }
    memset(&worker->latest, 0, sizeof(worker->latest));
    memcpy(worker->latest.request_id, command->request_id, 16);
    return launch(worker, command);
}

bool bb_component_worker_poll(bb_component_worker_t *worker,
                               binbot_component_event_t *event, int64_t now_ms)
{
    if (worker->pid == 0 && worker->command_pending) {
        binbot_component_command_t command = worker->pending_command;
        if (!launch(worker, &command)) return false;
        worker->command_pending = false;
        memset(&worker->latest, 0, sizeof(worker->latest));
        memcpy(worker->latest.request_id, command.request_id, 16);
        worker->pending = false;
    }
    if (worker->fd >= 0) {
        binbot_component_event_t incoming;
        while (recv(worker->fd, &incoming, sizeof(incoming), MSG_DONTWAIT) == sizeof(incoming)) {
            worker->latest = incoming;
            worker->pending = true;
        }
    }
    if (worker->pid > 0 && waitpid(worker->pid, NULL, WNOHANG) == worker->pid) {
        worker->pid = 0;
        worker_record_t saved;
        if (read_record(worker->directory, &saved) && !memcmp(saved.event.request_id, worker->latest.request_id, 16))
            worker->latest = saved.event;
        worker->pending = true;
    }
    /* A terminal event permits the Server to submit the next probe/update.
     * Publish it only after this worker has exited and released UART/locks. */
    if (worker->pid > 0 && worker->latest.status >= BINBOT_COMPONENT_SUCCEEDED)
        return false;
    if (worker->latest.event_seq && (worker->pending || now_ms - worker->last_send_ms >= 2000)) {
        *event = worker->latest;
        worker->pending = false;
        worker->last_send_ms = now_ms;
        return true;
    }
    return false;
}

bool bb_component_worker_receive_announcement(bb_component_worker_t *worker,
                                               binbot_component_event_t *event,
                                               int64_t now_ms)
{
    if (worker->pid > 0 || worker->command_pending) return false;
    if (worker->listener.fd < 0) {
        if (now_ms < worker->listen_retry_ms) return false;
        worker->listen_retry_ms = now_ms + 1000;
        const char *uart = getenv("BINBOT_COMPONENT_UART");
        if (bb_component_client_open(&worker->listener, uart && *uart ? uart : BB_COMPONENT_DEFAULT_UART))
            return false;
        worker->listen_retry_ms = 0;
    }
    bb_component_info_t info;
    int received = bb_component_client_receive_announcement(&worker->listener, &info);
    if (received < 0) {
        bb_component_client_close(&worker->listener);
        worker->listen_retry_ms = now_ms + 1000;
        return false;
    }
    if (!received) return false;
    /* Keep announcements separate from the job result used for OTA reconciliation. */
    work_t notice = {0};
    set_info(&notice, &info);
    notice.record.event.status = BINBOT_COMPONENT_PROBED;
    notice.record.event.event_seq = ++worker->announcement_sequence;
    memcpy(notice.record.event.request_id, "announce", 8);
    bb_component_put_u64(notice.record.event.request_id + 8, info.boot_id);
    *event = notice.record.event;
    return true;
}

void bb_component_worker_close(bb_component_worker_t *worker)
{
    bb_component_client_close(&worker->listener);
    if (worker->pid > 0) {
        if (kill(-worker->pid, SIGTERM)) kill(worker->pid, SIGTERM);
        (void)waitpid(worker->pid, NULL, 0);
    }
    if (worker->fd >= 0) close(worker->fd);
    worker->fd = -1;
    worker->pid = 0;
    worker->command_pending = false;
}
