"""Optional analysis cannot hold the voice router's GIL or wait forever."""
import json,os,sys,threading,time,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'module/runtime'))
from muse_alignment_worker import AlignmentWorker,AlignmentError

@unittest.skipUnless(os.name=='posix','Module process/pipe checks run on Linux')
class AlignmentIsolation(unittest.TestCase):
 def worker(self,body,timeout=2):
  return AlignmentWorker([sys.executable,'-u','-c','import sys,json,base64,time\nprint(json.dumps({"ready":True}),flush=True)\n'+body],timeout=timeout)
 def test_pcm_and_unicode_text_round_trip(self):
  w=self.worker('r=json.loads(sys.stdin.readline());assert base64.b64decode(r["pcm"])==b"\\x01\\x00"*160;assert r["text"]=="Lantern—blue";print(json.dumps({"words":[[0.0,0.1,0,7]]}),flush=True)')
  self.addCleanup(w.close);self.assertEqual(w.align(b'\1\0'*160,'Lantern—blue'),[[0.,.1,0,7]])
 def test_native_work_cannot_block_parent_threads_and_timeout_is_bounded(self):
  w=self.worker('sys.stdin.readline()\nwhile True:sum(range(100000))',timeout=.5)
  self.addCleanup(w.close);ticks=[];end=threading.Event()
  def pulse():
   while not end.wait(.01):ticks.append(1)
  thread=threading.Thread(target=pulse);thread.start();start=time.monotonic()
  try:
   with self.assertRaises(AlignmentError):w.align(b'\0\0'*160,'Blue lantern.')
  finally:end.set();thread.join(2)
  self.assertGreater(len(ticks),10);self.assertLess(time.monotonic()-start,2);self.assertIsNotNone(w.process.poll())
 def test_oversized_reply_is_rejected_and_worker_closed(self):
  w=self.worker('sys.stdin.readline();sys.stdout.write("x"*140000);sys.stdout.flush()');self.addCleanup(w.close)
  with self.assertRaises(AlignmentError):w.align(b'\0\0'*160,'Blue.')
  self.assertIsNotNone(w.process.poll())
 def test_invalid_window_does_not_enter_worker(self):
  w=self.worker('time.sleep(5)');self.addCleanup(w.close)
  for pcm in (b'',b'x',b'\0'*960002):
   with self.assertRaises(AlignmentError):w.align(pcm,'Blue.')
 def test_malformed_alignment_is_rejected(self):
  w=self.worker('sys.stdin.readline();print(json.dumps({"words":[[1,2]]}),flush=True)');self.addCleanup(w.close)
  with self.assertRaises(AlignmentError):w.align(b'\0\0'*160,'Blue.')

if __name__=='__main__':unittest.main()
