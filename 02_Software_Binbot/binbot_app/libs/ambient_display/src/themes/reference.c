#include "../ambient_internal.h"
#include "libs/freetype/lv_freetype.h"
#include "misc/cache/instance/lv_image_cache.h"
#include <cjson/cJSON.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The compiler preserves paint order and exports CSS motion separately. Only
 * the selected theme owns these sprites, fonts and live labels. */
enum { MAX_LAYERS = 224, MAX_GROUPS = 128, MAX_FRAMES = 16, MAX_FONTS = 32 };
typedef struct { float x, y, rotation, sx, sy, opacity, at; } pose_t;
typedef struct { float a,b,c,d,x,y,opacity; } matrix_t;
typedef struct {
    int parent; float cx, cy, duration, delay, x, y, preview_at; bool reverse, ease, typing,playing; unsigned clock;
    pose_t frames[MAX_FRAMES]; unsigned count; matrix_t matrix;
} motion_t;
typedef struct { char family[48]; unsigned size, weight; lv_font_t *font, *fallback, *symbols; } font_t;
typedef struct { int x,y,blur; uint32_t color; unsigned opacity; } shadow_t;
typedef enum { ART, TEXT, HAND, SEGMENTS, FLAPS, PAPER, SLIDER, WEATHER, PROGRESS, ARC, BLOCKTIME, SAND, TRACE, INDICATOR } kind_t;
typedef struct reference reference_t;
typedef struct {
    reference_t *scene;
    lv_obj_t *object;
    kind_t kind; int parent;
    float x,y,w,h,cx,cy,angle,anchor,size,cell_width,font_size,from,span;
    unsigned opacity; uint32_t color; bool vertical, multiline, mirrored, pre;
    int flow; float flow_gap,flow_start,flow_right,flow_center;
    shadow_t shadows[4]; unsigned shadow_count;
    char align[12], binding[16], when[24], format[1024], value[1024], source[640], mirror[640];
    const lv_font_t *font;
    lv_point_precise_t points[4]; unsigned point_count;
} layer_t;
struct reference {
    binbot_ambient_display_t *display; lv_obj_t *root;
    const ambient_theme_t *theme;
    layer_t layers[MAX_LAYERS]; unsigned count, font_count, motion_count;
    motion_t motions[MAX_GROUPS]; font_t fonts[MAX_FONTS];
    uint32_t started; binbot_ambient_snapshot_t data;
    char diary[1024]; lv_obj_t *caret; unsigned typing_count;
    char art_id[48];
};

static const char *str(cJSON *o, const char *key)
{ cJSON *v = cJSON_GetObjectItemCaseSensitive(o,key); return cJSON_IsString(v) ? v->valuestring : ""; }
static float num(cJSON *o, const char *key, float fallback)
{ cJSON *v = cJSON_GetObjectItemCaseSensitive(o,key); return cJSON_IsNumber(v) && isfinite(v->valuedouble) ? (float)v->valuedouble : fallback; }
static bool flag(cJSON *o, const char *key) { return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o,key)); }
static uint32_t rgb(const char *text)
{
    unsigned r=255,g=255,b=255;
    if(text[0]=='#' && strlen(text)==7) return (uint32_t)strtoul(text+1,NULL,16);
    if(sscanf(text,"rgb(%u, %u, %u)",&r,&g,&b)!=3) (void)sscanf(text,"rgba(%u, %u, %u",&r,&g,&b);
    return ((r&255)<<16)|((g&255)<<8)|(b&255);
}
static const char *family_file(const char *family)
{
    static const char *const names[][2] = {
        {"Space Grotesk","space-grotesk"},{"Orbitron","orbitron"},{"Audiowide","audiowide"},
        {"Fraunces","fraunces"},{"ZCOOL KuaiLe","zcool-kuaile"},{"Caveat","caveat"},
        {"Ma Shan Zheng","ma-shan-zheng"},{"Noto Serif SC","noto-serif-sc"},{"Fredoka","fredoka"},
        {"Special Elite","special-elite"},{"Nixie One","nixie-one"},{"Silkscreen","silkscreen"},{"JetBrains Mono","jetbrains-mono"},{"ui-monospace","jetbrains-mono"}
    };
    for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);++i) if(!strcmp(names[i][0],family)) return names[i][1];
    return NULL;
}
static const lv_font_t *font(reference_t *s, const char *family, unsigned size, unsigned weight)
{
    if(size<8||size>300||strlen(family)>=48) return NULL;
    for(unsigned i=0;i<s->font_count;++i)
        if(s->fonts[i].size==size&&s->fonts[i].weight==weight&&!strcmp(s->fonts[i].family,family)) return s->fonts[i].font;
    if(s->font_count==MAX_FONTS) return NULL;
    const char *root=s->display->asset_root;if(root[0]=='A'&&root[1]==':')root+=2;
    char path[640];const char *file=family_file(family);
    if(file) {
        snprintf(path,sizeof(path),"%s/standby/fonts/%s-%u.ttf",root,file,weight);
        FILE *check=fopen(path,"rb");if(check)fclose(check);else snprintf(path,sizeof(path),"%s/standby/fonts/%s.ttf",root,file);
    }
    else if(!strcmp(family,"VT323")) snprintf(path,sizeof(path),"%s/standby/terminal/VT323-Regular.ttf",root);
    else snprintf(path,sizeof(path),"%s/fonts/%s-%s.ttf",root,!strcmp(family,"Noto Sans SC")?"NotoSansSC":"InterTight",
                  weight>=600?"SemiBold":weight>=500?"Medium":"Regular");
    lv_font_t *f=lv_freetype_font_create(path,LV_FREETYPE_FONT_RENDER_MODE_BITMAP,size,LV_FREETYPE_FONT_STYLE_NORMAL);
    if(!f) return NULL;
    snprintf(path,sizeof(path),"%s/fonts/NotoSansSC-%s.ttf",root,weight>=600?"SemiBold":weight>=500?"Medium":"Regular");
    lv_font_t *fallback=lv_freetype_font_create(path,LV_FREETYPE_FONT_RENDER_MODE_BITMAP,size,LV_FREETYPE_FONT_STYLE_NORMAL);
    snprintf(path,sizeof(path),"%s/standby/fonts/terminal-symbols.ttf",root);
    lv_font_t *symbols=lv_freetype_font_create(path,LV_FREETYPE_FONT_RENDER_MODE_BITMAP,size,LV_FREETYPE_FONT_STYLE_NORMAL);
    if(symbols)symbols->fallback=LV_FONT_DEFAULT;
    if(fallback)fallback->fallback=symbols?symbols:LV_FONT_DEFAULT;
    f->fallback=fallback?fallback:symbols?symbols:LV_FONT_DEFAULT;
    font_t *entry=&s->fonts[s->font_count++];
    *entry=(font_t){.size=size,.weight=weight,.font=f,.fallback=fallback,.symbols=symbols};
    snprintf(entry->family,sizeof(entry->family),"%s",family);
    return f;
}

static pose_t transform(const char *text)
{
    pose_t p={.sx=1,.sy=1,.opacity=1};
    const char *v;
    if((v=strstr(text,"translate("))) {
        p.x=strtof(v+10,NULL);const char *comma=strchr(v,',');if(comma)p.y=strtof(comma+1,NULL);
    }
    if((v=strstr(text,"translateX(")))p.x=strtof(v+11,NULL);
    if((v=strstr(text,"translateY(")))p.y=strtof(v+11,NULL);
    if((v=strstr(text,"rotate(")))p.rotation=strtof(v+7,NULL);
    if((v=strstr(text,"scale("))) {
        p.sx=strtof(v+6,NULL);p.sy=p.sx;const char *end=strchr(v,')'),*comma=strchr(v,',');if(comma&&(!end||comma<end))p.sy=strtof(comma+1,NULL);
    }
    if((v=strstr(text,"scaleX(")))p.sx=strtof(v+7,NULL);
    if((v=strstr(text,"scaleY(")))p.sy=strtof(v+7,NULL);
    return p;
}
static matrix_t multiply(matrix_t p,matrix_t q)
{
    return (matrix_t){p.a*q.a+p.c*q.b,p.b*q.a+p.d*q.b,p.a*q.c+p.c*q.d,p.b*q.c+p.d*q.d,
        p.a*q.x+p.c*q.y+p.x,p.b*q.x+p.d*q.y+p.y,p.opacity*q.opacity};
}
static float mix(float a,float b,float t) { return a+(b-a)*t; }
static matrix_t animate(motion_t *m, uint32_t elapsed)
{
    float t=((float)elapsed-m->delay)/m->duration;
    if(t<0)t=0;else t-=floorf(t);
    if(m->reverse)t=1-t;
    unsigned i=0;while(i+1<m->count&&m->frames[i+1].at<t)++i;
    pose_t a=m->frames[i],b=m->frames[i+1<m->count?i+1:i];
    float u=b.at>a.at?(t-a.at)/(b.at-a.at):0;if(u<0)u=0;if(u>1)u=1;
    if(m->ease)u=(1-cosf(u*3.14159265359f))/2;
    float angle=mix(a.rotation,b.rotation,u)*.01745329252f;
    float sx=mix(a.sx,b.sx,u),sy=mix(a.sy,b.sy,u),c=cosf(angle),s=sinf(angle);
    matrix_t r={c*sx,s*sx,-s*sy,c*sy,0,0,mix(a.opacity,b.opacity,u)};
    r.x=m->cx-r.a*m->cx-r.c*m->cy+mix(a.x,b.x,u);
    r.y=m->cy-r.b*m->cx-r.d*m->cy+mix(a.y,b.y,u);
    return r;
}

static void led_line(lv_layer_t *canvas,float x1,float y1,float x2,float y2,int width,uint32_t color,unsigned opa)
{
    lv_draw_line_dsc_t d;lv_draw_line_dsc_init(&d);d.p1=(lv_point_precise_t){x1,y1};d.p2=(lv_point_precise_t){x2,y2};
    d.color=lv_color_hex(color);d.width=width;d.opa=opa;lv_draw_line(canvas,&d);
}
static const char *segments(char c)
{
    const char *digits[]={"abcdef","bc","abged","abgcd","fgbc","afgcd","afgedc","abc","abcdefg","abcdfg"};
    return c>='0'&&c<='9'?digits[c-'0']:c=='-'?"g":"";
}
static void readout(lv_event_t *event)
{
    layer_t *l=lv_event_get_user_data(event);lv_layer_t *canvas=lv_event_get_layer(event);
    lv_area_t area;lv_obj_get_coords(l->object,&area);
    float x=area.x1,y=area.y1,h=l->size;
    static const float line[7][4]={{14,8,46,8},{52,16,52,44},{52,56,52,84},{14,92,46,92},{8,56,8,84},{8,16,8,44},{14,50,46,50}};
    for(const char *p=l->value;*p;++p){
        if(*p==':'||*p==';') {
            for(unsigned j=0;j<2;++j)led_line(canvas,x+h*.1f,y+h*(j?.7f:.3f),x+h*.1f,y+h*(j?.7f:.3f),fmaxf(2,h*.1f),l->color,*p==';'?36:255);
            x+=h*.28f;continue;
        }
        const char *lit=segments(*p);
        for(unsigned i=0;i<7;++i) {
            unsigned opacity=strchr(lit,'a'+i)?255:18;
            led_line(canvas,x+(line[i][0]+6-line[i][1]*.105f)*h/100,y+line[i][1]*h/100,
                x+(line[i][2]+6-line[i][3]*.105f)*h/100,y+line[i][3]*h/100,fmaxf(1,h*.08f),l->color,opacity);
        }
        x+=h*.68f;
    }
}

static void text_update(layer_t *l,const char *text);
#include "reference_text.h"
#include "reference_live.h"

static bool add(reference_t *s,cJSON *json)
{
    if(s->count==MAX_LAYERS)return false;
    layer_t *l=&s->layers[s->count++];l->scene=s;l->parent=(int)num(json,"parent",-1);
    if(l->parent < -1 || l->parent >= (int)s->motion_count)return false;
    const char *type=str(json,"type");
    l->x=num(json,"x",0);l->y=num(json,"y",0);l->w=num(json,"w",1);l->h=num(json,"h",1);
    if(fabsf(l->x)>4000||fabsf(l->y)>4000||l->w<0||l->w>3000||l->h<0||l->h>6000)return false;
    l->opacity=(unsigned)num(json,"opacity",255);if(l->opacity>255)return false;
    l->color=rgb(str(json,"color"));l->size=num(json,"size",20);
    if(!strcmp(type,"art")) {
        l->kind=ART;const char *src=str(json,"src");if(!ambient_safe_path(src))return false;
        char relative[256];snprintf(relative,sizeof(relative),"standby/%s/%s",s->art_id,src);
        ambient_asset_path(s->display,relative,l->source,sizeof(l->source));
        lv_image_header_t header;if(lv_image_decoder_get_info(l->source,&header)!=LV_RESULT_OK||!header.w||!header.h||header.w>720||header.h>720)return false;
        l->object=lv_image_create(s->root);lv_image_set_src(l->object,l->source);lv_image_set_pivot(l->object,0,0);
        if(l->parent>=0){snprintf(l->mirror,sizeof(l->mirror),"%.*s-flip.png",(int)strlen(l->source)-4,l->source);}
    }
    else if(!strcmp(type,"text")) {
        l->kind=TEXT;l->font=font(s,str(json,"font"),(unsigned)l->size,(unsigned)num(json,"weight",400));if(!l->font)return false;
        l->object=lv_label_create(s->root);lv_label_set_text(l->object,"");lv_obj_set_style_text_font(l->object,l->font,0);
        l->vertical=flag(json,"vertical");l->multiline=flag(json,"multiline");l->pre=flag(json,"pre");l->angle=num(json,"angle",0);
        l->flow=(int)num(json,"flow",-1);l->flow_gap=num(json,"flowGap",0);l->flow_start=num(json,"flowStart",l->x);
        l->flow_right=num(json,"flowRight",720);l->flow_center=num(json,"flowCenter",-1);
        if(l->flow < -1||l->flow>10000||l->flow_gap<0||l->flow_gap>720||l->flow_right<0||l->flow_right>720)return false;
        l->anchor=num(json,"anchor",l->x);snprintf(l->align,sizeof(l->align),"%s",str(json,"align"));
        if(l->flow>=0)snprintf(l->align,sizeof(l->align),"start");
        if(strstr(str(json,"text"),"{time}") && l->size>60 && strlen(str(json,"text"))<16) l->w=fminf(720-l->x,l->size*3.6f);
        lv_obj_set_style_text_color(l->object,lv_color_hex(l->color),0);
        lv_obj_set_style_text_letter_space(l->object,(int)lroundf(num(json,"spacing",0)),0);
        lv_obj_set_style_text_line_space(l->object,(int)lroundf((l->vertical?l->size*1.05f:num(json,"lineHeight",l->font->line_height))-l->font->line_height),0);
        if(l->vertical) {lv_obj_set_width(l->object,(int)lroundf(l->size*1.1f));lv_label_set_long_mode(l->object,LV_LABEL_LONG_WRAP);}
        else {
            lv_obj_set_width(l->object,(int)ceilf(l->w));
            lv_label_set_long_mode(l->object,l->multiline?LV_LABEL_LONG_WRAP:LV_LABEL_LONG_DOT);
            if(!l->multiline)lv_obj_set_height(l->object,l->font->line_height+2);
        }
        if(!strcmp(l->align,"center")) {lv_obj_set_style_text_align(l->object,LV_TEXT_ALIGN_CENTER,0);l->x=l->anchor-l->w/2;}
        else if(!strcmp(l->align,"right")){lv_obj_set_style_text_align(l->object,LV_TEXT_ALIGN_RIGHT,0);l->x=l->anchor-l->w;}
        if(l->parent<0&&fabsf(l->angle)>.1f)lv_obj_set_style_transform_rotation(l->object,lroundf(l->angle*10),0);
        cJSON *shadows=cJSON_GetObjectItemCaseSensitive(json,"shadows"),*shadow;
        if(shadows&&(!cJSON_IsArray(shadows)||cJSON_GetArraySize(shadows)>4))return false;
        cJSON_ArrayForEach(shadow,shadows){
            shadow_t *v=&l->shadows[l->shadow_count++];
            v->x=lroundf(num(shadow,"x",0));v->y=lroundf(num(shadow,"y",0));v->blur=lroundf(num(shadow,"blur",0));
            v->color=rgb(str(shadow,"color"));v->opacity=(unsigned)num(shadow,"opacity",255);
            if(abs(v->x)>128||abs(v->y)>128||v->blur<0||v->blur>128||v->opacity>255)return false;
        }
        if(l->shadow_count){lv_obj_add_event_cb(l->object,text_shadow,LV_EVENT_ALL,l);lv_obj_refresh_ext_draw_size(l->object);}
    }
    else if(!strcmp(type,"hand")) {
        l->kind=HAND;l->cx=num(json,"cx",0);l->cy=num(json,"cy",0);
        snprintf(l->binding,sizeof(l->binding),"%s",str(json,"bind"));
        float stroke=num(json,"strokeWidth",0);if(l->w<1){l->w=fmaxf(1,stroke);l->x-=l->w/2;l->color=rgb(str(json,"stroke"));}
        l->object=ambient_box(s->root,l->x,l->y,fmaxf(1,l->w),fmaxf(1,l->h),l->color,(int)num(json,"rx",0));
        lv_obj_set_style_transform_pivot_x(l->object,lroundf(l->cx-l->x),0);lv_obj_set_style_transform_pivot_y(l->object,lroundf(l->cy-l->y),0);
    }
    else if(!strcmp(type,"slider")) {
        l->kind=SLIDER;l->from=num(json,"from",l->x);l->span=num(json,"span",524);
        l->object=ambient_box(s->root,l->x,l->y,l->w,l->h,l->color,2);
    }
    else if(!strcmp(type,"weather")) {
        l->kind=WEATHER;l->object=lv_image_create(s->root);
        lv_image_set_pivot(l->object,0,0);
        lv_image_set_scale_x(l->object,(uint32_t)lroundf(l->w*256/128));
        lv_image_set_scale_y(l->object,(uint32_t)lroundf(l->h*256/128));
        lv_obj_set_style_image_recolor(l->object,lv_color_hex(l->color),0);
        lv_obj_set_style_image_recolor_opa(l->object,255,0);
    }
    else if(!strcmp(type,"arc")) {
        l->kind=ARC;l->object=lv_arc_create(s->root);lv_obj_remove_style_all(l->object);
        lv_obj_set_size(l->object,lroundf(l->w),lroundf(l->h));
        lv_arc_set_bg_angles(l->object,0,360);lv_arc_set_rotation(l->object,270);
        lv_arc_set_range(l->object,0,60);
        lv_obj_set_style_arc_width(l->object,lroundf(l->size),LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(l->object,lv_color_hex(l->color),LV_PART_INDICATOR);
        lv_obj_set_style_arc_opa(l->object,255,LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(l->object,true,LV_PART_INDICATOR);
    }
    else if(!strcmp(type,"progress")) {
        l->kind=PROGRESS;snprintf(l->binding,sizeof(l->binding),"%s",str(json,"bind"));
        l->object=ambient_box(s->root,l->x,l->y,l->w,l->h,l->color,l->h/2);
    }
    else if(!strcmp(type,"paper")) {
        l->kind=PAPER;l->object=ambient_box(s->root,l->x,l->y,l->w,l->h,0xf7f1e4,0);
        for(int y=71;y<1100;y+=46){lv_obj_t *line=ambient_box(l->object,0,y,600,1,0x785a3c,0);lv_obj_set_style_bg_opa(line,18,0);}
    }
    else if(!strcmp(type,"segments")) {
        l->kind=SEGMENTS;l->object=ambient_box(s->root,l->x,l->y,ceilf(l->w),ceilf(l->h),0,0);
        lv_obj_set_style_bg_opa(l->object,0,0);lv_obj_add_event_cb(l->object,readout,LV_EVENT_DRAW_MAIN,l);
    }
    else if(!strcmp(type,"flaps")) {
        l->kind=FLAPS;l->cell_width=num(json,"cellWidth",34);l->font_size=num(json,"fontSize",24);
        unsigned n=(unsigned)num(json,"cells",(l->w+3)/(l->cell_width+3));
        if(l->cell_width<10||l->cell_width>100||n<1||n>16)return false;
        l->cell_width=fminf(l->cell_width,(l->w-(n-1)*3)/n);
        if(l->cell_width<8)return false;
        l->font=font(s,"Space Grotesk",(unsigned)l->font_size,600);if(!l->font)return false;
        l->object=ambient_box(s->root,l->x,l->y,ceilf(l->w),ceilf(l->h),0,0);lv_obj_set_style_bg_opa(l->object,0,0);
        for(unsigned i=0;i<n;++i){lv_obj_t *cell=ambient_box(l->object,i*(l->cell_width+3),0,l->cell_width,l->h,0x242424,5);
            lv_obj_t *name=lv_label_create(cell);lv_obj_set_style_text_font(name,l->font,0);lv_obj_set_style_text_color(name,lv_color_hex(l->color),0);lv_label_set_text(name,"");lv_obj_center(name);
            ambient_box(cell,0,l->h/2-1,l->cell_width,2,0x080808,0);
        }
    }
    else if(!strcmp(type,"blocktime")||!strcmp(type,"sand")||!strcmp(type,"trace")) {
        l->kind=!strcmp(type,"sand")?SAND:!strcmp(type,"trace")?TRACE:BLOCKTIME;
        if(l->kind==TRACE){
            cJSON *points=cJSON_GetObjectItemCaseSensitive(json,"points"),*point;
            if(!cJSON_IsArray(points)||cJSON_GetArraySize(points)<2||cJSON_GetArraySize(points)>4)return false;
            cJSON_ArrayForEach(point,points){
                cJSON *x=cJSON_GetArrayItem(point,0),*y=cJSON_GetArrayItem(point,1);
                if(!cJSON_IsNumber(x)||!cJSON_IsNumber(y)||!isfinite(x->valuedouble)||!isfinite(y->valuedouble)||fabs(x->valuedouble)>1000||fabs(y->valuedouble)>1000)return false;
                l->points[l->point_count++]=(lv_point_precise_t){x->valuedouble,y->valuedouble};
            }
            l->from=num(json,"duration",2800);l->span=num(json,"delay",0);if(l->from<100||l->from>120000)return false;
        }
        l->object=ambient_box(s->root,l->x,l->y,l->w,l->h,0,0);
        lv_obj_set_style_bg_opa(l->object,0,0);lv_obj_add_event_cb(l->object,live_draw,LV_EVENT_DRAW_MAIN,l);
    }
    else if(!strcmp(type,"indicator")) {
        l->kind=INDICATOR;snprintf(l->binding,sizeof(l->binding),"%s",str(json,"bind"));
        l->object=ambient_box(s->root,l->x,l->y,l->w,l->h,0,LV_RADIUS_CIRCLE);
    }
    else return false;
    if(!l->object)return false;
    const char *text=str(json,"text");if(strlen(text)>=sizeof(l->format))return false;snprintf(l->format,sizeof(l->format),"%s",text);
    snprintf(l->when,sizeof(l->when),"%s",str(json,"when"));
    lv_obj_set_pos(l->object,lroundf(l->x),lroundf(l->y));lv_obj_set_style_opa(l->object,l->opacity,0);
    lv_obj_remove_flag(l->object,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_EVENT_BUBBLE|LV_OBJ_FLAG_GESTURE_BUBBLE);
    return true;
}
static void destroy(void *context)
{
    reference_t *s=context;
    for(unsigned i=0;i<s->count;++i) if(s->layers[i].source[0]) {lv_image_cache_drop(s->layers[i].source);if(s->layers[i].mirror[0])lv_image_cache_drop(s->layers[i].mirror);}
    for(unsigned i=0;i<s->font_count;++i){
        lv_freetype_font_delete(s->fonts[i].font);
        if(s->fonts[i].fallback)lv_freetype_font_delete(s->fonts[i].fallback);
        if(s->fonts[i].symbols)lv_freetype_font_delete(s->fonts[i].symbols);
    }
    free(s);
}
static void *create(binbot_ambient_display_t *display,lv_obj_t *root,const ambient_theme_t *theme)
{
    const char *assets=display->asset_root;if(assets[0]=='A'&&assets[1]==':')assets+=2;
    bool night=(!strcmp(theme->info.id,"paper-cut")||!strcmp(theme->info.id,"sundial")||!strcmp(theme->info.id,"hourglass"))&&
        (display->snapshot.hour<6||display->snapshot.hour>=(!strcmp(theme->info.id,"paper-cut")?18:19));
    char art_id[48];snprintf(art_id,sizeof(art_id),"%s%s",theme->info.id,night?"-night":"");
    char path[640];snprintf(path,sizeof(path),"%s/standby/%s/reference.json",assets,art_id);
    FILE *f=fopen(path,"rb");if(!f)return NULL;
    char *text=malloc(262145);if(!text){fclose(f);return NULL;}
    size_t n=fread(text,1,262145,f);bool valid=n<=262144&&!ferror(f);fclose(f);
    cJSON *j=NULL;if(valid){text[n]=0;j=cJSON_ParseWithLength(text,n+1);}free(text);
    cJSON *layers=cJSON_GetObjectItemCaseSensitive(j,"layers"),*groups=cJSON_GetObjectItemCaseSensitive(j,"groups");
    valid=num(j,"schema",0)==1&&cJSON_IsArray(layers)&&cJSON_IsArray(groups)&&cJSON_GetArraySize(layers)>0&&cJSON_GetArraySize(layers)<=MAX_LAYERS&&cJSON_GetArraySize(groups)<=MAX_GROUPS;
    reference_t *s=valid?calloc(1,sizeof(*s)):NULL;if(!s){cJSON_Delete(j);return NULL;}
    s->display=display;s->root=root;s->theme=theme;s->started=lv_tick_get();
    snprintf(s->art_id,sizeof(s->art_id),"%s",art_id);
    cJSON *item;
    cJSON_ArrayForEach(item,groups){
        motion_t *m=&s->motions[s->motion_count++];m->parent=(int)num(item,"parent",-1);
        m->cx=num(item,"cx",0);m->cy=num(item,"cy",0);m->duration=num(item,"duration",1000);m->delay=num(item,"delay",0);
        m->reverse=flag(item,"reverse");m->ease=strcmp(str(item,"easing"),"linear")!=0;
        m->typing=!strcmp(str(item,"type"),"typing");m->x=num(item,"x",0);m->y=num(item,"y",0);m->playing=flag(item,"playing");
        m->clock=!strcmp(str(item,"type"),"minute")?1:!strcmp(str(item,"type"),"sundial")?2:0;
        m->preview_at=num(item,"previewAt",-1);
        cJSON *frames=cJSON_GetObjectItemCaseSensitive(item,"frames"),*frame;
        if(m->parent < -1||m->parent >= (int)s->motion_count-1||m->duration<50||m->duration>120000||m->preview_at < -1||m->preview_at>1||!cJSON_IsArray(frames)||cJSON_GetArraySize(frames)<1||cJSON_GetArraySize(frames)>MAX_FRAMES){valid=false;break;}
        cJSON_ArrayForEach(frame,frames){pose_t *p=&m->frames[m->count++];*p=transform(str(frame,"transform"));p->at=num(frame,"at",0);p->opacity=num(frame,"opacity",1);if(p->at<0||p->at>1||p->opacity<0||p->opacity>1)valid=false;}
    }
    cJSON_ArrayForEach(item,layers)if(valid)valid=add(s,item);
    cJSON_Delete(j);
    if(!valid){lv_obj_clean(root);destroy(s);return NULL;}
    if(!strcmp(theme->info.id,"typewriter"))s->caret=ambient_box(root,330,438,12,26,0xe8643c,0);
    return s;
}
static void text_update(layer_t *l,const char *text)
{
    if(!strcmp(l->value,text))return;
    snprintf(l->value,sizeof(l->value),"%s",text);
    if(l->kind==SEGMENTS){lv_obj_invalidate(l->object);return;}
    if(l->kind==FLAPS){
        const char *p=text;unsigned n=lv_obj_get_child_count(l->object);
        for(unsigned i=0;i<n;++i){char ch[8]=" ";if(*p){unsigned bytes=1;while(p[bytes]&&((unsigned char)p[bytes]&0xc0)==0x80&&bytes<4)++bytes;memcpy(ch,p,bytes);ch[bytes]=0;p+=bytes;}
            lv_obj_t *label=lv_obj_get_child(lv_obj_get_child(l->object,i),0);ambient_text_set(label,ch);lv_obj_center(label);
        }return;
    }
    if(l->kind==TEXT&&l->flow<0&&!l->vertical && (l->pre||(!l->multiline&&(strstr(l->format,"{time}")||strstr(l->format,"{eventtime")||strstr(l->format,"{world")||strstr(l->format,"{digit"))))){
        lv_point_t size;lv_text_get_size(&size,text,l->font,lv_obj_get_style_text_letter_space(l->object,0),0,LV_COORD_MAX,LV_TEXT_FLAG_NONE);
        int width=(int)fmaxf(l->w,size.x+4);lv_obj_set_width(l->object,width);
        if(!strcmp(l->align,"center"))l->x=l->anchor-width/2.f;else if(!strcmp(l->align,"right"))l->x=l->anchor-width;
        if(l->parent<0)lv_obj_set_x(l->object,lroundf(l->x));
    }
    if(l->vertical){char vertical[2048];unsigned at=0;const char *p=text;
        while(*p&&at+8<sizeof(vertical)){unsigned n=1;while(p[n]&&((unsigned char)p[n]&0xc0)==0x80&&n<4)++n;memcpy(vertical+at,p,n);at+=n;p+=n;if(*p)vertical[at++]='\n';}vertical[at]=0;ambient_text_set(l->object,vertical);
    }
    else ambient_text_set(l->object,text);
}
static void update(void *context,const binbot_ambient_snapshot_t *data)
{
    reference_t *s=context;s->data=*data;
    for(unsigned i=0;i<s->count;++i){layer_t *l=&s->layers[i];
        int row=-1;bool visible=true;
        if(sscanf(l->when,"event%d",&row)==1) visible=row>=0&&row<(int)data->event_count;
        else if(sscanf(l->when,"todo%d",&row)==1) visible=row>=0&&row<5&&data->todos[row].text[0];
        else if(sscanf(l->when,"fish%d",&row)==1) {
            unsigned open=data->todo_total>data->todo_done?data->todo_total-data->todo_done:0;
            visible=row>=0&&row<(int)(open?open:1);
        }
        if(l->format[0]=='{' && (sscanf(l->format,"{eventtime%d}",&row)==1 || sscanf(l->format,"{event%d}",&row)==1))
            visible=visible&&row>=0&&row<(int)data->event_count;
        lv_obj_set_flag(l->object,LV_OBJ_FLAG_HIDDEN,!visible);
        if(l->kind==TEXT||l->kind==SEGMENTS||l->kind==FLAPS){
            if(!strcmp(l->format,"{diary}")){binbot_ambient_format(data,l->format,s->diary,sizeof(s->diary));continue;}
            char text[1024];binbot_ambient_format(data,l->format,text,sizeof(text));text_update(l,text);
        }
        else if(l->kind==HAND){float angle=!strcmp(l->binding,"hour")?(data->hour%12+data->minute/60.f+data->second/3600.f)*30:
            !strcmp(l->binding,"minute")?(data->minute+data->second/60.f)*6:data->second*6;
            lv_obj_set_style_transform_rotation(l->object,lroundf(angle*10),0);
        }
        else if(l->kind==SLIDER)lv_obj_set_x(l->object,lroundf(l->from+(data->hour+data->minute/60.f+data->second/3600.f)*l->span/24));
        else if(l->kind==ARC)lv_arc_set_value(l->object,data->second);
        else if(l->kind==INDICATOR)indicator_update(l,data);
        else if(l->kind==SAND||l->kind==BLOCKTIME)lv_obj_invalidate(l->object);
        else if(l->kind==PROGRESS){
            float ratio=!strcmp(l->binding,"focus")?data->focus_minutes/120.f:data->todo_total?data->todo_done/(float)data->todo_total:0;
            lv_obj_set_width(l->object,lroundf(l->w*fminf(1,fmaxf(0,ratio))));
        }
        else if(l->kind==WEATHER){
            const char *id=data->weather_id,*glyph=strstr(id,"rain")||strstr(id,"shower")||strstr(id,"sleet")?"rain":
                strstr(id,"storm")||strstr(id,"hail")?"storm":strstr(id,"snow")?"drop":strstr(id,"night")?"moon":"sun";
            char relative[128],path[640];snprintf(relative,sizeof(relative),"standby/icons/%s.png",glyph);ambient_asset_path(s->display,relative,path,sizeof(path));
            if(strcmp(l->source,path)){if(l->source[0])lv_image_cache_drop(l->source);snprintf(l->source,sizeof(l->source),"%s",path);lv_image_set_src(l->object,l->source);}
            lv_obj_set_flag(l->object,LV_OBJ_FLAG_HIDDEN,!data->weather_valid);
        }
    }
    flows_update(s);
    for(unsigned i=1;i<s->count;++i){layer_t *l=&s->layers[i];if(l->kind!=TEXT||strcmp(l->format," {event0}"))continue;
        if(l->flow>=0)continue;
        for(unsigned j=i;j>0;--j){layer_t *prev=&s->layers[j-1];if(prev->kind!=TEXT||strcmp(prev->format,"{eventtime0}"))continue;
            lv_point_t measured;lv_text_get_size(&measured,prev->value,prev->font,lv_obj_get_style_text_letter_space(prev->object,0),0,LV_COORD_MAX,LV_TEXT_FLAG_NONE);
            l->x=prev->x+fmaxf(prev->w,measured.x+4)+8;lv_obj_set_x(l->object,lroundf(l->x));lv_obj_set_width(l->object,(int)fmaxf(20,672-l->x));break;
        }
    }
}
static void tick(void *context,uint32_t now)
{
    reference_t *s=context;uint32_t elapsed=now-s->started;
    if(!strcmp(s->theme->info.id,"ban-shell"))shell_tick(s,elapsed);
    for(unsigned i=0;i<s->count;++i)if(s->layers[i].kind==TRACE||s->layers[i].kind==SAND)lv_obj_invalidate(s->layers[i].object);
    char typed[1024]="";float carriage_x=0,carriage_y=0;
    if(s->caret){
        unsigned chars=0;for(const unsigned char *p=(const unsigned char *)s->diary;*p;++p)if((*p&0xc0)!=0x80)++chars;
        unsigned wanted=s->display->config.preview?chars:(elapsed/110)%(chars+50),used=0,lines=0;float column=0;
        const char *p=s->diary;
        for(unsigned i=0;*p&&i<wanted&&used+5<sizeof(typed);++i){unsigned n=1;while(p[n]&&((unsigned char)p[n]&0xc0)==0x80&&n<4)++n;
            if(*p=='\n'){++lines;column=0;}else column+=n>=3?28:15.5f;memcpy(typed+used,p,n);used+=n;p+=n;
        }
        typed[used]=0;carriage_x=330-44-column;carriage_y=452-64-lines*46;
        lv_obj_set_style_opa(s->caret,elapsed%800<400?200:0,0);
        for(unsigned i=0;i<s->count;++i)if(!strcmp(s->layers[i].format,"{diary}"))text_update(&s->layers[i],typed);
    }
    for(unsigned i=0;i<s->motion_count;++i){motion_t *m=&s->motions[i];matrix_t current=animate(m,elapsed);
        if(s->display->config.preview&&m->preview_at>=0)current=animate(m,lroundf(m->delay+m->duration*m->preview_at));
        if(m->playing&&!s->data.playing)current=animate(m,0);
        if(m->clock){
            float angle=m->clock==1?(s->data.minute+s->data.second/60.f)*6:(s->data.hour+s->data.minute/60.f-12)*15;
            float sy=m->clock==2?(150+fabsf(angle)/90*70)/170.80556f:1;
            float c=cosf(angle*.01745329252f),v=sinf(angle*.01745329252f);
            current=(matrix_t){c,v,-v*sy,c*sy,m->cx-c*m->cx+v*sy*m->cy,m->cy-v*m->cx-c*sy*m->cy,1};
        }
        if(m->typing)current=(matrix_t){1,0,0,1,carriage_x-m->x,carriage_y-m->y,1};
        m->matrix=m->parent>=0?multiply(s->motions[m->parent].matrix,current):current;
    }
    for(unsigned i=0;i<s->count;++i){layer_t *l=&s->layers[i];if(l->parent<0)continue;
        matrix_t m=s->motions[l->parent].matrix;float x=m.a*l->x+m.c*l->y+m.x,y=m.b*l->x+m.d*l->y+m.y;
        float sx=hypotf(m.a,m.b),sy=hypotf(m.c,m.d),angle=atan2f(m.b,m.a)*57.295779513f;
        if(l->kind==ART){bool mirror=m.a*m.d-m.b*m.c<0;
            if(mirror){x+=m.a*l->w;y+=m.b*l->w;angle=atan2f(-m.b,-m.a)*57.295779513f;}
            if(mirror!=l->mirrored){l->mirrored=mirror;lv_image_set_src(l->object,mirror?l->mirror:l->source);}
            lv_image_set_scale_x(l->object,(uint32_t)fmaxf(1,lroundf(sx*256)));lv_image_set_scale_y(l->object,(uint32_t)fmaxf(1,lroundf(sy*256)));
            int rotation=(int)lroundf(angle*10)%3600;if(rotation<0)rotation+=3600;lv_image_set_rotation(l->object,rotation);
        }
        else {lv_obj_set_style_transform_rotation(l->object,lroundf((angle+l->angle)*10),0);lv_obj_set_style_transform_scale_x(l->object,lroundf(sx*256),0);lv_obj_set_style_transform_scale_y(l->object,lroundf(sy*256),0);}
        lv_obj_set_pos(l->object,lroundf(x),lroundf(y));lv_obj_set_style_opa(l->object,(lv_opa_t)lroundf(l->opacity*m.opacity),0);
    }
}
const ambient_renderer_t ambient_reference_renderer={.id="reference",.create=create,.update=update,.tick=tick,.destroy=destroy};
