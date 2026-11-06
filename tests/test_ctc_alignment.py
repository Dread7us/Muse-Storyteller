import sys,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
try:import numpy as np
except ImportError:raise unittest.SkipTest('Numerical tests require NumPy; installed in Linux CI')
from muse_ctc_alignment import viterbi,AcousticAligner

class AcousticPaths(unittest.TestCase):
 def emissions(self,frames):
  result=np.full((len(frames),4),-12.,dtype=np.float32)
  for i,label in enumerate(frames):result[i,label]=0.
  return result
 def test_every_character_gets_its_own_real_frame_span(self):
  spans=viterbi(self.emissions([0,1,1,0,2,2,0]),[1,2])
  self.assertEqual([s[:2] for s in spans],[(1,3),(4,6)])
  self.assertTrue(all(s[2]>.99 for s in spans))
 def test_repeated_letters_need_an_intervening_blank(self):
  spans=viterbi(self.emissions([0,1,1,0,1,1,0]),[1,1])
  self.assertEqual([s[:2] for s in spans],[(1,3),(4,6)])
 def test_missing_time_and_excessive_windows_are_rejected(self):
  for values,tokens in [(self.emissions([1]),[1,1]),(np.zeros((1601,4),dtype=np.float32),[1]),(self.emissions([1]),[])]:
   with self.assertRaises(ValueError):viterbi(values,tokens)
 def test_clock_words_keep_global_word_ids_and_positive_durations(self):
  aligner=object.__new__(AcousticAligner);aligner.vocab={'<pad>':0,'A':1,'M':2,'|':3};aligner.blank=0;aligner.jump=320;aligner.receptive=400;aligner.inputs={'input_values':'tensor(float)'}
  class Model:
   def run(self,names,feeds):
    result=np.full((1,20,4),-12.,dtype=np.float32);result[:,:,0]=0
    for frame,label in [(2,1),(3,1),(8,3),(12,2),(13,2)]:result[0,frame,0]=-12;result[0,frame,label]=0
    return [result]
  aligner.model=Model();words,score=aligner.align(np.zeros(8000,dtype=np.float32),'A M',90)
  self.assertEqual([w[2] for w in words],[90,91]);self.assertTrue(all(w[0]<w[1] for w in words));self.assertGreater(score,.9)
 def test_all_blank_audio_does_not_become_confident_word_timing(self):
  aligner=object.__new__(AcousticAligner);aligner.vocab={'<pad>':0,'A':1,'|':2};aligner.blank=0;aligner.jump=320;aligner.receptive=400;aligner.inputs={'input_values':'tensor(float)'}
  class Model:
   def run(self,names,feeds):
    result=np.full((1,20,3),-15.,dtype=np.float32);result[:,:,0]=0;return [result]
  aligner.model=Model()
  with self.assertRaises(ValueError):aligner.align(np.zeros(8000,dtype=np.float32),'A')

if __name__=='__main__':unittest.main()
