"""Exercise production ranged-file buffering, bounds and stale callbacks."""
import shutil,subprocess,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]

def harness():
 s=(ROOT/'firmware/overlay/esp32/components/muse/muse_native_voice.cpp').read_text()
 code=s[s.index('static constexpr size_t FILE_BATCH='):s.index('static int read_file(')]
 return r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#define EXT_RAM_BSS_ATTR
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
static int lock;static char job[25]="1234567890abcdef12345678";static int64_t clock_us;
static int64_t now(){return clock_us;}
static void pause_ms(int n){clock_us+=n*1000;}
static bool stopped(){return false;}
static bool muse_link_req_ready(){return true;}
using Callback=void(*)(void*,int,const uint8_t*,size_t,bool);
static int64_t muse_link_req_open(const char*,const char*,const char *const*,bool,Callback,void*);
static void muse_link_req_cancel(int64_t){}
static std::string range_header;static size_t reply_size;static int reply_status=206;
static Callback last_callback;static void *last_context;
''' + code + r'''
static int64_t muse_link_req_open(const char *verb,const char *path,const char *const *headers,bool,Callback callback,void *context){
 assert(!strcmp(verb,"GET"));assert(std::string(path).find("/fs/raw/muse-storyteller/replies/")==0);
 assert(!strcmp(headers[0],"Range"));range_header=headers[1];last_callback=callback;last_context=context;
 std::vector<uint8_t> data(reply_size,0x4a);size_t half=reply_size/2;
 callback(context,reply_status,data.data(),half,false);callback(context,0,data.data()+half,reply_size-half,true);return 1;
}
int main(int argc,char **argv){assert(argc==2);std::string c=argv[1];uint8_t out[32768];size_t length=0;int status;
 if(c=="batch"){reply_size=32768;status=read_file_once("audio.mp3",65536,out,sizeof(out),length);assert(status==206&&length==32768&&out[32767]==0x4a);assert(range_header=="bytes=65536-98303");}
 else if(c=="small_limit"){reply_size=4097;status=read_file_once("meta.json",0,out,4096,length);assert(status==-1&&length==0);}
 else if(c=="invalid_cap"){assert(read_file_once("audio.mp3",0,out,32769,length)==-1);assert(!file_requests);}
 else if(c=="partial"){reply_size=117;status=read_file_once("audio.mp3",32768,out,sizeof(out),length);assert(status==206&&length==117);}
 else if(c=="stale"){reply_size=10;assert(read_file_once("audio.mp3",0,out,sizeof(out),length)==206);auto previous=last_context;reply_size=20;assert(read_file_once("audio.mp3",0,out,sizeof(out),length)==206);uint8_t bad[3]={};last_callback(previous,503,bad,3,true);assert(file_reply.status==206&&file_reply.length==20);}
 else assert(false);
}
'''

class NativeFileBatches(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cc=shutil.which('c++') or shutil.which('g++') or shutil.which('clang++')
  if not cc:raise unittest.SkipTest('Linux CI checks the production range buffer.')
  cls.temp=tempfile.TemporaryDirectory();cls.addClassCleanup(cls.temp.cleanup)
  src=Path(cls.temp.name)/'batch.cpp';src.write_text(harness());cls.exe=src.with_suffix('')
  subprocess.run([cc,'-std=c++17',str(src),'-o',str(cls.exe)],check=True,capture_output=True)
 def case(self,c):subprocess.run([str(self.exe),c],check=True,capture_output=True)
 def test_full_batch_and_range_header(self):self.case('batch')
 def test_small_requests_keep_their_own_size_limit(self):self.case('small_limit')
 def test_invalid_capacity_never_opens_request(self):self.case('invalid_cap')
 def test_growing_file_can_return_short_batch(self):self.case('partial')
 def test_previous_request_callback_cannot_mutate_new_batch(self):self.case('stale')

if __name__=='__main__':unittest.main()
