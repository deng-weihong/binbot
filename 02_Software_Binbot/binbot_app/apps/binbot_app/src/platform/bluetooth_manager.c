#include "binbot/bluetooth_manager.h"

#include <dbus/dbus.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct binbot_bluetooth_manager {
    pthread_mutex_t mutex;
    pthread_cond_t wake;
    pthread_t worker;
    bool stop;
    bool requested;
    binbot_bluetooth_action_t action;
    unsigned value;
    char device[BINBOT_BLUETOOTH_PATH_MAX];
    binbot_bluetooth_snapshot_t snapshot;
};

static pthread_once_t dbus_once = PTHREAD_ONCE_INIT;
static void initialize_dbus(void) { dbus_threads_init_default(); }

static bool property_value(DBusMessageIter properties, const char *name, DBusMessageIter *result)
{
    while(dbus_message_iter_get_arg_type(&properties) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry;
        const char *key = NULL;
        dbus_message_iter_recurse(&properties, &entry);
        dbus_message_iter_get_basic(&entry, &key);
        if(key && !strcmp(key, name) && dbus_message_iter_next(&entry) &&
           dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_VARIANT) {
            dbus_message_iter_recurse(&entry, result);
            return true;
        }
        dbus_message_iter_next(&properties);
    }
    return false;
}

static bool property(DBusMessageIter properties, const char *name, int type, void *result)
{
    DBusMessageIter value;
    if(!property_value(properties, name, &value) || dbus_message_iter_get_arg_type(&value) != type) return false;
    dbus_message_iter_get_basic(&value, result);
    return true;
}

static bool boolean_property(DBusMessageIter properties, const char *name)
{
    dbus_bool_t value = FALSE;
    property(properties, name, DBUS_TYPE_BOOLEAN, &value);
    return value;
}

static void string_property(DBusMessageIter properties, const char *name,
                             char *result, size_t capacity)
{
    const char *value = NULL;
    if(property(properties, name, DBUS_TYPE_STRING, &value)) snprintf(result, capacity, "%s", value);
}

typedef void (*interface_visitor_t)(const char *, const char *, DBusMessageIter,
                                    binbot_bluetooth_snapshot_t *);

static void visit_interfaces(DBusMessage *message, interface_visitor_t visitor,
                              binbot_bluetooth_snapshot_t *snapshot)
{
    DBusMessageIter root, objects;
    if(!dbus_message_iter_init(message, &root) ||
       dbus_message_iter_get_arg_type(&root) != DBUS_TYPE_ARRAY) return;
    dbus_message_iter_recurse(&root, &objects);
    while(dbus_message_iter_get_arg_type(&objects) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter object, interfaces;
        const char *path = NULL;
        dbus_message_iter_recurse(&objects, &object);
        dbus_message_iter_get_basic(&object, &path);
        dbus_message_iter_next(&object);
        dbus_message_iter_recurse(&object, &interfaces);
        while(dbus_message_iter_get_arg_type(&interfaces) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter interface, properties;
            const char *name = NULL;
            dbus_message_iter_recurse(&interfaces, &interface);
            dbus_message_iter_get_basic(&interface, &name);
            dbus_message_iter_next(&interface);
            dbus_message_iter_recurse(&interface, &properties);
            visitor(path, name, properties, snapshot);
            dbus_message_iter_next(&interfaces);
        }
        dbus_message_iter_next(&objects);
    }
}

static void read_device(const char *path, const char *interface, DBusMessageIter props,
                         binbot_bluetooth_snapshot_t *snapshot)
{
    if(!strcmp(interface, "org.bluez.Adapter1") && !snapshot->available) {
        snapshot->available = true;
        snapshot->powered = boolean_property(props, "Powered");
        snapshot->discoverable = boolean_property(props, "Discoverable");
        snprintf(snapshot->adapter, sizeof(snapshot->adapter), "%s", path);
        string_property(props, "Name", snapshot->name, sizeof(snapshot->name));
        string_property(props, "Alias", snapshot->name, sizeof(snapshot->name));
    }
    else if(!strcmp(interface, "org.bluez.Device1") && boolean_property(props, "Paired") &&
            snapshot->device_count < BINBOT_BLUETOOTH_MAX_DEVICES) {
        binbot_bluetooth_device_t *device = &snapshot->devices[snapshot->device_count++];
        snprintf(device->path, sizeof(device->path), "%s", path);
        string_property(props, "Address", device->name, sizeof(device->name));
        string_property(props, "Name", device->name, sizeof(device->name));
        string_property(props, "Alias", device->name, sizeof(device->name));
        char icon[64] = "";
        string_property(props, "Icon", icon, sizeof(icon));
        device->computer = !strcmp(icon, "computer");
        device->connected = boolean_property(props, "Connected");
    }
}

static void read_media(const char *path, const char *interface, DBusMessageIter props,
                        binbot_bluetooth_snapshot_t *snapshot)
{
    const binbot_bluetooth_device_t *current = NULL;
    for(size_t i = 0; i < snapshot->device_count; ++i) {
        const binbot_bluetooth_device_t *device = &snapshot->devices[i];
        if(device->connected) { current = device; break; }
    }
    if(!current) return;
    size_t length = strlen(current->path);
    if(strncmp(path, current->path, length) || path[length] != '/') return;
    if(!strcmp(interface, "org.bluez.MediaPlayer1")) {
        snprintf(snapshot->player, sizeof(snapshot->player), "%s", path);
        snapshot->playback_supported = true;
        char status[32] = "";
        string_property(props, "Status", status, sizeof(status));
        if(!strcmp(status, "playing")) snapshot->playing = true;
        DBusMessageIter track, metadata;
        if(property_value(props, "Track", &track) &&
           dbus_message_iter_get_arg_type(&track) == DBUS_TYPE_ARRAY &&
           dbus_message_iter_get_element_type(&track) == DBUS_TYPE_DICT_ENTRY) {
            dbus_message_iter_recurse(&track, &metadata);
            string_property(metadata, "Title", snapshot->title, sizeof(snapshot->title));
            string_property(metadata, "Artist", snapshot->artist, sizeof(snapshot->artist));
        }
    }
    else if(!strcmp(interface, "org.bluez.MediaTransport1")) {
        snprintf(snapshot->transport, sizeof(snapshot->transport), "%s", path);
        char state[32] = "";
        string_property(props, "State", state, sizeof(state));
        if(!strcmp(state, "active")) snapshot->playing = true;
        unsigned char codec;
        if(property(props, "Codec", DBUS_TYPE_BYTE, &codec))
            snprintf(snapshot->codec, sizeof(snapshot->codec), "%s",
                       codec == 0 ? "SBC" : codec == 2 ? "AAC" : "");
        dbus_uint16_t volume;
        if(property(props, "Volume", DBUS_TYPE_UINT16, &volume)) {
            snapshot->volume_supported = true;
            snapshot->volume = (volume * 100U + 63U) / 127U;
            if(snapshot->volume > 100) snapshot->volume = 100;
        }
    }
}

static int device_compare(const void *a, const void *b)
{
    const binbot_bluetooth_device_t *left = a, *right = b;
    if(left->connected != right->connected) return left->connected ? -1 : 1;
    return strcmp(left->path, right->path);
}

static DBusMessage *call(DBusConnection *connection, DBusMessage *message,
                          int timeout, char *error, size_t capacity)
{
    if(!message) return NULL;
    DBusError failure;
    dbus_error_init(&failure);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(connection, message, timeout, &failure);
    dbus_message_unref(message);
    if(!reply && error) {
        const char *text = "蓝牙操作失败，请重试";
        if(dbus_error_has_name(&failure, "org.bluez.Error.NotReady")) text = "请先打开蓝牙";
        else if(dbus_error_has_name(&failure, "org.bluez.Error.NotSupported")) text = "对方设备不支持此操作";
        else if(dbus_error_has_name(&failure, "org.bluez.Error.Failed") ||
                dbus_error_has_name(&failure, DBUS_ERROR_NO_REPLY)) text = "蓝牙连接失败，请确认对方设备可连接";
        snprintf(error, capacity, "%s", text);
    }
    dbus_error_free(&failure);
    return reply;
}

static bool refresh(DBusConnection *connection, binbot_bluetooth_snapshot_t *snapshot)
{
    DBusMessage *message = dbus_message_new_method_call("org.bluez", "/",
                            "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
    DBusMessage *reply = call(connection, message, 1500, NULL, 0);
    if(!reply) return false;
    if(!dbus_message_has_signature(reply, "a{oa{sa{sv}}}")) { dbus_message_unref(reply); return false; }
    visit_interfaces(reply, read_device, snapshot);
    qsort(snapshot->devices, snapshot->device_count, sizeof(snapshot->devices[0]), device_compare);
    visit_interfaces(reply, read_media, snapshot);
    dbus_message_unref(reply);
    return true;
}

static bool set_property(DBusConnection *connection, const char *path, const char *interface,
                          const char *name, int type, const void *value,
                          char *error, size_t capacity)
{
    DBusMessage *message = dbus_message_new_method_call("org.bluez", path,
                            "org.freedesktop.DBus.Properties", "Set");
    if(!message) return false;
    DBusMessageIter args, variant;
    char signature[] = {(char)type, 0};
    dbus_message_iter_init_append(message, &args);
    dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &interface);
    dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &name);
    dbus_message_iter_open_container(&args, DBUS_TYPE_VARIANT, signature, &variant);
    dbus_message_iter_append_basic(&variant, type, value);
    dbus_message_iter_close_container(&args, &variant);
    DBusMessage *reply = call(connection, message, 1500, error, capacity);
    if(!reply) return false;
    dbus_message_unref(reply);
    return true;
}

static void execute(DBusConnection *connection, const binbot_bluetooth_snapshot_t *snapshot,
                      binbot_bluetooth_action_t action, const char *device, unsigned value,
                      char *error, size_t capacity)
{
    if(action == BINBOT_BLUETOOTH_POWER || action == BINBOT_BLUETOOTH_DISCOVERABLE) {
        dbus_bool_t enabled = value != 0;
        if(action == BINBOT_BLUETOOTH_DISCOVERABLE && enabled &&
           !set_property(connection, snapshot->adapter, "org.bluez.Adapter1", "Pairable",
                           DBUS_TYPE_BOOLEAN, &enabled, error, capacity)) return;
        set_property(connection, snapshot->adapter, "org.bluez.Adapter1",
                       action == BINBOT_BLUETOOTH_POWER ? "Powered" : "Discoverable",
                       DBUS_TYPE_BOOLEAN, &enabled, error, capacity);
        return;
    }
    if(action == BINBOT_BLUETOOTH_VOLUME) {
        dbus_uint16_t volume = (value * 127U + 50U) / 100U;
        set_property(connection, snapshot->transport, "org.bluez.MediaTransport1", "Volume",
                       DBUS_TYPE_UINT16, &volume, error, capacity);
        return;
    }
    const char *path = device, *interface = "org.bluez.Device1", *method = "Connect";
    if(action == BINBOT_BLUETOOTH_DISCONNECT) method = "Disconnect";
    else if(action == BINBOT_BLUETOOTH_REMOVE) {
        path = snapshot->adapter; interface = "org.bluez.Adapter1"; method = "RemoveDevice";
    }
    else if(action >= BINBOT_BLUETOOTH_PLAY && action <= BINBOT_BLUETOOTH_NEXT) {
        static const char *methods[] = {"Play", "Pause", "Previous", "Next"};
        path = snapshot->player; interface = "org.bluez.MediaPlayer1";
        method = methods[action - BINBOT_BLUETOOTH_PLAY];
    }
    DBusMessage *message = dbus_message_new_method_call("org.bluez", path, interface, method);
    if(message && action == BINBOT_BLUETOOTH_REMOVE)
        dbus_message_append_args(message, DBUS_TYPE_OBJECT_PATH, &device, DBUS_TYPE_INVALID);
    DBusMessage *reply = call(connection, message, action == BINBOT_BLUETOOTH_CONNECT ? 10000 : 2000,
                               error, capacity);
    if(reply) dbus_message_unref(reply);
}

/* Only this worker touches D-Bus. LVGL reads a bounded snapshot and queues
 * actions, so slow connections never hold up touch or page transitions. */
static void *worker(void *data)
{
    binbot_bluetooth_manager_t *manager = data;
    DBusConnection *connection = NULL;
    for(;;) {
        pthread_mutex_lock(&manager->mutex);
        if(manager->stop) { pthread_mutex_unlock(&manager->mutex); break; }
        bool requested = manager->requested;
        binbot_bluetooth_action_t action = manager->action;
        unsigned value = manager->value;
        char device[BINBOT_BLUETOOTH_PATH_MAX];
        snprintf(device, sizeof(device), "%s", manager->device);
        binbot_bluetooth_snapshot_t previous = manager->snapshot;
        manager->requested = false;
        pthread_mutex_unlock(&manager->mutex);

        if(connection && !dbus_connection_get_is_connected(connection)) {
            dbus_connection_close(connection); dbus_connection_unref(connection); connection = NULL;
        }
        if(!connection) {
            DBusError error; dbus_error_init(&error);
            connection = dbus_bus_get_private(DBUS_BUS_SYSTEM, &error);
            dbus_error_free(&error);
            if(connection) dbus_connection_set_exit_on_disconnect(connection, FALSE);
        }
        binbot_bluetooth_snapshot_t next = {0};
        snprintf(next.error, sizeof(next.error), "%s", previous.error);
        if(connection) {
            if(requested) execute(connection, &previous, action, device, value, next.error, sizeof(next.error));
            if(!refresh(connection, &next)) {
                memset(&next, 0, sizeof(next));
                snprintf(next.error, sizeof(next.error), "蓝牙服务不可用");
            }
        }
        else snprintf(next.error, sizeof(next.error), "蓝牙服务不可用");
        next.ready = true;
        pthread_mutex_lock(&manager->mutex);
        /* A request may arrive during a periodic refresh. Preserve its busy
         * state until the next worker iteration executes that request. */
        if(manager->requested) {
            next.busy = true;
            next.error[0] = 0;
            if(manager->action == BINBOT_BLUETOOTH_CONNECT)
                snprintf(next.pending_device, sizeof(next.pending_device), "%s", manager->device);
        }
        manager->snapshot = next;
        if(!manager->stop && !manager->requested) {
            struct timespec deadline;
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_sec += 2;
            pthread_cond_timedwait(&manager->wake, &manager->mutex, &deadline);
        }
        bool stop = manager->stop;
        pthread_mutex_unlock(&manager->mutex);
        if(stop) break;
    }
    if(connection) { dbus_connection_close(connection); dbus_connection_unref(connection); }
    return NULL;
}

binbot_bluetooth_manager_t *binbot_bluetooth_manager_create(void)
{
    pthread_once(&dbus_once, initialize_dbus);
    binbot_bluetooth_manager_t *manager = calloc(1, sizeof(*manager));
    if(!manager) return NULL;
    pthread_mutex_init(&manager->mutex, NULL);
    pthread_cond_init(&manager->wake, NULL);
    if(pthread_create(&manager->worker, NULL, worker, manager)) {
        pthread_cond_destroy(&manager->wake); pthread_mutex_destroy(&manager->mutex); free(manager); return NULL;
    }
    return manager;
}

void binbot_bluetooth_manager_destroy(binbot_bluetooth_manager_t *manager)
{
    if(!manager) return;
    pthread_mutex_lock(&manager->mutex); manager->stop = true;
    pthread_cond_signal(&manager->wake); pthread_mutex_unlock(&manager->mutex);
    pthread_join(manager->worker, NULL);
    pthread_cond_destroy(&manager->wake); pthread_mutex_destroy(&manager->mutex); free(manager);
}

bool binbot_bluetooth_manager_snapshot(binbot_bluetooth_manager_t *manager,
                                       binbot_bluetooth_snapshot_t *snapshot)
{
    if(!manager || !snapshot) return false;
    pthread_mutex_lock(&manager->mutex); *snapshot = manager->snapshot; pthread_mutex_unlock(&manager->mutex);
    return true;
}

bool binbot_bluetooth_manager_request(binbot_bluetooth_manager_t *manager,
                                      binbot_bluetooth_action_t action,
                                      const char *device, unsigned value)
{
    if(!manager || action < BINBOT_BLUETOOTH_POWER || action > BINBOT_BLUETOOTH_VOLUME || value > 100) return false;
    pthread_mutex_lock(&manager->mutex);
    const binbot_bluetooth_snapshot_t *snapshot = &manager->snapshot;
    bool valid = snapshot->available && !snapshot->busy;
    if(action != BINBOT_BLUETOOTH_POWER) valid = valid && snapshot->powered;
    if(action >= BINBOT_BLUETOOTH_CONNECT && action <= BINBOT_BLUETOOTH_REMOVE) {
        bool found = false;
        for(size_t i = 0; device && i < snapshot->device_count; ++i)
            if(!strcmp(snapshot->devices[i].path, device)) found = true;
        valid = valid && found;
    }
    if(action >= BINBOT_BLUETOOTH_PLAY && action <= BINBOT_BLUETOOTH_NEXT)
        valid = valid && snapshot->playback_supported;
    if(action == BINBOT_BLUETOOTH_VOLUME) valid = valid && snapshot->volume_supported;
    if(valid) {
        manager->requested = true; manager->action = action; manager->value = value;
        snprintf(manager->device, sizeof(manager->device), "%s", device ? device : "");
        manager->snapshot.busy = true;
        manager->snapshot.error[0] = 0;
        if(action == BINBOT_BLUETOOTH_CONNECT)
            snprintf(manager->snapshot.pending_device, sizeof(manager->snapshot.pending_device), "%s", device);
        pthread_cond_signal(&manager->wake);
    }
    pthread_mutex_unlock(&manager->mutex);
    return valid;
}
