/* SPDX-License-Identifier: Apache-2.0 */
#include "muse_native_voice.h"
#include <atomic>
#include <cstring>
#include <cstdio>
#include <cstdlib>
extern "C" {
#include "muse_link.h"
#include "muse_module_llm.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "mbedtls/base64.h"
#include "psa/crypto.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
}

/* One background job owns these buffers. Network callbacks only copy bounded
 * frames; neither the UI nor the chat session waits for network or UART I/O. */
static portMUX_TYPE lock=portMUX_INITIALIZER_UNLOCKED;
static std::atomic<bool> running{false},cancelled{false};
static std::atomic<int> state{0};
static uint32_t generation;
static int selected_voice,selected_speed;
static char job[25];
EXT_RAM_BSS_ATTR static char script[8193];
static int64_t now(){return esp_timer_get_time();}
static void pause_ms(int ms){vTaskDelay(pdMS_TO_TICKS(ms));}
static bool stopped(){return cancelled.load();}

struct FileReply {uint8_t data[4096];size_t length;int status;bool overflow,done;};
EXT_RAM_BSS_ATTR static FileReply file_reply;
static bool file_poisoned;
static void file_cb(void *,int status,const uint8_t *data,size_t length,bool end){
    portENTER_CRITICAL(&lock);
    if(status)file_reply.status=status;
    if(length && data){
        if(length>sizeof(file_reply.data)-file_reply.length)file_reply.overflow=true;
        else{memcpy(file_reply.data+file_reply.length,data,length);file_reply.length+=length;}
    }
    if(end)file_reply.done=true;
    portEXIT_CRITICAL(&lock);
}
static int read_file(const char *name,size_t offset,uint8_t *out,size_t cap,size_t &length){
    length=0;if(file_poisoned||stopped()||!muse_link_req_ready())return -1;
    char path[180],range[64];
    snprintf(path,sizeof(path),"/fs/raw/muse-storyteller/replies/%s/%s",job,name);
    snprintf(range,sizeof(range),"bytes=%u-%u",(unsigned)offset,(unsigned)(offset+4095));
    const char *headers[]={"Range",range,nullptr};
    portENTER_CRITICAL(&lock);file_reply={};portEXIT_CRITICAL(&lock);
    int64_t id=muse_link_req_open("GET",path,headers,true,file_cb,nullptr);
    if(!id)return -1;
    int64_t until=now()+10000000;
    bool done=false;
    while(now()<until&&!stopped()){
        portENTER_CRITICAL(&lock);done=file_reply.done;portEXIT_CRITICAL(&lock);
        if(done){break;}
        pause_ms(5);
    }
    if(!done){
        muse_link_req_cancel(id);until=now()+2000000;
        while(now()<until){portENTER_CRITICAL(&lock);done=file_reply.done;portEXIT_CRITICAL(&lock);if(done){break;}
        pause_ms(5);}
        if(!done)file_poisoned=true; // Never reuse storage under a late callback.
        return -1;
    }
    portENTER_CRITICAL(&lock);
    int status=file_reply.status;
    if(file_reply.overflow||file_reply.length>cap)status=-1;
    else{length=file_reply.length;if(length)memcpy(out,file_reply.data,length);}
    portEXIT_CRITICAL(&lock);
    return status;
}

static char rpc_id[64],rpc_session[9];
static bool rpc_waiting,rpc_done,rpc_ok;
static uint32_t rpc_serial;
bool muse_native_voice_reply(const cJSON *root,const char *session){
    const char *id=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root,"request_id"));
    const cJSON *data=cJSON_GetObjectItemCaseSensitive(root,"data");
    const char *owner=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(data,"session"));
    const char *event=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(data,"event"));
    const cJSON *error=cJSON_GetObjectItemCaseSensitive(root,"error");
    const cJSON *code=cJSON_GetObjectItemCaseSensitive(error,"code");
    if(!id||strncmp(id,"cloud-",6))return false;
    portENTER_CRITICAL(&lock);
    if(rpc_waiting && !strcmp(id,rpc_id) && owner && !strcmp(owner,rpc_session) && !strcmp(owner,session)){
        rpc_ok=event&&!strcmp(event,"cloud")&&cJSON_IsNumber(code)&&code->valuedouble==0;
        rpc_done=true;rpc_waiting=false;
    }
    portEXIT_CRITICAL(&lock);return true;
}
static bool rpc(cJSON *data,bool cleanup=false){
    char owner[9];muse_module_llm_session(owner);
    if(!owner[0]||(!cleanup&&stopped())){cJSON_Delete(data);return false;}
    cJSON *root=cJSON_CreateObject();if(!root||!data){cJSON_Delete(root);cJSON_Delete(data);return false;}
    char id[64];snprintf(id,sizeof(id),"cloud-%08lx-%08lx",(unsigned long)esp_random(),(unsigned long)++rpc_serial);
    cJSON_AddStringToObject(root,"request_id",id);cJSON_AddStringToObject(root,"work_id","musevoice");
    cJSON_AddStringToObject(root,"object","musevoice.v1");cJSON_AddStringToObject(root,"action","cloud");
    cJSON_AddStringToObject(data,"session",owner);cJSON_AddNumberToObject(data,"generation",0);
    cJSON_AddItemToObject(root,"data",data);
    char *json=cJSON_PrintUnformatted(root);cJSON_Delete(root);if(!json)return false;
    portENTER_CRITICAL(&lock);strlcpy(rpc_id,id,sizeof(rpc_id));strlcpy(rpc_session,owner,sizeof(rpc_session));rpc_waiting=true;rpc_done=rpc_ok=false;portEXIT_CRITICAL(&lock);
    bool sent=muse_module_llm_send_line(json);cJSON_free(json);
    int64_t until=now()+10000000;bool done=false,ok=false;
    while(sent&&now()<until&&(cleanup||!stopped())){
        portENTER_CRITICAL(&lock);done=rpc_done;ok=rpc_ok;portEXIT_CRITICAL(&lock);
        if(done){break;}
        pause_ms(5);
    }
    portENTER_CRITICAL(&lock);rpc_waiting=false;portEXIT_CRITICAL(&lock);
    return done&&ok;
}
static cJSON *operation(const char *op){cJSON *d=cJSON_CreateObject();if(d){cJSON_AddStringToObject(d,"op",op);cJSON_AddStringToObject(d,"key",job);}return d;}
static bool hex_digest(const uint8_t *data,size_t length,char out[65]){
    uint8_t digest[32];size_t written=0;if(psa_hash_compute(PSA_ALG_SHA_256,data,length,digest,sizeof(digest),&written)!=PSA_SUCCESS||written!=32)return false;
    for(unsigned i=0;i<32;i++){snprintf(out+2*i,3,"%02x",digest[i]);}
    return true;
}
static void worker(void *){
    bool ready=false,complete=false;
    uint8_t block[4097];size_t count=0,offset=0;
    char expected_source[65]={},expected_audio[65]={};
    size_t source_bytes=0,audio_bytes=0;
    int64_t until=now()+90000000;
    // Metadata must describe the exact immutable script, never a guessed caption.
    while(now()<until&&!stopped()){
        int status=read_file("meta.json",0,block,4096,count);
        if(status==404){pause_ms(250);continue;}
        if(status!=200&&status!=206)break;
        block[count]=0;cJSON *m=cJSON_Parse((char *)block);
        const char *phase=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m,"state"));
        const char *hash=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m,"source_sha256"));
        const char *voice=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m,"voice"));
        const char *speed=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m,"speed"));
        const char *expected_voice=selected_voice==1?"avocado_v2:MAI_01":"avocado_v2:MAI_03";
        static const char *speeds[]={"slow","normal","fast"};
        bool identity=voice&&speed&&!strcmp(voice,expected_voice)&&!strcmp(speed,speeds[selected_speed]);
        double bytes=cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(m,"source_bytes"));
        if(identity&&phase&&(!strcmp(phase,"streaming")||!strcmp(phase,"complete"))&&hash&&strlen(hash)==64&&bytes>=1&&bytes<=8192&&bytes==(size_t)bytes){
            strlcpy(expected_source,hash,65);source_bytes=(size_t)bytes;ready=true;
        }
        bool error=(phase&&!strcmp(phase,"error"))||(phase&&!identity);cJSON_Delete(m);
        if(ready||error){break;}
        pause_ms(150);
    }
    if(ready){
        size_t at=0;while(at<source_bytes&&!stopped()){
            int status=read_file("source.md",at,block,4096,count);
            if((status!=200&&status!=206)||!count||count>source_bytes-at){ready=false;break;}
            memcpy(script+at,block,count);at+=count;
        }
        script[at]=0;char actual[65];
        ready=ready&&at==source_bytes&&!memchr(script,0,at)&&hex_digest((uint8_t *)script,at,actual)&&!strcmp(actual,expected_source);
    }
    psa_hash_operation_t digest=PSA_HASH_OPERATION_INIT;if(psa_hash_setup(&digest,PSA_ALG_SHA_256)!=PSA_SUCCESS)ready=false;
    if(ready&&!stopped()){
        cJSON *d=operation("begin");cJSON_AddStringToObject(d,"text",script);ready=rpc(d);
    }
    until=now()+180000000;
    char encoded[5500];
    while(ready&&!stopped()&&now()<until){
        int status=read_file("audio.mp3",offset,block,4096,count);
        if(status==200||status==206){
            if(!count||offset+count>32*1024*1024){ready=false;break;}
            size_t written=0;if(mbedtls_base64_encode((uint8_t *)encoded,sizeof(encoded)-1,&written,block,count)){ready=false;break;}encoded[written]=0;
            cJSON *d=operation("chunk");cJSON_AddNumberToObject(d,"offset",offset);cJSON_AddStringToObject(d,"audio",encoded);
            if(!rpc(d)){ready=false;break;}
            if(psa_hash_update(&digest,block,count)!=PSA_SUCCESS){ready=false;break;}offset+=count;
            // Cache exists before speaking is queued; decoder can grow in parallel.
            state.store(1);continue;
        }
        if(status!=404&&status!=416){ready=false;break;}
        int ms=read_file("meta.json",0,block,4096,count);
        if(ms!=200&&ms!=206){ready=false;break;}
        block[count]=0;cJSON *m=cJSON_Parse((char *)block);
        const char *phase=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m,"state"));
        const char *hash=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m,"audio_sha256"));
        double bytes=cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(m,"audio_bytes"));
        if(phase&&!strcmp(phase,"complete")&&hash&&strlen(hash)==64&&bytes>=128&&bytes<=32*1024*1024&&bytes==(size_t)bytes){audio_bytes=(size_t)bytes;strlcpy(expected_audio,hash,65);complete=offset==audio_bytes;}
        bool error=phase&&!strcmp(phase,"error");cJSON_Delete(m);
        if(complete||error){if(error)ready=false;break;}pause_ms(100);
    }
    if(ready&&complete&&!stopped()){
        uint8_t binary[32]={};char actual[65];size_t written=0;if(psa_hash_finish(&digest,binary,sizeof(binary),&written)!=PSA_SUCCESS||written!=32)ready=false;for(int i=0;i<32;i++)snprintf(actual+2*i,3,"%02x",binary[i]);
        if(!ready||strcmp(actual,expected_audio))ready=false;
        else{cJSON *d=operation("end");cJSON_AddNumberToObject(d,"bytes",audio_bytes);cJSON_AddStringToObject(d,"sha256",actual);ready=rpc(d);}
    }else ready=false;
    psa_hash_abort(&digest);
    if(!ready){rpc(operation("abort"),true);if(!stopped())state.store(-1);}
    running.store(false);vTaskDeleteWithCaps(nullptr);
}
bool muse_native_voice_begin(const char *key,uint32_t gen,int voice,int speed){
    bool expected=false;if(!key||strlen(key)!=24||!running.compare_exchange_strong(expected,true))return false;
    for(const char *p=key;*p;p++)if(!((*p>='0'&&*p<='9')||(*p>='a'&&*p<='f'))){running.store(false);return false;}
    generation=gen;selected_voice=voice==1?1:0;selected_speed=speed>=0&&speed<=2?speed:1;strlcpy(job,key,sizeof(job));script[0]=0;cancelled.store(false);state.store(0);
    if(xTaskCreatePinnedToCoreWithCaps(worker,"muse_native",16384,nullptr,3,nullptr,tskNO_AFFINITY,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)!=pdPASS){running.store(false);state.store(-1);return false;}return true;
}
int muse_native_voice_copy(uint32_t gen,char *text,size_t capacity,char key[25]){
    if(gen!=generation)return -2;
    int value=state.load();if(value==1){if(strlen(script)>=capacity)return -1;strlcpy(text,script,capacity);strlcpy(key,job,25);}return value;
}
void muse_native_voice_cancel(){cancelled.store(true);}
