/* Data-driven geometry from the source SVG/terminal components. Decorative
 * shapes remain the exact compiled artwork; these shapes depend on live data. */
static void live_rect(lv_layer_t *canvas, int x, int y, int w, int h, uint32_t color)
{
    if(w <= 0 || h <= 0) return;
    lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(color); d.bg_opa = 255;
    lv_area_t a = {x, y, x + w - 1, y + h - 1}; lv_draw_rect(canvas, &d, &a);
}
static float cubic(float a, float b, float c, float d, float t)
{
    float u = 1 - t; return u*u*u*a + 3*u*u*t*b + 3*u*t*t*c + t*t*t*d;
}
static float glass_left(float y)
{
    bool upper = y < 360; float lo = 0, hi = 1;
    for(unsigned i = 0; i < 14; ++i) {
        float t = (lo + hi) / 2;
        float v = upper ? cubic(132,250,300,360,t) : cubic(360,420,470,588,t);
        if(v < y) lo = t; else hi = t;
    }
    return upper ? cubic(410,410,492,492,(lo+hi)/2) : cubic(492,492,410,410,(lo+hi)/2);
}
static void live_draw(lv_event_t *event)
{
    layer_t *l = lv_event_get_user_data(event); reference_t *s = l->scene;
    const binbot_ambient_snapshot_t *d = &s->data;
    lv_layer_t *canvas = lv_event_get_layer(event);
    lv_area_t a; lv_obj_get_coords(l->object, &a);
    if(l->kind == BLOCKTIME) {
        static const char *const digits[] = {"111101101101111","010110010010111","111001111100111","111001111001111","101101111001001",
            "111100111001111","111100111101111","111001010010010","111101111101111","111101111001111"};
        float u = l->w / 30, rh = l->h / 5, x = a.x1;
        for(const char *p = d->time; *p; ++p) {
            unsigned columns = *p == ':' ? 1 : 3;
            for(unsigned row = 0; row < 5; ++row) for(unsigned col = 0; col < columns; ++col) {
                bool on = *p == ':' ? row == 1 || row == 3 : *p >= '0' && *p <= '9' && digits[*p-'0'][row*3+col] == '1';
                if(on) live_rect(canvas, lroundf(x+col*2*u), lroundf(a.y1+row*rh), lroundf(2*u), lroundf(a.y1+(row+1)*rh)-lroundf(a.y1+row*rh), l->color);
            }
            x += (columns*2+1)*u;
        }
    }
    else if(l->kind == SAND) {
        float remain = 1 - (d->minute + d->second/60.f)/60, top = 354-remain*196, ph = (1-remain)*180+8;
        for(int y = 132; y < 588; ++y) {
            int start = -1; float left = glass_left(y+.5f);
            for(int x = 400; x <= 600; ++x) {
                float q = (x-500)/100.f, curve = 1-q*q;
                float surface = y < 360 ? top+7*curve : 588-ph*.5f-(ph*.5f+34)*.5f*curve;
                bool on = x < 600 && x >= left && x <= 1000-left && y >= surface;
                if(on && start < 0) start = x;
                if(!on && start >= 0) {
                    float k = (y-132)/456.f;
                    uint32_t color = ((unsigned)(240-39*k)<<16) | ((unsigned)(194-51*k)<<8) | (unsigned)(122-53*k);
                    live_rect(canvas,a.x1+start-400,a.y1+y-132,x-start,1,color); start = -1;
                }
            }
        }
        int end = lroundf(588-ph*.75f-17), shift = (lv_tick_get()-s->started)/21%12;
        for(int y = 358; y < end; ++y) if((y+shift)%12 < 7)
            live_rect(canvas,a.x1+99,a.y1+y-132,3,1,0xe0a95e);
    }
    else if(l->kind == TRACE) {
        float position = fmodf(((float)(lv_tick_get()-s->started)-l->span)/l->from*640,640);
        float distance = 0;
        for(unsigned i = 1; i < l->point_count; ++i) {
            lv_point_precise_t p = l->points[i-1], q = l->points[i];
            float dx = q.x-p.x, dy = q.y-p.y, len = hypotf(dx,dy);
            if(len < .01f) continue;
            for(int k = -2; k < 5; ++k) {
                float lo = fmaxf(distance,k*640-position), hi = fminf(distance+len,k*640-position+18);
                if(hi > lo) led_line(canvas,a.x1+p.x+dx*(lo-distance)/len,a.y1+p.y+dy*(lo-distance)/len,
                    a.x1+p.x+dx*(hi-distance)/len,a.y1+p.y+dy*(hi-distance)/len,4,l->color,255);
            }
            distance += len;
        }
    }
}
static void indicator_update(layer_t *l, const binbot_ambient_snapshot_t *d)
{
    unsigned index = 0; bool on = false; uint32_t c = 0x0f3a40;
    if(sscanf(l->binding,"second%u",&index) == 1) { on = index*2 < (unsigned)d->second; c = on ? 0xff4fd8 : 0x0f3a40; }
    else if(sscanf(l->binding,"task%u",&index) == 1) {
        bool exists = index < 5 && d->todos[index].text[0];
        lv_obj_set_flag(l->object,LV_OBJ_FLAG_HIDDEN,!exists);
        c = exists && d->todos[index].done ? 0x7dffb0 : 0xffb547;
    }
    else if(!strcmp(l->binding,"shellcaret"))c=0xe8643c;
    else if(sscanf(l->binding,"hour%u",&index) == 1) {
        on = (unsigned)d->hour%12 == index; c = on ? 0xc8743f : 0x927c65;
        int r = on ? 7 : 4;
        lv_obj_set_size(l->object,r*2,r*2);
        lv_obj_set_pos(l->object,lroundf(l->x+l->w/2-r),lroundf(l->y+l->h/2-r));
    }
    lv_obj_set_style_bg_color(l->object,lv_color_hex(c),0);
}
static void shell_tick(reference_t *s, uint32_t elapsed)
{
    const char *commands[] = {"ban water","ban play","ban todo next","ban hug"};
    unsigned tick = elapsed/90, i = 0, cycle = 0;
    for(unsigned j = 0; j < 4; ++j) cycle += strlen(commands[j])+48;
    tick %= cycle;
    while(tick >= strlen(commands[i])+48) { tick -= strlen(commands[i])+48; ++i; }
    char cmd[32], reply[512] = "";
    snprintf(cmd,sizeof(cmd),"%.*s",(int)tick,commands[i]);
    if(tick >= strlen(commands[i])+8) {
        const char *formats[] = {"小伴：起来喝口水，走两步吧 ~","{media_state} · {track} — {artist}",
            "→ {next_todo}","(つ•‿•)つ  抱抱，辛苦啦"};
        binbot_ambient_format(&s->data,formats[i],reply,sizeof(reply));
    }
    for(unsigned j = 0; j < s->count; ++j) {
        layer_t *l = &s->layers[j];
        if(!strcmp(l->format,"{shell_command}")) text_update(l,cmd);
        if(!strcmp(l->format,"{shell_reply}")) text_update(l,reply);
    }
    for(unsigned j=0;j<s->count;++j){layer_t *l=&s->layers[j];
        if(l->kind!=INDICATOR||strcmp(l->binding,"shellcaret"))continue;
        lv_obj_set_flag(l->object,LV_OBJ_FLAG_HIDDEN,reply[0]||elapsed%1050>=525);
        for(unsigned k=0;k<s->count;++k){layer_t *command=&s->layers[k];
            if(!strcmp(command->format,"{shell_command}"))lv_obj_set_x(l->object,lroundf(command->x+text_width(command)+12));
        }
    }
}
