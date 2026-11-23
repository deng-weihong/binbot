#ifndef BINBOT_CAMERA_PUBLISH_H
#define BINBOT_CAMERA_PUBLISH_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct {
    const char *binary;
    const char *source;
    const char *destination;
    pid_t pid;
    uint64_t lease_until_ms;
    uint64_t retry_at_ms;
    uint64_t kill_at_ms;
} binbot_camera_publisher_t;

void binbot_camera_publisher_init(binbot_camera_publisher_t *publisher);
bool binbot_camera_publisher_available(const binbot_camera_publisher_t *publisher);
bool binbot_camera_publisher_lease(binbot_camera_publisher_t *publisher,
                                  uint32_t seconds, uint64_t now_ms);
void binbot_camera_publisher_poll(binbot_camera_publisher_t *publisher, uint64_t now_ms);
void binbot_camera_publisher_close(binbot_camera_publisher_t *publisher);
int binbot_camera_publisher_exec(int argc, char **argv);

#endif
