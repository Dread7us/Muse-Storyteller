"""Exercise the production reconnect dispatcher without a new task stack."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
SOURCE=ROOT/'firmware/overlay/esp32/main/app.c'

def harness():
    text=SOURCE.read_text(encoding='utf8')
    types=text[text.index('typedef struct {\n    const char *want_vm;'):text.index('static const vm_info_t *find_vm_by_id(')]
    dispatch='#if CONFIG_MUSE_ENABLED\n'+text[text.index('static _Atomic(hatch_vm_request_t *)'):text.index('#endif  // CONFIG_MUSE_ENABLED || CONFIG_HOMEHUB_VOICE')]
    return r'''
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <string>
using namespace std;
#define _Atomic(T) std::atomic<T>
#define CONFIG_MUSE_ENABLED 1
#define pdTRUE 1
#define portMAX_DELAY 999999
using TaskHandle_t=int;
static unsigned calls,waits,signals;
static int s_muse_joined=1;
static TaskHandle_t xTaskGetCurrentTaskHandle(){return 7;}
'''+types+r'''
static void hatch_vm_run(void *arg){auto *r=static_cast<hatch_vm_request_t*>(arg);++calls;assert(r->waiter==7);snprintf(r->vm_id,r->id_cap,"owned-vm");snprintf(r->vm_name,r->name_cap,"Muse");r->vm_token=strdup("test-fixture");r->ok=true;}
static void muse_process_vm_lookup();
static void xSemaphoreGive(int){++signals;muse_process_vm_lookup();}
static unsigned ulTaskNotifyTake(int,int){++waits;assert(calls==1);return 1;}
'''+dispatch+r'''
int main(int argc,char **argv){assert(argc==2);string name=argv[1];char id[128]={},title[128]={};char *token=nullptr;
 if(name=="once"){assert(app_hatch_vm_credentials("owned-vm",id,sizeof id,title,sizeof title,&token));assert(!strcmp(id,"owned-vm")&&!strcmp(title,"Muse")&&!strcmp(token,"test-fixture")&&calls==1&&waits==1&&signals==1);muse_process_vm_lookup();assert(calls==1);free(token);}
 else if(name=="busy"){hatch_vm_request_t busy{};s_vm_lookup_pending.store(&busy);assert(!app_hatch_vm_credentials("owned-vm",id,sizeof id,title,sizeof title,&token)&&!calls&&!waits&&s_vm_lookup_pending.load()==&busy);}
 else if(name=="boot"){s_muse_joined=0;assert(!app_hatch_vm_credentials("owned-vm",id,sizeof id,title,sizeof title,&token)&&!calls&&!waits);}
 else if(name=="invalid"){assert(!app_hatch_vm_credentials("owned-vm",nullptr,sizeof id,title,sizeof title,&token)&&!calls&&!waits);assert(!app_hatch_vm_credentials("owned-vm",id,0,title,sizeof title,&token));assert(!app_hatch_vm_credentials("owned-vm",id,sizeof id,title,sizeof title,nullptr));}
 else assert(false);
}
'''

class VmLookupWorker(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler=shutil.which('c++') or shutil.which('g++') or shutil.which('clang++')
        msvc=shutil.which('cl') if not compiler else None
        compiler=compiler or msvc
        if not compiler:raise unittest.SkipTest('Native compiler unavailable; Linux CI runs dispatcher checks.')
        cls.tmp=tempfile.TemporaryDirectory();cls.addClassCleanup(cls.tmp.cleanup)
        src=Path(cls.tmp.name)/'worker.cpp';src.write_text(harness(),encoding='utf8')
        cls.exe=Path(cls.tmp.name)/('worker.exe' if msvc else 'worker')
        cmd=([compiler,'/nologo','/std:c++20','/EHsc',f'/Fe:{cls.exe}',str(src)] if msvc else [compiler,'-std=c++20',str(src),'-o',str(cls.exe)])
        result=subprocess.run(cmd,cwd=cls.tmp.name,capture_output=True,text=True)
        if result.returncode:raise RuntimeError(result.stdout+result.stderr)
    def case(self,name):subprocess.run([str(self.exe),name],check=True,capture_output=True,text=True)
    def test_lookup_completes_once_without_allocating_a_task(self):self.case('once')
    def test_busy_queue_preserves_its_existing_job(self):self.case('busy')
    def test_boot_not_ready_does_not_wait(self):self.case('boot')
    def test_invalid_output_buffers_do_not_start_a_lookup(self):self.case('invalid')
