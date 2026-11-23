#include "camera_publish.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <unistd.h>

extern char **environ;

void binbot_camera_publisher_init(binbot_camera_publisher_t *publisher)
{
    memset(publisher, 0, sizeof(*publisher));
    publisher->binary = getenv("BINBOT_STREAM_FFMPEG");
    if(!publisher->binary || !publisher->binary[0])
        publisher->binary = "/userdata/binbot/ffmpeg";
    publisher->source = getenv("BINBOT_STREAM_SOURCE_URL");
    if(!publisher->source || !publisher->source[0])
        publisher->source = "rtsp://127.0.0.1:554/live/binbot";
    publisher->destination = getenv("BINBOT_STREAM_PUBLISH_URL");
}

bool binbot_camera_publisher_available(const binbot_camera_publisher_t *publisher)
{
    return publisher->destination &&
           strncmp(publisher->destination, "rtsp://", 7) == 0 &&
           access(publisher->binary, X_OK) == 0;
}

bool binbot_camera_publisher_lease(binbot_camera_publisher_t *publisher,
                                  uint32_t seconds, uint64_t now_ms)
{
    if(seconds > 90 || (seconds && seconds < 5) ||
       (seconds && !binbot_camera_publisher_available(publisher))) return false;
    publisher->lease_until_ms = seconds ? now_ms + seconds * 1000ULL : 0;
    binbot_camera_publisher_poll(publisher, now_ms);
    return true;
}

static void start_publisher(binbot_camera_publisher_t *publisher, uint64_t now_ms)
{
    char parent[32];
    snprintf(parent, sizeof(parent), "%ld", (long)getpid());
    /* Stream copy only: the RK_MPI camera service owns capture and encoding. */
    char *args[] = {
        "/proc/self/exe", "--camera-publisher", parent,
        (char *)publisher->binary, "-nostdin", "-hide_banner", "-loglevel", "quiet",
        "-rtsp_transport", "tcp", "-timeout", "10000000",
        "-i", (char *)publisher->source, "-map", "0:v:0", "-an", "-c:v", "copy",
        "-f", "rtsp", "-rtsp_transport", "tcp", "-rw_timeout", "10000000",
        (char *)publisher->destination, NULL,
    };
    posix_spawn_file_actions_t actions;
    int result = posix_spawn_file_actions_init(&actions);
    if(result == 0) {
        result = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        if(result == 0) result = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
        if(result == 0) result = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        if(result == 0) result = posix_spawn(&publisher->pid, "/proc/self/exe",
                                            &actions, NULL, args, environ);
        posix_spawn_file_actions_destroy(&actions);
    }
    publisher->retry_at_ms = now_ms + 5000;
    if(result != 0) {
        publisher->pid = 0;
        fprintf(stderr, "camera publisher: could not start (%d)\n", result);
    } else {
        fprintf(stdout, "camera publisher: started\n");
    }
}

int binbot_camera_publisher_exec(int argc, char **argv)
{
    if(argc < 2) return 1;
    char *end = NULL;
    long parent = strtol(argv[0], &end, 10);
    if(!end || *end || parent <= 1) return 1;
    /* Stop upload even if the dialog supervisor is killed before lease expiry. */
    if(prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || getppid() != (pid_t)parent) return 1;
    execve(argv[1], argv + 1, environ);
    return 1;
}

void binbot_camera_publisher_poll(binbot_camera_publisher_t *publisher, uint64_t now_ms)
{
    if(publisher->pid > 0) {
        pid_t result = waitpid(publisher->pid, NULL, WNOHANG);
        if(result == publisher->pid || (result < 0 && errno == ECHILD)) {
            publisher->pid = 0;
            publisher->kill_at_ms = 0;
            publisher->retry_at_ms = now_ms + 5000;
            fprintf(stdout, "camera publisher: stopped\n");
        }
    }
    if(publisher->lease_until_ms <= now_ms) {
        if(publisher->pid > 0) {
            if(!publisher->kill_at_ms) {
                (void)kill(publisher->pid, SIGTERM);
                publisher->kill_at_ms = now_ms + 2000;
            } else if(now_ms >= publisher->kill_at_ms) {
                (void)kill(publisher->pid, SIGKILL);
            }
        }
        return;
    }
    if(!publisher->pid && now_ms >= publisher->retry_at_ms)
        start_publisher(publisher, now_ms);
}

void binbot_camera_publisher_close(binbot_camera_publisher_t *publisher)
{
    if(publisher->pid > 0) {
        (void)kill(publisher->pid, SIGKILL);
        while(waitpid(publisher->pid, NULL, 0) < 0 && errno == EINTR) {}
        publisher->pid = 0;
    }
    publisher->lease_until_ms = 0;
}
