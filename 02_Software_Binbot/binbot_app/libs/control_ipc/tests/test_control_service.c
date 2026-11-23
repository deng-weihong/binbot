#define _POSIX_C_SOURCE 200809L
#include "binbot/control/service.h"
#include "binbot/control/ipc.h"
#include <assert.h>
#include <cJSON.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct { bb_control_service_t *service; unsigned calls; uint8_t brightness, volume; uint64_t pending; } fixture_t;
static bool brightness(void *context, uint8_t value)
{
    fixture_t *f=context; f->calls++;
    if(value==13) return false;
    f->brightness=value; return true;
}
static bool volume(void *context,uint8_t value,uint64_t request)
{
    fixture_t *f=context; f->volume=value; f->pending=request; return true;
}
static cJSON *receive_type(int fd,const char *type)
{
    for(int i=0;i<100;++i) {
        char buffer[BB_CONTROL_MAX_MESSAGE];
        int count=bb_control_receive(fd,buffer,sizeof(buffer));
        if(count>0) {
            cJSON *root=cJSON_Parse(buffer);
            const cJSON *item=cJSON_GetObjectItemCaseSensitive(root,"type");
            if(cJSON_IsString(item) && !strcmp(item->valuestring,type)) return root;
            cJSON_Delete(root);
        }
        struct timespec pause={0,1000000}; nanosleep(&pause,NULL);
    }
    assert(!"expected IPC result"); return NULL;
}
static void send_command(int fd,const char *boot,const char *id,int value,long offset,const char *name)
{
    char command[1024];
    long now=(long)time(NULL);
    snprintf(command,sizeof(command),
        "{\"type\":\"device.command\",\"schema_version\":2,\"boot_id\":\"%s\","
        "\"command_id\":\"%s\",\"name\":\"%s\",\"payload\":{\"%s\":%d},"
        "\"issued_at\":%ld,\"expires_at\":%ld}",boot,id,name,
        !strcmp(name,"screen.brightness.set")?"brightness":"volume",value,now+offset,now+offset+15);
    assert(bb_control_send(fd,command));
}
static bb_control_service_t *create(fixture_t *f,const char *socket,const char *journal)
{
    f->service=bb_control_service_create(socket,journal,(bb_control_hooks_t){f,brightness,volume});
    assert(f->service); bb_control_service_volume_result(f->service,0,40,true); return f->service;
}
static void test_vectors(fixture_t *f, int fd, const char *boot)
{
    FILE *file=fopen(BB_CONTROL_VECTORS_PATH,"r"); assert(file);
    char data[32768]; size_t count=fread(data,1,sizeof(data)-1,file); data[count]='\0';
    assert(feof(file)); fclose(file);
    cJSON *vectors=cJSON_Parse(data); assert(cJSON_IsArray(vectors));
    cJSON *vector; uint32_t tick=3000;
    cJSON_ArrayForEach(vector,vectors) {
        cJSON *root=cJSON_Duplicate(cJSON_GetObjectItem(vector,"command"),true); assert(root);
        cJSON_ReplaceItemInObjectCaseSensitive(root,"boot_id",cJSON_CreateString(boot));
        /* The shared vectors use a fixed clock; preserve missing fields. */
        if(cJSON_HasObjectItem(root,"issued_at"))
            cJSON_ReplaceItemInObjectCaseSensitive(root,"issued_at",cJSON_CreateNumber((double)time(NULL)));
        if(cJSON_HasObjectItem(root,"expires_at"))
            cJSON_ReplaceItemInObjectCaseSensitive(root,"expires_at",cJSON_CreateNumber((double)time(NULL)+15));
        char *json=cJSON_PrintUnformatted(root); assert(json && bb_control_send(fd,json));
        free(json); cJSON_Delete(root);
        bb_control_service_tick(f->service,tick,f->brightness); tick+=10;
        if(f->pending) { bb_control_service_volume_result(f->service,f->pending,f->volume,true); f->pending=0; }
        cJSON *result=receive_type(fd,"device.control.result");
        assert(cJSON_IsTrue(cJSON_GetObjectItem(result,"success")) ==
               cJSON_IsTrue(cJSON_GetObjectItem(vector,"valid")));
        cJSON_Delete(result);
    }
    cJSON_Delete(vectors);
}
static volatile sig_atomic_t running=1;
static void stop(int number) { (void)number; running=0; }
static uint32_t now_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint32_t)((uint64_t)t.tv_sec*1000+t.tv_nsec/1000000);
}
int main(int argc,char **argv)
{
    fixture_t f={.brightness=70};
    if(argc==4 && !strcmp(argv[1],"--serve")) {
        create(&f,argv[2],argv[3]); signal(SIGTERM,stop); signal(SIGINT,stop);
        while(running) {
            bb_control_service_tick(f.service,now_ms(),f.brightness);
            if(f.pending) { bb_control_service_volume_result(f.service,f.pending,f.volume,true); f.pending=0; }
            struct timespec pause={0,10000000}; nanosleep(&pause,NULL);
        }
        bb_control_service_destroy(f.service); return 0;
    }
    char folder[]="/tmp/binbot-control-test-XXXXXX"; assert(mkdtemp(folder));
    char socket[108],journal[200];
    snprintf(socket,sizeof(socket),"%s/control.sock",folder);
    snprintf(journal,sizeof(journal),"%s/commands.json",folder);
    create(&f,socket,journal);
    int fd=bb_control_connect(socket); assert(fd>=0);
    bb_control_service_tick(f.service,1000,f.brightness);
    cJSON *state=receive_type(fd,"device.control.state");
    char boot[33]; snprintf(boot,sizeof(boot),"%s",cJSON_GetObjectItem(state,"boot_id")->valuestring); cJSON_Delete(state);
    const char *id="11111111-1111-4111-8111-111111111111";
    send_command(fd,boot,id,40,0,"screen.brightness.set");
    bb_control_service_tick(f.service,1100,f.brightness);
    cJSON *result=receive_type(fd,"device.control.result");
    assert(cJSON_IsTrue(cJSON_GetObjectItem(result,"success"))); cJSON_Delete(result);
    assert(f.calls==1 && f.brightness==40);
    send_command(fd,boot,id,40,0,"screen.brightness.set");
    bb_control_service_tick(f.service,1200,f.brightness);
    result=receive_type(fd,"device.control.result"); cJSON_Delete(result); assert(f.calls==1);
    send_command(fd,boot,"22222222-2222-4222-8222-222222222222",20,-100,"screen.brightness.set");
    bb_control_service_tick(f.service,1300,f.brightness);
    result=receive_type(fd,"device.control.result"); assert(cJSON_IsFalse(cJSON_GetObjectItem(result,"success"))); cJSON_Delete(result); assert(f.calls==1);
    send_command(fd,boot,"33333333-3333-4333-8333-333333333333",13,0,"screen.brightness.set");
    bb_control_service_tick(f.service,1400,f.brightness);
    result=receive_type(fd,"device.control.result"); assert(cJSON_IsFalse(cJSON_GetObjectItem(result,"success"))); cJSON_Delete(result); assert(f.brightness==40);
    send_command(fd,boot,"44444444-4444-4444-8444-444444444444",61,0,"audio.volume.set");
    bb_control_service_tick(f.service,1500,f.brightness); assert(bb_control_volume_pending(f.service));
    assert(bb_control_receive(fd,(char[BB_CONTROL_MAX_MESSAGE]){0},BB_CONTROL_MAX_MESSAGE)>=0);
    bb_control_service_volume_result(f.service,f.pending,61,true);
    result=receive_type(fd,"device.control.result"); assert(cJSON_IsTrue(cJSON_GetObjectItem(result,"success"))); cJSON_Delete(result);
    close(fd); bb_control_service_destroy(f.service);
    create(&f,socket,journal); fd=bb_control_connect(socket); assert(fd>=0);
    bb_control_service_tick(f.service,2000,f.brightness); state=receive_type(fd,"device.control.state");
    snprintf(boot,sizeof(boot),"%s",cJSON_GetObjectItem(state,"boot_id")->valuestring); cJSON_Delete(state);
    unsigned calls=f.calls; send_command(fd,boot,id,40,0,"screen.brightness.set");
    bb_control_service_tick(f.service,2100,f.brightness);
    result=receive_type(fd,"device.control.result"); assert(cJSON_IsTrue(cJSON_GetObjectItem(result,"success"))); cJSON_Delete(result); assert(f.calls==calls);
    f.pending=0;
    test_vectors(&f,fd,boot);
    close(fd); bb_control_service_destroy(f.service); unlink(journal); rmdir(folder);
    puts("control: execution, failure, expiry, async volume and persistent dedup passed"); return 0;
}
