#define _GNU_SOURCE
#include "binbot/control/ipc.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

int bb_control_connect(const char *path)
{
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    if(!path || strlen(path) >= sizeof(address.sun_path)) return -1;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if(fd < 0) return -1;
    if(connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        int error = errno;
        close(fd); errno = error; return -1;
    }
    return fd;
}

bool bb_control_socket_open(bb_control_socket_t *s, const char *path)
{
    *s = (bb_control_socket_t){.listener = -1, .peer = -1};
    if(!path || strlen(path) >= sizeof(s->path)) return false;
    struct stat info;
    if(lstat(path, &info) == 0) {
        if(!S_ISSOCK(info.st_mode) || info.st_uid != geteuid()) return false;
        int probe = bb_control_connect(path);
        if(probe >= 0) { close(probe); return false; }
        if(errno != ECONNREFUSED && errno != ENOENT) return false;
        if(unlink(path) != 0) return false;
    }
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if(fd < 0) return false;
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
    if(bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0) { close(fd); return false; }
    if(chmod(path, 0600) != 0 || listen(fd, 1) != 0) { close(fd); unlink(path); return false; }
    s->listener = fd;
    snprintf(s->path, sizeof(s->path), "%s", path);
    return true;
}

void bb_control_socket_accept(bb_control_socket_t *s)
{
    if(s->listener < 0) return;
    int fd = accept4(s->listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if(fd < 0) return;
    struct ucred credentials;
    socklen_t size = sizeof(credentials);
    if(s->peer >= 0 || getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &size) != 0 ||
       credentials.uid != geteuid()) { close(fd); return; }
    s->peer = fd;
}

int bb_control_receive(int fd, char *buffer, size_t capacity)
{
    if(fd < 0 || capacity < 2) return -1;
    ssize_t count = recv(fd, buffer, capacity - 1, MSG_DONTWAIT | MSG_TRUNC);
    if(count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 0;
    if(count <= 0 || (size_t)count >= capacity) return -1;
    buffer[count] = '\0';
    return (int)count;
}

bool bb_control_send(int fd, const char *message)
{
    size_t size = message ? strlen(message) : 0;
    return fd >= 0 && size > 0 && size < BB_CONTROL_MAX_MESSAGE &&
           send(fd, message, size, MSG_DONTWAIT | MSG_NOSIGNAL) == (ssize_t)size;
}

void bb_control_socket_close(bb_control_socket_t *s)
{
    if(s->peer >= 0) close(s->peer);
    if(s->listener >= 0) { close(s->listener); unlink(s->path); }
    s->peer = s->listener = -1;
}
