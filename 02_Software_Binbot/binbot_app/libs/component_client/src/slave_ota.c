#define _POSIX_C_SOURCE 200809L
#include "binbot/component/slave_ota.h"
#include <errno.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SLOT_SIZE 0x35000u
#define HEADER_SIZE 160u

static uint64_t now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

static void pause_ms(unsigned milliseconds)
{
    struct timespec delay = {milliseconds / 1000, (long)(milliseconds % 1000) * 1000000};
    nanosleep(&delay, NULL);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put32(uint8_t *p, uint32_t value)
{
    for(unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (8 * i));
}

static int request(binbot_slave_client_t *client, uint8_t command, const uint8_t *data,
                    size_t size, uint8_t reply[64], size_t *reply_size, char error[160])
{
    uint8_t code = 0;
    if(!binbot_slave_client_connect(client, now_ms())) {
        snprintf(error, 160, "cannot connect to slave UART daemon");
        return -ENOTCONN;
    }
    uint32_t timeout = command == BINBOT_SLAVE_CMD_OTA_BEGIN ? 25000 :
                       command == BINBOT_SLAVE_CMD_OTA_FINISH ? 15000 : 2000;
    if(!binbot_slave_client_request(client, command, data, size, timeout, reply, reply_size, &code)) {
        snprintf(error, 160, "slave OTA command 0x%02x timed out", command);
        return -ETIMEDOUT;
    }
    if(code) {
        snprintf(error, 160, "slave OTA command 0x%02x rejected: %u", command, code);
        return code == BINBOT_SLAVE_IPC_ERR_TIMEOUT ? -ETIMEDOUT : code;
    }
    return 0;
}

int bb_slave_ota_open(binbot_slave_client_t *client, char error[160])
{
    binbot_slave_client_init(client, BINBOT_SLAVE_IPC_ROLE_OTA, getenv("BINBOT_SLAVE_SOCKET"));
    error[0] = 0;
    if(binbot_slave_client_connect(client, now_ms())) return 0;
    snprintf(error, 160, "slave UART daemon is unavailable");
    return -ENOTCONN;
}

int bb_slave_ota_status(binbot_slave_client_t *client, bb_component_info_t *info, char error[160])
{
    uint8_t reply[64];
    size_t size = 0;
    int result = request(client, BINBOT_SLAVE_CMD_OTA_INFO, NULL, 0, reply, &size, error);
    if(result) return result;
    if(size != 56 || reply[0] != 1 || reply[1] > 1 || get32(reply + 8) != SLOT_SIZE) {
        snprintf(error, 160, "slave firmware does not support the expected A/B layout");
        return -EPROTO;
    }
    memset(info, 0, sizeof(*info));
    info->profile = 4;
    info->state = reply[3];
    info->healthy = reply[2] == 1;
    info->boot_id = get32(reply + 4);
    info->capacity = get32(reply + 8);
    snprintf(info->version, sizeof(info->version), "%u", get32(reply + 12));
    snprintf(info->project, sizeof(info->project), "binbot_slave");
    memcpy(info->elf_sha256, reply + 16, 32);
    info->received = get32(reply + 48);
    /* Slot selection stays private to this updater. */
    info->transfer_id = reply[1];
    return 0;
}

int bb_slave_ota_abort(binbot_slave_client_t *client, char error[160])
{
    uint8_t reply[64];
    size_t size = 0;
    return request(client, BINBOT_SLAVE_CMD_OTA_ABORT, NULL, 0, reply, &size, error);
}

static bool digest_matches(const uint8_t *data, size_t size, const uint8_t expected[32])
{
    uint8_t digest[32];
    unsigned length = 0;
    return EVP_Digest(data, size, digest, &length, EVP_sha256(), NULL) == 1 &&
           length == 32 && !memcmp(digest, expected, 32);
}

static bool valid_package(const uint8_t *data, size_t size)
{
    if(size < 168 || memcmp(data, "BBSOTA1\0", 8) || get32(data + 8) != 1 ||
       get32(data + 12) != 0x582 || get32(data + 20) != SLOT_SIZE ||
       bb_component_crc32(data, 156) != get32(data + 156)) return false;
    for(unsigned i = 136; i < 156; ++i) if(data[i]) return false;
    uint32_t a = get32(data + 24), b = get32(data + 28);
    if(a < 4 || b < 4 || a > SLOT_SIZE || b > SLOT_SIZE || a % 4 || b % 4 ||
       size != HEADER_SIZE + a + b) return false;
    uint8_t identity_input[68];
    memcpy(identity_input, data + 16, 4);
    memcpy(identity_input + 4, data + 64, 64);
    return digest_matches(identity_input, sizeof(identity_input), data + 32) &&
           digest_matches(data + HEADER_SIZE, a, data + 64) &&
           digest_matches(data + HEADER_SIZE + a, b, data + 96) &&
           bb_component_crc32(data + HEADER_SIZE, a) == get32(data + 128) &&
           bb_component_crc32(data + HEADER_SIZE + a, b) == get32(data + 132);
}

static int transition(binbot_slave_client_t *client, bb_component_phase_fn phase,
                        bb_component_phase_t value, void *context, char error[160])
{
    if(!phase || !phase(value, context)) return 0;
    int result = bb_slave_ota_abort(client, error);
    return result ? result : -ECANCELED;
}

int bb_slave_ota_update(binbot_slave_client_t *client, const char *path,
                         bb_component_progress_fn progress, bb_component_phase_fn phase,
                         void *context, char error[160])
{
    FILE *file = fopen(path, "rb");
    if(!file) return -errno;
    int result = -EINVAL;
    uint8_t *package = NULL;
    if(fseek(file, 0, SEEK_END)) goto done;
    long file_size = ftell(file);
    if(file_size < 168 || file_size > (long)(HEADER_SIZE + 2 * SLOT_SIZE)) goto done;
    rewind(file);
    package = malloc((size_t)file_size);
    if(!package) { result = -ENOMEM; goto done; }
    if(fread(package, 1, (size_t)file_size, file) != (size_t)file_size ||
       !valid_package(package, (size_t)file_size)) {
        snprintf(error, 160, "invalid slave A/B package or SHA-256");
        goto done;
    }
    bb_component_info_t before;
    result = bb_slave_ota_status(client, &before, error);
    if(result) goto done;
    if(!before.healthy) { result = 6; goto done; }
    uint8_t slot = (uint8_t)(1 - before.transfer_id);
    uint32_t size = get32(package + 24 + slot * 4);
    const uint8_t *image = package + HEADER_SIZE + (slot ? get32(package + 24) : 0);
    uint8_t begin[48] = {0}, reply[64];
    size_t reply_size = 0;
    put32(begin, size);
    memcpy(begin + 4, package + 16, 4);
    memcpy(begin + 8, package + 128 + slot * 4, 4);
    memcpy(begin + 12, package + 32, 32);
    begin[44] = slot;
    result = transition(client, phase, BB_COMPONENT_PHASE_TRANSFERRING, context, error);
    if(result) goto done;
    result = request(client, BINBOT_SLAVE_CMD_OTA_BEGIN, begin, sizeof(begin), reply, &reply_size, error);
    if(result) goto done;
    if(reply_size != 4 || get32(reply) > size || get32(reply) % 4) { result = -EPROTO; goto done; }
    for(uint32_t offset = get32(reply); offset < size;) {
        result = transition(client, phase, BB_COMPONENT_PHASE_TRANSFERRING, context, error);
        if(result) goto done;
        uint8_t chunk[64];
        uint32_t count = size - offset;
        if(count > 60) count = 60;
        put32(chunk, offset);
        memcpy(chunk + 4, image + offset, count);
        result = request(client, BINBOT_SLAVE_CMD_OTA_DATA, chunk, count + 4, reply, &reply_size, error);
        if(result) goto done;
        if(reply_size != 4 || get32(reply) != offset + count) { result = -EPROTO; goto done; }
        offset += count;
        if(progress) progress((uint32_t)((uint64_t)offset * (uint32_t)file_size / size),
                              (uint32_t)file_size, context);
    }
    result = transition(client, phase, BB_COMPONENT_PHASE_VALIDATING, context, error);
    if(result) goto done;
    result = request(client, BINBOT_SLAVE_CMD_OTA_FINISH, NULL, 0, reply, &reply_size, error);
    if(result) goto done;
    result = transition(client, phase, BB_COMPONENT_PHASE_ACTIVATING, context, error);
    if(result) goto done;
    result = request(client, BINBOT_SLAVE_CMD_OTA_ACTIVATE, NULL, 0, reply, &reply_size, error);
    /* A lost activation ACK is resolved by observing the next confirmed boot. */
    if(phase) (void)phase(BB_COMPONENT_PHASE_VERIFYING, context);
    uint64_t deadline = now_ms() + 15000;
    result = -ETIMEDOUT;
    while(now_ms() < deadline) {
        pause_ms(200);
        bb_component_info_t info;
        if(bb_slave_ota_status(client, &info, error) || !info.healthy || info.boot_id == before.boot_id) continue;
        if(info.transfer_id == slot && !memcmp(info.elf_sha256, package + 32, 32) &&
           strtoul(info.version, NULL, 10) == get32(package + 16)) { result = 0; error[0] = 0; }
        else { result = 9; snprintf(error, 160, "slave rolled back to the confirmed image"); }
        break;
    }
    if(result == -ETIMEDOUT) snprintf(error, 160, "slave target boot has not been confirmed");
done:
    free(package);
    fclose(file);
    return result;
}
