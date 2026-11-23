#include "binbot/app_runtime.h"
#include "binbot/app_workspace.h"
#include "camera_publish.h"
#include "binbot/audio/client.h"
#include "binbot/component/cli.h"
#include "binbot/component/worker.h"
#include "binbot/control/service.h"
#include "binbot/control/ipc.h"
#include "binbot/platform.h"
#include "binbot/slave/ipc.h"
#include "binbot/ui_app.h"
#include "lvgl.h"

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t running = 1;

static uint64_t camera_now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000ULL + (uint64_t)now.tv_nsec / 1000000ULL;
}


typedef struct {
    app_runtime_t *runtime;
    ui_app_t *ui;
    binbot_audio_client_t *audio;
    app_workspace_t *workspace;
    bb_control_service_t *control;
    binbot_camera_publisher_t camera;
    uint64_t ipc_sequence;
    uint32_t revision;
    binbot_audio_remote_state_t last_state;
    bool last_state_valid;
    bool state_synced;
    uint32_t last_state_send_ms;
    binbot_audio_calendar_state_t calendar_state;
    bool calendar_state_valid;
    char display_asset[128];
    pid_t gif_pid;
    uint8_t gif_command_id[16];
    char gif_asset[72];
    char gif_temporary[384];
    char gif_final[320];
    bool module_receiving;
    uint8_t module_command_id[16];
    char module_asset_id[37];
    unsigned module_display_profile;
} remote_control_t;

static void stop_requested(int signal_number)
{
    (void)signal_number;
    running = 0;
}

static void handle_slave_packet(ui_app_t *ui, const binbot_slave_ipc_packet_t *packet)
{
    if(!ui || !packet) return;
    if(packet->type == BINBOT_SLAVE_IPC_MSG_RESPONSE && packet->command == BINBOT_SLAVE_CMD_MOTOR_PULSE) {
        if(packet->error_code == BINBOT_SLAVE_ERR_OK && packet->payload_length == 3 &&
           packet->payload[2] == BINBOT_SLAVE_ERR_OK) {
            unsigned duration = packet->payload[0] | (unsigned)packet->payload[1] << 8;
            fprintf(stdout, "main: vibration acknowledged duration=%u ms\n", duration);
        } else {
            fprintf(stderr, "main: vibration acknowledgement failed error=%u length=%u\n",
                    packet->error_code, (unsigned)packet->payload_length);
        }
        return;
    }
    if(packet->type == BINBOT_SLAVE_IPC_MSG_STATUS &&
       packet->command == BINBOT_SLAVE_CMD_GET_STATUS_ALL) {
        binbot_slave_status_all_t status;
        if(binbot_slave_status_decode(packet->payload, packet->payload_length, &status)) {
            ui_app_handle_encoder_count(ui, status.encoder_count, false);
        }
        return;
    }
    if(packet->type != BINBOT_SLAVE_IPC_MSG_EVENT ||
       packet->command != BINBOT_SLAVE_CMD_EVENT_REPORT) return;

    binbot_slave_event_report_t event_report;
    if(!binbot_slave_event_decode(packet->payload, packet->payload_length, &event_report)) {
        fprintf(stderr, "main: invalid slave event payload length %u\n",
                (unsigned)packet->payload_length);
        return;
    }
    if(event_report.event_flags & BINBOT_SLAVE_EVENT_COMPONENT_ADC_CHANGED)
        app_modules_request_probe(ui_app_modules(ui));
    if((event_report.event_flags & BINBOT_SLAVE_EVENT_INPUT_CHANGED) == 0) return;

    ui_app_handle_encoder_count(ui, event_report.encoder_count, true);

    bool encoder_down = (event_report.status_flags & 0x0001U) != 0;
    if(event_report.adc_key == 0 && !encoder_down) return;
    fprintf(stdout, "main: slave input event key=%u encoder=%u\n",
            (unsigned)event_report.adc_key, encoder_down ? 1U : 0U);
    ui_app_handle_slave_input(ui, event_report.adc_key, encoder_down);
}

static bool remote_send_packet(remote_control_t *remote,
                               binbot_audio_message_type_t type,
                               const void *payload, uint32_t payload_size)
{
    if(!remote || !payload || payload_size > sizeof(((binbot_audio_packet_t *)0)->payload)) {
        return false;
    }
    binbot_audio_packet_t packet;
    binbot_audio_packet_init(&packet, type, payload_size,
                             remote->ipc_sequence++, 0);
    memcpy(&packet.payload, payload, payload_size);
    return binbot_audio_client_send(remote->audio, &packet);
}

static void remote_send_result(remote_control_t *remote,
                               const uint8_t command_id[16], bool success,
                               const char *message)
{
    char uuid[37];
    snprintf(uuid, sizeof(uuid),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        command_id[0],command_id[1],command_id[2],command_id[3],command_id[4],command_id[5],
        command_id[6],command_id[7],command_id[8],command_id[9],command_id[10],command_id[11],
        command_id[12],command_id[13],command_id[14],command_id[15]);
    bb_control_service_complete_id(remote->control, uuid, success, message);
    binbot_audio_remote_result_t result;
    memset(&result, 0, sizeof(result));
    memcpy(result.command_id, command_id, sizeof(result.command_id));
    result.success = success ? 1U : 0U;
    snprintf(result.message, sizeof(result.message), "%s", message ? message : "");
    (void)remote_send_packet(remote, BINBOT_AUDIO_MSG_REMOTE_RESULT,
                             &result, sizeof(result));
}

static void remote_build_state(remote_control_t *remote,
                               binbot_audio_remote_state_t *state)
{
    memset(state, 0, sizeof(*state));
    const app_model_t *model = app_state_model(app_runtime_state(remote->runtime));
    if(model) {
        state->todo_count = (uint8_t)(model->todo_count <= 8U ? model->todo_count : 8U);
        for(size_t i = 0; i < state->todo_count; ++i) {
            snprintf(state->todos[i].text, sizeof(state->todos[i].text), "%s",
                     model->todos[i].text);
            state->todos[i].done = model->todos[i].done ? 1U : 0U;
        }
    }
    ui_app_music_snapshot_t music;
    ui_app_get_music_snapshot(remote->ui, &music);
    state->music_track_index = music.track_index;
    state->music_position_seconds = music.position_seconds;
    state->music_active = music.active ? 1U : 0U;
    state->music_playing = music.playing ? 1U : 0U;
    snprintf(state->display_asset, sizeof(state->display_asset), "%s",
             remote->display_asset);
}

static void remote_sync_state(remote_control_t *remote, bool force)
{
    binbot_audio_remote_state_t state;
    remote_build_state(remote, &state);
    bool changed = !remote->last_state_valid ||
                   memcmp(&state, &remote->last_state, sizeof(state)) != 0;
    if(changed) {
        remote->revision++;
        if(remote->revision == 0) remote->revision = 1;
        remote->last_state = state;
        remote->last_state_valid = true;
    }
    if((changed || force) && binbot_audio_client_fd(remote->audio) >= 0) {
        state.revision = remote->revision;
        remote->state_synced = remote_send_packet(
            remote, BINBOT_AUDIO_MSG_REMOTE_STATE, &state, sizeof(state));
        if(remote->state_synced) {
            remote->last_state_send_ms = platform_monotonic_ms();
        }
    }
}

static bool validate_gif_file(const char *path)
{
    struct stat status;
    if(stat(path, &status) != 0 || status.st_size < 6 ||
       status.st_size > 16 * 1024 * 1024) return false;
    FILE *file = fopen(path, "rb");
    if(!file) return false;
    char header[6];
    bool valid = fread(header, 1, sizeof(header), file) == sizeof(header) &&
                 (memcmp(header, "GIF87a", 6) == 0 ||
                  memcmp(header, "GIF89a", 6) == 0);
    fclose(file);
    return valid;
}

static bool remote_start_gif(remote_control_t *remote,
                             const binbot_audio_remote_command_t *command)
{
    if(remote->gif_pid > 0 || !command->resource[0]) return false;
    const char *asset_root = getenv("BINBOT_ASSET_ROOT");
    if(!asset_root || !asset_root[0]) asset_root = "/oem/assets";
    int final_length = snprintf(remote->gif_final, sizeof(remote->gif_final),
                                "%s/right_component_screen_216.gif", asset_root);
    int temporary_length = snprintf(
        remote->gif_temporary, sizeof(remote->gif_temporary),
        "%s.binbot-download-%ld", remote->gif_final, (long)getpid());
    if(final_length < 0 || (size_t)final_length >= sizeof(remote->gif_final) ||
       temporary_length < 0 ||
       (size_t)temporary_length >= sizeof(remote->gif_temporary)) return false;

    pid_t child = fork();
    if(child < 0) return false;
    if(child == 0) {
        execlp("wget", "wget", "-q", "-T", "20", "-t", "1", "-O",
               remote->gif_temporary, command->resource, (char *)NULL);
        _exit(127);
    }
    remote->gif_pid = child;
    memcpy(remote->gif_command_id, command->command_id,
           sizeof(remote->gif_command_id));
    snprintf(remote->gif_asset, sizeof(remote->gif_asset), "%s", command->text);
    return true;
}

static void remote_poll_gif(remote_control_t *remote)
{
    if(remote->gif_pid <= 0) return;
    int status = 0;
    pid_t result = waitpid(remote->gif_pid, &status, WNOHANG);
    if(result == 0) return;
    bool success = result == remote->gif_pid && WIFEXITED(status) &&
                   WEXITSTATUS(status) == 0 &&
                   validate_gif_file(remote->gif_temporary) &&
                   rename(remote->gif_temporary, remote->gif_final) == 0;
    if(success) {
        snprintf(remote->display_asset, sizeof(remote->display_asset), "%s",
                 remote->gif_asset);
        ui_app_reload_home_gif(remote->ui);
        remote_send_result(remote, remote->gif_command_id, true, "GIF replaced");
    }
    else {
        unlink(remote->gif_temporary);
        remote_send_result(remote, remote->gif_command_id, false,
                           "GIF download or validation failed");
    }
    remote->gif_pid = 0;
    remote_sync_state(remote, false);
}

static void remote_handle_command(remote_control_t *remote,
                                  const binbot_audio_remote_command_t *command)
{
    bool success = false;
    bool deferred = false;
    const char *message = "command rejected";
    app_state_t *state = app_runtime_state(remote->runtime);
    binbot_audio_remote_action_t action =
        (binbot_audio_remote_action_t)command->action;
    if(action >= BINBOT_REMOTE_MUSIC_PLAY &&
       action <= BINBOT_REMOTE_MUSIC_SELECT) {
        success = ui_app_apply_remote_music(remote->ui, action, command->value);
        message = success ? "music state updated" : "music command failed";
    }
    else if(action == BINBOT_REMOTE_TODO_ADD) {
        success = app_state_add_todo(state, command->text);
        message = success ? "todo added" : "todo list is full";
    }
    else if(action == BINBOT_REMOTE_TODO_SET_DONE) {
        success = app_state_set_todo_done(state, command->index, command->value != 0);
        message = success ? "todo updated" : "todo index is invalid";
    }
    else if(action == BINBOT_REMOTE_TODO_EDIT) {
        success = app_state_edit_todo(state, command->index, command->text);
        message = success ? "todo edited" : "todo index or text is invalid";
    }
    else if(action == BINBOT_REMOTE_TODO_DELETE) {
        success = app_state_delete_todo(state, command->index);
        message = success ? "todo deleted" : "todo index is invalid";
    }
    else if(action == BINBOT_REMOTE_DISPLAY_GIF) {
        success = remote_start_gif(remote, command);
        deferred = success;
        message = "another GIF update is running";
    }
    else if(action == BINBOT_REMOTE_MODULE_IMAGE) {
        success = !remote->module_receiving &&
                  app_modules_receive(ui_app_modules(remote->ui), command->resource);
        if(success) {
            memcpy(remote->module_command_id, command->command_id, 16);
            snprintf(remote->module_asset_id, sizeof(remote->module_asset_id), "%.36s", command->text);
            remote->module_display_profile = command->value;
            remote->module_receiving = true;
        }
        deferred = success;
        message = "module image receiver busy or unavailable";
    }
    if(success && !deferred) app_runtime_flush(remote->runtime);
    if(!deferred) {
        remote_send_result(remote, command->command_id, success, message);
        remote_sync_state(remote, false);
    }
}

static bool control_brightness(void *context, uint8_t value)
{
    remote_control_t *remote = context;
    app_state_t *state = app_runtime_state(remote->runtime);
    uint8_t previous = app_state_model(state)->brightness;
    if(!platform_set_brightness(value)) return false;
    app_state_set_brightness(state, value);
    if(app_runtime_flush(remote->runtime)) return true;
    (void)platform_set_brightness(previous);
    app_state_set_brightness(state, previous);
    return false;
}

static bool control_volume(void *context, uint8_t value, uint64_t sequence)
{
    remote_control_t *remote = context;
    return binbot_audio_client_request_volume(remote->audio, value, sequence);
}

static cJSON *control_snapshot(void *context)
{
    remote_control_t *remote = context;
    cJSON *snapshot = app_workspace_snapshot(remote->workspace);
    cJSON *features = cJSON_GetObjectItemCaseSensitive(snapshot, "features");
    if(features) cJSON_AddBoolToObject(features, "cameraAvailable", binbot_camera_publisher_available(&remote->camera));
    return snapshot;
}

static int control_command(void *context, const char *name, const cJSON *payload,
                           const char *id, uint64_t request, char *message, size_t capacity)
{
    remote_control_t *remote = context;
    if(!strcmp(name, "camera.publish")) {
        const cJSON *lease = cJSON_GetObjectItemCaseSensitive(payload, "lease_seconds");
        bool valid = cJSON_IsNumber(lease) && lease->valuedouble == lease->valueint &&
                     lease->valueint >= 0 && lease->valueint <= 60;
        bool ok = valid && binbot_camera_publisher_lease(&remote->camera, (uint32_t)lease->valueint, camera_now_ms());
        snprintf(message, capacity, "%s", ok ? "Camera lease applied" : "Camera publisher unavailable");
        return ok ? 1 : 0;
    }
    if(strcmp(name, "display.gif") && strcmp(name, "modules.image"))
        return app_workspace_command(remote->workspace, name, payload, id, request, message, capacity);
    const cJSON *path = cJSON_GetObjectItemCaseSensitive(payload, "asset_path");
    const cJSON *asset = cJSON_GetObjectItemCaseSensitive(payload, "asset_id");
    const char *dialog = getenv("BINBOT_DIALOG_URL");
    const char *scheme = dialog ? strstr(dialog, "://") : NULL;
    const char *authority = scheme ? scheme + 3 : NULL;
    const char *slash = authority ? strchr(authority, '/') : NULL;
    size_t host_length = authority ? (slash ? (size_t)(slash - authority) : strlen(authority)) : 0;
    if(!cJSON_IsString(path) || (strncmp(path->valuestring, "/v1/device/module-images/", 25) && strncmp(path->valuestring, "/v1/device/assets/", 18)) ||
       strstr(path->valuestring, "..") || !cJSON_IsString(asset) || !host_length || host_length > 200) {
        snprintf(message, capacity, "Invalid trusted asset path"); return 0;
    }
    binbot_audio_remote_command_t command = {0};
    command.action = !strcmp(name, "display.gif") ? BINBOT_REMOTE_DISPLAY_GIF : BINBOT_REMOTE_MODULE_IMAGE;
    unsigned offset = 0;
    for(const char *cursor = id; *cursor; ) {
        if(*cursor == '-') { cursor++; continue; }
        unsigned byte;
        if(offset >= 16 || sscanf(cursor, "%2x", &byte) != 1) return 0;
        command.command_id[offset++] = (uint8_t)byte; cursor += 2;
    }
    if(offset != 16) return 0;
    snprintf(command.text, sizeof(command.text), "%s", asset->valuestring);
    const cJSON *profile = cJSON_GetObjectItemCaseSensitive(payload, "display_profile");
    command.value = cJSON_IsNumber(profile) ? (uint32_t)profile->valueint : 0;
    int length = snprintf(command.resource, sizeof(command.resource), "%s://%.*s%s",
        !strncmp(dialog, "wss:", 4) || !strncmp(dialog, "https:", 6) ? "https" : "http",
        (int)host_length, authority, path->valuestring);
    if(length < 0 || (size_t)length >= sizeof(command.resource)) return 0;
    bool ok = false;
    if(command.action == BINBOT_REMOTE_DISPLAY_GIF) ok = remote_start_gif(remote, &command);
    else if(!remote->module_receiving && app_modules_receive(ui_app_modules(remote->ui), command.resource)) {
        memcpy(remote->module_command_id, command.command_id, 16);
        snprintf(remote->module_asset_id, sizeof(remote->module_asset_id), "%.36s", command.text);
        remote->module_display_profile = command.value;
        remote->module_receiving = true; ok = true;
    }
    snprintf(message, capacity, "%s", ok ? "Downloading image" : "Image receiver busy or unavailable");
    return ok ? 2 : 0;
}

int main(int argc, char **argv)
{
    if(argc > 1 && strcmp(argv[1], "--camera-publisher") == 0)
        return binbot_camera_publisher_exec(argc - 2, argv + 2);
    if(argc > 1 && strcmp(argv[1], "--component-worker") == 0) {
        return bb_component_worker_main(argc, argv);
    }
    if(argc > 1 && strcmp(argv[1], "--component") == 0) {
        return bb_component_cli_main(argc, argv);
    }
    signal(SIGINT, stop_requested);
    signal(SIGTERM, stop_requested);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    lv_init();
    if(!platform_initialize()) {
        lv_deinit();
        return EXIT_FAILURE;
    }

    const char *state_path = getenv("BINBOT_STATE_PATH");
    app_runtime_t *runtime = app_runtime_create(state_path);
    if(!runtime) {
        fprintf(stderr, "main: failed to create application runtime\n");
        platform_shutdown();
        lv_deinit();
        return EXIT_FAILURE;
    }

    ui_app_t *ui = ui_app_create(app_runtime_state(runtime));
    if(!ui) {
        fprintf(stderr, "main: failed to create presentation\n");
        app_runtime_destroy(runtime);
        platform_shutdown();
        lv_deinit();
        return EXIT_FAILURE;
    }

    const app_model_t *initial_model = app_state_model(app_runtime_state(runtime));
    if(initial_model && !platform_set_brightness(initial_model->brightness)) {
        fprintf(stderr, "main: failed to apply initial brightness %u%%\n",
                (unsigned)initial_model->brightness);
    }

    binbot_audio_client_t audio_client;
    const char *audio_socket = getenv("BINBOT_AUDIO_SOCKET");
    binbot_audio_client_init(&audio_client, BINBOT_AUDIO_ROLE_APP,
                             BINBOT_AUDIO_CAP_EVENTS | BINBOT_AUDIO_CAP_LEVEL |
                                 BINBOT_AUDIO_CAP_DIALOG_STATE |
                                 BINBOT_AUDIO_CAP_DIALOG_TEXT |
                                 BINBOT_AUDIO_CAP_CALENDAR_STATE |
                                 BINBOT_AUDIO_CAP_NOISE_STATE |
                                 BINBOT_AUDIO_CAP_REMOTE_COMMAND,
                             0, audio_socket);
    ui_app_set_audio_client(ui, &audio_client);

    remote_control_t remote = {
        .runtime = runtime,
        .ui = ui,
        .audio = &audio_client,
    };
    binbot_camera_publisher_init(&remote.camera);
    remote.workspace = app_workspace_create(runtime, ui, &audio_client, state_path);
    if(!remote.workspace) fprintf(stderr, "main: workspace state unavailable; extended controls disabled\n");
    const char *control_socket = getenv("BINBOT_CONTROL_SOCKET");
    const char *control_journal = getenv("BINBOT_CONTROL_JOURNAL");
    char default_journal[384];
    snprintf(default_journal, sizeof(default_journal), "%s.control.json",
             state_path && state_path[0] ? state_path : "/oem/data/binbot_state.ini");
    bb_control_service_t *control = bb_control_service_create(
        control_socket && *control_socket ? control_socket : BB_CONTROL_DEFAULT_SOCKET,
        control_journal && *control_journal ? control_journal : default_journal,
        (bb_control_hooks_t){.context=&remote, .brightness=control_brightness, .volume=control_volume,
            .command=remote.workspace ? control_command : NULL,
            .snapshot=remote.workspace ? control_snapshot : NULL});
    remote.control = control;
    app_workspace_set_control(remote.workspace, control);
    if(!control) fprintf(stderr, "main: independent control socket unavailable\n");
    bb_component_worker_t component_worker;
    bb_component_worker_init_exec(&component_worker);
    app_modules_set_worker(ui_app_modules(ui), &component_worker);
    snprintf(remote.display_asset, sizeof(remote.display_asset), "%s",
             "right_component_screen_216.gif");

    binbot_slave_client_t slave_client;
    const char *slave_socket = getenv("BINBOT_SLAVE_SOCKET");
    binbot_slave_client_init(&slave_client, BINBOT_SLAVE_IPC_ROLE_APP, slave_socket);
    ui_app_set_slave_client(ui, &slave_client);

    fprintf(stdout, "main: BinBot modular runtime started\n");
    bool volume_synced = false;
    uint8_t synced_volume = 0;
    uint32_t last_volume_request = 0;
    while(running) {
        uint32_t now = platform_monotonic_ms();
        binbot_audio_client_connect(&audio_client, now);
        if(binbot_audio_client_fd(&audio_client) < 0) {
            bb_control_service_audio_disconnected(control);
            volume_synced = false;
            remote.state_synced = false;
            ui_app_handle_dialog_state(ui, BINBOT_DIALOG_STATE_DISCONNECTED);
        }
        else {
            const app_model_t *model = app_state_model(app_runtime_state(runtime));
            if(model && !bb_control_volume_pending(control) &&
               now - last_volume_request >= 1000U &&
               (!volume_synced || synced_volume != model->volume) &&
               binbot_audio_client_set_volume(&audio_client, model->volume)) {
                last_volume_request = now;
                synced_volume = model->volume;
                volume_synced = true;
            }
            remote_sync_state(
                &remote,
                !remote.state_synced ||
                    now - remote.last_state_send_ms >= 5000U);
        }
        while(binbot_audio_client_fd(&audio_client) >= 0) {
            binbot_audio_packet_t packet;
            int received = binbot_audio_client_receive(&audio_client, &packet);
            if(received <= 0) break;
            app_workspace_audio(remote.workspace, &packet);
            if(packet.header.type == BINBOT_AUDIO_MSG_WAKE_DETECTED &&
               packet.header.payload_size == sizeof(packet.payload.wake)) {
                fprintf(stdout, "main: wake event %d at sequence %llu\n",
                        packet.payload.wake.hotword_index,
                        (unsigned long long)packet.header.sequence);
                ui_app_handle_wake_detected(ui, packet.payload.wake.hotword_index);
            }
            else if(packet.header.type == BINBOT_AUDIO_MSG_VOLUME_RESULT &&
                    packet.header.payload_size == sizeof(packet.payload.volume_result)) {
                const binbot_audio_volume_result_t *result = &packet.payload.volume_result;
                bool success = result->success && result->volume <= 100;
                if(success) {
                    app_state_set_volume(app_runtime_state(runtime), result->volume);
                    synced_volume = result->volume;
                    success = app_runtime_flush(runtime);
                }
                volume_synced = success;
                bb_control_service_volume_result(control, packet.header.sequence, result->volume, success);
            }
            else if(packet.header.type == BINBOT_AUDIO_MSG_NOISE_STATE &&
                    packet.header.payload_size == sizeof(packet.payload.noise)) {
                ui_app_handle_noise_state(ui, &packet.payload.noise);
            }
            else if(packet.header.type == BINBOT_AUDIO_MSG_DIALOG_STATE &&
                    packet.header.payload_size == sizeof(packet.payload.dialog_state)) {
                fprintf(stdout, "main: dialog state %u\n",
                        (unsigned)packet.payload.dialog_state.state);
                ui_app_handle_dialog_state(ui, packet.payload.dialog_state.state);
            }
            else if(packet.header.type == BINBOT_AUDIO_MSG_DIALOG_TEXT &&
                    packet.header.payload_size == sizeof(packet.payload.dialog_text)) {
                if(packet.header.flags & BINBOT_AUDIO_FLAG_DIALOG_PLAYBACK)
                    ui_app_handle_dialog_playback(ui, &packet.payload.dialog_text);
                else ui_app_handle_dialog_text(ui, &packet.payload.dialog_text);
            }
            else if(packet.header.type == BINBOT_AUDIO_MSG_REMOTE_COMMAND &&
                    packet.header.payload_size ==
                        sizeof(packet.payload.remote_command)) {
                remote_handle_command(&remote, &packet.payload.remote_command);
            }
            else if(packet.header.type == BINBOT_AUDIO_MSG_CALENDAR_STATE &&
                    packet.header.payload_size == sizeof(packet.payload.calendar_state)) {
                remote.calendar_state = packet.payload.calendar_state;
                remote.calendar_state_valid = true;
                (void)ui_app_handle_calendar_state(ui, &packet.payload.calendar_state);
                fprintf(stdout, "main: calendar revision=%u events=%u\n",
                        remote.calendar_state.revision,
                        (unsigned)remote.calendar_state.event_count);
            }
            else if(packet.header.type == BINBOT_AUDIO_MSG_COMPONENT_COMMAND &&
                    packet.header.payload_size == sizeof(packet.payload.component_command)) {
                const bool submitted = bb_component_worker_submit(&component_worker, &packet.payload.component_command);
                fprintf(stdout, "main: component action=%u submitted=%d\n",
                        (unsigned)packet.payload.component_command.action, submitted);
                if(!submitted && packet.payload.component_command.action == BINBOT_COMPONENT_PROBE) {
                    binbot_component_event_t event = {0};
                    memcpy(event.request_id, packet.payload.component_command.request_id, 16);
                    event.event_seq = 1;
                    event.status = BINBOT_COMPONENT_PROBED;
                    if(packet.payload.component_command.reserved[3] == 1) event.profile = 4;
                    snprintf(event.message, sizeof(event.message), "component worker busy or unavailable");
                    (void)remote_send_packet(&remote, BINBOT_AUDIO_MSG_COMPONENT_EVENT, &event, sizeof(event));
                }
            }
        }
        if(binbot_audio_client_fd(&audio_client) < 0) {
            bb_control_service_audio_disconnected(control);
            volume_synced = false;
            remote.state_synced = false;
            ui_app_handle_dialog_state(ui, BINBOT_DIALOG_STATE_DISCONNECTED);
        }
        binbot_slave_client_connect(&slave_client, now);
        app_firmware_tick(ui_app_firmware(ui), now);
        while(binbot_slave_client_fd(&slave_client) >= 0) {
            binbot_slave_ipc_packet_t packet;
            int received = binbot_slave_client_receive(&slave_client, &packet);
            if(received <= 0) break;
            if(!app_firmware_handle_packet(ui_app_firmware(ui), &packet))
                handle_slave_packet(ui, &packet);
        }
        app_runtime_tick(runtime, now);
        bb_control_service_tick(control, now, platform_reported_brightness());
        remote_poll_gif(&remote);
        binbot_camera_publisher_poll(&remote.camera, camera_now_ms());
        if(remote.module_receiving) {
            int received = app_modules_poll_receive(ui_app_modules(ui));
            if(received) {
                if(received > 0 && remote.module_display_profile &&
                   !app_modules_set_current(ui_app_modules(ui), remote.module_asset_id,
                                            remote.module_display_profile)) received = -1;
                remote_send_result(&remote, remote.module_command_id, received > 0,
                                   received > 0 ? "module gallery image received" : "module image download or validation failed");
                remote.module_receiving = false;
            }
        }
        binbot_component_event_t component_event;
        if(bb_component_worker_poll(&component_worker, &component_event, now)) {
            bool local = app_modules_handle_event(ui_app_modules(ui), &component_event);
            if(!local || component_event.status == BINBOT_COMPONENT_PROBED)
                (void)remote_send_packet(&remote, BINBOT_AUDIO_MSG_COMPONENT_EVENT,
                                         &component_event, sizeof(component_event));
        }
        if(bb_component_worker_receive_announcement(&component_worker, &component_event, now)) {
            fprintf(stdout, "main: component announcement profile=%u healthy=%u boot=%llu at=%u ms\n",
                    component_event.profile, component_event.healthy,
                    (unsigned long long)component_event.boot_id, now);
            (void)app_modules_handle_event(ui_app_modules(ui), &component_event);
            (void)remote_send_packet(&remote, BINBOT_AUDIO_MSG_COMPONENT_EVENT,
                                     &component_event, sizeof(component_event));
        }
        app_modules_tick(ui_app_modules(ui), now);
        app_workspace_tick(remote.workspace, now);
        uint32_t delay = lv_timer_handler();
        /* RV1106 has one CPU core.  A 30 fps display cadence leaves headroom
         * for audio and vision while keeping page transitions smooth. */
        if(delay == LV_NO_TIMER_READY || delay > 33) delay = 33;
        if(delay < 1) delay = 1;
        usleep(delay * 1000U);
    }

    if(remote.gif_pid > 0) {
        kill(remote.gif_pid, SIGTERM);
        (void)waitpid(remote.gif_pid, NULL, 0);
        unlink(remote.gif_temporary);
    }

    binbot_camera_publisher_close(&remote.camera);
    bb_component_worker_close(&component_worker);
    bb_control_service_destroy(control);
    binbot_audio_client_close(&audio_client);
    binbot_slave_client_close(&slave_client);
    ui_app_destroy(ui);
    app_workspace_destroy(remote.workspace);
    app_runtime_destroy(runtime);
    platform_shutdown();
    lv_deinit();
    fprintf(stdout, "main: stopped cleanly\n");
    return EXIT_SUCCESS;
}
