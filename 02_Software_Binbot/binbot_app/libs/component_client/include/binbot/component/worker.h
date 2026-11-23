#ifndef BINBOT_COMPONENT_WORKER_H
#define BINBOT_COMPONENT_WORKER_H

#include <sys/types.h>
#include <stdbool.h>
#include "binbot/audio/protocol.h"
#include "binbot/component/client.h"

typedef struct {
    pid_t pid;
    int fd;
    char directory[256];
    binbot_component_event_t latest;
    int64_t last_send_ms;
    bool pending;
    uint8_t active_action;
    bb_component_client_t listener;
    int64_t listen_retry_ms;
    uint64_t announcement_sequence;
    bool command_pending;
    binbot_component_command_t pending_command;
    bool reexec;
} bb_component_worker_t;

void bb_component_worker_init(bb_component_worker_t *worker);
/* The executable must dispatch --component-worker before initializing threads or UI. */
void bb_component_worker_init_exec(bb_component_worker_t *worker);
int bb_component_worker_main(int argc, char **argv);
bool bb_component_worker_submit(bb_component_worker_t *worker,
                                 const binbot_component_command_t *command);
bool bb_component_worker_poll(bb_component_worker_t *worker,
                               binbot_component_event_t *event, int64_t now_ms);
bool bb_component_worker_receive_announcement(bb_component_worker_t *worker,
                                               binbot_component_event_t *event,
                                               int64_t now_ms);
void bb_component_worker_close(bb_component_worker_t *worker);

#endif
