import sys,types,unittest
from pathlib import Path
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'module/runtime'))
import muse_playback_control as playback

class Position(unittest.TestCase):
 def test_volume_and_pause_resume_use_audio_position_not_late_highlights(self):
  clock=[0.];control=playback.PlaybackControl(60);starts=[];changes=set();callbacks=[]
  class Backend:
   word_schedule=[(0.,0,3),(8.,50,55)];active_play=False;playback_control=control
   word_callback=lambda self,a,b:callbacks.append((clock[0],a,b))
   def stop_audio(self):pass
   def rpc(self,unit,action,data=None):
    if action=='play_raw':
     starts.append((clock[0],len(data)));self.end=clock[0]+len(data)/32000.;return b'None'
    if action=='audio_status':return b'Runing' if clock[0]<self.end else b'None'
  class Cancel:
   def is_set(self):return False
   def wait(self,seconds):
    clock[0]+=seconds
    if clock[0]>=2.5 and 'volume' not in changes:changes.add('volume');control.update(False,40)
    if clock[0]>=6 and 'pause' not in changes:changes.add('pause');control.update(True,40)
    if clock[0]>=7.5 and 'resume' not in changes:changes.add('resume');control.update(False,40)
  with patch.object(playback,'time',types.SimpleNamespace(monotonic=lambda:clock[0])):
   result=playback.play(Backend(),b'\0'*(10*32000),60,Cancel(),lambda c:None,lambda data,v:data,16000,lambda code,message:RuntimeError(message))
  self.assertTrue(result['joined']);self.assertEqual(len(starts),3)
  self.assertAlmostEqual(starts[1][1]/32000.,7.5,delta=.03)
  self.assertAlmostEqual(starts[2][1]/32000.,4.,delta=.03)
  self.assertAlmostEqual(clock[0],11.5,delta=.05)
  self.assertEqual([x[1:] for x in callbacks],[(0,3),(50,55)])
 def test_delayed_poll_does_not_emit_a_burst_of_old_words(self):
  clock=[0.];callbacks=[]
  class Backend:
   word_schedule=[(i*.1,i,i+1) for i in range(20)];word_callback=lambda self,a,b:callbacks.append((clock[0],a,b));active_play=False
   def stop_audio(self):pass
   def rpc(self,unit,action,data=None):
    if action=='play_raw':return b'None'
    clock[0]+=.5;return b'Runing' if clock[0]<2 else b'None'
  with patch.object(playback,'time',types.SimpleNamespace(monotonic=lambda:clock[0],sleep=lambda t:None)):
   playback.play(Backend(),b'\0'*(2*32000),60,None,lambda c:None,lambda data,v:data,16000,lambda code,message:RuntimeError(message))
  self.assertLessEqual(len(callbacks),4);self.assertEqual(callbacks[-1][1:],(19,20))

if __name__=='__main__':unittest.main()
