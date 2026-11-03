/* SPDX-License-Identifier: Apache-2.0 */
#include "muse_memory.h"
#include "muse_chat.h"
#include "muse_mem.h"
#include "muse_state.h"
#include "muse_conversation.h"
#include "driver/uart.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
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
static muse_memory_view_t s_view;
static muse_usage_view_t s_usage;
static muse_recorder_view_t s_recorder;
static bool s_recorder_starting;
void muse_memory_recorder_copy(muse_recorder_view_t *out){if(!out)return;memset(out,0,sizeof(*out));if(s_mutex){xSemaphoreTake(s_mutex,portMAX_DELAY);*out=s_recorder;out->active=out->active||s_recorder_starting;xSemaphoreGive(s_mutex);}}
bool muse_memory_recorder_active(void){if(!s_mutex)return false;xSemaphoreTake(s_mutex,portMAX_DELAY);bool active=s_recorder.active||s_recorder_starting;xSemaphoreGive(s_mutex);return active;}

static int64_t s_usage_poll=5000000;
static volatile bool s_usage_soon;
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
bool muse_memory_usage_check(void)
{
    if(!s_mutex)return false;
    job_t *j=calloc(1,sizeof(*j));if(!j)return false;
    strcpy(j->op,"usagecheck");return enqueue(j);
}
bool muse_memory_recorder_action(const char *op,unsigned page,unsigned level){
 if(!s_mutex || !op || (strcmp(op,"recstart")&&strcmp(op,"recstop")&&strcmp(op,"recstatus")&&strcmp(op,"recpage")&&strcmp(op,"recprev")&&strcmp(op,"recnext")))return false;
 job_t *j=calloc(1,sizeof(*j));if(!j)return false;strlcpy(j->op,op,sizeof(j->op));j->page=page;j->offset=level;
 if(!strcmp(op,"recstart")){xSemaphoreTake(s_mutex,portMAX_DELAY);if(s_recorder.active||s_recorder_starting){xSemaphoreGive(s_mutex);free(j);return false;}s_recorder_starting=true;strlcpy(s_recorder.note,"Preparing local recording...",96);xSemaphoreGive(s_mutex);}
 if(!strcmp(op,"recprev")||!strcmp(op,"recnext")){xSemaphoreTake(s_mutex,portMAX_DELAY);s_recorder.text[0]=0;xSemaphoreGive(s_mutex);}
 bool ok=enqueue(j);if(!ok && !strcmp(op,"recstart")){xSemaphoreTake(s_mutex,portMAX_DELAY);s_recorder_starting=false;xSemaphoreGive(s_mutex);}return ok;
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
    if(s_job->chat[0])cJSON_AddStringToObject(d,"chat",s_job->chat);
    cJSON_AddNumberToObject(d,"page",s_job->page);
    cJSON_AddNumberToObject(d,"offset",!strcmp(s_job->op,"list")?s_job->page:s_job->offset);
    if(!strcmp(s_job->op,"put")){
        char part[257];s_sent_bytes=utf8_piece(s_job->text,s_job->offset);
        memcpy(part,s_job->text+s_job->offset,s_sent_bytes);part[s_sent_bytes]=0;
        cJSON_AddStringToObject(d,"key",s_job->key);cJSON_AddStringToObject(d,"role",s_job->user?"user":"assistant");
        cJSON_AddStringToObject(d,"part",part);cJSON_AddBoolToObject(d,"finish",!s_job->text[s_job->offset+s_sent_bytes]);
    }else if(!strcmp(s_job->op,"context"))cJSON_AddStringToObject(d,"query",s_job->text?s_job->text:"");
    char *json=cJSON_PrintUnformatted(root);cJSON_Delete(root);
    if(!json){finish(false,"Archive memory unavailable");return;}
    size_t n=strlen(json);
    bool sent=uart_write_bytes(CONFIG_MUSE_MODULE_LLM_UART_NUM,json,n)==(int)n && uart_write_bytes(CONFIG_MUSE_MODULE_LLM_UART_NUM,"\n",1)==1;
    cJSON_free(json);s_waiting=true;s_deadline=esp_timer_get_time()+(sent?8000000LL:100000LL);
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
        s_request[0]=0;
        if(!strcmp(s_job->op,"put") && s_job->offset && !s_job->restarted){s_job->offset=0;s_job->restarted=true;s_waiting=false;return true;}
        finish(false,"Archive could not finish; try again");return true;
    }
    const cJSON *r=cJSON_GetObjectItemCaseSensitive(d,"result");
    if(!cJSON_IsObject(r))return true;
    s_waiting=false;s_request[0]=0;s_job->retries=0;
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
            s_usage.learned=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(u,"learned"));s_usage.pending=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(u,"pending"));strlcpy(s_usage.reset,reset,sizeof(s_usage.reset));strlcpy(s_usage.checked,checked,sizeof(s_usage.checked));xSemaphoreGive(s_mutex);
        }
    }
    const cJSON *refresh=cJSON_GetObjectItemCaseSensitive(r,"usage_refresh");
    if(!cJSON_IsObject(refresh)){xSemaphoreTake(s_mutex,portMAX_DELAY);s_usage.refresh_supported=false;s_usage.refresh_pending=false;xSemaphoreGive(s_mutex);}
    if(cJSON_IsObject(refresh)){
        const char *note=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(refresh,"note"));
        const char *sync=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(refresh,"last_sync"));
        const char *phase=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(refresh,"status")),*identity=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(refresh,"id"));
        bool phase_ok=phase && (!strcmp(phase,"idle")||!strcmp(phase,"queued")||!strcmp(phase,"checking")||!strcmp(phase,"done")||!strcmp(phase,"error"));
        bool id_ok=identity && strlen(identity)<=32;
        if(id_ok)for(const char *p=identity;*p;p++)if(!((*p>='0'&&*p<='9')||(*p>='a'&&*p<='f')))id_ok=false;
        if(note && sync && strlen(note)<96 && strlen(sync)<32){
            bool safe=true;for(const char *p=note;*p;p++)if(*p<32 || *p>126)safe=false;
            for(const char *p=sync;*p;p++)if(*p<32 || *p>126)safe=false;
            if(safe){xSemaphoreTake(s_mutex,portMAX_DELAY);s_usage.refresh_supported=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(refresh,"supported"));s_usage.refresh_pending=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(refresh,"pending"));strlcpy(s_usage.refresh_note,note,96);strlcpy(s_usage.last_sync,sync,32);strlcpy(s_usage.refresh_state,phase_ok?phase:"idle",12);strlcpy(s_usage.refresh_id,id_ok?identity:"",33);xSemaphoreGive(s_mutex);}
        }
    }
    if(!strcmp(s_job->op,"usage") || !strcmp(s_job->op,"usagecheck")){finish(true,NULL);return true;}
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
