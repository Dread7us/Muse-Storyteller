"""Run the production queued-request recovery with a fake clock and gateway."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'firmware/overlay/esp32/components/muse'


def harness():
    source = (SOURCE / 'muse_chat_session.cpp').read_text(encoding='utf8')
    recovery = source[source.index('/* Begin queued connection recovery.'):
                      source.index('/* End queued connection recovery. */')]
    clear = source[source.index('static void connection_retry_clear(void)'):
                   source.index('\n#endif', source.index('static void connection_retry_clear(void)'))]
    api = source[source.index('extern "C" void muse_chat_retry_status('):
                 source.index('extern "C" bool muse_chat_bench_retry(')]
    return r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "muse_connection_retry.h"
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
enum {P_IDLE,P_CONNECTING,P_WAIT_REPLY,MUSE_CHAT_WAIT_CONNECTING,MUSE_HATCH_EV_HEARD,CMD_RETRY_CONNECTION,CMD_WAKE};
static const int pdTRUE=1;
struct muse_chat_retry_status_t {bool active=false,connecting=false,sent=false,blocked=false,cancelling=false;unsigned attempts=0,seconds=0,http_status=0;};
struct cmd_t {int type;unsigned gen;char *text;};
static muse_chat_retry_status_t s_retry;
static int s_retry_lock,s_chat_wait,s_connect_http_status;
static unsigned s_bench_connection_failures;
static char *s_unsent_request;
static int64_t s_retry_at_us,s_auto_next_us,clock_us;
static std::atomic<unsigned> s_gen{1},s_cancel_wait_generation{0};
static bool s_vm_direct=false,s_chat_wifi_held=false,configured=true,wifi=true,cancel_during_connect=false,queue_full=false;
static void *s_cmds=(void *)1;
struct {int phase=P_IDLE;unsigned gen=1;int64_t start_us=0,history_next_us=0;} s_turn;
static unsigned posts,connects,wifi_applies,refreshes,wakes;
static std::string posted;
static std::vector<int> outcomes;
static int64_t now_us(){return clock_us;}
static uint32_t esp_random(){return 12345;}
static bool muse_hatch_configured(){return configured;}
static bool muse_wifi_connected(){return wifi;}
static void muse_wifi_apply(){++wifi_applies;}
static void muse_wifi_keep_awake(bool on){s_chat_wifi_held=on;}
static void emit(int,const char*){}
static void forget_vm(){++refreshes;}
extern "C" bool muse_chat_cancel_wait(void);
static bool ensure_connected(){
 ++connects;assert(!outcomes.empty());int status=outcomes.front();outcomes.erase(outcomes.begin());
 s_connect_http_status=status==200?0:status;
 if(cancel_during_connect)muse_chat_cancel_wait();
 return status==200;
}
static bool xQueueSend(void*,const cmd_t *cmd,int){if(cmd->type==CMD_WAKE)++wakes;return !queue_full;}
''' + clear + r'''
static void turn_finish(){connection_retry_clear();s_turn.phase=P_IDLE;s_chat_wifi_held=false;}
static void post_chat(const char *text){assert(text);++posts;posted=text;s_turn.phase=P_WAIT_REPLY;}
''' + recovery + api + r'''
static void start(const char *text="Summarize the test fixture."){
 turn_finish();s_turn.gen=++s_gen;queue_connection_request(strdup(text));
}
static void due(){clock_us=s_retry_at_us;pending_connection_tick();}
int main(int argc,char **argv){assert(argc==2);std::string test=argv[1];
 if(test=="backoff"){
  unsigned previous=0;
  for(unsigned n=1;n<100;++n){unsigned low=muse_connection_retry_delay_ms(n,0),high=muse_connection_retry_delay_ms(n,low);
   assert(low>=previous&&low>=1000&&low<=30000);assert(high>=low&&high<=60000);previous=low;}
  assert(muse_connection_retry_delay_ms(1000000,0)==30000);
  for(int s:{0,408,425,429,500,502,503,504,599})assert(muse_connection_retry_automatic(s));
  for(int s:{400,401,403,404,409,422,600})assert(!muse_connection_retry_automatic(s));
 }
 else if(test=="recover"){
  start();outcomes={502,503,0,429,200};
  for(unsigned n=1;n<=4;++n){due();assert(posts==0&&connects==n&&s_retry.attempts==n&&!s_retry.connecting);
   int64_t at=s_retry_at_us;assert(at>clock_us);pending_connection_tick();assert(connects==n);clock_us=at-1;pending_connection_tick();assert(connects==n);}
  due();assert(posts==1&&posted=="Summarize the test fixture."&&s_turn.phase==P_WAIT_REPLY&&!s_retry.active&&!s_unsent_request);
  assert(refreshes==1);
 }
 else if(test=="retry_now"){
  start();outcomes={502,200};due();int64_t later=s_retry_at_us;assert(later>clock_us);
  connection_retry_now();assert(!s_retry_at_us);pending_connection_tick();assert(posts==1);
 }
 else if(test=="cancel_before"){
  start();outcomes={200};assert(muse_chat_cancel_wait());pending_connection_tick();
  assert(posts==0&&connects==0&&s_turn.phase==P_IDLE&&!s_unsent_request&&s_cancel_wait_generation.load());
 }
 else if(test=="cancel_during"){
  start();outcomes={200};cancel_during_connect=true;pending_connection_tick();
  assert(connects==1&&posts==0&&s_turn.phase==P_IDLE&&!s_unsent_request);
 }
 else if(test=="cancel_full_queue"){
  start();queue_full=true;assert(muse_chat_cancel_wait());assert(s_cancel_wait_generation.load()==s_gen.load());
  pending_connection_tick();assert(posts==0&&!s_unsent_request);
 }
 else if(test=="auth"){
  start();outcomes={403,200};due();assert(s_retry.blocked&&!posts);clock_us+=100000000;pending_connection_tick();assert(connects==1);
  connection_retry_now();pending_connection_tick();assert(posts==1);
 }
 else if(test=="wifi"){
  start();wifi=false;due();assert(wifi_applies==1&&!connects&&!posts&&s_retry.active);
  wifi=true;outcomes={200};due();assert(posts==1);
 }
 else if(test=="replace"){
  start("Old task");outcomes={502,200};due();start("New task");due();assert(posts==1&&posted=="New task");
 }
 else if(test=="countdown"){
  start();outcomes={502};due();muse_chat_retry_status_t status;muse_chat_retry_status(&status);
  assert(status.active&&status.http_status==502&&status.seconds>0);clock_us=s_retry_at_us; muse_chat_retry_status(&status);
  assert(status.seconds==0);
 }
 else assert(false);
}
'''


class ConnectionRetry(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('c++') or shutil.which('g++') or shutil.which('clang++')
        msvc = shutil.which('cl') if not compiler else None
        compiler = compiler or msvc
        if not compiler:
            raise unittest.SkipTest('Native compiler unavailable; Linux CI runs the recovery harness.')
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        source = Path(cls.tmp.name) / 'connection.cpp'
        source.write_text(harness(), encoding='utf8')
        cls.exe = Path(cls.tmp.name) / ('connection.exe' if msvc else 'connection')
        command = ([compiler, '/nologo', '/std:c++17', '/EHsc', f'/I{SOURCE}', f'/Fe:{cls.exe}', str(source)]
                   if msvc else [compiler, '-std=c++17', '-I', str(SOURCE), str(source), '-o', str(cls.exe)])
        result = subprocess.run(command, cwd=cls.tmp.name, capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

    def run_case(self, case):
        subprocess.run([str(self.exe), case], check=True, capture_output=True, text=True)

    def test_backoff_has_jitter_a_cap_and_http_classification(self): self.run_case('backoff')
    def test_multiple_transient_failures_submit_only_once_after_connection(self): self.run_case('recover')
    def test_retry_now_skips_the_scheduled_wait(self): self.run_case('retry_now')
    def test_cancel_before_connection_never_submits(self): self.run_case('cancel_before')
    def test_cancel_during_connection_never_submits(self): self.run_case('cancel_during')
    def test_cancel_survives_a_full_command_queue(self): self.run_case('cancel_full_queue')
    def test_auth_refusal_waits_for_manual_retry(self): self.run_case('auth')
    def test_wifi_recovery_keeps_the_unsent_request(self): self.run_case('wifi')
    def test_a_new_question_replaces_the_old_unsent_question(self): self.run_case('replace')
    def test_countdown_uses_the_actual_deadline(self): self.run_case('countdown')
