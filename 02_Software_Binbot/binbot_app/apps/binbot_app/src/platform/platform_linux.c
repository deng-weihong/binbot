#include "binbot/platform.h"

#include "lvgl.h"
#if defined(BINBOT_HOST_SIMULATOR)
#include "drivers/sdl/lv_sdl_keyboard.h"
#include "drivers/sdl/lv_sdl_mouse.h"
#include "drivers/sdl/lv_sdl_window.h"
#else
#include "drivers/display/fb/lv_linux_fbdev.h"
#include "drivers/evdev/lv_evdev.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint8_t reported_brightness;

static bool write_backlight_value(uint8_t percent)
{
#if defined(BINBOT_HOST_SIMULATOR)
    (void)percent;
    return true;
#else
    const char *directory = getenv("BINBOT_BACKLIGHT_PATH");
    if(!directory || !directory[0]) directory = "/sys/class/backlight/backlight";

    char brightness_path[256];
    char max_path[256];
    const size_t suffix_length = strlen("/brightness");
    const size_t directory_length = strlen(directory);
    if(directory_length >= suffix_length &&
       strcmp(directory + directory_length - suffix_length, "/brightness") == 0) {
        snprintf(brightness_path, sizeof(brightness_path), "%s", directory);
        size_t length = directory_length - suffix_length;
        if(length >= sizeof(max_path)) return false;
        memcpy(max_path, directory, length);
        snprintf(max_path + length, sizeof(max_path) - length, "/max_brightness");
    }
    else {
        snprintf(brightness_path, sizeof(brightness_path), "%s/brightness", directory);
        snprintf(max_path, sizeof(max_path), "%s/max_brightness", directory);
    }

    FILE *max_file = fopen(max_path, "r");
    unsigned long maximum = 0;
    if(max_file) {
        (void)fscanf(max_file, "%lu", &maximum);
        fclose(max_file);
    }
    if(maximum == 0) {
        fprintf(stderr, "platform: cannot read backlight maximum %s\n", max_path);
        return false;
    }

    unsigned long value = (maximum * percent + 50U) / 100U;
    char text[32];
    int length = snprintf(text, sizeof(text), "%lu\n", value);
    int fd = open(brightness_path, O_WRONLY | O_CLOEXEC);
    if(fd < 0) {
        fprintf(stderr, "platform: cannot open backlight %s: %s\n",
                brightness_path, strerror(errno));
        return false;
    }
    bool success = write(fd, text, (size_t)length) == (ssize_t)length;
    if(!success) {
        fprintf(stderr, "platform: cannot set backlight %u%% at %s: %s\n",
                (unsigned)percent, brightness_path, strerror(errno));
    }
    close(fd);
    return success;
#endif
}

bool platform_initialize(void)
{
    /* POSIX TZ works on the device even without an installed timezone database. */
    if(setenv("TZ", "CST-8", 1) != 0) {
        fprintf(stderr, "platform: cannot set China timezone: %s\n", strerror(errno));
        return false;
    }
    tzset();
    fprintf(stdout, "platform: timezone=China Standard Time (UTC+08:00)\n");

#if defined(BINBOT_HOST_SIMULATOR)
    lv_display_t *display = lv_sdl_window_create(720, 720);
    if(!display) {
        fprintf(stderr, "platform: failed to create SDL simulator window\n");
        return false;
    }
    lv_display_set_default(display);
    lv_sdl_window_set_title(display, "BinBot UI Simulator");
    lv_sdl_window_set_resizeable(display, false);

    const char *zoom_text = getenv("BINBOT_SIMULATOR_ZOOM");
    if(zoom_text && zoom_text[0]) {
        char *end = NULL;
        float zoom = strtof(zoom_text, &end);
        if(end != zoom_text && *end == '\0' && zoom > 0.1f && zoom <= 4.0f) {
            lv_sdl_window_set_zoom(display, zoom);
        }
    }
    if(!lv_sdl_mouse_create()) fprintf(stderr, "platform: SDL mouse unavailable\n");
    if(!lv_sdl_keyboard_create()) fprintf(stderr, "platform: SDL keyboard unavailable\n");
    fprintf(stdout, "platform: SDL simulator=720x720\n");
    return true;
#else
    const char *fbdev = getenv("LVGL_FBDEV");
    if(!fbdev || !fbdev[0]) fbdev = "/dev/fb0";

    lv_display_t *display = lv_linux_fbdev_create();
    if(!display || lv_linux_fbdev_set_file(display, fbdev) != LV_RESULT_OK) {
        fprintf(stderr, "platform: failed to open framebuffer %s\n", fbdev);
        return false;
    }
    lv_display_set_default(display);

    const char *evdev = getenv("LVGL_EVDEV");
    if(evdev && evdev[0]) {
        if(!lv_evdev_create(LV_INDEV_TYPE_NONE, evdev)) {
            fprintf(stderr, "platform: failed to open input %s\n", evdev);
        }
    }
    else if(lv_evdev_discovery_start(NULL, NULL) != LV_RESULT_OK) {
        fprintf(stderr, "platform: evdev discovery unavailable\n");
    }

    fprintf(stdout, "platform: framebuffer=%s, resolution=%ldx%ld\n", fbdev,
            (long)lv_display_get_horizontal_resolution(display),
            (long)lv_display_get_vertical_resolution(display));
    return true;
#endif
}

void platform_shutdown(void)
{
#if defined(BINBOT_HOST_SIMULATOR)
    lv_sdl_quit();
#endif
}

uint32_t platform_monotonic_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint32_t)((uint64_t)now.tv_sec * 1000ULL + (uint64_t)now.tv_nsec / 1000000ULL);
}

bool platform_set_brightness(uint8_t percent)
{
    if(percent < 1U) percent = 1U;
    if(percent > 100U) percent = 100U;
    bool success = write_backlight_value(percent);
    reported_brightness = success ? percent : 0;
    return success;
}

uint8_t platform_reported_brightness(void) { return reported_brightness; }
