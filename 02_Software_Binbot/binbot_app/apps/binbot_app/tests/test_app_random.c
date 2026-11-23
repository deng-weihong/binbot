#define _DEFAULT_SOURCE
#include "binbot/app_modules.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *ids[]={"11111111-2222-4333-8444-555555555555","aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee"};
static void fixture(const char *root, const char *id)
{
    char path[512]; snprintf(path,sizeof(path),"%s/%s",root,id); assert(!mkdir(path,0700));
    snprintf(path,sizeof(path),"%s/%s/manifest.json",root,id); FILE *f=fopen(path,"wb"); assert(f);
    fprintf(f,"{\"schema\":1,\"id\":\"%s\",\"name\":\"image\"",id);
    const char *keys[]={"round","holo","thumbnail"};
    for(unsigned i=0;i<3;++i) fprintf(f,",\"%s\":{\"size\":10,\"sha256\":\"%064d\",\"width\":%u,\"height\":240}",keys[i],0,i==1?320:240);
    fputs("}",f); fclose(f);
    for(unsigned i=0;i<3;++i) {
        snprintf(path,sizeof(path),"%s/%s/%s.%s",root,id,keys[i],i==2?"png":"gif");
        f=fopen(path,"wb"); assert(f); assert(fwrite("0123456789",1,10,f)==10); fclose(f);
    }
}
int main(void)
{
    char root[]="/tmp/binbot-random-XXXXXX"; assert(mkdtemp(root));
    fixture(root,ids[0]); fixture(root,ids[1]);
    setenv("BINBOT_COMPONENT_STATE_DIR",root,1); setenv("BINBOT_COMPONENT_UART","/nonexistent-random-test",1);
    app_modules_t *m=app_modules_create(root); assert(m && app_modules_image_count(m)==2);
    app_modules_tick(m,500);
    bb_component_worker_t worker; bb_component_worker_init(&worker); app_modules_set_worker(m,&worker);
    binbot_component_event_t event={.has_info=1,.healthy=1,.profile=1,.boot_id=42,.status=BINBOT_COMPONENT_PROBED};
    app_modules_handle_event(m,&event);
    assert(app_modules_set_current(m,ids[0],1)); assert(app_modules_random_set(m,1,10));
    assert(app_modules_random_remaining(m,1)==10);
    app_modules_tick(m,10500);
    assert(worker.active_action==BINBOT_COMPONENT_FILE_UPDATE && app_modules_push_profile(m)==1);
    memcpy(event.request_id,worker.latest.request_id,16); bb_component_worker_close(&worker);
    event.status=BINBOT_COMPONENT_FAILED; app_modules_handle_event(m,&event);
    assert(!strcmp(app_modules_current(m,1),ids[0]));
    app_modules_tick(m,20500);
    assert(worker.active_action==BINBOT_COMPONENT_FILE_UPDATE);
    memcpy(event.request_id,worker.latest.request_id,16); bb_component_worker_close(&worker);
    event.status=BINBOT_COMPONENT_SUCCEEDED; app_modules_handle_event(m,&event);
    assert(!strcmp(app_modules_current(m,1),ids[1]));
    event.status=BINBOT_COMPONENT_PROBED; event.event_seq++;
    strcpy(event.message,"{\"pen\":{\"supported\":true,\"mode\":1,\"face\":0}}"); app_modules_handle_event(m,&event);
    worker.pid=1; worker.active_action=BINBOT_COMPONENT_PROBE;
    assert(app_modules_pen_set(m,1,0)); memcpy(event.request_id,worker.pending_command.request_id,16);
    worker.command_pending=false; worker.pid=0; event.status=BINBOT_COMPONENT_SUCCEEDED;
    app_modules_handle_event(m,&event); app_modules_tick(m,30500);
    assert(worker.active_action==BINBOT_COMPONENT_PROBE); bb_component_worker_close(&worker);
    assert(!strcmp(app_modules_current(m,1),ids[1]) && app_modules_random_interval(m,1)==10);
    app_modules_destroy(m);
    m=app_modules_create(root); assert(app_modules_random_interval(m,1)==10);
    assert(!app_modules_random_set(m,1,42) && !app_modules_random_set(m,3,10));
    app_modules_destroy(m);
    char path[512]; const char *files[]={"manifest.json","round.gif","holo.gif","thumbnail.png"};
    for(unsigned i=0;i<2;++i) {
        for(unsigned n=0;n<4;++n) { snprintf(path,sizeof(path),"%s/%s/%s",root,ids[i],files[n]); unlink(path); }
        snprintf(path,sizeof(path),"%s/%s",root,ids[i]); rmdir(path);
    }
    const char *states[]={"display.json","job.state","worker.lock","asset.gif","asset.gif.download"};
    for(unsigned i=0;i<sizeof(states)/sizeof(states[0]);++i) { snprintf(path,sizeof(path),"%s/%s",root,states[i]); unlink(path); }
    assert(!rmdir(root));
    puts("background random switching, nonrepeat selection, failure retry, info pause and persistence passed");
    return 0;
}
