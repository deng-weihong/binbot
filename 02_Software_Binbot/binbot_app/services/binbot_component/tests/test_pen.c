#define _DEFAULT_SOURCE
#include "binbot/component/worker.h"
#include <assert.h>
#include <poll.h>
#include <pty.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void serve(bb_component_worker_t *worker, int master, bb_component_pen_t *state, bool setting)
{
    bb_component_parser_t parser={0}; bb_component_frame_t request;
    unsigned sets=0;
    for(unsigned i=0;i<400 && worker->pid>0;++i) {
        struct pollfd fd={master,POLLIN,0};
        if(poll(&fd,1,10)>0 && fd.revents&POLLIN) {
            uint8_t bytes[512]; ssize_t count=read(master,bytes,sizeof(bytes)); assert(count>0);
            for(ssize_t n=0;n<count;++n) if(bb_component_parser_push(&parser,bytes[n],&request)) {
                bb_component_frame_t reply={.type=BB_COMPONENT_RESPONSE,.sequence=request.sequence,.command=request.command,.length=2};
                if(request.command==BB_COMPONENT_HELLO) {
                    reply.length+=BB_COMPONENT_INFO_BYTES;
                    uint8_t *body=reply.payload+2;
                    body[0]=1; body[2]=1; body[3]=BB_COMPONENT_CAP_PEN;
                    bb_component_put_u64(body+4,42); bb_component_put_u32(body+12,1024*1024);
                    strcpy((char *)body+32,"35"); strcpy((char *)body+64,bb_component_project(1));
                } else {
                    assert(request.command==BB_COMPONENT_PEN_GET || request.command==BB_COMPONENT_PEN_SET);
                    if(request.command==BB_COMPONENT_PEN_SET) {
                        assert(setting && bb_component_pen_decode(request.payload,request.length,state)); sets++;
                    } else assert(!request.length);
                    reply.length+=BB_COMPONENT_PEN_BYTES; bb_component_pen_encode(state,reply.payload+2);
                }
                uint8_t wire[BB_COMPONENT_FRAME_BYTES]; size_t size=bb_component_encode(&reply,wire,sizeof(wire));
                assert(write(master,wire,size)==(ssize_t)size);
            }
        }
        binbot_component_event_t event;
        bb_component_worker_poll(worker,&event,i*10);
    }
    assert(worker->pid==0 && sets==(unsigned)setting);
}

int main(void)
{
    int master,slave; char uart[128],root[]="/tmp/binbot-pen-uart-XXXXXX";
    assert(mkdtemp(root) && !openpty(&master,&slave,uart,NULL,NULL));
    setenv("BINBOT_COMPONENT_UART",uart,1); setenv("BINBOT_COMPONENT_STATE_DIR",root,1);
    bb_component_worker_t worker; bb_component_worker_init(&worker);
    bb_component_pen_t state={0};
    binbot_component_command_t command={.action=BINBOT_COMPONENT_PROBE}; command.request_id[0]=1;
    assert(bb_component_worker_submit(&worker,&command)); serve(&worker,master,&state,false);
    assert(worker.latest.status==BINBOT_COMPONENT_PROBED && worker.latest.healthy);
    assert(strstr(worker.latest.message,"\"supported\":true"));
    bb_component_pen_t requested={.mode=1,.face=2,.flags=3,.unix_time=1770000000,.utc_offset=-18000,
        .temperature=-35,.low=-80,.high=45,.weather_time=1769999000};
    command.action=BINBOT_COMPONENT_PEN_CONTROL; command.before_boot_id=42; command.request_id[0]=2;
    bb_component_pen_encode(&requested,(uint8_t *)command.url);
    assert(bb_component_worker_submit(&worker,&command)); serve(&worker,master,&state,true);
    assert(worker.latest.status==BINBOT_COMPONENT_SUCCEEDED);
    assert(strstr(worker.latest.message,"\"mode\":1,\"face\":2"));
    assert(state.temperature==-35 && state.utc_offset==-18000);
    command.request_id[0]=3; command.before_boot_id=43;
    assert(bb_component_worker_submit(&worker,&command)); serve(&worker,master,&state,false);
    assert(worker.latest.status==BINBOT_COMPONENT_FAILED);
    assert(state.mode==1 && state.face==2);
    bb_component_worker_close(&worker); close(master); close(slave);
    char path[256]; snprintf(path,sizeof(path),"%s/worker.lock",root); unlink(path); assert(!rmdir(root));
    puts("pen capability discovery, UART GET/SET, worker ABI and changed boot rejection passed");
    return 0;
}
