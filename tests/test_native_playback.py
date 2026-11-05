import base64,hashlib,io,json,sys,tempfile,threading,types,unittest
from pathlib import Path
from unittest.mock import patch
from concurrent.futures import Future
try:import numpy
except ImportError:pass
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'module/runtime'))
import muse_native_audio as native
import muse_playback_control as control
from muse_cloud_worker import checked_payload
with patch.dict(sys.modules,{'muse_zmq':types.SimpleNamespace()}):
 import muse_voice_backend as adapter
class Interrupted(Exception):pass
def check(cancel):
 if cancel and cancel.is_set():raise Interrupted()
class Player:
 mic=None
 def __init__(self,interrupt=False):self.parts=[];self.words=[];self.interrupt=interrupt
 def play(self,pcm,volume,cancel):
  self.parts.append(len(pcm));self.words.extend(self.word_schedule)
  if self.interrupt:cancel.set();check(cancel)
class TransferRetries(unittest.TestCase):
 def setUp(self):
  self.temp=tempfile.TemporaryDirectory();self.store=native.Store(self.temp.name);self.store.warm=lambda:None
  class Decoder:
   returncode=None
   def __init__(self):self.stdin=io.BytesIO()
   def poll(self):return self.returncode
   def wait(self,timeout=None):self.returncode=0;return 0
   def terminate(self):self.returncode=0
   def kill(self):self.returncode=0
  class Item(native.Item):
   def __init__(self,root,key,text,session):
    self.key,self.text,self.session=key,text,session;self.folder=root/key;self.folder.mkdir()
    self.mp3=self.folder/'voice.mp3';self.pcm=self.folder/'voice.pcm';self.pcm.write_bytes(b'\0\0'*160)
    self.file=self.mp3.open('wb');self.decoder=Decoder();self.received=0;self.digest=hashlib.sha256();self.done=False;self.error=None
  self.patch=patch.object(native,'Item',Item);self.patch.start()
  self.key='1234567890abcdef12345678';self.request('begin',text='A complete reply.')
 def request(self,op,**data):return self.store.request(dict(op=op,key=self.key,_transport_verified=True,**data),'12345678')
 def tearDown(self):
  for item in self.store.items.values():
   if not item.done:item.abort()
  self.store.pool.shutdown(wait=True);self.patch.stop();self.temp.cleanup()
 def test_lost_chunk_and_completion_acknowledgements_are_safe(self):
  encoded=base64.b64encode(b'abc').decode()
  self.assertEqual(self.request('chunk',offset=0,audio=encoded),{'received':3})
  self.assertEqual(self.request('chunk',offset=0,audio=encoded),{'received':3})
  with self.assertRaises(native.NativeAudioError):self.request('chunk',offset=0,audio=base64.b64encode(b'xyz').decode())
  digest=hashlib.sha256(b'abc').hexdigest()
  self.request('end',bytes=3,sha256=digest)
  self.assertEqual(self.request('end',bytes=3,sha256=digest),{'received':3})
  self.assertEqual(self.store.items[self.key].mp3.read_bytes(),b'abc')
 def test_aborted_transfer_resumes_without_duplicating_the_prefix(self):
  self.request('chunk',offset=0,audio=base64.b64encode(b'old').decode());self.request('abort')
  result=self.request('begin',text='A complete reply.');self.assertEqual(result['received'],3);self.assertFalse(result['complete'])
  self.request('chunk',offset=3,audio=base64.b64encode(b'new').decode())
  self.assertEqual(self.store.items[self.key].mp3.read_bytes(),b'oldnew')
 def test_restart_recovers_partial_cache_but_hash_is_still_required(self):
  self.request('chunk',offset=0,audio=base64.b64encode(b'abc').decode());self.request('abort')
  fresh=native.Store(self.temp.name);fresh.warm=lambda:None
  try:
   result=fresh.request(dict(op='begin',key=self.key,text='A complete reply.',_transport_verified=True),'87654321')
   self.assertEqual(result['received'],3);self.assertFalse(result['complete'])
   with self.assertRaises(native.NativeAudioError):fresh.request(dict(op='end',key=self.key,bytes=3,sha256=hashlib.sha256(b'bad').hexdigest(),_transport_verified=True),'87654321')
   result=fresh.request(dict(op='begin',key=self.key,text='A complete reply.',_transport_verified=True),'87654321')
   self.assertEqual(result['received'],0)
  finally:
   for item in fresh.items.values():
    if not item.done:item.abort()
   fresh.pool.shutdown(wait=True)
 def test_shortened_valid_base64_is_rejected_before_cache_changes(self):
  with self.assertRaises(native.NativeAudioError):self.request('chunk',offset=0,audio=base64.b64encode(b'ab').decode(),bytes=3,sha256=hashlib.sha256(b'abc').hexdigest())
  self.assertEqual(self.store.items[self.key].received,0);self.assertEqual(self.store.items[self.key].mp3.stat().st_size,0)
 def test_changed_payload_is_rejected_before_its_operation_runs(self):
  payload=json.dumps(dict(op='chunk',key=self.key,offset=0,audio=base64.b64encode(b'abc').decode()))
  packet=dict(op='packet',payload=payload,sha256=hashlib.sha256(payload.encode()).hexdigest())
  self.assertTrue(checked_payload(packet)['_transport_verified'])
  packet['payload']=payload.replace('YWJj','YWI=')
  with self.assertRaises(native.NativeAudioError):checked_payload(packet)
  self.assertFalse(checked_payload(dict(op='begin',_transport_verified=True)).get('_transport_verified',False))
 def test_service_close_preserves_prefix_and_releases_waiting_timing(self):
  self.request('chunk',offset=0,audio=base64.b64encode(b'protected-prefix').decode())
  item=self.store.items[self.key];future=item.timing_future
  self.store.close()
  self.assertEqual(item.mp3.read_bytes(),b'protected-prefix');self.assertTrue(item.error)
  if not future.cancelled():
   with self.assertRaises(native.NativeAudioError):future.result(3)
  self.assertFalse((item.folder/'complete.json').exists())
class CompleteCache(unittest.TestCase):
 def setUp(self):
  self.temp=tempfile.TemporaryDirectory();self.directory=Path(self.temp.name);self.text='The blue lantern glowed. The keeper came home.';self.key='1234567890abcdef12345678'
  self.store=native.Store(self.directory);folder=self.store.root/self.key;folder.mkdir()
  (folder/'voice.mp3').write_bytes(b'test-native-cache')
  self.pcm=b'\0\0'*(16000*36);(folder/'voice.pcm').write_bytes(self.pcm)
  (folder/'complete.json').write_text(json.dumps({'source_sha256':hashlib.sha256(self.text.encode()).hexdigest(),'audio_bytes':17,'audio_sha256':hashlib.sha256(b'test-native-cache').hexdigest(),'pcm_bytes':len(self.pcm)}))
  self.folder=folder
  def unavailable():raise RuntimeError('Timing model failed')
  self.store.warm=unavailable
  self.mock=patch.dict(sys.modules,{'muse_voice_backend':types.SimpleNamespace(check_cancel=check)})
  self.mock.start()
 def tearDown(self):self.mock.stop();self.store.pool.shutdown(wait=True);self.temp.cleanup()
 def test_alignment_failure_never_truncates_valid_audio(self):
  player=Player();result=self.store.play(player,self.key,self.text,50,threading.Event(),None)
  self.assertEqual(player.parts,[len(self.pcm)]);self.assertTrue(result['complete'])
  self.assertEqual(result['played_pcm_bytes'],len(self.pcm));self.assertEqual(player.words[-1][2],len(self.text))
 def test_failed_background_timing_still_plays_the_complete_clip(self):
  item=self.store._load(self.key,self.text);item.timing_future=Future();item.timing_future.set_exception(RuntimeError('Analysis failed'))
  player=Player();result=self.store.play(player,self.key,self.text,50,threading.Event(),None)
  self.assertEqual(result['played_pcm_bytes'],len(self.pcm));self.assertEqual(player.words[-1][2],len(self.text))
 def test_optional_cache_and_report_writes_cannot_fail_completed_audio(self):
  original=Path.write_text
  def full(path,*args,**kwargs):
   if path.name in ('timing.pending','last-playback.json'):raise OSError('Cache unavailable')
   return original(path,*args,**kwargs)
  with patch.object(Path,'write_text',full):
   player=Player();result=self.store.play(player,self.key,self.text,50,threading.Event(),None)
  self.assertTrue(result['complete']);self.assertEqual(sum(player.parts),len(self.pcm));self.assertEqual(player.words[-1][2],len(self.text))
 def test_timing_analyzes_incomplete_windows_but_publishes_only_after_integrity(self):
  (self.folder/'complete.json').unlink();seen=threading.Event()
  class Aligner:
   def align(self,pcm,text):seen.set();return []
  self.store.warm=lambda:Aligner()
  item=types.SimpleNamespace(text=self.text,folder=self.folder,pcm=self.folder/'voice.pcm',done=False,error=None,size=lambda:(self.folder/'voice.pcm').stat().st_size)
  future=self.store.pool.submit(self.store._schedule,item,None)
  try:
   self.assertTrue(seen.wait(3));self.assertFalse(future.done());self.assertFalse((self.folder/'timing.json').exists())
  finally:item.done=True
  schedule=future.result(3)
  self.assertEqual(schedule[-1][2],len(self.text));self.assertEqual(json.loads((self.folder/'timing.json').read_text())['pcm_bytes'],len(self.pcm))
 def test_restart_recovers_complete_cache_without_realigning(self):
  first=Player();self.store.play(first,self.key,self.text,50,threading.Event(),None)
  fresh=native.Store(self.directory)
  try:
   fresh.warm=lambda:self.fail('Replay should use saved timing')
   second=Player();fresh.play(second,self.key,self.text,50,threading.Event(),None)
   self.assertEqual(first.parts,second.parts);self.assertEqual(first.words,second.words)
  finally:fresh.pool.shutdown(wait=True)
 def test_complete_cache_begin_avoids_upload_after_service_restart(self):
  fresh=native.Store(self.directory)
  try:
   result=fresh.request(dict(op='begin',key=self.key,text=self.text),'12345678')
   self.assertTrue(result['complete']);self.assertEqual(result['received'],17)
   self.assertEqual(result['sha256'],hashlib.sha256(b'test-native-cache').hexdigest())
  finally:fresh.pool.shutdown(wait=True)
 def test_stop_then_restart_keeps_the_whole_clip(self):
  with self.assertRaises(Interrupted):self.store.play(Player(True),self.key,self.text,50,threading.Event(),None)
  player=Player();self.store.play(player,self.key,self.text,50,threading.Event(),None)
  self.assertEqual(player.parts,[len(self.pcm)]);self.assertFalse(self.store.active)
 def test_partial_cache_or_changed_script_is_not_replayed(self):
  with self.assertRaises(native.NativeAudioError):self.store.play(Player(),self.key,self.text+' changed',50,threading.Event(),None)
  (self.folder/'complete.json').unlink()
  with self.assertRaises(native.NativeAudioError):self.store.play(Player(),self.key,self.text,50,threading.Event(),None)
 def test_multiple_transport_blocks_never_omit_pcm(self):
  pcm=b'\0\0'*(16000*180);(self.folder/'voice.pcm').write_bytes(pcm)
  meta=json.loads((self.folder/'complete.json').read_text());meta['pcm_bytes']=len(pcm);(self.folder/'complete.json').write_text(json.dumps(meta))
  player=Player();result=self.store.play(player,self.key,self.text,50,threading.Event(),None)
  self.assertEqual(sum(player.parts),len(pcm));self.assertGreater(result['blocks'],1)
 def test_complete_147_second_reply_fits_actual_volume_scaler(self):
  pcm=b'\0\0'*(16000*147);(self.folder/'voice.pcm').write_bytes(pcm)
  meta=json.loads((self.folder/'complete.json').read_text());meta['pcm_bytes']=len(pcm);(self.folder/'complete.json').write_text(json.dumps(meta))
  class ScaledPlayer(Player):
   def play(self,pcm,volume,cancel):
    self.scaled=adapter.scale_pcm(pcm,volume);super().play(pcm,volume,cancel)
  player=ScaledPlayer();result=self.store.play(player,self.key,self.text,50,threading.Event(),None)
  self.assertEqual(result['blocks'],1);self.assertEqual(len(player.scaled),len(pcm))
 def test_earlier_sentence_end_is_preferred_to_a_mid_sentence_block(self):
  pcm=b'\0\0'*(16000*180);(self.folder/'voice.pcm').write_bytes(pcm)
  meta=json.loads((self.folder/'complete.json').read_text());meta['pcm_bytes']=len(pcm);(self.folder/'complete.json').write_text(json.dumps(meta))
  start=self.text.index('The keeper')
  self.store._schedule=lambda item,cancel:[(0,0,3),(80,start,start+3)]
  player=Player();self.store.play(player,self.key,self.text,50,threading.Event(),None)
  self.assertEqual(player.parts,[80*32000,100*32000])
class Completion(unittest.TestCase):
 def test_volume_scaling_preserves_all_samples_and_truncation(self):
  import array
  values=array.array('h',range(-32768,32768));data=values.tobytes()
  for volume in (0,1,37,50,99,100):
   expected=array.array('h',(int(v*volume/100) for v in values)).tobytes()
   self.assertEqual(adapter.scale_pcm(data,volume),expected)
 def test_native_stop_at_85_percent_is_an_error(self):
  clock=[0.]
  class Cancel:
   def is_set(self):return False
   def wait(self,seconds):clock[0]+=seconds
  class Backend:
   word_schedule=[];word_callback=None;active_play=False
   def stop_audio(self):pass
   def rpc(self,unit,action,data=None):return b'Runing' if action=='audio_status' and clock[0]<8.5 else b'None'
  fake=types.SimpleNamespace(monotonic=lambda:clock[0])
  with patch.object(control,'time',fake):
   with self.assertRaisesRegex(RuntimeError,'ended too early'):
    control.play(Backend(),b'\0'*(32000*10),50,Cancel(),check,lambda pcm,v:pcm,16000,lambda code,message:RuntimeError(message))
if __name__=='__main__':unittest.main()
