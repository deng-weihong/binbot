/* Exercise the actual downloader's fork/wait/file lifecycle. Delaying the
 * executable makes the stale-cache race deterministic, independent of HTTP. */
#include "../../../libs/component_client/src/worker.c"
#include <assert.h>
#include <dirent.h>
#include <poll.h>

static void write_fixture(const char *path, size_t length, uint8_t byte)
{
    FILE *file = fopen(path, "wb");
    assert(file);
    for (size_t i = 0; i < length; ++i) assert(fputc(byte, file) != EOF);
    assert(fclose(file) == 0);
}

static void expect_no_partial(const char *directory)
{
    DIR *dir = opendir(directory);
    assert(dir);
    struct dirent *entry;
    while ((entry = readdir(dir))) assert(!strstr(entry->d_name, ".download-"));
    closedir(dir);
}

static void test_terminal_waits_for_worker_exit(const char *directory,
                                               uint32_t status)
{
    int sockets[2];
    /* The sandbox disallows manually sending SOCK_SEQPACKET packets; the
     * worker's recv path is identical for this ordered test event. */
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    binbot_component_event_t event = {.status = status, .event_seq = 1};
    event.request_id[0] = 0xaa;
    const ssize_t event_sent = write(sockets[1], &event, sizeof(event));
    assert(event_sent == sizeof(event));
    const pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        close(sockets[0]);
        close(sockets[1]);
        sleep(1);
        _exit(0);
    }
    bb_component_worker_t worker = {.fd = sockets[0], .pid = child};
    snprintf(worker.directory, sizeof(worker.directory), "%s", directory);
    close(sockets[1]);
    /* Success/probe must not reach the caller while the old worker holds UART. */
    assert(!bb_component_worker_poll(&worker, &event, 0));
    assert(worker.pid == child && worker.pending);
    bool emitted = false;
    for (unsigned retry = 0; retry < 1000 && !emitted; ++retry) {
        emitted = bb_component_worker_poll(&worker, &event, retry);
        const struct timespec pause = {0, 1000000};
        if (!emitted) nanosleep(&pause, NULL);
    }
    assert(emitted && event.status == status && worker.pid == 0);
    bb_component_worker_close(&worker);
}

int main(void)
{
    char directory[] = "/tmp/binbot-download-test-XXXXXX";
    assert(mkdtemp(directory));
    char executable[320], source[320], cached[320], state[320];
    path_for(executable, sizeof(executable), directory, "wget");
    path_for(source, sizeof(source), directory, "response");
    path_for(cached, sizeof(cached), directory, "component.gif");
    path_for(state, sizeof(state), directory, "job.state");
    FILE *script = fopen(executable, "wb");
    assert(script);
    /* argv matches wget -q -T 15 -O output URL. */
    assert(fputs("#!/bin/sh\n/bin/sleep 0.2\n/bin/cp \"$6\" \"$5\"\n"
                 "exit \"${BINBOT_TEST_WGET_EXIT:-0}\"\n", script) >= 0);
    assert(fclose(script) == 0 && chmod(executable, 0700) == 0);
    assert(setenv("PATH", directory, 1) == 0);

    work_t work = {.directory = directory, .fd = -1};
    work.record.magic = 0x42434f32;
    work.record.size = sizeof(work.record);
    work.record.command.action = BINBOT_COMPONENT_FILE_UPDATE;
    work.record.command.size = 512;
    snprintf(work.record.command.url, sizeof(work.record.command.url), "%s", source);
    uint8_t bytes[512], digest[32];
    memset(bytes, 0x55, sizeof(bytes));
    unsigned int digest_size;
    assert(EVP_Digest(bytes, sizeof(bytes), digest, &digest_size, EVP_sha256(), NULL) == 1);
    assert(digest_size == sizeof(digest));
    hex_digest(work.record.command.sha256, digest);

    /* A previous, larger cache must not cancel a smaller download at startup. */
    write_fixture(cached, 16384, 0x77);
    write_fixture(source, sizeof(bytes), 0x55);
    assert(download(&work, cached) == 0);
    assert(check_file(&work, cached) == 0);
    expect_no_partial(directory);

    test_terminal_waits_for_worker_exit(directory, BINBOT_COMPONENT_SUCCEEDED);
    test_terminal_waits_for_worker_exit(directory, BINBOT_COMPONENT_PROBED);

    /* A nonzero exit is accepted only when all requested bytes match. */
    assert(setenv("BINBOT_TEST_WGET_EXIT", "1", 1) == 0);
    assert(download(&work, cached) == 0);
    assert(check_file(&work, cached) == 0);
    assert(unsetenv("BINBOT_TEST_WGET_EXIT") == 0);

    /* Failed downloads preserve the verified cache and remove partial files. */
    write_fixture(source, sizeof(bytes), 0x66);
    assert(download(&work, cached) == -EINVAL);
    assert(check_file(&work, cached) == 0);
    expect_no_partial(directory);
    write_fixture(source, 128, 0x55);
    assert(download(&work, cached) == -EINVAL);
    assert(check_file(&work, cached) == 0);
    expect_no_partial(directory);
    write_fixture(source, 2048, 0x55);
    assert(download(&work, cached) != 0);
    assert(check_file(&work, cached) == 0);
    expect_no_partial(directory);
    snprintf(work.record.command.url, sizeof(work.record.command.url), "file://%s", source);
    write_fixture(source, sizeof(bytes), 0x55);
    assert(download(&work, cached) == 0);
    assert(check_file(&work, cached) == 0);
    write_fixture(source, sizeof(bytes), 0x66);
    assert(download(&work, cached) == -EINVAL);
    assert(check_file(&work, cached) == 0);
    expect_no_partial(directory);
    snprintf(work.record.command.url, sizeof(work.record.command.url), "%s", source);
    work.record.command.cancel = 1;
    assert(download(&work, cached) == -ECANCELED);
    assert(check_file(&work, cached) == 0);
    expect_no_partial(directory);

    unlink(executable);
    unlink(source);
    unlink(cached);
    unlink(state);
    assert(rmdir(directory) == 0);
    puts("component download regression tests passed");
    return 0;
}
