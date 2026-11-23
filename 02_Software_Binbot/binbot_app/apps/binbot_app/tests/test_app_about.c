#define _DEFAULT_SOURCE
#include "ui_internal.h"
#include "binbot/build_info.h"
#include "draw/snapshot/lv_snapshot.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static lv_obj_t *find_label(lv_obj_t *root, const char *text)
{
    if(lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), text)) return root;
    for(uint32_t i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = find_label(lv_obj_get_child(root, i), text);
        if(found) return found;
    }
    return NULL;
}

static void settle(void)
{
    lv_tick_inc(300);
    lv_timer_handler();
    lv_obj_update_layout(lv_screen_active());
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    (void)area; (void)pixels;
    lv_display_flush_ready(display);
}

static void put32(uint8_t *data, uint32_t value)
{
    for(unsigned i = 0; i < 4; ++i) data[i] = (uint8_t)(value >> (i * 8));
}

static binbot_slave_ipc_packet_t reply(app_firmware_t *firmware, uint32_t version)
{
    binbot_slave_ipc_packet_t packet;
    binbot_slave_ipc_packet_init(&packet, BINBOT_SLAVE_IPC_MSG_RESPONSE,
                                 BINBOT_SLAVE_CMD_OTA_INFO, firmware->request_id);
    packet.payload_length = 56;
    packet.payload[0] = 1;
    packet.payload[2] = 1;
    put32(packet.payload + 4, 12);
    put32(packet.payload + 8, 0x35000);
    put32(packet.payload + 12, version);
    return packet;
}

static void drain_request(int fd, app_firmware_t *firmware)
{
    binbot_slave_ipc_packet_t packet;
    ssize_t count = recv(fd, &packet, sizeof(packet), MSG_DONTWAIT);
    assert(count > 0 && binbot_slave_ipc_packet_is_valid(&packet, (size_t)count));
    assert(packet.type == BINBOT_SLAVE_IPC_MSG_REQUEST && packet.command == BINBOT_SLAVE_CMD_OTA_INFO);
    assert(packet.request_id == firmware->request_id && packet.payload_length == 0);
}

static void snapshot(ui_app_t *app)
{
    const char *path = getenv("BINBOT_ABOUT_SNAPSHOT");
    if(!path) return;
    lv_draw_buf_t *frame = lv_snapshot_take(app->root, LV_COLOR_FORMAT_RGB888);
    assert(frame && frame->header.w == 720 && frame->header.h == 720);
    FILE *file = fopen(path, "wb"); assert(file);
    fprintf(file, "P6\n720 720\n255\n");
    for(unsigned y = 0; y < 720; ++y) {
        const uint8_t *row = frame->data + y * frame->header.stride;
        for(unsigned x = 0; x < 720; ++x) {
            uint8_t pixel[] = {row[x * 3 + 2], row[x * 3 + 1], row[x * 3]};
            assert(fwrite(pixel, 1, 3, file) == 3);
        }
    }
    assert(!fclose(file));
    lv_draw_buf_destroy(frame);
}

int main(void)
{
    char directory[] = "/tmp/binbot-about-ui-XXXXXX"; assert(mkdtemp(directory));
    setenv("BINBOT_MODULE_IMAGE_ROOT", directory, 1);
    setenv("BINBOT_DEVICE_ID", "binbot-001", 1);
    setenv("BINBOT_INITIAL_ROUTE", "settings", 1);
    lv_init();
    lv_display_t *display = lv_display_create(720, 720); assert(display);
    static uint8_t draw_buffer[720 * 64 * 4];
    lv_display_set_buffers(display, draw_buffer, NULL, sizeof(draw_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    app_state_t *state = app_state_create(); assert(state);
    ui_app_t *app = ui_app_create(state); assert(app);
    assert(find_label(app->current.root, "v" BINBOT_APP_VERSION));
    assert(!find_label(app->current.root, "v2.3.1"));
    lv_obj_t *label = find_label(app->current.root, "关于本机"); assert(label);
    lv_obj_send_event(lv_obj_get_parent(label), LV_EVENT_CLICKED, NULL);
    settle();
    assert(app->route == UI_ROUTE_ABOUT && find_label(app->current.root, "读取中…"));
    assert(find_label(app->current.root, "binbot-001"));

    int sockets[2]; assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets));
    binbot_slave_client_t client;
    binbot_slave_client_init(&client, BINBOT_SLAVE_IPC_ROLE_APP, NULL);
    client.fd = sockets[0];
    app_firmware_t *firmware = ui_app_firmware(app);
    firmware->client = &client;
    app_firmware_tick(firmware, 1000);
    drain_request(sockets[1], firmware);
    binbot_slave_ipc_packet_t packet = reply(firmware, 3);
    packet.request_id++;
    assert(!app_firmware_handle_packet(firmware, &packet));
    packet.request_id--;
    assert(app_firmware_handle_packet(firmware, &packet));
    settle();
    assert(firmware->healthy && firmware->version == 3 && firmware->boot_id == 12);
    assert(find_label(app->current.root, "v3 · 已确认"));
    snapshot(app);

    label = find_label(app->current.root, "刷新版本"); assert(label);
    lv_obj_send_event(lv_obj_get_parent(label), LV_EVENT_CLICKED, NULL);
    app_firmware_tick(firmware, 2000);
    drain_request(sockets[1], firmware);
    packet = reply(firmware, 4);
    packet.error_code = BINBOT_SLAVE_ERR_BUSY;
    assert(app_firmware_handle_packet(firmware, &packet));
    settle();
    assert(find_label(app->current.root, "升级中，请稍后"));
    app_firmware_tick(firmware, 2300);
    drain_request(sockets[1], firmware);
    packet = reply(firmware, 4);
    packet.payload[1] = 1;
    assert(app_firmware_handle_packet(firmware, &packet));
    settle();
    assert(firmware->slot == 1 && find_label(app->current.root, "v4 · 已确认"));

    app_firmware_refresh(firmware);
    app_firmware_tick(firmware, 3000);
    drain_request(sockets[1], firmware);
    packet = reply(firmware, 5);
    packet.payload_length = 55;
    assert(app_firmware_handle_packet(firmware, &packet));
    settle();
    assert(find_label(app->current.root, "版本读取不可用"));
    app_firmware_refresh(firmware);
    app_firmware_tick(firmware, 4000);
    drain_request(sockets[1], firmware);
    packet = reply(firmware, 5);
    app_firmware_tick(firmware, 9000);
    assert(!app_firmware_handle_packet(firmware, &packet));
    settle();
    assert(find_label(app->current.root, "未连接"));
    assert(!find_label(app->current.root, "v4 · 已确认"));

    lv_obj_send_event(lv_obj_get_child(app->current.root, 1), LV_EVENT_CLICKED, NULL);
    settle();
    assert(app->route == UI_ROUTE_SETTINGS);
    binbot_slave_client_close(&client);
    close(sockets[1]);
    ui_app_destroy(app); app_state_destroy(state); lv_deinit();
    assert(!rmdir(directory));
    puts("About navigation, live firmware, busy retry, malformed reply and timeout passed");
    return 0;
}
