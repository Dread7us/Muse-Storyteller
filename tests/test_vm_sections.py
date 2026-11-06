import hashlib,json,sys,tempfile,types,unittest
from pathlib import Path
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'));sys.path.insert(0,str(ROOT/'module/runtime'))
import muse_vm_sections as sections
from muse_speech_text import SpeechText

class GeneratedSections(unittest.TestCase):
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
   folder=Path(directory);text='The blue lantern glowed. The river was quiet. The keeper came home.'
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

if __name__=='__main__':unittest.main()
