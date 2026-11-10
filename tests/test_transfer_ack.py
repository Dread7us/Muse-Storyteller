"""Production acknowledgement fence for resumed native audio downloads."""
import re, shutil, subprocess, tempfile, unittest
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
SOURCE=(ROOT/'firmware/overlay/esp32/components/muse/muse_native_voice.cpp').read_text()

def commit_harness():
    start=SOURCE.index('static bool finish_section(')
    production=SOURCE[start:SOURCE.index('static bool hex_digest(',start)]
    budget=SOURCE[SOURCE.index('struct SectionRetryBudget {'):SOURCE.index('static void worker(')]
    wait=SOURCE[SOURCE.index('struct SectionWait {'):SOURCE.index('static bool transfer_sections(')]
    return r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
struct cJSON {bool status;};
struct Reply {bool status,ok;int error;unsigned sections;size_t bytes;};
static std::vector<Reply> replies;
static size_t next=0,rpc_prefix_bytes=0;
static unsigned rpc_ready_sections=0,recoveries=0;
static bool rpc_bad_frame=false;
static int rpc_error=0;
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
static cJSON *cJSON_Duplicate(cJSON *p,bool){return new cJSON(*p);}
static void cJSON_Delete(cJSON *p){delete p;}
static cJSON *operation(const char *op){assert(!strcmp(op,"series_status"));return new cJSON{true};}
static bool stopped(){return false;}
static bool recover_transfer_frame(){recoveries++;return true;}
static bool rpc(cJSON *p,bool cleanup,unsigned tries){
    assert(!cleanup&&tries==1&&next<replies.size());
    auto r=replies[next++];assert(p->status==r.status);delete p;
    rpc_error=r.error;rpc_ready_sections=r.sections;rpc_prefix_bytes=r.bytes;
    return r.ok;
}
''' + production + budget + wait + r'''
int main(){
    replies={{false,false,-1,0,0},{true,true,0,3,4608}};
    assert(finish_section(new cJSON{false},2,4608)&&next==2&&recoveries==1);
    replies={{false,false,-1,0,0},{true,true,0,2,3072},{false,true,0,0,0}};next=0;
    assert(finish_section(new cJSON{false},2,4608)&&next==3);
    replies={{false,false,-1,0,0},{true,true,0,3,3072},{false,false,-35,0,0}};next=0;
    assert(!finish_section(new cJSON{false},2,4608)&&next==3);

    SectionRetryBudget retry;
    assert(retry.allow(2)&&retry.allow(2)&&!retry.allow(2));
    assert(retry.allow(4)&&retry.allow(4)&&!retry.allow(3));
    assert(retry.allow(5)&&retry.allow(8)&&retry.allow(12));
    for(unsigned i=13;retry.total<16;i++)assert(retry.allow(i));
    assert(!retry.allow(20));

    // A silent producer cannot renew the same section's deadline.
    const int64_t start=90000000;
    SectionWait missing(start,false);
    assert(!missing.rechecking(start)&&missing.rechecking(start+5000000));
    assert(!missing.expired(start+17999999)&&missing.expired(start+18000000));
    assert(missing.request_deadline(start+17500000)==start+18000000);
    // Consecutive holes are probed separately with a shorter wait.
    SectionWait next_missing(start+18000000,true);
    assert(next_missing.rechecking(start+18000000));
    assert(next_missing.request_deadline(start+18000000)==start+20500000);
    assert(next_missing.expired(start+20500000));
}
'''

class TransferAcknowledgements(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler=shutil.which('c++') or shutil.which('g++') or shutil.which('clang++')
        if not compiler:raise unittest.SkipTest('Native compiler unavailable; Linux CI checks acknowledgement fences.')
        cls.tmp=tempfile.TemporaryDirectory();cls.addClassCleanup(cls.tmp.cleanup)
        function=SOURCE[SOURCE.index('static bool native_block_ack_valid('):SOURCE.index('static bool recover_transfer_frame(')]
        harness='#include <cstddef>\n#include <cassert>\n'+function+r'''
int main(){
    // New uploads and blocks beyond an existing prefix require exact progress.
    assert(native_block_ack_valid(1536,1536,0));
    assert(!native_block_ack_valid(3072,1536,0));
    assert(!native_block_ack_valid(1024,1536,0));
    // The receiver returns its entire retained prefix for matching duplicates.
    assert(native_block_ack_valid(28672,1536,28672));
    assert(native_block_ack_valid(28672,27648,28672));
    assert(native_block_ack_valid(28672,28672,28672));
    assert(native_block_ack_valid(29184,29184,28672));
    // Neither arbitrary greater offsets nor a stale prefix count as an ACK.
    assert(!native_block_ack_valid(30000,1536,28672));
    assert(!native_block_ack_valid(27648,1536,28672));
    assert(!native_block_ack_valid(28672,29184,28672));
    // Coalesce network frames but stop exactly at any retained-prefix edge.
    assert(native_block_length(4096,1536,27648,28672,false)==1024);
    assert(native_block_length(4096,1536,28672,28672,false)==1536);
    assert(native_block_length(2048,1536,3072,4096,false)==1024);
    assert(native_block_length(700,1536,0,0,false)==0);
    assert(native_block_length(700,1536,0,0,true)==700);
    assert(native_block_length(0,1536,0,0,true)==0);
}
'''
        src=Path(cls.tmp.name)/'ack.cpp';src.write_text(harness);cls.exe=Path(cls.tmp.name)/'ack'
        subprocess.run([compiler,'-std=c++17',str(src),'-o',str(cls.exe)],check=True,capture_output=True)
        src=Path(cls.tmp.name)/'commit.cpp';src.write_text(commit_harness());cls.commit=src.with_suffix('')
        subprocess.run([compiler,'-std=c++17',str(src),'-o',str(cls.commit)],check=True,capture_output=True)
    def test_exact_and_retained_prefix_acknowledgements(self):
        subprocess.run([str(self.exe)],check=True,capture_output=True)
    def test_lost_confirmation_and_retry_budget_follow_verified_progress(self):
        subprocess.run([str(self.commit)],check=True,capture_output=True)

class PacketQueueBudget(unittest.TestCase):
    def test_audio_packet_leaves_room_in_module_tty_queue(self):
        # Production capacity, worst-case offset and metadata. The existing
        # round-trip tests separately check admission and damaged-body rejection.
        import base64, hashlib, json
        capacity=int(re.search(r'direct_block\[(\d+)\]',SOURCE).group(1))
        body=bytes(range(256))*(capacity//256);self.assertEqual(len(body),capacity)
        info=dict(session='f'*8,generation=0,key='f'*24,offset=32*1024*1024,
                  bytes=len(body),sha256=hashlib.sha256(body).hexdigest(),packet_sha256='f'*64)
        request=dict(request_id='cloud-'+'f'*8+'-'+'f'*8,work_id='musevoice',
                     object='musevoice.v1',action='cloud',raw_info=info,data=base64.b64encode(body).decode())
        self.assertLessEqual(len(json.dumps(request).encode())+1,3072)

if __name__=='__main__':unittest.main()
