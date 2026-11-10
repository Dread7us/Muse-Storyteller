import sys,threading,types,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/"tools"));sys.path.insert(0,str(ROOT/"module/runtime"))
from muse_vm_speech_check import SpeechCheck,word_anchors

class PaddedAnchors(unittest.TestCase):
 def test_padding_changes_analysis_only_and_returns_original_audio_times(self):
  aligned=[dict(word='She',start=.24,end=.42,probability=.9),dict(word=' understood',start=.42,end=.9,probability=.9),dict(word=' then.',start=.9,end=1.4,probability=.9)]
  anchors=word_anchors(aligned,'She understood then.',1.5,20,.24)
  self.assertEqual([w[2] for w in anchors],[20,21,22]);self.assertAlmostEqual(anchors[0][0],0.)
  self.assertAlmostEqual(anchors[-1][1],1.16)
 def test_collapsed_missing_or_low_confidence_words_remain_rejected(self):
  row=dict(word='She',start=0.,end=.3,probability=.9)
  for aligned,text in [([dict(row,end=0.)],'She'),([row],'She understood'),([dict(row,probability=.1)],'She')]:
   with self.assertRaises(ValueError):word_anchors(aligned,text,1.5)

class ShortUtterances(unittest.TestCase):
 def checker(self,heard,original=''):
  try:import numpy
  except ImportError:self.skipTest('NumPy is needed for waveform checks')
  checker=SpeechCheck.__new__(SpeechCheck);checker.lock=threading.Lock()
  def recognize(audio,**kwargs):
   return [types.SimpleNamespace(text=heard if len(audio)>16000 else original)],None
  def align(audio,text,index):
   self.assertEqual(len(audio),8000)
   return [[.05,.45,index]],.95
  checker.model=types.SimpleNamespace(transcribe=recognize)
  checker.acoustic=types.SimpleNamespace(align=align)
  return checker
 def test_short_recheck_delivers_only_original_clip_and_anchors(self):
  result=self.checker('Listen. Listen.').check(bytes(16000),'"Listen."',43)
  self.assertEqual(result['pcm_samples'],8000)
  self.assertEqual(result['words'],[[.05,.45,43]])
 def test_wrong_or_over_repeated_audio_remains_rejected(self):
  for heard in ('Wait. Wait.', 'Listen. Listen. Listen. Listen.'):
   with self.assertRaisesRegex(ValueError,'differs from its text'):
    self.checker(heard).check(bytes(16000),'Listen.')
  with self.assertRaisesRegex(ValueError,'differs from its text'):
   self.checker('Listen. Listen.',original='Listen. Listen.').check(bytes(16000),'Listen.')

if __name__=='__main__':unittest.main()
