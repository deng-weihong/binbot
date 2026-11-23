#define _DEFAULT_SOURCE
#include "binbot/component/client.h"
#include "binbot/component/cli.h"

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void usage(const char *program, bool app_entry)
{
    fprintf(stderr, "Usage: %s%s [--uart %s] [--stage-only] status|abort|ota firmware.bin|motor stop/ccw/cw 0..100\n",
            program, app_entry ? " --component" : "", BB_COMPONENT_DEFAULT_UART);
    fprintf(stderr, "       light on/off solid/breathe/flow/blink/beat R G B brightness speed minutes\n");
    fprintf(stderr, "       pen image/info both/time/weather\n");
}

static bool number(const char *text, unsigned maximum, unsigned *value)
{
    if(!text[0] || text[0] < '0' || text[0] > '9') return false;
    char *end;
    errno = 0;
    unsigned long parsed = strtoul(text, &end, 10);
    if(errno || *end || parsed > maximum) return false;
    *value = (unsigned)parsed;
    return true;
}

static void report_progress(uint32_t received, uint32_t total, void *context)
{
    unsigned *previous = context;
    const unsigned percent = (unsigned)((uint64_t)received * 100 / total);
    if (*previous != percent) {
        fprintf(stderr, "\rOTA %3u%% (%u/%u bytes)", percent, received, total);
        fflush(stderr);
        *previous = percent;
    }
}

int bb_component_cli_main(int argc, char **argv)
{
    const char *uart = BB_COMPONENT_DEFAULT_UART;
    bool stage_only = false;
    const bool app_entry = argc > 1 && strcmp(argv[1], "--component") == 0;
    int arg = app_entry ? 2 : 1;
    while (arg < argc && argv[arg][0] == '-') {
        if (strcmp(argv[arg], "--uart") == 0 && arg + 1 < argc) {
            uart = argv[arg + 1];
            arg += 2;
        } else if (strcmp(argv[arg], "--stage-only") == 0) {
            stage_only = true;
            ++arg;
        } else {
            usage(argv[0], app_entry);
            return strcmp(argv[arg], "--help") == 0 ? 0 : 2;
        }
    }
    if (arg >= argc) { usage(argv[0], app_entry); return 2; }
    const char *command = argv[arg++];
    const bool ota = strcmp(command, "ota") == 0;
    const bool motor = strcmp(command, "motor") == 0;
    const bool light = strcmp(command, "light") == 0;
    const bool pen = strcmp(command, "pen") == 0;
    if ((ota && arg != argc - 1) || (motor && arg != argc - 2) || (light && arg != argc - 8) || (pen && arg != argc - 2) ||
        (!ota && !motor && !light && !pen && arg != argc) ||
        (!ota && !motor && !light && !pen && strcmp(command, "status") != 0 && strcmp(command, "abort") != 0) ||
        (stage_only && !ota)) {
        usage(argv[0], app_entry);
        return 2;
    }
    unsigned direction = BB_COMPONENT_MOTOR_STOP, speed = 0;
    bb_component_light_settings_t settings = {0};
    const char *effects[] = {"solid", "breathe", "flow", "blink", "beat"};
    const char *modes[] = {"image", "info"}, *faces[] = {"both", "time", "weather"};
    unsigned mode = 0, face = 0;
    if(pen) {
        while(mode < 2 && strcmp(argv[arg],modes[mode])) ++mode;
        while(face < 3 && strcmp(argv[arg+1],faces[face])) ++face;
        if(mode == 2 || face == 3) { usage(argv[0],app_entry); return 2; }
    }
    if(light) {
        if(!strcmp(argv[arg], "on")) settings.enabled = 1;
        else if(strcmp(argv[arg], "off")) { usage(argv[0], app_entry); return 2; }
        unsigned effect = 0;
        while(effect < 5 && strcmp(argv[arg + 1], effects[effect])) ++effect;
        unsigned values[6];
        bool valid = effect < 5;
        for(unsigned i = 0; i < 6 && valid; ++i)
            valid = number(argv[arg + 2 + i], i < 3 ? 255 : i == 5 ? 120 : 100, &values[i]);
        if(!valid || values[4] < 5) { usage(argv[0], app_entry); return 2; }
        settings = (bb_component_light_settings_t){settings.enabled, effect, values[0], values[1],
                   values[2], values[3], values[4], values[5]};
    }
    if (motor) {
        if (!strcmp(argv[arg], "ccw")) direction = BB_COMPONENT_MOTOR_CCW;
        else if (!strcmp(argv[arg], "cw")) direction = BB_COMPONENT_MOTOR_CW;
        else if (strcmp(argv[arg], "stop")) { usage(argv[0], app_entry); return 2; }
        char *end;
        long value = strtol(argv[arg + 1], &end, 10);
        if (!argv[arg + 1][0] || *end || value < 0 || value > 100) { usage(argv[0], app_entry); return 2; }
        speed = (unsigned)value;
    }
    bb_component_client_t client;
    int result = bb_component_client_open(&client, uart);
    if (!result && ota) {
        unsigned percent = 101;
        result = bb_component_client_ota(&client, argv[arg], stage_only, report_progress, &percent);
        fprintf(stderr, "\n");
        if (!result) puts(stage_only ? "Firmware validated and staged; boot partition unchanged." :
                                      "OTA complete; new firmware boot and health confirmed.");
    } else if (!result && strcmp(command, "abort") == 0) {
        result = bb_component_client_abort(&client);
        if (!result) puts("Transfer aborted.");
    } else if (!result) {
        bb_component_info_t info;
        result = bb_component_client_status(&client, &info);
        if (!result && motor) result = bb_component_client_motor_set(&client, direction, speed, &info);
        if (!result && light) result = bb_component_client_light_set(&client, &settings, &info);
        if(!result && pen) {
            bb_component_pen_t state = info.pen;
            state.mode = mode; state.face = face;
            time_t now = time(NULL); struct tm local;
            if(now >= 1577836800 && (uint64_t)now <= UINT32_MAX && localtime_r(&now,&local)) {
                state.flags |= BB_PEN_TIME_VALID; state.unix_time = now; state.utc_offset = local.tm_gmtoff;
                bb_component_pen_advance(&state,0);
            }
            result = bb_component_client_pen_set(&client,&state,&info);
        }
        if (!result)
            printf("uart=%s profile=%u project=%s version=%s healthy=%u\n"
                   "boot=%016llx state=%u transfer=%016llx received=%u total=%u slot=%u\n",
                   uart, info.profile, info.project, info.version, info.healthy,
                   (unsigned long long)info.boot_id, info.state,
                   (unsigned long long)info.transfer_id, info.received, info.total, info.capacity);
        if (!result && info.motor_supported)
            printf("motor=%s speed=%u%%\n", info.motor_direction == BB_COMPONENT_MOTOR_CCW ? "ccw" :
                   info.motor_direction == BB_COMPONENT_MOTOR_CW ? "cw" : "stop", info.motor_speed);
        if(!result && info.light_state_valid)
            printf("light=%s effect=%s rgb=%u,%u,%u brightness=%u%% speed=%u%% timer=%umin remaining=%umin\n",
                   info.light.enabled ? "on" : "off", effects[info.light.effect], info.light.red,
                   info.light.green, info.light.blue, info.light.brightness, info.light.speed,
                   info.light.timer_minutes, info.light_remaining_minutes);
        if(!result && info.pen_state_valid)
            printf("pen=%s face=%s time=%u offset=%d weather=%s temperature=%d/10C\n",
                   modes[info.pen.mode], faces[info.pen.face], info.pen.unix_time, info.pen.utc_offset,
                   info.pen.flags & BB_PEN_WEATHER_VALID ? "current" : "unavailable", info.pen.temperature);
    }
    if (result) fprintf(stderr, "Component operation failed (%d): %s\n", result, client.error);
    bb_component_client_close(&client);
    return result ? 1 : 0;
}
