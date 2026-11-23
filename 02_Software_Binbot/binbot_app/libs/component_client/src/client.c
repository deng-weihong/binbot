#define _POSIX_C_SOURCE 200809L
#include "binbot/component/client.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/tty.h>
#include <openssl/evp.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

_Static_assert(BB_COMPONENT_UART_BAUD == 460800,
               "Update the termios baud constant together with the wire rate");

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int fail(bb_component_client_t *c, int code, const char *message)
{
    snprintf(c->error, sizeof(c->error), "%s", message);
    return code;
}

static int wait_fd(int fd, short events, int64_t deadline)
{
    for (;;) {
        int64_t remaining = deadline - now_ms();
        if (remaining <= 0) return -ETIMEDOUT;
        struct pollfd p = {fd, events, 0};
        int result = poll(&p, 1, remaining > INT_MAX ? INT_MAX : (int)remaining);
        if (result < 0 && errno == EINTR) continue;
        if (result < 0) return -errno;
        if (!result) return -ETIMEDOUT;
        if (p.revents & events) return 0;
        if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) return -EIO;
    }
}

static int send_bytes(int fd, const uint8_t *bytes, size_t length, int64_t deadline)
{
    while (length) {
        int result = wait_fd(fd, POLLOUT, deadline);
        if (result) return result;
        ssize_t n = write(fd, bytes, length);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n <= 0) return n < 0 ? -errno : -EIO;
        bytes += n;
        length -= (size_t)n;
    }
    return 0;
}

static bool matching_reply(const bb_component_frame_t *reply,
                            const bb_component_frame_t *request)
{
    return reply->type == BB_COMPONENT_RESPONSE &&
           reply->sequence == request->sequence &&
           reply->command == request->command && reply->length >= 2;
}

static int receive_reply(bb_component_client_t *c,
                          const bb_component_frame_t *request,
                          bb_component_frame_t *reply, int64_t deadline)
{
    for (;;) {
        while (bb_component_parser_next(&c->parser, reply)) {
            if (matching_reply(reply, request)) return 0;
        }
        int result = wait_fd(c->fd, POLLIN, deadline);
        if (result) return result;
        uint8_t byte;
        ssize_t n = read(c->fd, &byte, 1);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n <= 0) return n < 0 ? -errno : -EIO;
        const int64_t now = now_ms();
        if (now - c->last_byte_ms > 200) c->parser.length = 0;
        c->last_byte_ms = now;
        if (bb_component_parser_push(&c->parser, byte, reply) &&
            matching_reply(reply, request)) return 0;
    }
}

static int request(bb_component_client_t *c, uint16_t command,
                    const uint8_t *payload, size_t length,
                    bb_component_frame_t *reply, int timeout_ms, unsigned attempts)
{
    bb_component_frame_t frame = {0};
    frame.type = BB_COMPONENT_REQUEST;
    frame.sequence = ++c->sequence;
    frame.command = command;
    frame.length = (uint16_t)length;
    if (length) memcpy(frame.payload, payload, length);
    uint8_t bytes[BB_COMPONENT_FRAME_BYTES];
    const size_t count = bb_component_encode(&frame, bytes, sizeof(bytes));
    for (unsigned attempt = 0; attempt < attempts; ++attempt) {
        const int64_t deadline = now_ms() + timeout_ms;
        int result = send_bytes(c->fd, bytes, count, deadline);
        if (!result) result = receive_reply(c, &frame, reply, deadline);
        if (result == -ETIMEDOUT) continue;
        if (result) return fail(c, result, "UART I/O failed");
        const uint16_t status = bb_component_get_u16(reply->payload);
        if (status) return fail(c, status, bb_component_status_name(status));
        c->error[0] = 0;
        return 0;
    }
    return fail(c, -ETIMEDOUT, "UART response timed out");
}

int bb_component_client_open(bb_component_client_t *c, const char *uart)
{
    memset(c, 0, sizeof(*c));
    c->fd = -1;
    int fd = open(uart ? uart : BB_COMPONENT_DEFAULT_UART,
                   O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return fail(c, -errno, strerror(errno));
    int discipline;
    if (ioctl(fd, TIOCGETD, &discipline) < 0 || discipline != N_TTY) {
        int result = fail(c, -EBUSY,
                          "UART has another line discipline; release its current attachment first");
        close(fd);
        return result;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        int result = fail(c, -errno, "UART is already in use by another component client");
        close(fd);
        return result;
    }
    if (tcgetattr(fd, &c->saved_termios) < 0) {
        int result = fail(c, -errno, "Cannot read UART configuration");
        close(fd);
        return result;
    }
    struct termios config = c->saved_termios;
    config.c_iflag = 0;
    config.c_oflag = 0;
    config.c_lflag = 0;
    config.c_cflag = CS8 | CLOCAL | CREAD;
    config.c_cc[VMIN] = 0;
    config.c_cc[VTIME] = 0;
    cfsetispeed(&config, B460800);
    cfsetospeed(&config, B460800);
    if (tcsetattr(fd, TCSANOW, &config) < 0 || tcflush(fd, TCIOFLUSH) < 0) {
        int result = fail(c, -errno, "Cannot configure UART as 460800 8N1");
        (void)tcsetattr(fd, TCSANOW, &c->saved_termios);
        close(fd);
        return result;
    }
    c->fd = fd;
    return 0;
}

void bb_component_client_close(bb_component_client_t *c)
{
    if (c->fd >= 0) {
        (void)tcsetattr(c->fd, TCSANOW, &c->saved_termios);
        close(c->fd);
        c->fd = -1;
    }
}

static int motor_reply(bb_component_client_t *c, const bb_component_frame_t *reply,
                       bb_component_info_t *info)
{
    if (reply->length != 2 + BB_COMPONENT_MOTOR_BYTES ||
        reply->payload[2] > BB_COMPONENT_MOTOR_CW || reply->payload[3] > 100 ||
        (reply->payload[2] != BB_COMPONENT_MOTOR_STOP && !reply->payload[3]))
        return fail(c, -EPROTO, "Invalid motor response");
    info->motor_direction = reply->payload[2];
    info->motor_speed = reply->payload[3];
    return 0;
}

static int decode_info(bb_component_client_t *c, const uint8_t *p,
                        size_t length, bb_component_info_t *info)
{
    if (length != BB_COMPONENT_INFO_BYTES || !bb_component_project(p[0]) ||
        p[1] > BB_COMPONENT_ACTIVATED || p[2] > 1 ||
        !memchr(p + 32, 0, 32) || !memchr(p + 64, 0, 32) ||
        strcmp((const char *)p + 64, bb_component_project(p[0])))
        return fail(c, -EPROTO, "Invalid HELLO response");
    memset(info, 0, sizeof(*info));
    info->profile = p[0];
    info->state = p[1];
    info->healthy = p[2] == 1;
    info->boot_id = bb_component_get_u64(p + 4);
    info->capacity = bb_component_get_u32(p + 12);
    info->received = bb_component_get_u32(p + 16);
    info->total = bb_component_get_u32(p + 20);
    info->transfer_id = bb_component_get_u64(p + 24);
    memcpy(info->version, p + 32, 32);
    memcpy(info->project, p + 64, 32);
    memcpy(info->elf_sha256, p + 96, 32);
    info->motor_supported = info->profile == BB_COMPONENT_SHOWCASE_TURNTABLE &&
                            (p[3] & BB_COMPONENT_CAP_MOTOR) != 0;
    info->light_supported = info->profile == BB_COMPONENT_AMBIENT_LIGHT &&
                            (p[3] & BB_COMPONENT_CAP_LIGHT) != 0;
    info->pen_supported = info->profile == BB_COMPONENT_ROUND_SCREEN &&
                          (p[3] & BB_COMPONENT_CAP_PEN) != 0;
    return 0;
}

static int light_reply(bb_component_client_t *c, const bb_component_frame_t *reply,
                       bb_component_info_t *info)
{
    if(reply->length != 2 + BB_COMPONENT_LIGHT_STATE_BYTES ||
       !bb_component_light_decode(reply->payload + 2, BB_COMPONENT_LIGHT_BYTES, &info->light))
        return fail(c, -EPROTO, "Invalid light response");
    info->light_remaining_minutes = bb_component_get_u16(reply->payload + 2 + BB_COMPONENT_LIGHT_BYTES);
    if(info->light_remaining_minutes > info->light.timer_minutes ||
       (!info->light.enabled && info->light_remaining_minutes))
        return fail(c, -EPROTO, "Invalid light timer response");
    info->light_state_valid = true;
    return 0;
}

static int pen_reply(bb_component_client_t *c, const bb_component_frame_t *reply,
                     bb_component_info_t *info)
{
    if(reply->length != 2 + BB_COMPONENT_PEN_BYTES ||
       !bb_component_pen_decode(reply->payload + 2, BB_COMPONENT_PEN_BYTES, &info->pen))
        return fail(c, -EPROTO, "Invalid pen display response");
    info->pen_state_valid = true;
    return 0;
}

static int get_info(bb_component_client_t *c, bb_component_info_t *info,
                     int timeout_ms, unsigned attempts)
{
    bb_component_frame_t reply;
    int result = request(c, BB_COMPONENT_HELLO, NULL, 0, &reply, timeout_ms, attempts);
    if (result) return result;
    result = decode_info(c, reply.payload + 2, reply.length - 2, info);
    if (result) return result;
    if (info->motor_supported && info->healthy) {
        result = request(c, BB_COMPONENT_MOTOR_GET, NULL, 0, &reply, timeout_ms, attempts);
        if (result) return result;
        return motor_reply(c, &reply, info);
    }
    if(info->light_supported && info->healthy) {
        result = request(c, BB_COMPONENT_LIGHT_GET, NULL, 0, &reply, timeout_ms, attempts);
        return result ? result : light_reply(c, &reply, info);
    }
    if(info->pen_supported && info->healthy) {
        result = request(c, BB_COMPONENT_PEN_GET, NULL, 0, &reply, timeout_ms, attempts);
        return result ? result : pen_reply(c, &reply, info);
    }
    return 0;
}

int bb_component_client_status(bb_component_client_t *c, bb_component_info_t *info)
{
    return get_info(c, info, 1000, 3);
}

int bb_component_client_probe(bb_component_client_t *c, bb_component_info_t *info)
{
    return get_info(c, info, 250, 2);
}

static int flush_announcement_ack(bb_component_client_t *c)
{
    while (c->ack_sent < c->ack_length) {
        ssize_t n = write(c->fd, c->announcement_ack + c->ack_sent,
                           c->ack_length - c->ack_sent);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) return 0;
        if (n <= 0) return n < 0 ? -errno : -EIO;
        c->ack_sent += (size_t)n;
    }
    c->ack_sent = c->ack_length = 0;
    return 0;
}

int bb_component_client_receive_announcement(bb_component_client_t *c,
                                             bb_component_info_t *info)
{
    int result = flush_announcement_ack(c);
    if (result || c->ack_length) return result;
    bb_component_frame_t frame;
    /* Bound work per GUI tick even if the wire is noisy. */
    for (unsigned count = 0; count < 4096; ++count) {
        bool ready = bb_component_parser_next(&c->parser, &frame);
        if (!ready) {
            uint8_t byte;
            ssize_t n = read(c->fd, &byte, 1);
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && errno == EAGAIN) return 0;
            if (n < 0) return -errno;
            if (!n) return 0;
            const int64_t now = now_ms();
            if (now - c->last_byte_ms > 200) c->parser.length = 0;
            c->last_byte_ms = now;
            ready = bb_component_parser_push(&c->parser, byte, &frame);
        }
        if (!ready || frame.type != BB_COMPONENT_EVENT ||
            frame.command != BB_COMPONENT_ANNOUNCE ||
            frame.length != BB_COMPONENT_ANNOUNCE_BYTES) continue;
        if (decode_info(c, frame.payload, BB_COMPONENT_INFO_BYTES, info) || !info->boot_id)
            continue;
        if (info->motor_supported && info->healthy) {
            const uint8_t *motor = frame.payload + BB_COMPONENT_INFO_BYTES;
            if (motor[0] > BB_COMPONENT_MOTOR_CW || motor[1] > 100 ||
                (motor[0] != BB_COMPONENT_MOTOR_STOP && !motor[1])) continue;
            info->motor_direction = motor[0];
            info->motor_speed = motor[1];
        }
        frame.type = BB_COMPONENT_RESPONSE;
        frame.length = BB_COMPONENT_ANNOUNCE_ACK_BYTES;
        bb_component_put_u16(frame.payload, BB_COMPONENT_OK);
        bb_component_put_u64(frame.payload + 2, info->boot_id);
        c->ack_length = bb_component_encode(&frame, c->announcement_ack,
                                             sizeof(c->announcement_ack));
        c->ack_sent = 0;
        result = flush_announcement_ack(c);
        if (result) return result;
        c->error[0] = 0;
        return 1;
    }
    return 0;
}

int bb_component_client_motor_set(bb_component_client_t *c,
                                  uint8_t direction, uint8_t speed,
                                  bb_component_info_t *info)
{
    if (!info || direction > BB_COMPONENT_MOTOR_CW || speed > 100)
        return fail(c, -EINVAL, "Motor requires direction stop/ccw/cw and speed 0..100");
    if (info->profile != BB_COMPONENT_SHOWCASE_TURNTABLE || !info->motor_supported)
        return fail(c, -ENOTSUP, "Component firmware does not support motor control");
    if (!info->healthy) return fail(c, BB_COMPONENT_NOT_READY, "Component is not ready");
    const uint8_t payload[] = {direction, speed};
    bb_component_frame_t reply;
    int result = request(c, BB_COMPONENT_MOTOR_SET, payload, sizeof(payload), &reply, 1000, 3);
    return result ? result : motor_reply(c, &reply, info);
}

int bb_component_client_light_set(bb_component_client_t *c,
                                   const bb_component_light_settings_t *settings,
                                   bb_component_info_t *info)
{
    if(!info || !bb_component_light_valid(settings)) return fail(c, -EINVAL, "Invalid light settings");
    if(info->profile != BB_COMPONENT_AMBIENT_LIGHT || !info->light_supported)
        return fail(c, -ENOTSUP, "Component firmware does not support light control");
    if(!info->healthy) return fail(c, BB_COMPONENT_NOT_READY, "Component is not ready");
    uint8_t payload[BB_COMPONENT_LIGHT_BYTES];
    bb_component_light_encode(settings, payload);
    bb_component_frame_t reply;
    int result = request(c, BB_COMPONENT_LIGHT_SET, payload, sizeof(payload), &reply, 1000, 3);
    return result ? result : light_reply(c, &reply, info);
}

int bb_component_client_pen_set(bb_component_client_t *c, const bb_component_pen_t *state,
                                bb_component_info_t *info)
{
    if(!info || !bb_component_pen_valid(state)) return fail(c, -EINVAL, "Invalid pen settings");
    if(info->profile != BB_COMPONENT_ROUND_SCREEN || !info->pen_supported)
        return fail(c, -ENOTSUP, "Pen firmware upgrade required");
    if(!info->healthy) return fail(c, BB_COMPONENT_NOT_READY, "Pen is not ready");
    uint8_t payload[BB_COMPONENT_PEN_BYTES];
    bb_component_pen_encode(state, payload);
    bb_component_frame_t reply;
    int result = request(c, BB_COMPONENT_PEN_SET, payload, sizeof(payload), &reply, 1000, 3);
    return result ? result : pen_reply(c, &reply, info);
}

int bb_component_client_abort(bb_component_client_t *c)
{
    bb_component_info_t info;
    int result = bb_component_client_status(c, &info);
    if (result || info.state == BB_COMPONENT_IDLE) return result;
    uint8_t payload[8];
    bb_component_put_u64(payload, info.transfer_id);
    bb_component_frame_t reply;
    return request(c, BB_COMPONENT_TRANSFER_ABORT, payload, sizeof(payload), &reply, 1000, 3);
}

static int firmware_info(bb_component_client_t *c, FILE *file,
                          bb_component_image_t *image, uint32_t *total,
                          uint8_t digest[32])
{
    struct stat stat;
    if (fstat(fileno(file), &stat) || !S_ISREG(stat.st_mode) ||
        stat.st_size < BB_COMPONENT_IMAGE_PREFIX_BYTES ||
        (uint64_t)stat.st_size > UINT32_MAX)
        return fail(c, -EINVAL, "Firmware must be an ESP32-C3 application .bin");
    uint8_t bytes[4096];
    if (fread(bytes, 1, BB_COMPONENT_IMAGE_PREFIX_BYTES, file) !=
        BB_COMPONENT_IMAGE_PREFIX_BYTES ||
        !bb_component_image_parse(bytes, BB_COMPONENT_IMAGE_PREFIX_BYTES, image))
        return fail(c, -EINVAL, "Firmware has no supported BinBot ESP32-C3 profile");
    *total = (uint32_t)stat.st_size;
    rewind(file);
    EVP_MD_CTX *sha = EVP_MD_CTX_new();
    if (!sha) return fail(c, -ENOMEM, "Cannot allocate SHA-256 context");
    int ok = EVP_DigestInit_ex(sha, EVP_sha256(), NULL);
    size_t length;
    uint64_t hashed = 0;
    while (ok && (length = fread(bytes, 1, sizeof(bytes), file)) > 0) {
        hashed += length;
        ok = EVP_DigestUpdate(sha, bytes, length);
    }
    unsigned int digest_length = 0;
    ok = ok && !ferror(file) && hashed == *total &&
         EVP_DigestFinal_ex(sha, digest, &digest_length) && digest_length == 32;
    EVP_MD_CTX_free(sha);
    if (!ok) return fail(c, -EIO, "Cannot hash firmware");
    return 0;
}

static int random_id(bb_component_client_t *c, uint64_t *id)
{
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return fail(c, -errno, "Cannot open /dev/urandom");
    uint8_t *p = (uint8_t *)id;
    size_t left = sizeof(*id);
    while (left) {
        ssize_t n = read(fd, p, left);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            close(fd);
            return fail(c, -EIO, "Cannot generate transfer ID");
        }
        p += n;
        left -= (size_t)n;
    }
    close(fd);
    if (!*id) *id = 1;
    return 0;
}

static int verify_boot(bb_component_client_t *c, uint64_t old_boot,
                        const bb_component_image_t *image)
{
    const int64_t deadline = now_ms() + 35000;
    while (now_ms() < deadline) {
        bb_component_info_t info;
        const int result = get_info(c, &info, 1000, 1);
        if (!result && info.boot_id != old_boot && info.healthy) {
            if (info.profile == image->profile &&
                strcmp(info.project, image->project) == 0 &&
                strcmp(info.version, image->version) == 0 &&
                memcmp(info.elf_sha256, image->elf_sha256, 32) == 0) return 0;
            return fail(c, -EPROTO, "Device rebooted into another image (possible rollback)");
        }
        const struct timespec pause = {0, 250000000};
        nanosleep(&pause, NULL);
    }
    return fail(c, -ETIMEDOUT, "Firmware staged; new healthy boot was not confirmed");
}

static int checkpoint(bb_component_client_t *c, bb_component_phase_fn hook,
                      bb_component_phase_t phase, void *context)
{
    if (!hook || !hook(phase, context)) return 0;
    const int result = bb_component_client_abort(c);
    if (result) return result;
    return fail(c, -ECANCELED, "Transfer cancelled before activation");
}

int bb_component_client_ota_ex(bb_component_client_t *c, const char *firmware,
                              bool stage_only, bb_component_progress_fn progress,
                              bb_component_phase_fn phase, void *context)
{
    FILE *file = fopen(firmware, "rb");
    if (!file) return fail(c, -errno, "Cannot open firmware");
    bb_component_image_t image;
    uint32_t total;
    uint8_t digest[32];
    bb_component_frame_t reply;
    bb_component_info_t info;
    int result = firmware_info(c, file, &image, &total, digest);
    if (result) goto done;
    result = checkpoint(c, phase, BB_COMPONENT_PHASE_PREPARING, context);
    if (result) goto done;
    result = bb_component_client_status(c, &info);
    if (result) goto done;
    if (!info.healthy) {
        result = fail(c, BB_COMPONENT_NOT_READY, "Device has not completed initialization");
        goto done;
    }
    if (info.profile != image.profile || strcmp(info.project, image.project) != 0) {
        result = fail(c, BB_COMPONENT_WRONG_IMAGE, "Firmware profile does not match device");
        goto done;
    }
    if (total > info.capacity) {
        result = fail(c, BB_COMPONENT_TOO_LARGE, "Firmware does not fit inactive OTA slot");
        goto done;
    }
    uint64_t id;
    if (info.state != BB_COMPONENT_IDLE) {
        uint8_t query[8];
        bb_component_put_u64(query, info.transfer_id);
        result = request(c, BB_COMPONENT_TRANSFER_QUERY, query, sizeof(query), &reply, 1000, 3);
        if (result) goto done;
        if (reply.length != 2 + BB_COMPONENT_QUERY_BYTES ||
            bb_component_get_u32(reply.payload + 10) != total ||
            memcmp(reply.payload + 22, digest, 32) != 0 ||
            reply.payload[2] == BB_COMPONENT_ACTIVATED) {
            result = fail(c, BB_COMPONENT_BUSY, "Another transfer is active; inspect status or abort it");
            goto done;
        }
        id = info.transfer_id;
    } else {
        result = random_id(c, &id);
        if (result) goto done;
    }
    uint8_t begin[BB_COMPONENT_BEGIN_BYTES] = {0};
    bb_component_put_u64(begin, id);
    begin[8] = 1;
    begin[9] = image.profile;
    bb_component_put_u32(begin + 12, total);
    memcpy(begin + 16, digest, 32);
    result = request(c, BB_COMPONENT_TRANSFER_BEGIN, begin, sizeof(begin), &reply, 30000, 3);
    if (result) goto done;
    if (reply.length != 6) {
        result = fail(c, -EPROTO, "Invalid BEGIN response");
        goto done;
    }
    uint32_t offset = bb_component_get_u32(reply.payload + 2);
    if (offset > total || (offset != total && offset % BB_COMPONENT_CHUNK_BYTES)) {
        result = fail(c, -EPROTO, "Invalid resume offset");
        goto done;
    }
    if (fseek(file, (long)offset, SEEK_SET)) {
        result = fail(c, -EIO, "Cannot seek firmware");
        goto done;
    }
    if (progress) progress(offset, total, context);
    while (offset < total) {
        result = checkpoint(c, phase, BB_COMPONENT_PHASE_TRANSFERRING, context);
        if (result) goto done;
        uint8_t data[12 + BB_COMPONENT_CHUNK_BYTES];
        bb_component_put_u64(data, id);
        bb_component_put_u32(data + 8, offset);
        const size_t count = total - offset < BB_COMPONENT_CHUNK_BYTES ?
                                total - offset : BB_COMPONENT_CHUNK_BYTES;
        if (fread(data + 12, 1, count, file) != count) {
            result = fail(c, -EIO, "Cannot read firmware chunk");
            goto done;
        }
        result = request(c, BB_COMPONENT_TRANSFER_DATA, data, 12 + count, &reply, 1000, 3);
        if (result) goto done;
        if (reply.length != 6 || bb_component_get_u32(reply.payload + 2) != offset + count) {
            result = fail(c, -EPROTO, "Device acknowledged an unexpected offset");
            goto done;
        }
        offset += (uint32_t)count;
        if (progress) progress(offset, total, context);
    }
    uint8_t control[8];
    bb_component_put_u64(control, id);
    result = checkpoint(c, phase, BB_COMPONENT_PHASE_VALIDATING, context);
    if (result) goto done;
    result = request(c, BB_COMPONENT_TRANSFER_COMMIT, control, sizeof(control), &reply, 30000, 3);
    if (result || stage_only) goto done;
    result = checkpoint(c, phase, BB_COMPONENT_PHASE_ACTIVATING, context);
    if (result) goto done;
    result = request(c, BB_COMPONENT_OTA_ACTIVATE, control, sizeof(control), &reply, 1000, 1);
    /* A lost activation ACK can be resolved by observing the new boot. */
    if (result > 0) goto done;
    if (phase) (void)phase(BB_COMPONENT_PHASE_VERIFYING, context);
    result = verify_boot(c, info.boot_id, &image);
done:
    fclose(file);
    return result;
}

int bb_component_client_ota(bb_component_client_t *c, const char *firmware,
                            bool stage_only, bb_component_progress_fn progress,
                            void *context)
{
    return bb_component_client_ota_ex(c, firmware, stage_only, progress, NULL, context);
}

static int file_info(bb_component_client_t *c, FILE *file, uint16_t width, uint16_t height,
                     uint32_t *total, uint8_t digest[32])
{
    struct stat stat;
    if (fstat(fileno(file), &stat) || !S_ISREG(stat.st_mode) ||
        stat.st_size < 13 || (uint64_t)stat.st_size > BB_COMPONENT_FILE_MAX_BYTES)
        return fail(c, -EINVAL, "GIF must be a regular file no larger than 512 KiB");
    uint8_t header[13];
    if (fread(header, 1, sizeof(header), file) != sizeof(header) ||
        (memcmp(header, "GIF87a", 6) != 0 && memcmp(header, "GIF89a", 6) != 0) ||
        bb_component_get_u16(header + 6) != width ||
        bb_component_get_u16(header + 8) != height) {
        char message[80];
        snprintf(message, sizeof(message), "GIF must be a %ux%u GIF87a/GIF89a image", width, height);
        return fail(c, -EINVAL, message);
    }
    *total = (uint32_t)stat.st_size;
    rewind(file);
    EVP_MD_CTX *sha = EVP_MD_CTX_new();
    if (!sha) return fail(c, -ENOMEM, "Cannot allocate SHA-256 context");
    uint8_t bytes[4096];
    size_t length;
    uint64_t hashed = 0;
    int ok = EVP_DigestInit_ex(sha, EVP_sha256(), NULL);
    while (ok && (length = fread(bytes, 1, sizeof(bytes), file)) > 0) {
        hashed += length;
        ok = EVP_DigestUpdate(sha, bytes, length);
    }
    unsigned int digest_length = 0;
    ok = ok && !ferror(file) && hashed == *total &&
         EVP_DigestFinal_ex(sha, digest, &digest_length) && digest_length == 32;
    EVP_MD_CTX_free(sha);
    if (!ok) return fail(c, -EIO, "Cannot hash GIF");
    return 0;
}

static int file_abort(bb_component_client_t *c, uint64_t id)
{
    uint8_t control[8];
    bb_component_put_u64(control, id);
    bb_component_frame_t reply;
    return request(c, BB_COMPONENT_FILE_ABORT, control, sizeof(control),
                   &reply, 1000, 3);
}

static int file_checkpoint(bb_component_client_t *c, bb_component_phase_fn phase,
                           bb_component_phase_t value, void *context,
                           uint64_t id)
{
    if (!phase || !phase(value, context)) return 0;
    (void)file_abort(c, id);
    return fail(c, -ECANCELED, "GIF transfer cancelled");
}

int bb_component_client_file_update_ex(bb_component_client_t *c,
                                       const char *name, const char *path,
                                       bb_component_progress_fn progress,
                                       bb_component_phase_fn phase,
                                       void *context)
{
    const uint8_t file_id = bb_component_file_id_from_name(name);
    if (!file_id) return fail(c, -EINVAL, "Unsupported component display asset");
    FILE *file = fopen(path, "rb");
    if (!file) return fail(c, -errno, "Cannot open GIF");
    uint32_t total = 0;
    uint8_t digest[32];
    bb_component_frame_t reply;
    uint64_t id = 0;
    bool active = false;
    bb_component_info_t info;
    int result = bb_component_client_status(c, &info);
    if (result) goto done;
    uint16_t width, height = 240;
    if (info.profile == BB_COMPONENT_ROUND_SCREEN) width = 240;
    else if (info.profile == BB_COMPONENT_HOLOGRAPHIC_SCREEN) width = 320;
    else {
        result = fail(c, -ENOTSUP, "Component does not support a display GIF");
        goto done;
    }
    result = file_info(c, file, width, height, &total, digest);
    if (result) goto done;
    result = random_id(c, &id);
    if (result) goto done;

    uint8_t begin[BB_COMPONENT_FILE_BEGIN_BYTES] = {0};
    bb_component_put_u64(begin, id);
    begin[8] = file_id;
    begin[9] = BB_COMPONENT_FILE_TYPE_GIF;
    bb_component_put_u32(begin + 12, total);
    memcpy(begin + 16, digest, sizeof(digest));
    result = request(c, BB_COMPONENT_FILE_BEGIN, begin, sizeof(begin),
                     &reply, 30000, 3);
    if (result) goto done;
    active = true;
    if (reply.length != 6) {
        result = fail(c, -EPROTO, "Invalid GIF BEGIN response");
        goto done;
    }
    uint32_t offset = bb_component_get_u32(reply.payload + 2);
    if (offset > total || (offset != total && offset % BB_COMPONENT_CHUNK_BYTES)) {
        result = fail(c, -EPROTO, "Invalid GIF resume offset");
        goto done;
    }
    if (fseek(file, (long)offset, SEEK_SET)) {
        result = fail(c, -EIO, "Cannot seek GIF");
        goto done;
    }
    if (progress) progress(offset, total, context);
    while (offset < total) {
        result = file_checkpoint(c, phase, BB_COMPONENT_PHASE_TRANSFERRING,
                                 context, id);
        if (result) goto done;
        uint8_t data[12 + BB_COMPONENT_CHUNK_BYTES];
        bb_component_put_u64(data, id);
        bb_component_put_u32(data + 8, offset);
        const size_t count = total - offset < BB_COMPONENT_CHUNK_BYTES ?
                             total - offset : BB_COMPONENT_CHUNK_BYTES;
        if (fread(data + 12, 1, count, file) != count) {
            result = fail(c, -EIO, "Cannot read GIF chunk");
            goto done;
        }
        result = request(c, BB_COMPONENT_FILE_DATA, data, 12 + count,
                         &reply, 2000, 3);
        if (result) goto done;
        if (reply.length != 6 ||
            bb_component_get_u32(reply.payload + 2) != offset + count) {
            result = fail(c, -EPROTO, "Device acknowledged an unexpected GIF offset");
            goto done;
        }
        offset += (uint32_t)count;
        if (progress) progress(offset, total, context);
    }
    uint8_t control[8];
    bb_component_put_u64(control, id);
    result = file_checkpoint(c, phase, BB_COMPONENT_PHASE_VALIDATING,
                             context, id);
    if (result) goto done;
    result = request(c, BB_COMPONENT_FILE_COMMIT, control, sizeof(control),
                     &reply, 30000, 3);
    if (!result && reply.length != 6)
        result = fail(c, -EPROTO, "Invalid GIF COMMIT response");
done:
    if (result && active) {
        /* A successful ABORT must not erase the transfer's original error. */
        char error[sizeof(c->error)];
        memcpy(error, c->error, sizeof(error));
        (void)file_abort(c, id);
        memcpy(c->error, error, sizeof(c->error));
    }
    fclose(file);
    return result;
}

int bb_component_client_file_update(bb_component_client_t *c,
                                    const char *name, const char *path,
                                    bb_component_progress_fn progress,
                                    void *context)
{
    return bb_component_client_file_update_ex(c, name, path, progress, NULL,
                                              context);
}
