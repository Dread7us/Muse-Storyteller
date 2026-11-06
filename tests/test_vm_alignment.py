import sys,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/"tools"))
from muse_vm_speech_check import word_anchors

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

if __name__=='__main__':unittest.main()
