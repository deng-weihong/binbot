#include "lvgl.h"
#include "drivers/evdev/lv_evdev.h"
#include <assert.h>
#include <linux/input.h>
#include <stdio.h>
#include <unistd.h>

static unsigned pressed, released, clicked;
static void event(lv_event_t *e)
{
    if(lv_event_get_code(e)==LV_EVENT_PRESSED)++pressed;
    if(lv_event_get_code(e)==LV_EVENT_RELEASED)++released;
    if(lv_event_get_code(e)==LV_EVENT_CLICKED)++clicked;
}
static void emit(int fd,unsigned short type,unsigned short code,int value)
{
    struct input_event e={.type=type,.code=code,.value=value};
    assert(write(fd,&e,sizeof(e))==sizeof(e));
}
static void flush(lv_display_t *display,const lv_area_t *area,uint8_t *pixels)
{(void)area;(void)pixels;lv_display_flush_ready(display);}
int main(void)
{
    lv_init();lv_display_t *display=lv_display_create(720,720);assert(display);
    static uint8_t buffer[720*64*4];
    lv_display_set_buffers(display,buffer,NULL,sizeof(buffer),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display,flush);
    lv_obj_t *button=lv_button_create(lv_screen_active());
    lv_obj_set_pos(button,100,100);lv_obj_set_size(button,160,160);
    lv_obj_add_event_cb(button,event,LV_EVENT_ALL,NULL);
    int fds[2];assert(!pipe(fds));
    lv_indev_t *pointer=lv_evdev_create_fd(LV_INDEV_TYPE_POINTER,fds[0]);assert(pointer);
    /* A long render can leave entire taps queued before the next input poll. */
    for(unsigned i=0;i<3;++i){
        emit(fds[1],EV_ABS,ABS_MT_TRACKING_ID,123);
        emit(fds[1],EV_ABS,ABS_MT_POSITION_X,150);
        emit(fds[1],EV_ABS,ABS_MT_POSITION_Y,150);
        emit(fds[1],EV_KEY,BTN_TOUCH,1);emit(fds[1],EV_SYN,SYN_REPORT,0);
        emit(fds[1],EV_ABS,ABS_MT_TRACKING_ID,-1);
        emit(fds[1],EV_KEY,BTN_TOUCH,0);emit(fds[1],EV_SYN,SYN_REPORT,0);
    }
    lv_obj_update_layout(lv_screen_active());lv_tick_inc(40);lv_timer_handler();
    assert(pressed==3&&released==3&&clicked==3);
    lv_evdev_delete(pointer);close(fds[1]);lv_deinit();
    puts("Buffered evdev press/release frames preserve all three taps");
    return 0;
}
