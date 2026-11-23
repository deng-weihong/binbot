#define _DEFAULT_SOURCE
#include "binbot/ambient_display.h"
#include "draw/snapshot/lv_snapshot.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{ (void)area; (void)pixels; lv_display_flush_ready(display); }
static void advance(unsigned ms)
{
    while(ms) { unsigned step = ms > 33 ? 33 : ms; lv_tick_inc(step); lv_timer_handler(); ms -= step; }
    lv_obj_update_layout(lv_screen_active());
}
static lv_obj_t *label(lv_obj_t *root, const char *text)
{
    if(lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), text)) return root;
    for(unsigned i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = label(lv_obj_get_child(root, i), text); if(found) return found;
    }
    return NULL;
}
static void write_file(const char *path, const char *text)
{ FILE *f = fopen(path, "w"); assert(f); assert(fputs(text, f) >= 0); assert(!fclose(f)); }
static void destroy_on_wake(void *context)
{ binbot_ambient_destroy(*(binbot_ambient_display_t **)context); }

static void capture(lv_obj_t *root, const char *id)
{
    const char *dir = getenv("BINBOT_AMBIENT_SNAPSHOT_DIR"); if(!dir) return;
    char path[640]; snprintf(path,sizeof(path),"%s/%s.ppm",dir,id);
    lv_draw_buf_t *frame = lv_snapshot_take(root,LV_COLOR_FORMAT_RGB888); assert(frame);
    FILE *f=fopen(path,"wb"); assert(f); fprintf(f,"P6\n720 720\n255\n");
    for(unsigned y=0;y<720;++y)for(unsigned x=0;x<720;++x){const uint8_t *p=frame->data+y*frame->header.stride+x*3;uint8_t rgb[]={p[2],p[1],p[0]};assert(fwrite(rgb,1,3,f)==3);}
    fclose(f);lv_draw_buf_destroy(frame);
}

int main(void)
{
    lv_init();
    lv_display_t *screen = lv_display_create(720, 720); assert(screen);
    static uint8_t buffer[720 * 64 * 4];
    lv_display_set_buffers(screen, buffer, NULL, sizeof(buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(screen, flush);
    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(parent); lv_obj_set_size(parent, 720, 720);
    binbot_ambient_config_t config = {.parent = parent, .asset_root = getenv("BINBOT_ASSET_ROOT"), .preview = true};
    binbot_ambient_display_t *display = binbot_ambient_create(&config); assert(display);
    size_t count = binbot_ambient_theme_count(display); assert(count == 32);
    const char *removed[]={"green-screen","orbit-deck","warm-radio","amber-clock"};
    for(unsigned i=0;i<4;++i)assert(!binbot_ambient_theme_find(display,removed[i]));
    binbot_ambient_snapshot_t data = {.time = "12:34", .date = "2026-11-03", .hour = 12, .minute = 34,
        .weather_valid = true, .city = "Shenzhen", .weather = "Sunny", .weather_icon = "home/weather/sun.png",
        .year=2026,.month=11,.day=4,.weekday=3,.second=26,
        .temperature = 24, .todo_total = 4, .todo_done = 2, .next_todo = "Finish one thing", .focus_seconds = 1499,
        .weather_id="sun",.focus_minutes=32,.todos={{.text="完成屏保还原",.time="14:00"}},
        .events={{.title="项目讨论",.time="15:00",.start=1793833200}},.event_count=1,
        .world_time={"04:34","00:34","13:34"}};
    struct tm end={.tm_year=2026-1900,.tm_mon=10,.tm_mday=4,.tm_hour=15,.tm_min=45,.tm_isdst=-1};
    data.events[0].end=mktime(&end);
    char formatted[160];
    binbot_ambient_format(&data,"{time} {weekday_cn} {eventtime0} {event0} {done}/{total}",formatted,sizeof(formatted));
    assert(!strcmp(formatted,"12:34 周三 15:00 项目讨论 2/4"));
    strcpy(data.todos[1].text,"{time}");
    binbot_ambient_format(&data,"{todo1}",formatted,sizeof(formatted)); assert(!strcmp(formatted,"{time}"));
    data.todos[1].text[0]=0;
    char tiny[3];binbot_ambient_format(&data,"{weekday_cn}",tiny,sizeof(tiny));assert(!tiny[0]);
    for(size_t i = 0; i < count; ++i) {
        const binbot_ambient_theme_t *theme = binbot_ambient_theme_at(display, i); assert(theme);
        assert(binbot_ambient_show(display, theme->id, &data)); advance(400);
        assert(!strcmp(binbot_ambient_current_theme(display), theme->id));
        if(!strcmp(theme->id, "face")) assert(label(parent, "12:34 · 轻触唤醒"));
        capture(parent,theme->id);
        if(!strcmp(theme->id, "paper-cut")) {
            lv_obj_t *at=label(parent,"15:00"),*title=label(parent," 项目讨论");assert(at&&title);
            lv_area_t a,b;lv_obj_get_coords(at,&a);lv_obj_get_coords(title,&b);assert(b.x1>a.x2);
        }
        if(!strcmp(theme->id, "departures")) {
            lv_obj_t *digit=label(parent,"1");assert(digit);
            lv_obj_t *cells=lv_obj_get_parent(lv_obj_get_parent(digit));
            assert(lv_obj_get_child_count(cells)==5);
            assert(label(lv_obj_get_child(cells,4),"4"));
        }
        if(!strcmp(theme->id, "shapes")) {
            assert(label(parent, "24°"));
            data.weather_valid = false; binbot_ambient_update(display, &data);
            assert(label(parent, "--°"));
            data.weather_valid = true;
        }
        if(!strcmp(theme->id, "ban-shell")) {
            assert(label(parent, ":26"));
            assert(label(parent,"15:45"));
            assert(label(parent,"■■")&&label(parent,"□□"));
            data.focus_minutes=0;binbot_ambient_update(display,&data);
            assert(label(parent,"▱▱▱▱▱▱"));
            data.focus_minutes=150;binbot_ambient_update(display,&data);
            lv_obj_t *bar=label(parent,"▰▰▰▰▰▰");assert(bar);
            const unsigned symbols[]={0x25b0,0x275a,0x203f};
            for(unsigned symbol=0;symbol<3;++symbol){
                lv_font_glyph_dsc_t glyph;
                assert(lv_font_get_glyph_dsc(lv_obj_get_style_text_font(bar,0),&glyph,symbols[symbol],0));
                assert(!glyph.is_placeholder);
            }
            data.focus_minutes=32;
            data.event_count=0;binbot_ambient_update(display,&data);
            lv_obj_t *empty=label(parent,"  暂无日程");assert(empty&&!lv_obj_has_flag(empty,LV_OBJ_FLAG_HIDDEN));
            data.event_count=1;
            data.second=27;binbot_ambient_update(display,&data);
            assert(label(parent, ":27"));data.second=26;
        }
        if(!strcmp(theme->id,"sprout-term")){
            assert(label(parent,"WED NOV 04"));
            for(unsigned focus=0;focus<=150;focus+=30){
                data.focus_minutes=focus;binbot_ambient_update(display,&data);advance(33);
                char plant[160];binbot_ambient_format(&data,"{sprout}",plant,sizeof(plant));
                lv_obj_t *sprout=label(parent,plant);assert(sprout);
                assert(lv_obj_get_height(sprout)<=80);
                assert(lv_obj_get_y(sprout)+lv_obj_get_height(sprout)<518);
            }
            data.focus_minutes=32;
        }
        if(!strcmp(theme->id,"hourglass")){
            const unsigned minutes[]={0,1,34,59};
            for(unsigned j=0;j<4;++j){
                data.minute=minutes[j];binbot_ambient_update(display,&data);advance(33);
                char remaining[16];snprintf(remaining,sizeof(remaining),"%u",60-minutes[j]);
                lv_obj_t *n=label(parent,remaining),*unit=label(parent," 分钟");assert(n&&unit);
                lv_area_t a,b;lv_obj_get_coords(n,&a);lv_obj_get_coords(unit,&b);assert(b.x1>a.x2-2);
            }
            data.minute=34;
        }
        if(!strcmp(theme->id,"neon-kitty")){
            assert(label(parent,"还有 2 件事喵～"));
            data.todo_done=4;binbot_ambient_update(display,&data);
            assert(label(parent,"今天全部完成喵！"));data.todo_done=2;
        }
        binbot_ambient_hide(display); assert(!binbot_ambient_active(display));
        assert(!binbot_ambient_reload(display)); advance(33);
        assert(lv_obj_get_child_count(parent) == 0);
        if(!strcmp(theme->id,"sundial")||!strcmp(theme->id,"hourglass")){
            data.hour=23;strcpy(data.time,"23:34");
            assert(binbot_ambient_show(display,theme->id,&data));advance(33);
            assert(!strcmp(binbot_ambient_current_theme(display),theme->id));
            char id[64];snprintf(id,sizeof(id),"%s-night",theme->id);capture(parent,id);
            binbot_ambient_hide(display);advance(33);
            data.hour=12;strcpy(data.time,"12:34");
        }
    }
    /* Repeated creation and deferred destruction must not accumulate LVGL objects. */
    for(unsigned cycle = 0; cycle < 3; ++cycle) for(size_t i = 0; i < count; ++i) {
        assert(binbot_ambient_show(display, binbot_ambient_theme_at(display, i)->id, &data));
        advance(66); binbot_ambient_hide(display); advance(33);
        assert(lv_obj_get_child_count(parent) == 0);
    }
    assert(binbot_ambient_show(display, "removed-theme", &data));
    assert(!strcmp(binbot_ambient_current_theme(display), "face"));
    /* Destroy must drain queued retirements even without another timer turn. */
    binbot_ambient_hide(display); binbot_ambient_destroy(display); advance(66);
    assert(lv_obj_get_child_count(parent) == 0);

    char directory[] = "/tmp/binbot-ambient-test-XXXXXX"; assert(mkdtemp(directory));
    char standby[512], theme_dir[512], index[512], manifest[512], scene[512];
    snprintf(standby, sizeof(standby), "%s/standby", directory); assert(!mkdir(standby, 0700));
    snprintf(theme_dir, sizeof(theme_dir), "%s/standby/bad", directory); assert(!mkdir(theme_dir, 0700));
    snprintf(index, sizeof(index), "%s/standby/index.json", directory);
    snprintf(manifest, sizeof(manifest), "%s/standby/bad/manifest.json", directory);
    snprintf(scene, sizeof(scene), "%s/standby/bad/scene.json", directory);
    write_file(index, "{\"schema\":1,\"themes\":[\"../escape\",\"bad\",\"bad\"]}");
    write_file(manifest, "{\"schema\":1,\"id\":\"bad\",\"name\":\"Bad\",\"renderer\":\"scene\"}");
    write_file(scene, "{\"schema\":1,\"layers\":[{\"type\":\"text\",\"text\":\"valid first\"},{\"type\":\"image\",\"src\":\"../escape.png\"}]}");
    config.asset_root = directory;
    display = binbot_ambient_create(&config); assert(display);
    assert(binbot_ambient_theme_count(display) == 2);
    assert(binbot_ambient_show(display, "bad", &data));
    assert(!strcmp(binbot_ambient_current_theme(display), "face"));
    binbot_ambient_hide(display); advance(33);
    write_file(manifest, "{\"schema\":99,\"id\":\"bad\",\"name\":\"Bad\",\"renderer\":\"scene\"}");
    assert(binbot_ambient_reload(display)); assert(binbot_ambient_theme_count(display) == 1);
    write_file(index, "{broken");
    assert(!binbot_ambient_reload(display)); assert(binbot_ambient_theme_count(display) == 1);
    binbot_ambient_destroy(display);
    assert(!unlink(scene) && !unlink(manifest) && !unlink(index));
    assert(!rmdir(theme_dir) && !rmdir(standby) && !rmdir(directory));
    /* Destruction from a touch callback is part of the public lifecycle contract. */
    config.asset_root = getenv("BINBOT_ASSET_ROOT");
    config.preview = false;
    config.wake = destroy_on_wake; config.user_data = &display;
    display = binbot_ambient_create(&config); assert(display);
    assert(binbot_ambient_show(display, "face", &data));
    lv_obj_send_event(lv_obj_get_child(parent, 0), LV_EVENT_CLICKED, NULL);
    advance(66); assert(lv_obj_get_child_count(parent) == 0);
    assert(lv_mem_test() == LV_RESULT_OK);
    lv_deinit();
    puts("Independent display: data, packages, malformed scenes, fallback, repeated lifecycle and callback destruction passed");
    return 0;
}
