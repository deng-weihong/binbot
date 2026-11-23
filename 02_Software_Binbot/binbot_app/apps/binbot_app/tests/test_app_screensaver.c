#define _DEFAULT_SOURCE
#include "ui_internal.h"
#include "draw/snapshot/lv_snapshot.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static time_t wall_time = 1792125240;
time_t __wrap_time(time_t *value) { if(value) *value = wall_time; return wall_time; }
static lv_indev_data_t pointer_data;
static void read_pointer(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev; *data = pointer_data;
}

static lv_obj_t *label(lv_obj_t *root, const char *text)
{
    if(lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), text)) return root;
    for(unsigned i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = label(lv_obj_get_child(root, i), text);
        if(found) return found;
    }
    return NULL;
}

static lv_obj_t *thumbnail(lv_obj_t *root)
{
    if(lv_obj_check_type(root, &lv_image_class)) {
        const void *src=lv_image_get_src(root);
        if(src&&lv_image_src_get_type(src)==LV_IMAGE_SRC_VARIABLE) {
            const lv_image_dsc_t *image=src;
            if(image->header.w==152&&image->header.h==152&&image->header.cf==LV_COLOR_FORMAT_RGB565)return root;
        }
    }
    for(unsigned i=0;i<lv_obj_get_child_count(root);++i) {
        lv_obj_t *found=thumbnail(lv_obj_get_child(root,i));if(found)return found;
    }
    return NULL;
}

static uint32_t thumbnail_hash(lv_obj_t *image)
{
    const lv_image_dsc_t *src=lv_image_get_src(image);assert(src&&src->data_size<=48*1024);
    uint32_t hash=2166136261U;
    for(unsigned i=0;i<src->data_size;++i)hash=(hash^src->data[i])*16777619U;
    return hash;
}

static void thumbnail_matches_file(const char *id, lv_obj_t *choice)
{
    const char *assets = getenv("BINBOT_ASSET_ROOT");
    assert(assets && *assets);
    char path[512];
    int length = snprintf(path, sizeof(path), "%s/standby/previews/%s.rgb565", assets, id);
    assert(length > 0 && (size_t)length < sizeof(path));
    FILE *file = fopen(path, "rb"); assert(file);
    lv_obj_t *image = thumbnail(choice); assert(image);
    const lv_image_dsc_t *buffer = lv_image_get_src(image);
    assert(buffer->data_size == 152 * 152 * 2);
    uint8_t row[152 * 2];
    for(unsigned y = 0; y < 152; ++y) {
        assert(fread(row, 1, sizeof(row), file) == sizeof(row));
        assert(memcmp(row, buffer->data + y * buffer->header.stride, sizeof(row)) == 0);
    }
    assert(fgetc(file) == EOF && !ferror(file));
    fclose(file);
}

static void advance(unsigned ms)
{
    while(ms) {
        unsigned step = ms > 33 ? 33 : ms;
        lv_tick_inc(step); lv_timer_handler(); ms -= step;
    }
    lv_obj_update_layout(lv_screen_active());
}

static unsigned thumbnail_count(lv_obj_t *root)
{
    unsigned n = 0;
    if(lv_obj_check_type(root, &lv_image_class)) {
        const void *src = lv_image_get_src(root);
        if(src && lv_image_src_get_type(src) == LV_IMAGE_SRC_VARIABLE) {
            const lv_image_dsc_t *image = src;
            n = image->header.w == 152 && image->header.h == 152;
        }
    }
    for(unsigned i = 0; i < lv_obj_get_child_count(root); ++i)
        n += thumbnail_count(lv_obj_get_child(root, i));
    return n;
}

static lv_obj_t *reveal_choice(lv_obj_t *page, const char *name)
{
    lv_obj_t *text = label(page, name); assert(text);
    lv_obj_t *choice = lv_obj_get_parent(text), *list = lv_obj_get_parent(choice);
    lv_obj_scroll_to_y(list, lv_obj_get_y(choice) - 6, LV_ANIM_OFF);
    advance(650);
    assert(thumbnail_count(page) > 0 && thumbnail_count(page) <= 12);
    assert(thumbnail(choice));
    return choice;
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    (void)area; (void)pixels; lv_display_flush_ready(display);
}

static void snapshot(ui_app_t *app, const char *name)
{
    const char *out = getenv("BINBOT_SAVER_SNAPSHOT_DIR");
    if(!out) return;
    char path[512]; snprintf(path, sizeof(path), "%s/saver-%s-native.ppm", out, name);
    lv_draw_buf_t *frame = lv_snapshot_take(app->root, LV_COLOR_FORMAT_RGB888); assert(frame);
    FILE *file = fopen(path, "wb"); assert(file); fprintf(file, "P6\n720 720\n255\n");
    for(unsigned y = 0; y < 720; ++y) for(unsigned x = 0; x < 720; ++x) {
        const uint8_t *p = frame->data + y * frame->header.stride + x * 3;
        uint8_t rgb[] = {p[2], p[1], p[0]}; assert(fwrite(rgb, 1, 3, file) == 3);
    }
    fclose(file); lv_draw_buf_destroy(frame);
}

static uint32_t eye_hash(lv_obj_t *root)
{
    lv_obj_t *image = lv_obj_get_child(root, 0);
    const lv_image_dsc_t *mask = lv_image_get_src(image);
    assert(mask->header.cf == LV_COLOR_FORMAT_A8 && mask->data_size == 480 * 288);
    uint32_t hash = 2166136261U;
    for(unsigned i = 0; i < mask->data_size; ++i) hash = (hash ^ mask->data[i]) * 16777619U;
    return hash;
}

int main(void)
{
    setenv("BINBOT_INITIAL_ROUTE", "apps", 1);
    lv_init(); lv_rand_set_seed(5);
    lv_display_t *display = lv_display_create(720, 720); assert(display);
    static uint8_t buffer[720 * 64 * 4];
    lv_display_set_buffers(display, buffer, NULL, sizeof(buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    app_state_t *state = app_state_create(); assert(state);
    ui_app_t *app = ui_app_create(state); assert(app && app->screensaver);
    lv_indev_t *pointer = lv_indev_create(); assert(pointer);
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer, read_pointer);
    advance(500);
    assert(!ui_screensaver_active(app->screensaver));
    assert(!label(app->current.root, "小伴表情"));
    assert(!label(app->current.root, "Screensaver"));
    snapshot(app, "more");
    lv_obj_t *page = app->current.root;
    ui_screensaver_show(app->screensaver);
    assert(ui_screensaver_active(app->screensaver) && app->current.root == page);
    advance(400);
    lv_obj_t *overlay = lv_obj_get_child(app->root, -1);
    assert(!lv_obj_has_flag(overlay, LV_OBJ_FLAG_EVENT_BUBBLE));
    snapshot(app, "idle");
    uint32_t first = eye_hash(overlay);
    advance(1000);
    assert(first != eye_hash(overlay));
    assert(!ui_app_open_control_center_drag(app, 300));
    ui_app_open_control_center(app);
    assert(!app->control_center || !ui_control_center_is_active(app->control_center));

    bool seen[4] = {0};
    const char *names[] = {"blush", "spark", "anger", "zzz"};
    unsigned visible_for[4] = {0};
    for(unsigned i = 0; i < 12000 && !(seen[0] && seen[1] && seen[2] && seen[3]); ++i) {
        advance(33);
        for(unsigned k = 0; k < 4; ++k) {
            lv_obj_t *effect = lv_obj_get_child(overlay, (int32_t)k + 1);
            if(lv_obj_get_style_opa(effect, 0) == 255) visible_for[k] += 33;
            else visible_for[k] = 0;
            if(!seen[k] && visible_for[k] >= 300) {
                const char *source = lv_image_get_src(effect); assert(source);
                lv_image_header_t header; assert(lv_image_decoder_get_info(source, &header) == LV_RESULT_OK);
                assert(header.w > 0 && header.h > 0);
                snapshot(app, names[k]); seen[k] = true;
            }
        }
    }
    assert(seen[0] && seen[1] && seen[2] && seen[3]);
    /* Waking over Settings must not pass the same touch to its menu tile. */
    pointer_data.point = (lv_point_t){440, 490};
    pointer_data.state = LV_INDEV_STATE_PRESSED; advance(33);
    pointer_data.state = LV_INDEV_STATE_RELEASED; advance(66);
    assert(!ui_screensaver_active(app->screensaver));
    assert(app->route == UI_ROUTE_APPS && app->current.root == page && !app->route_pending);

    lv_display_trigger_activity(display); advance(59000);
    assert(!ui_screensaver_active(app->screensaver));
    lv_display_trigger_activity(display); advance(59000);
    assert(!ui_screensaver_active(app->screensaver));
    advance(1250); assert(ui_screensaver_active(app->screensaver));
    ui_app_handle_wake_detected(app, 0); advance(66);
    assert(!ui_screensaver_active(app->screensaver) && app->route == UI_ROUTE_ASSISTANT);
    ui_app_handle_dialog_state(app, BINBOT_DIALOG_STATE_LISTENING);
    advance(61000); assert(!ui_screensaver_active(app->screensaver));
    ui_app_handle_dialog_state(app, BINBOT_DIALOG_STATE_IDLE);
    advance(59000); assert(!ui_screensaver_active(app->screensaver));
    ui_screensaver_show(app->screensaver); assert(ui_screensaver_active(app->screensaver));
    ui_app_handle_encoder_count(app, 0, false);
    ui_app_handle_encoder_count(app, 1, true); advance(33);
    assert(!ui_screensaver_active(app->screensaver));
    ui_screensaver_show(app->screensaver);
    ui_app_handle_slave_input(app, 2, false); advance(33);
    assert(!ui_screensaver_active(app->screensaver));
    for(unsigned i = 0; i < 20; ++i) {
        ui_screensaver_show(app->screensaver); advance(66);
        ui_screensaver_wake(app->screensaver); advance(33);
    }
    ui_screensaver_show(app->screensaver);
    ui_screensaver_wake(app->screensaver);
    advance(33);
    ui_app_navigate(app, UI_ROUTE_SCREENSAVER_SETTINGS); advance(650);
    assert(app->route == UI_ROUTE_SCREENSAVER_SETTINGS);
    assert(label(app->current.root, "表盘"));
    assert(label(app->current.root, "选择表盘") && label(app->current.root, "32 款"));
    assert(!label(app->current.root, "屏保样式"));
    const binbot_ambient_display_t *themes = ui_screensaver_display(app->screensaver);
    assert(binbot_ambient_theme_count(themes) == 32);
    lv_obj_t *still=thumbnail(app->current.root);assert(still);
    thumbnail_matches_file(binbot_ambient_theme_at(themes, 0)->id, app->current.root);
    assert(lv_obj_get_child_count(lv_obj_get_parent(still))==1);
    uint32_t still_hash=thumbnail_hash(still);advance(1200);
    assert(thumbnail(app->current.root)==still&&thumbnail_hash(still)==still_hash);
    snapshot(app, "settings");
    page = app->current.root;
    lv_obj_t *standby = reveal_choice(page, "小伴的房间");
    lv_obj_send_event(standby, LV_EVENT_CLICKED, NULL); advance(66);
    assert(strcmp(app_state_model(state)->screensaver.theme_id, "face"));
    for(unsigned i = 1; i < 32; ++i) {
        const binbot_ambient_theme_t *theme = binbot_ambient_theme_at(themes, i);
        lv_obj_t *choice = reveal_choice(page, theme->name);
        thumbnail_matches_file(theme->id, choice);
        lv_obj_send_event(choice, LV_EVENT_CLICKED, NULL); advance(66);
        assert(!strcmp(app_state_model(state)->screensaver.theme_id, theme->id));
        assert(thumbnail(page));
        lv_obj_t *preview = label(page, "立即预览"); assert(preview);
        lv_obj_send_event(lv_obj_get_parent(preview), LV_EVENT_CLICKED, NULL);
        advance(400); assert(ui_screensaver_active(app->screensaver));
        assert(!thumbnail(page));
        assert(!strcmp(binbot_ambient_current_theme(themes), theme->id));
        snapshot(app, theme->id);
        ui_screensaver_wake(app->screensaver); advance(650);
        assert(app->current.root == page && app->route == UI_ROUTE_SCREENSAVER_SETTINGS);
        assert(thumbnail(page));
    }
    /* One gallery contains Expressions and every standby face. Selecting a face
     * turns shuffle off; enabling shuffle from Expressions restores Standby. */
    lv_obj_t *expression = reveal_choice(page, "小伴表情");
    thumbnail_matches_file("face", expression);
    lv_obj_send_event(expression, LV_EVENT_CLICKED, NULL); advance(66);
    assert(!strcmp(app_state_model(state)->screensaver.theme_id, "face"));
    assert(!strcmp(app_state_model(state)->screensaver.standby_id, "little-grove"));
    lv_obj_t *shuffle = NULL;
    lv_obj_t *options = lv_obj_get_parent(label(page, "闲置后显示"));
    for(unsigned i = 0; i < lv_obj_get_child_count(options); ++i) {
        lv_obj_t *child = lv_obj_get_child(options, i);
        if(lv_obj_has_flag(child, LV_OBJ_FLAG_CLICKABLE) && lv_obj_get_width(child) == 61) shuffle = child;
    }
    assert(shuffle);
    lv_obj_send_event(shuffle, LV_EVENT_CLICKED, NULL); advance(66);
    assert(!strcmp(app_state_model(state)->screensaver.theme_id, "little-grove"));
    assert(app_state_model(state)->screensaver.shuffle);
    assert(label(page, "每次显示时从全部表盘中随机挑选"));
    lv_obj_t *preview = label(page, "立即预览"); assert(preview);
    lv_obj_send_event(lv_obj_get_parent(preview), LV_EVENT_CLICKED, NULL); advance(66);
    assert(ui_screensaver_active(app->screensaver) && !thumbnail(page));
    assert(strcmp(binbot_ambient_current_theme(themes), "face"));
    ui_screensaver_wake(app->screensaver); advance(650);
    lv_obj_send_event(reveal_choice(page, "小森林"), LV_EVENT_CLICKED, NULL); advance(66);
    assert(!app_state_model(state)->screensaver.shuffle);
    lv_obj_send_event(lv_obj_get_parent(label(page, "从不")), LV_EVENT_CLICKED, NULL);
    lv_display_trigger_activity(display); advance(61000);
    assert(!ui_screensaver_active(app->screensaver));
    lv_obj_send_event(lv_obj_get_parent(label(page, "30 秒")), LV_EVENT_CLICKED, NULL);
    lv_display_trigger_activity(display); advance(29000); assert(!ui_screensaver_active(app->screensaver));
    advance(1250); assert(ui_screensaver_active(app->screensaver));
    /* A tap exposes controls; previous/next wrap across the 31 standby faces. */
    overlay = lv_obj_get_child(app->root, -1);
    lv_obj_send_event(overlay, LV_EVENT_CLICKED, NULL);
    lv_obj_t *prev = label(overlay, "‹"); assert(prev);
    lv_obj_send_event(lv_obj_get_parent(prev), LV_EVENT_CLICKED, NULL); advance(66);
    assert(!strcmp(binbot_ambient_current_theme(themes), "hourglass"));
    pointer_data = (lv_indev_data_t){.point={610,420},.state=LV_INDEV_STATE_PRESSED}; advance(50);
    for(int i=1;i<=12;++i){pointer_data.point.x=610-i*40;advance(50);}
    pointer_data.state=LV_INDEV_STATE_RELEASED; advance(100);
    assert(!strcmp(binbot_ambient_current_theme(themes), "little-grove"));
    overlay = lv_obj_get_child(app->root, -1);
    lv_obj_t *exit = label(overlay, "退出待机"); assert(exit);
    lv_obj_send_event(lv_obj_get_parent(exit), LV_EVENT_CLICKED, NULL); advance(66);
    assert(!ui_screensaver_active(app->screensaver));
    app_screensaver_settings_t settings = app_state_model(state)->screensaver;
    settings.shuffle = true; assert(app_state_set_screensaver(state, &settings));
    char previous[BINBOT_AMBIENT_ID_SIZE] = "face";
    for(unsigned i = 0; i < 10; ++i) {
        ui_screensaver_show(app->screensaver);
        const char *id = binbot_ambient_current_theme(ui_screensaver_display(app->screensaver));
        assert(id && strcmp(previous, id)); snprintf(previous, sizeof(previous), "%s", id);
        advance(66); ui_screensaver_wake(app->screensaver); advance(33);
    }
    ui_app_go_back(app); advance(66); assert(app->route == UI_ROUTE_SETTINGS);
    snapshot(app, "main-settings");
    ui_app_destroy(app); app_state_destroy(state); advance(1000);
    assert(lv_mem_test() == LV_RESULT_OK);
    lv_deinit();
    puts("Screensaver: original animations, settings, all themes, preview, idle policy, shuffle, touch/voice/hardware wake and cleanup passed");
    return 0;
}
