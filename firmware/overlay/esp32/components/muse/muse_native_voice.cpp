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
static std::atomic<bool> section_mode{false},sections_complete{false};
static std::atomic<unsigned> direct_failure{0};
static uint32_t generation;
static int selected_voice,selected_speed;
static char job[25];
static muse_native_voice_status_t progress;
static int64_t started_us,answer_ready_us;
static void phase(unsigned value,unsigned error=0){portENTER_CRITICAL(&lock);progress.phase=value;progress.error=error;if(value==4&&!progress.opening_ready_ms){progress.opening_ready_ms=(unsigned)((esp_timer_get_time()-started_us)/1000);if(progress.words_ready)progress.opening_words=progress.words_ready;}portEXIT_CRITICAL(&lock);}
static void buffered(size_t received,size_t total){portENTER_CRITICAL(&lock);progress.received=received;progress.total=total;portEXIT_CRITICAL(&lock);}
EXT_RAM_BSS_ATTR static char script[8193];
static int64_t now(){return esp_timer_get_time();}
static void pause_ms(int ms){vTaskDelay(pdMS_TO_TICKS(ms));}
static bool stopped(){return cancelled.load();}

static constexpr size_t FILE_BATCH=32768;
struct FileReply {uint8_t data[FILE_BATCH];size_t length,limit;int status;bool overflow,done;};
EXT_RAM_BSS_ATTR static FileReply file_reply;
EXT_RAM_BSS_ATTR static uint8_t file_batch[FILE_BATCH+1];
EXT_RAM_BSS_ATTR static char section_metadata[4097];
static unsigned file_requests;
static uint32_t file_generation;
static void file_cb(void *context,int status,const uint8_t *data,size_t length,bool end){
    portENTER_CRITICAL(&lock);
    if((uint32_t)(uintptr_t)context!=file_generation){portEXIT_CRITICAL(&lock);return;}
    if(status)file_reply.status=status;
    if(length && data){
        if(length>file_reply.limit-file_reply.length)file_reply.overflow=true;
        else{memcpy(file_reply.data+file_reply.length,data,length);file_reply.length+=length;}
    }
    if(end||status<0)file_reply.done=true;
    portEXIT_CRITICAL(&lock);
}
static int read_file_once(const char *name,size_t offset,uint8_t *out,size_t cap,size_t &length,int64_t deadline=0){
    length=0;if(!cap||cap>FILE_BATCH||stopped()||!muse_link_req_ready())return -1;
    if(deadline&&now()>=deadline)return -1;
    char path[180],range[64];
    snprintf(path,sizeof(path),"/fs/raw/muse-storyteller/replies/%s/%s",job,name);
    snprintf(range,sizeof(range),"bytes=%u-%u",(unsigned)offset,(unsigned)(offset+cap-1));
    const char *headers[]={"Range",range,nullptr};
    portENTER_CRITICAL(&lock);file_reply={};file_reply.limit=cap;uint32_t token=++file_generation;portEXIT_CRITICAL(&lock);
    int64_t id=muse_link_req_open("GET",path,headers,true,file_cb,(void *)(uintptr_t)token);
    if(!id)return -1;
    file_requests++;
    int64_t until=now()+10000000;
    if(deadline&&deadline<until)until=deadline;
    bool done=false;
    while(now()<until&&!stopped()){
        portENTER_CRITICAL(&lock);done=file_reply.done;portEXIT_CRITICAL(&lock);
        if(done){break;}
        pause_ms(5);
    }
    if(!done){
        muse_link_req_cancel(id);until=now()+2000000;
        if(deadline&&deadline<until)until=deadline;
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
static bool rpc_waiting,rpc_done,rpc_ok,rpc_bad_frame;
static int rpc_error;
static size_t rpc_received,rpc_prefix_bytes;
static unsigned rpc_ready_sections,rpc_offline_sections;
static bool rpc_complete,rpc_ready,rpc_ready_known;
static char rpc_hash[65];
static uint32_t rpc_serial;
static int64_t profile_wire_us,profile_ack_us;
static unsigned profile_packets;
static unsigned direct_recoveries;
static size_t direct_resume_bytes;
static bool hex_digest(const uint8_t *data,size_t length,char out[65]);
static bool native_message_key(const char *message,const char *text,char out[25]){
    char source[65],identity[256],hash[65];
    if(!hex_digest((const uint8_t *)text,strlen(text),source))return false;
    int length=snprintf(identity,sizeof(identity),"muse-app-message-v1\n%s\n%s",message,source);
    if(length<0||(size_t)length>=sizeof(identity)||!hex_digest((const uint8_t *)identity,(size_t)length,hash))return false;
    memcpy(out,hash,24);out[24]=0;return true;
}
static bool digest_snapshot(const psa_hash_operation_t *source,char out[65]){
    psa_hash_operation_t copy=PSA_HASH_OPERATION_INIT;uint8_t binary[32];size_t written=0;
    bool okay=psa_hash_clone(source,&copy)==PSA_SUCCESS&&psa_hash_finish(&copy,binary,sizeof(binary),&written)==PSA_SUCCESS&&written==32;
    psa_hash_abort(&copy);
    if(okay)for(unsigned i=0;i<32;i++)snprintf(out+2*i,3,"%02x",binary[i]);
    return okay;
}
bool muse_native_voice_reply(const cJSON *root,const char *session){
    char reason[97]={0};int rejected=0;
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
        if(rpc_error){
            const char *message=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(error,"message"));
            if(message){
                strlcpy(reason,message,sizeof(reason));
                rpc_bad_frame=!strncmp(message,"Binary block ",13)||!strncmp(message,"Invalid binary block ",21)||
                    !strcmp(message,"Native request integrity check failed");
            }
            rejected=rpc_error;
        }
        const cJSON *result=cJSON_GetObjectItemCaseSensitive(data,"result");
        double received=cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(result,"received"));
        if(received>=0 && received<=32*1024*1024 && received==(size_t)received)rpc_received=(size_t)received;
        rpc_complete=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result,"complete"));
        const cJSON *ready=cJSON_GetObjectItemCaseSensitive(result,"ready");
        rpc_ready=cJSON_IsTrue(ready);rpc_ready_known=cJSON_IsBool(ready);
        double prefix=cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(result,"prefix_bytes"));
        double parts=cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(result,"ready_sections"));
        double offline=cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(result,"offline_sections"));
        if(prefix>=0&&prefix<=32*1024*1024&&prefix==(size_t)prefix)rpc_prefix_bytes=(size_t)prefix;
        if(parts>=0&&parts<=128&&parts==(unsigned)parts)rpc_ready_sections=(unsigned)parts;
        if(offline>=0&&offline<=rpc_ready_sections&&offline==(unsigned)offline)rpc_offline_sections=(unsigned)offline;
        const char *hash=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(result,"sha256"));
        if(hash&&strlen(hash)==64)strlcpy(rpc_hash,hash,sizeof(rpc_hash));
        rpc_done=true;rpc_waiting=false;
    }
    portEXIT_CRITICAL(&lock);
    if(rejected){for(char *p=reason;*p;p++)if((unsigned char)*p<32)*p=' ';printf("@native_rpc_error code=%d reason=%s\n",rejected,reason);fflush(stdout);}
    return true;
}
static void reset_rpc_reply(const char *id,const char *owner){
    portENTER_CRITICAL(&lock);
    strlcpy(rpc_id,id,sizeof(rpc_id));strlcpy(rpc_session,owner,sizeof(rpc_session));
    rpc_received=rpc_prefix_bytes=0;rpc_ready_sections=rpc_offline_sections=0;rpc_hash[0]=0;
    rpc_complete=rpc_ready=rpc_ready_known=rpc_bad_frame=false;
    rpc_error=-1;rpc_waiting=true;rpc_done=rpc_ok=false;
    portEXIT_CRITICAL(&lock);
}
static bool rpc_once(cJSON *data,bool cleanup){
    portENTER_CRITICAL(&lock);rpc_error=-90;rpc_bad_frame=false;portEXIT_CRITICAL(&lock);
    char owner[9];muse_module_llm_session(owner);
    if(!owner[0]||(!cleanup&&stopped())){cJSON_Delete(data);return false;}
    // Preserve the exact serialized payload through the UART parser. A valid
    // JSON frame alone cannot detect dropped characters inside a base64 string.
    const char *op=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(data,"op"));
    int64_t timeout_us=op&&(!strcmp(op,"part_begin")||!strcmp(op,"part_end")||!strcmp(op,"series_status"))?4000000:10000000;
    char *payload=cJSON_PrintUnformatted(data);cJSON_Delete(data);char payload_hash[65];
    if(!payload || !hex_digest((const uint8_t *)payload,strlen(payload),payload_hash)){cJSON_free(payload);return false;}
    data=cJSON_CreateObject();
    if(!data){cJSON_free(payload);return false;}
    cJSON_AddStringToObject(data,"op","packet");
    // The vendor's UART extractor counts braces even inside quoted JSON.
    // An ASCII envelope keeps response punctuation and unmatched code braces
    // out of that extractor; the checksum still covers the original UTF-8.
    size_t encoded_size=4*((strlen(payload)+2)/3)+1,encoded_length=0;
    char *encoded_payload=(char *)malloc(encoded_size);
    if(!encoded_payload||mbedtls_base64_encode((uint8_t *)encoded_payload,encoded_size,&encoded_length,(const uint8_t *)payload,strlen(payload))){free(encoded_payload);cJSON_free(payload);cJSON_Delete(data);return false;}
    encoded_payload[encoded_length]=0;
    cJSON_AddStringToObject(data,"payload_b64",encoded_payload);free(encoded_payload);
    cJSON_AddStringToObject(data,"sha256",payload_hash);
    cJSON *root=cJSON_CreateObject();if(!root||!data){cJSON_Delete(root);cJSON_Delete(data);cJSON_free(payload);return false;}
    char id[64];snprintf(id,sizeof(id),"cloud-%08lx-%08lx",(unsigned long)esp_random(),(unsigned long)++rpc_serial);
    cJSON_AddStringToObject(root,"request_id",id);cJSON_AddStringToObject(root,"work_id","musevoice");
    cJSON_AddStringToObject(root,"object","musevoice.v1");cJSON_AddStringToObject(root,"action","cloud");
    cJSON_AddStringToObject(data,"session",owner);cJSON_AddNumberToObject(data,"generation",0);
    cJSON_AddItemToObject(root,"data",data);
    char *json=cJSON_PrintUnformatted(root);cJSON_Delete(root);cJSON_free(payload);if(!json)return false;
    reset_rpc_reply(id,owner);
    bool sent=muse_module_llm_send_line(json);cJSON_free(json);
    int64_t until=now()+timeout_us;bool done=false,ok=false;
    while(sent&&now()<until&&(cleanup||!stopped())){
        portENTER_CRITICAL(&lock);done=rpc_done;ok=rpc_ok;portEXIT_CRITICAL(&lock);
        if(done){break;}
        pause_ms(5);
    }
    portENTER_CRITICAL(&lock);rpc_waiting=false;portEXIT_CRITICAL(&lock);
    return done&&ok;
}
static bool rpc_raw_body(const char *key,const uint8_t *raw,size_t decoded,size_t offset,const char *hash){
    portENTER_CRITICAL(&lock);rpc_error=-90;rpc_bad_frame=false;portEXIT_CRITICAL(&lock);
    char owner[9];muse_module_llm_session(owner);
    if(!owner[0]||stopped())return false;
    char signature[65],canonical[150];
    snprintf(canonical,sizeof(canonical),"%s:%u:%u:%s",key,(unsigned)offset,(unsigned)decoded,hash);
    if(!hex_digest((const uint8_t *)canonical,strlen(canonical),signature)){return false;}
    cJSON *root=cJSON_CreateObject(),*info=cJSON_CreateObject();
    if(!root||!info){cJSON_Delete(root);cJSON_Delete(info);return false;}
    // Keep the complete frame in JSON mode. The stock RAW extractor can lose
    // framing during long transfers; raw_info/data already has the same verified
    // receiver path after decoding its single base64 layer.
    char id[64];snprintf(id,sizeof(id),"cloud-%08lx-%08lx",(unsigned long)esp_random(),(unsigned long)++rpc_serial);
    cJSON_AddStringToObject(root,"request_id",id);cJSON_AddStringToObject(root,"work_id","musevoice");cJSON_AddStringToObject(root,"object","musevoice.v1");cJSON_AddStringToObject(root,"action","cloud");
    cJSON_AddStringToObject(info,"session",owner);cJSON_AddNumberToObject(info,"generation",0);cJSON_AddStringToObject(info,"key",key);cJSON_AddNumberToObject(info,"offset",offset);cJSON_AddNumberToObject(info,"bytes",decoded);cJSON_AddStringToObject(info,"sha256",hash);cJSON_AddStringToObject(info,"packet_sha256",signature);cJSON_AddItemToObject(root,"raw_info",info);
    size_t capacity=4*((decoded+2)/3)+1,encoded_length=0;char *encoded=(char *)malloc(capacity);
    if(!encoded||mbedtls_base64_encode((uint8_t *)encoded,capacity,&encoded_length,raw,decoded)){free(encoded);cJSON_Delete(root);return false;}
    encoded[encoded_length]=0;cJSON_AddStringToObject(root,"data",encoded);free(encoded);
    char *json=cJSON_PrintUnformatted(root);cJSON_Delete(root);
    if(!json)return false;
    reset_rpc_reply(id,owner);
    int64_t wire_started=now();
    bool sent=muse_module_llm_send_line(json);cJSON_free(json);
    int64_t ack_started=now();profile_wire_us+=ack_started-wire_started;profile_packets++;
    int64_t until=now()+3000000;bool done=false,ok=false;
    while(sent&&now()<until&&!stopped()){
        portENTER_CRITICAL(&lock);done=rpc_done;ok=rpc_ok;portEXIT_CRITICAL(&lock);
        if(done){break;}
        pause_ms(2);
    }
    profile_ack_us+=now()-ack_started;
    portENTER_CRITICAL(&lock);rpc_waiting=false;portEXIT_CRITICAL(&lock);return done&&ok;
}
static bool rpc_raw_once(cJSON *data){
    const char *key=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(data,"key"));
    const char *audio=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(data,"audio"));
    const char *expected=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(data,"sha256"));
    double bytes=cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(data,"bytes"));
    double offset=cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(data,"offset"));
    if(!key||strlen(key)!=24||!audio||strlen(audio)>11000||!expected||strlen(expected)!=64||!(bytes>=1&&bytes<=8192)||!(offset>=0&&offset<=32*1024*1024)||bytes!=(size_t)bytes||offset!=(size_t)offset){cJSON_Delete(data);return false;}
    uint8_t *raw=(uint8_t *)malloc((size_t)bytes);size_t decoded=0;char hash[65];
    bool okay=raw&&!mbedtls_base64_decode(raw,(size_t)bytes,&decoded,(const uint8_t *)audio,strlen(audio))&&decoded==(size_t)bytes&&hex_digest(raw,decoded,hash)&&!strcmp(hash,expected);
    if(okay)okay=rpc_raw_body(key,raw,decoded,(size_t)offset,hash);
    free(raw);cJSON_Delete(data);return okay;
}
static bool native_block_ack_valid(size_t received,size_t expected,size_t prefix){
    // An identical block replayed inside a verified existing prefix returns
    // the receiver's full prefix length, not the end of this older block.
    return received==expected||(expected<=prefix&&received==prefix);
}
static size_t native_block_length(size_t available,size_t capacity,size_t offset,size_t prefix,bool finished){
    size_t count=available>capacity?capacity:available;
    if(count<capacity&&!finished)return 0;
    // A retained prefix can end between packet boundaries after a build or
    // interrupted final frame. Verify its tail before appending new bytes.
    if(offset<prefix&&count>prefix-offset)count=prefix-offset;
    return count;
}
static bool recover_transfer_frame(){
    portENTER_CRITICAL(&lock);
    bool damaged=rpc_error==-1||rpc_bad_frame;
    portEXIT_CRITICAL(&lock);
    if(!damaged||stopped()||direct_recoveries>=(section_mode.load()?32u:2u)||
       !muse_module_llm_recover_transfer_link())return false;
    direct_recoveries++;
    portENTER_CRITICAL(&lock);progress.recoveries=direct_recoveries;portEXIT_CRITICAL(&lock);
    return true;
}
static bool rpc_binary_block(const uint8_t *raw,size_t bytes,size_t offset){
    char hash[65];
    if(!raw||!bytes||bytes>8192||offset+bytes>32*1024*1024||!hex_digest(raw,bytes,hash))return false;
    for(unsigned attempt=0;attempt<3&&!stopped();attempt++){
        if(rpc_raw_body(job,raw,bytes,offset,hash)){
            portENTER_CRITICAL(&lock);size_t acknowledged=rpc_received;portEXIT_CRITICAL(&lock);
            if(native_block_ack_valid(acknowledged,offset+bytes,direct_resume_bytes))return true;
        }
        portENTER_CRITICAL(&lock);int code=rpc_error;size_t acknowledged=rpc_received;portEXIT_CRITICAL(&lock);
        printf("@native_rpc {\"op\":\"chunk\",\"attempt\":%u,\"code\":%d,\"received\":%u,\"expected\":%u}\n",attempt+1,code,(unsigned)acknowledged,(unsigned)(offset+bytes));fflush(stdout);
        if(attempt<2&&recover_transfer_frame()){
            printf("@native_direct {\"event\":\"recovering_transfer\",\"attempt\":%u,\"offset\":%u}\n",direct_recoveries,(unsigned)offset);fflush(stdout);
        }
        if(attempt<2)pause_ms(100*(attempt+1));
    }
    return false;
}
static bool rpc(cJSON *data,bool cleanup=false,unsigned attempts=3){
    bool ok=false;size_t expected_received=0;
    const char *op=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(data,"op"));
    if(op&&!strcmp(op,"chunk"))expected_received=(size_t)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(data,"offset"))+(size_t)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(data,"bytes"));
    unsigned limit=cleanup?1U:attempts;
    for(unsigned attempt=0;attempt<limit&&(!stopped()||cleanup);attempt++){
        cJSON *copy=cJSON_Duplicate(data,true);if(!copy)break;
        if(op&&!strcmp(op,"chunk")?rpc_raw_once(copy):rpc_once(copy,cleanup)){
            portENTER_CRITICAL(&lock);size_t acknowledged=rpc_received;portEXIT_CRITICAL(&lock);
            if(!expected_received || acknowledged==expected_received){ok=true;break;}
        }
        if(!cleanup){
            portENTER_CRITICAL(&lock);int code=rpc_error;size_t acknowledged=rpc_received;portEXIT_CRITICAL(&lock);
            printf("@native_rpc {\"op\":\"%s\",\"attempt\":%u,\"code\":%d,\"received\":%u,\"expected\":%u}\n",op?op:"unknown",attempt+1,code,(unsigned)acknowledged,(unsigned)expected_received);fflush(stdout);
            if(attempt+1<limit&&!recover_transfer_frame())pause_ms(200*(attempt+1));
        }
    }
    cJSON_Delete(data);return ok;
}
static cJSON *operation(const char *op){cJSON *d=cJSON_CreateObject();if(d){cJSON_AddStringToObject(d,"op",op);cJSON_AddStringToObject(d,"key",job);}return d;}
static bool finish_section(cJSON *request,unsigned index,size_t expected_prefix){
    bool complete=false;
    for(unsigned attempt=0;attempt<2&&!stopped();attempt++){
        cJSON *copy=cJSON_Duplicate(request,true);
        if(!copy)break;
        if(rpc(copy,false,1)){complete=true;break;}
        portENTER_CRITICAL(&lock);bool missed=rpc_error==-1||rpc_bad_frame;portEXIT_CRITICAL(&lock);
        if(!missed||stopped())break;
        recover_transfer_frame();
        if(rpc(operation("series_status"),false,1)){
            // The section may already have passed its audio and timing checks.
            // Use the Module's verified prefix to distinguish that from a
            // packet that never arrived, without sending the word data again.
            portENTER_CRITICAL(&lock);
            complete=rpc_ready_sections>index&&rpc_prefix_bytes>=expected_prefix;
            portEXIT_CRITICAL(&lock);
            if(complete)break;
        }else recover_transfer_frame();
    }
    cJSON_Delete(request);
    return complete;
}
static bool hex_digest(const uint8_t *data,size_t length,char out[65]){
    uint8_t digest[32];size_t written=0;if(psa_hash_compute(PSA_ALG_SHA_256,data,length,digest,sizeof(digest),&written)!=PSA_SUCCESS||written!=32)return false;
    for(unsigned i=0;i<32;i++){snprintf(out+2*i,3,"%02x",digest[i]);}
    return true;
}
static bool publish_sections_ready(){
    if(!rpc(operation("series_status")))return false;
    portENTER_CRITICAL(&lock);bool ready=rpc_ready;portEXIT_CRITICAL(&lock);
    if(ready){phase(4);state.store(1);}
    return true;
}
static void section_detail(unsigned detail){
    portENTER_CRITICAL(&lock);progress.detail=detail;portEXIT_CRITICAL(&lock);
}

struct SectionWait {
    static constexpr int64_t full_us=18000000, quiet_us=2500000, recheck_us=5000000;
    int64_t started,until;
    SectionWait(int64_t at,bool quiet):started(at),until(at+(quiet?quiet_us:full_us)){}
    bool expired(int64_t at) const{return at>=until;}
    bool rechecking(int64_t at) const{return until-started==quiet_us||at-started>=recheck_us;}
    int64_t request_deadline(int64_t at) const{return at+quiet_us<until?at+quiet_us:until;}
};

static bool transfer_sections(const char *source_hash,size_t source_bytes,unsigned sections,unsigned &stage,bool &retryable,bool source_failed){
    section_mode.store(true);auto &block=file_batch;size_t count=0,at=0,total=0;unsigned ready_words=0;
    portENTER_CRITICAL(&lock);if(!progress.source_ready_ms)progress.source_ready_ms=(unsigned)((now()-started_us)/1000);progress.detail=1;portEXIT_CRITICAL(&lock);
    stage=1;
    while(at<source_bytes&&!stopped()){
        size_t wanted=source_bytes-at;if(wanted>FILE_BATCH)wanted=FILE_BATCH;
        int status=read_file("source.md",at,block,wanted,count);
        if((status!=200&&status!=206)||!count||count>source_bytes-at)return false;
        memcpy(script+at,block,count);at+=count;
    }
    script[at]=0;char actual[65];
    if(at!=source_bytes||memchr(script,0,at)||!hex_digest((const uint8_t *)script,at,actual)||strcmp(actual,source_hash)){retryable=false;return false;}
    int status=read_file("plan.json",0,block,FILE_BATCH,count);
    if((status!=200&&status!=206)||!count)return false;
    block[count]=0;cJSON *plan=cJSON_Parse((char *)block);
    const char *hash=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(plan,"source_sha256"));
    if(!plan||!hash||strcmp(hash,source_hash)||cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(plan,"sections"))!=(int)sections){cJSON_Delete(plan);retryable=false;return false;}
    unsigned words=0,opening=0;unsigned short word_counts[128]={0};
    cJSON *parts=cJSON_GetObjectItemCaseSensitive(plan,"sections");
    for(unsigned i=0;i<sections;i++){
        cJSON *part=cJSON_GetArrayItem(parts,i);double n=cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(part,"word_count"));
        if(n<1||n>36||n!=(unsigned)n){cJSON_Delete(plan);retryable=false;return false;}
        word_counts[i]=(unsigned)n;words+=(unsigned)n;
        if(!opening&&i>=1&&words>=36&&cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(part,"sentence_end")))opening=words;
    }
    portENTER_CRITICAL(&lock);progress.words_total=words;progress.opening_words=opening?opening:words;progress.sections_total=sections;portEXIT_CRITICAL(&lock);
    cJSON *begin=operation("series_begin");cJSON_AddStringToObject(begin,"text",script);cJSON_AddItemToObject(begin,"plan",plan);
    // rpc consumes the parsed plan before the next network read, returning its
    // small JSON allocations to the internal-memory reserve immediately.
    if(!rpc(begin))return false;
    portENTER_CRITICAL(&lock);
    bool cached=rpc_complete,opening_ready=rpc_ready;
    unsigned prefix=rpc_ready_sections,offline_prefix=rpc_offline_sections;size_t prefix_bytes=rpc_prefix_bytes;
    portEXIT_CRITICAL(&lock);
    // Older Modules omit prefix_bytes. They still work through the existing
    // per-section cache checks, and the final seal checks every section hash.
    if(prefix>sections||prefix_bytes<(size_t)(prefix-offline_prefix)*144||prefix_bytes>(size_t)(prefix-offline_prefix)*120000){
        prefix=0;offline_prefix=0;prefix_bytes=0;
    }
    bool mixed=offline_prefix!=0;
    for(unsigned i=0;i<prefix;i++)ready_words+=word_counts[i];
    total=prefix_bytes;buffered(total,0);
    portENTER_CRITICAL(&lock);progress.sections_ready=prefix;progress.words_ready=ready_words;progress.audio_ready_ms=(unsigned)(total/4);portEXIT_CRITICAL(&lock);
    if(opening_ready){phase(4);state.store(1);}
    if(cached&&opening_ready){sections_complete.store(true);return true;}
    bool source_quiet=false;
    for(unsigned index=prefix;index<sections&&!stopped();index++){
        section_detail(MUSE_NATIVE_SECTION_WAITING);
        char name[40],expected[65]={0};size_t bytes=0;bool found=false,use_offline=false;
        snprintf(name,sizeof(name),"part-%03u.json",index);stage=2;
        SectionWait wait(now(),source_quiet);
        int64_t next_check=now()+3000000;
        while(!wait.expired(now())&&!stopped()){
            if(wait.rechecking(now()))section_detail(MUSE_NATIVE_SECTION_RECHECKING);
            status=read_file_once(name,0,block,4096,count,wait.request_deadline(now()));
            if(status==401||status==403){retryable=false;return false;}
            if(status==404){
                if(!source_failed&&now()>=next_check){
                    next_check=now()+3000000;
                    int metadata=read_file_once("meta.json",0,block,4096,count,wait.request_deadline(now()));
                    if((metadata==200||metadata==206)&&count){
                        block[count]=0;cJSON *m=cJSON_Parse((char *)block);
                        const char *phase=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m,"state"));
                        const char *sha=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m,"source_sha256"));
                        source_failed=phase&&!strcmp(phase,"error")&&sha&&!strcmp(sha,source_hash);
                        cJSON_Delete(m);
                    }
                }
                if(source_failed){use_offline=true;break;}
                pause_ms(250);continue;
            }
            if((status!=200&&status!=206)||!count){pause_ms(250);continue;}
            block[count]=0;memcpy(section_metadata,block,count+1);
            cJSON *m=cJSON_Parse(section_metadata);
            const char *phase_name=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m,"state"));
            const char *sha=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m,"audio_sha256"));
            double size=cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(m,"audio_bytes"));
            const cJSON *part_index=cJSON_GetObjectItemCaseSensitive(m,"index");
            bool same_index=cJSON_IsNumber(part_index)&&cJSON_GetNumberValue(part_index)==index;
            bool error=phase_name&&!strcmp(phase_name,"error")&&same_index;
            found=phase_name&&!strcmp(phase_name,"complete")&&cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m,"aligned"))&&
                same_index&&
                sha&&strlen(sha)==64&&size>=144&&size<=120000&&size==(size_t)size;
            if(found){bytes=(size_t)size;strlcpy(expected,sha,sizeof(expected));}
            cJSON_Delete(m);
            if(error){use_offline=true;break;}
            if(found){source_quiet=false;break;}
            pause_ms(250);
        }
        if(stopped())return false;
        if(!found&&!use_offline){
            // The producer can disappear before publishing a terminal record.
            // Probe later sections independently, but avoid another full wait
            // for each consecutive hole while no online section is arriving.
            source_quiet=true;use_offline=true;
            printf("@native_section_wait {\"index\":%u,\"event\":\"deadline\",\"wait_ms\":%u}\n",index+1,(unsigned)((now()-wait.started)/1000));fflush(stdout);
        }
        if(use_offline){
            section_detail(MUSE_NATIVE_SECTION_PIPER);
            static const char *speeds[]={"slow","normal","fast"};
            cJSON *request=operation("part_offline");cJSON_AddNumberToObject(request,"index",index);
            cJSON_AddStringToObject(request,"speed",speeds[selected_speed]);
            if(!rpc(request))return false;
            mixed=true;
        }else{
            section_detail(MUSE_NATIVE_SECTION_DOWNLOADING);
            cJSON *request=operation("part_begin");cJSON_AddNumberToObject(request,"index",index);
            cJSON_AddNumberToObject(request,"bytes",bytes);cJSON_AddStringToObject(request,"sha256",expected);
            cJSON_AddBoolToObject(request,"resume",true);
            if(!rpc(request,false,2))return false;
            portENTER_CRITICAL(&lock);size_t received=rpc_received;bool complete=rpc_complete;portEXIT_CRITICAL(&lock);
            if(received>bytes||(complete&&received!=bytes))return false;
            if(!complete){
                size_t offset=received;stage=3;snprintf(name,sizeof(name),"part-%03u.mp3",index);
                while(offset<bytes&&!stopped()){
                    size_t wanted=bytes-offset;if(wanted>FILE_BATCH)wanted=FILE_BATCH;
                    status=read_file(name,offset,block,wanted,count);
                    if((status!=200&&status!=206)||!count||count>bytes-offset)return false;
                    for(size_t sent=0;sent<count;){
                        size_t portion=count-sent;if(portion>1536)portion=1536;
                        if(!rpc_binary_block(block+sent,portion,offset+sent))return false;
                        sent+=portion;
                    }
                    offset+=count;buffered(total+offset,0);
                }
                if(stopped())return false;
                section_detail(MUSE_NATIVE_SECTION_CHECKING);
                cJSON *m=cJSON_Parse(section_metadata);request=operation("part_end");
                cJSON_AddNumberToObject(request,"pcm_samples",cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(m,"pcm_samples")));
                cJSON_AddBoolToObject(request,"aligned",cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m,"aligned")));
                cJSON_AddItemToObject(request,"words",cJSON_DetachItemFromObjectCaseSensitive(m,"words"));cJSON_Delete(m);
                if(!finish_section(request,index,total+bytes))return false;
            }
        }
        total+=bytes;buffered(total,0);
        ready_words+=word_counts[index];
        portENTER_CRITICAL(&lock);progress.sections_ready=index+1;progress.words_ready=ready_words;progress.audio_ready_ms=(unsigned)((uint64_t)total/4);portEXIT_CRITICAL(&lock);
        if(state.load()!=1){
            portENTER_CRITICAL(&lock);bool known=rpc_ready_known,ready=rpc_ready;portEXIT_CRITICAL(&lock);
            if(ready){phase(4);state.store(1);}
            else if(!known&&!publish_sections_ready())return false;
        }
        printf("@native_section {\"index\":%u,\"count\":%u,\"bytes\":%u,\"opening_ready\":%s,\"offline\":%s}\n",index+1,sections,(unsigned)bytes,state.load()==1?"true":"false",use_offline?"true":"false");fflush(stdout);
    }
    stage=4;
    if(mixed){
        if(stopped()||!rpc(operation("series_end_mixed")))return false;
        sections_complete.store(true);buffered(total,total);
        return state.load()==1||(publish_sections_ready()&&state.load()==1);
    }
    int64_t deadline=now()+90000000;bool sealed=false;
    while(now()<deadline&&!stopped()){
        status=read_file("meta.json",0,block,4096,count);
        if(status!=200&&status!=206)return false;
        block[count]=0;cJSON *m=cJSON_Parse((char *)block);
        const char *phase_name=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m,"state"));
        const char *sha=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m,"audio_sha256"));
        bool error=phase_name&&!strcmp(phase_name,"error");
        sealed=phase_name&&!strcmp(phase_name,"complete")&&sha&&strlen(sha)==64&&
            cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(m,"audio_bytes"))==total;
        cJSON *request=nullptr;
        if(sealed){request=operation("series_end");cJSON_AddNumberToObject(request,"bytes",total);cJSON_AddStringToObject(request,"sha256",sha);}
        cJSON_Delete(m);
        if(error){retryable=false;return false;}
        if(request){if(!rpc(request))return false;break;}
        pause_ms(150);
    }
    if(!sealed||stopped())return false;
    sections_complete.store(true);buffered(total,total);
    return state.load()==1||(publish_sections_ready()&&state.load()==1);
}
static bool transfer_once(unsigned &stage,bool &retryable){
    bool ready=false,complete=false;
    // One worker owns the PSRAM batch. Keep large data off its task stack.
    auto &block=file_batch;size_t count=0,offset=0;stage=0;retryable=true;
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
        double sections=cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(m,"section_count"));
        bool source_failed=phase&&!strcmp(phase,"error");
        if(identity&&cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(m,"version"))==2&&phase&&
           (!strcmp(phase,"sections")||!strcmp(phase,"complete")||source_failed)&&hash&&strlen(hash)==64&&
           bytes>=1&&bytes<=8192&&bytes==(size_t)bytes&&sections>=1&&sections<=128&&sections==(unsigned)sections){
            strlcpy(expected_source,hash,65);source_bytes=(size_t)bytes;cJSON_Delete(m);
            return transfer_sections(expected_source,source_bytes,(unsigned)sections,stage,retryable,source_failed);
        }
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
    while(ready&&!stopped()&&now()<until){stage=3;
        if(audio_bytes&&offset>=audio_bytes){complete=offset==audio_bytes;ready=complete;break;}
        size_t wanted=FILE_BATCH;if(audio_bytes&&audio_bytes-offset<wanted)wanted=audio_bytes-offset;
        int status=read_file("audio.mp3",offset,block,wanted,count);
        if(status==200||status==206){
            if(!count||offset+count>32*1024*1024){ready=false;break;}
            size_t sent=0;while(sent<count){size_t portion=count-sent>1536?1536:count-sent;
            if(!rpc_binary_block(block+sent,portion,offset+sent)){ready=false;break;}
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
struct SectionRetryBudget {
    unsigned checkpoint=0,stalled=0,total=0;
    bool allow(unsigned verified){
        if(verified>checkpoint){checkpoint=verified;stalled=0;}
        if(stalled>=2||total>=16)return false;
        ++stalled;++total;
        return true;
    }
};

static void worker(void *){
    file_requests=0;profile_wire_us=profile_ack_us=0;profile_packets=0;
    unsigned stage=0;bool retryable=true,ready=false;
    const int64_t legacy_recovery_deadline=now()+360000000;
    SectionRetryBudget budget;
    for(unsigned attempt=0;!stopped();attempt++){
        if(attempt){
            portENTER_CRITICAL(&lock);if(attempt>progress.recoveries)progress.recoveries=attempt;portEXIT_CRITICAL(&lock);
            phase(7,stage+1);
            printf("@native_recovery {\"attempt\":%u,\"stage\":%u}\n",attempt,stage);fflush(stdout);
            // Rejoin the existing paired Module session. Never reboot the
            // device or request synthesis again. A new begin discovers the
            // receiver's verified prefix, including after a Module restart.
            bool sectional=section_mode.load();
            int64_t wait_until=now()+90000000;
            if(!sectional&&wait_until>legacy_recovery_deadline)wait_until=legacy_recovery_deadline;
            pause_ms(sectional?500+300*budget.stalled:1000*attempt);
            while(now()<wait_until&&!stopped()){
                muse_module_llm_status_t st;muse_module_llm_status(&st);
                if((st.state==MUSE_MODULE_LLM_READY||(section_mode.load()&&(st.state==MUSE_MODULE_LLM_SPEAKING||st.state==MUSE_MODULE_LLM_PREPARING)))&&muse_link_req_ready())break;
                pause_ms(100);
            }
        }
        if(stopped()||(attempt&&!section_mode.load()&&now()>=legacy_recovery_deadline))break;
        phase(attempt?7:1);
        ready=transfer_once(stage,retryable);
        if(ready||!retryable)break;
        if(section_mode.load()){
            portENTER_CRITICAL(&lock);unsigned verified=progress.sections_ready;portEXIT_CRITICAL(&lock);
            if(!budget.allow(verified))break;
        }else if(attempt>=2)break;
    }
    if(!ready){
        bool was_stopped=stopped();
        phase(was_stopped?6:5,was_stopped?9:stage+1);
        if(section_mode.load())rpc(operation("abort"),true);
        if(!was_stopped&&!stopped()){
            state.store(-1);
            if(section_mode.load())rpc(operation("series_abort"),true);
        }
    }
    printf("@native_batch {\"ok\":%s,\"file_requests\":%u,\"packets\":%u,\"wire_ms\":%u,\"ack_ms\":%u,\"elapsed_ms\":%u}\n",ready?"true":"false",file_requests,profile_packets,(unsigned)(profile_wire_us/1000),(unsigned)(profile_ack_us/1000),(unsigned)((now()-started_us)/1000));fflush(stdout);
    running.store(false);vTaskDeleteWithCaps(nullptr);
}

/* A bounded producer/consumer buffer applies backpressure to the encrypted
 * download. UART acknowledgements arrive on the independent Module task, so
 * draining never depends on a second request on the blocked network task.
 * Neither task runs on LVGL. Complete HTTP EOF + SHA + decoder success are
 * required before the existing disk-backed player is allowed to speak. */
static constexpr size_t DIRECT_CAP=256*1024;
EXT_RAM_BSS_ATTR static uint8_t direct_ring[DIRECT_CAP];
// The single job owner reuses this bounded PSRAM block, outside its task stack.
// A complete base64 request must fit comfortably below the Module's 4096-byte
// Linux TTY queue even if its reader is briefly descheduled. Wait for each
// checked acknowledgement before sending another packet.
EXT_RAM_BSS_ATTR static uint8_t direct_block[1536];
static size_t direct_read,direct_write,direct_used;
static int direct_status;
static bool direct_done,direct_failed;
static size_t direct_max_frame;
static uint32_t direct_token;
static char direct_message[128];
static void direct_cb(void *context,int status,const uint8_t *data,size_t length,bool end){
    uint32_t token=(uint32_t)(uintptr_t)context;size_t at=0;
    int64_t deadline=now()+90000000;
    portENTER_CRITICAL(&lock);if(token==direct_token&&length>direct_max_frame)direct_max_frame=length;portEXIT_CRITICAL(&lock);
    while(at<length){
        portENTER_CRITICAL(&lock);
        if(token!=direct_token){portEXIT_CRITICAL(&lock);return;}
        if(status)direct_status=status;
        if(stopped()||direct_failed){direct_failed=true;portEXIT_CRITICAL(&lock);return;}
        size_t take=length-at,room=DIRECT_CAP-direct_used;
        if(take>room)take=room;
        if(take>4096)take=4096;
        if(take>DIRECT_CAP-direct_write)take=DIRECT_CAP-direct_write;
        if(take){memcpy(direct_ring+direct_write,data+at,take);direct_write=(direct_write+take)%DIRECT_CAP;direct_used+=take;at+=take;}
        portEXIT_CRITICAL(&lock);
        if(!take){
            if(now()>deadline){portENTER_CRITICAL(&lock);if(token==direct_token)direct_failed=direct_done=true;portEXIT_CRITICAL(&lock);return;}
            pause_ms(2);
        }
    }
    portENTER_CRITICAL(&lock);
    if(token==direct_token){if(status)direct_status=status;if(status<0)direct_failed=true;if(end||status<0)direct_done=true;}
    portEXIT_CRITICAL(&lock);
}
static bool direct_transfer(bool legacy){
    profile_wire_us=profile_ack_us=0;profile_packets=0;int64_t source_wait_us=0;
    direct_failure.store(20); // cache/session preparation
    portENTER_CRITICAL(&lock);direct_status=0;portEXIT_CRITICAL(&lock);
    char encoded_id[400],path[512];size_t pos=0;
    for(const unsigned char *p=(const unsigned char *)direct_message;*p;p++){
        unsigned char c=*p;
        if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.')encoded_id[pos++]=(char)c;
        else{snprintf(encoded_id+pos,4,"%%%02X",c);pos+=3;}
    }encoded_id[pos]=0;
    snprintf(path,sizeof(path),"%s?message_id=%s",legacy?"/voice/tts-stream":"/api/voice/tts-stream",encoded_id);
    cJSON *begin=operation("begin");cJSON_AddStringToObject(begin,"text",script);
    if(!rpc(begin)){
        portENTER_CRITICAL(&lock);int error=rpc_error;portEXIT_CRITICAL(&lock);
        printf("@native_direct {\"event\":\"cache_begin_failed\",\"code\":%d}\n",error);fflush(stdout);return false;
    }
    char prefix_hash[65];
    portENTER_CRITICAL(&lock);bool cached=rpc_complete;size_t cached_bytes=rpc_received;strlcpy(prefix_hash,rpc_hash,sizeof(prefix_hash));portEXIT_CRITICAL(&lock);
    direct_resume_bytes=cached_bytes;
    if(cached&&cached_bytes>=128){direct_failure.store(0);buffered(cached_bytes,cached_bytes);phase(4);state.store(1);return true;}
    if(cached_bytes&&strlen(prefix_hash)!=64)return false;
    // The native endpoint may ignore Range. Re-read its prefix over the fast
    // encrypted link, verify it locally, then upload only the missing suffix.
    // Never join a different audio response onto a retained partial file.
    portENTER_CRITICAL(&lock);
    direct_read=direct_write=direct_used=direct_max_frame=0;direct_status=0;direct_done=direct_failed=false;uint32_t token=++direct_token;
    portEXIT_CRITICAL(&lock);
    const char *headers[]={"Accept","audio/mpeg",nullptr};
    direct_failure.store(21); // paired request could not open
    int64_t request=muse_link_req_open("GET",path,headers,true,direct_cb,(void *)(uintptr_t)token);
    if(!request)return false;
    direct_failure.store(22); // download/source error unless superseded below
    psa_hash_operation_t digest=PSA_HASH_OPERATION_INIT;
    bool okay=psa_hash_setup(&digest,PSA_ALG_SHA_256)==PSA_SUCCESS,finished=false;
    auto &block=direct_block;size_t offset=0;int status=0;
    int64_t deadline=now()+360000000,first=0;
    phase(cached_bytes?7:2);buffered(cached_bytes,0);
    while(okay&&!stopped()&&now()<deadline){
        size_t count=0;bool failed=false;
        portENTER_CRITICAL(&lock);
        status=direct_status;finished=direct_done;failed=direct_failed;
        count=native_block_length(direct_used,sizeof(block),offset,cached_bytes,finished);
        if(count){size_t a=count>DIRECT_CAP-direct_read?DIRECT_CAP-direct_read:count;memcpy(block,direct_ring+direct_read,a);if(count>a)memcpy(block+a,direct_ring,count-a);direct_read=(direct_read+count)%DIRECT_CAP;direct_used-=count;}
        portEXIT_CRITICAL(&lock);
        if(failed||(status&&!(status>=200&&status<300))){okay=false;break;}
        if(count){
            if(!first){first=now();printf("@native_direct {\"event\":\"first_audio\",\"ms\":%u}\n",(unsigned)((first-started_us)/1000));fflush(stdout);}
            if(offset+count>32*1024*1024||psa_hash_update(&digest,block,count)!=PSA_SUCCESS){direct_failure.store(24);okay=false;break;}
            if(offset<cached_bytes){
                offset+=count;
                if(offset==cached_bytes){
                    char actual[65];
                    if(!digest_snapshot(&digest,actual)||strcmp(actual,prefix_hash)){
                        // Existing end validation invalidates this changed prefix;
                        // the bounded next attempt must start clean, not mix files.
                        if(digest_snapshot(&digest,actual)){cJSON *invalid=operation("end");cJSON_AddNumberToObject(invalid,"bytes",cached_bytes);cJSON_AddStringToObject(invalid,"sha256",actual);rpc_once(invalid,true);}
                        printf("@native_direct {\"event\":\"prefix_changed\",\"bytes\":%u}\n",(unsigned)cached_bytes);fflush(stdout);
                        direct_failure.store(24);okay=false;break;
                    }
                    printf("@native_direct {\"event\":\"prefix_verified\",\"bytes\":%u,\"uploaded_again\":0}\n",(unsigned)cached_bytes);fflush(stdout);phase(2);
                }
                continue;
            }
            if(!rpc_binary_block(block,count,offset)){direct_failure.store(23);okay=false;break;}
            offset+=count;buffered(offset,0);
        }else if(finished)break;
        else{int64_t waiting=now();pause_ms(5);source_wait_us+=now()-waiting;}
    }
    portENTER_CRITICAL(&lock);++direct_token;portEXIT_CRITICAL(&lock);
    if(!finished||!okay||stopped())muse_link_req_cancel(request);
    okay=okay&&finished&&!stopped()&&status>=200&&status<300&&offset>=128;
    if(okay){
        direct_failure.store(24); // final hash/decoder admission
        phase(3);uint8_t binary[32];size_t written=0;char hash[65];
        okay=psa_hash_finish(&digest,binary,sizeof(binary),&written)==PSA_SUCCESS&&written==32;
        if(okay){for(unsigned i=0;i<32;i++)snprintf(hash+2*i,3,"%02x",binary[i]);cJSON *end=operation("end");cJSON_AddNumberToObject(end,"bytes",offset);cJSON_AddStringToObject(end,"sha256",hash);okay=rpc(end);}
    }
    psa_hash_abort(&digest);
    if(okay){direct_failure.store(0);buffered(offset,offset);phase(4);state.store(1);}
    else rpc(operation("abort"),true);
    printf("@native_direct {\"event\":\"download_end\",\"ok\":%s,\"status\":%d,\"bytes\":%u,\"max_frame\":%u,\"ms\":%u,\"wire_ms\":%u,\"ack_ms\":%u,\"source_wait_ms\":%u,\"packets\":%u}\n",okay?"true":"false",status,(unsigned)offset,(unsigned)direct_max_frame,(unsigned)((now()-started_us)/1000),(unsigned)(profile_wire_us/1000),(unsigned)(profile_ack_us/1000),(unsigned)(source_wait_us/1000),profile_packets);fflush(stdout);
    // Match the official client: only a 404 selects the legacy endpoint.
    return okay;
}
static bool native_direct_retryable(int status){return status!=400&&status!=401&&status!=403;}
static void direct_worker(void *){
    bool ready=false;
    for(unsigned attempt=0;attempt<2&&!stopped();attempt++){
        if(attempt){portENTER_CRITICAL(&lock);if(progress.recoveries<attempt)progress.recoveries=attempt;portEXIT_CRITICAL(&lock);phase(7);pause_ms(500);}
        ready=direct_transfer(false);
        portENTER_CRITICAL(&lock);int status=direct_status;portEXIT_CRITICAL(&lock);
        if(!ready&&status==404&&!stopped())ready=direct_transfer(true);
        if(ready)break;
        if(!native_direct_retryable(status))break; // access failures need attention, not a retry loop
    }
    if(!ready){phase(stopped()?6:5,direct_failure.load()?direct_failure.load():8);if(!stopped())state.store(-1);}
    running.store(false);vTaskDeleteWithCaps(nullptr);
}
bool muse_native_voice_begin_message(const char *message,const char *text,uint32_t gen){
    if(!message||!*message||strlen(message)>=sizeof(direct_message)||!text||!*text||strlen(text)>=sizeof(script))return false;
    bool expected=false;if(!running.compare_exchange_strong(expected,true))return false;
    generation=gen;section_mode.store(false);sections_complete.store(false);direct_recoveries=0;direct_resume_bytes=0;direct_failure.store(0);strlcpy(direct_message,message,sizeof(direct_message));strlcpy(script,text,sizeof(script));
    // Identity follows the immutable message and exact script, so Restart and
    // later boots can reuse its checked partial or complete local cache.
    if(!native_message_key(message,text,job)){running.store(false);return false;}
    cancelled.store(false);state.store(0);
    portENTER_CRITICAL(&lock);progress={};progress.phase=1;strlcpy(progress.job,job,sizeof(progress.job));started_us=now();answer_ready_us=0;portEXIT_CRITICAL(&lock);
    if(xTaskCreatePinnedToCoreWithCaps(direct_worker,"muse_direct",16384,nullptr,3,nullptr,tskNO_AFFINITY,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)!=pdPASS){running.store(false);state.store(-1);return false;}return true;
}
bool muse_native_voice_begin(const char *key,uint32_t gen,int voice,int speed){
    bool expected=false;if(!key||strlen(key)!=24||!running.compare_exchange_strong(expected,true))return false;
    for(const char *p=key;*p;p++)if(!((*p>='0'&&*p<='9')||(*p>='a'&&*p<='f'))){running.store(false);return false;}
    generation=gen;section_mode.store(false);sections_complete.store(false);direct_recoveries=0;direct_resume_bytes=0;direct_failure.store(0);selected_voice=voice==1?1:0;selected_speed=speed>=0&&speed<=2?speed:1;strlcpy(job,key,sizeof(job));script[0]=0;cancelled.store(false);state.store(0);
    portENTER_CRITICAL(&lock);progress={};progress.phase=1;strlcpy(progress.job,key,sizeof(progress.job));started_us=now();answer_ready_us=0;portEXIT_CRITICAL(&lock);
    if(xTaskCreatePinnedToCoreWithCaps(worker,"muse_native",16384,nullptr,3,nullptr,tskNO_AFFINITY,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)!=pdPASS){running.store(false);state.store(-1);return false;}return true;
}
int muse_native_voice_copy(uint32_t gen,char *text,size_t capacity,char key[25]){
    if(gen!=generation)return -2;
    int value=state.load();if(value==1){if(strlen(script)>=capacity)return -1;strlcpy(text,script,capacity);strlcpy(key,job,25);}return value;
}
bool muse_native_voice_has_pending_sections(){return section_mode.load()&&!sections_complete.load();}
void muse_native_voice_cancel(){cancelled.store(true);}

bool muse_native_voice_rebind(uint32_t previous,uint32_t current){if(generation!=previous||cancelled.load()||state.load()<0)return false;generation=current;return true;}
void muse_native_voice_status(muse_native_voice_status_t *out){if(!out)return;portENTER_CRITICAL(&lock);*out=progress;out->elapsed_ms=started_us?(unsigned)((now()-started_us)/1000):0;portEXIT_CRITICAL(&lock);out->active=running.load();}
static const char *native_failure_title(unsigned failure,int status){
    if((failure==21||failure==22)&&status==429)return "MUSE VOICE BUSY - RETRY";
    if((failure==21||failure==22)&&(status==401||status==403))return "VOICE ACCESS DENIED";
    if((failure==21||failure==22)&&status==404)return "MUSE VOICE UNAVAILABLE";
    switch(failure){
    case 20:return "VOICE CACHE - RESTART";
    case 21:return "VOICE LINK - RESTART";
    case 22:return "VOICE DOWNLOAD - RESTART";
    case 23:return "VOICE TRANSFER - RESTART";
    case 24:return "AUDIO CHECK - RESTART";
    default:return "VOICE FAILED - RESTART";
    }
}
const char *muse_native_voice_error_title(void){portENTER_CRITICAL(&lock);int status=direct_status;portEXIT_CRITICAL(&lock);return native_failure_title(direct_failure.load(),status);}
void muse_native_voice_answer_ready(uint32_t gen,bool pending){if(gen!=generation)return;portENTER_CRITICAL(&lock);if(pending)answer_ready_us=0;else if(!answer_ready_us)answer_ready_us=now();portEXIT_CRITICAL(&lock);}
