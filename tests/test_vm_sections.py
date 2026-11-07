import hashlib,json,sys,tempfile,threading,types,unittest
from pathlib import Path
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'));sys.path.insert(0,str(ROOT/'module/runtime'))
import muse_vm_sections as sections
from muse_speech_text import SpeechText

class GeneratedSections(unittest.TestCase):
 def test_synthesis_and_plan_transfer_can_start_before_models_finish_loading(self):
  with tempfile.TemporaryDirectory() as directory:
   folder=Path(directory);(folder/'source.md').write_text('The blue lantern glowed beside the old window. The river was quiet under the midnight sky.')
   rendering=threading.Event();checked=[]
   class Checker:
    def check(self,*args):checked.append(True);return dict(aligned=True)
   def factory():
    self.assertTrue(rendering.wait(2),'Synthesis incorrectly waits for model loading')
    self.assertTrue((folder/'plan.json').exists());self.assertTrue((folder/'meta.json').exists())
    return Checker()
   def render(folder,part,spoken,voice,speed,executable,ffmpeg,deadline,checker):
    rendering.set();self.assertTrue(checker.check(b'', 'test', 0)['aligned'])
    (folder/('part-%03d.mp3'%part['index'])).write_bytes(b'a'*144)
    return dict(audio_bytes=144)
   with patch.object(sections,'_render',side_effect=render):
    result=sections.synthesize_sections(folder,'avocado_v2:MAI_01',checker_factory=factory)
   self.assertEqual(result['state'],'complete');self.assertEqual(len(checked),2)
 def test_model_load_failure_cannot_publish_playable_audio(self):
  with tempfile.TemporaryDirectory() as directory:
   folder=Path(directory);(folder/'source.md').write_text('The blue lantern glowed.')
   def factory():raise RuntimeError('Model unavailable')
   def render(*args):return args[-1].check(b'', 'test', 0)
   with patch.object(sections,'_render',side_effect=render),self.assertRaises(RuntimeError):
    sections.synthesize_sections(folder,'avocado_v2:MAI_01',checker_factory=factory)
   self.assertEqual(json.loads((folder/'meta.json').read_text())['state'],'error')
   self.assertFalse((folder/'part-000.mp3').exists())
 def test_failed_voice_is_retried_before_any_section_is_published(self):
  with tempfile.TemporaryDirectory() as directory:
   folder=Path(directory);spoken=SpeechText('The blue lantern glowed beside the quiet river at midnight.')
   part=sections.plan_sections(spoken)[0];calls=[]
   class Process:
    def __init__(self,args,stdout,**kwargs):stdout.write(b'x'*200);calls.append('speak')
    def wait(self,timeout):return 0
   def run(args,**kwargs):
    if args[-1]=='pipe:1':return types.SimpleNamespace(stdout=b'\0'*115200)
    Path(args[-1]).write_bytes(b'a'*(144*100));return types.SimpleNamespace(returncode=0)
   class Checker:
    n=0
    def check(self,pcm,text,first):
     self.n+=1
     if self.n==1:
      self_outer.assertFalse((folder/'part-000.json').exists())
      raise ValueError('Generated speech repeats')
     return dict(aligned=True,pcm_samples=len(pcm)//2,words=[[0,.2,first]],agreement=1.)
   self_outer=self
   with patch.object(sections.subprocess,'Popen',Process),patch.object(sections.subprocess,'run',run),patch.object(sections,'terminate'):
    meta=sections._render(folder,part,spoken,'avocado_v2:MAI_03','normal','tts','ffmpeg',sections.time.monotonic()+60,Checker())
   self.assertEqual(calls,['speak','speak']);self.assertEqual(meta['attempts'],2)
   self.assertTrue(meta['aligned']);self.assertEqual(json.loads((folder/'part-000.json').read_text())['state'],'complete')
   self.assertEqual(hashlib.sha256((folder/'part-000.mp3').read_bytes()).hexdigest(),meta['audio_sha256'])
 def test_final_speech_failure_publishes_an_error_and_no_playable_section(self):
  with tempfile.TemporaryDirectory() as directory:
   folder=Path(directory);spoken=SpeechText('The blue lantern glowed.');part=sections.plan_sections(spoken)[0]
   with patch.object(sections.subprocess,'Popen',side_effect=RuntimeError('Provider failed')):
    with self.assertRaises(RuntimeError):sections._render(folder,part,spoken,'avocado_v2:MAI_03','normal','tts','ffmpeg',sections.time.monotonic()+60,None)
   self.assertEqual(json.loads((folder/'part-000.json').read_text())['state'],'error')
   self.assertFalse((folder/'part-000.mp3').exists())
 def test_joined_hash_and_order_cover_every_sentence(self):
  with tempfile.TemporaryDirectory() as directory:
   folder=Path(directory);text=('The blue lantern glowed beside the old window. '
                               'The river was quiet under the midnight sky. '
                               'The keeper came home just before the rain.')
   (folder/'source.md').write_text(text)
   def render(folder,part,*args):
    data=bytes([part['index']+1])*144;(folder/('part-%03d.mp3'%part['index'])).write_bytes(data)
    return dict(audio_bytes=len(data))
   with patch.object(sections,'_render',side_effect=render):
    result=sections.synthesize_sections(folder,'avocado_v2:MAI_03',checker=object())
   audio=(folder/'audio.mp3').read_bytes()
   self.assertEqual(audio,b'\1'*144+b'\2'*144+b'\3'*144)
   self.assertEqual(result['state'],'complete');self.assertEqual(result['ready_sections'],3)
   self.assertEqual(result['audio_sha256'],hashlib.sha256(audio).hexdigest())
 def test_short_dialogue_shares_a_section_without_dropping_words(self):
  spoken=SpeechText('"Wait." "Why?" "Listen." The hallway fell silent as the lights went out. "Run."')
  old=sections.plan_sections(spoken,minimum_words=1)
  grouped=sections.plan_sections(spoken)
  self.assertLess(len(grouped),len(old))
  self.assertGreaterEqual(min(part['word_count'] for part in grouped),7)
  self.assertEqual(sum(part['word_count'] for part in grouped),len(spoken.words))
  self.assertTrue(grouped[-1]['sentence_end'])

if __name__=='__main__':unittest.main()
