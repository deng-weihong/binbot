#define _DEFAULT_SOURCE
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "binbot/component/client.h"
#include "binbot/component/file_receiver.h"
#include "binbot/component/ota_receiver.h"
#include "binbot/component/worker.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <poll.h>
#include <pthread.h>
#include <pty.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <unistd.h>

#define IMAGE_BYTES 8195
static bool s_app_entry;

typedef struct {
    int master;
    int slave;
    atomic_bool stop;
    bb_component_receiver_t receiver;
    bb_component_file_receiver_t files;
    uint8_t profile;
    uint8_t flash[2 * 1024 * 1024];
    size_t written;
    unsigned begins, writes, finishes, activations, aborts;
    unsigned drop_data;
    bool corrupt_hello;
    bool drop_activation;
    bool rebooted;
    bool wrong_boot;
    bool slow_data;
    bool motor_supported, motor_busy, motor_bad_reply;
    uint8_t motor_direction, motor_speed;
    unsigned motor_sets;
    bool light_supported, light_busy;
    unsigned light_bad_reply, light_sets;
    bb_component_light_settings_t light;
} peer_t;

static uint16_t begin(void *p, uint32_t total)
{
    peer_t *peer = p;
    assert(total <= sizeof(peer->flash));
    ++peer->begins;
    peer->written = 0;
    memset(peer->flash, 0xff, sizeof(peer->flash));
    return BB_COMPONENT_OK;
}

static uint16_t write_chunk(void *p, const uint8_t *bytes, size_t n)
{
    peer_t *peer = p;
    assert(peer->written + n <= sizeof(peer->flash));
    memcpy(peer->flash + peer->written, bytes, n);
    peer->written += n;
    ++peer->writes;
    return BB_COMPONENT_OK;
}

static uint16_t finish(void *p, const uint8_t expected[32])
{
    peer_t *peer = p;
    uint8_t actual[32];
    unsigned length;
    ++peer->finishes;
    assert(EVP_Digest(peer->flash, peer->written, actual, &length, EVP_sha256(), NULL));
    assert(length == 32);
    return memcmp(actual, expected, 32) == 0 ? BB_COMPONENT_OK : BB_COMPONENT_HASH;
}

static void abort_transfer(void *p) { ++((peer_t *)p)->aborts; }
static uint16_t activate(void *p)
{
    peer_t *peer = p;
    ++peer->activations;
    peer->rebooted = true;
    return BB_COMPONENT_OK;
}

static void make_image(uint8_t *image)
{
    for (size_t i = 0; i < IMAGE_BYTES; ++i) image[i] = (uint8_t)(i * 13);
    image[0] = 0xe9;
    bb_component_put_u16(image + 12, 5);
    bb_component_put_u32(image + 32, 0xabcd5432);
    memset(image + 48, 0, 64);
    strcpy((char *)image + 48, "25");
    strcpy((char *)image + 80, bb_component_project(BB_COMPONENT_ROUND_SCREEN));
}

static void emit(peer_t *peer, bb_component_frame_t *reply, bool corrupt)
{
    uint8_t wire[BB_COMPONENT_FRAME_BYTES];
    size_t length = bb_component_encode(reply, wire, sizeof(wire));
    if (corrupt) wire[length - 1] ^= 1;
    size_t sent = 0;
    while (sent < length) {
        ssize_t n = write(peer->master, wire + sent, length - sent);
        if (n < 0 && errno == EINTR) continue;
        assert(n > 0);
        sent += (size_t)n;
    }
}

static void handle(peer_t *peer, const bb_component_frame_t *request)
{
    if (request->type != BB_COMPONENT_REQUEST) return;
    bb_component_frame_t reply = {0};
    reply.type = BB_COMPONENT_RESPONSE;
    reply.sequence = request->sequence;
    reply.command = request->command;
    size_t n = 0;
    uint16_t status = BB_COMPONENT_OK;
    if (request->command == BB_COMPONENT_HELLO) {
        uint8_t *p = reply.payload + 2;
        p[0] = peer->profile ? peer->profile : BB_COMPONENT_ROUND_SCREEN;
        p[1] = peer->rebooted ? BB_COMPONENT_IDLE : peer->receiver.state;
        p[2] = 1;
        p[3] = (peer->motor_supported ? BB_COMPONENT_CAP_MOTOR : 0) |
               (peer->light_supported ? BB_COMPONENT_CAP_LIGHT : 0);
        bb_component_put_u64(p + 4, peer->rebooted ? 2 : 1);
        bb_component_put_u32(p + 12, peer->receiver.capacity);
        bb_component_put_u32(p + 16, peer->receiver.received);
        bb_component_put_u32(p + 20, peer->receiver.total);
        bb_component_put_u64(p + 24, peer->receiver.transfer_id);
        strcpy((char *)p + 32, peer->rebooted ? (char *)peer->flash + 48 : "24");
        strcpy((char *)p + 64, bb_component_project(p[0]));
        if (peer->rebooted) memcpy(p + 96, peer->flash + 176, 32);
        if (peer->wrong_boot && peer->rebooted) p[96] ^= 1;
        n = BB_COMPONENT_INFO_BYTES;
    } else if (request->command == BB_COMPONENT_MOTOR_GET || request->command == BB_COMPONENT_MOTOR_SET) {
        bool setting = request->command == BB_COMPONENT_MOTOR_SET;
        if (!peer->motor_supported) status = BB_COMPONENT_UNSUPPORTED;
        else if (request->length != (setting ? 2 : 0) ||
                 (setting && (request->payload[0] > 2 || request->payload[1] > 100)))
            status = BB_COMPONENT_BAD_REQUEST;
        else if (setting && peer->motor_busy) status = BB_COMPONENT_BUSY;
        else {
            if (setting) {
                peer->motor_direction = request->payload[1] ? request->payload[0] : 0;
                peer->motor_speed = request->payload[1];
                ++peer->motor_sets;
            }
            reply.payload[2] = peer->motor_direction;
            reply.payload[3] = peer->motor_speed;
            n = peer->motor_bad_reply ? 1 : BB_COMPONENT_MOTOR_BYTES;
        }
    } else if(request->command == BB_COMPONENT_LIGHT_GET || request->command == BB_COMPONENT_LIGHT_SET) {
        bool setting = request->command == BB_COMPONENT_LIGHT_SET;
        bb_component_light_settings_t next;
        if(!peer->light_supported) status = BB_COMPONENT_UNSUPPORTED;
        else if(request->length != (setting ? BB_COMPONENT_LIGHT_BYTES : 0) ||
                (setting && !bb_component_light_decode(request->payload, request->length, &next)))
            status = BB_COMPONENT_BAD_REQUEST;
        else if(setting && peer->light_busy) status = BB_COMPONENT_BUSY;
        else {
            if(setting) { peer->light = next; ++peer->light_sets; }
            bb_component_light_encode(&peer->light, reply.payload + 2);
            bb_component_put_u16(reply.payload + 2 + BB_COMPONENT_LIGHT_BYTES,
                                peer->light.enabled ? peer->light.timer_minutes : 0);
            n = BB_COMPONENT_LIGHT_STATE_BYTES;
            if(peer->light_bad_reply == 1) --n;
            if(peer->light_bad_reply == 2) reply.payload[2 + 6] = 0;
            if(peer->light_bad_reply == 3)
                bb_component_put_u16(reply.payload + 2 + BB_COMPONENT_LIGHT_BYTES, 121);
        }
    } else if (request->command >= BB_COMPONENT_FILE_BEGIN &&
               request->command <= BB_COMPONENT_FILE_ABORT) {
        status = bb_component_file_receiver_command(&peer->files, request->command,
                                                    request->payload, request->length,
                                                    reply.payload + 2, &n);
    } else {
        status = bb_component_receiver_command(&peer->receiver, request->command,
                                               request->payload, request->length,
                                               reply.payload + 2, &n);
    }
    reply.length = (uint16_t)(2 + n);
    if (peer->slow_data && request->command == BB_COMPONENT_TRANSFER_DATA) usleep(1000);
    bb_component_put_u16(reply.payload, status);
    if (request->command == BB_COMPONENT_TRANSFER_DATA && peer->drop_data &&
        bb_component_get_u32(request->payload + 8) == 512) {
        --peer->drop_data;
        return;
    }
    if (request->command == BB_COMPONENT_OTA_ACTIVATE && peer->drop_activation) {
        peer->drop_activation = false;
        return;
    }
    if (request->command == BB_COMPONENT_HELLO && peer->corrupt_hello) {
        peer->corrupt_hello = false;
        emit(peer, &reply, true);
        return;
    }
    /* Ignore unrelated replies even when they arrive before the matching ACK. */
    --reply.sequence;
    emit(peer, &reply, false);
    ++reply.sequence;
    emit(peer, &reply, false);
}

static void *peer_thread(void *context)
{
    peer_t *peer = context;
    bb_component_parser_t parser = {0};
    bb_component_frame_t request;
    while (!atomic_load(&peer->stop)) {
        struct pollfd fd = {peer->master, POLLIN, 0};
        int result = poll(&fd, 1, 20);
        if (result < 0 && errno == EINTR) continue;
        assert(result >= 0);
        if (!result) continue;
        uint8_t bytes[256];
        ssize_t count = read(peer->master, bytes, sizeof(bytes));
        assert(count > 0);
        struct termios config;
        assert(tcgetattr(peer->slave, &config) == 0);
        /* PTYs do not emulate baud timing. Enforce the agreed rate before the
         * emulator processes any bytes from the actual CLI or worker. */
        assert(cfgetispeed(&config) == B460800 && cfgetospeed(&config) == B460800);
        for (ssize_t i = 0; i < count; ++i)
            if (bb_component_parser_push(&parser, bytes[i], &request)) handle(peer, &request);
    }
    return NULL;
}

static int run_tool(const char *tool, const char *uart, const char *command,
                     const char *firmware, bool stage)
{
    pid_t pid = fork();
    assert(pid >= 0);
    if (!pid) {
        const char *args[10];
        size_t n = 0;
        args[n++] = tool;
        if (s_app_entry) args[n++] = "--component";
        args[n++] = "--uart";
        args[n++] = uart;
        if (stage) args[n++] = "--stage-only";
        args[n++] = command;
        if (firmware) args[n++] = firmware;
        args[n] = NULL;
        execv(tool, (char *const *)args);
        _exit(127);
    }
    int status;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status));
    return WEXITSTATUS(status);
}

static void test_bad_hash(const uint8_t *image)
{
    peer_t peer = {0};
    bb_component_ota_ops_t ops = {begin, write_chunk, finish, abort_transfer, activate};
    bb_component_receiver_init(&peer.receiver, BB_COMPONENT_ROUND_SCREEN, IMAGE_BYTES, &ops, &peer);
    uint8_t payload[524] = {0}, reply[128];
    size_t reply_n;
    bb_component_put_u64(payload, 7);
    payload[8] = 1;
    payload[9] = BB_COMPONENT_ROUND_SCREEN;
    bb_component_put_u32(payload + 12, IMAGE_BYTES);
    assert(bb_component_receiver_command(&peer.receiver, BB_COMPONENT_TRANSFER_BEGIN,
                                          payload, 48, reply, &reply_n) == BB_COMPONENT_OK);
    for (size_t offset = 0; offset < IMAGE_BYTES; offset += BB_COMPONENT_CHUNK_BYTES) {
        size_t count = IMAGE_BYTES - offset;
        if (count > BB_COMPONENT_CHUNK_BYTES) count = BB_COMPONENT_CHUNK_BYTES;
        bb_component_put_u32(payload + 8, (uint32_t)offset);
        memcpy(payload + 12, image + offset, count);
        assert(bb_component_receiver_command(&peer.receiver, BB_COMPONENT_TRANSFER_DATA,
                                              payload, 12 + count, reply, &reply_n) == BB_COMPONENT_OK);
    }
    assert(bb_component_receiver_command(&peer.receiver, BB_COMPONENT_TRANSFER_COMMIT,
                                          payload, 8, reply, &reply_n) == BB_COMPONENT_HASH);
    assert(peer.activations == 0 && peer.aborts == 1 && peer.receiver.state == BB_COMPONENT_IDLE);
    assert(bb_component_receiver_command(&peer.receiver, BB_COMPONENT_OTA_ACTIVATE,
                                          payload, 8, reply, &reply_n) == BB_COMPONENT_NOT_READY);
}

/* Binary stdin/stdout bridge lets Python exercise the production worker through
 * a real HTTP download and PTY running the production ESP receiver state machine. */
static int bridge(void)
{
    peer_t peer = {0};
    peer.slow_data = true;
    bb_component_ota_ops_t ops = {begin, write_chunk, finish, abort_transfer, activate};
    bb_component_receiver_init(&peer.receiver, BB_COMPONENT_ROUND_SCREEN, sizeof(peer.flash), &ops, &peer);
    int slave;
    char uart[128];
    assert(openpty(&peer.master, &slave, uart, NULL, NULL) == 0);
    peer.slave = slave;
    struct termios config;
    assert(tcgetattr(slave, &config) == 0);
    cfmakeraw(&config);
    assert(cfsetispeed(&config, B460800) == 0 && cfsetospeed(&config, B460800) == 0);
    assert(tcsetattr(slave, TCSANOW, &config) == 0);
    pid_t emulator = fork();
    assert(emulator >= 0);
    if (!emulator) { (void)peer_thread(&peer); _exit(0); }
    setenv("BINBOT_COMPONENT_UART", uart, 1);
    bb_component_worker_t worker;
    bb_component_worker_init(&worker);
    for (int64_t tick = 0; ; tick += 20) {
        struct pollfd fd = {STDIN_FILENO, POLLIN, 0};
        if (poll(&fd, 1, 20) > 0) {
            binbot_component_command_t command;
            ssize_t n = read(STDIN_FILENO, &command, sizeof(command));
            if (!n) break;
            assert(n == sizeof(command));
            if (command.action == 3) {
                bb_component_worker_close(&worker);
                bb_component_worker_init(&worker);
                continue;
            }
            if (command.action == 4) {
                assert(kill(emulator, SIGSTOP) == 0);
                continue;
            }
            if (command.action == 5) {
                assert(kill(emulator, SIGCONT) == 0);
                bb_component_frame_t notice = {.type = BB_COMPONENT_EVENT, .sequence = 1,
                    .command = BB_COMPONENT_ANNOUNCE, .length = BB_COMPONENT_ANNOUNCE_BYTES};
                notice.payload[0] = BB_COMPONENT_ROUND_SCREEN;
                notice.payload[2] = 1;
                bb_component_put_u64(notice.payload + 4, 1);
                bb_component_put_u32(notice.payload + 12, sizeof(peer.flash));
                strcpy((char *)notice.payload + 32, "24");
                strcpy((char *)notice.payload + 64, bb_component_project(BB_COMPONENT_ROUND_SCREEN));
                emit(&peer, &notice, false);
                continue;
            }
            if (!bb_component_worker_submit(&worker, &command) && command.action == BINBOT_COMPONENT_PROBE) {
                binbot_component_event_t event = {0};
                memcpy(event.request_id, command.request_id, 16);
                event.event_seq = 1;
                event.status = BINBOT_COMPONENT_PROBED;
                strcpy(event.message, "worker busy");
                assert(write(STDOUT_FILENO, &event, sizeof(event)) == sizeof(event));
            }
        }
        binbot_component_event_t event;
        if (bb_component_worker_poll(&worker, &event, tick))
            assert(write(STDOUT_FILENO, &event, sizeof(event)) == sizeof(event));
        if (bb_component_worker_receive_announcement(&worker, &event, tick))
            assert(write(STDOUT_FILENO, &event, sizeof(event)) == sizeof(event));
    }
    bb_component_worker_close(&worker);
    kill(emulator, SIGCONT);
    kill(emulator, SIGTERM);
    waitpid(emulator, NULL, 0);
    close(slave);
    close(peer.master);
    return 0;
}

static int cancel_after_commit(bb_component_phase_t phase, void *context)
{
    (void)context;
    return phase == BB_COMPONENT_PHASE_ACTIVATING;
}

static void test_cancel(const char *firmware)
{
    peer_t peer = {0};
    bb_component_ota_ops_t ops = {begin, write_chunk, finish, abort_transfer, activate};
    bb_component_receiver_init(&peer.receiver, BB_COMPONENT_ROUND_SCREEN, IMAGE_BYTES, &ops, &peer);
    int slave;
    char uart[128];
    assert(openpty(&peer.master, &slave, uart, NULL, NULL) == 0);
    peer.slave = slave;
    pthread_t thread;
    assert(pthread_create(&thread, NULL, peer_thread, &peer) == 0);
    bb_component_client_t client;
    assert(bb_component_client_open(&client, uart) == 0);
    assert(bb_component_client_ota_ex(&client, firmware, false, NULL, cancel_after_commit, NULL) == -ECANCELED);
    bb_component_client_close(&client);
    atomic_store(&peer.stop, true);
    assert(pthread_join(thread, NULL) == 0);
    assert(peer.finishes == 1 && peer.activations == 0 && peer.aborts == 1);
    close(slave);
    close(peer.master);
}

static uint16_t file_begin(void *context, uint8_t file_id, uint32_t total,
                            const uint8_t expected[32])
{
    (void)expected;
    assert(file_id == BB_COMPONENT_FILE_DISPLAY_GIF);
    return begin(context, total);
}

static uint16_t file_finish(void *context, uint8_t file_id, const uint8_t expected[32])
{
    peer_t *peer = context;
    assert(file_id == BB_COMPONENT_FILE_DISPLAY_GIF);
    uint16_t width = peer->profile == BB_COMPONENT_ROUND_SCREEN ? 240 : 320;
    assert(bb_component_get_u16(peer->flash + 6) == width);
    assert(bb_component_get_u16(peer->flash + 8) == 240);
    return finish(context, expected);
}

static void test_file_update(void)
{
    uint8_t gif[1027];
    for (size_t i = 0; i < sizeof(gif); ++i) gif[i] = (uint8_t)i;
    memcpy(gif, "GIF89a", 6);
    char path[] = "/tmp/binbot-uart-gif-XXXXXX";
    int file = mkstemp(path);
    assert(file >= 0);
    for (uint8_t profile = BB_COMPONENT_ROUND_SCREEN; profile <= BB_COMPONENT_SHOWCASE_TURNTABLE; ++profile) {
        peer_t peer = {.profile = profile};
        bb_component_file_ops_t ops = {file_begin, write_chunk, file_finish, abort_transfer};
        bb_component_file_receiver_init(&peer.files, &ops, &peer);
        char uart[128];
        assert(openpty(&peer.master, &peer.slave, uart, NULL, NULL) == 0);
        pthread_t thread;
        assert(pthread_create(&thread, NULL, peer_thread, &peer) == 0);
        bb_component_client_t client;
        assert(bb_component_client_open(&client, uart) == 0);
        uint16_t width = profile == BB_COMPONENT_ROUND_SCREEN ? 240 : 320;
        bb_component_put_u16(gif + 6, width);
        bb_component_put_u16(gif + 8, 240);
        assert(pwrite(file, gif, sizeof(gif), 0) == sizeof(gif));
        int result = bb_component_client_file_update(&client, "display.gif", path, NULL, NULL);
        if (profile == BB_COMPONENT_SHOWCASE_TURNTABLE) {
            assert(result == -ENOTSUP && peer.begins == 0);
        } else {
            assert(result == 0);
            assert(peer.begins == 1 && peer.finishes == 1 && peer.written == sizeof(gif));
            assert(memcmp(peer.flash, gif, sizeof(gif)) == 0);
            width = width == 240 ? 320 : 132;
            bb_component_put_u16(gif + 6, width);
            bb_component_put_u16(gif + 8, width == 132 ? 132 : 240);
            assert(pwrite(file, gif, sizeof(gif), 0) == sizeof(gif));
            assert(bb_component_client_file_update(&client, "display.gif", path, NULL, NULL) == -EINVAL);
            assert(peer.begins == 1 && peer.finishes == 1);
        }
        bb_component_client_close(&client);
        atomic_store(&peer.stop, true);
        assert(pthread_join(thread, NULL) == 0);
        close(peer.slave);
        close(peer.master);
    }
    close(file);
    unlink(path);
}

static void test_motor_worker(void)
{
    struct rlimit original, limited;
    assert(!getrlimit(RLIMIT_NOFILE, &original));
    limited = original;
    if(limited.rlim_cur > 1024) limited.rlim_cur = 1024;
    assert(!setrlimit(RLIMIT_NOFILE, &limited));
    char directory[] = "/tmp/binbot-motor-worker-XXXXXX"; assert(mkdtemp(directory));
    char record[256]; snprintf(record, sizeof(record), "%s/job.state", directory);
    FILE *file = fopen(record, "wb"); assert(file);
    assert(fwrite("previous OTA record", 1, 19, file) == 19); fclose(file);
    peer_t peer = {.profile = BB_COMPONENT_SHOWCASE_TURNTABLE,
                   .motor_supported = true, .motor_speed = 40};
    char uart[128]; assert(openpty(&peer.master, &peer.slave, uart, NULL, NULL) == 0);
    pthread_t thread; assert(pthread_create(&thread, NULL, peer_thread, &peer) == 0);
    setenv("BINBOT_COMPONENT_STATE_DIR", directory, 1);
    setenv("BINBOT_COMPONENT_UART", uart, 1);
    bb_component_worker_t worker; bb_component_worker_init(&worker);
    for(unsigned i = 0; i < 3; ++i) {
        binbot_component_command_t command = {.action = BINBOT_COMPONENT_MOTOR_CONTROL};
        command.request_id[0] = (uint8_t)(i + 1);
        command.reserved[0] = BB_COMPONENT_SHOWCASE_TURNTABLE;
        command.reserved[1] = i == 0 ? BB_COMPONENT_MOTOR_CCW : i == 1 ? BB_COMPONENT_MOTOR_CW : BB_COMPONENT_MOTOR_STOP;
        command.reserved[2] = (uint8_t)(25 + i * 25);
        assert(bb_component_worker_submit(&worker, &command));
        binbot_component_event_t event = {0};
        bool received = false;
        for(unsigned tick = 0; tick < 300 && !received; ++tick) {
            received = bb_component_worker_poll(&worker, &event, tick * 10);
            if(!received) usleep(10000);
        }
        if(!received || worker.pid != 0 || event.status != BINBOT_COMPONENT_SUCCEEDED)
            fprintf(stderr, "motor worker: received=%d pid=%ld status=%u seq=%llu message=%s sets=%u\n",
                    received, (long)worker.pid, event.status, (unsigned long long)event.event_seq,
                    event.message, peer.motor_sets);
        assert(received && worker.pid == 0 && event.status == BINBOT_COMPONENT_SUCCEEDED);
        assert(!memcmp(event.request_id, command.request_id, 16));
        assert(event.has_info && event.motor_supported && event.motor_direction == command.reserved[1]);
        assert(event.motor_speed == command.reserved[2]);
    }
    bb_component_worker_close(&worker);
    atomic_store(&peer.stop, true); assert(pthread_join(thread, NULL) == 0);
    assert(peer.motor_sets == 3);
    close(peer.slave); close(peer.master);
    file = fopen(record, "rb"); assert(file);
    char bytes[20] = {0}; assert(fread(bytes, 1, sizeof(bytes), file) == 19); fclose(file);
    assert(!strcmp(bytes, "previous OTA record"));
    unlink(record);
    snprintf(record, sizeof(record), "%s/worker.lock", directory); unlink(record);
    assert(!rmdir(directory));
    unsetenv("BINBOT_COMPONENT_STATE_DIR"); unsetenv("BINBOT_COMPONENT_UART");
    assert(!setrlimit(RLIMIT_NOFILE, &original));
}

static void test_motor(void)
{
    for (unsigned variant = 0; variant < 4; ++variant) {
        peer_t peer = {.profile = BB_COMPONENT_SHOWCASE_TURNTABLE,
                       .motor_supported = variant != 0, .motor_speed = 40,
                       .motor_busy = variant == 2, .motor_bad_reply = variant == 3};
        char uart[128];
        assert(openpty(&peer.master, &peer.slave, uart, NULL, NULL) == 0);
        pthread_t thread;
        assert(pthread_create(&thread, NULL, peer_thread, &peer) == 0);
        bb_component_client_t client;
        bb_component_info_t info;
        assert(bb_component_client_open(&client, uart) == 0);
        int result = bb_component_client_status(&client, &info);
        if (variant == 3) assert(result == -EPROTO);
        else {
            assert(result == 0 && info.motor_supported == (variant != 0));
            assert(bb_component_client_motor_set(&client, 3, 40, &info) == -EINVAL);
            assert(bb_component_client_motor_set(&client, 1, 101, &info) == -EINVAL);
            result = bb_component_client_motor_set(&client, BB_COMPONENT_MOTOR_CCW, 25, &info);
            if (variant == 0) assert(result == -ENOTSUP);
            else if (variant == 2) assert(result == BB_COMPONENT_BUSY);
            else {
                assert(result == 0 && info.motor_direction == BB_COMPONENT_MOTOR_CCW && info.motor_speed == 25);
                assert(bb_component_client_motor_set(&client, BB_COMPONENT_MOTOR_CW, 100, &info) == 0);
                assert(info.motor_direction == BB_COMPONENT_MOTOR_CW && info.motor_speed == 100);
                assert(bb_component_client_motor_set(&client, BB_COMPONENT_MOTOR_CW, 0, &info) == 0);
                assert(info.motor_direction == BB_COMPONENT_MOTOR_STOP && info.motor_speed == 0);
                assert(bb_component_client_motor_set(&client, BB_COMPONENT_MOTOR_STOP, 60, &info) == 0);
                assert(bb_component_client_status(&client, &info) == 0);
                assert(info.motor_direction == BB_COMPONENT_MOTOR_STOP && info.motor_speed == 60);
            }
        }
        bb_component_client_close(&client);
        atomic_store(&peer.stop, true);
        assert(pthread_join(thread, NULL) == 0);
        assert(peer.motor_sets == (variant == 1 ? 4 : 0));
        close(peer.slave); close(peer.master);
    }
}

static void test_light(const char *tool)
{
    for(unsigned variant = 0; variant < 6; ++variant) {
        peer_t peer = {.profile = BB_COMPONENT_AMBIENT_LIGHT, .light_supported = variant != 0,
            .light_busy = variant == 2, .light_bad_reply = variant >= 3 ? variant - 2 : 0,
            .light = {1, BB_COMPONENT_LIGHT_SOLID, 255, 160, 80, 20, 50, 0}};
        char uart[128]; assert(openpty(&peer.master, &peer.slave, uart, NULL, NULL) == 0);
        pthread_t thread; assert(pthread_create(&thread, NULL, peer_thread, &peer) == 0);
        bb_component_client_t client;
        bb_component_info_t info;
        assert(!bb_component_client_open(&client, uart));
        int result = bb_component_client_probe(&client, &info);
        bb_component_light_settings_t settings = {1, BB_COMPONENT_LIGHT_FLOW, 76, 201, 240, 60, 75, 15};
        if(variant >= 3) assert(result == -EPROTO);
        else {
            assert(!result && info.light_supported == (variant != 0));
            settings.speed = 0;
            assert(bb_component_client_light_set(&client, &settings, &info) == -EINVAL);
            settings.speed = 75;
            result = bb_component_client_light_set(&client, &settings, &info);
            if(variant == 0) assert(result == -ENOTSUP);
            else if(variant == 2) assert(result == BB_COMPONENT_BUSY);
            else {
                assert(!result && info.light_state_valid && info.light.effect == BB_COMPONENT_LIGHT_FLOW);
                assert(info.light.red == 76 && info.light.brightness == 60 && info.light_remaining_minutes == 15);
                settings.enabled = 0;
                assert(!bb_component_client_light_set(&client, &settings, &info));
                assert(!info.light.enabled && !info.light_remaining_minutes);
                assert(!bb_component_client_status(&client, &info) && !info.light.enabled);
            }
        }
        bb_component_client_close(&client);
        if(variant == 1) {
            pid_t child = fork(); assert(child >= 0);
            if(!child) {
                const char *args[16]; unsigned count = 0;
                args[count++] = tool;
                if(s_app_entry) args[count++] = "--component";
                const char *control[] = {"--uart", uart, "light", "on", "beat", "123", "92", "255", "35", "50", "1"};
                for(unsigned i = 0; i < sizeof(control) / sizeof(control[0]); ++i) args[count++] = control[i];
                args[count] = NULL; execv(tool, (char *const *)args); _exit(127);
            }
            int status; assert(waitpid(child, &status, 0) == child);
            assert(WIFEXITED(status) && !WEXITSTATUS(status));
        }
        atomic_store(&peer.stop, true); assert(!pthread_join(thread, NULL));
        assert(peer.light_sets == (variant == 1 ? 3 : 0));
        if(variant == 1) assert(peer.light.enabled && peer.light.effect == BB_COMPONENT_LIGHT_BEAT &&
                                peer.light.red == 123 && peer.light.brightness == 35 && peer.light.timer_minutes == 1);
        close(peer.slave); close(peer.master);
    }
}

static void test_light_worker(void)
{
    struct rlimit original, limited;
    assert(!getrlimit(RLIMIT_NOFILE, &original)); limited = original;
    if(limited.rlim_cur > 1024) limited.rlim_cur = 1024;
    assert(!setrlimit(RLIMIT_NOFILE, &limited));
    char directory[] = "/tmp/binbot-light-worker-XXXXXX"; assert(mkdtemp(directory));
    peer_t peer = {.profile = BB_COMPONENT_AMBIENT_LIGHT, .light_supported = true,
                   .light = {1, BB_COMPONENT_LIGHT_SOLID, 255, 160, 80, 20, 50, 0}};
    char uart[128]; assert(!openpty(&peer.master, &peer.slave, uart, NULL, NULL));
    pthread_t thread; assert(!pthread_create(&thread, NULL, peer_thread, &peer));
    setenv("BINBOT_COMPONENT_STATE_DIR", directory, 1); setenv("BINBOT_COMPONENT_UART", uart, 1);
    bb_component_worker_t worker; bb_component_worker_init(&worker);
    for(unsigned i = 0; i < 2; ++i) {
        binbot_component_command_t command = {.action = BINBOT_COMPONENT_LIGHT_CONTROL, .before_boot_id = i + 1};
        command.request_id[0] = i + 1; command.reserved[0] = BB_COMPONENT_AMBIENT_LIGHT;
        bb_component_light_settings_t settings = {1, BB_COMPONENT_LIGHT_BLINK, 76, 201, 240, 35, 75, 30};
        bb_component_light_encode(&settings, (uint8_t *)command.file_name);
        assert(bb_component_worker_submit(&worker, &command));
        binbot_component_event_t event = {0}; bool received = false;
        for(unsigned tick = 0; tick < 300 && !received; ++tick) {
            received = bb_component_worker_poll(&worker, &event, tick * 10);
            if(!received) usleep(10000);
        }
        assert(received && !worker.pid && !memcmp(event.request_id, command.request_id, 16));
        assert(event.status == (i ? BINBOT_COMPONENT_FAILED : BINBOT_COMPONENT_SUCCEEDED));
        if(!i) assert(strstr(event.message, "\"fx\":3") && strstr(event.message, "\"left\":30"));
    }
    bb_component_worker_close(&worker);
    atomic_store(&peer.stop, true); assert(!pthread_join(thread, NULL));
    assert(peer.light_sets == 1); close(peer.slave); close(peer.master);
    char lock[256]; snprintf(lock, sizeof(lock), "%s/worker.lock", directory); unlink(lock);
    assert(!rmdir(directory)); unsetenv("BINBOT_COMPONENT_STATE_DIR"); unsetenv("BINBOT_COMPONENT_UART");
    assert(!setrlimit(RLIMIT_NOFILE, &original));
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--bridge")) return bridge();
    assert(argc == 2 || (argc == 3 && strcmp(argv[2], "--component") == 0));
    s_app_entry = argc == 3;
    test_motor();
    test_motor_worker();
    test_light(argv[1]);
    test_light_worker();
    test_file_update();
    uint8_t image[IMAGE_BYTES];
    make_image(image);
    test_bad_hash(image);
    char firmware[] = "/tmp/binbot-uart-ota-XXXXXX";
    int file = mkstemp(firmware);
    assert(file >= 0);
    assert(write(file, image, sizeof(image)) == sizeof(image));
    close(file);
    test_cancel(firmware);
    for (unsigned test = 0; test < 2; ++test) {
        peer_t peer = {0};
        peer.drop_data = test ? 0 : 3;
        peer.corrupt_hello = test == 0;
        peer.drop_activation = test == 0;
        peer.wrong_boot = test == 1;
        bb_component_ota_ops_t ops = {begin, write_chunk, finish, abort_transfer, activate};
        bb_component_receiver_init(&peer.receiver, BB_COMPONENT_ROUND_SCREEN, IMAGE_BYTES, &ops, &peer);
        int slave;
        char uart[128];
        assert(openpty(&peer.master, &slave, uart, NULL, NULL) == 0);
        peer.slave = slave;
        fcntl(peer.master, F_SETFD, FD_CLOEXEC);
        fcntl(slave, F_SETFD, FD_CLOEXEC);
        pthread_t thread;
        assert(pthread_create(&thread, NULL, peer_thread, &peer) == 0);
        if (!test) {
            assert(run_tool(argv[1], uart, "status", NULL, false) == 0);
            /* Exhaust all DATA retries, then resume from the acknowledged offset. */
            assert(run_tool(argv[1], uart, "ota", firmware, false) == 1);
            assert(run_tool(argv[1], uart, "ota", firmware, true) == 0);
            bb_component_client_t client, second;
            bb_component_info_t info;
            assert(bb_component_client_open(&client, uart) == 0);
            assert(bb_component_client_open(&second, uart) < 0);
            assert(bb_component_client_status(&client, &info) == 0);
            assert(info.state == BB_COMPONENT_COMMITTED && info.boot_id == 1);
            bb_component_client_close(&client);
        }
        assert(run_tool(argv[1], uart, "ota", firmware, false) == (test ? 1 : 0));
        atomic_store(&peer.stop, true);
        assert(pthread_join(thread, NULL) == 0);
        assert(peer.begins == 1 && peer.finishes == 1 && peer.activations == 1);
        assert(peer.writes == (IMAGE_BYTES + 511) / 512);
        assert(memcmp(peer.flash, image, sizeof(image)) == 0);
        close(slave);
        close(peer.master);
    }
    unlink(firmware);
    puts("component OTA tests passed");
    return 0;
}
