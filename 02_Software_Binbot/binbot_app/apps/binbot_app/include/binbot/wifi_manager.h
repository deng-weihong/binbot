#ifndef BINBOT_WIFI_MANAGER_H
#define BINBOT_WIFI_MANAGER_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    BINBOT_WIFI_MAX_NETWORKS = 24,
    BINBOT_WIFI_SSID_MAX = 96,
    BINBOT_WIFI_ERROR_MAX = 128,
};

typedef struct {
    char ssid[BINBOT_WIFI_SSID_MAX];
    int signal_dbm;
    bool secured;
    bool connected;
} binbot_wifi_network_t;

typedef enum {
    BINBOT_WIFI_IDLE,
    BINBOT_WIFI_SCANNING,
    BINBOT_WIFI_READY,
    BINBOT_WIFI_CONNECTING,
    BINBOT_WIFI_CONNECTED,
    BINBOT_WIFI_ERROR,
} binbot_wifi_state_t;

typedef struct binbot_wifi_manager binbot_wifi_manager_t;

binbot_wifi_manager_t *binbot_wifi_manager_create(void);
void binbot_wifi_manager_destroy(binbot_wifi_manager_t *manager);
bool binbot_wifi_manager_scan(binbot_wifi_manager_t *manager);
bool binbot_wifi_manager_connect(binbot_wifi_manager_t *manager,
                                  const char *ssid, const char *password);
bool binbot_wifi_manager_snapshot(const binbot_wifi_manager_t *manager,
                                  binbot_wifi_network_t *networks,
                                  size_t network_capacity,
                                  size_t *network_count,
                                  binbot_wifi_state_t *state,
                                  char *connected_ssid,
                                  size_t connected_ssid_size,
                                  char *error,
                                  size_t error_size);

#ifdef __cplusplus
}
#endif

#endif
