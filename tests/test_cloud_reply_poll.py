"""Queued and in-flight audio packets get prompt replies; idle remains paced."""
import sys,threading,types,unittest
from pathlib import Path
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'module/runtime'))
import muse_cloud_worker as cloud

class ReplyPolling(unittest.TestCase):
 def test_active_work_uses_short_poll_and_returns_to_idle_even_on_error(self):
  for fail in (False,True):
   entered=threading.Event();release=threading.Event();sent=threading.Event()
   class Store:
    pool=types.SimpleNamespace(submit=lambda *a:None)
    def warm(self):pass
    def request(self,*args):
     entered.set();release.wait(3)
     if fail:raise RuntimeError('test failure')
     return {'received':1536}
    def close(self):pass
   engine=types.SimpleNamespace(lock=threading.Lock(),ready=True,session='12345678')
   with patch.object(cloud,'Store',Store):
    worker=cloud.CloudWorker(lambda *a:sent.set(),lambda *a,**k:k,engine)
   try:
    self.assertEqual(cloud.reply_poll_ms(worker),20)
    worker.submit(dict(route='test',session='12345678',data={'op':'test'}))
    self.assertTrue(entered.wait(2));self.assertEqual(cloud.reply_poll_ms(worker),2)
    release.set();self.assertTrue(sent.wait(2))
   finally:release.set();worker.close()
   self.assertFalse(worker.active.is_set());self.assertEqual(cloud.reply_poll_ms(worker),20)
 def test_queued_request_is_prompt_before_worker_starts(self):
  import queue
  worker=types.SimpleNamespace(active=threading.Event(),jobs=queue.Queue())
  worker.jobs.put('queued');self.assertEqual(cloud.reply_poll_ms(worker),2)
  worker.jobs.get();self.assertEqual(cloud.reply_poll_ms(worker),20)

if __name__=='__main__':unittest.main()
