#include "../ui_internal.h"

#include "binbot/wifi_manager.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

enum {
    WIFI_CONTENT_WIDTH = UI_WIDTH - 44,
    WIFI_ROW_HEIGHT = 68,
    WIFI_ROW_GAP = 11,
    WIFI_KEYBOARD_Y = 396,
    WIFI_KEYBOARD_HEIGHT = UI_HEIGHT - WIFI_KEYBOARD_Y,
};

typedef struct wifi_context wifi_context_t;

typedef struct {
    wifi_context_t *context;
    size_t index;
} wifi_row_binding_t;

struct wifi_context {
    ui_app_t *app;
    binbot_wifi_manager_t *manager;
    lv_timer_t *timer;
    lv_obj_t *root;
    lv_obj_t *status_value;
    lv_obj_t *status_hint;
    lv_obj_t *status_detail;
    lv_obj_t *status_signal;
    lv_obj_t *refresh;
    lv_obj_t *refresh_label;
    lv_obj_t *list;
    lv_obj_t *empty;
    lv_obj_t *overlay;
    lv_obj_t *backdrop;
    lv_obj_t *password_panel;
    lv_obj_t *password_textarea;
    lv_obj_t *keyboard;
    lv_obj_t *password_ssid;
    lv_obj_t *connect_button;
    binbot_wifi_network_t networks[BINBOT_WIFI_MAX_NETWORKS];
    size_t network_count;
    binbot_wifi_state_t state;
    char connected_ssid[BINBOT_WIFI_SSID_MAX];
    char error[BINBOT_WIFI_ERROR_MAX];
    binbot_wifi_network_t rendered_networks[BINBOT_WIFI_MAX_NETWORKS];
    size_t rendered_count;
    wifi_row_binding_t bindings[BINBOT_WIFI_MAX_NETWORKS];
    lv_obj_t *rows[BINBOT_WIFI_MAX_NETWORKS];
    char selected_ssid[BINBOT_WIFI_SSID_MAX];
    char joining_ssid[BINBOT_WIFI_SSID_MAX];
};

static void wifi_row_clicked(lv_event_t *event);

static int wifi_signal_level(int dbm)
{
    if(dbm >= -60) return 3;
    if(dbm >= -75) return 2;
    return 1;
}

static lv_obj_t *wifi_bars(lv_obj_t *parent, int32_t x, int32_t y, int level, bool white)
{
    lv_obj_t *bars = ui_box(parent, x, y, 18, 18, 0, 0);
    lv_obj_set_style_bg_opa(bars, LV_OPA_TRANSP, 0);
    for(int i = 0; i < 3; ++i) {
        lv_obj_t *bar = ui_box(bars, i * 7, 12 - i * 6, 4, 6 + i * 6,
                               i < level ? (white ? 0xffffff : UI_REF_INK) : 0xd6d6d6, 2);
        if(white && i >= level) lv_obj_set_style_bg_opa(bar, 77, 0);
    }
    return bars;
}

static void wifi_set_status(wifi_context_t *context)
{
    if(!context) return;
    bool joining = context->state == BINBOT_WIFI_CONNECTING;
    int signal_level = 3;
    for(size_t i = 0; i < context->network_count; ++i)
        if(context->networks[i].connected) signal_level = wifi_signal_level(context->networks[i].signal_dbm);
    for(int i = 0; i < 3; ++i)
        lv_obj_set_style_bg_opa(lv_obj_get_child(context->status_signal, i),
                                i < signal_level ? LV_OPA_COVER : 77, 0);
    lv_label_set_text(context->status_value, joining ? context->joining_ssid :
                      context->connected_ssid[0] ? context->connected_ssid : "未连接");
    lv_label_set_text(context->status_hint, joining ? "正在连接…" : "当前网络");
    lv_label_set_text(context->status_detail, joining ? "连接中…" :
                      context->connected_ssid[0] ? "已连接" : "");
    if(context->connected_ssid[0] && !joining) lv_obj_remove_flag(context->status_signal, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(context->status_signal, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(context->refresh_label, context->state == BINBOT_WIFI_SCANNING ? "扫描中…" : "刷新");
    lv_obj_set_width(context->refresh, context->state == BINBOT_WIFI_SCANNING ? 88 : 58);
    lv_obj_set_x(context->refresh, context->state == BINBOT_WIFI_SCANNING ? 603 : 633);
    if(context->empty) {
        const char *empty_text = context->state == BINBOT_WIFI_ERROR
                                     ? context->error
                                     : context->state == BINBOT_WIFI_READY
                                         ? "附近没有发现 WiFi"
                                         : context->state == BINBOT_WIFI_SCANNING ? "正在搜索 WiFi…" : "没有其他网络";
        lv_label_set_text(context->empty, empty_text);
    }
    if(context->state == BINBOT_WIFI_SCANNING || context->state == BINBOT_WIFI_CONNECTING) {
        lv_obj_add_state(context->refresh, LV_STATE_DISABLED);
    }
    else {
        lv_obj_remove_state(context->refresh, LV_STATE_DISABLED);
    }
}

static bool wifi_networks_equal(const wifi_context_t *context,
                                const binbot_wifi_network_t *networks, size_t count)
{
    if(!context || count != context->rendered_count) return false;
    return count == 0 || memcmp(context->rendered_networks, networks,
                                count * sizeof(*networks)) == 0;
}

static void wifi_render_list(wifi_context_t *context)
{
    if(!context || !context->list) return;
    for(size_t i = 0; i < context->rendered_count; ++i) {
        if(context->rows[i] && lv_obj_is_valid(context->rows[i])) {
            lv_obj_delete(context->rows[i]);
        }
        context->rows[i] = NULL;
    }
    context->rendered_count = context->network_count;
    if(context->rendered_count > BINBOT_WIFI_MAX_NETWORKS) {
        context->rendered_count = BINBOT_WIFI_MAX_NETWORKS;
    }
    if(context->rendered_count > 0) {
        memcpy(context->rendered_networks, context->networks,
               context->rendered_count * sizeof(context->networks[0]));
    }
    size_t visible = 0;
    for(size_t i = 0; i < context->rendered_count; ++i)
        if(!context->rendered_networks[i].connected) ++visible;
    if(visible == 0) {
        lv_obj_remove_flag(context->empty, LV_OBJ_FLAG_HIDDEN);
    }
    else {
        lv_obj_add_flag(context->empty, LV_OBJ_FLAG_HIDDEN);
    }

    size_t row_index = 0;
    for(size_t i = 0; i < context->rendered_count; ++i) {
        const binbot_wifi_network_t *network = &context->rendered_networks[i];
        if(network->connected) continue;
        int32_t y = (int32_t)row_index++ * (WIFI_ROW_HEIGHT + WIFI_ROW_GAP);
        context->bindings[i] = (wifi_row_binding_t){.context = context, .index = i};
        lv_obj_t *row = ui_ref_button(context->list, 0, y, WIFI_CONTENT_WIDTH,
                                       WIFI_ROW_HEIGHT, 23, "todo_row");
        lv_obj_add_event_cb(row, wifi_row_clicked, LV_EVENT_CLICKED, &context->bindings[i]);
        context->rows[i] = row;

        wifi_bars(row, 18, 25, wifi_signal_level(network->signal_dbm), false);
        lv_obj_t *ssid = ui_ref_label(row, network->ssid, 18, UI_FONT_MEDIUM, UI_REF_INK, 50, 24);
        lv_obj_set_width(ssid, 492);
        lv_label_set_long_mode(ssid, LV_LABEL_LONG_DOT);

        if(network->secured) {
            lv_obj_t *lock = ui_ref_icon(row, "lock", 20, UI_REF_FAINT);
            lv_obj_set_pos(lock, 566, 24);
        }
        lv_obj_t *connect = ui_box(row, 600, 16, 58, 36, 0xe7e7e7, 18);
        ui_ref_surface(connect, "wifi_connect");
        ui_centered_label(connect, "连接", ui_ref_font(15, UI_FONT_MEDIUM), UI_REF_INK);
    }
}

static void wifi_update_from_manager(wifi_context_t *context)
{
    if(!context || !context->manager) return;
    binbot_wifi_network_t networks[BINBOT_WIFI_MAX_NETWORKS];
    size_t count = 0;
    binbot_wifi_state_t state = BINBOT_WIFI_IDLE;
    char connected[BINBOT_WIFI_SSID_MAX];
    char error[BINBOT_WIFI_ERROR_MAX];
    if(!binbot_wifi_manager_snapshot(context->manager, networks, BINBOT_WIFI_MAX_NETWORKS,
                                     &count, &state, connected, sizeof(connected),
                                     error, sizeof(error))) return;
    bool changed = state != context->state ||
                   strcmp(connected, context->connected_ssid) != 0 ||
                   strcmp(error, context->error) != 0 ||
                   !wifi_networks_equal(context, networks, count);
    memcpy(context->networks, networks, count * sizeof(networks[0]));
    context->network_count = count;
    context->state = state;
    snprintf(context->connected_ssid, sizeof(context->connected_ssid), "%s", connected);
    snprintf(context->error, sizeof(context->error), "%s", error);
    if(!changed) return;
    if(state == BINBOT_WIFI_ERROR && error[0]) ui_app_show_toast(context->app, error);
    wifi_set_status(context);
    if(!wifi_networks_equal(context, networks, count)) wifi_render_list(context);
}

static void wifi_timer_cb(lv_timer_t *timer)
{
    wifi_context_t *context = timer ? lv_timer_get_user_data(timer) : NULL;
    wifi_update_from_manager(context);
}

static void wifi_scan_clicked(lv_event_t *event)
{
    wifi_context_t *context = lv_event_get_user_data(event);
    if(!context || !context->manager) return;
    if(!binbot_wifi_manager_scan(context->manager)) {
        ui_app_show_toast(context->app, "WiFi 正在处理中");
    }
}

static void wifi_hide_password(wifi_context_t *context)
{
    if(!context || !context->overlay) return;
    lv_keyboard_set_textarea(context->keyboard, NULL);
    lv_obj_add_flag(context->overlay, LV_OBJ_FLAG_HIDDEN);
    if(context->backdrop) lv_obj_delete(context->backdrop);
    context->backdrop = NULL;
    context->selected_ssid[0] = '\0';
}

static void wifi_connect(wifi_context_t *context, const char *ssid, const char *password)
{
    if(!context || !context->manager || !ssid || !ssid[0]) return;
    if(!binbot_wifi_manager_connect(context->manager, ssid, password ? password : "")) {
        ui_app_show_toast(context->app, "WiFi 正在处理中");
        return;
    }
    snprintf(context->joining_ssid, sizeof(context->joining_ssid), "%s", ssid);
    wifi_hide_password(context);
    context->state = BINBOT_WIFI_CONNECTING;
    wifi_set_status(context);
}

static void wifi_password_action(wifi_context_t *context)
{
    if(!context || !context->password_textarea || !context->selected_ssid[0]) return;
    const char *password = lv_textarea_get_text(context->password_textarea);
    if(strlen(password) < 8) return;
    wifi_connect(context, context->selected_ssid, password);
}

static void wifi_password_event(lv_event_t *event)
{
    wifi_context_t *context = lv_event_get_user_data(event);
    lv_event_code_t code = lv_event_get_code(event);
    if(!context) return;
    if(code == LV_EVENT_CLICKED) wifi_password_action(context);
}

static void wifi_cancel_event(lv_event_t *event)
{
    wifi_hide_password(lv_event_get_user_data(event));
}

static void wifi_overlay_event(lv_event_t *event)
{
    wifi_context_t *context = lv_event_get_user_data(event);
    if(context && lv_event_get_target(event) == context->overlay &&
       lv_event_get_code(event) == LV_EVENT_CLICKED) wifi_hide_password(context);
}

static void wifi_row_clicked(lv_event_t *event)
{
    wifi_row_binding_t *binding = lv_event_get_user_data(event);
    if(!binding || !binding->context || binding->index >= binding->context->rendered_count) return;
    wifi_context_t *context = binding->context;
    const binbot_wifi_network_t *network = &context->rendered_networks[binding->index];
    if(network->connected) {
        ui_app_show_toast(context->app, "已经连接到此网络");
        return;
    }
    snprintf(context->selected_ssid, sizeof(context->selected_ssid), "%s", network->ssid);
    if(!network->secured) {
        wifi_connect(context, network->ssid, "");
        return;
    }
    lv_label_set_text(context->password_ssid, network->ssid);
    lv_textarea_set_text(context->password_textarea, "");
    lv_keyboard_set_textarea(context->keyboard, context->password_textarea);
    context->backdrop = ui_ref_backdrop(context->overlay, context->root, 0xefefef, 153, 12);
    lv_obj_remove_flag(context->overlay, LV_OBJ_FLAG_HIDDEN);
}

static void wifi_password_changed(lv_event_t *event)
{
    wifi_context_t *context = lv_event_get_user_data(event);
    if(strlen(lv_textarea_get_text(context->password_textarea)) >= 8)
        lv_obj_remove_state(context->connect_button, LV_STATE_DISABLED);
    else lv_obj_add_state(context->connect_button, LV_STATE_DISABLED);
}

static void wifi_keyboard_event(lv_event_t *event)
{
    wifi_context_t *context = lv_event_get_user_data(event);
    if(lv_event_get_code(event) == LV_EVENT_CANCEL) wifi_hide_password(context);
    else if(lv_event_get_code(event) == LV_EVENT_READY) wifi_password_action(context);
}

static void wifi_power_clicked(lv_event_t *event)
{
    wifi_context_t *context = lv_event_get_user_data(event);
    ui_app_show_toast(context->app, "Wi-Fi 开关尚未接入设备服务");
}

static void wifi_destroy(void *user_data)
{
    wifi_context_t *context = user_data;
    if(!context) return;
    if(context->timer) lv_timer_delete(context->timer);
    if(context->manager) binbot_wifi_manager_destroy(context->manager);
    free(context);
}

ui_screen_t ui_wifi_screen_create(ui_app_t *app)
{
    wifi_context_t *context = calloc(1, sizeof(*context));
    if(!context) return (ui_screen_t){0};
    context->app = app;
    context->manager = binbot_wifi_manager_create();
    if(!context->manager) {
        free(context);
        return (ui_screen_t){0};
    }

    lv_obj_t *root = ui_ref_shell_latest(app, "无线网络", "WI-FI");
    context->root = root;
    lv_obj_t *power = ui_button(root, 615, 30, 76, 43, UI_REF_CLAY, 22);
    ui_box(power, 37, 5, 32, 32, 0xffffff, 16);
    lv_obj_add_event_cb(power, wifi_power_clicked, LV_EVENT_CLICKED, context);

    lv_obj_t *status = ui_box(root, 22, 96, 676, 101, 0x333333, 27);
    ui_ref_surface(status, "wifi_status");
    lv_obj_t *wifi_icon = ui_box(status, 22, 25, 50, 50, UI_REF_CLAY, 25);
    lv_obj_t *wifi_mark = ui_ref_icon(wifi_icon, "wifi", 26, 0xffffff);
    lv_obj_center(wifi_mark);
    context->status_hint = ui_ref_label(status, "当前网络", 15, UI_FONT_REGULAR, 0xaaaaaa, 86, 24);
    context->status_value = ui_ref_label(status, "未连接", 22, UI_FONT_SEMIBOLD, 0xffffff, 86, 48);
    lv_obj_set_width(context->status_value, 490);
    lv_label_set_long_mode(context->status_value, LV_LABEL_LONG_DOT);
    context->status_signal = wifi_bars(status, 609, 28, 3, true);
    context->status_detail = ui_ref_label(status, "", 15, UI_FONT_REGULAR, 0xaaaaaa, 576, 52);
    lv_obj_set_width(context->status_detail, 78);
    lv_obj_set_style_text_align(context->status_detail, LV_TEXT_ALIGN_RIGHT, 0);
    ui_ref_label(root, "附近网络", 17, UI_FONT_SEMIBOLD, UI_REF_INK, 29, 218);
    context->refresh = ui_ref_button(root, 633, 211, 58, 36, 18, "wifi_connect");
    context->refresh_label = ui_centered_label(context->refresh, "刷新", ui_ref_font(15, UI_FONT_MEDIUM), UI_REF_INK);
    lv_obj_add_event_cb(context->refresh, wifi_scan_clicked, LV_EVENT_CLICKED, context);

    context->list = ui_box(root, 22, 261, 676, 437, 0, 0);
    lv_obj_set_style_bg_opa(context->list, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(context->list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(context->list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(context->list, LV_SCROLLBAR_MODE_OFF);
    context->empty = ui_centered_label(context->list, "正在搜索 WiFi…", ui_ref_font(15, UI_FONT_REGULAR), UI_REF_MUTED);
    lv_obj_set_width(context->empty, 628);

    context->overlay = ui_box(root, 0, 0, UI_WIDTH, UI_HEIGHT, 0xefefef, 0);
    lv_obj_set_style_bg_opa(context->overlay, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(context->overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(context->overlay, wifi_overlay_event, LV_EVENT_CLICKED, context);
    context->password_panel = ui_box(context->overlay, 22, 135, 676, 238, 0xffffff, 32);
    ui_ref_surface(context->password_panel, "wifi_password");
    ui_ref_label(context->password_panel, "输入密码以连接", 15, UI_FONT_REGULAR, UI_REF_FAINT, 22, 25);
    context->password_ssid = ui_ref_label(context->password_panel, "", 23, UI_FONT_SEMIBOLD, UI_REF_INK, 22, 48);
    lv_obj_set_width(context->password_ssid, 632);
    lv_label_set_long_mode(context->password_ssid, LV_LABEL_LONG_DOT);
    context->password_textarea = lv_textarea_create(context->password_panel);
    lv_obj_remove_style_all(context->password_textarea);
    lv_obj_set_pos(context->password_textarea, 22, 93);
    lv_obj_set_size(context->password_textarea, 632, 58);
    lv_textarea_set_one_line(context->password_textarea, true);
    lv_textarea_set_max_length(context->password_textarea, 63);
    lv_textarea_set_password_mode(context->password_textarea, true);
    lv_textarea_set_placeholder_text(context->password_textarea, "至少 8 位密码");
    lv_obj_set_style_bg_color(context->password_textarea, lv_color_hex(0xe7e7e7), 0);
    lv_obj_set_style_bg_opa(context->password_textarea, LV_OPA_COVER, 0);
    ui_ref_surface(context->password_textarea, "wifi_input");
    lv_obj_set_style_radius(context->password_textarea, 29, 0);
    ui_resources_set_font(context->password_textarea, ui_ref_font(17, UI_FONT_REGULAR), 0);
    lv_obj_set_style_pad_left(context->password_textarea, 22, 0);
    lv_obj_set_style_pad_right(context->password_textarea, 22, 0);
    lv_obj_set_style_pad_top(context->password_textarea, 16, 0);
    lv_obj_set_style_text_color(context->password_textarea, lv_color_hex(UI_REF_INK), 0);
    lv_obj_set_size(context->password_textarea, 632, 58);
    lv_obj_add_event_cb(context->password_textarea, wifi_password_changed, LV_EVENT_VALUE_CHANGED, context);

    lv_obj_t *cancel = ui_ref_button(context->password_panel, 22, 165, 311, 50, 25, "wifi_action");
    ui_centered_label(cancel, "取消", ui_ref_font(15, UI_FONT_MEDIUM), UI_REF_INK);
    lv_obj_add_event_cb(cancel, wifi_cancel_event, LV_EVENT_CLICKED, context);
    context->connect_button = ui_button(context->password_panel, 344, 165, 310, 50, UI_REF_CLAY, 25);
    ui_centered_label(context->connect_button, "连接", ui_ref_font(15, UI_FONT_SEMIBOLD), 0xffffff);
    lv_obj_set_style_opa(context->connect_button, 102, LV_STATE_DISABLED);
    lv_obj_add_state(context->connect_button, LV_STATE_DISABLED);
    lv_obj_add_event_cb(context->connect_button, wifi_password_event, LV_EVENT_CLICKED, context);

    context->keyboard = lv_keyboard_create(context->overlay);
    lv_obj_set_align(context->keyboard, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(context->keyboard, 0, WIFI_KEYBOARD_Y);
    lv_obj_set_size(context->keyboard, UI_WIDTH, WIFI_KEYBOARD_HEIGHT);
    lv_keyboard_set_textarea(context->keyboard, context->password_textarea);
    lv_keyboard_set_popovers(context->keyboard, false);
    lv_obj_set_style_bg_color(context->keyboard, lv_color_hex(0xe9e9e9), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(context->keyboard, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(context->keyboard, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_row(context->keyboard, 5, LV_PART_MAIN);
    ui_resources_set_font(context->keyboard, ui_ref_font(17, UI_FONT_SEMIBOLD), LV_PART_ITEMS);
    lv_obj_set_style_text_color(context->keyboard, lv_color_hex(UI_REF_INK), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(context->keyboard, lv_color_hex(0xffffff), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(context->keyboard, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_border_width(context->keyboard, 0, LV_PART_ITEMS);
    lv_obj_set_style_radius(context->keyboard, 8, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(context->keyboard, lv_color_hex(0xdcdcdc), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_add_event_cb(context->keyboard, wifi_keyboard_event, LV_EVENT_READY, context);
    lv_obj_add_event_cb(context->keyboard, wifi_keyboard_event, LV_EVENT_CANCEL, context);
    lv_obj_add_flag(context->overlay, LV_OBJ_FLAG_HIDDEN);

    context->state = BINBOT_WIFI_IDLE;
    wifi_set_status(context);
    context->timer = lv_timer_create(wifi_timer_cb, 250, context);
    (void)binbot_wifi_manager_scan(context->manager);
    return (ui_screen_t){root, context, NULL, wifi_destroy, NULL};
}
