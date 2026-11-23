#ifndef BINBOT_CONTROL_IPC_H
#define BINBOT_CONTROL_IPC_H
#include <stdbool.h>
#include <stddef.h>

#define BB_CONTROL_MAX_MESSAGE 16384
#define BB_CONTROL_DEFAULT_SOCKET "/tmp/binbot_control.sock"

typedef struct {
    int listener, peer;
    char path[108];
} bb_control_socket_t;

bool bb_control_socket_open(bb_control_socket_t *socket, const char *path);
void bb_control_socket_accept(bb_control_socket_t *socket);
void bb_control_socket_close(bb_control_socket_t *socket);
int bb_control_connect(const char *path);
int bb_control_receive(int fd, char *buffer, size_t capacity);
bool bb_control_send(int fd, const char *message);
#endif
