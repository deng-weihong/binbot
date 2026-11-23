#include "../ui_internal.h"
#include "binbot/app_diagnostics.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct { const char *name, *english, *icon; } tests[] = {
    {"左声道麦克风", "Mic · Left", "mic"}, {"右声道麦克风", "Mic · Right", "mic"},
    {"扬声器", "Speaker", "speaker"}, {"屏幕", "Display", "monitor"},
};
static const uint32_t colors[] = {0xff0000, 0x00ff00, 0x0000ff, 0xffffff, 0, 0};
static const char *color_names[] = {"红", "绿", "蓝", "白", "黑", "灰阶"};
static const char *headings[] = {"MIC · LEFT", "MIC · RIGHT", "SPEAKER", "DISPLAY"};

enum { METER_HEIGHT = 271, PROGRESS_WIDTH = 122 };

typedef struct diagnostics_context diagnostics_context_t;
typedef struct { diagnostics_context_t *context; unsigned index; } diag_binding_t;

struct diagnostics_context {
    ui_app_t *app;
    app_diagnostics_t *audio;
    lv_timer_t *timer;
    lv_obj_t *root, *panel;
    lv_obj_t *tabs[4], *tab_icons[4], *tab_circles[4], *tab_names[4], *tab_english[4];
    diag_binding_t bindings[4];
    unsigned selected;
    lv_obj_t *meters[2], *level[2], *peak[2], *peak_text[2];
    lv_obj_t *record, *record_label, *record_dot, *play, *play_icon, *play_label;
    lv_obj_t *progress, *record_status;
    lv_obj_t *tones[2], *tone_labels[2], *speaker, *speaker_icon, *rings[3], *audio_status;
    lv_obj_t *overlay, *color_label, *cells[64], *touch_count, *touch_exit, *screen_count;
    int color_index;
    uint64_t touched;
    lv_point_t last_touch;
    bool dragging;
};

static void sync(diagnostics_context_t *c);
static void build_panel(diagnostics_context_t *c);

static lv_obj_t *text(lv_obj_t *parent, const char *value, unsigned size,
                       ui_font_weight_t weight, uint32_t color, int x, int y, int width)
{
    lv_obj_t *label = ui_ref_label(parent, value, size, weight, color, x, y);
    lv_obj_set_width(label, width);
    return label;
}

static void enabled(lv_obj_t *button, bool active)
{
    if(active) lv_obj_remove_state(button, LV_STATE_DISABLED);
    else lv_obj_add_state(button, LV_STATE_DISABLED);
    lv_obj_set_style_opa(button, active ? LV_OPA_COVER : LV_OPA_40, 0);
}

static void back(lv_event_t *event)
{
    diagnostics_context_t *c = lv_event_get_user_data(event);
    ui_app_go_back(c->app);
}

static void select_test(lv_event_t *event)
{
    diag_binding_t *binding = lv_event_get_user_data(event);
    diagnostics_context_t *c = binding->context;
    if(c->selected == binding->index) return;
    app_diagnostics_stop(c->audio);
    c->selected = binding->index;
    app_diagnostics_set_microphone(c->audio, c->selected < 2);
    build_panel(c);
    sync(c);
}

static void record(lv_event_t *event)
{
    diagnostics_context_t *c = lv_event_get_user_data(event);
    app_diagnostics_record(c->audio);
    sync(c);
}

static void play(lv_event_t *event)
{
    diagnostics_context_t *c = lv_event_get_user_data(event);
    app_diag_status_t status = app_diagnostics_status(c->audio);
    if(status.playback) app_diagnostics_stop(c->audio);
    else app_diagnostics_play(c->audio, c->selected ? APP_DIAG_PLAY_RIGHT : APP_DIAG_PLAY_LEFT);
    sync(c);
}

static void tone(lv_event_t *event)
{
    diag_binding_t *binding = lv_event_get_user_data(event);
    diagnostics_context_t *c = binding->context;
    app_diag_play_t kind = binding->index ? APP_DIAG_PLAY_SWEEP : APP_DIAG_PLAY_TONE;
    if(app_diagnostics_status(c->audio).playback == kind) app_diagnostics_stop(c->audio);
    else app_diagnostics_play(c->audio, kind);
    sync(c);
}

static unsigned touch_total(const diagnostics_context_t *c)
{
    unsigned count = 0;
    for(unsigned i = 0; i < 64; ++i) count += (unsigned)((c->touched >> i) & 1);
    return count;
}

static void finish_display(lv_event_t *event)
{
    diagnostics_context_t *c = lv_event_get_user_data(event);
    lv_event_stop_bubbling(event);
    lv_obj_delete_async(c->overlay);
    c->overlay = NULL;
    c->color_index = -1;
    char value[32]; snprintf(value, sizeof(value), "%u/64 格", touch_total(c));
    lv_label_set_text(c->screen_count, value);
}

static void mark_touch(diagnostics_context_t *c, int x, int y)
{
    if(x < 0 || y < 0 || x >= UI_WIDTH || y >= UI_HEIGHT) return;
    unsigned index = (unsigned)(y / 90 * 8 + x / 90);
    uint64_t bit = UINT64_C(1) << index;
    if(c->touched & bit) return;
    c->touched |= bit;
    lv_obj_set_style_bg_color(c->cells[index], lv_color_hex(0x10b981), 0);
    lv_obj_set_style_bg_opa(c->cells[index], 204, 0);
    char value[80]; snprintf(value, sizeof(value), "滑动填满所有格子 · %u/64", touch_total(c));
    lv_label_set_text(c->touch_count, value);
    if(c->touched == UINT64_MAX) lv_label_set_text(c->touch_exit, "完成");
}

static void touch(lv_event_t *event)
{
    diagnostics_context_t *c = lv_event_get_user_data(event);
    if(c->color_index != 6) return;
    lv_event_code_t code = lv_event_get_code(event);
    if(code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) { c->dragging = false; return; }
    if(code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING) return;
    lv_indev_t *input = lv_event_get_indev(event);
    if(!input) return;
    lv_point_t point; lv_indev_get_point(input, &point);
    if(code == LV_EVENT_PRESSING && c->dragging) {
        int dx = point.x - c->last_touch.x, dy = point.y - c->last_touch.y;
        int steps = (abs(dx) > abs(dy) ? abs(dx) : abs(dy)) / 15 + 1;
        for(int i = 1; i <= steps; ++i)
            mark_touch(c, c->last_touch.x + dx * i / steps, c->last_touch.y + dy * i / steps);
    }
    else mark_touch(c, point.x, point.y);
    c->last_touch = point;
    c->dragging = true;
}

static void next_color(lv_event_t *event)
{
    diagnostics_context_t *c = lv_event_get_user_data(event);
    lv_event_stop_bubbling(event);
    if(c->color_index >= 6) return;
    ++c->color_index;
    lv_obj_set_style_bg_grad_dir(c->overlay, LV_GRAD_DIR_NONE, 0);
    if(c->color_index < 6) {
        lv_obj_set_style_bg_color(c->overlay, lv_color_hex(colors[c->color_index]), 0);
        if(c->color_index == 5) {
            lv_obj_set_style_bg_grad_color(c->overlay, lv_color_hex(0xffffff), 0);
            lv_obj_set_style_bg_grad_dir(c->overlay, LV_GRAD_DIR_HOR, 0);
        }
        char value[96]; snprintf(value, sizeof(value), "%s · %d/6 · 轻触下一项",
                                  color_names[c->color_index], c->color_index + 1);
        lv_label_set_text(c->color_label, value);
        return;
    }
    lv_obj_clean(c->overlay);
    lv_obj_set_style_bg_color(c->overlay, lv_color_hex(0x111111), 0);
    for(unsigned i = 0; i < 64; ++i) {
        c->cells[i] = ui_box(c->overlay, (int)(i % 8) * 90, (int)(i / 8) * 90, 90, 90, 0x111111, 0);
        lv_obj_set_style_bg_opa(c->cells[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(c->cells[i], 1, 0);
        lv_obj_set_style_border_color(c->cells[i], lv_color_hex(0xffffff), 0);
        lv_obj_set_style_border_opa(c->cells[i], LV_OPA_10, 0);
    }
    lv_obj_t *toolbar = ui_box(c->overlay, 136, 16, 448, 44, 0xffffff, 22);
    lv_obj_set_style_bg_opa(toolbar, 230, 0);
    c->touch_count = text(toolbar, "滑动填满所有格子 · 0/64", 15, UI_FONT_MEDIUM,
                           UI_REF_INK, 18, 12, 330);
    lv_obj_t *exit = ui_button(toolbar, 366, 6, 70, 32, UI_REF_INK, 16);
    c->touch_exit = ui_centered_label(exit, "结束", ui_ref_font(14, UI_FONT_REGULAR), 0xffffff);
    lv_obj_add_event_cb(exit, finish_display, LV_EVENT_CLICKED, c);
}

static void start_display(lv_event_t *event)
{
    diagnostics_context_t *c = lv_event_get_user_data(event);
    if(c->overlay) return;
    c->touched = 0; c->dragging = false; c->color_index = 0;
    lv_label_set_text(c->screen_count, "0/64 格");
    c->overlay = ui_button(c->root, 0, 0, UI_WIDTH, UI_HEIGHT, colors[0], 0);
    lv_obj_set_style_transform_scale(c->overlay, 256, LV_STATE_PRESSED);
    lv_obj_set_style_transform_width(c->overlay, 0, LV_STATE_PRESSED);
    lv_obj_set_style_transform_height(c->overlay, 0, LV_STATE_PRESSED);
    lv_obj_t *hint = ui_box(c->overlay, 220, 660, 280, 40, 0, 20);
    lv_obj_set_style_bg_opa(hint, 102, 0);
    c->color_label = ui_centered_label(hint, "红 · 1/6 · 轻触下一项", ui_ref_font(15, UI_FONT_REGULAR), 0xffffff);
    lv_obj_add_event_cb(c->overlay, next_color, LV_EVENT_CLICKED, c);
    lv_obj_add_event_cb(c->overlay, touch, LV_EVENT_ALL, c);
}

static void build_microphone(diagnostics_context_t *c)
{
    const char *description = c->selected ? "对着设备右侧麦克风说话或轻敲，观察右声道电平，并录音回放检查音质。"
                                           : "对着设备左侧麦克风说话或轻敲，观察左声道电平，并录音回放检查音质。";
    lv_obj_t *description_label = text(c->panel, description, 15, UI_FONT_REGULAR, UI_REF_MUTED, 22, 82, 404);
    lv_obj_set_style_text_line_space(description_label, 5, 0);
    for(unsigned i = 0; i < 2; ++i) {
        bool selected = c->selected == i;
        c->meters[i] = ui_box(c->panel, 22 + (int)i * 208, 139, 196, 362,
                               selected ? 0xffffff : 0xe7e7e7, 22);
        if(selected) {
            lv_obj_set_style_outline_color(c->meters[i], lv_color_hex(UI_REF_CLAY), 0);
            lv_obj_set_style_outline_opa(c->meters[i], 153, 0);
            lv_obj_set_style_outline_width(c->meters[i], 2, 0);
        }
        else lv_obj_set_style_opa(c->meters[i], 153, 0);
        lv_obj_t *name = text(c->meters[i], i ? "右声道 R" : "左声道 L", 15,
                              UI_FONT_MEDIUM, UI_REF_INK, 0, 16, 196);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_t *track = ui_box(c->meters[i], 74, 47, 48, METER_HEIGHT, 0xf5f5f5, 24);
        lv_obj_set_style_clip_corner(track, true, 0);
        c->level[i] = ui_box(track, 0, METER_HEIGHT, 48, 0, selected ? UI_REF_CLAY : 0xbdbdbd, 24);
        c->peak[i] = ui_box(track, 4, METER_HEIGHT - 3, 40, 3, 0x767676, 2);
        c->peak_text[i] = text(c->meters[i], "峰值 0", 14, UI_FONT_REGULAR, UI_REF_FAINT, 0, 325, 196);
        lv_obj_set_style_text_align(c->peak_text[i], LV_TEXT_ALIGN_CENTER, 0);
    }
    lv_obj_t *toolbar = ui_box(c->panel, 22, 513, 404, 67, 0xe7e7e7, 22);
    ui_ref_surface(toolbar, "diag_toolbar");
    c->record = ui_button(toolbar, 8, 10, 112, 48, 0xffffff, 16);
    c->record_dot = ui_box(c->record, 15, 18, 12, 12, UI_REF_CLAY, 6);
    c->record_label = text(c->record, "录音 5 秒", 15, UI_FONT_MEDIUM, UI_REF_INK, 35, 14, 76);
    lv_obj_add_event_cb(c->record, record, LV_EVENT_CLICKED, c);
    lv_obj_t *progress = ui_box(toolbar, 132, 8, PROGRESS_WIDTH, 8, 0xffffff, 4);
    c->progress = ui_box(progress, 0, 0, 0, 8, UI_REF_CLAY, 4);
    c->record_status = text(toolbar, "等待麦克风数据", 13, UI_FONT_REGULAR, UI_REF_MUTED, 132, 20, PROGRESS_WIDTH);
    lv_obj_set_style_text_line_space(c->record_status, 3, 0);
    c->play = ui_button(toolbar, 266, 10, 130, 48, 0xffffff, 16);
    c->play_icon = ui_ref_icon(c->play, "play", 17, UI_REF_INK);
    lv_obj_set_pos(c->play_icon, 13, 16);
    c->play_label = text(c->play, c->selected ? "播放右声道" : "播放左声道", 15,
                           UI_FONT_MEDIUM, UI_REF_INK, 38, 14, 90);
    lv_obj_add_event_cb(c->play, play, LV_EVENT_CLICKED, c);
}

static void build_speaker(diagnostics_context_t *c)
{
    lv_obj_t *description = text(c->panel, "设备只有一个扬声器。播放测试音，听声音是否清晰、有无破音或杂音。",
                                  15, UI_FONT_REGULAR, UI_REF_MUTED, 22, 82, 404);
    lv_obj_set_style_text_line_space(description, 5, 0);
    for(unsigned i = 0; i < 3; ++i) {
        c->rings[i] = ui_box(c->panel, 144, 252, 160, 160, 0xffffff, 100);
        lv_obj_set_style_bg_opa(c->rings[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(c->rings[i], lv_color_hex(UI_REF_CLAY), 0);
        lv_obj_set_style_border_width(c->rings[i], 2, 0);
    }
    c->speaker = ui_box(c->panel, 168, 269, 112, 112, 0xe7e7e7, 56);
    c->speaker_icon = ui_ref_icon(c->speaker, "speaker", 44, UI_REF_MUTED);
    lv_obj_center(c->speaker_icon);
    c->audio_status = text(c->panel, "", 14, UI_FONT_REGULAR, UI_REF_CLAY, 22, 490, 404);
    lv_obj_set_style_text_align(c->audio_status, LV_TEXT_ALIGN_CENTER, 0);
    for(unsigned i = 0; i < 2; ++i) {
        c->tones[i] = ui_ref_button(c->panel, 22 + (int)i * 208, 524, 196, 56, 20, "diag_tone");
        c->tone_labels[i] = ui_centered_label(c->tones[i], i ? "扫频 200 Hz–8 kHz" : "1 kHz 测试音",
                                               ui_ref_font(16, UI_FONT_MEDIUM), UI_REF_INK);
        lv_obj_add_event_cb(c->tones[i], tone, LV_EVENT_CLICKED, &c->bindings[i]);
    }
}

static void build_display(diagnostics_context_t *c)
{
    lv_obj_t *description = text(c->panel, "依次显示纯色画面检查坏点、漏光与色偏，然后滑动填满触摸格子检查触控。轻触画面切换下一项。",
                                  15, UI_FONT_REGULAR, UI_REF_MUTED, 22, 82, 404);
    lv_obj_set_style_text_line_space(description, 5, 0);
    for(unsigned i = 0; i < 6; ++i) {
        lv_obj_t *swatch = ui_box(c->panel, 22 + ((int)i * 412 + 3) / 6, 277, 61, 61, colors[i], 14);
        lv_obj_set_style_border_color(swatch, lv_color_hex(0), 0);
        lv_obj_set_style_border_opa(swatch, LV_OPA_10, 0);
        lv_obj_set_style_border_width(swatch, 1, 0);
        if(i == 5) {
            lv_obj_set_style_bg_grad_color(swatch, lv_color_hex(0xffffff), 0);
            lv_obj_set_style_bg_grad_dir(swatch, LV_GRAD_DIR_HOR, 0);
        }
        lv_obj_t *name = text(swatch, color_names[i], 13, UI_FONT_REGULAR,
                                i == 1 || i == 3 ? 0x333333 : 0xffffff, 6, 35, 48);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_RIGHT, 0);
    }
    text(c->panel, "触摸测试", 15, UI_FONT_REGULAR, UI_REF_MUTED, 22, 490, 240);
    char value[32]; snprintf(value, sizeof(value), "%u/64 格", touch_total(c));
    c->screen_count = text(c->panel, value, 15, UI_FONT_MEDIUM, UI_REF_INK, 306, 490, 120);
    lv_obj_set_style_text_align(c->screen_count, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_t *start = ui_button(c->panel, 22, 524, 404, 56, UI_REF_INK, 20);
    ui_centered_label(start, "开始屏幕测试", ui_ref_font(17, UI_FONT_MEDIUM), 0xffffff);
    lv_obj_add_event_cb(start, start_display, LV_EVENT_CLICKED, c);
}

static void build_panel(diagnostics_context_t *c)
{
    lv_obj_clean(c->panel);
    text(c->panel, headings[c->selected], 15, UI_FONT_REGULAR, UI_REF_FAINT, 22, 22, 404);
    text(c->panel, tests[c->selected].name, 24, UI_FONT_SEMIBOLD, UI_REF_INK, 22, 43, 404);
    for(unsigned i = 0; i < 4; ++i) {
        bool active = i == c->selected;
        lv_obj_set_style_bg_image_opa(c->tabs[i], active ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(c->tabs[i], lv_color_hex(active ? UI_REF_INK : 0xffffff), 0);
        lv_obj_set_style_shadow_opa(c->tabs[i], active ? 0 : 31, 0);
        lv_obj_set_style_bg_color(c->tab_circles[i], lv_color_hex(active ? 0x3c3c3c : 0xe7e7e7), 0);
        ui_icon_set_color(c->tab_icons[i], active ? 0xffffff : UI_REF_INK);
        lv_obj_set_style_text_color(c->tab_names[i], lv_color_hex(active ? 0xffffff : UI_REF_INK), 0);
        lv_obj_set_style_text_color(c->tab_english[i], lv_color_hex(active ? 0x8d8d8d : UI_REF_FAINT), 0);
    }
    if(c->selected < 2) build_microphone(c);
    else if(c->selected == 2) build_speaker(c);
    else build_display(c);
}

static const char *audio_error(app_diag_status_t status)
{
    if(!status.connected) return "音频服务未连接";
    switch(status.error) {
        case APP_DIAG_ERROR_CAPTURE: return "等待麦克风数据";
        case APP_DIAG_ERROR_GAP: return "录音丢帧，请重新录音";
        case APP_DIAG_ERROR_PLAYBACK: return "播放失败，请重试";
        case APP_DIAG_ERROR_BUSY: return "语音对话中，请稍后";
        default: return "";
    }
}

static void sync(diagnostics_context_t *c)
{
    app_diag_status_t status = app_diagnostics_status(c->audio);
    const app_model_t *model = app_state_model(c->app->state);
    bool muted = model && model->volume == 0;
    if(c->selected < 2) {
        for(unsigned i = 0; i < 2; ++i) {
            int height = status.level[i] * METER_HEIGHT / 100;
            lv_obj_set_height(c->level[i], height);
            lv_obj_set_y(c->level[i], METER_HEIGHT - height);
            lv_obj_set_style_bg_color(c->level[i], lv_color_hex(status.level[i] > 85 ? 0xe8463c :
                                      c->selected == i ? UI_REF_CLAY : 0xbdbdbd), 0);
            lv_obj_set_y(c->peak[i], METER_HEIGHT - 3 - status.peak[i] * (METER_HEIGHT - 3) / 100);
            char value[32]; snprintf(value, sizeof(value), "峰值 %u", status.peak[i]);
            lv_label_set_text(c->peak_text[i], value);
        }
        enabled(c->record, status.capture_ready && !status.recording);
        enabled(c->play, status.recorded && !status.recording && !status.busy && status.connected);
        lv_obj_set_style_bg_color(c->record, lv_color_hex(status.recording ? UI_REF_CLAY : 0xffffff), 0);
        lv_obj_set_style_text_color(c->record_label, lv_color_hex(status.recording ? 0xffffff : UI_REF_INK), 0);
        lv_obj_set_style_bg_color(c->record_dot, lv_color_hex(status.recording ? 0xffffff : UI_REF_CLAY), 0);
        lv_label_set_text(c->record_label, status.recording ? "录音中" : status.recorded ? "重新录音" : "录音 5 秒");
        bool playing = status.playback != APP_DIAG_PLAY_NONE;
        lv_obj_set_style_bg_color(c->play, lv_color_hex(playing ? UI_REF_INK : 0xffffff), 0);
        ui_ref_icon_set_name(c->play_icon, playing ? "pause" : "play");
        ui_icon_set_color(c->play_icon, playing ? 0xffffff : UI_REF_INK);
        lv_obj_set_style_text_color(c->play_label, lv_color_hex(playing ? 0xffffff : UI_REF_INK), 0);
        lv_label_set_text(c->play_label, playing ? "停止" : c->selected ? "播放右声道" : "播放左声道");
        unsigned progress = status.recording ? status.record_ms : playing ? status.playback_ms : status.recorded ? 5000 : 0;
        lv_obj_set_width(c->progress, (int)(progress * PROGRESS_WIDTH / 5000));
        lv_obj_set_style_bg_color(c->progress, lv_color_hex(playing ? UI_REF_INK : UI_REF_CLAY), 0);
        char value[128];
        if(audio_error(status)[0]) snprintf(value, sizeof(value), "%s", audio_error(status));
        else if(status.recording) snprintf(value, sizeof(value), "%u.%u / 5.0 秒", status.record_ms / 1000, status.record_ms / 100 % 10);
        else if(playing && muted) snprintf(value, sizeof(value), "扬声器已静音");
        else if(playing) snprintf(value, sizeof(value), "正在播放%s声道 · %u.%u 秒", status.playback == APP_DIAG_PLAY_RIGHT ? "右" : "左",
                                  status.playback_ms / 1000, status.playback_ms / 100 % 10);
        else snprintf(value, sizeof(value), "%s", status.recorded ? "录音完成，可单独回放各声道" : "录一段声音后回放检查音质");
        lv_label_set_text(c->record_status, value);
    }
    else if(c->selected == 2) {
        bool playing = status.playback != APP_DIAG_PLAY_NONE;
        lv_obj_set_style_bg_color(c->speaker, lv_color_hex(playing ? UI_REF_CLAY : 0xe7e7e7), 0);
        ui_icon_set_color(c->speaker_icon, playing ? 0xffffff : UI_REF_MUTED);
        for(unsigned i = 0; i < 3; ++i) {
            unsigned phase = (lv_tick_get() + i * 500) % 1600;
            int size = 112 + (int)(phase * 104 / 1600);
            lv_obj_set_size(c->rings[i], size, size);
            lv_obj_set_pos(c->rings[i], 224 - size / 2, 325 - size / 2);
            lv_obj_set_style_border_opa(c->rings[i], playing ? (lv_opa_t)((1600 - phase) * 128 / 1600) : 0, 0);
        }
        for(unsigned i = 0; i < 2; ++i) {
            bool active = status.playback == (i ? APP_DIAG_PLAY_SWEEP : APP_DIAG_PLAY_TONE);
            enabled(c->tones[i], status.connected && !status.busy);
            lv_obj_set_style_bg_image_opa(c->tones[i], active ? 0 : 255, 0);
            lv_obj_set_style_bg_color(c->tones[i], lv_color_hex(active ? UI_REF_INK : 0xe7e7e7), 0);
            lv_obj_set_style_text_color(c->tone_labels[i], lv_color_hex(active ? 0xffffff : UI_REF_INK), 0);
            lv_label_set_text(c->tone_labels[i], active ? "停止" : i ? "扫频 200 Hz–8 kHz" : "1 kHz 测试音");
        }
        const char *message = status.busy ? "语音对话中，请稍后" : audio_error(status);
        lv_label_set_text(c->audio_status, message[0] ? message : muted ? "扬声器已静音" : "");
    }
}

static void tick(lv_timer_t *timer) { sync(lv_timer_get_user_data(timer)); }

static void destroy(void *data)
{
    diagnostics_context_t *c = data;
    lv_timer_delete(c->timer);
    app_diagnostics_destroy(c->audio);
    free(c);
}

ui_screen_t ui_diagnostics_screen_create(ui_app_t *app)
{
    diagnostics_context_t *c = calloc(1, sizeof(*c));
    if(!c) return (ui_screen_t){0};
    c->app = app; c->color_index = -1;
    const char *socket_path = app->audio_client ? app->audio_client->socket_path : getenv("BINBOT_AUDIO_SOCKET");
    c->audio = app_diagnostics_create(socket_path);
    app_diagnostics_set_microphone(c->audio, true);
    c->root = ui_ref_page(app);
    lv_obj_t *button = ui_ref_button(c->root, 29, 26, 50, 50, 25, "circle50");
    lv_obj_t *icon = ui_ref_icon(button, "back", 25, UI_REF_INK); lv_obj_center(icon);
    lv_obj_add_event_cb(button, back, LV_EVENT_CLICKED, c);
    ui_ref_label(c->root, "HARDWARE TEST", 15, UI_FONT_REGULAR, UI_REF_FAINT, 93, 27);
    ui_ref_label(c->root, "功能测试", 24, UI_FONT_SEMIBOLD, UI_REF_INK, 93, 48);
    for(unsigned i = 0; i < 4; ++i) {
        c->bindings[i] = (diag_binding_t){c, i};
        c->tabs[i] = ui_ref_button(c->root, 22, 96 + (int)i * 153, 214, 143, 24, "diag_tab");
        ui_ref_shadow(c->tabs[i], 0, 31, 30, -12, 10);
        c->tab_circles[i] = ui_box(c->tabs[i], 16, 49, 44, 44, 0xe7e7e7, 22);
        c->tab_icons[i] = ui_ref_icon(c->tab_circles[i], tests[i].icon, 20, UI_REF_INK);
        lv_obj_center(c->tab_icons[i]);
        c->tab_names[i] = text(c->tabs[i], tests[i].name, 16, UI_FONT_SEMIBOLD, UI_REF_INK, 72, 50, 126);
        c->tab_english[i] = text(c->tabs[i], tests[i].english, 14, UI_FONT_REGULAR, UI_REF_FAINT, 72, 76, 126);
        lv_obj_add_event_cb(c->tabs[i], select_test, LV_EVENT_CLICKED, &c->bindings[i]);
    }
    c->panel = ui_box(c->root, 250, 96, 448, 602, 0xffffff, 30);
    ui_ref_surface(c->panel, "diag_panel");
    ui_ref_shadow(c->panel, 0, 31, 30, -12, 10);
    build_panel(c);
    sync(c);
    c->timer = lv_timer_create(tick, 50, c);
    return (ui_screen_t){c->root, c, NULL, destroy, NULL};
}
