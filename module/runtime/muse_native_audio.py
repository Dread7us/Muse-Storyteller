"""Native Muse MP3 cache and bounded local timing. No network or paid service."""
import base64,hashlib,json,os,re,subprocess,threading,time,uuid,shutil
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
HOME=Path('/opt/muse-cloud-voice-v41')
CACHE=Path('/var/cache/muse-native-voice')
MAX_AUDIO=32*1024*1024
class NativeAudioError(RuntimeError):pass

class Item:
 def __init__(self,root,key,text,session):
  self.key,self.text,self.session=key,text,session;self.folder=root/key;self.folder.mkdir(mode=0o700)
  self.mp3=self.folder/'voice.mp3';self.pcm=self.folder/'voice.pcm';self.received=0;self.digest=hashlib.sha256();self.done=False;self.error=None
  self.file=self.mp3.open('wb');self.decoder=subprocess.Popen([str(HOME/'decode_mp3'),str(self.pcm)],stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
 def abort(self):
  self.error='Native audio transfer cancelled'
  try:self.decoder.stdin.close()
  except (OSError,ValueError):pass
  if self.decoder.poll() is None:
   self.decoder.terminate()
   try:self.decoder.wait(3)
   except subprocess.TimeoutExpired:self.decoder.kill();self.decoder.wait(3)
  if not self.file.closed:self.file.close()
 def size(self):return self.pcm.stat().st_size if self.pcm.exists() else 0

class Aligner:
 def __init__(self):
  import sys
  sys.path.insert(0,str(HOME/'vendor'));sys.path.insert(0,str(HOME))
  import ctranslate2,tokenizers
  from native_tokenizer import Tokenizer
  from feature_extractor import FeatureExtractor
  self.ct=ctranslate2;self.extract=FeatureExtractor();self.tokenizer=Tokenizer(tokenizers.Tokenizer.from_file(str(HOME/'tiny-model/tokenizer.json')),False,task='transcribe',language='en')
  self.model=ctranslate2.models.Whisper(str(HOME/'tiny-model'),device='cpu',compute_type='int8',intra_threads=2,inter_threads=1)
  self.lock=threading.Lock()
 def align(self,pcm,text):
  import numpy as np
  t=self.tokenizer;tokens=t.encode(text)
  if not tokens:return []
  if len(tokens)>400:raise NativeAudioError('Timing text exceeds one analysis window')
  with self.lock:
   audio=np.frombuffer(pcm,dtype='<i2').astype(np.float32)/32768.
   features=self.extract(audio);frames=features.shape[-1]
   encoded=self.model.encode(self.ct.StorageView.from_array(features[None,...]))
   result=self.model.align(encoded,t.sot_sequence,[tokens],frames,median_filter_width=7)[0]
  ti=np.array([p[0] for p in result.alignments]);tt=np.array([p[1] for p in result.alignments]);jumps=np.pad(np.diff(ti),(1,0),constant_values=1).astype(bool);times=tt[jumps]/50.
  words,groups=t.split_to_word_tokens(tokens+[t.eot]);bounds=np.pad(np.cumsum([len(g) for g in groups[:-1]]),(1,0))
  output=[];cursor=0
  for word,a,b in zip(words, bounds[:-1],bounds[1:]):
   if not word:continue
   pos=text.find(word,cursor)
   if pos<0:raise NativeAudioError('Timing text does not match its script')
   cursor=pos+len(word);output.append((float(times[a]),float(times[b]),pos,cursor))
  return output

class Store:
 def __init__(self,root=CACHE):
  self.root=Path(root)/uuid.uuid4().hex;self.root.mkdir(mode=0o700,parents=True,exist_ok=True);self.lock=threading.RLock();self.items={};self.active=set();self.aligner=None;self.pool=ThreadPoolExecutor(max_workers=1,thread_name_prefix='native-word-timing');self.warming=None;self.warm_lock=threading.Lock()
 def warm(self):
  with self.warm_lock:
   if self.aligner is None:self.aligner=Aligner()
   return self.aligner
 def request(self,data,session):
  op,key=data.get('op'),data.get('key')
  if not isinstance(key,str) or not re.fullmatch('[0-9a-f]{24}',key):raise NativeAudioError('Invalid native audio key')
  with self.lock:
   item=self.items.get(key)
   if op=='begin':
    text=data.get('text')
    if not isinstance(text,str) or not text.strip() or len(text.encode())>8192 or '\x00' in text:raise NativeAudioError('Native speech needs its original script')
    if item:
     if item.session!=session or item.text!=text:raise NativeAudioError('Native audio identity changed')
     return {'received':item.received}
    # Only paired device uploads are admitted by the caller. Retain at most eight recent replies.
    if shutil.disk_usage(self.root).free<128*1024*1024:raise NativeAudioError('Native audio cache needs free storage')
    if len(self.items)>=8:
     oldest=next((k for k in self.items if k not in self.active),None)
     if oldest is None:raise NativeAudioError('Native playback cache is busy')
     old=self.items.pop(oldest)
     if not old.done:old.abort()
     shutil.rmtree(old.folder)
    item=Item(self.root,key,text,session);self.items[key]=item
    if self.warming is None:self.warming=self.pool.submit(self.warm)
    return {'received':0}
   if item is None or item.session!=session:raise NativeAudioError('Stale native audio transfer')
   if op=='abort':
    if not item.done:item.abort()
    return {'received':item.received}
   if item.done:raise NativeAudioError('Native audio transfer already complete')
   if op=='chunk':
    offset=data.get('offset');encoded=data.get('audio')
    if type(offset) is not int or offset<0 or not isinstance(encoded,str) or len(encoded)>11000:raise NativeAudioError('Invalid native audio block')
    try:raw=base64.b64decode(encoded,validate=True)
    except ValueError:raise NativeAudioError('Invalid native audio encoding')
    if shutil.disk_usage(self.root).free<64*1024*1024:raise NativeAudioError('Native audio cache storage is low')
    if not raw or len(raw)>8192 or offset+len(raw)>MAX_AUDIO:raise NativeAudioError('Native audio exceeds cache bound')
    if offset!=item.received:raise NativeAudioError('Native audio block is out of order')
    if item.error or item.decoder.poll() is not None:raise NativeAudioError('Native audio decoder stopped')
    item.file.write(raw);item.file.flush();item.decoder.stdin.write(raw);item.decoder.stdin.flush();item.digest.update(raw);item.received+=len(raw)
    return {'received':item.received}
   if op=='end':
    if data.get('bytes')!=item.received or data.get('sha256')!=item.digest.hexdigest():item.abort();raise NativeAudioError('Native audio integrity check failed')
    item.file.close();item.decoder.stdin.close();item.decoder.wait(timeout=8)
    if item.decoder.returncode or item.size()<320: item.error='Native audio decoding failed';raise NativeAudioError(item.error)
    item.done=True;return {'received':item.received}
   raise NativeAudioError('Unknown native audio operation')
 def _chunk(self,item,position,cursor,cancel):
  from muse_voice_backend import check_cancel
  from muse_speech_text import SpeechText
  # Mapping is to the authoritative script, never ASR-rewritten display text.
  spoken=SpeechText(item.text);words=spoken.words;normalized=spoken.text
  duration=4.;deadline=time.monotonic()+45
  while True:
   check_cancel(cancel)
   if item.error:raise NativeAudioError(item.error)
   available=item.size()/32000.
   if item.done and position>=available-.01:return None
   wanted=position+duration
   if available<wanted and not item.done:
    if time.monotonic()>deadline:raise NativeAudioError('Native audio buffering timed out')
    time.sleep(.04);continue
   end=min(available,wanted);count=int((end-position)*16000)*2
   with item.pcm.open('rb') as stream:stream.seek(int(position*16000)*2);pcm=stream.read(count)
   remaining=[w for w in words if w[1]>cursor]
   if not remaining:return (pcm,[],end,len(normalized))
   # A generous lookahead lets unspoken words collapse at the window edge; only stable interior timings are used.
   candidate=remaining[:min(160,max(24,int(duration*7)))];a=candidate[0][0];z=candidate[-1][1];phrase=normalized[a:z]
   aligned=self.warm().align(pcm,phrase);limit=(end-position) if item.done and end>=available-.01 else max(0,(end-position)-.65)
   schedule=[];safe=[];advance=cursor
   for start,finish,wa,wz in aligned:
    if finish-start<.025 or finish>limit:continue
    begin,stop=a+wa,a+wz
    matches=[w for w in words if w[0]<stop and w[1]>begin]
    if not matches:continue
    lo,hi=min(w[2] for w in matches),max(w[3] for w in matches);schedule.append((start,lo,hi));advance=max(advance,stop)
    raw=phrase[wa:wz].strip()
    if raw and (raw[-1] in '.!?;:' or (stop<len(normalized) and normalized[stop] in '.!?;:')):
     boundary=stop+1 if stop<len(normalized) and normalized[stop] in '.!?;:' else stop
     safe.append((finish,boundary))
   final=item.done and end>=available-.01
   if final:return (pcm,schedule,end,len(normalized))
   # Prefer sentence boundaries; extend the lookahead instead of inserting a mid-sentence gap.
   choices=[x for x in safe if x[0]>=1.]
   if choices:
    finish,advance=choices[-1];finish=min(finish+.12,end-position);n=int(finish*16000)*2
    return (pcm[:n],[w for w in schedule if w[0]<finish],position+n/32000.,advance)
   if duration>=24:raise NativeAudioError('Native word timing could not find a safe sentence boundary')
   duration+=4
 def play(self,backend,key,text,volume,cancel,on_play):
  from muse_voice_backend import check_cancel
  with self.lock:
   item=self.items.get(key)
   if item is not None:self.active.add(key)
  if item is None or item.text!=text:raise NativeAudioError('Native reply cache is unavailable')
  position=0.;cursor=0;started=False
  if backend.mic:backend.mic.pause_output(True)
  try:
   self.warm()
   part=self._chunk(item,position,cursor,cancel)
   while part is not None:
    check_cancel(cancel);pcm,schedule,next_position,next_cursor=part
    future=self.pool.submit(self._chunk,item,next_position,next_cursor,cancel)
    backend.word_schedule=schedule
    if not started:
     if on_play:on_play()
     started=True
    backend.play(pcm,volume,cancel)
    part=future.result(timeout=50);position,cursor=next_position,next_cursor
  finally:
   if 'future' in locals():
    # Acknowledgement waits for the current bounded timing task to stop.
    future.cancel()
    try:future.result(timeout=50)
    except Exception:pass
   with self.lock:self.active.discard(key)
   if backend.mic:backend.mic.pause_output(False)
