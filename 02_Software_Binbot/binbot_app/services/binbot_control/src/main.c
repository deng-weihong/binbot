#define _POSIX_C_SOURCE 200809L
#include "binbot/control/ipc.h"
#include <cJSON.h>
#include <mosquitto.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    struct mosquitto *mqtt;
    int app;
    bool connected, subscribed;
    char base[128], command[144], state[BB_CONTROL_MAX_MESSAGE];
    uint64_t last_app, last_publish;
} control_t;
static volatile sig_atomic_t running = 1;
static void stop(int number) { (void)number; running = 0; }
static uint64_t monotonic_ms(void)
{
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}
static const char *environment(const char *name, const char *fallback)
{
    const char *value = getenv(name); return value && *value ? value : fallback;
}
static void publish(control_t *c, const char *suffix, const char *json)
{
    char topic[160]; snprintf(topic,sizeof(topic),"%s/%s",c->base,suffix);
    if(c->connected && c->subscribed)
        mosquitto_publish(c->mqtt,NULL,topic,(int)strlen(json),json,1,false);
}
static void not_ready(control_t *c)
{
    if(!c->state[0]) return;
    cJSON *state = cJSON_Parse(c->state);
    if(!state) return;
    cJSON_ReplaceItemInObjectCaseSensitive(state,"ready",cJSON_CreateBool(false));
    char *json=cJSON_PrintUnformatted(state);
    if(json) { publish(c,"state",json); free(json); }
    cJSON_Delete(state);
    c->state[0]='\0';
}
static void connected(struct mosquitto *mqtt, void *context, int result)
{
    control_t *c=context; c->connected=result==0; c->subscribed=false;
    if(!result) mosquitto_subscribe(mqtt,NULL,c->command,1);
    else fprintf(stderr,"control: MQTT connection rejected code=%d\n",result);
}
static void subscribed(struct mosquitto *mqtt,void *context,int mid,int count,const int *qos)
{
    (void)mqtt; (void)mid;
    control_t *c=context; c->subscribed=count==1 && qos[0]!=128;
    c->last_publish=0;
}
static void disconnected(struct mosquitto *mqtt,void *context,int result)
{
    (void)mqtt; (void)result;
    control_t *c=context; c->connected=false; c->subscribed=false;
}
static void received(struct mosquitto *mqtt,void *context,const struct mosquitto_message *message)
{
    (void)mqtt;
    control_t *c=context;
    if(message->retain || strcmp(message->topic,c->command) || message->payloadlen<=0 ||
       message->payloadlen>=BB_CONTROL_MAX_MESSAGE) return;
    char buffer[BB_CONTROL_MAX_MESSAGE];
    memcpy(buffer,message->payload,(size_t)message->payloadlen); buffer[message->payloadlen]='\0';
    if(c->app>=0 && c->state[0] && monotonic_ms()-c->last_app<10000 &&
       bb_control_send(c->app,buffer)) return;
    not_ready(c);
    /* No local command queue: the backend expires the unresolved request. */
}
static bool valid_id(const char *id)
{
    if(!id || !*id || strlen(id)>64) return false;
    for(const char *p=id;*p;++p)
        if(!((*p>='a' && *p<='z') || (*p>='A' && *p<='Z') || (*p>='0' && *p<='9') ||
             *p=='.' || *p=='_' || *p=='-' || *p==':')) return false;
    return strcmp(id,".") && strcmp(id,"..");
}

int main(void)
{
    const char *device=getenv("BINBOT_DEVICE_ID"), *token=getenv("BINBOT_DEVICE_TOKEN");
    const char *host=environment("BINBOT_MQTT_HOST","127.0.0.1");
    const char *socket=environment("BINBOT_CONTROL_SOCKET",BB_CONTROL_DEFAULT_SOCKET);
    char *end=NULL;
    long port=strtol(environment("BINBOT_MQTT_PORT","1883"),&end,10);
    if(!valid_id(device) || !token || !*token || strlen(token)>512 || !end || *end || port<1 || port>65535) {
        fprintf(stderr,"control: configure unique BINBOT_DEVICE_ID, DEVICE_TOKEN and valid MQTT port\n"); return 2;
    }
    unsigned char random[8]; char client[112], suffix[17];
    FILE *rng=fopen("/dev/urandom","rb");
    bool random_ok=rng && fread(random,1,sizeof(random),rng)==sizeof(random);
    if(rng) fclose(rng);
    if(!random_ok) return 2;
    for(unsigned i=0;i<sizeof(random);++i) snprintf(suffix+i*2,3,"%02x",random[i]);
    snprintf(client,sizeof(client),"bb-%s:%s",device,suffix);
    control_t c={.app=-1};
    snprintf(c.base,sizeof(c.base),"binbot/v2/devices/%s",device);
    snprintf(c.command,sizeof(c.command),"%s/command",c.base);
    if(mosquitto_lib_init()!=MOSQ_ERR_SUCCESS) return 2;
    c.mqtt=mosquitto_new(client,true,&c);
    if(!c.mqtt) { mosquitto_lib_cleanup(); return 2; }
    int result=mosquitto_username_pw_set(c.mqtt,device,token);
    const char *ca=getenv("BINBOT_MQTT_CA_FILE");
    if(result==MOSQ_ERR_SUCCESS && ca && *ca) result=mosquitto_tls_set(c.mqtt,ca,NULL,NULL,NULL,NULL);
    char will_topic[160]; snprintf(will_topic,sizeof(will_topic),"%s/availability",c.base);
    const char *will="{\"online\":false}";
    if(result==MOSQ_ERR_SUCCESS) result=mosquitto_will_set(c.mqtt,will_topic,(int)strlen(will),will,1,false);
    if(result!=MOSQ_ERR_SUCCESS) { fprintf(stderr,"control: invalid MQTT setup\n"); mosquitto_destroy(c.mqtt); mosquitto_lib_cleanup(); return 2; }
    mosquitto_connect_callback_set(c.mqtt,connected);
    mosquitto_subscribe_callback_set(c.mqtt,subscribed);
    mosquitto_disconnect_callback_set(c.mqtt,disconnected);
    mosquitto_message_callback_set(c.mqtt,received);
    signal(SIGTERM,stop); signal(SIGINT,stop);
    uint64_t reconnect_at=0, app_at=0;
    unsigned backoff=1;
    fprintf(stdout,"control: started device=%s\n",device); fflush(stdout);
    while(running) {
        uint64_t now=monotonic_ms();
        if(!c.connected && now>=reconnect_at) {
            result=mosquitto_connect(c.mqtt,host,(int)port,15);
            if(result==MOSQ_ERR_SUCCESS) {
                /* CONNACK is processed by loop below; avoid reconnecting while pending. */
                reconnect_at=now+3000;
            } else {
                reconnect_at=now+backoff*1000U;
                if(backoff<30) backoff=backoff*2>30?30:backoff*2;
            }
        }
        result=mosquitto_loop(c.mqtt,20,1);
        if(c.connected) backoff=1;
        if(result!=MOSQ_ERR_SUCCESS && result!=MOSQ_ERR_NO_CONN) c.connected=false;
        if(c.app<0 && now>=app_at) { c.app=bb_control_connect(socket); app_at=now+1000; }
        if(c.app>=0) {
            char buffer[BB_CONTROL_MAX_MESSAGE];
            for(unsigned i=0;i<16;++i) {
                int count=bb_control_receive(c.app,buffer,sizeof(buffer));
                if(count<0) { close(c.app); c.app=-1; not_ready(&c); break; }
                if(!count) break;
                cJSON *root=cJSON_ParseWithOpts(buffer,NULL,true);
                const cJSON *type=cJSON_GetObjectItemCaseSensitive(root,"type");
                if(cJSON_IsString(type) && !strcmp(type->valuestring,"device.control.state")) {
                    snprintf(c.state,sizeof(c.state),"%s",buffer); c.last_app=now;
                    publish(&c,"state",buffer); c.last_publish=now;
                } else if(cJSON_IsString(type) && !strcmp(type->valuestring,"device.control.result")) publish(&c,"result",buffer);
                cJSON_Delete(root);
            }
        }
        if(c.state[0] && now-c.last_app>=10000) not_ready(&c);
        if(c.connected && c.subscribed && c.state[0] && now-c.last_publish>=1000) {
            publish(&c,"state",c.state); c.last_publish=now;
        }
        if(!c.connected) { struct timespec pause={0,20000000}; nanosleep(&pause,NULL); }
    }
    not_ready(&c);
    mosquitto_disconnect(c.mqtt); mosquitto_loop(c.mqtt,100,1);
    if(c.app>=0) close(c.app);
    mosquitto_destroy(c.mqtt); mosquitto_lib_cleanup();
    return 0;
}
