import base64,hashlib,io,json,struct,sys,tempfile,threading,time,types,unittest
from pathlib import Path
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'module/runtime'));sys.path.insert(0,str(ROOT/'tools'))
import muse_native_audio as native
from muse_narration_series import checked_plan,aligned_schedule,SeriesError
from muse_vm_sections import plan_sections
from muse_vm_speech_check import repeated_speech
from muse_speech_text import SpeechText
from muse_voice_backend import scale_pcm

class Plans(unittest.TestCase):
 def test_all_words_and_number_pronunciation_are_preserved(self):
  text='**SCP-9192** cost $12.45. '+('The lantern glowed beside the river and the keeper walked slowly home. '*8)
  spoken=SpeechText(text);parts=plan_sections(spoken)
  plan=dict(version=2,source_sha256=hashlib.sha256(text.encode()).hexdigest(),sections=parts)
  actual,_=checked_plan(text,plan);self.assertEqual(actual.text,spoken.text)
  self.assertEqual(sum(p['word_count'] for p in parts),len(spoken.words))
  self.assertTrue(all(p['word_count']<=36 for p in parts))
 def test_long_sentence_is_one_complete_buffering_group(self):
  spoken=SpeechText(' '.join(['lantern']*80)+'.');parts=plan_sections(spoken)
  self.assertGreater(len(parts),1);self.assertTrue(all(p['sentence']==0 for p in parts))
  self.assertFalse(any(p['sentence_end'] for p in parts[:-1]));self.assertTrue(parts[-1]['sentence_end'])
 def test_omitted_or_changed_plan_is_rejected(self):
  text='The blue lantern glowed.';spoken=SpeechText(text);parts=plan_sections(spoken)
  plan=dict(version=2,source_sha256=hashlib.sha256(text.encode()).hexdigest(),sections=parts)
  for changed in [None,{},dict(plan,sections=[]),dict(plan,source_sha256='0'*64),dict(plan,sections=[dict(parts[0],word_count=1)])]:
   with self.assertRaises(SeriesError):checked_plan(text,changed)
 def test_repetition_check_respects_deliberate_repeated_text(self):
  phrase='The blue lantern is glowing'
  self.assertFalse(repeated_speech((phrase+'. ')*4,(phrase+'. ')*4))
  self.assertTrue(repeated_speech(phrase,(phrase+'. ')*8))
  self.assertTrue(repeated_speech(phrase,(phrase+'. ')*2))
  self.assertTrue(repeated_speech('Go home.','Go home. Go home.'))
  self.assertTrue(repeated_speech('Hello.','Hello. Hello.'))
  self.assertFalse(repeated_speech('The keeper came home.','The keeper came home.'))

class SeriesTests(unittest.TestCase):
 def setUp(self):
  self.tmp=tempfile.TemporaryDirectory();self.store=native.Store(self.tmp.name)
  self.store.warm=lambda:self.fail('Section playback must not run a model on the Module')
  self.text='The blue lantern glowed. The keeper came home. The river was quiet.'
  self.spoken=SpeechText(self.text);self.parts=plan_sections(self.spoken,minimum_words=1);self.key='1234567890abcdef12345678'
  self.plan=dict(version=2,source_sha256=hashlib.sha256(self.text.encode()).hexdigest(),sections=self.parts)
  class Decoder:
   returncode=None
   def __init__(self):self.stdin=io.BytesIO()
   def poll(self):return self.returncode
   def wait(self,timeout=None):self.returncode=0;return 0
   def terminate(self):self.returncode=0
   def kill(self):self.returncode=0
  class FakeItem:
   def __init__(self,root,key,text,session):
    self.key,self.text,self.session=key,text,session;self.folder=root/key;self.folder.mkdir()
    self.mp3=self.folder/'voice.mp3';self.pcm=self.folder/'voice.pcm';self.pcm.write_bytes(struct.pack('<h',6000)*(16000*6))
    self.file=self.mp3.open('wb');self.decoder=Decoder();self.received=0;self.digest=hashlib.sha256();self.done=False;self.error=None
   def size(self):return self.pcm.stat().st_size
   def abort(self):
    self.error='cancelled'
    if not self.file.closed:self.file.close()
    if not self.decoder.stdin.closed:self.decoder.stdin.close()
  self.patch=patch.object(native,'Item',FakeItem);self.patch.start()
  self.request('series_begin',text=self.text,plan=self.plan)
 def tearDown(self):
  self.store.close();self.patch.stop();self.tmp.cleanup()
 def request(self,op,**data):return self.store.request(dict(op=op,key=self.key,_transport_verified=True,**data),'12345678')
 def upload(self,index,bad_words=False):
  data=(bytes((index+1,))*144);sha=hashlib.sha256(data).hexdigest();part=self.parts[index]
  self.request('part_begin',index=index,bytes=len(data),sha256=sha)
  self.request('chunk',offset=0,bytes=len(data),sha256=sha,audio=base64.b64encode(data).decode())
  rows=[[j*.4,j*.4+.3,part['word_start']+j] for j in range(part['word_count'])]
  if bad_words:rows=rows[:-1]
  return self.request('part_end',pcm_samples=96000,aligned=True,words=rows)
 def seal(self):
  data=b''.join(bytes((i+1,))*144 for i in range(len(self.parts)))
  return self.request('series_end',bytes=len(data),sha256=hashlib.sha256(data).hexdigest())
 def test_play_can_start_before_the_last_section_and_never_omits_it(self):
  self.upload(0);self.assertFalse(self.request('series_status')['ready'])
  self.upload(1);self.assertTrue(self.request('series_status')['ready'])
  played=threading.Event();parts=[];words=[];errors=[];results=[]
  class Player:
   mic=None
   def play(p,pcm,volume,cancel):parts.append(len(pcm));words.extend(p.word_schedule);played.set()
  def run():
   try:results.append(self.store.play(Player(),self.key,self.text,60,threading.Event(),None))
   except Exception as e:errors.append(e)
  worker=threading.Thread(target=run);worker.start()
  try:
   self.assertTrue(played.wait(2));self.assertTrue(worker.is_alive());self.assertEqual(sum(parts),384000)
   self.upload(2);self.seal()
  finally:worker.join(3)
  self.assertFalse(worker.is_alive());self.assertFalse(errors);self.assertTrue(results[0]['complete'])
  self.assertEqual(sum(parts),576000);self.assertEqual(len(words),len(self.spoken.words))
  self.assertEqual([w[1] for w in words],sorted(w[1] for w in words))
 def test_wrong_checksum_is_rejected_before_admission(self):
  self.request('part_begin',index=0,bytes=144,sha256='0'*64)
  with self.assertRaises(native.NativeAudioError):self.request('chunk',offset=0,bytes=144,sha256='0'*64,audio=base64.b64encode(b'x'*144).decode())
  self.assertFalse(self.request('series_status')['ready'])
 def test_missing_word_has_no_estimated_timing_fallback(self):
  with self.assertRaises(native.NativeAudioError):self.upload(0,True)
  self.assertFalse(self.request('series_status')['ready'])
 def test_end_requires_every_section_and_the_complete_hash(self):
  self.upload(0)
  with self.assertRaises(native.NativeAudioError):self.seal()
  self.upload(1);self.upload(2)
  with self.assertRaises(native.NativeAudioError):self.request('series_end',bytes=432,sha256='0'*64)
  self.assertFalse(self.request('series_status')['complete']);self.assertTrue(self.seal()['complete'])
 def test_restart_loads_checked_sections_without_a_model_or_new_transfer(self):
  for i in range(len(self.parts)):self.upload(i)
  self.seal();fresh=native.Store(self.tmp.name);played=[]
  class Player:
   mic=None
   def play(p,pcm,volume,cancel):played.append(len(pcm))
  try:
   fresh.warm=lambda:self.fail('Cached restart must not start analysis')
   result=fresh.play(Player(),self.key,self.text,60,threading.Event(),None)
   self.assertTrue(result['complete']);self.assertEqual(sum(played),576000)
  finally:fresh.close()
 def test_late_duplicate_section_ack_does_not_duplicate_audio(self):
  self.upload(0);size=self.store.series[self.key].items[0].size()
  self.assertEqual(self.request('part_end')['received'],144)
  self.assertEqual(self.store.series[self.key].items[0].size(),size)
 def test_interrupted_transfer_preserves_checked_sections_and_partial_bytes(self):
  self.upload(0)
  raw=bytes([2])*144;sha=hashlib.sha256(raw).hexdigest();part=self.parts[1]
  self.request('part_begin',index=1,bytes=len(raw),sha256=sha)
  def block(offset,body):
   return self.request('chunk',offset=offset,bytes=len(body),sha256=hashlib.sha256(body).hexdigest(),audio=base64.b64encode(body).decode())
  block(0,raw[:72])
  status=self.request('series_begin',text=self.text,plan=self.plan)
  self.assertEqual((status['ready_sections'],status['prefix_bytes'],status['received']),(1,144,216))
  self.assertEqual(self.request('part_begin',index=1,bytes=len(raw),sha256=sha,resume=True)['received'],72)
  self.assertEqual(block(0,raw[:72])['received'],72)
  self.assertEqual(block(72,raw[72:])['received'],144)
  rows=[[j*.4,j*.4+.3,part['word_start']+j] for j in range(part['word_count'])]
  self.assertTrue(self.request('part_end',pcm_samples=96000,aligned=True,words=rows)['ready'])
  self.upload(2)
  self.assertTrue(self.seal()['complete'])
 def test_terminal_error_drains_already_checked_sentences(self):
  self.upload(0);self.upload(1)
  self.request('series_abort')
  played=[]
  class Player:
   mic=None
   def play(p,pcm,volume,cancel):played.append(len(pcm))
  with self.assertRaisesRegex(native.NativeAudioError,'transfer stopped'):
   self.store.play(Player(),self.key,self.text,60,threading.Event(),None)
  self.assertEqual(sum(played),384000)
 def test_offline_section_keeps_surrounding_aria_and_can_be_replayed(self):
  self.upload(0)
  synthesized=[]
  def synthesize(phrase,cancel,check,speed):
   synthesized.append((phrase,speed))
   return struct.pack('<h',24000)*96000
  with patch.dict(sys.modules,{'muse_piper':types.SimpleNamespace(synthesize=synthesize)}):
   status=self.request('part_offline',index=1,speed='normal')
   self.request('part_offline',index=1,speed='normal')
  self.assertEqual(len(synthesized),1)
  self.assertEqual((status['ready_sections'],status['prefix_bytes'],status['offline_sections']),(2,144,1))
  with self.assertRaisesRegex(native.NativeAudioError,'omitted'):
   self.request('series_end_mixed')
  self.upload(2)
  self.assertTrue(self.request('series_end_mixed')['complete'])
  fresh=native.Store(self.tmp.name);played=[];sources=[];levels=[]
  class Player:
   mic=None
   narration_source_callback=staticmethod(sources.append)
   def play(p,pcm,volume,cancel):
    played.append(len(pcm));levels.append(struct.unpack_from('<h',scale_pcm(pcm,volume))[0])
  try:result=fresh.play(Player(),self.key,self.text,80,threading.Event(),None)
  finally:fresh.close()
  self.assertTrue(result['complete']);self.assertEqual(result['offline_sections'],1)
  self.assertEqual(sources,[False,True,False])
  self.assertEqual(sum(played),576000)
  self.assertEqual(levels,[4800,4800,4800])
 def test_online_and_offline_in_one_sentence_are_scaled_separately(self):
  self.key='2345678901abcdef12345678'
  self.parts=[dict(self.parts[0],sentence_end=False),dict(self.parts[1],sentence=0),dict(self.parts[2],sentence=1)]
  self.request('series_begin',text=self.text,plan=dict(self.plan,sections=self.parts))
  self.upload(0)
  offline=struct.pack('<h',24000)*96000
  with patch.dict(sys.modules,{'muse_piper':types.SimpleNamespace(synthesize=lambda *args:offline)}):
   self.request('part_offline',index=1,speed='normal')
  self.upload(2);self.request('series_end_mixed')
  blocks=[]
  class Player:
   mic=None
   def play(p,pcm,volume,cancel):blocks.append(scale_pcm(pcm,volume))
  self.store.play(Player(),self.key,self.text,80,None,None)
  level=struct.pack('<h',4800)
  self.assertEqual(blocks,[level*192000,level*96000])
  self.assertEqual(self.store.series[self.key].items[1].pcm.read_bytes(),offline)
 def test_nan_or_nonmonotonic_word_timing_is_rejected(self):
  part=self.parts[0];rows=[[j*.4,j*.4+.3,j] for j in range(part['word_count'])]
  for changed in [dict(aligned=False,words=rows),dict(aligned=True,words=[[float('nan'),.3,0]]+rows[1:]),dict(aligned=True,words=rows[::-1])]:
   with self.assertRaises(SeriesError):aligned_schedule(self.spoken,part,changed,6)
 def test_missing_cached_timing_is_rebuilt_instead_of_stranding_restart(self):
  for i in range(len(self.parts)):self.upload(i)
  self.seal();series=self.store.series[self.key]
  (series.items[1].folder/'aligned.json').unlink()
  fresh=native.Store(self.tmp.name)
  try:
   result=fresh.request(dict(op='series_begin',key=self.key,text=self.text,plan=self.plan,_transport_verified=True),'12345678')
   self.assertFalse(result['complete']);self.assertEqual(result['ready_sections'],1)
   data=bytes([2])*144
   result=fresh.request(dict(op='part_begin',key=self.key,index=1,bytes=144,sha256=hashlib.sha256(data).hexdigest(),_transport_verified=True),'12345678')
   self.assertEqual(result['received'],0)
  finally:fresh.close()
 def test_stopping_while_waiting_for_the_next_sentence_exits_promptly(self):
  self.upload(0);self.upload(1);cancel=threading.Event();played=threading.Event();errors=[]
  class Player:
   mic=None
   def play(p,pcm,volume,event):played.set()
  def run():
   try:self.store.play(Player(),self.key,self.text,60,cancel,None)
   except Exception as e:errors.append(e)
  worker=threading.Thread(target=run);worker.start()
  self.assertTrue(played.wait(2));cancel.set();worker.join(2)
  self.assertFalse(worker.is_alive());self.assertTrue(errors)

if __name__=='__main__':unittest.main()
