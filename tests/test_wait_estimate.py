"""Compile and exercise the real countdown and adaptive timing model."""
import shutil, subprocess, tempfile, unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
SOURCE=ROOT/'firmware/overlay/esp32/components/muse'
HARNESS=r'''
#include "muse_wait_estimate.h"
#include <assert.h>
#include <math.h>
#include <string.h>
int main(int argc,char **argv){
 assert(argc==2);muse_wait_model_t model;muse_wait_defaults(&model);
 muse_wait_countdown_t state={0};muse_wait_view_t view;
 if(!strcmp(argv[1],"ready_gate")){
  view=muse_wait_countdown(&state,0,50,false);assert(view.progress_percent==0&&view.seconds==50);
  view=muse_wait_countdown(&state,500,50,false);assert(view.progress_percent<100&&view.late);
  view=muse_wait_countdown(&state,500,50,true);assert(view.progress_percent==100&&!view.late&&view.seconds==0);
 }else if(!strcmp(argv[1],"monotonic")){
  view=muse_wait_countdown(&state,25,50,false);unsigned previous=view.progress_percent;
  assert(previous==50);
  view=muse_wait_countdown(&state,26,100,false);assert(view.progress_percent==previous);
  view=muse_wait_countdown(&state,28,30,false);assert(view.progress_percent>previous);
 }else if(!strcmp(argv[1],"word_count")){
  assert(muse_wait_budget(&model,90)>muse_wait_budget(&model,45));
  assert(muse_wait_budget(&model,4500)==muse_wait_budget(&model,400));
 }else if(!strcmp(argv[1],"learning")){
  float before=model.response_s;muse_wait_learn(&model,60,20,40);
  assert(model.response_s>before&&model.samples==1&&muse_wait_model_valid(&model));
  muse_wait_model_t saved=model;muse_wait_learn(&model,NAN,20,40);assert(!memcmp(&saved,&model,sizeof(model)));
  muse_wait_learn(&model,4,.1f,40);assert(!memcmp(&saved,&model,sizeof(model)));
 }else if(!strcmp(argv[1],"invalid_saved_model")){
  assert(muse_wait_model_valid(&model));model.boot_s=NAN;assert(!muse_wait_model_valid(&model));
  muse_wait_defaults(&model);model.seconds_per_word=99;assert(!muse_wait_model_valid(&model));
 }else if(!strcmp(argv[1],"new_session")){
  muse_wait_countdown(&state,80,50,true);state=(muse_wait_countdown_t){0};
  view=muse_wait_countdown(&state,0,50,false);assert(view.progress_percent==0);
 }else assert(0);
 return 0;
}
'''
class Countdown(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cc=shutil.which('cc') or shutil.which('gcc') or shutil.which('clang')
  if not cc:raise unittest.SkipTest('Linux CI runs the production countdown model.')
  cls.temp=tempfile.TemporaryDirectory();cls.addClassCleanup(cls.temp.cleanup)
  src=Path(cls.temp.name)/'wait.c';src.write_text(HARNESS);cls.exe=src.with_suffix('')
  subprocess.run([cc,'-std=c11','-I',str(SOURCE),str(src),str(SOURCE/'muse_wait_estimate.c'),'-lm','-o',str(cls.exe)],check=True,capture_output=True)
 def run_case(self,name):subprocess.run([str(self.exe),name],check=True,capture_output=True)
 def test_full_progress_is_reserved_for_real_readiness(self):self.run_case('ready_gate')
 def test_revised_estimates_cannot_drain_the_bar(self):self.run_case('monotonic')
 def test_preparation_scales_with_opening_word_count(self):self.run_case('word_count')
 def test_learning_uses_bounded_successes_only(self):self.run_case('learning')
 def test_invalid_saved_models_are_rejected(self):self.run_case('invalid_saved_model')
 def test_new_turn_starts_empty_again(self):self.run_case('new_session')
