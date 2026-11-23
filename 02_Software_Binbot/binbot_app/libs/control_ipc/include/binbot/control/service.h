#ifndef BINBOT_CONTROL_SERVICE_H
#define BINBOT_CONTROL_SERVICE_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <cJSON.h>

typedef struct bb_control_service bb_control_service_t;
typedef struct {
    void *context;
    bool (*brightness)(void *context, uint8_t value);
    bool (*volume)(void *context, uint8_t value, uint64_t request);
    /* Return 0=failed, 1=completed, 2=awaiting an actual peripheral result. */
    int (*command)(void *context, const char *name, const cJSON *payload, const char *command_id,
                   uint64_t request, char *message, size_t capacity);
    cJSON *(*snapshot)(void *context);
} bb_control_hooks_t;

bb_control_service_t *bb_control_service_create(const char *socket, const char *journal,
                                               bb_control_hooks_t hooks);
void bb_control_service_destroy(bb_control_service_t *service);
void bb_control_service_tick(bb_control_service_t *service, uint32_t now, uint8_t brightness);
bool bb_control_volume_pending(const bb_control_service_t *service);
void bb_control_service_volume_result(bb_control_service_t *service, uint64_t request,
                                      uint8_t value, bool success);
void bb_control_service_audio_disconnected(bb_control_service_t *service);
void bb_control_service_complete(bb_control_service_t *service, uint64_t request,
                                 bool success, const char *message);
void bb_control_service_complete_id(bb_control_service_t *service, const char *command_id,
                                    bool success, const char *message);
#endif
