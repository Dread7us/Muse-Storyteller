"""Exercise production turn/status/revision logic with a fake clock and JSON tree."""
import shutil,subprocess,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def harness():
    source=(ROOT/'firmware/overlay/esp32/components/muse/muse_chat_session.cpp').read_text(encoding='utf8')
    tracking=source[source.index('struct task_progress_t {'):source.index('/* End reply work tracking. */')]
    final=source[source.index('static const char *reply_final_text('):source.index('EXT_RAM_BSS_ATTR static char s_final_reply')]
    done=source[source.index('static void message_done('):source.index('static const char *msg_id(')]
    binding=source[source.index('static int find_msg('):source.index('static void append_text(')]
    events=source[source.index('static const char *msg_id('):source.index('static void on_chat_ack(')]
    restart=source[source.index('static void restart_reply('):source.index('static void handle(')]
    check=source[source.index('static void check_turn(void)'):source.index('/* ---- Inbound dispatch ---- */')]
    drain=source[source.index('    if (playback == MUSE_MODULE_LLM_PLAYBACK_DONE) {'):source.index('    if (playback == MUSE_MODULE_LLM_PLAYBACK_CANCELLED) {')]
    return r'''
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <map>
#include <vector>
#define CONFIG_MUSE_MODULE_LLM 1
#define EXT_RAM_BSS_ATTR
#define TEXT_MAX 8193
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
enum {P_IDLE,P_WAIT_REPLY,P_WAIT_FINAL,TTS_NONE,TTS_QUEUED,TTS_ACTIVE,TTS_FINISHED,M_DONE,M_TEXT,MUSE_HATCH_EV_SENT,MUSE_HATCH_EV_HEARD,MUSE_MODULE_LLM_PLAYBACK_DONE};
static const int64_t FINAL_TIMEOUT_US=15000000,REPLY_TIMEOUT_US=60000000,TEXT_REPLY_TIMEOUT_US=300000000,TEXT_TURN_CAP_US=900000000,SETTLE_US=3000000;
struct cJSON {int type=0;double valuedouble=0;std::string value;std::map<std::string,cJSON*> fields;std::vector<cJSON*> items;};
static cJSON *object(){return new cJSON;}
static cJSON *str(const char *s){auto *p=new cJSON;p->type=1;p->value=s;return p;}
static cJSON *arr(std::initializer_list<cJSON*> items){auto *p=new cJSON;p->type=2;p->items=items;return p;}
static cJSON *cJSON_GetObjectItem(cJSON *p,const char *k){return p&&p->fields.count(k)?p->fields[k]:nullptr;}
static const char *cJSON_GetStringValue(cJSON *p){return p&&p->type==1?p->value.c_str():nullptr;}
static bool cJSON_IsArray(cJSON *p){return p&&p->type==2;}
static bool cJSON_IsNumber(cJSON *p){return p&&p->type==3;}
static bool cJSON_IsFalse(cJSON *p){return p&&p->type==4;}
static int cJSON_GetArraySize(cJSON *p){return p?(int)p->items.size():0;}
static cJSON *cJSON_GetArrayItem(cJSON *p,int i){return p&&i>=0&&i<(int)p->items.size()?p->items[i]:nullptr;}
#define strlcpy test_strlcpy
#define strlcat test_strlcat
static size_t strlcpy(char *out,const char *in,size_t cap){size_t n=strlen(in);if(cap){size_t k=n<cap-1?n:cap-1;memcpy(out,in,k);out[k]=0;}return n;}
static size_t strlcat(char *out,const char *in,size_t cap){size_t n=strlen(out);return n+strlcpy(out+n,in,cap-n);}
''' + tracking + r'''
struct msg_t {char id[80]{},tail[128]{},external_key[25]{};bool done=false,native_skip=false;size_t len=0;unsigned conversation_id=0,pcm_start=0,pcm_frames=0;int tts=TTS_NONE;};
struct rejected_t {bool overflow=false;};
static bool muse_chat_is_rejected(rejected_t*,const char*){return false;}
static void muse_chat_reject(rejected_t*,const char*){}
#define MAX_MSGS 8
struct turn_t {int phase=P_WAIT_REPLY,tts_msg=-1,nmsgs=1;unsigned gen=1,pcm_out=0;size_t mp3_len=0;bool text=false,native_started=false,native_direct=false,agent_busy=false,acked=false,silent=false,native_claimed=false;int64_t start_us=0,end_sent_us=0,chat_us=0,last_event_us=0,last_content_us=0,playback_paused_us=0,playback_pause_check=0;char *texts=nullptr,native_job[25]{},user_ids[2][80]{};msg_t msgs[8];task_progress_t task_progress{};rejected_t rejected;};
static turn_t s_turn;
static char text_storage[8*TEXT_MAX],s_pending_reply[TEXT_MAX];
static char s_final_reply[TEXT_MAX];
static int64_t s_last_seq=0;
static int s_pending_reply_msg=-1;
static bool s_bench_reply=false,s_can_restart=false,s_playback_active=false;
static bool s_restart_wait=false,s_transport_pending=false,s_replaying=false,native_rebind_ok=false,sections_pending=false;
static unsigned s_gen=1,rebound_previous=0,rebound_current=0;
static char s_reply_shown[128];
static int64_t clock_us=0;
static bool completed=false;static std::string failure;static unsigned conversations=0,cancels=0;
struct muse_module_llm_status_t {bool paused=false;};
static void muse_module_llm_status(muse_module_llm_status_t *out){out->paused=false;}
static int64_t now_us(){return clock_us;}
static void on_dictation_end(bool){}
static void turn_fail(const char *reason){failure=reason;s_turn.phase=P_IDLE;}
static void turn_done(bool){completed=true;s_turn.phase=P_IDLE;}
static void log_marks(){}
static void mark(int){}
static void emit(int,const char*){}
static void muse_state_set_caption(const char*){}
static void muse_hatch_console(const char*,const char*,const char*,...){}
static unsigned muse_conversation_add(bool,const char*){return ++conversations;}
static unsigned muse_conversation_add_transient(bool,const char*){return ++conversations;}
static void muse_native_voice_answer_ready(unsigned,bool){}
static void muse_native_voice_cancel(){cancels++;}
static bool muse_native_voice_has_pending_sections(){return sections_pending;}
static bool muse_native_voice_rebind(unsigned previous,unsigned current){rebound_previous=previous;rebound_current=current;return native_rebind_ok;}
static void muse_module_llm_cancel(unsigned){}
static void drain_out(){}
static void append_text(msg_t &m,const char *text){m.len+=strlen(text);strlcat(s_turn.texts+(&m-s_turn.msgs)*TEXT_MAX,text,TEXT_MAX);strlcat(m.tail,text,sizeof(m.tail));}
static void show_reply_start(const msg_t&){}
''' + binding + final + done + events + restart + check + '\nstatic void drain(int playback){\n'+drain+'\n}\n'+r'''
static void reset(){s_turn=turn_t{};memset(text_storage,0,sizeof(text_storage));s_turn.texts=text_storage;clock_us=0;failure.clear();completed=false;conversations=cancels=0;s_pending_reply_msg=-1;s_last_seq=0;s_gen=1;native_rebind_ok=false;sections_pending=false;s_can_restart=false;s_restart_wait=false;s_transport_pending=false;s_replaying=false;rebound_previous=rebound_current=0;}
static void incoming(const char *kind,int seq,const char *id,const char *text){auto *line=object(),*p=object(),*n=object();n->type=3;n->valuedouble=seq;line->fields["seq"]=n;line->fields["type"]=str("event");line->fields["event"]=str(kind);line->fields["payload"]=p;p->fields["message_id"]=str(id);p->fields["display_text"]=str(text);p->fields["text"]=str(text);on_event(line);}
static void ack(){strcpy(text_storage,"I am working.");s_turn.msgs[0].done=true;s_turn.msgs[0].len=strlen(text_storage);s_turn.msgs[0].tts=TTS_FINISHED;}
static void work(bool task,const char *id,const char *status,const char *activity=nullptr,bool linked=false){task_progress_update(s_turn.task_progress,task,id,status,activity,linked);s_turn.agent_busy=task_progress_busy(s_turn.task_progress);}
int main(int argc,char **argv){assert(argc==2);std::string c=argv[1];reset();
 if(c=="busy_gap"){ack();work(true,"root","running");clock_us=61000000;check_turn();assert(!completed&&failure.empty()&&s_turn.phase==P_WAIT_REPLY);}
 else if(c=="idle_activity"){work(true,"root","running");work(false,nullptr,nullptr,"idle");assert(s_turn.agent_busy);work(false,nullptr,"idle");assert(s_turn.agent_busy);}
 else if(c=="children"){ack();work(true,"root","running");work(true,"child","running");work(true,"child","completed");assert(s_turn.agent_busy);work(true,"root","completed");clock_us=4000000;check_turn();assert(completed&&failure.empty());}
 else if(c=="foreign"){work(true,"root","running");assert(!task_progress_update(s_turn.task_progress,true,"old","completed",nullptr,false));assert(task_progress_busy(s_turn.task_progress));}
 else if(c=="failed"){ack();work(true,"root","running");work(true,"root","failed");clock_us=4000000;check_turn();assert(!completed&&failure=="MUSE TASK FAILED - CHECK CHAT");}
 else if(c=="deadline"){ack();work(true,"root","running");clock_us=TEXT_TURN_CAP_US+1;check_turn();assert(!completed&&failure=="MUSE TASK TIMED OUT");}
 else if(c=="queued_final"){ack();s_turn.msgs[0].tts=TTS_QUEUED;clock_us=61000000;check_turn();assert(!completed&&failure.empty());}
 else if(c=="revision"){ack();strcpy(s_turn.msgs[0].external_key,"old");message_done(0,"The completed result is here.");assert(std::string(text_storage)=="The completed result is here."&&s_turn.msgs[0].tts==TTS_QUEUED&&!s_turn.msgs[0].external_key[0]&&conversations==1);}
 else if(c=="duplicate"){ack();message_done(0,"I am working.");assert(s_turn.msgs[0].tts==TTS_FINISHED&&!conversations);}
 else if(c=="active_revision"){ack();s_turn.tts_msg=0;s_turn.msgs[0].tts=TTS_ACTIVE;message_done(0,"The completed result is here.");assert(std::string(text_storage)=="I am working."&&s_pending_reply_msg==0);drain(MUSE_MODULE_LLM_PLAYBACK_DONE);assert(std::string(text_storage)=="The completed result is here."&&s_turn.msgs[0].tts==TTS_QUEUED&&s_pending_reply_msg==-1);}
 else if(c=="structured"){auto *p=object(),*t=object(),*m=object(),*part=object(),*part2=object();p->fields["transcript"]=t;m->fields["id"]=str("root");m->fields["role"]=str("assistant");part->fields["type"]=str("text");part->fields["text"]=str("Result one.");part2->fields["type"]=str("text");part2->fields["text"]=str("Result two.");m->fields["content"]=arr({part,part2});t->fields["messages"]=arr({m});char out[80];assert(std::string(reply_final_text(p,"root",out,sizeof(out)))=="Result one.\nResult two.");char small[4];assert(!reply_final_text(p,"root",small,sizeof(small)));assert(!failure.empty());}
 else if(c=="legacy_idle"){work(false,nullptr,nullptr,"working");assert(s_turn.agent_busy);work(false,nullptr,"idle");assert(!s_turn.agent_busy);ack();clock_us=4000000;check_turn();assert(completed);}
 else if(c=="mixed_sequence"){s_turn.nmsgs=0;incoming("delta.message_start",3157,"ack","");incoming("delta.text_append",3158,"ack","Working.");incoming("delta.message_done",3161,"ack","Working.");incoming("chat.seen",3163,"","");assert(s_turn.nmsgs==1);incoming("message.assistant",3030,"final","The delay test is complete.");assert(s_turn.nmsgs==2&&s_turn.msgs[1].done&&s_turn.msgs[1].tts==TTS_QUEUED&&std::string(text_storage+TEXT_MAX)=="The delay test is complete.");assert(s_last_seq==3163);}
 else if(c=="stream_replay"){s_turn.nmsgs=0;incoming("delta.message_start",100,"reply","");incoming("delta.text_append",101,"reply","Once.");incoming("delta.text_append",101,"reply","Once.");assert(std::string(text_storage)=="Once.");}
 else if(c=="durable_duplicate"){s_turn.nmsgs=0;s_last_seq=4000;incoming("message.assistant",3030,"final","Completed.");s_turn.msgs[0].tts=TTS_FINISHED;unsigned saved=conversations;incoming("message.assistant",3030,"final","Completed.");assert(conversations==saved&&s_turn.msgs[0].tts==TTS_FINISHED);incoming("message.assistant",3030,"final","Completed with detail.");assert(s_turn.msgs[0].tts==TTS_QUEUED&&std::string(text_storage)=="Completed with detail.");}
 else if(c=="failed_direct_restart"){ack();s_can_restart=true;s_turn.native_direct=s_turn.native_started=true;s_turn.phase=P_IDLE;restart_reply();assert(s_turn.gen==2&&s_turn.phase==P_WAIT_REPLY&&!s_turn.native_started&&s_turn.msgs[0].tts==TTS_QUEUED&&rebound_previous==1&&rebound_current==2&&s_restart_wait);assert(std::string(text_storage)=="I am working."&&!conversations);}
 else if(c=="buffered_direct_restart"){ack();s_can_restart=true;s_turn.native_direct=s_turn.native_started=true;native_rebind_ok=true;restart_reply();assert(s_turn.native_started&&rebound_previous==1&&rebound_current==2&&s_turn.gen==2);}
 else if(c=="cached_direct_restart"){ack();s_can_restart=true;s_turn.native_direct=true;strcpy(s_turn.msgs[0].external_key,"verified-cache");restart_reply();assert(!strcmp(s_turn.msgs[0].external_key,"verified-cache")&&s_turn.msgs[0].tts==TTS_QUEUED&&!rebound_previous);}
 else if(c=="legacy_restart"){ack();s_can_restart=true;s_turn.native_started=true;strcpy(s_turn.native_job,"legacy-job");restart_reply();assert(!s_turn.native_started&&!s_replaying&&s_turn.gen==2);}
 else if(c=="section_restart"){ack();s_can_restart=true;sections_pending=true;s_turn.native_claimed=true;strcpy(s_turn.native_job,"section-job");strcpy(s_turn.msgs[0].external_key,"partial-cache");restart_reply();assert(s_turn.gen==2&&!s_turn.native_claimed&&!s_turn.native_started&&!s_replaying&&!s_turn.msgs[0].external_key[0]&&s_restart_wait&&cancels==1);}
 else assert(false);
}
'''
class TaskLifecycle(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compiler=shutil.which('c++') or shutil.which('g++') or shutil.which('clang++')
        if not cls.compiler:raise unittest.SkipTest('Native C++ compiler unavailable; Linux CI exercises production lifecycle.')
        cls.tmp=tempfile.TemporaryDirectory();cls.addClassCleanup(cls.tmp.cleanup)
        source=Path(cls.tmp.name)/'lifecycle.cpp';source.write_text(harness(),encoding='utf8');cls.exe=Path(cls.tmp.name)/'lifecycle'
        result=subprocess.run([cls.compiler,'-std=c++17','-O0',str(source),'-o',str(cls.exe)],capture_output=True,text=True)
        if result.returncode:raise RuntimeError(result.stderr)
    def run_case(self,name):subprocess.run([str(self.exe),name],check=True,capture_output=True,text=True)
    def test_restart_resumes_the_section_producer_as_well_as_audio(self):self.case('section_restart')
    def test_work_survives_more_than_twenty_seconds_after_ack(self):self.run_case('busy_gap')
    def test_idle_activity_cannot_finish_running_task(self):self.run_case('idle_activity')
    def test_child_task_completion_preserves_root(self):self.run_case('children')
    def test_unrelated_terminal_status_is_ignored(self):self.run_case('foreign')
    def test_failed_task_after_ack_is_explicit(self):self.run_case('failed')
    def test_overall_deadline_cannot_report_success(self):self.run_case('deadline')
    def test_queued_result_is_not_complete(self):self.run_case('queued_final')
    def test_revised_final_text_replaces_ack_and_refreshes_audio(self):self.run_case('revision')
    def test_duplicate_final_does_not_replay(self):self.run_case('duplicate')
    def test_active_ack_finishes_before_revision(self):self.run_case('active_revision')
    def test_structured_final_and_overflow(self):self.run_case('structured')
    def test_legacy_idle_status_can_finish(self):self.run_case('legacy_idle')
    def test_lower_persisted_sequence_delivers_real_final(self):self.run_case('mixed_sequence')
    def test_live_delta_replay_does_not_duplicate_text(self):self.run_case('stream_replay')
    def test_persisted_duplicate_and_revision(self):self.run_case('durable_duplicate')
    def test_failed_direct_restart_allows_fresh_fetch(self):self.run_case('failed_direct_restart')
    def test_running_direct_restart_rebinds_generation(self):self.run_case('buffered_direct_restart')
    def test_cached_direct_restart_keeps_verified_audio(self):self.run_case('cached_direct_restart')
    def test_legacy_restart_keeps_retry_behavior(self):self.run_case('legacy_restart')
