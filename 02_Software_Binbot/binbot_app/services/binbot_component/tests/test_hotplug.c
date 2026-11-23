#define _DEFAULT_SOURCE
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "binbot/component/worker.h"

#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <pty.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

static int64_t now_ms(void)
{
    struct timespec ts;
    assert(!clock_gettime(CLOCK_MONOTONIC, &ts));
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void check_ack(int master, const bb_component_frame_t *announcement)
{
    bb_component_parser_t parser = {0};
    bb_component_frame_t ack;
    struct pollfd fd = {master, POLLIN, 0};
    assert(poll(&fd, 1, 100) == 1);
    uint8_t bytes[14 + BB_COMPONENT_ANNOUNCE_ACK_BYTES];
    assert(read(master, bytes, sizeof(bytes)) == sizeof(bytes));
    for(size_t i = 0; i < sizeof(bytes); ++i)
        assert(bb_component_parser_push(&parser, bytes[i], &ack) == (i == sizeof(bytes) - 1));
    assert(ack.type == BB_COMPONENT_RESPONSE && ack.command == BB_COMPONENT_ANNOUNCE);
    assert(ack.sequence == announcement->sequence && ack.length == BB_COMPONENT_ANNOUNCE_ACK_BYTES);
    assert(bb_component_get_u16(ack.payload) == BB_COMPONENT_OK);
    assert(bb_component_get_u64(ack.payload + 2) == bb_component_get_u64(announcement->payload + 4));
}

int main(int argc, char **argv)
{
    if(argc > 1 && !strcmp(argv[1], "--component-worker")) {
        const char *mapping = getenv("BINBOT_TEST_PARENT_MAPPING");
        assert(mapping);
        FILE *maps = fopen("/proc/self/maps", "r"); assert(maps);
        char line[1024];
        while(fgets(line, sizeof(line), maps)) assert(!strstr(line, mapping));
        fclose(maps);
        return bb_component_worker_main(argc, argv);
    }
    struct rlimit limit;
    assert(!getrlimit(RLIMIT_NOFILE, &limit));
    if(limit.rlim_cur > 1024) limit.rlim_cur = 1024;
    assert(!setrlimit(RLIMIT_NOFILE, &limit));
    int master, slave;
    char uart[128], directory[] = "/tmp/binbot-hotplug-XXXXXX";
    assert(!openpty(&master, &slave, uart, NULL, NULL));
    assert(mkdtemp(directory));
    char mapping_path[256];
    snprintf(mapping_path, sizeof(mapping_path), "%s/camera-mapping", directory);
    FILE *mapped_file = fopen(mapping_path, "w+"); assert(mapped_file);
    assert(!ftruncate(fileno(mapped_file), 4096));
    void *mapping = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fileno(mapped_file), 0);
    assert(mapping != MAP_FAILED);
    assert(!setenv("BINBOT_TEST_PARENT_MAPPING", mapping_path, 1));
    assert(!setenv("BINBOT_COMPONENT_UART", uart, 1));
    assert(!setenv("BINBOT_COMPONENT_STATE_DIR", directory, 1));
    bb_component_worker_t worker;
    bb_component_worker_init_exec(&worker);
    binbot_component_event_t event;
    assert(!bb_component_worker_receive_announcement(&worker, &event, 0));
    assert(worker.listener.fd >= 0 && worker.pid == 0);
    worker.latest.status = BINBOT_COMPONENT_SUCCEEDED;
    worker.latest.event_seq = 42;

    bb_component_frame_t notice = {.type = BB_COMPONENT_EVENT, .sequence = 7,
        .command = BB_COMPONENT_ANNOUNCE, .length = BB_COMPONENT_ANNOUNCE_BYTES};
    notice.payload[0] = BB_COMPONENT_AMBIENT_LIGHT;
    bb_component_put_u64(notice.payload + 4, 123);
    bb_component_put_u32(notice.payload + 12, 1024 * 1024);
    strcpy((char *)notice.payload + 32, "33");
    strcpy((char *)notice.payload + 64, bb_component_project(notice.payload[0]));
    uint8_t wire[BB_COMPONENT_FRAME_BYTES];
    size_t length = bb_component_encode(&notice, wire, sizeof(wire));
    assert(write(master, wire, 11) == 11);
    assert(!bb_component_worker_receive_announcement(&worker, &event, 1));
    assert(write(master, wire + 11, length - 11) == (ssize_t)(length - 11));
    assert(bb_component_worker_receive_announcement(&worker, &event, 2));
    assert(event.has_info && event.profile == BB_COMPONENT_AMBIENT_LIGHT && !event.healthy);
    assert(event.boot_id == 123 && !strcmp(event.version, "33"));
    assert(worker.latest.status == BINBOT_COMPONENT_SUCCEEDED && worker.latest.event_seq == 42);
    check_ack(master, &notice);

    assert(write(master, wire, length) == (ssize_t)length);
    assert(bb_component_worker_receive_announcement(&worker, &event, 3));
    assert(event.event_seq == 2);
    check_ack(master, &notice);
    notice.payload[2] = 1;
    ++notice.sequence;
    length = bb_component_encode(&notice, wire, sizeof(wire));
    wire[length - 1] ^= 1;
    assert(write(master, wire, length) == (ssize_t)length);
    assert(!bb_component_worker_receive_announcement(&worker, &event, 4));
    wire[length - 1] ^= 1;
    assert(write(master, wire, length) == (ssize_t)length);
    assert(bb_component_worker_receive_announcement(&worker, &event, 5));
    assert(event.healthy && event.event_seq == 3);
    check_ack(master, &notice);

    notice.payload[0] = BB_COMPONENT_SHOWCASE_TURNTABLE;
    notice.payload[3] = BB_COMPONENT_CAP_MOTOR;
    strcpy((char *)notice.payload + 64, bb_component_project(notice.payload[0]));
    notice.payload[BB_COMPONENT_INFO_BYTES] = BB_COMPONENT_MOTOR_CW;
    notice.payload[BB_COMPONENT_INFO_BYTES + 1] = 65;
    length = bb_component_encode(&notice, wire, sizeof(wire));
    assert(write(master, wire, length) == (ssize_t)length);
    assert(bb_component_worker_receive_announcement(&worker, &event, 6));
    assert(event.motor_supported && event.motor_direction == BB_COMPONENT_MOTOR_CW && event.motor_speed == 65);
    check_ack(master, &notice);

    binbot_component_command_t command = {.action = BINBOT_COMPONENT_PROBE};
    command.request_id[0] = 99;
    int64_t start = now_ms();
    assert(bb_component_worker_submit(&worker, &command));
    assert(worker.listener.fd == -1);
    assert(!bb_component_worker_receive_announcement(&worker, &event, 7));
    binbot_component_command_t next = {.action = BINBOT_COMPONENT_OTA};
    next.request_id[0] = 100;
    assert(bb_component_worker_submit(&worker, &next));
    assert(worker.command_pending && worker.active_action == BINBOT_COMPONENT_PROBE);
    assert(bb_component_worker_submit(&worker, &next));
    binbot_component_command_t other = {.action = BINBOT_COMPONENT_PROBE};
    other.request_id[0] = 101;
    assert(!bb_component_worker_submit(&worker, &other));
    bool received = false;
    while(!received && now_ms() - start < 1500) {
        received = bb_component_worker_poll(&worker, &event, now_ms());
        if(!received) usleep(1000);
    }
    assert(received && worker.pid == 0 && event.status == BINBOT_COMPONENT_PROBED && !event.has_info);
    assert(now_ms() - start < 1200);
    assert(!bb_component_worker_receive_announcement(&worker, &event, now_ms()));
    assert(worker.listener.fd == -1);
    (void)bb_component_worker_poll(&worker, &event, now_ms());
    assert(worker.pid > 0 && worker.active_action == BINBOT_COMPONENT_OTA && !worker.command_pending);
    assert(!memcmp(worker.latest.request_id, next.request_id, sizeof(next.request_id)));
    bb_component_worker_close(&worker);
    assert(!bb_component_worker_receive_announcement(&worker, &event, now_ms()));
    assert(worker.listener.fd >= 0);
    bb_component_worker_close(&worker);
    close(master);
    close(slave);
    char path[256];
    snprintf(path, sizeof(path), "%s/worker.lock", directory);
    unlink(path);
    snprintf(path, sizeof(path), "%s/job.state", directory);
    unlink(path);
    snprintf(path, sizeof(path), "%s/job.tmp", directory);
    unlink(path);
    assert(!munmap(mapping, 4096));
    assert(!fclose(mapped_file));
    assert(!unlink(mapping_path));
    assert(!rmdir(directory));
    return 0;
}
