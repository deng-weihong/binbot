#ifndef BINBOT_APP_WEATHER_H
#define BINBOT_APP_WEATHER_H

typedef struct { const char *name, *icon; } app_weather_info_t;
/* Shared protocol mapping for the home page and all display snapshots. */
const app_weather_info_t *app_weather_info(unsigned condition);
#endif
