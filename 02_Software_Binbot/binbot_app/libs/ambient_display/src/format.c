#include "binbot/ambient_display.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void repeat(char *out,size_t capacity,const char *glyph,unsigned n)
{
    size_t used=0,width=strlen(glyph);
    while(n--&&used+width<capacity){memcpy(out+used,glyph,width);used+=width;}
    out[used]=0;
}

static void cn(unsigned n, char *out, size_t capacity)
{
    static const char *digits[] = {"零","一","二","三","四","五","六","七","八","九"};
    if(n < 10) snprintf(out, capacity, "%s", digits[n]);
    else if(n < 20) snprintf(out, capacity, "十%s", n % 10 ? digits[n % 10] : "");
    else if(n < 100) snprintf(out, capacity, "%s十%s", digits[n / 10], n % 10 ? digits[n % 10] : "");
    else snprintf(out, capacity, "%u", n);
}

static const char *weather_en(const binbot_ambient_snapshot_t *d)
{
    if(!d->weather_valid) return "--";
    if(d->weather_english[0]) return d->weather_english;
    if(strstr(d->weather_id, "snow")) return "SNOW";
    if(strstr(d->weather_id, "storm") || strstr(d->weather_id, "hail")) return "STORM";
    if(strstr(d->weather_id, "rain") || strstr(d->weather_id, "shower") || strstr(d->weather_id, "sleet")) return "RAIN";
    if(strstr(d->weather_id, "cloud")) return "CLOUDY";
    if(strstr(d->weather_id, "night")) return "CLEAR";
    if(!strcmp(d->weather_id, "overcast")) return "OVERCAST";
    if(!strcmp(d->weather_id, "fog")) return "FOG";
    return "SUNNY";
}

static void value(const binbot_ambient_snapshot_t *d, const char *key, char *out, size_t capacity)
{
    static const char *months[] = {"JAN","FEB","MAR","APR","MAY","JUN","JUL","AUG","SEP","OCT","NOV","DEC"};
    static const char *days[] = {"SUN","MON","TUE","WED","THU","FRI","SAT"};
    static const char *days_cn[] = {"周日","周一","周二","周三","周四","周五","周六"};
    const char *text = NULL;
    int i = -1;
    if(!strcmp(key, "time")) text = d->time;
    else if(!strcmp(key, "time_blink")) { snprintf(out, capacity, "%02d%c%02d", d->hour, d->second % 2 ? ';' : ':', d->minute); return; }
    else if(!strcmp(key, "hour")) { snprintf(out, capacity, "%02d", d->hour); return; }
    else if(!strcmp(key, "minute")) { snprintf(out, capacity, "%02d", d->minute); return; }
    else if(!strcmp(key, "second")) { snprintf(out, capacity, "%02d", d->second); return; }
    else if(!strcmp(key, "digits")) { snprintf(out, capacity, "%02d%02d%02d", d->hour, d->minute, d->second); return; }
    else if(sscanf(key, "digit%d", &i) == 1 && i >= 0 && i < 6) {
        char digits[32]; snprintf(digits, sizeof(digits), "%02d%02d%02d", d->hour, d->minute, d->second);
        snprintf(out, capacity, "%c", digits[i]); return;
    }
    else if(!strcmp(key, "year")) { snprintf(out, capacity, "%04d", d->year); return; }
    else if(!strcmp(key, "year2")) { snprintf(out, capacity, "%02d", d->year % 100); return; }
    else if(!strcmp(key, "month")) { snprintf(out, capacity, "%d", d->month); return; }
    else if(!strcmp(key, "month2")) { snprintf(out, capacity, "%02d", d->month); return; }
    else if(!strcmp(key, "day")) { snprintf(out, capacity, "%d", d->day); return; }
    else if(!strcmp(key, "day2")) { snprintf(out, capacity, "%02d", d->day); return; }
    else if(!strcmp(key,"day_year")) {
        static const int prior[]={0,31,59,90,120,151,181,212,243,273,304,334};
        int month=d->month>=1&&d->month<=12?d->month:1;
        bool leap=d->year%4==0&&(d->year%100!=0||d->year%400==0);
        snprintf(out,capacity,"%d",prior[month-1]+d->day+(leap&&month>2));return;
    }
    else if(!strcmp(key,"hour_remaining")){snprintf(out,capacity,"%d",60-d->minute);return;}
    else if(!strcmp(key, "month_en")) text = d->month >= 1 && d->month <= 12 ? months[d->month - 1] : "--";
    else if(!strcmp(key, "weekday_en")) text = d->weekday >= 0 && d->weekday < 7 ? days[d->weekday] : "--";
    else if(!strcmp(key, "weekday_cn")) text = d->weekday >= 0 && d->weekday < 7 ? days_cn[d->weekday] : "--";
    else if(!strcmp(key,"weekday_jp")){static const char *jp[]={"日","月","火","水","木","金","土"};text=d->weekday>=0&&d->weekday<7?jp[d->weekday]:"--";}
    else if(!strcmp(key, "hour_cn")) { cn((unsigned)d->hour, out, capacity); return; }
    else if(!strcmp(key, "minute_cn")) { cn((unsigned)d->minute, out, capacity); return; }
    else if(!strcmp(key, "month_cn")) { cn((unsigned)d->month, out, capacity); return; }
    else if(!strcmp(key, "day_cn")) { cn((unsigned)d->day, out, capacity); return; }
    else if(!strcmp(key, "date_cn")) {
        char month[32], day[32]; cn((unsigned)d->month, month, sizeof(month)); cn((unsigned)d->day, day, sizeof(day));
        snprintf(out, capacity, "%s月%s日", month, day); return;
    }
    else if(!strcmp(key, "weather")) text = d->weather_valid ? d->weather : "待更新";
    else if(!strcmp(key, "weather_en")) text = weather_en(d);
    else if(!strcmp(key,"weather_jp")) {
        const char *w=weather_en(d);text=!strcmp(w,"RAIN")?"雨":!strcmp(w,"STORM")?"雷雨":!strcmp(w,"SNOW")?"雪":
            !strcmp(w,"FOG")?"霧":!strcmp(w,"CLOUDY")?"曇り":!strcmp(w,"OVERCAST")?"曇天":d->weather_valid?"晴れ":"待更新";
    }
    else if(!strcmp(key, "temperature")) { if(d->weather_valid) snprintf(out, capacity, "%d", d->temperature); else snprintf(out, capacity, "--"); return; }
    else if(!strcmp(key, "temperature_cn")) {
        if(!d->weather_valid) text = "待更新";
        else { char n[32]; cn((unsigned)abs(d->temperature), n, sizeof(n)); snprintf(out, capacity, "%s%s", d->temperature < 0 ? "零下" : "", n); return; }
    }
    else if(!strcmp(key, "range")) {
        if(d->weather_valid) snprintf(out, capacity, "%d° / %d°", d->temperature_low, d->temperature_high);
        else snprintf(out, capacity, "--° / --°");
        return;
    }
    else if(!strcmp(key, "track")) text = d->track[0] ? d->track : "暂无音乐";
    else if(!strcmp(key, "artist")) text = d->artist;
    else if(!strcmp(key,"next_todo"))text=d->next_todo[0]?d->next_todo:"待办全部完成，今天很棒";
    else if(!strcmp(key, "media_state")) text = d->playing ? "▶ NOW PLAYING" : "❚❚ PAUSED";
    else if(!strcmp(key, "media_line")) {
        snprintf(out, capacity, "%s%s", d->playing ? "ON AIR · " : "STANDBY", d->playing ? d->track : ""); return;
    }
    else if(!strcmp(key, "focus_minutes")) { snprintf(out, capacity, "%u", d->focus_minutes); return; }
    else if(!strcmp(key, "done")) { snprintf(out, capacity, "%u", d->todo_done); return; }
    else if(!strcmp(key, "total")) { snprintf(out, capacity, "%u", d->todo_total); return; }
    else if(!strcmp(key, "remaining")) { snprintf(out, capacity, "%u", d->todo_total > d->todo_done ? d->todo_total - d->todo_done : 0); return; }
    else if(!strcmp(key,"fish_count")){unsigned open=d->todo_total>d->todo_done?d->todo_total-d->todo_done:0;snprintf(out,capacity,"%u",open>5?5:open);return;}
    else if(!strcmp(key,"kitty_todo")){unsigned open=d->todo_total>d->todo_done?d->todo_total-d->todo_done:0;if(open)snprintf(out,capacity,"还有 %u 件事喵～",open);else snprintf(out,capacity,"今天全部完成喵！");return;}
    else if(!strcmp(key,"busy"))text=d->todo_total>d->todo_done?"busy":"chill";
    else if(!strcmp(key,"temp_spark"))text=d->weather_valid?"▁▂▄▆▇█▇▅":"--------";
    else if(!strcmp(key,"todo_on")||!strcmp(key,"todo_off")) {
        unsigned n=!strcmp(key,"todo_on")?d->todo_done:d->todo_total>d->todo_done?d->todo_total-d->todo_done:0;
        repeat(out,capacity,!strcmp(key,"todo_on")?"■":"□",n>20?20:n);return;
    }
    else if(!strncmp(key,"focus6_",7)||!strncmp(key,"focus8_",7)) {
        unsigned max=key[5]=='6'?6:8,on=key[5]=='6'?(d->focus_minutes+12)/25:(d->focus_minutes*8+75)/150;
        if(on>max)on=max;
        bool off=strstr(key,"off")!=NULL;
        repeat(out,capacity,max==6?(off?"▱":"▰"):(off?"░":"█"),off?max-on:on);return;
    }
    else if(!strcmp(key,"sprout")||!strcmp(key,"sprout_level")) {
        unsigned stage=d->focus_minutes>=120?3:d->focus_minutes>=60?2:d->focus_minutes>=30?1:0;
        static const char *plant[]={"      \n      \n   .  \n [___]","      \n   ,  \n   |  \n [___]","      \n  \\|/ \n   |  \n [___]","   @  \n  \\|/ \n   |  \n [___]"};
        if(!strcmp(key,"sprout_level")){snprintf(out,capacity,"%u",stage+1);return;}text=plant[stage];
    }
    else if(sscanf(key,"sprout_log%d",&i)==1&&i>=0&&i<4){
        unsigned pick=((unsigned)(d->unix_time/3)+i)%7;
        const char *formats[]={"小伴: 今天{weather} {temperature}°，出门记得看天","sched: 下一项 {eventtime0} {event0}",
            "focus: 今日已专注 {focus_minutes} 分钟","music: {media_state} · {track} — {artist}","小伴: 起来喝口水吧 (•‿•)","todo: 还剩 {remaining} 件事，慢慢来","sprout: 小芽 Lv.{sprout_level}，又长高了一点"};
        binbot_ambient_format(d,formats[pick],out,capacity);return;
    }
    else if(sscanf(key,"logtime%d",&i)==1&&i>=0&&i<4){int seconds=d->hour*3600+d->minute*60+d->second-(3-i)*3;if(seconds<0)seconds+=86400;snprintf(out,capacity,"%02d:%02d:%02d",seconds/3600,seconds/60%60,seconds%60);return;}
    else if(sscanf(key,"taskstatus%d",&i)==1&&i>=0&&i<5)text=d->todos[i].done?"OK":"RUN";
    else if(sscanf(key, "todo%d", &i) == 1 && i >= 0 && i < 5) text = d->todos[i].text;
    else if(sscanf(key, "tasktime%d", &i) == 1 && i >= 0 && i < 5) text = d->todos[i].time;
    else if(sscanf(key, "eventtime%d", &i) == 1 && i >= 0 && i < 5) text = i<(int)d->event_count?d->events[i].time:"";
    else if(sscanf(key, "eventend%d", &i) == 1 && i >= 0 && i < 5) {
        time_t end=(time_t)d->events[i].end;struct tm local;
        if(i<(int)d->event_count&&end>0&&localtime_r(&end,&local))snprintf(out,capacity,"%02d:%02d",local.tm_hour,local.tm_min);
        else out[0]=0;
        return;
    }
    else if(sscanf(key, "eventflight%d", &i) == 1 && i >= 0 && i < 5) {
        if(i < (int)d->event_count) snprintf(out, capacity, "XB%02u", (unsigned)i+1); else out[0]=0; return;
    }
    else if(sscanf(key, "eventstatus%d", &i) == 1 && i >= 0 && i < 5) text = i >= (int)d->event_count ? "" :
        d->events[i].start <= d->unix_time ? "进行中" : d->events[i].start - d->unix_time < 1800 ? "即将开始" : "准时";
    else if(sscanf(key, "event%d", &i) == 1 && i >= 0 && i < 5) text = i<(int)d->event_count&&d->events[i].title[0] ? d->events[i].title : (i == 0 ? "暂无日程" : "");
    else if(sscanf(key, "world%d", &i) == 1 && i >= 0 && i < 3) text = d->world_time[i];
    else if(!strcmp(key, "countdown")) {
        int64_t seconds = d->event_count ? d->events[0].start - d->unix_time : 0;
        if(seconds < 0) seconds = 0;
        snprintf(out, capacity, "%02u:%02u:%02u", (unsigned)(seconds / 3600), (unsigned)(seconds / 60 % 60), (unsigned)(seconds % 60)); return;
    }
    else if(!strcmp(key, "uptime")) {
        snprintf(out, capacity, "%02u:%02u:%02u", d->uptime_seconds / 3600, d->uptime_seconds / 60 % 60, d->uptime_seconds % 60); return;
    }
    else if(!strcmp(key, "terminal_line")) text = d->event_count ? "calendar ready. next event synced." : "focus session ready. say 开始专注";
    else if(!strcmp(key, "diary")) {
        char events[192], media[192], temperature[24];
        if(d->event_count) snprintf(events,sizeof(events),"%s 还有「%s」。",d->events[0].time,d->events[0].title);
        else snprintf(events,sizeof(events),"接下来没有别的安排。");
        if(d->playing) snprintf(media,sizeof(media),"耳边放着《%s》。",d->track);else snprintf(media,sizeof(media),"屋里很安静。");
        snprintf(temperature,sizeof(temperature),"%d°",d->temperature);
        snprintf(out,capacity,"%d月%d日 %s %s\n\n窗外%s，%s。\n今天 %u 件事，做完了 %u 件。\n%s\n%s\n已经专注 %u 分钟，辛苦啦。\n\n—— 小伴",
            d->month,d->day,days_cn[d->weekday>=0&&d->weekday<7?d->weekday:0],d->time,d->weather_valid?d->weather:"天气待更新",d->weather_valid?temperature:"--°",
            d->todo_total,d->todo_done,events,media,d->focus_minutes);
        return;
    }
    else if(!strcmp(key, "issue")) { snprintf(out, capacity, "%04d%02d%02d", d->year, d->month, d->day); return; }
    snprintf(out, capacity, "%s", text ? text : "");
}

void binbot_ambient_format(const binbot_ambient_snapshot_t *snapshot, const char *format,
                           char *output, size_t capacity)
{
    if(!capacity || !output) return;
    output[0] = 0;
    if(!snapshot || !format) return;
    size_t used = 0;
    while(*format && used + 1 < capacity) {
        const char *end;
        if(*format == '{' && (end = strchr(format, '}')) && end - format < 48) {
            char key[48], result[2048];
            size_t n = (size_t)(end - format - 1); memcpy(key, format + 1, n); key[n] = 0;
            value(snapshot, key, result, sizeof(result));
            n = strlen(result); if(n > capacity - used - 1) n = capacity - used - 1;
            memcpy(output + used, result, n); used += n; format = end + 1;
        }
        else output[used++] = *format++;
    }
    output[used] = 0;
    /* Never leave the final UTF-8 code point half written. */
    if(used) {
        size_t start = used - 1; while(start && ((unsigned char)output[start] & 0xc0) == 0x80) --start;
        unsigned char c = (unsigned char)output[start];
        size_t bytes = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
        if(used - start < bytes) output[start] = 0;
    }
}
