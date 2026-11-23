#define _DEFAULT_SOURCE
#include "ui_internal.h"
#include "draw/snapshot/lv_snapshot.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static lv_obj_t *label(lv_obj_t *root, const char *text)
{
    if(lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root),text)) return root;
    for(unsigned i=0;i<lv_obj_get_child_count(root);++i) {
        lv_obj_t *found = label(lv_obj_get_child(root,i),text); if(found) return found;
    }
    return NULL;
}
static void settle(void) { lv_tick_inc(300); lv_timer_handler(); lv_obj_update_layout(lv_screen_active()); }
static void click(ui_app_t *app, const char *text)
{
    lv_obj_t *found = label(app->current.root,text); assert(found);
    lv_obj_send_event(lv_obj_get_parent(found),LV_EVENT_CLICKED,NULL); settle();
}
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{ (void)area; (void)pixels; lv_display_flush_ready(display); }
static void snapshot(ui_app_t *app, const char *name)
{
    const char *directory = getenv("BINBOT_PEN_SNAPSHOT"); if(!directory) return;
    char path[512]; snprintf(path,sizeof(path),"%s/%s.ppm",directory,name);
    lv_draw_buf_t *frame=lv_snapshot_take(app->root,LV_COLOR_FORMAT_RGB888); assert(frame);
    FILE *file=fopen(path,"wb"); assert(file); fprintf(file,"P6\n720 720\n255\n");
    for(unsigned y=0;y<720;++y) for(unsigned x=0;x<720;++x) {
        const uint8_t *p=frame->data+y*frame->header.stride+x*3;
        uint8_t rgb[]={p[2],p[1],p[0]}; assert(fwrite(rgb,1,3,file)==3);
    }
    fclose(file); lv_draw_buf_destroy(frame);
}
static void confirmed(ui_app_t *app, bb_component_worker_t *worker,
                      binbot_component_event_t *event, unsigned mode, unsigned face, bool success)
{
    assert(worker->command_pending);
    memcpy(event->request_id,worker->pending_command.request_id,16);
    worker->command_pending=false;
    event->status=success?BINBOT_COMPONENT_SUCCEEDED:BINBOT_COMPONENT_FAILED;
    event->event_seq++;
    snprintf(event->message,sizeof(event->message),"{\"pen\":{\"supported\":true,\"mode\":%u,\"face\":%u}}",mode,face);
    app_modules_handle_event(app->modules,event); settle();
    assert(!app_modules_pen_busy(app->modules));
}

static void weather_fixture(const char *path, unsigned code, unsigned day, int temperature)
{
    FILE *file=fopen(path,"wb"); assert(file);
    fprintf(file,"{\"current\":{\"temperature_2m\":%d,\"weather_code\":%u,\"is_day\":%u,\"time\":%ld},"
                 "\"daily\":{\"temperature_2m_min\":[-5],\"temperature_2m_max\":[26]}}",
            temperature,code,day,(long)time(NULL));
    fclose(file);
}

int main(void)
{
    char root[]="/tmp/binbot-pen-ui-XXXXXX"; assert(mkdtemp(root));
    setenv("BINBOT_MODULE_IMAGE_ROOT",root,1); setenv("BINBOT_COMPONENT_STATE_DIR",root,1);
    setenv("BINBOT_COMPONENT_UART","/nonexistent-pen-test",1); setenv("BINBOT_INITIAL_ROUTE","modules",1);
    char path[512]; snprintf(path,sizeof(path),"%s/wget",root);
    FILE *file=fopen(path,"wb"); assert(file);
    fprintf(file,"#!/bin/sh\n/bin/cp \"$BINBOT_WEATHER_TEST_JSON\" \"$5\"\n"); fclose(file); assert(!chmod(path,0700));
    setenv("PATH",root,1); setenv("BINBOT_WEATHER_CITY","test-city",1);
    snprintf(path,sizeof(path),"%s/fixture.json",root); setenv("BINBOT_WEATHER_TEST_JSON",path,1);
    file=fopen(path,"wb"); assert(file);
    fprintf(file,"{\"results\":[{\"latitude\":30,\"longitude\":120}],\"current\":{\"temperature_2m\":22,\"weather_code\":0,\"time\":%ld},\"daily\":{\"temperature_2m_min\":[18],\"temperature_2m_max\":[26]}}",(long)time(NULL));
    fclose(file);
    lv_init(); lv_display_t *display=lv_display_create(720,720); assert(display);
    static uint8_t buffer[720*64*4]; lv_display_set_buffers(display,buffer,NULL,sizeof(buffer),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display,flush);
    app_state_t *state=app_state_create(); ui_app_t *app=ui_app_create(state); assert(app);
    bb_component_worker_t worker; bb_component_worker_init(&worker);
    worker.pid=1; worker.active_action=BINBOT_COMPONENT_PROBE; app_modules_set_worker(app->modules,&worker);
    binbot_component_event_t event={.has_info=1,.profile=1,.healthy=1,.boot_id=42,.status=BINBOT_COMPONENT_PROBED,.event_seq=1};
    strcpy(event.message,"{\"pen\":{\"supported\":false}}"); app_modules_handle_event(app->modules,&event);
    click(app,"笔筒"); assert(!app_modules_pen_supported(app->modules));
    strcpy(event.message,"{\"pen\":{\"supported\":true,\"mode\":0,\"face\":0}}");
    event.event_seq++; app_modules_handle_event(app->modules,&event); settle();
    assert(app_modules_pen_supported(app->modules));
    lv_tick_inc(5000); lv_timer_handler();
    for(unsigned i=0;i<200 && !(app_modules_pen_state(app->modules)->flags&BB_PEN_WEATHER_VALID);++i) {
        app_modules_tick(app->modules,1000+i*20); usleep(10000);
    }
    assert(app_modules_pen_state(app->modules)->flags&BB_PEN_WEATHER_VALID);
    assert(app_modules_pen_state(app->modules)->temperature==220);
    click(app,"随机切换"); assert(app_modules_random_interval(app->modules,1)==60);
    click(app,"每 10 秒"); assert(app_modules_random_interval(app->modules,1)==10);
    snapshot(app,"pen-random");
    click(app,"时间与天气");
    assert(app_modules_pen_busy(app->modules) && app_modules_pen_state(app->modules)->mode==BB_PEN_IMAGE);
    bb_component_pen_t pending;
    assert(bb_component_pen_decode((uint8_t *)worker.pending_command.url,BB_COMPONENT_PEN_BYTES,&pending));
    assert(pending.mode==BB_PEN_INFO && pending.flags==3 && pending.utc_offset>=-43200);
    confirmed(app,&worker,&event,1,0,true);
    assert(label(app->current.root,"表盘样式 Face")); snapshot(app,"pen-info");
    assert(app_modules_random_interval(app->modules,1)==10);
    assert(!label(app->current.root,"模拟天气 Weather"));
    assert(!label(app->current.root,"演示数据"));
    click(app,"仅天气");
    assert(bb_component_pen_decode((uint8_t *)worker.pending_command.url,BB_COMPONENT_PEN_BYTES,&pending));
    assert(pending.face==BB_PEN_WEATHER); confirmed(app,&worker,&event,1,2,true); snapshot(app,"pen-weather");
    click(app,"仅时间"); confirmed(app,&worker,&event,1,2,false);
    assert(app_modules_pen_state(app->modules)->face==BB_PEN_WEATHER);
    click(app,"仅时间"); confirmed(app,&worker,&event,1,1,true); snapshot(app,"pen-time");
    static const unsigned codes[]={0,2,3,61,95,73,45,0};
    static const unsigned conditions[]={BB_PEN_SUN,BB_PEN_CLOUD,BB_PEN_OVERCAST,BB_PEN_RAIN,
                                       BB_PEN_STORM,BB_PEN_SNOW,BB_PEN_FOG,BB_PEN_NIGHT};
    for(unsigned i=0;i<8;++i) {
        weather_fixture(path,codes[i],i!=7,10+i);
        for(unsigned poll=0;poll<200;++poll) {
            app_modules_tick(app->modules,(i+1)*1801000U+poll*20);
            if(app_modules_pen_state(app->modules)->temperature==(int)(10+i)*10) break;
            usleep(10000);
        }
        assert(app_modules_pen_state(app->modules)->temperature==(int)(10+i)*10);
        assert(app_modules_pen_state(app->modules)->condition==conditions[i]);
        click(app,"仅天气");
        assert(bb_component_pen_decode((uint8_t *)worker.pending_command.url,BB_COMPONENT_PEN_BYTES,&pending));
        assert(pending.condition==conditions[i]);
        confirmed(app,&worker,&event,1,2,true);
        char name[40]; snprintf(name,sizeof(name),"pen-live-weather-%u",i); snapshot(app,name);
    }
    click(app,"显示图片"); confirmed(app,&worker,&event,0,1,true);
    assert(!label(app->current.root,"表盘样式 Face")); assert(label(app->current.root,"每 10 秒"));
    assert(!app_modules_random_set(app->modules,1,11)); assert(!app_modules_pen_set(app->modules,2,0));
    event.boot_id++; event.status=BINBOT_COMPONENT_PROBED; event.event_seq++; strcpy(event.message,"{}");
    app_modules_handle_event(app->modules,&event); settle(); assert(!app_modules_pen_supported(app->modules));
    event.has_info=0; event.event_seq++; app_modules_handle_event(app->modules,&event); settle();
    assert(label(app->current.root,"笔筒未插入"));
    worker.pid=0; bb_component_worker_close(&worker); ui_app_destroy(app); app_state_destroy(state); lv_deinit();
    app_modules_t *restored=app_modules_create(root); assert(restored);
    assert(app_modules_random_interval(restored,1)==10); app_modules_destroy(restored);
    const char *files[]={"wget","fixture.json","display.json",".weather.json",".weather.bin"};
    for(unsigned i=0;i<sizeof(files)/sizeof(files[0]);++i) { snprintf(path,sizeof(path),"%s/%s",root,files[i]); unlink(path); }
    assert(!rmdir(root));
    puts("pen UI, weather sync, confirmed state, failure retry, preferences and unplug passed");
    return 0;
}
