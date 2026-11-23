#ifndef BINBOT_PLATFORM_H
#define BINBOT_PLATFORM_H

#include <stdbool.h>
#include <stdint.h>

bool platform_initialize(void);
void platform_shutdown(void);
uint32_t platform_monotonic_ms(void);
bool platform_set_brightness(uint8_t percent);
/* Last successful backlight write; zero means its result is unknown. */
uint8_t platform_reported_brightness(void);

#endif
