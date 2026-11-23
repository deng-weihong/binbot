#include "binbot/wifi_manager.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <pthread.h>

enum {
    WIFI_JOB_NONE,
    WIFI_JOB_SCAN,
    WIFI_JOB_CONNECT,
    WIFI_SCAN_SETTLE_MS = 800,
    WIFI_CONNECT_TIMEOUT_MS = 15000,
};

struct binbot_wifi_manager {
    pthread_mutex_t mutex;
    pthread_t worker;
    bool worker_running;
    bool worker_created;
    bool stop_requested;
    int job;
    char connect_ssid[BINBOT_WIFI_SSID_MAX];
    char connect_password[128];
    binbot_wifi_network_t networks[BINBOT_WIFI_MAX_NETWORKS];
    size_t network_count;
    binbot_wifi_state_t state;
    char connected_ssid[BINBOT_WIFI_SSID_MAX];
    char error[BINBOT_WIFI_ERROR_MAX];
};

static const char *wifi_interface(void)
{
    const char *value = getenv("BINBOT_WIFI_INTERFACE");
    return value && value[0] ? value : "wlan0";
}

static void set_error_locked(binbot_wifi_manager_t *manager, const char *message)
{
    snprintf(manager->error, sizeof(manager->error), "%s", message ? message : "操作失败");
    manager->state = BINBOT_WIFI_ERROR;
}

/* Execute wpa_cli/udhcpc without a shell so network credentials cannot be
 * interpreted as command syntax. The command output is intentionally bounded. */
static int run_command(char *const argv[], char *output, size_t output_size)
{
    int pipe_fds[2];
    if(pipe(pipe_fds) != 0) return -1;
    pid_t child = fork();
    if(child < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return -1;
    }
    if(child == 0) {
        int null_fd = open("/dev/null", O_WRONLY);
        close(pipe_fds[0]);
        dup2(pipe_fds[1], STDOUT_FILENO);
        if(null_fd >= 0) dup2(null_fd, STDERR_FILENO);
        if(null_fd > STDERR_FILENO) close(null_fd);
        close(pipe_fds[1]);
        execvp(argv[0], argv);
        _exit(127);
    }

    close(pipe_fds[1]);
    size_t used = 0;
    if(output && output_size > 0) output[0] = '\0';
    char buffer[256];
    ssize_t received;
    while((received = read(pipe_fds[0], buffer, sizeof(buffer))) > 0) {
        if(output && output_size > 1 && used < output_size - 1) {
            size_t copy = (size_t)received;
            if(copy > output_size - 1 - used) copy = output_size - 1 - used;
            memcpy(output + used, buffer, copy);
            used += copy;
            output[used] = '\0';
        }
    }
    close(pipe_fds[0]);
    int status = 0;
    while(waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    if(!WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

static int run_wpa(const char *command, const char *argument,
                   char *output, size_t output_size)
{
    char interface_arg[32];
    snprintf(interface_arg, sizeof(interface_arg), "-i%s", wifi_interface());
    char *argv[5] = {"wpa_cli", interface_arg, (char *)command, (char *)argument, NULL};
    if(!argument) argv[3] = NULL;
    int result = run_command(argv, output, output_size);
    /* wpa_cli commonly exits 0 even when the control interface replies with
     * `FAIL`; treat that reply as a failed operation for every mutating call. */
    if(result == 0 && output && strncmp(output, "FAIL", 4) == 0) return 1;
    return result;
}

static bool command_ok(int result, const char *output)
{
    return result == 0 && (!output || strncmp(output, "FAIL", 4) != 0);
}

static bool wpa_quote(const char *value, char *quoted, size_t capacity)
{
    if(!value || !quoted || capacity < 3) return false;
    size_t used = 0;
    quoted[used++] = '"';
    for(const unsigned char *cursor = (const unsigned char *)value; *cursor; ++cursor) {
        if(*cursor < 0x20 || *cursor == 0x7f) return false;
        if(*cursor == '\\' || *cursor == '"') {
            if(used + 2 >= capacity) return false;
            quoted[used++] = '\\';
        }
        if(used + 1 >= capacity) return false;
        quoted[used++] = (char)*cursor;
    }
    if(used + 2 > capacity) return false;
    quoted[used++] = '"';
    quoted[used] = '\0';
    return true;
}

static bool parse_status(const char *text, bool *connected,
                         char *ssid, size_t ssid_size)
{
    if(connected) *connected = false;
    if(ssid && ssid_size > 0) ssid[0] = '\0';
    if(!text) return false;
    char copy[4096];
    snprintf(copy, sizeof(copy), "%s", text);
    char *save = NULL;
    for(char *line = strtok_r(copy, "\n", &save); line;
        line = strtok_r(NULL, "\n", &save)) {
        if(strncmp(line, "wpa_state=COMPLETED", 19) == 0 && connected) {
            *connected = true;
        }
        else if(strncmp(line, "ssid=", 5) == 0 && ssid && ssid_size > 0) {
            snprintf(ssid, ssid_size, "%s", line + 5);
        }
    }
    return true;
}

static void parse_scan_results(binbot_wifi_manager_t *manager, const char *text)
{
    manager->network_count = 0;
    if(!text) return;
    char copy[8192];
    snprintf(copy, sizeof(copy), "%s", text);
    char *save = NULL;
    for(char *line = strtok_r(copy, "\n", &save); line &&
        manager->network_count < BINBOT_WIFI_MAX_NETWORKS;
        line = strtok_r(NULL, "\n", &save)) {
        if(strncmp(line, "bssid", 5) == 0 || line[0] == '-' || line[0] == '\0') continue;

        char *fields[5] = {0};
        char *cursor = line;
        bool valid = true;
        for(size_t index = 0; index < 4; ++index) {
            fields[index] = cursor;
            char *separator = strchr(cursor, '\t');
            if(!separator) {
                valid = false;
                break;
            }
            *separator = '\0';
            cursor = separator + 1;
        }
        fields[4] = cursor;
        if(!valid || !fields[4][0]) continue;

        binbot_wifi_network_t *network = &manager->networks[manager->network_count];
        snprintf(network->ssid, sizeof(network->ssid), "%s", fields[4]);
        network->signal_dbm = (int)strtol(fields[2], NULL, 10);
        network->secured = strstr(fields[3], "WPA") != NULL ||
                           strstr(fields[3], "WEP") != NULL ||
                           strstr(fields[3], "SAE") != NULL;
        network->connected = strstr(fields[3], "[CURRENT]") != NULL;
        manager->network_count++;
    }
}

static bool manager_should_stop(binbot_wifi_manager_t *manager)
{
    pthread_mutex_lock(&manager->mutex);
    bool stop = manager->stop_requested;
    pthread_mutex_unlock(&manager->mutex);
    return stop;
}

static void finish_job(binbot_wifi_manager_t *manager, binbot_wifi_state_t state,
                       const char *error)
{
    pthread_mutex_lock(&manager->mutex);
    if(error) {
        set_error_locked(manager, error);
    }
    else {
        manager->state = state;
        manager->error[0] = '\0';
    }
    manager->worker_running = false;
    manager->job = WIFI_JOB_NONE;
    pthread_mutex_unlock(&manager->mutex);
}

static void do_scan(binbot_wifi_manager_t *manager)
{
    char output[8192];
    if(run_wpa("scan", NULL, output, sizeof(output)) != 0) {
        finish_job(manager, BINBOT_WIFI_ERROR, "无法启动 WiFi 扫描");
        return;
    }
    usleep(WIFI_SCAN_SETTLE_MS * 1000U);
    if(manager_should_stop(manager)) {
        finish_job(manager, BINBOT_WIFI_IDLE, NULL);
        return;
    }
    if(run_wpa("scan_results", NULL, output, sizeof(output)) != 0) {
        finish_job(manager, BINBOT_WIFI_ERROR, "无法读取 WiFi 列表");
        return;
    }

    pthread_mutex_lock(&manager->mutex);
    parse_scan_results(manager, output);
    char status_output[4096];
    pthread_mutex_unlock(&manager->mutex);
    bool connected = false;
    char connected_ssid[BINBOT_WIFI_SSID_MAX];
    if(run_wpa("status", NULL, status_output, sizeof(status_output)) == 0) {
        parse_status(status_output, &connected, connected_ssid, sizeof(connected_ssid));
    }
    pthread_mutex_lock(&manager->mutex);
    manager->connected_ssid[0] = '\0';
    if(connected) snprintf(manager->connected_ssid, sizeof(manager->connected_ssid),
                           "%s", connected_ssid);
    for(size_t index = 0; index < manager->network_count; ++index) {
        if(connected && strcmp(manager->networks[index].ssid, manager->connected_ssid) == 0) {
            manager->networks[index].connected = true;
        }
    }
    manager->state = connected ? BINBOT_WIFI_CONNECTED : BINBOT_WIFI_READY;
    manager->error[0] = '\0';
    manager->worker_running = false;
    manager->job = WIFI_JOB_NONE;
    pthread_mutex_unlock(&manager->mutex);
}

static bool parse_network_id(const char *text, int *network_id)
{
    if(!text || !network_id) return false;
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if(end == text || value < 0 || value > 100000) return false;
    *network_id = (int)value;
    return true;
}

static void do_connect(binbot_wifi_manager_t *manager, const char *ssid,
                       const char *password)
{
    char output[4096];
    int network_id = -1;
    if(run_wpa("add_network", NULL, output, sizeof(output)) != 0 ||
       !parse_network_id(output, &network_id)) {
        finish_job(manager, BINBOT_WIFI_ERROR, "无法创建 WiFi 配置");
        return;
    }

    char id_text[24];
    snprintf(id_text, sizeof(id_text), "%d", network_id);
    char quoted_ssid[BINBOT_WIFI_SSID_MAX * 2 + 4];
    char quoted_password[260];
    if(!wpa_quote(ssid, quoted_ssid, sizeof(quoted_ssid))) {
        run_wpa("remove_network", id_text, NULL, 0);
        finish_job(manager, BINBOT_WIFI_ERROR, "WiFi 名称或密码包含不支持的字符");
        return;
    }
    char *set_ssid[] = {"wpa_cli", NULL, "set_network", id_text, "ssid", quoted_ssid, NULL};
    char interface_arg[32];
    snprintf(interface_arg, sizeof(interface_arg), "-i%s", wifi_interface());
    set_ssid[1] = interface_arg;
    if(!command_ok(run_command(set_ssid, output, sizeof(output)), output)) {
        run_wpa("remove_network", id_text, NULL, 0);
        finish_job(manager, BINBOT_WIFI_ERROR, "无法设置 WiFi 名称");
        return;
    }
    if(password[0]) {
        if(!wpa_quote(password, quoted_password, sizeof(quoted_password))) {
            run_wpa("remove_network", id_text, NULL, 0);
            finish_job(manager, BINBOT_WIFI_ERROR, "WiFi 名称或密码包含不支持的字符");
            return;
        }
        char *set_psk[] = {"wpa_cli", interface_arg, "set_network", id_text,
                           "psk", quoted_password, NULL};
        if(!command_ok(run_command(set_psk, output, sizeof(output)), output)) {
            run_wpa("remove_network", id_text, NULL, 0);
            finish_job(manager, BINBOT_WIFI_ERROR, "无法设置 WiFi 密码");
            return;
        }
    }
    else {
        char *set_open[] = {"wpa_cli", interface_arg, "set_network", id_text,
                            "key_mgmt", "NONE", NULL};
        if(!command_ok(run_command(set_open, output, sizeof(output)), output)) {
            run_wpa("remove_network", id_text, NULL, 0);
            finish_job(manager, BINBOT_WIFI_ERROR, "无法设置开放网络");
            return;
        }
    }
    char *enable[] = {"wpa_cli", interface_arg, "enable_network", id_text, NULL};
    char *select[] = {"wpa_cli", interface_arg, "select_network", id_text, NULL};
    if(!command_ok(run_command(enable, output, sizeof(output)), output) ||
       !command_ok(run_command(select, output, sizeof(output)), output)) {
        run_wpa("remove_network", id_text, NULL, 0);
        finish_job(manager, BINBOT_WIFI_ERROR, "无法切换 WiFi 网络");
        return;
    }
    /* Persist only after the live network has been selected. Some images do
     * not enable update_config; a failed save must not hide a successful join. */
    (void)run_wpa("save_config", NULL, output, sizeof(output));

    uint64_t waited_ms = 0;
    while(waited_ms < WIFI_CONNECT_TIMEOUT_MS && !manager_should_stop(manager)) {
        char status_output[4096];
        bool connected = false;
        char connected_ssid[BINBOT_WIFI_SSID_MAX];
        if(run_wpa("status", NULL, status_output, sizeof(status_output)) == 0) {
            parse_status(status_output, &connected, connected_ssid, sizeof(connected_ssid));
            if(connected && strcmp(connected_ssid, ssid) == 0) break;
        }
        usleep(500 * 1000U);
        waited_ms += 500;
    }
    if(manager_should_stop(manager)) {
        run_wpa("remove_network", id_text, NULL, 0);
        finish_job(manager, BINBOT_WIFI_IDLE, NULL);
        return;
    }
    char *dhcp[] = {"udhcpc", "-i", (char *)wifi_interface(), "-n", "-q", "-t", "3", "-T", "1", NULL};
    (void)run_command(dhcp, output, sizeof(output));

    char status_output[4096];
    bool connected = false;
    char connected_ssid[BINBOT_WIFI_SSID_MAX];
    if(run_wpa("status", NULL, status_output, sizeof(status_output)) == 0) {
        parse_status(status_output, &connected, connected_ssid, sizeof(connected_ssid));
    }
    if(!connected || strcmp(connected_ssid, ssid) != 0) {
        run_wpa("remove_network", id_text, NULL, 0);
        finish_job(manager, BINBOT_WIFI_ERROR, "WiFi 连接失败，请检查密码");
        return;
    }
    pthread_mutex_lock(&manager->mutex);
    snprintf(manager->connected_ssid, sizeof(manager->connected_ssid), "%s", ssid);
    for(size_t index = 0; index < manager->network_count; ++index) {
        manager->networks[index].connected =
            strcmp(manager->networks[index].ssid, ssid) == 0;
    }
    manager->state = BINBOT_WIFI_CONNECTED;
    manager->error[0] = '\0';
    manager->worker_running = false;
    manager->job = WIFI_JOB_NONE;
    pthread_mutex_unlock(&manager->mutex);
}

static void *wifi_worker(void *user_data)
{
    binbot_wifi_manager_t *manager = user_data;
    pthread_mutex_lock(&manager->mutex);
    int job = manager->job;
    char ssid[BINBOT_WIFI_SSID_MAX];
    char password[sizeof(manager->connect_password)];
    snprintf(ssid, sizeof(ssid), "%s", manager->connect_ssid);
    snprintf(password, sizeof(password), "%s", manager->connect_password);
    pthread_mutex_unlock(&manager->mutex);

    if(job == WIFI_JOB_SCAN) do_scan(manager);
    else if(job == WIFI_JOB_CONNECT) do_connect(manager, ssid, password);
    else finish_job(manager, BINBOT_WIFI_ERROR, "无效的 WiFi 操作");
    return NULL;
}

static bool start_job(binbot_wifi_manager_t *manager, int job,
                      const char *ssid, const char *password)
{
    pthread_t previous_worker;
    bool reap_previous = false;
    pthread_mutex_lock(&manager->mutex);
    if(manager->worker_running) {
        pthread_mutex_unlock(&manager->mutex);
        return false;
    }
    if(manager->worker_created) {
        previous_worker = manager->worker;
        manager->worker_created = false;
        reap_previous = true;
    }
    pthread_mutex_unlock(&manager->mutex);
    if(reap_previous) pthread_join(previous_worker, NULL);

    pthread_mutex_lock(&manager->mutex);
    if(manager->worker_running || manager->stop_requested) {
        pthread_mutex_unlock(&manager->mutex);
        return false;
    }
    manager->worker_running = true;
    manager->job = job;
    manager->error[0] = '\0';
    if(ssid) snprintf(manager->connect_ssid, sizeof(manager->connect_ssid), "%s", ssid);
    if(password) snprintf(manager->connect_password, sizeof(manager->connect_password),
                          "%s", password);
    manager->state = job == WIFI_JOB_SCAN ? BINBOT_WIFI_SCANNING : BINBOT_WIFI_CONNECTING;
    int result = pthread_create(&manager->worker, NULL, wifi_worker, manager);
    if(result != 0) {
        manager->worker_running = false;
        manager->job = WIFI_JOB_NONE;
        set_error_locked(manager, "无法启动 WiFi 后台任务");
        pthread_mutex_unlock(&manager->mutex);
        return false;
    }
    manager->worker_created = true;
    pthread_mutex_unlock(&manager->mutex);
    return true;
}

binbot_wifi_manager_t *binbot_wifi_manager_create(void)
{
    binbot_wifi_manager_t *manager = calloc(1, sizeof(*manager));
    if(!manager) return NULL;
    if(pthread_mutex_init(&manager->mutex, NULL) != 0) {
        free(manager);
        return NULL;
    }
    manager->state = BINBOT_WIFI_IDLE;
    return manager;
}

void binbot_wifi_manager_destroy(binbot_wifi_manager_t *manager)
{
    if(!manager) return;
    pthread_mutex_lock(&manager->mutex);
    manager->stop_requested = true;
    bool created = manager->worker_created;
    pthread_t worker = manager->worker;
    manager->worker_created = false;
    pthread_mutex_unlock(&manager->mutex);
    if(created) pthread_join(worker, NULL);
    pthread_mutex_destroy(&manager->mutex);
    free(manager);
}

bool binbot_wifi_manager_scan(binbot_wifi_manager_t *manager)
{
    return manager && start_job(manager, WIFI_JOB_SCAN, NULL, NULL);
}

bool binbot_wifi_manager_connect(binbot_wifi_manager_t *manager,
                                  const char *ssid, const char *password)
{
    if(!manager || !ssid || !ssid[0] || !password) return false;
    return start_job(manager, WIFI_JOB_CONNECT, ssid, password);
}

bool binbot_wifi_manager_snapshot(const binbot_wifi_manager_t *manager,
                                  binbot_wifi_network_t *networks,
                                  size_t network_capacity,
                                  size_t *network_count,
                                  binbot_wifi_state_t *state,
                                  char *connected_ssid,
                                  size_t connected_ssid_size,
                                  char *error,
                                  size_t error_size)
{
    if(!manager) return false;
    pthread_mutex_lock((pthread_mutex_t *)&manager->mutex);
    size_t count = manager->network_count;
    if(count > network_capacity) count = network_capacity;
    if(networks && count > 0) memcpy(networks, manager->networks,
                                     count * sizeof(*networks));
    if(network_count) *network_count = count;
    if(state) *state = manager->state;
    if(connected_ssid && connected_ssid_size > 0) {
        snprintf(connected_ssid, connected_ssid_size, "%s", manager->connected_ssid);
    }
    if(error && error_size > 0) snprintf(error, error_size, "%s", manager->error);
    pthread_mutex_unlock((pthread_mutex_t *)&manager->mutex);
    return true;
}
