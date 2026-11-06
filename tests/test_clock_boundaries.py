import sys,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'module/runtime'))
from muse_speech_text import SpeechText

class ClockBoundaries(unittest.TestCase):
 def test_bare_clock_keeps_the_following_word_and_its_highlight_span(self):
  raw='The 9:40 will come back.';spoken=SpeechText(raw)
  self.assertEqual(spoken.text,'The nine forty A M will come back.')
  word=next(w for w in spoken.words if w[4]=='will')
  self.assertEqual(raw.encode()[word[2]:word[3]],b'will')
 def test_clock_suffix_does_not_consume_part_of_the_next_word(self):
  for raw,expected in [('9:40 among the trees','nine forty A M among the trees'),('9:40 PM will return','nine forty P M will return'),('19:40 will return','seven forty P M will return'),('9:40. Then leave.','nine forty A M. Then leave.')]:
   self.assertEqual(SpeechText(raw).text,expected)
 def test_money_scores_and_identifiers_stay_contextual(self):
  spoken=SpeechText('$12.45; score 35-20; SCP-9192.').text
  self.assertIn('twelve dollars and forty five cents',spoken)
  self.assertIn('thirty five to twenty',spoken)
  self.assertIn('S C P nine one nine two',spoken)

if __name__=='__main__':unittest.main()
