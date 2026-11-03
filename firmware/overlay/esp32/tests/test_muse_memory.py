"""Compile the real archive UART client; exercise bounds, retries and ownership."""
import os,shlex,subprocess,tempfile,unittest
from pathlib import Path
from test_muse_module_llm_runtime import COMMON,ROOT,JSON

EXTRA=r'''
#define CONFIG_SPIRAM 1
#define portMAX_DELAY 0
typedef int StaticSemaphore_t;
typedef void *SemaphoreHandle_t;
static inline void *xSemaphoreCreateMutexStatic(StaticSemaphore_t *s){return s;}
static inline int xSemaphoreTake(void *s,int t){(void)s;(void)t;return 1;}
static inline int xSemaphoreGive(void *s){(void)s;return 1;}
static inline void *heap_caps_calloc(size_t n,size_t z,int caps){(void)caps;return calloc(n,z);}
static inline void vTaskDelay(int t){fake_time+=(int64_t)t*1000;}
'''
HARNESS=r'''
#include "fake_common.h"
char fake_uart[65536];size_t fake_uart_n;int64_t fake_time=100;
#include "muse_memory.c"
static muse_mode_t mode=MUSE_MODE_IDLE;
muse_mode_t muse_state_mode(float *seconds){(void)seconds;return mode;}
static unsigned cleared;
void muse_conversation_clear(void){cleared++;}
static cJSON *ack(const char *text,int next,bool more,int pages)
{
 cJSON *r=cJSON_CreateObject(),*d=cJSON_CreateObject(),*e=cJSON_CreateObject(),*v=cJSON_CreateObject();
 cJSON_AddStringToObject(r,"request_id",s_request);cJSON_AddStringToObject(r,"work_id","musevoice");cJSON_AddStringToObject(r,"object","musevoice.event");
 cJSON_AddItemToObject(r,"data",d);cJSON_AddItemToObject(r,"error",e);cJSON_AddNumberToObject(e,"code",0);
 cJSON_AddStringToObject(d,"session","1234abcd");cJSON_AddStringToObject(d,"event","memory");cJSON_AddNumberToObject(d,"generation",0);cJSON_AddNumberToObject(d,"protocol",1);cJSON_AddItemToObject(d,"result",v);
 cJSON_AddStringToObject(v,"chat","aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
 if(text){cJSON_AddStringToObject(v,"text",text);cJSON_AddNumberToObject(v,"next",next);cJSON_AddBoolToObject(v,"more",more);cJSON_AddNumberToObject(v,"pages",pages);}
 return r;
}
static cJSON *rec_ack(bool active,bool malformed){
 cJSON *r=ack(NULL,0,false,1),*v=cJSON_GetObjectItem(cJSON_GetObjectItem(r,"data"),"result"),*rec=cJSON_CreateObject();cJSON_AddItemToObject(v,"recorder",rec);
 cJSON_AddBoolToObject(rec,"supported",true);if(malformed)cJSON_AddNumberToObject(rec,"active",0);else cJSON_AddBoolToObject(rec,"active",active);cJSON_AddBoolToObject(rec,"audio",false);cJSON_AddStringToObject(rec,"state",active?"recording":"idle");cJSON_AddStringToObject(rec,"note","Local recorder");cJSON_AddStringToObject(rec,"id","");cJSON_AddStringToObject(rec,"text","");cJSON_AddNumberToObject(rec,"seconds",0);cJSON_AddNumberToObject(rec,"lines",0);cJSON_AddNumberToObject(rec,"audio_mb",0);cJSON_AddNumberToObject(rec,"page",0);cJSON_AddNumberToObject(rec,"pages",1);return r;
}
int main(int argc,char **argv)
{
 assert(argc==2);muse_memory_init();
 switch(atoi(argv[1])){
 case 0:{
  char message[804];memset(message,'x',800);memcpy(message+254,"\xc3\xa9",2);message[800]=0;
  muse_memory_record(true,message);muse_memory_tick("1234abcd",false);assert(!fake_uart_n);
  char rebuilt[804]={0};unsigned n=0;
  for(int k=0;k<8;k++){
   fake_uart_n=0;muse_memory_tick("1234abcd",true);if(!s_job)break;
   cJSON *wire=cJSON_Parse(fake_uart);assert(wire);const cJSON *d=cJSON_GetObjectItem(wire,"data");
   const char *part=cJSON_GetStringValue(cJSON_GetObjectItem(d,"part"));assert(strlen(part)<=256);
   assert(cJSON_GetNumberValue(cJSON_GetObjectItem(d,"offset"))==n);strcpy(rebuilt+n,part);n+=(unsigned)strlen(part);cJSON_Delete(wire);
   cJSON *reply=ack(NULL,0,false,1);assert(muse_memory_reply(reply,"1234abcd"));cJSON_Delete(reply);
  }
  assert(!s_job);assert(!strcmp(message,rebuilt));assert(s_messages==1);break;
 }
 case 1:{
  assert(muse_memory_action("page",NULL,0));muse_memory_tick("1234abcd",true);assert(s_waiting);
  cJSON *r=ack("first ",6,true,2);cJSON *d=cJSON_GetObjectItem(r,"data");
  cJSON_SetValuestring(cJSON_GetObjectItem(d,"session"),"deadbeef");muse_memory_reply(r,"1234abcd");assert(s_job->offset==0 && s_waiting);
  cJSON_SetValuestring(cJSON_GetObjectItem(d,"session"),"1234abcd");muse_memory_reply(r,"1234abcd");cJSON_Delete(r);assert(s_job->offset==6 && !s_waiting);
  muse_memory_tick("1234abcd",true);r=ack("last",10,false,2);muse_memory_reply(r,"1234abcd");cJSON_Delete(r);
  assert(!s_job);assert(!strcmp(s_page,"first last"));assert(s_view.pages==2);break;
 }
 case 2:{
  muse_memory_record(true,"complete message");muse_memory_tick("1234abcd",true);char previous[64];strcpy(previous,s_request);
  fake_time+=9000000;muse_memory_tick("1234abcd",false);assert(!strcmp(previous,s_request));
  muse_memory_tick("1234abcd",true);assert(!strcmp(previous,s_request));assert(s_job->offset==0);
  cJSON *r=ack(NULL,0,false,1);muse_memory_reply(r,"1234abcd");cJSON_Delete(r);assert(!s_job);break;
 }
 case 3:{
  mode=MUSE_MODE_SPEAKING;assert(!muse_memory_action("new",NULL,0));assert(!s_switching);
  mode=MUSE_MODE_IDLE;assert(muse_memory_action("new",NULL,0));assert(s_switching);
  muse_memory_tick("1234abcd",true);cJSON *r=ack(NULL,0,false,1);muse_memory_reply(r,"1234abcd");cJSON_Delete(r);assert(!s_switching);break;
 }
 case 4:{
  muse_memory_action("page",NULL,0);muse_memory_tick("1234abcd",true);
  cJSON *r=ack("bad",99999,true,1);muse_memory_reply(r,"1234abcd");cJSON_Delete(r);
  assert(!s_job);assert(!s_page[0]);assert(strstr(s_view.status,"Invalid"));break;
 }

 case 5:{
  const char *cid="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  mode=MUSE_MODE_SPEAKING;assert(!muse_memory_action("delete",cid,0));assert(!s_switching);
  mode=MUSE_MODE_IDLE;assert(muse_memory_action("delete",cid,0));assert(s_switching);
  muse_memory_tick("1234abcd",false);assert(!fake_uart_n);muse_memory_tick("1234abcd",true);
  cJSON *wire=cJSON_Parse(fake_uart);assert(cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetObjectItem(wire,"data"),"confirm")));cJSON_Delete(wire);
  strcpy(s_context,"old memory");s_messages=9;
  cJSON *r=ack(NULL,0,false,1),*v=cJSON_GetObjectItem(cJSON_GetObjectItem(r,"data"),"result");
  cJSON_AddBoolToObject(v,"deleted",true);cJSON_AddBoolToObject(v,"active_deleted",true);cJSON_AddStringToObject(v,"replacement","bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
  muse_memory_reply(r,"1234abcd");cJSON_Delete(r);assert(!s_switching && !s_job);assert(cleared==1 && !s_context[0] && s_messages==0 && !s_view.resumed);break;
 }
 case 6:{
  const char *cid="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",*token="1234567890abcdef12345678";
  assert(muse_memory_action("suggest",cid,2));muse_memory_tick("1234abcd",true);
  cJSON *r=ack(NULL,0,false,1),*v=cJSON_GetObjectItem(cJSON_GetObjectItem(r,"data"),"result");
  cJSON_AddStringToObject(v,"title","The Blue Lantern");cJSON_AddStringToObject(v,"token",token);
  muse_memory_reply(r,"1234abcd");cJSON_Delete(r);assert(!strcmp(s_view.suggestion,"The Blue Lantern") && !strcmp(s_view.title_token,token));assert(s_view.title_variant==2);
  assert(muse_memory_rename(cid,token));fake_uart_n=0;muse_memory_tick("1234abcd",true);
  cJSON *wire=cJSON_Parse(fake_uart);assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(wire,"data"),"token")),token));cJSON_Delete(wire);
  r=ack(NULL,0,false,1);v=cJSON_GetObjectItem(cJSON_GetObjectItem(r,"data"),"result");cJSON_AddBoolToObject(v,"renamed",true);
  muse_memory_reply(r,"1234abcd");cJSON_Delete(r);assert(!strcmp(s_view.action,"rename") && s_view.action_version==2 && !cleared);break;
 }
 case 7:{
  assert(muse_memory_action("delete","bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",0));muse_memory_tick("1234abcd",true);
  cJSON *r=ack(NULL,0,false,1),*v=cJSON_GetObjectItem(cJSON_GetObjectItem(r,"data"),"result");cJSON_AddBoolToObject(v,"deleted",true);cJSON_AddBoolToObject(v,"active_deleted",true);cJSON_AddStringToObject(v,"replacement","cccccccccccccccccccccccccccccccc");
  muse_memory_reply(r,"1234abcd");cJSON_Delete(r);assert(!cleared && !s_switching && !s_view.action_version);assert(strstr(s_view.status,"Invalid"));break;
 }
 case 8:{
  fake_time=10000000;muse_memory_tick("1234abcd",false);assert(!fake_uart_n && !s_job);
  muse_memory_tick("1234abcd",true);assert(s_job && !strcmp(s_job->op,"usage"));
  cJSON *r=ack(NULL,0,false,1),*v=cJSON_GetObjectItem(cJSON_GetObjectItem(r,"data"),"result"),*u=cJSON_CreateObject();cJSON_AddItemToObject(v,"usage",u);
  cJSON_AddNumberToObject(u,"used",1);cJSON_AddNumberToObject(u,"observed",1);cJSON_AddNumberToObject(u,"age",12);cJSON_AddStringToObject(u,"reset","Nov 6");cJSON_AddStringToObject(u,"checked","Oct 30 13:00");
  muse_memory_reply(r,"1234abcd");cJSON_Delete(r);muse_usage_view_t out={0};assert(muse_memory_usage_copy(&out) && out.used==1 && out.age==12 && !s_job && !s_view.action_version);break;
 }
 case 9:{
  fake_time=10000000;muse_memory_tick("1234abcd",true);
  cJSON *r=ack(NULL,0,false,1),*v=cJSON_GetObjectItem(cJSON_GetObjectItem(r,"data"),"result"),*u=cJSON_CreateObject();cJSON_AddItemToObject(v,"usage",u);
  cJSON_AddNumberToObject(u,"used",101);cJSON_AddNumberToObject(u,"observed",1);cJSON_AddNumberToObject(u,"age",12);cJSON_AddStringToObject(u,"reset","Nov 6");cJSON_AddStringToObject(u,"checked","Oct 30 13:00");
  muse_memory_reply(r,"1234abcd");cJSON_Delete(r);muse_usage_view_t out={0};assert(!muse_memory_usage_copy(&out) && !s_job);break;
 }

 case 10:{
  unsigned version=s_view.version;assert(muse_memory_usage_check());assert(s_view.version==version && !s_switching);
  muse_memory_tick("1234abcd",false);assert(!fake_uart_n);
  muse_memory_tick("1234abcd",true);assert(s_job && !strcmp(s_job->op,"usagecheck"));
  cJSON *r=ack(NULL,0,false,1),*v=cJSON_GetObjectItem(cJSON_GetObjectItem(r,"data"),"result"),*q=cJSON_CreateObject();cJSON_AddItemToObject(v,"usage_refresh",q);
  cJSON_AddBoolToObject(q,"supported",true);cJSON_AddBoolToObject(q,"pending",true);cJSON_AddStringToObject(q,"note","Checking Muse in Chrome...");cJSON_AddStringToObject(q,"last_sync","Nov 1, 2026 09:41");
  muse_memory_reply(r,"1234abcd");cJSON_Delete(r);muse_usage_view_t out={0};muse_memory_usage_copy(&out);assert(out.refresh_supported && out.refresh_pending && !s_job && !s_view.action_version && !cleared);
  assert(!strcmp(out.last_sync,"Nov 1, 2026 09:41"));
  muse_memory_usage_poll();muse_memory_tick("1234abcd",true);assert(s_job && !strcmp(s_job->op,"usage"));r=ack(NULL,0,false,1);muse_memory_reply(r,"1234abcd");cJSON_Delete(r);muse_memory_usage_copy(&out);assert(!out.refresh_supported && !out.refresh_pending);break;
 }
 case 11:{
  muse_memory_usage_check();muse_memory_tick("1234abcd",true);
  cJSON *r=ack(NULL,0,false,1),*v=cJSON_GetObjectItem(cJSON_GetObjectItem(r,"data"),"result"),*q=cJSON_CreateObject();cJSON_AddItemToObject(v,"usage_refresh",q);
  cJSON_AddBoolToObject(q,"supported",true);cJSON_AddBoolToObject(q,"pending",true);cJSON_AddStringToObject(q,"note","invalid\nnote");cJSON_AddStringToObject(q,"last_sync","Nov 1, 2026 09:41");
  muse_memory_reply(r,"1234abcd");cJSON_Delete(r);muse_usage_view_t out={0};muse_memory_usage_copy(&out);assert(!out.refresh_supported && !out.last_sync[0] && !s_job);break;
 }
 case 12:{
  assert(muse_memory_recorder_action("recstart",1,75));assert(muse_memory_recorder_active());
  muse_memory_tick("1234abcd",false);assert(!fake_uart_n);
  muse_memory_tick("1234abcd",true);cJSON *wire=cJSON_Parse(fake_uart);cJSON *d=cJSON_GetObjectItem(wire,"data");assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(d,"op")),"recstart"));assert(cJSON_GetNumberValue(cJSON_GetObjectItem(d,"offset"))==75);cJSON_Delete(wire);
  cJSON *r=ack(NULL,0,false,1),*result=cJSON_GetObjectItem(cJSON_GetObjectItem(r,"data"),"result"),*rec=cJSON_CreateObject();cJSON_AddItemToObject(result,"recorder",rec);
  cJSON_AddBoolToObject(rec,"supported",true);cJSON_AddBoolToObject(rec,"active",true);cJSON_AddBoolToObject(rec,"audio",true);cJSON_AddStringToObject(rec,"state","recording");cJSON_AddStringToObject(rec,"note","Recording offline");cJSON_AddStringToObject(rec,"id","rec-000001-12345678");cJSON_AddStringToObject(rec,"text","");cJSON_AddNumberToObject(rec,"seconds",2);cJSON_AddNumberToObject(rec,"lines",1);cJSON_AddNumberToObject(rec,"audio_mb",0);cJSON_AddNumberToObject(rec,"page",0);cJSON_AddNumberToObject(rec,"pages",1);
  muse_memory_reply(r,"1234abcd");cJSON_Delete(r);muse_recorder_view_t out;muse_memory_recorder_copy(&out);assert(out.supported&&out.active&&out.audio&&out.seconds==2&&!s_job&&!s_recorder_starting);assert(!s_view.content_version);break;
 }
 case 13:{
  assert(muse_memory_recorder_action("recstart",0,100));assert(muse_memory_recorder_active());muse_memory_tick("1234abcd",true);
  cJSON *r=ack(NULL,0,false,1);cJSON_SetNumberValue(cJSON_GetObjectItem(cJSON_GetObjectItem(r,"error"),"code"),-40);muse_memory_reply(r,"1234abcd");cJSON_Delete(r);assert(!muse_memory_recorder_active());assert(!s_job && !s_recorder_starting && !s_view.content_version);break;
 }
 case 14:{
  s_recorder.active=true;assert(muse_memory_recorder_action("recstatus",0,0));muse_memory_tick("1234abcd",true);cJSON *r=rec_ack(false,true);muse_memory_reply(r,"1234abcd");cJSON_Delete(r);assert(muse_memory_recorder_active());break;
 }
 case 15:{
  assert(muse_memory_recorder_action("recstart",0,100));muse_memory_tick("1234abcd",true);
  for(int k=0;k<3;k++){fake_time+=9000000;muse_memory_tick("1234abcd",true);}assert(!s_job && muse_memory_recorder_active());
  assert(muse_memory_recorder_action("recstatus",0,0));muse_memory_tick("1234abcd",true);cJSON *r=rec_ack(false,false);muse_memory_reply(r,"1234abcd");cJSON_Delete(r);assert(!muse_memory_recorder_active());break;
 }
 default:assert(0);
 }
 return 0;
}
'''
class Tests(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.tmp=tempfile.TemporaryDirectory();cls.addClassCleanup(cls.tmp.cleanup);out=Path(cls.tmp.name)
  common=COMMON.replace('bytes[8][64]','bytes[24][64]').replace('n<=8','n<=24').replace('q->count==8','q->count==24').replace('%8','%24')+EXTRA
  (out/'fake_common.h').write_text(common)
  for name in ['driver/uart.h','esp_heap_caps.h','esp_random.h','esp_timer.h','freertos/FreeRTOS.h','freertos/queue.h','freertos/semphr.h','freertos/task.h','sdkconfig.h']:
   p=out/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text('#include "fake_common.h"\n')
  (out/'test.c').write_text(HARNESS);cls.binary=out/'memory'
  cmd=[*shlex.split(os.environ.get('CC','cc')),'-std=gnu11','-Wall','-Wextra','-Werror','-I',str(out),'-I',str(JSON),'-I',str(ROOT/'components/muse'),str(out/'test.c'),str(JSON/'cJSON.c'),'-lm','-o',str(cls.binary)]
  result=subprocess.run(cmd,capture_output=True,text=True)
  if result.returncode:raise AssertionError(result.stdout+result.stderr)
 def run_case(self,n):
  result=subprocess.run([str(self.binary),str(n)],capture_output=True,text=True);self.assertEqual(result.returncode,0,result.stdout+result.stderr)
 def test_full_utf8_upload_and_voice_priority(self):self.run_case(0)
 def test_stale_session_rejected_and_pages_assembled(self):self.run_case(1)
 def test_timeout_repeats_same_identity_and_offset(self):self.run_case(2)
 def test_chat_switch_waits_until_voice_idle(self):self.run_case(3)
 def test_bad_page_bounds_leave_old_text_untouched(self):self.run_case(4)

 def test_delete_waits_for_voice_and_clears_deleted_active_context(self):self.run_case(5)
 def test_title_preview_and_accept_preserve_exact_token(self):self.run_case(6)
 def test_wrong_chat_delete_ack_cannot_clear_current_conversation(self):self.run_case(7)
 def test_usage_poll_yields_to_voice_and_retains_official_calibration(self):self.run_case(8)
 def test_invalid_usage_cannot_become_a_gauge(self):self.run_case(9)

 def test_manual_usage_check_metadata_and_legacy_fallback(self):self.run_case(10)
 def test_invalid_refresh_text_is_not_displayed(self):self.run_case(11)

 def test_recorder_start_waits_for_idle_and_does_not_change_archive(self):self.run_case(12)
 def test_rejected_recorder_start_releases_local_microphone_guard(self):self.run_case(13)

 def test_malformed_recorder_flag_cannot_release_microphone_guard(self):self.run_case(14)
 def test_start_timeout_keeps_guard_until_real_status_arrives(self):self.run_case(15)
