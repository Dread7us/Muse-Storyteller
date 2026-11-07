"""Exercise the real pause acknowledgment handler, including sentence waits."""
import shutil,subprocess,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def harness():
 source=(ROOT/'firmware/overlay/esp32/components/muse/muse_module_llm.c').read_text()
 code=source[source.index('static void playback_pause_update('):source.index('static void on_frame(')]
 recovery=source[source.index('static bool resume_recovery_due('):source.index('static void playback_controls(')]
 return r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
enum {MUSE_MODULE_LLM_PREPARING=3,MUSE_MODULE_LLM_SPEAKING=4};
struct runtime_t {int64_t pause_since=0,buffering_since=0,deadline=1000;bool speaking=true;};
static int s_lock,status=-1;static const char *detail="";
static struct {bool paused=false;} s_status;
static void set_status(int value,const char *text){status=value;detail=text;}
''' + code + recovery + r'''
int main(int argc,char **argv){assert(argc==2);runtime_t r;
 if(!strcmp(argv[1],"pause")){playback_pause_update(&r,true,100);assert(s_status.paused&&r.pause_since==100&&status==4);}
 else if(!strcmp(argv[1],"resume")){r.pause_since=100;playback_pause_update(&r,false,500);assert(!s_status.paused&&!r.pause_since&&r.deadline==1400&&status==4);}
 else if(!strcmp(argv[1],"resume_buffering")){r.pause_since=100;r.buffering_since=90;playback_pause_update(&r,false,500);assert(!s_status.paused&&status==3&&!strcmp(detail,"BUFFERING NEXT SENTENCE..."));}
 else if(!strcmp(argv[1],"pause_buffering")){r.buffering_since=90;playback_pause_update(&r,true,100);assert(s_status.paused&&status==3&&!strcmp(detail,"PAUSED"));}
 else if(!strcmp(argv[1],"duplicate_ack")){r.pause_since=100;playback_pause_update(&r,false,500);playback_pause_update(&r,false,600);assert(r.deadline==1400);}
 else if(!strcmp(argv[1],"before_playback")){r.speaking=false;playback_pause_update(&r,false,100);assert(status==-1);}
 else if(!strcmp(argv[1],"resume_recovery")){assert(resume_recovery_due(true,false,100,2000100,0,0));}
 else if(!strcmp(argv[1],"recovery_limit")){assert(!resume_recovery_due(true,false,100,2000100,2,0));assert(!resume_recovery_due(true,false,100,2000100,1,3000000));}
 else if(!strcmp(argv[1],"no_false_recovery")){assert(!resume_recovery_due(false,true,100,2000100,0,0));assert(!resume_recovery_due(true,true,100,2000100,0,0));}
 else if(!strcmp(argv[1],"recovery_wait")){assert(!resume_recovery_due(true,false,0,2000100,0,0));assert(!resume_recovery_due(true,false,100,2000099,0,0));}
 else assert(0);
}
'''
class PauseStates(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cc=shutil.which('c++') or shutil.which('g++') or shutil.which('clang++')
  if not cc:raise unittest.SkipTest('Linux CI runs the production pause handler.')
  cls.temp=tempfile.TemporaryDirectory();cls.addClassCleanup(cls.temp.cleanup)
  src=Path(cls.temp.name)/'pause.cpp';src.write_text(harness());cls.exe=src.with_suffix('')
  subprocess.run([cc,'-std=c++17',str(src),'-o',str(cls.exe)],check=True,capture_output=True)
 def run_case(self,name):subprocess.run([str(self.exe),name],check=True,capture_output=True)
 def test_pause_acknowledges_the_audio_state(self):self.run_case('pause')
 def test_resume_extends_the_deadline_once(self):self.run_case('resume')
 def test_resume_while_buffering_cannot_claim_speaking(self):self.run_case('resume_buffering')
 def test_pause_while_buffering_keeps_preparing(self):self.run_case('pause_buffering')
 def test_duplicate_ack_cannot_extend_the_deadline_twice(self):self.run_case('duplicate_ack')
 def test_ack_before_audio_cannot_claim_speaking(self):self.run_case('before_playback')
 def test_stuck_resume_uses_bounded_recovery(self):self.run_case('resume_recovery')
 def test_control_recovery_has_attempt_and_cooldown_limits(self):self.run_case('recovery_limit')
 def test_matching_state_or_unacknowledged_pause_cannot_trigger_resume_recovery(self):self.run_case('no_false_recovery')
 def test_control_recovery_waits_for_a_real_pending_transition(self):self.run_case('recovery_wait')
