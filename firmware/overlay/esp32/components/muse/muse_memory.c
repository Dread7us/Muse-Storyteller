#include "esp_attr.h"
/* SPDX-License-Identifier: Apache-2.0 */
#include "muse_memory.h"
#include "muse_module_llm.h"
#include "muse_link.h"
#include "muse_chat.h"
#include "muse_mem.h"
#include "muse_state.h"
#include "muse_conversation.h"
#include "driver/uart.h"
#include "esp_random.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct {
    char op[12],chat[33],key[25],title_token[25];
    char *text;
    unsigned page,offset,retries;
    bool user,restarted;
    uint32_t context_serial;
} job_t;
static QueueHandle_t s_jobs;
static SemaphoreHandle_t s_mutex;
EXT_RAM_BSS_ATTR static muse_memory_view_t s_view;
EXT_RAM_BSS_ATTR static muse_usage_view_t s_usage;
EXT_RAM_BSS_ATTR static muse_recorder_view_t s_recorder;
EXT_RAM_BSS_ATTR static muse_sd_view_t s_sd;
static bool s_sd_inflight;
void muse_memory_sd_copy(muse_sd_view_t *out)
{
    if(!s_mutex){memset(out,0,sizeof(*out));return;}
    xSemaphoreTake(s_mutex,portMAX_DELAY);
    *out=s_sd;out->busy=out->busy||s_sd_inflight;
    xSemaphoreGive(s_mutex);
}
static bool s_recorder_starting;
void muse_memory_recorder_copy(muse_recorder_view_t *out){if(!out)return;memset(out,0,sizeof(*out));if(s_mutex){xSemaphoreTake(s_mutex,portMAX_DELAY);*out=s_recorder;out->active=out->active||s_recorder_starting;xSemaphoreGive(s_mutex);}}
bool muse_memory_recorder_active(void){if(!s_mutex)return false;xSemaphoreTake(s_mutex,portMAX_DELAY);bool active=s_recorder.active||s_recorder_starting;xSemaphoreGive(s_mutex);return active;}

static int64_t s_usage_poll=5000000;
static volatile bool s_usage_soon;
static int64_t s_usage_auto_due=45000000;
static struct {bool valid;unsigned used;char reset[20];} s_usage_prediction;
void muse_memory_usage_poll(void){s_usage_soon=true;}
bool muse_memory_usage_copy(muse_usage_view_t *out)
{
    if(!s_mutex || !out)return false;
    xSemaphoreTake(s_mutex,portMAX_DELAY);*out=s_usage;xSemaphoreGive(s_mutex);
    return out->valid;
}
static job_t *s_job;
static char s_request[64],s_session[9];
static char *s_page,*s_assembly,*s_context;
static size_t s_assembled;
static uint32_t s_serial,s_context_request,s_context_done;
static unsigned s_sent_bytes,s_messages;
static int64_t s_deadline;
static bool s_waiting,s_context_ok;
static volatile bool s_switching;
bool muse_memory_switching(void){return s_switching;}
static bool integer(const cJSON *n,int low,int high)
{return cJSON_IsNumber(n) && isfinite(n->valuedouble) && n->valuedouble>=low && n->valuedouble<=high && n->valuedouble==(double)n->valueint;}

static void status(const char *text)
{
    xSemaphoreTake(s_mutex,portMAX_DELAY);
    strlcpy(s_view.status,text,sizeof(s_view.status));s_view.version++;
    xSemaphoreGive(s_mutex);
}
void muse_memory_init(void)
{
    if (s_mutex) return;
    static StaticSemaphore_t storage;
    s_mutex=xSemaphoreCreateMutexStatic(&storage);
    s_jobs=xQueueCreate(20,sizeof(job_t *));
    s_page=heap_caps_calloc(1,8192,MUSE_BIG_CAPS);
    s_assembly=heap_caps_calloc(1,8192,MUSE_BIG_CAPS);
    s_context=heap_caps_calloc(1,4800,MUSE_BIG_CAPS);
    s_recorder.pages=1;strlcpy(s_recorder.state,"idle",16);
    s_usage.refresh_supported=true;
    s_view.resumed=true; /* restore context from the durable active draft after reboot */
    status("Local drafts / Save to bookmark");
}
static bool enqueue(job_t *job)
{
    if (job && s_jobs && s_page && s_assembly && s_context && xQueueSend(s_jobs,&job,0)==pdTRUE) return true;
    if(job){free(job->text);free(job);}
    if(s_mutex)status("Archive busy; message not saved");
    return false;
}
void muse_memory_record(bool user,const char *text)
{
    if(!s_mutex || !text || !text[0] || strlen(text)>8192)return;
    job_t *j=calloc(1,sizeof(*j));if(!j)return;
    strcpy(j->op,"put");j->user=user;
    snprintf(j->key,sizeof(j->key),"%08lx%08lx%08lx",(unsigned long)esp_random(),(unsigned long)esp_random(),(unsigned long)esp_random());
    j->text=heap_caps_malloc(strlen(text)+1,MUSE_BIG_CAPS);
    if(!j->text){free(j);status("Archive memory unavailable");return;}
    strcpy(j->text,text);enqueue(j);
}
static void usage_outcome(const char *state,const char *note)
{
    xSemaphoreTake(s_mutex,portMAX_DELAY);
    s_usage.refresh_pending=!strcmp(state,"checking");
    strlcpy(s_usage.refresh_state,state,sizeof(s_usage.refresh_state));
    strlcpy(s_usage.refresh_note,note,sizeof(s_usage.refresh_note));
    xSemaphoreGive(s_mutex);
}

static portMUX_TYPE usage_http_lock=portMUX_INITIALIZER_UNLOCKED;
static struct {char text[257];size_t length;int status;bool done,overflow;} usage_http;
static uint32_t usage_http_token;
static void usage_http_reply(void *ctx,int status,const uint8_t *data,size_t size,bool end)
{
    portENTER_CRITICAL(&usage_http_lock);
    if((uint32_t)(uintptr_t)ctx==usage_http_token){
        if(status)usage_http.status=status;
        if(data&&size){
            if(size>sizeof(usage_http.text)-1-usage_http.length)usage_http.overflow=true;
            else {memcpy(usage_http.text+usage_http.length,data,size);usage_http.length+=size;}
        }
        if(end||status<0)usage_http.done=true;
    }
    portEXIT_CRITICAL(&usage_http_lock);
}

/* Run only in the usage task. Bounded replies and tokens fence late callbacks. */
static int usage_request(const char *path,const char *post,char out[257])
{
    const char *headers[]={"Content-Type","application/json",NULL};
    portENTER_CRITICAL(&usage_http_lock);
    memset(&usage_http,0,sizeof(usage_http));uint32_t token=++usage_http_token;
    portEXIT_CRITICAL(&usage_http_lock);
    int64_t id=muse_link_req_open(post?"POST":"GET",path,post?headers:NULL,!post,usage_http_reply,(void *)(uintptr_t)token);
    if(!id)return -1;
    bool sent=!post||muse_link_req_send(id,post,strlen(post),true,1000);
    const int64_t limit=esp_timer_get_time()+4000000;
    int result=-1;bool done=false;
    while(sent&&esp_timer_get_time()<limit){
        portENTER_CRITICAL(&usage_http_lock);
        done=usage_http.done;
        if(done&&(!out||!usage_http.overflow)){
            result=usage_http.status;
            if(out)memcpy(out,usage_http.text,usage_http.length+1);
        }
        portEXIT_CRITICAL(&usage_http_lock);
        if(done)break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if(!done)muse_link_req_cancel(id);
    return result;
}

extern const char *identity_node_id(void);
static void usage_worker(void *unused)
{
    const char *error="Muse did not return a fresh reading.";
    char nonce[33],path[100],prompt[1024],thread[37],out[257];
    uint8_t mac[6];
    for(unsigned i=0;i<4;i++)snprintf(nonce+8*i,9,"%08lx",(unsigned long)esp_random());
    if(!muse_link_req_ready()||esp_read_mac(mac,ESP_MAC_WIFI_STA)!=ESP_OK){
        error="Connect Muse to Wi-Fi and retry.";goto done;
    }
    snprintf(thread,sizeof(thread),"7e8eb80c-2b28-4721-aeca-%02x%02x%02x%02x%02x%02x",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    snprintf(path,sizeof(path),"/fs/raw/muse-storyteller/usage/%s.json",nonce);
    snprintf(prompt,sizeof(prompt),
      "Muse Storyteller usage check. Call your own live, read-only subscription-status tool for this exact consumer Muse account. "
      "Use only its current weekly whole percent used and reset date, in UTC as shown by Muse Settings. Never infer, round, or reuse an earlier reply. "
      "Create the directory if needed and atomically save JSON at ~/muse-storyteller/usage/%s.json. "
      "On tool success write {\"nonce\":\"%s\",\"state\":\"verified\",\"used\":PERCENT,\"reset\":\"Mon D\",\"observed_at\":UNIX_SECONDS}. "
      "Use the current VM clock at the check. If the tool is unavailable, write the same nonce with state unavailable and omit all values. "
      "Do not browse, change billing/settings, read chats or credentials, or create audio. Reply briefly when saved.",nonce,nonce);
    cJSON *body=cJSON_CreateObject();
    if(!body)goto done;
    cJSON_AddStringToObject(body,"message",prompt);cJSON_AddStringToObject(body,"session_id",thread);
    cJSON_AddStringToObject(body,"device_id",identity_node_id());cJSON_AddStringToObject(body,"output_modality","text");
    char *post=cJSON_PrintUnformatted(body);cJSON_Delete(body);
    if(!post)goto done;
    int result=usage_request("/chat/stream",post,NULL);cJSON_free(post);
    if(result>0&&(result<200||result>=300)){error="Muse could not start the allowance check.";goto done;}
    // A lost acknowledgement can follow an accepted request. Poll the unique
    // result instead of submitting a duplicate request.
    const int64_t until=esp_timer_get_time()+65000000;
    while(esp_timer_get_time()<until){
        vTaskDelay(pdMS_TO_TICKS(1800));
        int code=usage_request(path,NULL,out);
        if(code==401||code==403){error="Muse could not read the check result.";break;}
        if(code!=200&&code!=206)continue;
        cJSON *reading=cJSON_ParseWithOpts(out,NULL,true);
        const char *id=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(reading,"nonce"));
        const char *state=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(reading,"state"));
        if(!id||strcmp(id,nonce)||!state){cJSON_Delete(reading);continue;}
        if(strcmp(state,"verified")){cJSON_Delete(reading);error="Muse's allowance tool was unavailable.";break;}
        const cJSON *used=cJSON_GetObjectItemCaseSensitive(reading,"used"),*stamp=cJSON_GetObjectItemCaseSensitive(reading,"observed_at");
        const char *reset=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(reading,"reset"));
        char month[4]={0};unsigned day=0;int end=0;
        bool valid=integer(used,0,100)&&cJSON_IsNumber(stamp)&&stamp->valuedouble>=1700000000.&&stamp->valuedouble<4102444800.&&
            floor(stamp->valuedouble)==stamp->valuedouble&&reset&&strlen(reset)<=6&&
            sscanf(reset,"%3[A-Za-z] %u%n",month,&day,&end)==2&&reset[end]==0&&day>=1&&day<=31;
        if(!valid){cJSON_Delete(reading);error="Muse returned an invalid reading.";break;}
        cJSON *payload=cJSON_CreateObject();
        if(payload){
            char canonical_reset[8];snprintf(canonical_reset,sizeof(canonical_reset),"%s %u",month,day);
            cJSON_AddNumberToObject(payload,"used",used->valueint);
            cJSON_AddStringToObject(payload,"reset",canonical_reset);
            cJSON_AddNumberToObject(payload,"observed_at",stamp->valuedouble);
            if(s_usage_prediction.valid){
                cJSON *prediction=cJSON_AddObjectToObject(payload,"prediction");
                if(prediction){
                    cJSON_AddNumberToObject(prediction,"used",s_usage_prediction.used);
                    cJSON_AddStringToObject(prediction,"reset",s_usage_prediction.reset);
                }
            }else cJSON_AddNullToObject(payload,"prediction");
        }
        cJSON_Delete(reading);
        char *encoded=payload?cJSON_PrintUnformatted(payload):NULL;cJSON_Delete(payload);
        job_t *job=calloc(1,sizeof(*job));if(!job){cJSON_free(encoded);break;}
        strcpy(job->op,"usageapply");job->text=encoded?strdup(encoded):NULL;cJSON_free(encoded);
        if(job->text&&xQueueSend(s_jobs,&job,0)==pdTRUE){vTaskDeleteWithCaps(NULL);return;}
        free(job->text);free(job);error="Device is busy. Retry in a moment.";break;
    }
done:
    usage_outcome("error",error);vTaskDeleteWithCaps(NULL);
}

bool muse_memory_usage_check(void)
{
    if(!s_mutex||!s_jobs)return false;
    xSemaphoreTake(s_mutex,portMAX_DELAY);
    if(s_usage.refresh_pending){xSemaphoreGive(s_mutex);return false;}
    s_usage_prediction.valid=s_usage.valid;s_usage_prediction.used=s_usage.used;
    strlcpy(s_usage_prediction.reset,s_usage.reset,sizeof(s_usage_prediction.reset));
    s_usage.refresh_pending=true;
    strlcpy(s_usage.refresh_state,"checking",sizeof(s_usage.refresh_state));
    strlcpy(s_usage.refresh_note,"Muse is checking the weekly allowance.",sizeof(s_usage.refresh_note));
    xSemaphoreGive(s_mutex);
    if(xTaskCreatePinnedToCoreWithCaps(usage_worker,"muse_usage",6144,NULL,2,NULL,tskNO_AFFINITY,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)==pdPASS)return true;
    usage_outcome("error","Device is busy. Retry in a moment.");return false;
}
bool muse_memory_recorder_action(const char *op,unsigned page,unsigned level){
 if(!s_mutex || !op || (strcmp(op,"recstart")&&strcmp(op,"recstop")&&strcmp(op,"recstatus")&&strcmp(op,"recpage")&&strcmp(op,"recprev")&&strcmp(op,"recnext")))return false;
 job_t *j=calloc(1,sizeof(*j));if(!j)return false;strlcpy(j->op,op,sizeof(j->op));j->page=page;j->offset=level;
 if(!strcmp(op,"recstart")){xSemaphoreTake(s_mutex,portMAX_DELAY);if(s_recorder.active||s_recorder_starting){xSemaphoreGive(s_mutex);free(j);return false;}s_recorder_starting=true;strlcpy(s_recorder.note,"Preparing local recording...",96);xSemaphoreGive(s_mutex);}
 if(!strcmp(op,"recprev")||!strcmp(op,"recnext")){xSemaphoreTake(s_mutex,portMAX_DELAY);s_recorder.text[0]=0;xSemaphoreGive(s_mutex);}
 bool ok=enqueue(j);if(!ok && !strcmp(op,"recstart")){xSemaphoreTake(s_mutex,portMAX_DELAY);s_recorder_starting=false;xSemaphoreGive(s_mutex);}return ok;
}
bool muse_memory_sd_action(const char *action,const char *token,const char *name)
{
    if(!s_mutex || !action || strlen(action)>=25 || (token && strlen(token)>=33) || (name && strlen(name)>96))return false;
    job_t *job=calloc(1,sizeof(*job));
    if(!job)return false;
    strcpy(job->op,"sd");strcpy(job->key,action);
    if(token)strcpy(job->chat,token);
    if(name && !(job->text=strdup(name))){free(job);return false;}
    xSemaphoreTake(s_mutex,portMAX_DELAY);
    bool accepted=!s_sd_inflight && (!s_sd.busy || !strcmp(action,"status"));
    if(accepted)s_sd_inflight=true;
    xSemaphoreGive(s_mutex);
    if(!accepted){free(job->text);free(job);return false;}
    if(enqueue(job))return true;
    xSemaphoreTake(s_mutex,portMAX_DELAY);s_sd_inflight=false;xSemaphoreGive(s_mutex);
    return false;
}
bool muse_memory_action(const char *op,const char *chat,unsigned page)
{
    if(op && (!strcmp(op,"new")||!strcmp(op,"resume")))muse_chat_playback_clear();
    if(!s_mutex || !op || (strcmp(op,"save")&&strcmp(op,"list")&&strcmp(op,"page")&&strcmp(op,"resume")&&strcmp(op,"new")&&strcmp(op,"suggest")&&strcmp(op,"delete")))return false;
    bool change=!strcmp(op,"new") || !strcmp(op,"resume") || !strcmp(op,"delete");
    muse_mode_t mode=muse_state_mode(NULL);
    if(change && (s_switching || (mode!=MUSE_MODE_IDLE && mode!=MUSE_MODE_ERROR))){status("Finish speaking before switching chats");return false;}
    job_t *j=calloc(1,sizeof(*j));if(!j)return false;
    strlcpy(j->op,op,sizeof(j->op));strlcpy(j->chat,chat?chat:"",sizeof(j->chat));j->page=page;
    if(change)s_switching=true;
    bool ok=enqueue(j);if(!ok && change)s_switching=false;if(ok)status("Loading local conversation...");return ok;
}
bool muse_memory_rename(const char *chat,const char *token)
{
    if(!s_mutex || !chat || strlen(chat)!=32 || !token || strlen(token)!=24)return false;
    job_t *j=calloc(1,sizeof(*j));if(!j)return false;
    strcpy(j->op,"rename");strlcpy(j->chat,chat,sizeof(j->chat));strlcpy(j->title_token,token,sizeof(j->title_token));
    bool ok=enqueue(j);if(ok)status("Updating local title...");return ok;
}
bool muse_memory_copy(muse_memory_view_t *out,char *text,size_t cap)
{
    if(!s_mutex || !s_page || !out || !text || cap<8192)return false;
    xSemaphoreTake(s_mutex,portMAX_DELAY);
    bool changed=out->version!=s_view.version;
    if(changed){*out=s_view;strlcpy(text,s_page,cap);}
    xSemaphoreGive(s_mutex);return changed;
}
static void finish(bool ok,const char *message)
{
    if(s_job && !strcmp(s_job->op,"sd")){
        xSemaphoreTake(s_mutex,portMAX_DELAY);
        s_sd_inflight=false;
        if(!ok){s_sd.busy=false;s_sd.token[0]=s_sd.confirm[0]=0;strlcpy(s_sd.note,message?message:"Module did not confirm; tap Refresh",sizeof(s_sd.note));s_sd.version++;}
        xSemaphoreGive(s_mutex);message=NULL;
    }
    if(s_job && !strcmp(s_job->op,"usageapply")){
        usage_outcome(ok?"done":"error",ok?"Muse reading updated.":"Module could not apply the reading.");message=NULL;
    }
    if(s_job && !strncmp(s_job->op,"rec",3)){
        xSemaphoreTake(s_mutex,portMAX_DELAY);
        if(!strcmp(s_job->op,"recstart")){if(!ok && !s_waiting)s_recorder.active=true;s_recorder_starting=false;}
        if(!ok){strlcpy(s_recorder.note,"Module did not confirm; retry or check its power",96);s_recorder.version++;}
        xSemaphoreGive(s_mutex);message=NULL;
    }
    if(s_job && (!strcmp(s_job->op,"new") || !strcmp(s_job->op,"resume") || !strcmp(s_job->op,"delete")))s_switching=false;
    if(s_job && !strcmp(s_job->op,"context")){
        xSemaphoreTake(s_mutex,portMAX_DELAY);
        s_context_ok=ok;s_context_done=s_job->context_serial;
        if(ok)strlcpy(s_context,s_assembly,4800);
        xSemaphoreGive(s_mutex);
    }
    if(s_job){free(s_job->text);free(s_job);s_job=NULL;}
    s_waiting=false;s_assembled=0;s_request[0]=0;
    if(message)status(message);
}
static unsigned utf8_piece(const char *text,unsigned offset)
{
    size_t remain=strlen(text+offset);unsigned n=remain<256?(unsigned)remain:256;
    if(n<remain)while(n && (((unsigned char)text[offset+n]&0xc0)==0x80))n--;
    return n;
}
void muse_memory_tick(const char *session,bool idle)
{
    if(!s_jobs || !s_page || !s_assembly || !s_context)return;
    if(s_waiting && strcmp(session,s_session)) {s_waiting=false;s_deadline=0;}
    if(s_waiting && esp_timer_get_time()<s_deadline)return;
    if(!idle)return; /* audio, recording and their control frames always win */
    if(s_usage_soon){s_usage_soon=false;s_usage_poll=0;}
    if(s_waiting){
        s_waiting=false;
        if(++s_job->retries>2){finish(false,"Archive unavailable; live conversation still works");return;}
    }
    if(!s_job){
        if(xQueueReceive(s_jobs,&s_job,0)!=pdTRUE){
            int64_t now=esp_timer_get_time();
            if(now>=s_usage_auto_due&&muse_link_req_ready()){
                s_usage_auto_due=now+600000000LL;
                muse_usage_view_t account={0};
                bool valid=muse_memory_usage_copy(&account);
                uint32_t age=account.age+((uint32_t)(now/1000)-account.received_ms)/1000;
                if((!valid||age>=86400)&&muse_memory_usage_check())s_usage_auto_due=now+86400000000LL;
            }
            if(esp_timer_get_time()<s_usage_poll)return;
            s_usage_poll=esp_timer_get_time()+60000000;
            s_job=calloc(1,sizeof(*s_job));if(!s_job)return;strcpy(s_job->op,"usage");
        }
        s_assembled=0;s_assembly[0]=0;
    }
    strlcpy(s_session,session,sizeof(s_session));
    cJSON *root=cJSON_CreateObject(),*d=cJSON_CreateObject();
    if(!root || !d){cJSON_Delete(root);cJSON_Delete(d);finish(false,"Archive memory unavailable");return;}
    /* Retries repeat an immutable request identity and payload. */
    if(!s_request[0])snprintf(s_request,sizeof(s_request),"mem-%s-%08lx",session,(unsigned long)++s_serial);
    cJSON_AddStringToObject(root,"request_id",s_request);
    cJSON_AddStringToObject(root,"work_id","musevoice");cJSON_AddStringToObject(root,"object","musevoice.v1");
    cJSON_AddStringToObject(root,"action","memory");cJSON_AddItemToObject(root,"data",d);
    cJSON_AddStringToObject(d,"session",session);cJSON_AddNumberToObject(d,"generation",0);
    cJSON_AddStringToObject(d,"op",s_job->op);
    if(!strcmp(s_job->op,"delete"))cJSON_AddBoolToObject(d,"confirm",true);
    if(!strcmp(s_job->op,"rename"))cJSON_AddStringToObject(d,"token",s_job->title_token);
    if(!strcmp(s_job->op,"sd")){
        cJSON_AddStringToObject(d,"sd_action",s_job->key);
        if(s_job->chat[0])cJSON_AddStringToObject(d,"token",s_job->chat);
        if(s_job->text)cJSON_AddStringToObject(d,"name",s_job->text);
    }else if(s_job->chat[0])cJSON_AddStringToObject(d,"chat",s_job->chat);
    cJSON_AddNumberToObject(d,"page",s_job->page);
    cJSON_AddNumberToObject(d,"offset",!strcmp(s_job->op,"list")?s_job->page:s_job->offset);
    if(!strcmp(s_job->op,"put")){
        char part[257];s_sent_bytes=utf8_piece(s_job->text,s_job->offset);
        memcpy(part,s_job->text+s_job->offset,s_sent_bytes);part[s_sent_bytes]=0;
        cJSON_AddStringToObject(d,"key",s_job->key);cJSON_AddStringToObject(d,"role",s_job->user?"user":"assistant");
        cJSON_AddStringToObject(d,"part",part);cJSON_AddBoolToObject(d,"finish",!s_job->text[s_job->offset+s_sent_bytes]);
    }else if(!strcmp(s_job->op,"context"))cJSON_AddStringToObject(d,"query",s_job->text?s_job->text:"");
    else if(!strcmp(s_job->op,"usageapply"))cJSON_AddItemToObject(d,"observation",cJSON_Parse(s_job->text));
    char *json=cJSON_PrintUnformatted(root);cJSON_Delete(root);
    if(!json){finish(false,"Archive memory unavailable");return;}

    bool sent=muse_module_llm_send_line(json);
    cJSON_free(json);s_waiting=true;s_deadline=esp_timer_get_time()+(sent?8000000LL:100000LL);
}
static void sd_text(const cJSON *object,const char *key,char *out,size_t size)
{
    const char *value=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object,key));
    strlcpy(out,value && strlen(value)<size ? value : "",size);
}
static unsigned sd_number(const cJSON *object,const char *key)
{
    const cJSON *value=cJSON_GetObjectItemCaseSensitive(object,key);
    return integer(value,0,1000000000)?value->valueint:0;
}
static bool sd_flag(const cJSON *object,const char *key)
{return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(object,key));}
static bool sd_reply(const cJSON *result)
{
    const cJSON *sd=cJSON_GetObjectItemCaseSensitive(result,"sd");
    const cJSON *items=cJSON_GetObjectItemCaseSensitive(sd,"items");
    if(!cJSON_IsObject(sd) || !cJSON_IsArray(items))return false;
    xSemaphoreTake(s_mutex,portMAX_DELAY);
    s_sd.version++;
    s_sd.busy=sd_flag(sd,"busy");s_sd.mounted=sd_flag(sd,"mounted");s_sd.writable=sd_flag(sd,"writable");
    s_sd.formattable=sd_flag(sd,"format");s_sd.more=sd_flag(sd,"more");
    s_sd.page=sd_number(sd,"page");s_sd.count=sd_number(sd,"count");s_sd.total_mb=sd_number(sd,"total_mb");
    s_sd.free_mb=sd_number(sd,"free_mb");s_sd.done=sd_number(sd,"done");s_sd.total=sd_number(sd,"total");
    sd_text(sd,"state",s_sd.state,sizeof(s_sd.state));sd_text(sd,"note",s_sd.note,sizeof(s_sd.note));
    sd_text(sd,"path",s_sd.path,sizeof(s_sd.path));sd_text(sd,"token",s_sd.token,sizeof(s_sd.token));
    sd_text(sd,"confirm",s_sd.confirm,sizeof(s_sd.confirm));sd_text(sd,"detail",s_sd.detail,sizeof(s_sd.detail));
    sd_text(sd,"moving",s_sd.moving,sizeof(s_sd.moving));
    for(unsigned i=0;i<MUSE_SD_ROWS;i++){
        const cJSON *item=cJSON_GetArrayItem(items,i);
        s_sd.items[i].folder=sd_flag(item,"folder");
        sd_text(item,"name",s_sd.items[i].name,sizeof(s_sd.items[i].name));
        sd_text(item,"token",s_sd.items[i].token,sizeof(s_sd.items[i].token));
    }
    xSemaphoreGive(s_mutex);
    return true;
}
bool muse_memory_reply(const cJSON *root,const char *session)
{
    const char *id=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root,"request_id"));
    if(!id || strncmp(id,"mem-",4))return false;
    if(!s_job || !s_waiting || strcmp(id,s_request))return true;
    const cJSON *d=cJSON_GetObjectItemCaseSensitive(root,"data"),*error=cJSON_GetObjectItemCaseSensitive(root,"error");
    const char *boot=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(d,"session"));
    const char *ev=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(d,"event"));
    const char *work=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root,"work_id"));
    const char *object=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root,"object"));
    const cJSON *gen=cJSON_GetObjectItemCaseSensitive(d,"generation");
    const cJSON *protocol=cJSON_GetObjectItemCaseSensitive(d,"protocol");
    if(!boot || strcmp(boot,session) || !ev || strcmp(ev,"memory") || !work || strcmp(work,"musevoice") || !object || strcmp(object,"musevoice.event") || !integer(gen,0,0) || !integer(protocol,1,1))return true;
    const cJSON *code=cJSON_GetObjectItemCaseSensitive(error,"code");
    if(!cJSON_IsNumber(code) || code->valueint){
        if(!strcmp(s_job->op,"usage")){finish(false,NULL);return true;}
        if(!strcmp(s_job->op,"sd")){finish(false,"Card service unavailable; tap Refresh");return true;}
        s_request[0]=0;
        if(!strcmp(s_job->op,"put") && s_job->offset && !s_job->restarted){s_job->offset=0;s_job->restarted=true;s_waiting=false;return true;}
        finish(false,"Archive could not finish; try again");return true;
    }
    const cJSON *r=cJSON_GetObjectItemCaseSensitive(d,"result");
    if(!cJSON_IsObject(r))return true;
    s_waiting=false;s_request[0]=0;s_job->retries=0;
    if(!strcmp(s_job->op,"sd")){finish(sd_reply(r),NULL);return true;}
    bool rec_valid=false;
    const cJSON *rec=cJSON_GetObjectItemCaseSensitive(r,"recorder");
    if(cJSON_IsObject(rec)){
        const char *state=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(rec,"state")),*note=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(rec,"note")),*identity=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(rec,"id")),*text=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(rec,"text"));
        const cJSON *seconds=cJSON_GetObjectItemCaseSensitive(rec,"seconds"),*lines=cJSON_GetObjectItemCaseSensitive(rec,"lines"),*mb=cJSON_GetObjectItemCaseSensitive(rec,"audio_mb"),*page=cJSON_GetObjectItemCaseSensitive(rec,"page"),*pages=cJSON_GetObjectItemCaseSensitive(rec,"pages");
        bool safe=cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(rec,"active"))&&cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(rec,"supported"))&&cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(rec,"audio"))&&state&&strlen(state)<16&&note&&strlen(note)<96&&identity&&strlen(identity)<33&&text&&strlen(text)<1025&&integer(seconds,0,1000000000)&&integer(lines,0,1000000000)&&integer(mb,0,65535)&&integer(page,0,1000000)&&integer(pages,1,1000000)&&page->valueint<pages->valueint;
        if(safe){for(const char *p=state;*p;p++)if(*p<32||*p>126)safe=false;for(const char *p=note;*p;p++)if(*p<32||*p>126)safe=false;for(const char *p=identity;*p;p++)if(*p<32||*p>126)safe=false;}
        if(safe){rec_valid=true;xSemaphoreTake(s_mutex,portMAX_DELAY);s_recorder.supported=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(rec,"supported"));s_recorder.active=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(rec,"active"));s_recorder.audio=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(rec,"audio"));s_recorder.older=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(rec,"older"));s_recorder.newer=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(rec,"newer"));s_recorder.seconds=seconds->valueint;s_recorder.lines=lines->valueint;s_recorder.audio_mb=mb->valueint;s_recorder.page=page->valueint;s_recorder.pages=pages->valueint;strlcpy(s_recorder.state,state,16);strlcpy(s_recorder.note,note,96);strlcpy(s_recorder.id,identity,33);if(!strncmp(s_job->op,"rec",3)&&strcmp(s_job->op,"recstatus"))strlcpy(s_recorder.text,text,1025);s_recorder.version++;xSemaphoreGive(s_mutex);}
    }
    if(!strncmp(s_job->op,"rec",3)){finish(rec_valid,NULL);return true;}
    const cJSON *u=cJSON_GetObjectItemCaseSensitive(r,"usage");
    const cJSON *used=cJSON_GetObjectItemCaseSensitive(u,"used"),*observed=cJSON_GetObjectItemCaseSensitive(u,"observed"),*age=cJSON_GetObjectItemCaseSensitive(u,"age");
    const char *reset=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(u,"reset")),*checked=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(u,"checked"));
    if(integer(used,0,100) && integer(observed,0,100) && integer(age,0,604800) && reset && checked && strlen(reset)<20 && strlen(checked)<24){
        bool safe=true;for(const char *p=reset;*p;p++)if(*p<32 || *p>126)safe=false;for(const char *p=checked;*p;p++)if(*p<32 || *p>126)safe=false;
        if(safe){
            xSemaphoreTake(s_mutex,portMAX_DELAY);s_usage.valid=true;s_usage.used=used->valueint;s_usage.observed=observed->valueint;s_usage.age=age->valueint;s_usage.received_ms=(uint32_t)(esp_timer_get_time()/1000);
            s_usage.learned=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(u,"learned"));s_usage.pending=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(u,"pending"));strlcpy(s_usage.reset,reset,sizeof(s_usage.reset));strlcpy(s_usage.checked,checked,sizeof(s_usage.checked));
            const cJSON *accuracy=cJSON_GetObjectItemCaseSensitive(u,"accuracy");
            const cJSON *count=cJSON_GetObjectItemCaseSensitive(accuracy,"count"),*miss=cJSON_GetObjectItemCaseSensitive(accuracy,"latest_error_pp"),*mae=cJSON_GetObjectItemCaseSensitive(accuracy,"recent_mae_tenths");
            s_usage.accuracy_count=integer(count,0,120)?count->valueint:0;
            s_usage.accuracy_valid=integer(miss,-100,100);
            s_usage.accuracy_miss=s_usage.accuracy_valid?miss->valueint:0;
            s_usage.accuracy_mae=integer(mae,0,1000)?mae->valueint:0;
            xSemaphoreGive(s_mutex);
        }
    }
    const cJSON *refresh=cJSON_GetObjectItemCaseSensitive(r,"usage_refresh");
    if(cJSON_IsObject(refresh)){
        const char *sync=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(refresh,"last_sync"));
        if(sync && strlen(sync)<32){
            bool safe=true;
            for(const char *p=sync;*p;p++)if(*p<32 || *p>126)safe=false;
            if(safe){xSemaphoreTake(s_mutex,portMAX_DELAY);strlcpy(s_usage.last_sync,sync,32);xSemaphoreGive(s_mutex);}
        }
    }
    if(!strcmp(s_job->op,"usageapply")){finish(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r,"paired_applied")),NULL);return true;}
    if(!strcmp(s_job->op,"usage")){finish(true,NULL);return true;}
    const char *cid=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(r,"chat"));
    if(cid && strlen(cid)==32 && !s_job->chat[0])strlcpy(s_job->chat,cid,sizeof(s_job->chat));
    if(!strcmp(s_job->op,"put")){
        s_job->offset+=s_sent_bytes;
        if(s_job->text[s_job->offset])return true;
        xSemaphoreTake(s_mutex,portMAX_DELAY);s_messages++;xSemaphoreGive(s_mutex);
        finish(true,"Draft saved locally");return true;
    }
    if(!strcmp(s_job->op,"suggest") || !strcmp(s_job->op,"rename") || !strcmp(s_job->op,"delete")){
        if(!cid || strcmp(cid,s_job->chat)){finish(false,"Invalid library response");return true;}
        const char *title=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(r,"title"));
        const char *token=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(r,"token"));
        const char *replacement=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(r,"replacement"));
        bool suggest=!strcmp(s_job->op,"suggest"),removed=!strcmp(s_job->op,"delete");
        bool active=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r,"active_deleted"));
        if((suggest && (!title || !title[0] || strlen(title)>80 || !token || strlen(token)!=24)) ||
           (!suggest && !removed && !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r,"renamed"))) ||
           (removed && (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r,"deleted")) || (active && (!replacement || strlen(replacement)!=32))))){finish(false,"Invalid library response");return true;}
        if(removed && active)muse_conversation_clear();
        xSemaphoreTake(s_mutex,portMAX_DELAY);
        strlcpy(s_view.action,s_job->op,sizeof(s_view.action));strlcpy(s_view.action_chat,cid,33);s_view.action_version++;
        if(suggest){strlcpy(s_view.suggestion,title,sizeof(s_view.suggestion));strlcpy(s_view.title_token,token,25);s_view.title_variant=s_job->page;const cJSON *choices=cJSON_GetObjectItemCaseSensitive(r,"choices");s_view.title_choices=integer(choices,1,8)?(unsigned)choices->valueint:1;}
        if(removed && active){s_view.resumed=false;s_messages=0;s_context[0]=0;strlcpy(s_view.chat,replacement,33);}
        xSemaphoreGive(s_mutex);
        finish(true,suggest?"Free local title preview":removed?"Conversation deleted":"Title updated");return true;
    }
    if(!strcmp(s_job->op,"page") || !strcmp(s_job->op,"context")){
        const char *part=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(r,"text"));
        const cJSON *next=cJSON_GetObjectItemCaseSensitive(r,"next"),*more=cJSON_GetObjectItemCaseSensitive(r,"more");
        const cJSON *pages=cJSON_GetObjectItemCaseSensitive(r,"pages");
        if(!part || strlen(part)>1080 || s_assembled+strlen(part)>=8192 || !integer(next,s_job->offset,s_job->offset+180) || !integer(pages,1,1000000) || s_job->page>=(unsigned)pages->valueint || !cJSON_IsBool(more) || (cJSON_IsTrue(more) && next->valueint==(int)s_job->offset)){
            finish(false,"Invalid archive page");return true;
        }
        strcpy(s_assembly+s_assembled,part);s_assembled+=strlen(part);s_job->offset=next->valueint;
        if(cJSON_IsTrue(more))return true;
        if(!strcmp(s_job->op,"context")){finish(true,NULL);return true;}
        xSemaphoreTake(s_mutex,portMAX_DELAY);
        strlcpy(s_page,s_assembly,8192);strlcpy(s_view.chat,s_job->chat,33);
        const cJSON *style_start=cJSON_GetObjectItemCaseSensitive(r,"style_start");s_view.style_start=integer(style_start,0,127)?(uint8_t)style_start->valueint:0;
        s_view.bold_start=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r,"bold_start"));s_view.code_start=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r,"code_start"));
        s_view.page=s_job->page;s_view.pages=(unsigned)pages->valueint;s_view.library=false;s_view.content_version++;
        xSemaphoreGive(s_mutex);
        finish(true,"Saved text / Resume to continue this chat");return true;
    }
    if(!strcmp(s_job->op,"list")){
        const cJSON *items=cJSON_GetObjectItemCaseSensitive(r,"items");
        xSemaphoreTake(s_mutex,portMAX_DELAY);
        const cJSON *offset=cJSON_GetObjectItemCaseSensitive(r,"offset");
        s_view.library=true;s_view.count=0;s_view.library_offset=integer(offset,0,100000)?(unsigned)offset->valueint:s_job->page;s_view.more=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r,"more"));s_view.content_version++;
        if(cJSON_IsArray(items))for(int i=0;i<cJSON_GetArraySize(items)&&i<5;i++){
            const cJSON *item=cJSON_GetArrayItem(items,i);
            const char *key=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item,"id")),*title=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item,"title"));
            if(!key || strlen(key)!=32 || !title)continue;
            muse_memory_item_t *out=&s_view.items[s_view.count++];strlcpy(out->id,key,33);strlcpy(out->title,title,sizeof(out->title));out->saved=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item,"saved")) || cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(item,"saved"))==1;
        }
        xSemaphoreGive(s_mutex);finish(true,"Local library / tap a chat to read");return true;
    }
    bool resumed=!strcmp(s_job->op,"resume"),fresh=!strcmp(s_job->op,"new");
    if(resumed || fresh){
        muse_conversation_clear();
        xSemaphoreTake(s_mutex,portMAX_DELAY);s_view.resumed=resumed;strlcpy(s_view.chat,s_job->chat,33);s_messages=0;s_view.library=false;s_view.pages=0;s_view.content_version++;xSemaphoreGive(s_mutex);
    }
    finish(true,resumed?"Resumed / context used on your next question":fresh?"New local conversation started":"Conversation bookmarked");return true;
}
char *muse_memory_prompt(const char *question)
{
    if(!s_mutex || !question)return NULL;
    xSemaphoreTake(s_mutex,portMAX_DELAY);bool needed=s_view.resumed || s_messages>=8;xSemaphoreGive(s_mutex);
    if(!needed)return NULL;
    job_t *j=calloc(1,sizeof(*j));if(!j)return NULL;
    strcpy(j->op,"context");j->text=heap_caps_malloc(strlen(question)+1,MUSE_BIG_CAPS);if(j->text)strcpy(j->text,question);j->context_serial=++s_context_request;
    if(!j->text){free(j);return NULL;}
    uint32_t serial=j->context_serial;if(!enqueue(j))return NULL;
    int64_t until=esp_timer_get_time()+12000000LL;
    while(esp_timer_get_time()<until){
        xSemaphoreTake(s_mutex,portMAX_DELAY);
        bool done=s_context_done==serial,ok=done&&s_context_ok;
        char *out=NULL;
        if(ok && s_context[0]){
            const char *prefix="Continue our conversation using these local transcript excerpts as background data. They may be incomplete; do not invent missing memories. Follow my current question.\n<saved_transcript>\n";
            const char *suffix="\n</saved_transcript>\nMy current question: ";
            size_t cap=strlen(prefix)+strlen(s_context)+strlen(suffix)+strlen(question)+1;
            out=heap_caps_malloc(cap,MUSE_BIG_CAPS);
            if(out)snprintf(out,cap,"%s%s%s%s",prefix,s_context,suffix,question);
        }
        xSemaphoreGive(s_mutex);
        if(done)return out;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    status("Context unavailable; question sent normally");return NULL;
}
