#define _DEFAULT_SOURCE
#include "binbot/component/slave_ota.h"
#include "binbot/component/worker.h"
#include "ab_ota.h"
#include <assert.h>
#include <arpa/inet.h>
#include <errno.h>
#include <openssl/evp.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { IMAGE_SIZE = 512, PACKAGE_SIZE = 160 + 2 * IMAGE_SIZE };
typedef struct {
    uint8_t flash[0x70000];
    ab_ota_t ota;
    bool rollback, lost_data_ack, lost_activate_ack;
    unsigned data_commands;
} peer_t;
static peer_t *peer;

static bool flash_read(uint32_t address, void *data, uint32_t size)
{
    assert(address + size <= sizeof(peer->flash));
    memcpy(data, peer->flash + address, size);
    return true;
}

static bool flash_erase(uint32_t address)
{
    assert(address % AB_PAGE_SIZE == 0);
    memset(peer->flash + address, 0xff, AB_PAGE_SIZE);
    return true;
}

static bool flash_write(uint32_t address, const void *data, uint32_t size)
{
    const uint8_t *p = data;
    for(uint32_t i = 0; i < size; ++i) {
        assert((peer->flash[address + i] & p[i]) == p[i]);
        peer->flash[address + i] &= p[i];
    }
    return true;
}
static const ab_flash_t storage = {flash_read, flash_erase, flash_write, UINT32_MAX};

static void put32(uint8_t *p, uint32_t value)
{
    for(unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (8 * i));
}

static void digest(const void *bytes, size_t size, uint8_t output[32])
{
    unsigned length;
    assert(EVP_Digest(bytes, size, output, &length, EVP_sha256(), NULL) == 1 && length == 32);
}

static void hex(char *output, const uint8_t input[32])
{
    for(unsigned i = 0; i < 32; ++i) snprintf(output + i * 2, 3, "%02x", input[i]);
}

static void package_create(uint8_t package[PACKAGE_SIZE])
{
    memset(package, 0, PACKAGE_SIZE);
    memcpy(package, "BBSOTA1\0", 8);
    put32(package + 8, 1); put32(package + 12, 0x582);
    put32(package + 16, 2); put32(package + 20, AB_SLOT_SIZE);
    put32(package + 24, IMAGE_SIZE); put32(package + 28, IMAGE_SIZE);
    memset(package + 160, 0x17, IMAGE_SIZE);
    memset(package + 160 + IMAGE_SIZE, 0x42, IMAGE_SIZE);
    digest(package + 160, IMAGE_SIZE, package + 64);
    digest(package + 160 + IMAGE_SIZE, IMAGE_SIZE, package + 96);
    uint8_t identity[68];
    memcpy(identity, package + 16, 4); memcpy(identity + 4, package + 64, 64);
    digest(identity, sizeof(identity), package + 32);
    put32(package + 128, ab_crc32(package + 160, IMAGE_SIZE));
    put32(package + 132, ab_crc32(package + 160 + IMAGE_SIZE, IMAGE_SIZE));
    put32(package + 156, ab_crc32(package, 156));
}

static void peer_initialize(uint8_t initial_slot, bool rollback)
{
    memset(peer, 0, sizeof(*peer));
    memset(peer->flash, 0xff, sizeof(peer->flash));
    peer->rollback = rollback;
    ab_record_t record = {.magic = AB_META_MAGIC, .generation = 1,
                           .active = initial_slot, .confirmed = initial_slot, .pending = AB_NONE};
    for(unsigned slot = 0; slot < 2; ++slot) {
        memset(peer->flash + ab_slot_address(slot), 0x11, 128);
        record.image[slot].size = 128;
        record.image[slot].version = 1;
        record.image[slot].crc = ab_crc32(peer->flash + ab_slot_address(slot), 128);
    }
    record.crc = ab_crc32(&record, 124);
    memcpy(peer->flash + AB_META_BASE, &record, sizeof(record));
    assert(ab_load(&peer->ota, &storage, initial_slot));
    assert(ab_boot_select(&peer->ota) == initial_slot);
}

static void send_all(int fd, const void *bytes, size_t size)
{
    const uint8_t *data = bytes;
    while(size) {
        ssize_t written = write(fd, data, size);
        if(written < 0 && errno == EINTR) continue;
        assert(written > 0);
        data += written; size -= (size_t)written;
    }
}

static void peer_run(int master)
{
    binbot_slave_parser_t parser;
    binbot_slave_parser_init(&parser);
    for(;;) {
        struct pollfd pfd = {.fd = master, .events = POLLIN};
        if(poll(&pfd, 1, 100) <= 0) continue;
        uint8_t input[128];
        ssize_t count = read(master, input, sizeof(input));
        if(count <= 0) { usleep(10000); continue; }
        for(ssize_t i = 0; i < count; ++i) {
            binbot_slave_frame_t request;
            if(binbot_slave_parser_feed(&parser, input[i], &request) != BINBOT_SLAVE_PARSE_FRAME) continue;
            binbot_slave_frame_t reply = request;
            reply.flags = BINBOT_SLAVE_FLAG_RESPONSE;
            reply.payload_length = 0;
            uint8_t error = 0;
            if(request.command >= AB_INFO && request.command <= AB_ABORT)
                error = ab_command(&peer->ota, request.command, request.payload,
                                   request.payload_length, reply.payload, &reply.payload_length);
            else if(request.command == BINBOT_SLAVE_CMD_GET_STATUS_ALL) {
                memset(reply.payload, 0, 22); reply.payload_length = 22;
            } else if(request.command == BINBOT_SLAVE_CMD_EVENT_ENABLE) {
                memcpy(reply.payload, request.payload, 3); reply.payload[3] = 0; reply.payload_length = 4;
            }
            if(error) { reply.flags |= BINBOT_SLAVE_FLAG_ERROR; reply.payload[0] = error; reply.payload_length = 1; }
            bool drop = false;
            if(request.command == AB_DATA) {
                peer->data_commands++;
                if(!peer->lost_data_ack) { peer->lost_data_ack = true; drop = true; }
            }
            if(request.command == AB_ACTIVATE && !peer->lost_activate_ack) {
                peer->lost_activate_ack = true; drop = true;
            }
            uint8_t encoded[BINBOT_SLAVE_MAX_FRAME];
            size_t size = binbot_slave_frame_encode(&reply, encoded, sizeof(encoded));
            if(!drop) send_all(master, encoded, size);
            if(peer->ota.reset_requested) {
                assert(ab_load(&peer->ota, &storage, AB_NONE));
                int slot = ab_boot_select(&peer->ota);
                assert(slot >= 0);
                peer->ota.running_slot = (uint8_t)slot;
                if(peer->rollback) {
                    assert(ab_load(&peer->ota, &storage, AB_NONE));
                    slot = ab_boot_select(&peer->ota); assert(slot >= 0);
                    peer->ota.running_slot = (uint8_t)slot;
                } else assert(ab_confirm(&peer->ota));
            }
        }
    }
}

static pid_t serve_package(const uint8_t package[PACKAGE_SIZE], char url[768])
{
    int fd = socket(AF_INET, SOCK_STREAM, 0); assert(fd >= 0);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0 && listen(fd, 1) == 0);
    socklen_t length = sizeof(address);
    assert(getsockname(fd, (struct sockaddr *)&address, &length) == 0);
    snprintf(url, 768, "http://127.0.0.1:%u/firmware", ntohs(address.sin_port));
    pid_t pid = fork(); assert(pid >= 0);
    if(!pid) {
        alarm(15);
        int client = accept(fd, NULL, NULL); assert(client >= 0);
        char request[2048] = {0};
        size_t used = 0;
        while(!strstr(request, "\r\n\r\n")) {
            ssize_t n = read(client, request + used, sizeof(request) - used - 1);
            assert(n > 0); used += (size_t)n;
        }
        char header[128];
        int n = snprintf(header, sizeof(header), "HTTP/1.1 200 OK\r\nContent-Length: %u\r\nConnection: close\r\n\r\n", PACKAGE_SIZE);
        send_all(client, header, (size_t)n); send_all(client, package, PACKAGE_SIZE);
        close(client); close(fd); _exit(0);
    }
    close(fd); return pid;
}

static int cancel_activation(bb_component_phase_t phase, void *context)
{
    (void)context;
    return phase == BB_COMPONENT_PHASE_ACTIVATING;
}

static void scenario(const char *daemon, unsigned mode)
{
    uint8_t initial_slot = mode == 1 ? 1 : 0;
    peer_initialize(initial_slot, mode == 2);
    char directory[] = "/tmp/binbot-slave-ota-XXXXXX"; assert(mkdtemp(directory));
    char socket_path[108], uart[128], file_path[320];
    snprintf(socket_path, sizeof(socket_path), "%s/slave.sock", directory);
    assert(setenv("BINBOT_SLAVE_SOCKET", socket_path, 1) == 0);
    assert(setenv("BINBOT_COMPONENT_STATE_DIR", directory, 1) == 0);
    int master, slave; assert(openpty(&master, &slave, uart, NULL, NULL) == 0);
    assert(setenv("BINBOT_SLAVE_DEVICE", uart, 1) == 0);
    pid_t daemon_pid = fork(); assert(daemon_pid >= 0);
    if(!daemon_pid) { close(master); close(slave); execl(daemon, daemon, (char *)NULL); _exit(127); }
    pid_t peer_pid = fork(); assert(peer_pid >= 0);
    if(!peer_pid) { close(slave); peer_run(master); _exit(0); }
    close(master); close(slave);
    for(unsigned i = 0; i < 200 && access(socket_path, F_OK); ++i) usleep(10000);
    assert(access(socket_path, F_OK) == 0);
    uint8_t package[PACKAGE_SIZE]; package_create(package);
    if(mode == 3) package[160] ^= 1;
    if(mode == 4) {
        snprintf(file_path, sizeof(file_path), "%s/package.ota", directory);
        FILE *file = fopen(file_path, "wb"); assert(file);
        assert(fwrite(package, 1, sizeof(package), file) == sizeof(package) && fclose(file) == 0);
        binbot_slave_client_t client, ordinary;
        char error[160]; bb_component_info_t info;
        assert(bb_slave_ota_open(&client, error) == 0);
        assert(bb_slave_ota_status(&client, &info, error) == 0);
        binbot_slave_client_init(&ordinary, BINBOT_SLAVE_IPC_ROLE_APP, socket_path);
        assert(binbot_slave_client_connect(&ordinary, 1));
        uint8_t payload[64] = {1, 1}, code; size_t size;
        assert(binbot_slave_client_request(&ordinary, BINBOT_SLAVE_CMD_SET_OUTPUT, payload, 2, 1000, payload, &size, &code));
        assert(code == BINBOT_SLAVE_ERR_BUSY);
        assert(bb_slave_ota_update(&client, file_path, NULL, cancel_activation, NULL, error) == -ECANCELED);
        assert(peer->ota.record.pending == AB_NONE && peer->ota.record.confirmed == initial_slot);
        binbot_slave_client_close(&ordinary); binbot_slave_client_close(&client);
        unlink(file_path);
    } else {
        binbot_component_command_t command = {.action = BINBOT_COMPONENT_OTA, .size = PACKAGE_SIZE, .before_boot_id = 1};
        command.request_id[0] = (uint8_t)(mode + 1); command.reserved[3] = 1;
        snprintf(command.version, sizeof(command.version), "2");
        snprintf(command.before_version, sizeof(command.before_version), "1");
        memset(command.before_elf_sha256, '0', 64);
        hex(command.elf_sha256, package + 32);
        uint8_t hash[32]; digest(package, sizeof(package), hash); hex(command.sha256, hash);
        pid_t http_pid = serve_package(package, command.url);
        bb_component_worker_t worker; bb_component_worker_init(&worker);
        assert(bb_component_worker_submit(&worker, &command));
        binbot_component_event_t event = {0}; bool terminal = false;
        for(unsigned i = 0; i < 2000 && !terminal; ++i) {
            if(bb_component_worker_poll(&worker, &event, i * 10)) {
                assert(event.profile == 4);
                terminal = event.status == BINBOT_COMPONENT_SUCCEEDED || event.status == BINBOT_COMPONENT_FAILED;
            }
            if(!terminal) usleep(10000);
        }
        assert(terminal);
        if(mode < 2) {
            assert(event.status == BINBOT_COMPONENT_SUCCEEDED && event.has_info && event.healthy);
            assert(event.bytes_written == PACKAGE_SIZE && event.total_bytes == PACKAGE_SIZE);
            assert(peer->ota.record.confirmed == 1 - initial_slot);
            assert(!memcmp(peer->flash + ab_slot_address(1 - initial_slot),
                           package + 160 + (1 - initial_slot) * IMAGE_SIZE, IMAGE_SIZE));
        } else {
            assert(event.status == BINBOT_COMPONENT_FAILED);
            assert(peer->ota.record.confirmed == initial_slot);
            if(mode == 3) assert(peer->data_commands == 0);
        }
        bb_component_worker_close(&worker);
        int status; assert(waitpid(http_pid, &status, 0) == http_pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
        const char *files[] = {"firmware.bin", "job.state", "worker.lock"};
        for(unsigned i = 0; i < 3; ++i) { snprintf(file_path, sizeof(file_path), "%s/%s", directory, files[i]); unlink(file_path); }
    }
    kill(peer_pid, SIGTERM); kill(daemon_pid, SIGTERM);
    assert(waitpid(peer_pid, NULL, 0) == peer_pid && waitpid(daemon_pid, NULL, 0) == daemon_pid);
    unlink(socket_path); assert(rmdir(directory) == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    peer = mmap(NULL, sizeof(*peer), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    assert(peer != MAP_FAILED);
    for(unsigned mode = 0; mode < 5; ++mode) scenario(argv[1], mode);
    munmap(peer, sizeof(*peer));
    puts("slave OTA: HTTP download, worker, IPC, UART, A/B, lost ACKs, rollback, corruption and cancellation passed");
    return 0;
}
