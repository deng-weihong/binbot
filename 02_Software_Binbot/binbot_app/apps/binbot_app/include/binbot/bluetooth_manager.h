#ifndef BINBOT_BLUETOOTH_MANAGER_H
#define BINBOT_BLUETOOTH_MANAGER_H

#include <stdbool.h>
#include <stddef.h>

enum { BINBOT_BLUETOOTH_MAX_DEVICES = 24, BINBOT_BLUETOOTH_PATH_MAX = 256 };

typedef struct {
    char path[BINBOT_BLUETOOTH_PATH_MAX];
    char name[128];
    bool computer;
    bool connected;
} binbot_bluetooth_device_t;

typedef struct {
    bool ready;
    bool available;
    bool powered;
    bool discoverable;
    bool busy;
    bool playing;
    bool playback_supported;
    bool volume_supported;
    unsigned volume;
    char name[128];
    char adapter[BINBOT_BLUETOOTH_PATH_MAX];
    char player[BINBOT_BLUETOOTH_PATH_MAX];
    char transport[BINBOT_BLUETOOTH_PATH_MAX];
    char pending_device[BINBOT_BLUETOOTH_PATH_MAX];
    char codec[24];
    char title[256];
    char artist[128];
    char error[128];
    size_t device_count;
    binbot_bluetooth_device_t devices[BINBOT_BLUETOOTH_MAX_DEVICES];
} binbot_bluetooth_snapshot_t;

typedef enum {
    BINBOT_BLUETOOTH_POWER,
    BINBOT_BLUETOOTH_DISCOVERABLE,
    BINBOT_BLUETOOTH_CONNECT,
    BINBOT_BLUETOOTH_DISCONNECT,
    BINBOT_BLUETOOTH_REMOVE,
    BINBOT_BLUETOOTH_PLAY,
    BINBOT_BLUETOOTH_PAUSE,
    BINBOT_BLUETOOTH_PREVIOUS,
    BINBOT_BLUETOOTH_NEXT,
    BINBOT_BLUETOOTH_VOLUME,
} binbot_bluetooth_action_t;

typedef struct binbot_bluetooth_manager binbot_bluetooth_manager_t;

binbot_bluetooth_manager_t *binbot_bluetooth_manager_create(void);
void binbot_bluetooth_manager_destroy(binbot_bluetooth_manager_t *manager);
bool binbot_bluetooth_manager_snapshot(binbot_bluetooth_manager_t *manager,
                                       binbot_bluetooth_snapshot_t *snapshot);
bool binbot_bluetooth_manager_request(binbot_bluetooth_manager_t *manager,
                                      binbot_bluetooth_action_t action,
                                      const char *device, unsigned value);

#endif
