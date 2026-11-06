"""Exercise production idle recovery guards and serialized UART burst pacing."""
import json,shutil,subprocess,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def harness():
    source=(ROOT/'firmware/overlay/esp32/components/muse/muse_module_llm.c').read_text()
    send=source[source.index('bool muse_module_llm_send_line('):source.index('void muse_module_llm_session(')]
    safe=source[source.index('static const char *uart_safe_json('):source.index('bool muse_module_llm_send_line(')]
    deadline=source[source.index('static void check_deadline('):source.index('static void runtime_task(')]
    policy=source[source.index('static bool transfer_recovery_allowed('):source.index('static void check_deadline(')]
    return r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
enum {OP_NONE,OP_HELLO,OP_STATUS,OP_LISTEN_BEGIN,OP_LISTEN,OP_LISTEN_END,OP_SPEAK,OP_CANCEL,MUSE_MODULE_LLM_DISCOVERING};
struct runtime_t {int operation=OP_NONE;int64_t pause_since=0,deadline=0,retry_at=0,next_framing_recovery=0,stable_ready_since=0;unsigned framing_recoveries=0;bool stop_unconfirmed=false;};
static bool s_ready=true,protected_recorder=false;static int s_lock=0,status=-1,recovered=0,ended=0,cancelled=0;
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
#define ESP_LOGW(...) ((void)0)
static const char *TAG="test";
static void end_listening(runtime_t*){ended++;}
static void fail(runtime_t *r,const char*){r->operation=OP_NONE;r->deadline=0;}
static void set_status(int s,const char*){status=s;}
static const char *waiting_detail(runtime_t*,int64_t){return "Loading";}
static bool muse_memory_recorder_active(){return protected_recorder;}
static void recover_uart_framing(){recovered++;}
static void set_playback(int){}
#define MUSE_MODULE_LLM_PLAYBACK_FAILED 10
static void cancel_operation(runtime_t*,bool){cancelled++;}
''' + policy + deadline + r'''
static bool s_tx_mutex=true;static std::string wire;static std::vector<size_t> bursts;static int waits=0,fail_write=-1;
#define portMAX_DELAY 0
#define CONFIG_MUSE_MODULE_LLM_UART_NUM 1
#define ESP_OK 0
#define pdMS_TO_TICKS(x) (x)
static void xSemaphoreTake(bool,int){}
static void xSemaphoreGive(bool){}
static int uart_write_bytes(int,const char *s,size_t n){bursts.push_back(n);if((int)bursts.size()==fail_write)return -1;wire.append(s,n);return (int)n;}
static int uart_wait_tx_done(int,int){waits++;return ESP_OK;}
static void vTaskDelay(int){}
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
static char *heap_caps_malloc(size_t n,int){return (char*)malloc(n);}
''' + safe + send + r'''
int main(int argc,char **argv){assert(argc==2);std::string c=argv[1];runtime_t r;r.deadline=100;r.operation=OP_HELLO;
 if(c=="hello"){check_deadline(&r,100);assert(recovered==1&&r.framing_recoveries==1&&!s_ready&&r.retry_at>100);}
 else if(c=="status"){r.operation=OP_STATUS;check_deadline(&r,100);assert(!recovered&&!s_ready);}
 else if(c=="recorder"){protected_recorder=true;check_deadline(&r,100);assert(!recovered&&!r.framing_recoveries);}
 else if(c=="paused"){r.operation=OP_SPEAK;r.pause_since=1;check_deadline(&r,100);assert(!recovered&&!cancelled&&r.operation==OP_SPEAK);}
 else if(c=="listen"){r.operation=OP_LISTEN;check_deadline(&r,100);assert(ended==1&&!recovered);}
 else if(c=="speaking"){r.operation=OP_SPEAK;check_deadline(&r,100);assert(cancelled==1&&!recovered);}
 else if(c=="cap"){r.framing_recoveries=2;check_deadline(&r,100);assert(!recovered);}
 else if(c=="cooldown"){r.next_framing_recovery=200;check_deadline(&r,100);assert(!recovered);}
 else if(c=="early"){check_deadline(&r,99);assert(!recovered&&r.operation==OP_HELLO&&s_ready);}
 else if(c=="transfer_policy"){assert(transfer_recovery_allowed(OP_NONE,false)&&transfer_recovery_allowed(OP_STATUS,false));assert(!transfer_recovery_allowed(OP_NONE,true)&&!transfer_recovery_allowed(OP_SPEAK,false)&&!transfer_recovery_allowed(OP_LISTEN_BEGIN,false)&&!transfer_recovery_allowed(OP_LISTEN,false));}
 else if(c=="transfer_recorder"){protected_recorder=true;assert(!transfer_recovery_allowed(OP_NONE,false)&&!transfer_recovery_allowed(OP_HELLO,false));}
 else if(c=="pacing"){for(size_t n:{2,147,179,900,1024,5000}){wire.clear();bursts.clear();waits=0;std::string payload(n,'a');assert(muse_module_llm_send_line(payload.c_str()));assert(wire==payload+"\n");assert(*std::max_element(bursts.begin(),bursts.end())<=128);assert(waits==(int)((n+127)/128));}}
 else if(c=="write_failure"){fail_write=2;std::string payload(900,'a');assert(!muse_module_llm_send_line(payload.c_str()));assert(wire.size()==128&&bursts.size()==2);}
 else if(c=="brace_roundtrip"){std::string payload=R"({"data":{"text":"An unmatched { and escaped \"quote\" and a literal backslash \\{."}})";assert(muse_module_llm_send_line(payload.c_str()));std::cout<<wire;}
 else assert(false);
}
'''
class UartRecovery(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler=shutil.which('c++') or shutil.which('g++') or shutil.which('clang++')
        if not compiler:raise unittest.SkipTest('Native compiler unavailable; Linux CI exercises production UART guards.')
        cls.tmp=tempfile.TemporaryDirectory();cls.addClassCleanup(cls.tmp.cleanup)
        src=Path(cls.tmp.name)/'uart.cpp';src.write_text(harness());cls.exe=Path(cls.tmp.name)/'uart'
        r=subprocess.run([compiler,'-std=c++17','-O0',str(src),'-o',str(cls.exe)],capture_output=True,text=True)
        if r.returncode:raise RuntimeError(r.stderr)
    def case(self,name):subprocess.run([str(self.exe),name],check=True,capture_output=True,text=True)
    def test_idle_hello_timeout_recovers_once(self):self.case('hello')
    def test_status_timeout_first_retries_hello(self):self.case('status')
    def test_recorder_is_always_protected(self):self.case('recorder')
    def test_paused_playback_is_protected(self):self.case('paused')
    def test_listening_uses_its_existing_finalization(self):self.case('listen')
    def test_playback_failure_uses_cancellation(self):self.case('speaking')
    def test_recovery_attempt_cap(self):self.case('cap')
    def test_recovery_cooldown(self):self.case('cooldown')
    def test_no_recovery_before_deadline(self):self.case('early')
    def test_transfer_recovery_yields_to_capture_and_queued_commands(self):self.case('transfer_policy')
    def test_transfer_recovery_protects_recorder(self):self.case('transfer_recorder')
    def test_small_and_large_json_are_paced_without_byte_changes(self):self.case('pacing')
    def test_failed_write_stops_before_next_frame(self):self.case('write_failure')
    def test_json_string_braces_round_trip_without_vendor_nesting(self):
        result=subprocess.run([str(self.exe),'brace_roundtrip'],check=True,capture_output=True,text=True)
        self.assertEqual(json.loads(result.stdout)['data']['text'],r'An unmatched { and escaped "quote" and a literal backslash \{.')
        self.assertEqual(result.stdout.count('{'),2);self.assertEqual(result.stdout.count('}'),2)
        self.assertIn(r'\u007b',result.stdout)
