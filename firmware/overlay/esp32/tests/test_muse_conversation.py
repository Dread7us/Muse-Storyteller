"""Native tests for complete, bounded, volatile conversation history."""
import os,shlex,subprocess,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
class Conversation(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.tmp=tempfile.TemporaryDirectory();out=Path(cls.tmp.name)
  (out/'test.c').write_text(r'''
#include "muse_conversation_model.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
static muse_conversation_model_t m;
int main(int argc,char **argv) {
 assert(argc==2);
 switch(atoi(argv[1])) {
 case 0: {
  const char *sentence="The beginning of this sentence must remain visible, as well as every word through the very end.";
  assert(muse_conversation_model_add(&m,true,sentence));
  assert(strstr(m.text,sentence));assert(strstr(m.text,"You:\nThe beginning"));
  assert(muse_conversation_model_add(&m,false,"A complete Muse response."));
  assert(strstr(m.text,"Muse:\nA complete Muse response."));
  assert(m.count==2);break;
 }
 case 1: {
  static char reply[8193];memset(reply,'x',8192);reply[8192]=0;
  for(int i=0;i<7;i++)assert(muse_conversation_model_add(&m,false,reply));
  assert(m.len<sizeof(m.text));assert(m.count==3);
  for(unsigned i=0,at=0;i<m.count;i++) {assert(!memcmp(m.text+at,"Muse:\n",6));at+=(unsigned)m.lengths[i];}
  assert(m.text[m.len]==0);assert(m.ids[0]==5 && m.ids[2]==7 && m.serial==7);break;
 }
 case 2: {
  for(int i=0;i<8;i++)assert(muse_conversation_model_add(&m,true,i==0?"oldest":"retained"));
  assert(muse_conversation_model_add(&m,false,"newest"));
  assert(m.count==8);assert(!strstr(m.text,"oldest"));assert(strstr(m.text,"newest"));break;
 }
 case 3: {
  assert(muse_conversation_model_add(&m,true,"Hello \xf0\x9f\x8c\x99 and caf\xc3\xa9."));
  assert(strstr(m.text,"\xf0\x9f\x8c\x99"));
  size_t before=m.len;assert(!muse_conversation_model_add(&m,false,""));assert(m.len==before);
  static char huge[8194];memset(huge,'z',8193);huge[8193]=0;
  assert(!muse_conversation_model_add(&m,false,huge));assert(m.len==before);break;
 }
 default:return 2;
 }
 return 0;
}
''',encoding='utf-8')
  cls.binary=out/'conversation'
  cmd=[*shlex.split(os.environ.get('CC','cc')),'-std=gnu11','-Wall','-Wextra','-Werror','-I',str(ROOT/'components/muse'),str(out/'test.c'),str(ROOT/'components/muse/muse_conversation_model.c'),'-o',str(cls.binary)]
  r=subprocess.run(cmd,capture_output=True,text=True)
  if r.returncode:raise AssertionError(r.stdout+r.stderr)
 @classmethod
 def tearDownClass(cls):cls.tmp.cleanup()
 def run_case(self,n):
  r=subprocess.run([str(self.binary),str(n)],capture_output=True,text=True)
  self.assertEqual(r.returncode,0,r.stdout+r.stderr)
 def test_complete_utterance_and_reply(self):self.run_case(0)
 def test_large_replies_evict_whole_messages(self):self.run_case(1)
 def test_recent_message_limit(self):self.run_case(2)
 def test_utf8_and_oversized_message_isolation(self):self.run_case(3)
if __name__=='__main__':unittest.main()
