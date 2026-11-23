#include "binbot/agent/ipc.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    int fd;
} server_args_t;

static void *serve_once(void *opaque)
{
    server_args_t *args = opaque;
    char request[BINBOT_AGENT_REQUEST_MAX + 1];
    char error[128];
    size_t size = 0;
    binbot_agent_message_type_t type = BINBOT_AGENT_MSG_ERROR;
    assert(binbot_agent_ipc_read(args->fd, &type, request, sizeof(request),
                                 &size, 1000, error, sizeof(error)));
    assert(type == BINBOT_AGENT_MSG_REQUEST);
    assert(strcmp(request, "看到了什么？") == 0);
    const char response[] = "我看到一只杯子。";
    assert(binbot_agent_ipc_write(args->fd, BINBOT_AGENT_MSG_RESPONSE,
                                  response, strlen(response), 1000,
                                  error, sizeof(error)));
    close(args->fd);
    return NULL;
}

int main(void)
{
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    server_args_t args = {.fd = sockets[1]};
    pthread_t thread;
    assert(pthread_create(&thread, NULL, serve_once, &args) == 0);

    const char request[] = "看到了什么？";
    char error[128];
    if(!binbot_agent_ipc_write(sockets[0], BINBOT_AGENT_MSG_REQUEST,
                               request, strlen(request), 1000,
                               error, sizeof(error))) {
        fprintf(stderr, "request write failed: %s\n", error);
        assert(false);
    }
    char response[BINBOT_AGENT_RESPONSE_MAX + 1];
    size_t response_size = 0;
    binbot_agent_message_type_t type = BINBOT_AGENT_MSG_ERROR;
    assert(binbot_agent_ipc_read(sockets[0], &type, response,
                                 sizeof(response), &response_size, 1000,
                                 error, sizeof(error)));
    assert(type == BINBOT_AGENT_MSG_RESPONSE);
    assert(strcmp(response, "我看到一只杯子。") == 0);
    close(sockets[0]);
    assert(pthread_join(thread, NULL) == 0);
    puts("agent IPC tests passed");
    return 0;
}
