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
static muse_native_voice_status_t progress;
static int64_t started_us,answer_ready_us;
static void phase(unsigned value,unsigned error=0){portENTER_CRITICAL(&lock);progress.phase=value;progress.error=error;portEXIT_CRITICAL(&lock);}
static void buffered(size_t received,size_t total){portENTER_CRITICAL(&lock);progress.received=received;progress.total=total;portEXIT_CRITICAL(&lock);}
EXT_RAM_BSS_ATTR static char script[8193];
static int64_t now(){return esp_timer_get_time();}
static void pause_ms(int ms){vTaskDelay(pdMS_TO_TICKS(ms));}
static bool stopped(){return cancelled.load();}

struct FileReply {uint8_t data[4096];size_t length;int status;bool overflow,done;};
EXT_RAM_BSS_ATTR static FileReply file_reply;
static uint32_t file_generation;
static void file_cb(void *context,int status,const uint8_t *data,size_t length,bool end){
    portENTER_CRITICAL(&lock);
    if((uint32_t)(uintptr_t)context!=file_generation){portEXIT_CRITICAL(&lock);return;}
    if(status)file_reply.status=status;
    if(length && data){
        if(length>sizeof(file_reply.data)-file_reply.length)file_reply.overflow=true;
        else{memcpy(file_reply.data+file_reply.length,data,length);file_reply.length+=length;}
    }
    if(end||status<0)file_reply.done=true;
    portEXIT_CRITICAL(&lock);
}
static int read_file_once(const char *name,size_t offset,uint8_t *out,size_t cap,size_t &length){
    length=0;if(stopped()||!muse_link_req_ready())return -1;
    char path[180],range[64];
    snprintf(path,sizeof(path),"/fs/raw/muse-storyteller/replies/%s/%s",job,name);
    snprintf(range,sizeof(range),"bytes=%u-%u",(unsigned)offset,(unsigned)(offset+cap-1));
    const char *headers[]={"Range",range,nullptr};
    portENTER_CRITICAL(&lock);file_reply={};uint32_t token=++file_generation;portEXIT_CRITICAL(&lock);
    int64_t id=muse_link_req_open("GET",path,headers,true,file_cb,(void *)(uintptr_t)token);
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
        // The next request has a new token; late callbacks cannot touch it.
        return -1;
    }
    portENTER_CRITICAL(&lock);
    int status=file_reply.status;
    if(file_reply.overflow||file_reply.length>cap)status=-1;
    else{length=file_reply.length;if(length)memcpy(out,file_reply.data,length);}
    portEXIT_CRITICAL(&lock);
    return status;
}
static int read_file(const char *name,size_t offset,uint8_t *out,size_t cap,size_t &length){
    int status=-1;unsigned attempt=0;int64_t deadline=now()+60000000;
    while(attempt<4 && now()<deadline && !stopped()){
        if(!muse_link_req_ready()){pause_ms(250);continue;}
        status=read_file_once(name,offset,out,cap,length);
        if(status>0&&status<500&&status!=408&&status!=429)return status;
        attempt++;printf("@native_transfer {\"retry\":%u,\"offset\":%u,\"status\":%d}\n",attempt,(unsigned)offset,status);fflush(stdout);
        pause_ms(250*attempt);
    }
    return status;
}

static char rpc_id[64],rpc_session[9];
static bool rpc_waiting,rpc_done,rpc_ok;
static int rpc_error;
static size_t rpc_received;
static bool rpc_complete;
static char rpc_hash[65];
static uint32_t rpc_serial;
static bool hex_digest(const uint8_t *data,size_t length,char out[65]);
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
        rpc_error=cJSON_IsNumber(code)?code->valueint:-99;
        const cJSON *result=cJSON_GetObjectItemCaseSensitive(data,"result");
        double received=cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(result,"received"));
        if(received>=0 && received<=32*1024*1024 && received==(size_t)received)rpc_received=(size_t)received;
        rpc_complete=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result,"complete"));
        const char *hash=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(result,"sha256"));
        if(hash&&strlen(hash)==64)strlcpy(rpc_hash,hash,sizeof(rpc_hash));
        rpc_done=true;rpc_waiting=false;
    }
    portEXIT_CRITICAL(&lock);return true;
}
static bool rpc_once(cJSON *data,bool cleanup){
    char owner[9];muse_module_llm_session(owner);
    if(!owner[0]||(!cleanup&&stopped())){cJSON_Delete(data);return false;}
    // Preserve the exact serialized payload through the UART parser. A valid
    // JSON frame alone cannot detect dropped characters inside a base64 string.
    char *payload=cJSON_PrintUnformatted(data);cJSON_Delete(data);char payload_hash[65];
    if(!payload || !hex_digest((const uint8_t *)payload,strlen(payload),payload_hash)){cJSON_free(payload);return false;}
    data=cJSON_CreateObject();
    if(!data){cJSON_free(payload);return false;}
    cJSON_AddStringToObject(data,"op","packet");
    cJSON_AddItemToObject(data,"payload",cJSON_CreateStringReference(payload));
    cJSON_AddStringToObject(data,"sha256",payload_hash);
    cJSON *root=cJSON_CreateObject();if(!root||!data){cJSON_Delete(root);cJSON_Delete(data);cJSON_free(payload);return false;}
    char id[64];snprintf(id,sizeof(id),"cloud-%08lx-%08lx",(unsigned long)esp_random(),(unsigned long)++rpc_serial);
    cJSON_AddStringToObject(root,"request_id",id);cJSON_AddStringToObject(root,"work_id","musevoice");
    cJSON_AddStringToObject(root,"object","musevoice.v1");cJSON_AddStringToObject(root,"action","cloud");
    cJSON_AddStringToObject(data,"session",owner);cJSON_AddNumberToObject(data,"generation",0);
    cJSON_AddItemToObject(root,"data",data);
    char *json=cJSON_PrintUnformatted(root);cJSON_Delete(root);cJSON_free(payload);if(!json)return false;
    portENTER_CRITICAL(&lock);strlcpy(rpc_id,id,sizeof(rpc_id));strlcpy(rpc_session,owner,sizeof(rpc_session));rpc_received=0;rpc_complete=false;rpc_hash[0]=0;rpc_error=-1;rpc_waiting=true;rpc_done=rpc_ok=false;portEXIT_CRITICAL(&lock);
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
static bool rpc(cJSON *data,bool cleanup=false){
    bool ok=false;size_t expected_received=0;
    const char *op=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(data,"op"));
    if(op&&!strcmp(op,"chunk"))expected_received=(size_t)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(data,"offset"))+(size_t)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(data,"bytes"));
    for(unsigned attempt=0;attempt<(cleanup?1U:3U)&&(!stopped()||cleanup);attempt++){
        cJSON *copy=cJSON_Duplicate(data,true);if(!copy)break;
        if(rpc_once(copy,cleanup)){
            portENTER_CRITICAL(&lock);size_t acknowledged=rpc_received;portEXIT_CRITICAL(&lock);
            if(!expected_received || acknowledged==expected_received){ok=true;break;}
        }
        if(!cleanup){
            portENTER_CRITICAL(&lock);int code=rpc_error;size_t acknowledged=rpc_received;portEXIT_CRITICAL(&lock);
            printf("@native_rpc {\"op\":\"%s\",\"attempt\":%u,\"code\":%d,\"received\":%u,\"expected\":%u}\n",op?op:"unknown",attempt+1,code,(unsigned)acknowledged,(unsigned)expected_received);fflush(stdout);
            pause_ms(500*(attempt+1));
        }
    }
    cJSON_Delete(data);return ok;
}
static cJSON *operation(const char *op){cJSON *d=cJSON_CreateObject();if(d){cJSON_AddStringToObject(d,"op",op);cJSON_AddStringToObject(d,"key",job);}return d;}
static bool hex_digest(const uint8_t *data,size_t length,char out[65]){
    uint8_t digest[32];size_t written=0;if(psa_hash_compute(PSA_ALG_SHA_256,data,length,digest,sizeof(digest),&written)!=PSA_SUCCESS||written!=32)return false;
    for(unsigned i=0;i<32;i++){snprintf(out+2*i,3,"%02x",digest[i]);}
    return true;
}
static bool transfer_once(unsigned &stage,bool &retryable){
    bool ready=false,complete=false;
    uint8_t block[4097];size_t count=0,offset=0;stage=0;retryable=true;
    char expected_source[65]={},expected_audio[65]={};
    size_t source_bytes=0,audio_bytes=0;
    int64_t until=now()+720000000;
    // Metadata must describe the exact immutable script, never a guessed caption.
    while(now()<until&&!stopped()){
        portENTER_CRITICAL(&lock);int64_t answered=answer_ready_us;portEXIT_CRITICAL(&lock);
        bool answer_timeout=answered && now()-answered>90000000;
        int status=read_file("meta.json",0,block,4096,count);
        if(status==404){if(answer_timeout){retryable=false;break;}pause_ms(250);continue;}
        if(status!=200&&status!=206){retryable=status!=401&&status!=403;printf("@native_transfer {\"stage\":\"metadata\",\"status\":%d}\n",status);fflush(stdout);break;}
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
            if(!strcmp(phase,"complete")){
                double audio=cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(m,"audio_bytes"));
                const char *sha=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m,"audio_sha256"));
                if(audio>=128 && audio<=32*1024*1024 && audio==(size_t)audio && sha && strlen(sha)==64){audio_bytes=(size_t)audio;strlcpy(expected_audio,sha,65);}
            }
        }
        bool error=(phase&&!strcmp(phase,"error"))||(phase&&!identity);cJSON_Delete(m);
        if(error)retryable=false;
        if(ready||error){break;}
        if(answer_timeout){retryable=false;break;}
        pause_ms(150);
    }
    if(ready){stage=1;
        size_t at=0;while(at<source_bytes&&!stopped()){
            int status=read_file("source.md",at,block,4096,count);
            if((status!=200&&status!=206)||!count||count>source_bytes-at){ready=false;break;}
            memcpy(script+at,block,count);at+=count;
        }
        script[at]=0;char actual[65];
        bool fetched=ready;
        ready=ready&&at==source_bytes&&!memchr(script,0,at)&&hex_digest((uint8_t *)script,at,actual)&&!strcmp(actual,expected_source);
        if(fetched&&!ready)retryable=false; // a different script is not a reconnect
    }
    psa_hash_operation_t digest=PSA_HASH_OPERATION_INIT;if(psa_hash_setup(&digest,PSA_ALG_SHA_256)!=PSA_SUCCESS)ready=false;
    if(ready&&!stopped()){stage=2;
        cJSON *d=operation("begin");cJSON_AddStringToObject(d,"text",script);ready=rpc(d);
    }
    size_t resume=0;bool cached=false;char cached_hash[65]={};
    if(ready){
        portENTER_CRITICAL(&lock);resume=rpc_received;cached=rpc_complete;strlcpy(cached_hash,rpc_hash,sizeof(cached_hash));portEXIT_CRITICAL(&lock);
        if(resume>32*1024*1024 || (audio_bytes && resume>audio_bytes))ready=false;
        else if(cached){
            ready=audio_bytes && resume==audio_bytes && !strcmp(cached_hash,expected_audio);
            if(ready){buffered(resume,audio_bytes);phase(4);state.store(1);psa_hash_abort(&digest);return true;}
        }
        offset=resume;buffered(offset,audio_bytes);phase(2);
    }
    until=now()+1200000000;
    char encoded[5500];
    while(ready&&!stopped()&&now()<until){stage=3;
        int status=read_file("audio.mp3",offset,block,4096,count);
        if(status==200||status==206){
            if(!count||offset+count>32*1024*1024){ready=false;break;}
            size_t sent=0;while(sent<count){size_t portion=count-sent>4096?4096:count-sent;
            size_t written=0;if(mbedtls_base64_encode((uint8_t *)encoded,sizeof(encoded)-1,&written,block+sent,portion)){ready=false;break;}encoded[written]=0;
            cJSON *d=operation("chunk");cJSON_AddNumberToObject(d,"offset",offset+sent);cJSON_AddStringToObject(d,"audio",encoded);
            char chunk_hash[65];if(!hex_digest(block+sent,portion,chunk_hash)){cJSON_Delete(d);ready=false;break;}
            cJSON_AddNumberToObject(d,"bytes",portion);cJSON_AddStringToObject(d,"sha256",chunk_hash);
            if(!rpc(d)){ready=false;break;}
            portENTER_CRITICAL(&lock);size_t acknowledged=rpc_received;portEXIT_CRITICAL(&lock);
            if(acknowledged!=offset+sent+portion){ready=false;break;}sent+=portion;
            }if(!ready)break;
            if(!resume && psa_hash_update(&digest,block,count)!=PSA_SUCCESS){ready=false;break;}offset+=count;buffered(offset,audio_bytes);
            // Cache exists before speaking is queued; decoder can grow in parallel.
            continue;
        }
        if(status!=404&&status!=416){ready=false;break;}
        int ms=read_file("meta.json",0,block,4096,count);
        if(ms!=200&&ms!=206){ready=false;break;}
        block[count]=0;cJSON *m=cJSON_Parse((char *)block);
        const char *phase=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m,"state"));
        const char *hash=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m,"audio_sha256"));
        double bytes=cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(m,"audio_bytes"));
        if(phase&&!strcmp(phase,"complete")&&hash&&strlen(hash)==64&&bytes>=128&&bytes<=32*1024*1024&&bytes==(size_t)bytes){audio_bytes=(size_t)bytes;strlcpy(expected_audio,hash,65);complete=offset==audio_bytes;buffered(offset,audio_bytes);}
        bool error=phase&&!strcmp(phase,"error");cJSON_Delete(m);
        if(complete||error){if(error){ready=false;retryable=false;}break;}pause_ms(100);
    }
    if(ready&&complete&&!stopped()){stage=4;
        phase(3);uint8_t binary[32]={};char actual[65];strlcpy(actual,expected_audio,sizeof(actual));size_t written=0;
        if(!resume){
            if(psa_hash_finish(&digest,binary,sizeof(binary),&written)!=PSA_SUCCESS||written!=32)ready=false;
            for(int i=0;i<32;i++)snprintf(actual+2*i,3,"%02x",binary[i]);
            if(strcmp(actual,expected_audio))ready=false;
        }
        // A resumed prefix is hash-checked together with every new byte by
        // the Module. No complete cache is admitted until that SHA matches.
        if(ready){cJSON *d=operation("end");cJSON_AddNumberToObject(d,"bytes",audio_bytes);cJSON_AddStringToObject(d,"sha256",actual);ready=rpc(d);if(ready){phase(4);state.store(1);}}
    }else ready=false;
    psa_hash_abort(&digest);
    if(!ready){printf("@native_transfer {\"failed_stage\":%u,\"received_bytes\":%u,\"cancelled\":%s,\"retryable\":%s}\n",stage,(unsigned)offset,stopped()?"true":"false",retryable?"true":"false");fflush(stdout);rpc(operation("abort"),true);}
    return ready;
}
static void worker(void *){
    unsigned stage=0;bool retryable=true,ready=false;
    const int64_t recovery_deadline=now()+360000000;
    for(unsigned attempt=0;attempt<3&&!stopped();attempt++){
        if(attempt){
            portENTER_CRITICAL(&lock);progress.recoveries=attempt;portEXIT_CRITICAL(&lock);
            phase(7,stage+1);
            printf("@native_recovery {\"attempt\":%u,\"stage\":%u}\n",attempt,stage);fflush(stdout);
            // Rejoin the existing paired Module session. Never reboot the
            // device or request synthesis again. A new begin discovers the
            // receiver's verified prefix, including after a Module restart.
            int64_t wait_until=now()+90000000;
            pause_ms(1000*attempt);
            while(now()<wait_until&&now()<recovery_deadline&&!stopped()){
                muse_module_llm_status_t st;muse_module_llm_status(&st);
                if(st.state==MUSE_MODULE_LLM_READY&&muse_link_req_ready())break;
                pause_ms(100);
            }
        }
        if(stopped() || (attempt&&now()>=recovery_deadline))break;
        phase(attempt?7:1);
        ready=transfer_once(stage,retryable);
        if(ready||!retryable)break;
    }
    if(!ready){phase(stopped()?6:5,stopped()?9:stage+1);if(!stopped())state.store(-1);}
    running.store(false);vTaskDeleteWithCaps(nullptr);
}
bool muse_native_voice_begin(const char *key,uint32_t gen,int voice,int speed){
    bool expected=false;if(!key||strlen(key)!=24||!running.compare_exchange_strong(expected,true))return false;
    for(const char *p=key;*p;p++)if(!((*p>='0'&&*p<='9')||(*p>='a'&&*p<='f'))){running.store(false);return false;}
    generation=gen;selected_voice=voice==1?1:0;selected_speed=speed>=0&&speed<=2?speed:1;strlcpy(job,key,sizeof(job));script[0]=0;cancelled.store(false);state.store(0);
    portENTER_CRITICAL(&lock);progress={};progress.phase=1;strlcpy(progress.job,key,sizeof(progress.job));started_us=now();answer_ready_us=0;portEXIT_CRITICAL(&lock);
    if(xTaskCreatePinnedToCoreWithCaps(worker,"muse_native",16384,nullptr,3,nullptr,tskNO_AFFINITY,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)!=pdPASS){running.store(false);state.store(-1);return false;}return true;
}
int muse_native_voice_copy(uint32_t gen,char *text,size_t capacity,char key[25]){
    if(gen!=generation)return -2;
    int value=state.load();if(value==1){if(strlen(script)>=capacity)return -1;strlcpy(text,script,capacity);strlcpy(key,job,25);}return value;
}
void muse_native_voice_cancel(){cancelled.store(true);}

bool muse_native_voice_rebind(uint32_t previous,uint32_t current){if(generation!=previous||cancelled.load()||state.load()<0)return false;generation=current;return true;}
void muse_native_voice_status(muse_native_voice_status_t *out){if(!out)return;portENTER_CRITICAL(&lock);*out=progress;out->elapsed_ms=started_us?(unsigned)((now()-started_us)/1000):0;portEXIT_CRITICAL(&lock);out->active=running.load();}
void muse_native_voice_answer_ready(uint32_t gen,bool pending){if(gen!=generation)return;portENTER_CRITICAL(&lock);if(pending)answer_ready_us=0;else if(!answer_ready_us)answer_ready_us=now();portEXIT_CRITICAL(&lock);}
