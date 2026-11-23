#ifndef BINBOT_COMPONENT_CLIENT_H
#define BINBOT_COMPONENT_CLIENT_H

#include <termios.h>
#include "binbot/component/protocol.h"
#include "binbot/component/light.h"
#include "binbot/component/pen.h"

#define BB_COMPONENT_DEFAULT_UART "/dev/ttyS3"

typedef struct {
    uint8_t profile;
    uint8_t state;
    bool healthy;
    uint64_t boot_id;
    uint32_t capacity;
    uint32_t received;
    uint32_t total;
    uint64_t transfer_id;
    char version[32];
    char project[32];
    uint8_t elf_sha256[32];
    bool motor_supported;
    uint8_t motor_direction;
    uint8_t motor_speed;
    bool light_supported, light_state_valid;
    bb_component_light_settings_t light;
    uint16_t light_remaining_minutes;
    bool pen_supported, pen_state_valid;
    bb_component_pen_t pen;
} bb_component_info_t;

typedef struct {
    int fd;
    uint16_t sequence;
    struct termios saved_termios;
    bb_component_parser_t parser;
    int64_t last_byte_ms;
    uint8_t announcement_ack[14 + BB_COMPONENT_ANNOUNCE_ACK_BYTES];
    size_t ack_length, ack_sent;
    char error[160];
} bb_component_client_t;

typedef void (*bb_component_progress_fn)(uint32_t received, uint32_t total,
                                         void *context);
typedef enum {
    BB_COMPONENT_PHASE_PREPARING = 1,
    BB_COMPONENT_PHASE_TRANSFERRING = 3,
    BB_COMPONENT_PHASE_VALIDATING = 4,
    BB_COMPONENT_PHASE_ACTIVATING = 5,
    BB_COMPONENT_PHASE_VERIFYING = 6,
} bb_component_phase_t;
/* Nonzero requests cancellation before activation; hooks run at ACK boundaries. */
typedef int (*bb_component_phase_fn)(bb_component_phase_t phase, void *context);

/* Return 0 on success, negative errno for local errors, positive wire status. */
int bb_component_client_open(bb_component_client_t *client, const char *uart);
void bb_component_client_close(bb_component_client_t *client);
int bb_component_client_status(bb_component_client_t *client,
                               bb_component_info_t *info);
/* Short retries for presence checks; OTA/control retain their normal timeout. */
int bb_component_client_probe(bb_component_client_t *client, bb_component_info_t *info);
/* Nonblocking: 1 for an identity announcement, 0 for no event, negative errno. */
int bb_component_client_receive_announcement(bb_component_client_t *client,
                                             bb_component_info_t *info);
int bb_component_client_abort(bb_component_client_t *client);
int bb_component_client_motor_set(bb_component_client_t *client,
                                  uint8_t direction, uint8_t speed,
                                  bb_component_info_t *info);
int bb_component_client_light_set(bb_component_client_t *client,
                                   const bb_component_light_settings_t *settings,
                                   bb_component_info_t *info);
int bb_component_client_pen_set(bb_component_client_t *client,
                                const bb_component_pen_t *state, bb_component_info_t *info);
int bb_component_client_ota(bb_component_client_t *client, const char *firmware,
                            bool stage_only, bb_component_progress_fn progress,
                            void *context);
int bb_component_client_ota_ex(bb_component_client_t *client, const char *firmware,
                              bool stage_only, bb_component_progress_fn progress,
                              bb_component_phase_fn phase, void *context);
/* Replace one of the component's named GIF assets without rebooting it. */
int bb_component_client_file_update(bb_component_client_t *client,
                                    const char *name, const char *path,
                                    bb_component_progress_fn progress,
                                    void *context);
int bb_component_client_file_update_ex(bb_component_client_t *client,
                                       const char *name, const char *path,
                                       bb_component_progress_fn progress,
                                       bb_component_phase_fn phase,
                                       void *context);

#endif
