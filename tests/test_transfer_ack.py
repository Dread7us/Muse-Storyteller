"""Production acknowledgement fence for resumed native audio downloads."""
import re, shutil, subprocess, tempfile, unittest
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
SOURCE=(ROOT/'firmware/overlay/esp32/components/muse/muse_native_voice.cpp').read_text()

class TransferAcknowledgements(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler=shutil.which('c++') or shutil.which('g++') or shutil.which('clang++')
        if not compiler:raise unittest.SkipTest('Native compiler unavailable; Linux CI checks acknowledgement fences.')
        cls.tmp=tempfile.TemporaryDirectory();cls.addClassCleanup(cls.tmp.cleanup)
        function=SOURCE[SOURCE.index('static bool native_block_ack_valid('):SOURCE.index('static bool rpc_binary_block(')]
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
    def test_exact_and_retained_prefix_acknowledgements(self):
        subprocess.run([str(self.exe)],check=True,capture_output=True)

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
