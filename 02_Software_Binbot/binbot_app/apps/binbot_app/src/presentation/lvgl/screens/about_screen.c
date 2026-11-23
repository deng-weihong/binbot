#include "../ui_internal.h"
#include "binbot/build_info.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>

typedef struct {
    ui_app_t *app;
    lv_timer_t *timer;
    lv_obj_t *mcu_version, *uptime, *storage, *storage_percent, *storage_ring;
    lv_obj_t *refresh, *refresh_label;
    uint32_t last_version_tap;
    unsigned version_taps;
    lv_obj_t *version_toast;
    lv_timer_t *version_toast_timer;
} about_context_t;

static void clear_version_toast(about_context_t *context)
{
    if(context->version_toast_timer) lv_timer_delete(context->version_toast_timer);
    if(context->version_toast) lv_obj_delete(context->version_toast);
    context->version_toast_timer = NULL;
    context->version_toast = NULL;
}

static void version_toast_expired(lv_timer_t *timer)
{
    clear_version_toast(lv_timer_get_user_data(timer));
}

static void about_version(lv_event_t *event)
{
    about_context_t *context = lv_event_get_user_data(event);
    uint32_t now = lv_tick_get();
    context->version_taps = context->version_taps && now - context->last_version_tap < 1500
        ? context->version_taps + 1 : 1;
    context->last_version_tap = now;
    if(context->version_taps == 5) {
        context->version_taps = 0;
        clear_version_toast(context);
        ui_app_navigate(context->app, UI_ROUTE_DIAGNOSTICS);
    }
    else if(context->version_taps >= 2) {
        char text[64];
        snprintf(text, sizeof(text), "再点 %u 次进入功能测试", 5 - context->version_taps);
        clear_version_toast(context);
        context->version_toast = ui_box(context->app->current.root, 180, 630, 360, 54, UI_REF_INK, 27);
        lv_obj_set_style_bg_opa(context->version_toast, LV_OPA_90, 0);
        ui_centered_label(context->version_toast, text, ui_ref_font(16, UI_FONT_MEDIUM), 0xffffff);
        context->version_toast_timer = lv_timer_create(version_toast_expired, 1200, context);
    }
}

static void about_back(lv_event_t *event)
{
    ui_app_go_back(lv_event_get_user_data(event));
}

static void about_unavailable(lv_event_t *event)
{
    ui_app_show_toast(lv_event_get_user_data(event), "该服务尚未接入");
}

static void about_sync(about_context_t *context)
{
    app_firmware_t *firmware = &context->app->firmware;
    char text[80];
    switch(firmware->status) {
        case APP_FIRMWARE_READY:
            snprintf(text, sizeof(text), "v%u · %s", firmware->version,
                     firmware->healthy ? "已确认" : "待确认");
            break;
        case APP_FIRMWARE_READING: snprintf(text, sizeof(text), "读取中…"); break;
        case APP_FIRMWARE_BUSY: snprintf(text, sizeof(text), "升级中，请稍后"); break;
        case APP_FIRMWARE_OFFLINE: snprintf(text, sizeof(text), "未连接"); break;
        case APP_FIRMWARE_UNSUPPORTED: snprintf(text, sizeof(text), "版本读取不可用"); break;
        default: snprintf(text, sizeof(text), "未读取"); break;
    }
    lv_label_set_text(context->mcu_version, text);
    lv_label_set_text(context->refresh_label, firmware->requested ? "读取中" : "刷新版本");
    if(firmware->requested) lv_obj_add_state(context->refresh, LV_STATE_DISABLED);
    else lv_obj_remove_state(context->refresh, LV_STATE_DISABLED);

    struct sysinfo info;
    if(sysinfo(&info) == 0 && info.uptime >= 0) {
        unsigned long minutes = (unsigned long)info.uptime / 60;
        if(minutes >= 1440)
            snprintf(text, sizeof(text), "%lu 天 %lu 小时", minutes / 1440, minutes / 60 % 24);
        else if(minutes >= 60)
            snprintf(text, sizeof(text), "%lu 小时 %lu 分钟", minutes / 60, minutes % 60);
        else snprintf(text, sizeof(text), "%lu 分钟", minutes);
    }
    else snprintf(text, sizeof(text), "不可用");
    lv_label_set_text(context->uptime, text);

    struct statvfs storage;
    if(statvfs("/oem", &storage) == 0 && storage.f_blocks) {
        double total = (double)storage.f_blocks * storage.f_frsize;
        double used = (double)(storage.f_blocks - storage.f_bfree) * storage.f_frsize;
        unsigned percent = (unsigned)(used * 100.0 / total + 0.5);
        double unit = total >= 1024.0 * 1024 * 1024 ? 1024.0 * 1024 * 1024 : 1024.0 * 1024;
        snprintf(text, sizeof(text), "%.1f / %.1f %s", used / unit, total / unit,
                 unit > 1024.0 * 1024 ? "GB" : "MB");
        lv_label_set_text(context->storage, text);
        snprintf(text, sizeof(text), "%u%%", percent);
        lv_label_set_text(context->storage_percent, text);
        lv_arc_set_value(context->storage_ring, (int32_t)percent);
    }
    else {
        lv_label_set_text(context->storage, "不可用");
        lv_label_set_text(context->storage_percent, "--");
        lv_arc_set_value(context->storage_ring, 0);
    }
}

static void about_timer(lv_timer_t *timer)
{
    about_sync(lv_timer_get_user_data(timer));
}

static void about_refresh(lv_event_t *event)
{
    about_context_t *context = lv_event_get_user_data(event);
    app_firmware_refresh(&context->app->firmware);
    about_sync(context);
}

static lv_obj_t *about_value(lv_obj_t *parent, const char *label, const char *value, int32_t y)
{
    ui_ref_label(parent, label, 16, UI_FONT_REGULAR, UI_REF_MUTED, 20, y);
    lv_obj_t *text = ui_ref_label(parent, value, 16, UI_FONT_MEDIUM, UI_REF_INK, 111, y);
    lv_obj_set_width(text, 256);
    lv_label_set_long_mode(text, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_RIGHT, 0);
    return text;
}

static void about_destroy(void *user_data)
{
    about_context_t *context = user_data;
    if(!context) return;
    if(context->timer) lv_timer_delete(context->timer);
    clear_version_toast(context);
    free(context);
}

ui_screen_t ui_about_screen_create(ui_app_t *app)
{
    about_context_t *context = calloc(1, sizeof(*context));
    if(!context) return (ui_screen_t){0};
    context->app = app;
    lv_obj_t *root = ui_ref_page(app);
    lv_obj_t *back = ui_ref_button(root, 29, 26, 50, 50, 25, "circle50");
    lv_obj_t *back_icon = ui_ref_icon(back, "back", 25, UI_REF_INK);
    lv_obj_center(back_icon);
    lv_obj_add_event_cb(back, about_back, LV_EVENT_CLICKED, app);
    ui_ref_label(root, "ABOUT", 15, UI_FONT_REGULAR, UI_REF_FAINT, 93, 27);
    ui_ref_label(root, "关于本机", 24, UI_FONT_SEMIBOLD, UI_REF_INK, 93, 48);

    lv_obj_t *summary = ui_box(root, 22, 96, 676, 132, 0xffffff, 30);
    ui_ref_surface(summary, "about_summary");
    lv_obj_t *logo = ui_box(summary, 20, 22, 88, 88, UI_REF_INK, 28);
    ui_ref_shadow(logo, UI_REF_CLAY, 110, 30, -14, 16);
    ui_box(logo, 28, 36, 10, 16, 0xffffff, 5);
    ui_box(logo, 50, 36, 10, 16, 0xffffff, 5);
    ui_box(logo, 32, 62, 24, 6, UI_REF_CLAY, 3);
    ui_ref_label(summary, "小伴", 30, UI_FONT_SEMIBOLD, UI_REF_INK, 128, 18);
    ui_ref_label(summary, "XiaoBan", 22, UI_FONT_REGULAR, UI_REF_FAINT, 194, 27);
    ui_ref_label(summary, "你桌上的 AI 小伙伴", 16, UI_FONT_REGULAR, UI_REF_MUTED, 128, 61);
    lv_obj_t *build = ui_ref_label(summary, "RV1106 v" BINBOT_APP_VERSION " · " BINBOT_APP_BUILD_DATE " 构建",
                                     14, UI_FONT_REGULAR, UI_REF_FAINT, 128, 91);
    lv_obj_set_width(build, 408);
    lv_label_set_long_mode(build, LV_LABEL_LONG_DOT);
    context->refresh = ui_button(summary, 552, 38, 104, 56, UI_REF_CLAY, 28);
    ui_ref_shadow(context->refresh, UI_REF_CLAY, 100, 24, -10, 12);
    context->refresh_label = ui_centered_label(context->refresh, "刷新版本", ui_ref_font(16, UI_FONT_MEDIUM), 0xffffff);
    lv_obj_add_event_cb(context->refresh, about_refresh, LV_EVENT_CLICKED, context);

    lv_obj_t *info = ui_box(root, 22, 240, 387, 390, 0xffffff, 26);
    ui_ref_surface(info, "about_info");
    const char *device = getenv("BINBOT_DEVICE_ID");
    about_value(info, "设备型号", "RV1106 / CH582", 28);
    about_value(info, "设备编号", device && device[0] ? device : "未配置", 90);
    lv_obj_t *version = ui_button(info, 8, 136, 371, 62, 0xffffff, 0);
    lv_obj_set_style_bg_opa(version, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(version, LV_OPA_10, LV_STATE_PRESSED);
    ui_ref_label(version, "系统版本", 16, UI_FONT_REGULAR, UI_REF_MUTED, 12, 17);
    lv_obj_t *version_value = ui_ref_label(version, "v" BINBOT_APP_VERSION, 16,
                                            UI_FONT_MEDIUM, UI_REF_INK, 103, 17);
    lv_obj_set_width(version_value, 256);
    lv_obj_set_style_text_align(version_value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_add_event_cb(version, about_version, LV_EVENT_CLICKED, context);
    context->mcu_version = about_value(info, "MCU 固件", "读取中…", 216);
    char mac[32] = "未连接";
    FILE *file = fopen("/sys/class/net/wlan0/address", "r");
    if(file) { if(fscanf(file, "%31s", mac) != 1) snprintf(mac, sizeof(mac), "不可用"); fclose(file); }
    about_value(info, "Wi-Fi MAC", mac, 278);
    context->uptime = about_value(info, "运行时间", "", 341);
    for(int i = 1; i < 6; ++i) ui_box(info, 20, 20 + i * 63, 347, 1, 0xf1f1f1, 0);

    lv_obj_t *storage = ui_box(root, 421, 240, 277, 230, 0xffffff, 26);
    ui_ref_surface(storage, "about_storage");
    context->storage_ring = ui_ref_ring(storage, 18, 78, 74, 8);
    context->storage_percent = ui_ref_label(storage, "--", 16, UI_FONT_SEMIBOLD, UI_REF_INK, 18, 104);
    lv_obj_set_width(context->storage_percent, 74);
    lv_obj_set_style_text_align(context->storage_percent, LV_TEXT_ALIGN_CENTER, 0);
    ui_ref_label(storage, "存储 Storage", 15, UI_FONT_REGULAR, UI_REF_FAINT, 108, 78);
    context->storage = ui_ref_label(storage, "", 16, UI_FONT_SEMIBOLD, UI_REF_INK, 108, 106);
    lv_obj_set_width(context->storage, 153);
    lv_label_set_long_mode(context->storage, LV_LABEL_LONG_DOT);
    ui_ref_label(storage, "设备数据 · 应用", 14, UI_FONT_REGULAR, UI_REF_FAINT, 108, 132);

    lv_obj_t *links = ui_box(root, 421, 482, 277, 148, 0xffffff, 26);
    ui_ref_surface(links, "about_links");
    const char *legal[] = {"用户协议", "隐私政策", "开源许可"};
    for(int i = 0; i < 3; ++i) {
        lv_obj_t *button = ui_button(links, 8, 8 + i * 44, 261, 44, 0xffffff, 16);
        lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
        ui_ref_label(button, legal[i], 16, UI_FONT_REGULAR, 0x404040, 12, 11);
        lv_obj_t *arrow = ui_ref_icon(button, "back", 17, 0xd4d4d4);
        lv_obj_set_pos(arrow, 232, 14);
        lv_image_set_rotation(lv_obj_get_child(arrow, 0), 1800);
        lv_obj_add_event_cb(button, about_unavailable, LV_EVENT_CLICKED, app);
    }
    const char *actions[] = {"重启设备", "恢复出厂设置"};
    const char *icons[] = {"undo", "trash"};
    for(int i = 0; i < 2; ++i) {
        uint32_t color = i ? UI_REF_CLAY : UI_REF_INK;
        lv_obj_t *button = ui_ref_button(root, 22 + i * 344, 642, 332, 56, 28, "about_action");
        lv_obj_t *icon = ui_ref_icon(button, icons[i], 19, color);
        lv_obj_set_pos(icon, i ? 104 : 122, 19);
        ui_ref_label(button, actions[i], 16, UI_FONT_MEDIUM, color, i ? 131 : 149, 17);
        lv_obj_add_event_cb(button, about_unavailable, LV_EVENT_CLICKED, app);
    }
    app_firmware_refresh(&app->firmware);
    about_sync(context);
    context->timer = lv_timer_create(about_timer, 250, context);
    return (ui_screen_t){root, context, NULL, about_destroy, NULL};
}
