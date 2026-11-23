#include "../ambient_internal.h"
#include "libs/freetype/lv_freetype.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Native ports of the six Canvas faces. Drawing stays at the original canvas
 * resolution; the low-resolution pixel scenes use nearest-neighbour scaling. */
enum { ROOM, SKY, DOTS, BUDDY, LAVA, PARTICLES, MAX_PARTICLES=2200 };
typedef struct { float x,y,vx,vy,tx,ty,a; bool dust; } particle_t;
typedef struct {
    binbot_ambient_display_t *display; lv_obj_t *root,*picture;
    lv_image_dsc_t image; uint16_t *pixels;
    int kind,n,clip_x,clip_y,clip_w,clip_h;
    uint32_t started,last_frame; binbot_ambient_snapshot_t data;
    FT_Library ft; FT_Face chinese,digits;
    particle_t *particles; unsigned particle_count; int minute;
    lv_obj_t *time,*seconds,*date,*weather,*next,*range;
    lv_font_t *clock_font;
} pixel_t;
static uint16_t color(unsigned c) { return ((c>>8)&0xf800)|((c>>5)&0x7e0)|((c>>3)&0x1f); }
static uint16_t blend(uint16_t old,unsigned c,unsigned alpha)
{
    if(alpha>=255)return color(c);
    unsigned r=((old>>11)*255/31*(255-alpha)+(c>>16&255)*alpha)/255;
    unsigned g=((old>>5&63)*255/63*(255-alpha)+(c>>8&255)*alpha)/255;
    unsigned b=((old&31)*255/31*(255-alpha)+(c&255)*alpha)/255;
    return color((r<<16)|(g<<8)|b);
}
static void rect(pixel_t *s,float fx,float fy,float fw,float fh,unsigned c,unsigned a)
{
    int x=(int)lroundf(fx),y=(int)lroundf(fy),w=(int)lroundf(fw),h=(int)lroundf(fh);
    int x2=x+w,y2=y+h;
    if(x<s->clip_x)x=s->clip_x;
    if(y<s->clip_y)y=s->clip_y;
    if(x2>s->clip_x+s->clip_w)x2=s->clip_x+s->clip_w;
    if(y2>s->clip_y+s->clip_h)y2=s->clip_y+s->clip_h;
    uint16_t rgb=color(c);
    for(int j=y;j<y2;++j)for(int i=x;i<x2;++i){uint16_t *p=&s->pixels[j*s->n+i];*p=a>=255?rgb:blend(*p,c,a);}
}
#define R(x,y,w,h,c) rect(s,x,y,w,h,c,255)
#define A(x,y,w,h,c,a) rect(s,x,y,w,h,c,a)
static void clip(pixel_t *s,int x,int y,int w,int h) {s->clip_x=x;s->clip_y=y;s->clip_w=w;s->clip_h=h;}
static void disc(pixel_t *s,int cx,int cy,int r,unsigned c,unsigned opacity)
{
    for(int y=-r;y<=r;++y)for(int x=-r;x<=r;++x)if(x*x+y*y<=r*r+r*.6f)rect(s,cx+x,cy+y,1,1,c,opacity);
}
static const char *bits(uint32_t ch)
{
    static const char *nums[]={"111101101101111","010110010010111","111001111100111","111001111001111","101101111001001","111100111001111","111100111101111","111001010010010","111101111101111","111101111001111"};
    static const char *letters[]={"010101111101101","110101110101110","011100100100011","110101101101110","111100110100111","111100110100100","011100101101011","101101111101101","111010010010111","001001001101010","101101110101101","100100100100111","101111111101101","110101101101101","010101101101010","110101110100100","010101101110011","110101110101101","011100010001110","111010010010010","101101101101111","101101101101010","101101111111101","101101010101101","101101010010010","111001010100111"};
    if(ch>='0'&&ch<='9')return nums[ch-'0'];
    if(ch>='a'&&ch<='z')ch-=32;
    if(ch>='A'&&ch<='Z')return letters[ch-'A'];
    if(ch==':')return "000010000010000";
    if(ch=='-')return "000000111000000";
    if(ch==0xb0)return "010101010000000";
    if(ch=='/')return "001001010100100";
    if(ch=='.')return "000000000000010";
    if(ch=='%')return "101001010100101";
    return "000000000000000";
}
static uint32_t utf8(const char **text)
{
    const unsigned char *p=(const unsigned char *)*text;uint32_t c=*p++;unsigned n=0;
    if(c>=0xf0){c&=7;n=3;}else if(c>=0xe0){c&=15;n=2;}else if(c>=0xc0){c&=31;n=1;}
    while(n--&&*p)c=(c<<6)|(*p++&63);
    *text=(const char *)p;return c;
}
static int width(const char *text,int k) {int n=0;while(*text){utf8(&text);++n;}return n*4*k-k;}
static void ptext(pixel_t *s,const char *text,int x,int y,int k,unsigned c)
{
    while(*text){const char *g=bits(utf8(&text));for(unsigned i=0;i<15;++i)if(g[i]=='1')R(x+(i%3)*k,y+(i/3)*k,k,k,c);x+=4*k;}
}
static int ztext(pixel_t *s,const char *text,int x,int y,unsigned c)
{
    int start=x;if(!s->chinese)return 0;
    while(*text){uint32_t ch=utf8(&text);if(FT_Load_Char(s->chinese,ch,FT_LOAD_RENDER))continue;
        FT_GlyphSlot g=s->chinese->glyph;FT_Bitmap *b=&g->bitmap;
        int gx=x+g->bitmap_left,gy=y+12-g->bitmap_top;
        for(unsigned j=0;j<b->rows;++j)for(unsigned i=0;i<b->width;++i)if(b->buffer[j*b->pitch+i]>110)R(gx+(int)i,gy+(int)j,1,1,c);
        x+=(int)(g->advance.x/64);
    }return x-start;
}
static void formatted(pixel_t *s,const char *fmt,char *out,size_t cap) {binbot_ambient_format(&s->data,fmt,out,cap);}
static bool rain(pixel_t *s) {return strstr(s->data.weather_id,"rain")||strstr(s->data.weather_id,"shower")||strstr(s->data.weather_id,"storm")||strstr(s->data.weather_id,"sleet");}
static bool snow(pixel_t *s) {return strstr(s->data.weather_id,"snow")!=NULL;}

static void room(pixel_t *s,unsigned f)
{
    const binbot_ambient_snapshot_t *d=&s->data;bool night=d->hour<6||d->hour>=19,sleep=d->hour>=23||d->hour<7;
    R(0,0,180,124,night?0x2e2640:0xefdcc0);
    for(int y=6;y<124;y+=12)for(int x=(y/6)%4?6:0;x<180;x+=12)R(x,y,2,2,night?0x372e4d:0xe5cfae);
    unsigned sky=night?0x1a2252:rain(s)?0x8496ad:0x8fd3ff;
    R(12,16,64,60,0x6b4b3a);R(15,19,58,54,sky);clip(s,15,19,58,54);
    if(night){for(int i=0;i<14;++i)if(((f>>2)+i)%5)R(16+(i*37)%56,21+(i*23)%44,1,1,0xfff6d8);disc(s,60,32,6,0xfff2c4,255);disc(s,63,30,5,sky,255);}
    else if(!rain(s))disc(s,30,34,7,0xffd75e,255);
    for(int k=0;k<2;++k){int x=((int)(f*.5f)+(k?46:0)+76)%100-10,y=k?38:52;R(x,y,18,4,night?0x39407a:0xffffff);R(x+4,y-3,9,3,night?0x39407a:0xffffff);}
    if(rain(s))for(int i=0;i<20;++i)R(16+(i*13)%56,19+(f*3+i*17)%54,1,3,0xd8e8ff);
    if(snow(s))for(int i=0;i<18;++i)R(16+(i*13+(f>>2)%3)%56,19+(f+i*17)%54,1,1,0xffffff);
    clip(s,0,0,180,180);R(43,19,2,54,0x6b4b3a);R(15,45,58,2,0x6b4b3a);R(10,74,68,4,0x7d5843);
    char hm[32],line[256];snprintf(hm,sizeof(hm),"%02d%c%02d",d->hour,d->second%2?' ':':',d->minute);
    ptext(s,hm,93,23,4,night?0x1c1729:0xd9bf9b);ptext(s,hm,92,22,4,night?0xffd9a8:0x5a3a2a);
    formatted(s,"{weekday_en} {month_en} {day}",line,sizeof(line));ptext(s,line,92,48,2,night?0xb9a4d6:0x8a6a52);
    formatted(s,"{temperature}° {weather_en}",line,sizeof(line));ptext(s,line,92,62,2,0xe8643c);
    R(96,96,72,3,0x7d5843);R(100,80,5,16,0xe8643c);R(106,83,5,13,0x5d7f9e);R(112,78,4,18,0xe9b949);R(117,86,10,10,0x8a6a52);R(148,88,12,8,0xc46a45);
    static const int leaf[5][2]={{150,82},{155,79},{146,84},{158,84},{152,76}};
    for(unsigned i=0;i<5;++i)R(leaf[i][0]+(leaf[i][1]<80?(f>>3)%2:0),leaf[i][1],3,4,0x6f9a52);
    R(0,124,180,8,0x9a6a48);R(0,124,180,1,0xb8875f);R(0,132,180,48,0x6e4630);
    if(night)for(int y=101;y<124;++y){float hw=(y-101)*.9f+4;A(25-hw,y,hw*2,1,0xffd68c,41);}
    R(18,120,16,4,0x3d3d46);R(25,104,2,16,0x3d3d46);R(22,98,4,7,0x3d3d46);R(15,92,20,7,0xe8643c);R(20,99,10,2,night?0xfff1b8:0xf6d8a0);
    R(65,92,48,32,0xf4f1ea);R(64,93,50,30,0xf4f1ea);R(68,96,42,22,0x121016);R(84,123,10,1,0xd8d2c6);
    if(sleep){R(76,107,9,2,0xffffff);R(93,107,9,2,0xffffff);int zy=(f>>2)%10;ptext(s,"Z",114,86-zy,1,night?0xffd9a8:0x5a3a2a);ptext(s,"Z",119,80-zy,2,night?0xffd9a8:0x5a3a2a);}
    else {const int looks[]={0,0,2,2,2,0,-2,-2,0,0};int lx=looks[(f>>4)%10],eh=f%46<2?2:10;R(78+lx,102+(10-eh)/2,5,eh,0xffffff);R(95+lx,102+(10-eh)/2,5,eh,0xffffff);}
    R(124,112,11,12,0xf4f1ea);R(135,115,3,6,0xf4f1ea);R(125,113,9,2,0x6b3f22);
    for(int k=0;k<2;++k)for(int st=0;st<5;++st)A(127+k*4+((st+(f>>2)+k)%3==0?1:0),108-st*2-((f>>1)%2),1,2,0xffffff,128);
    R(146,116,22,8,0x3a3a40);R(148,114,16,2,0x3a3a40);R(160,111,10,9,0x3a3a40);R(160,109,2,2,0x3a3a40);R(168,109,2,2,0x3a3a40);R(162,115,2,1,0x8c8c99);R(166,115,2,1,0x8c8c99);
    if((f>>3)%3)R(141,121,6,2,0x3a3a40);else R(142,118,2,4,0x3a3a40);
    formatted(s,d->event_count?"下一项 {eventtime0} {event0}":"今天没有更多日程",line,sizeof(line));ztext(s,line,10,140,0xf6e3c8);
    formatted(s,"TASK {done}/{total}  FOCUS {focus_minutes}M",line,sizeof(line));ptext(s,line,10,163,2,0xd9a77f);
}

static void sky(pixel_t *s,unsigned f)
{
    static const unsigned palettes[4][10]={
        {0x0b1030,0x131c45,0x1d2a5c,0x2a3a70,0x1a2246,0x111833,0x0d1428,0x2d3566,0xe8ecff,0x8f9bd0},
        {0x3a3a7a,0x8a5a92,0xe08a8a,0xf7c088,0x6a5480,0x43375e,0x2f2a48,0xf6c9b8,0xfffaf0,0xffe0c8},
        {0x3d9bff,0x62b2ff,0x8ccaff,0xbfe4ff,0x7aa6c8,0x4f8a6a,0x5f9c5a,0xffffff,0xffffff,0xeaf6ff},
        {0x2b2556,0x6a3a78,0xc4587a,0xff9a5a,0x5a3a6a,0x3a2650,0x2a1e3c,0xffb48a,0xfff4e6,0xffd2b0}};
    int h=s->data.hour,m=s->data.minute,p=h<5||h>=20?0:h<7?1:h<17?2:3;const unsigned *P=palettes[p];bool night=p==0;
    for(int i=0;i<4;++i){R(0,i*36,180,36,P[i]);if(i)for(int y=i*36-4;y<i*36;++y)for(int x=y%2;x<180;x+=2)if(y-i*36+4>=2||x%4==y%4)R(x,y,1,1,P[i]);}
    if(night)for(int i=0;i<40;++i)if(((f>>2)+i*7)%9)R((i*47)%180,(i*29)%100,1,1,i%6?0xcfd6ff:0xffffff);
    float fr=night?(((h+24-20)%24)*60+m)/540.f:fminf(1,fmaxf(0,((h-5)*60+m)/900.f));
    int bx=lroundf(14+fr*152),by=lroundf(104-sinf(fr*3.14159265f)*44);
    if(night){disc(s,bx,by,7,0xfff3c8,255);disc(s,bx+3,by-2,6,P[by/36<3?by/36:3],255);}
    else {disc(s,bx,by,10,0xffe68c,64);disc(s,bx,by,7,h<7||h>=17?0xffb15e:0xffe27a,255);}
    for(int i=0;i<3;++i){int x=lroundf(fmodf(f*(.25f+i*.12f)+i*70,230)-40),y=70+i*12;R(x,y,22,4,P[7]);R(x+5,y-3,10,3,P[7]);R(x+14,y-2,5,2,P[7]);}
    for(int x=0;x<180;++x){int hh=lroundf(34+9*sinf(x*.045f)+6*sinf(x*.13f+1));R(x,146-hh,1,hh,P[4]);}
    for(int x=0;x<180;++x){int hh=lroundf(18+7*sinf(x*.07f+2)+3*sinf(x*.21f));R(x,152-hh,1,hh,P[5]);}
    R(0,150,180,30,P[6]);const int trees[]={22,34,150,162};for(unsigned i=0;i<4;++i){int tx=trees[i];R(tx,140,2,10,0x2a1e18);for(int k=0;k<4;++k)R(tx-3+k,130+k*3,8-k*2,3,night?0x16243a:0x2f6a40);}
    R(112,140,20,12,night?0x3a3350:0xf0dcc0);for(int k=0;k<6;++k)R(110+k,139-k,24-k*2,1,0xc4583a);
    R(118,144,4,4,night?0xffd36b:0x5b6b80);R(126,146,4,6,0x6b4430);R(127,132,3,5,0x8a5a40);
    for(int k=0;k<4;++k)A(128+((k+(f>>2))%2),129-k*3-((f>>1)%3),2,2,0xffffff,115);
    if(rain(s))for(int i=0;i<40;++i)A((i*23+f)%180,(f*4+i*31)%180,1,3,0xd2e1ff,179);
    if(snow(s))for(int i=0;i<36;++i)R((i*23+(f>>1))%180,(f+i*31)%180,1,1,0xffffff);
    char hm[32],line[128];snprintf(hm,sizeof(hm),"%02d%c%02d",h,s->data.second%2?' ':':',m);int tx=(180-width(hm,5))/2;
    ptext(s,hm,tx+1,15,5,0x34485a);ptext(s,hm,tx,14,5,P[8]);
    formatted(s,"{weekday_en} {month_en} {day}  {temperature}°",line,sizeof(line));ptext(s,line,(180-width(line,2))/2,46,2,P[9]);
    const char *bl="RISE --:--   SET --:--";ptext(s,bl,(180-width(bl,2))/2,164,2,night?0x8f9bd0:0xf4ffe8);
}

static void buddy(pixel_t *s,unsigned f)
{
    const unsigned bg=0xf3e4cf,l=0xe6c6a2,m=0xe8643c,d=0x3a2418,w=0xfffaf2;bool sleep=s->data.hour>=23||s->data.hour<7;
    R(0,0,180,180,bg);char line[256],hm[32];snprintf(hm,sizeof(hm),"%02d%c%02d",s->data.hour,s->data.second%2?' ':':',s->data.minute);
    ptext(s,hm,8,8,3,d);formatted(s,"{weekday_en} {month_en} {day}",line,sizeof(line));ptext(s,line,8,28,2,m);
    /* Power decoration deliberately has no fabricated battery percentage. */
    R(148,9,22,11,d);R(170,12,2,5,d);R(150,11,18,7,bg);R(152,14,14,1,m);
    R(0,128,180,2,d);for(int x=6;x<180;x+=17){R(x,125,1,3,d);R(x-1,123,3,2,(x/17)%2?m:l);}
    int ht=f%40;const int hops[]={0,3,6,8,8,6,3,0};int hop=!sleep&&ht<8?hops[ht]:(int)((f>>3)%2),bx=22,by=92-hop;
    R(bx+6,126,28-hop*2,2,l);R(bx+1,by,38,28,d);R(bx,by+1,40,26,d);R(bx+2,by+2,36,24,w);R(bx+1,by+3,38,22,w);R(bx+5,by+5,30,17,d);
    R(bx+8,by+28,6,3,d);R(bx+26,by+28,6,3,d);R(bx+19,by-4,2,4,d);R(bx+18,by-7,4,3,(f>>2)%4?m:l);
    if(sleep){R(bx+11,by+13,6,1,w);R(bx+23,by+13,6,1,w);}
    else{int eh=f%52<2?1:7;const int look[]={0,1,1,0,-1,-1};int lx=look[(f>>4)%6];R(bx+12+lx,by+9+(7-eh)/2,4,eh,w);R(bx+24+lx,by+9+(7-eh)/2,4,eh,w);}
    ht=f%120;if(!sleep&&ht<28){int hy=by-12-(ht>>1);const int heart[][3]={{1,0,2},{5,0,2},{0,1,8},{0,2,8},{1,3,6},{2,4,4},{3,5,2}};for(unsigned i=0;i<7;++i)R(bx+36+heart[i][0],hy+heart[i][1],heart[i][2],1,m);}
    const char *lines[]={"今天也要加油","记得喝口水",s->data.event_count?"{eventtime0} 有安排":"今天很轻松","专注 {focus_minutes} 分钟","我在这儿陪你"};
    formatted(s,sleep?"晚安，好梦":lines[f/84%5],line,sizeof(line));
    int bw=10;const char *p=line;while(*p){uint32_t ch=utf8(&p);if(s->chinese&&!FT_Load_Char(s->chinese,ch,FT_LOAD_DEFAULT))bw+=s->chinese->glyph->advance.x/64;}
    if(bw>164)bw=164;
    int x0=172-bw,y0=64;R(x0,y0,bw,22,d);R(x0+1,y0+1,bw-2,20,w);for(int k=0;k<4;++k)R(x0-4+k,y0+14+k,5-k,1,d);ztext(s,line,x0+5,y0+4,d);
    if(sleep){int zy=(f>>2)%10;ptext(s,"Z",bx+44,by-4-zy,2,d);ptext(s,"Z",bx+52,by-12-zy,1,d);}
    unsigned stats[]={s->data.focus_minutes/12,s->data.todo_total?(unsigned)lroundf(s->data.todo_done*10.f/s->data.todo_total):0,8};const char *names[]={"FOCUS","TASKS","MOOD"};
    for(int i=0;i<3;++i){int y=140+i*13;ptext(s,names[i],8,y,2,d);for(int k=0;k<10;++k)R(62+k*11,y,9,10,(unsigned)k<stats[i]?m:l);}
    for(int x=0;x<180;++x)A(x,0,1,180,d,10);
}

static void dot_put(uint8_t *a,const char *text,int x,int y,int k,unsigned v)
{
    while(*text){const char *g=bits(utf8(&text));for(int i=0;i<15;++i)if(g[i]=='1')for(int j=0;j<k;++j)for(int q=0;q<k;++q){int px=x+i%3*k+j,py=y+i/3*k+q;if(px>=0&&px<72&&py>=0&&py<72)a[py*72+px]=(uint8_t)v;}x+=4*k;}
}
static void dots(pixel_t *s,unsigned f)
{
    uint8_t a[72*72]={0};char text[512];formatted(s,"{weekday_en} {month2}/{day2}",text,sizeof(text));dot_put(a,text,4,4,1,2);
    formatted(s,"{temperature}°",text,sizeof(text));dot_put(a,text,68-width(text,1),4,1,2);
    snprintf(text,sizeof(text),"%02d%c%02d",s->data.hour,s->data.second%2?' ':':',s->data.minute);dot_put(a,text,(72-width(text,3))/2,15,3,1);
    /* Chinese ticker uses the same thresholded 12 px glyphs as the source. */
    formatted(s,"{eventtime0} {event0} · 今天{weather} {temperature}° · 已专注 {focus_minutes} 分钟 · 小伴一直在这儿 ·",text,sizeof(text));
    const char *p=text;int tw=0;while(*p){uint32_t ch=utf8(&p);if(s->chinese&&!FT_Load_Char(s->chinese,ch,FT_LOAD_DEFAULT))tw+=s->chinese->glyph->advance.x/64;}
    int start=72-(int)(f%(unsigned)(tw+72));p=text;
    while(*p){uint32_t ch=utf8(&p);if(!s->chinese||FT_Load_Char(s->chinese,ch,FT_LOAD_RENDER))continue;FT_GlyphSlot g=s->chinese->glyph;FT_Bitmap *b=&g->bitmap;
        for(unsigned y=0;y<b->rows;++y)for(unsigned x=0;x<b->width;++x){int px=start+g->bitmap_left+(int)x,py=39+12-g->bitmap_top+(int)y;if(px>=0&&px<72&&py>=0&&py<72&&b->buffer[y*b->pitch+x]>110)a[py*72+px]=3;}
        start+=g->advance.x/64;
    }
    dot_put(a,"TASK",4,61,1,2);int lit=s->data.todo_total?lroundf(46.f*s->data.todo_done/s->data.todo_total):0;
    for(int x=0;x<46;++x)for(int y=61;y<66;++y)if(y!=63||x%2==0)a[y*72+21+x]=x<lit?1:0;
    R(0,0,720,720,0x0a0604);const unsigned cols[]={0x2a160d,0xff7a3d,0xc98a5e,0xffd2ac};
    for(int y=0;y<72;++y)for(int x=0;x<72;++x){int v=a[y*72+x],cx=x*10+5,cy=y*10+5;if(v)disc(s,cx,cy,6,v==1?0xff6e32:0xffbe8c,v==1?56:36);disc(s,cx,cy,v?4:3,cols[v],255);}
}

static void lava(pixel_t *s,unsigned f)
{
    float t=f/24.f,balls[7][3];for(int i=0;i<7;++i){balls[i][0]=60+38*sinf(t*.23f*(1+(i%3)*.5f)+i*1.7f);balls[i][1]=62+52*sinf(t*.17f*(1+i*.13f)+i*2.3f);balls[i][2]=(13+i%3*5)*(13+i%3*5);}
    for(int y=0;y<120;++y){float k=y/120.f,cr=255,cg=61+(179-61)*k,cb=127+(64-127)*k;
        for(int x=0;x<120;++x){float v=0;for(int i=0;i<7;++i){float dx=x-balls[i][0],dy=y-balls[i][1];v+=balls[i][2]/(dx*dx+dy*dy+1);}
            float a=v>=1?fminf(1,.82f+(v-1)*.25f):v>.6f?(v-.6f)*.9f:0;
            unsigned c=((unsigned)(28+(cr-28)*a)<<16)|((unsigned)(10+(cg-10)*a)<<8)|(unsigned)(34+(cb-34)*a);s->pixels[y*120+x]=color(c);
        }
    }
}
static float random_float(void) {return lv_rand(0,65535)/65535.f;}
static void particle_targets(pixel_t *s)
{
    if(!s->digits)return;
    uint8_t *mask=calloc(720*720,1);if(!mask)return;
    const char *p=s->data.time;int advance=0;
    for(const char *q=p;*q;++q)if(!FT_Load_Char(s->digits,(unsigned char)*q,FT_LOAD_DEFAULT))advance+=s->digits->glyph->advance.x/64;
    int pen=(720-advance)/2,baseline=330+80;
    for(;*p;++p)if(!FT_Load_Char(s->digits,(unsigned char)*p,FT_LOAD_RENDER)){FT_GlyphSlot g=s->digits->glyph;FT_Bitmap *b=&g->bitmap;
        int x0=pen+g->bitmap_left,y0=baseline-g->bitmap_top;for(unsigned y=0;y<b->rows;++y)for(unsigned x=0;x<b->width;++x){int xx=x0+(int)x,yy=y0+(int)y;if(xx>=0&&xx<720&&yy>=0&&yy<720)mask[yy*720+xx]=b->buffer[y*b->pitch+x];}pen+=g->advance.x/64;
    }
    unsigned count=0;for(unsigned y=0;y<720;y+=9)for(unsigned x=0;x<720;x+=9)if(mask[y*720+x]>128&&count<MAX_PARTICLES-160){particle_t *p=&s->particles[count++];p->tx=x;p->ty=y;p->dust=false;}
    free(mask);
    for(unsigned i=0;i<count;++i){unsigned j=lv_rand(i,count-1);float tx=s->particles[i].tx,ty=s->particles[i].ty;s->particles[i].tx=s->particles[j].tx;s->particles[i].ty=s->particles[j].ty;s->particles[j].tx=tx;s->particles[j].ty=ty;}
    unsigned total=count+160;for(unsigned i=0;i<total;++i){particle_t *p=&s->particles[i];if(i>=s->particle_count){p->x=random_float()*720;p->y=random_float()*720;p->a=random_float()*6.28f;}p->dust=i>=count;p->vx+=(random_float()-.5f)*46;p->vy+=(random_float()-.5f)*46;}
    s->particle_count=total;s->minute=s->data.minute;
}
static void particles(pixel_t *s,unsigned f)
{
    if(s->minute!=s->data.minute)particle_targets(s);
    A(0,0,720,720,0x100b09,82);
    for(unsigned i=0;i<s->particle_count;++i){particle_t *p=&s->particles[i];if(p->dust){p->a+=.004f;float r=300+(i%5)*9;p->tx=360+cosf(p->a)*r;p->ty=340+sinf(p->a)*r*.9f;}
        if(s->display->config.preview){p->x=p->tx;p->y=p->ty;}
        else {p->vx=(p->vx+(p->tx-p->x)*.035f+sinf(f*.05f+i)*.15f)*.86f;p->vy=(p->vy+(p->ty-p->y)*.035f+cosf(f*.045f+i)*.15f)*.86f;p->x+=p->vx;p->y+=p->vy;}
        if(p->dust)A(p->x-2.5f,p->y-2.5f,3,3,0xe8643c,140);else R(p->x-2.5f,p->y-2.5f,6,6,i%7==0?0xe8643c:0xf6ede2);
    }
}

static lv_obj_t *label(pixel_t *s,const char *text,unsigned size,uint32_t color,int x,int y,int w)
{lv_obj_t *o=ambient_text(s->display,s->root,text,size,500,color,x,y);if(w){lv_obj_set_width(o,w);lv_label_set_long_mode(o,LV_LABEL_LONG_DOT);}return o;}
static void editorial(pixel_t *s)
{
    unsigned ink=s->kind==LAVA?0xffe6ef:0xf6ede2;
    label(s,s->kind==LAVA?"BanOS · Lava":"BanOS · Particles",13,ink,48,40,300);
    s->date=label(s,"",13,ink,360,40,312);lv_obj_set_style_text_align(s->date,LV_TEXT_ALIGN_RIGHT,0);
    ambient_box(s->root,48,69,624,1,ink,0);lv_obj_set_style_bg_opa(lv_obj_get_child(s->root,-1),51,0);
    ambient_box(s->root,48,600,624,1,ink,0);lv_obj_set_style_bg_opa(lv_obj_get_child(s->root,-1),51,0);
    label(s,"WEATHER",12,ink,48,616,0);label(s,"NEXT",12,ink,222,616,0);label(s,"RANGE",12,ink,540,616,132);
    s->weather=label(s,"",20,ink,48,640,150);s->next=label(s,"",20,ink,222,640,290);s->range=label(s,"",20,ink,540,640,132);lv_obj_set_style_text_align(s->range,LV_TEXT_ALIGN_RIGHT,0);
    if(s->kind==LAVA){
        s->time=label(s,"",212,0xffffff,48,156,624);
        const char *assets=s->display->asset_root;if(assets[0]=='A'&&assets[1]==':')assets+=2;
        char path[640];snprintf(path,sizeof(path),"%s/standby/fonts/space-grotesk-500.ttf",assets);
        s->clock_font=lv_freetype_font_create(path,LV_FREETYPE_FONT_RENDER_MODE_BITMAP,212,LV_FREETYPE_FONT_STYLE_NORMAL);
        if(s->clock_font)lv_obj_set_style_text_font(s->time,s->clock_font,0);
        lv_obj_set_style_text_letter_space(s->time,-10,0);
        s->seconds=label(s,"",14,ink,48,388,0);
    }
}
static void destroy(void *context)
{pixel_t *s=context;if(s->clock_font)lv_freetype_font_delete(s->clock_font);if(s->chinese)FT_Done_Face(s->chinese);if(s->digits)FT_Done_Face(s->digits);if(s->ft)FT_Done_FreeType(s->ft);free(s->particles);free(s->pixels);free(s);}
static void *create(binbot_ambient_display_t *display,lv_obj_t *root,const ambient_theme_t *theme)
{
    static const char *ids[]={"pixel-room","pixel-sky","dot-matrix","pixel-buddy","lava-lamp","particle-time"};
    int kind=-1;for(int i=0;i<6;++i)if(!strcmp(theme->info.id,ids[i]))kind=i;if(kind<0)return NULL;
    pixel_t *s=calloc(1,sizeof(*s));if(!s)return NULL;s->display=display;s->root=root;s->kind=kind;s->n=kind==LAVA?120:kind==DOTS||kind==PARTICLES?720:180;s->minute=-1;
    s->pixels=calloc((size_t)s->n*s->n,sizeof(uint16_t));if(!s->pixels){free(s);return NULL;}
    clip(s,0,0,s->n,s->n);s->started=lv_tick_get();s->last_frame=UINT32_MAX;
    s->image=(lv_image_dsc_t){.header={.magic=LV_IMAGE_HEADER_MAGIC,.cf=LV_COLOR_FORMAT_RGB565,.w=s->n,.h=s->n,.stride=s->n*2},.data_size=s->n*s->n*2,.data=(const uint8_t *)s->pixels};
    s->picture=lv_image_create(root);lv_image_set_src(s->picture,&s->image);lv_image_set_pivot(s->picture,0,0);lv_image_set_scale(s->picture,720*256/s->n);lv_image_set_antialias(s->picture,kind==LAVA);
    const char *assets=display->asset_root;if(assets[0]=='A'&&assets[1]==':')assets+=2;char path[640];
    if(FT_Init_FreeType(&s->ft)){lv_obj_clean(root);destroy(s);return NULL;}
    snprintf(path,sizeof(path),"%s/fonts/NotoSansSC-SemiBold.ttf",assets);
    if(!FT_New_Face(s->ft,path,0,&s->chinese))FT_Set_Pixel_Sizes(s->chinese,0,12);
    if(kind==PARTICLES){s->particles=calloc(MAX_PARTICLES,sizeof(particle_t));snprintf(path,sizeof(path),"%s/standby/fonts/space-grotesk-700.ttf",assets);if(FT_New_Face(s->ft,path,0,&s->digits)){snprintf(path,sizeof(path),"%s/standby/fonts/space-grotesk.ttf",assets);(void)FT_New_Face(s->ft,path,0,&s->digits);}if(s->digits)FT_Set_Pixel_Sizes(s->digits,0,250);if(!s->particles||!s->digits){lv_obj_clean(root);destroy(s);return NULL;}}
    if(kind==LAVA||kind==PARTICLES)editorial(s);
    return s;
}
static void update(void *context,const binbot_ambient_snapshot_t *data)
{
    pixel_t *s=context;s->data=*data;
    if(s->kind==LAVA||s->kind==PARTICLES){char text[256];
        formatted(s,"{weekday_en} {month_en} {day2}",text,sizeof(text));ambient_text_set(s->date,text);
        formatted(s,"{weather} {temperature}°",text,sizeof(text));ambient_text_set(s->weather,text);
        formatted(s,"{eventtime0} {event0}",text,sizeof(text));ambient_text_set(s->next,text);
        formatted(s,"{range}",text,sizeof(text));ambient_text_set(s->range,text);
        if(s->kind==LAVA){ambient_text_set(s->time,data->time);formatted(s,":{second} SEC",text,sizeof(text));ambient_text_set(s->seconds,text);}
    }
}
static void tick(void *context,uint32_t now)
{
    pixel_t *s=context;unsigned fps=s->kind==LAVA?24:s->kind==PARTICLES?30:s->kind==BUDDY?10:s->kind==DOTS?18:12;
    unsigned f=(unsigned)((uint64_t)(now-s->started)*fps/1000);if(f==s->last_frame)return;s->last_frame=f;
    switch(s->kind){case ROOM:room(s,f);break;case SKY:sky(s,f);break;case DOTS:dots(s,f);break;case BUDDY:buddy(s,f);break;case LAVA:lava(s,f);break;default:particles(s,f);break;}
    lv_obj_invalidate(s->picture);
}
const ambient_renderer_t ambient_pixel_renderer={.id="pixel",.create=create,.update=update,.tick=tick,.destroy=destroy};
