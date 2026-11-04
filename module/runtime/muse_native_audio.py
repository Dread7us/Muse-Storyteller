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
     if not item.error:return {'received':item.received}
     if key in self.active:raise NativeAudioError('Native playback cache is busy')
     item.abort();shutil.rmtree(item.folder);del self.items[key]
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
   if item.done:
    if op=='end' and data.get('bytes')==item.received and data.get('sha256')==item.digest.hexdigest():return {'received':item.received}
    raise NativeAudioError('Native audio transfer already complete')
   if op=='chunk':
    offset=data.get('offset');encoded=data.get('audio')
    if type(offset) is not int or offset<0 or not isinstance(encoded,str) or len(encoded)>11000:raise NativeAudioError('Invalid native audio block')
    try:raw=base64.b64decode(encoded,validate=True)
    except ValueError:raise NativeAudioError('Invalid native audio encoding')
    if shutil.disk_usage(self.root).free<64*1024*1024:raise NativeAudioError('Native audio cache storage is low')
    if not raw or len(raw)>8192 or offset+len(raw)>MAX_AUDIO:raise NativeAudioError('Native audio exceeds cache bound')
    if offset<item.received and offset+len(raw)<=item.received:
     with item.mp3.open('rb') as previous:
      previous.seek(offset)
      if previous.read(len(raw))==raw:return {'received':item.received}
    if offset!=item.received:raise NativeAudioError('Native audio block is out of order')
    if item.error or item.decoder.poll() is not None:raise NativeAudioError('Native audio decoder stopped')
    item.file.write(raw);item.file.flush();item.decoder.stdin.write(raw);item.decoder.stdin.flush();item.digest.update(raw);item.received+=len(raw)
    return {'received':item.received}
   if op=='end':
    if data.get('bytes')!=item.received or data.get('sha256')!=item.digest.hexdigest():item.abort();raise NativeAudioError('Native audio integrity check failed')
    item.file.close();item.decoder.stdin.close();item.decoder.wait(timeout=8)
    if item.decoder.returncode or item.size()<320: item.error='Native audio decoding failed';raise NativeAudioError(item.error)
    item.done=True
    meta={'source_sha256':hashlib.sha256(item.text.encode()).hexdigest(),'audio_bytes':item.received,'audio_sha256':item.digest.hexdigest(),'pcm_bytes':item.size()}
    target=item.folder/'complete.json';temp=target.with_suffix('.pending');temp.write_text(json.dumps(meta));temp.replace(target)
    return {'received':item.received}
   raise NativeAudioError('Unknown native audio operation')
 def _load(self,key,text):
  # Complete caches survive a Module restart; partial files never count as ready.
  base=self.root.parent
  for folder in base.glob('*/'+key):
   try:
    if folder.is_symlink() or folder.parent.is_symlink():continue
    meta=json.loads((folder/'complete.json').read_text())
    raw=text.encode('utf8')
    if meta.get('source_sha256')!=hashlib.sha256(raw).hexdigest():continue
    mp3=folder/'voice.mp3';pcm=folder/'voice.pcm'
    if mp3.stat().st_size!=meta['audio_bytes'] or pcm.stat().st_size!=meta['pcm_bytes']:continue
    if hashlib.sha256(mp3.read_bytes()).hexdigest()!=meta['audio_sha256']:continue
    item=object.__new__(Item)
    item.key,item.text,item.session=key,text,None
    item.folder,item.mp3,item.pcm=folder,mp3,pcm
    item.received,item.done,item.error=meta['audio_bytes'],True,None
    item.file=None;item.decoder=None
    self.items[key]=item
    return item
   except (OSError,ValueError,KeyError,TypeError):continue
  return None
 def _schedule(self,item,cancel):
  from muse_voice_backend import check_cancel
  from muse_word_timing import WordTiming
  from muse_speech_text import SpeechText
  # Estimate against complete PCM as the safe fallback. Alignment may improve
  # timing, but it must never truncate, drop or stop valid narration.
  pcm=item.pcm.read_bytes();spoken=SpeechText(item.text)
  timing=WordTiming(item.text);fallback=timing.phrase(timing.normalized,pcm)
  cache=item.folder/'timing.json'
  signature=hashlib.sha256(item.text.encode()).hexdigest()
  try:
   stored=json.loads(cache.read_text())
   if stored.get('version')==2 and stored.get('source')==signature and stored.get('pcm_bytes')==len(pcm):
    values=stored['words']
    if len(values)==len(spoken.words) and all(0<=t<len(pcm)/32000 and 0<=a<b<=len(item.text.encode()) for t,a,b in values):return values
  except (OSError,ValueError,TypeError,KeyError):pass
  words=spoken.words;anchors={};position=0.;cursor=0;duration=len(pcm)/32000.
  try:
   aligner=self.warm()
   while position<duration-.04 and cursor<len(words):
    check_cancel(cancel)
    end=min(duration,position+24);limit=end if end==duration else end-.8
    candidates=words[cursor:cursor+120];a=candidates[0][0];z=candidates[-1][1]
    aligned=aligner.align(pcm[int(position*16000)*2:int(end*16000)*2],spoken.text[a:z])
    progress=[]
    for begin,finish,lo,hi in aligned:
     if finish-begin<.025 or position+finish>limit:continue
     for index in range(cursor,min(len(words),cursor+len(candidates))):
      word=words[index]
      if word[0]<a+hi and word[1]>a+lo:
       anchors.setdefault(index,position+begin);progress.append((index,position+begin))
    if end==duration:break
    if not progress:break
    index,at=max(progress,key=lambda x:x[0])
    if index<=cursor or at<=position+.2:break
    # Overlap the last stable word for analysis only. Playback is one uncut file.
    cursor,position=index,at
  except Exception:
   check_cancel(cancel) # cancellation remains authoritative; alignment is optional
  # Use true timings only when every word has a stable monotonic anchor. A
  # collapsed alignment can otherwise make the reader race to the last word.
  good=len(anchors)==len(words) and all(anchors[i]<=anchors[i+1] for i in range(len(words)-1))
  schedule=[(anchors[i],w[2],w[3]) for i,w in enumerate(words)] if good else fallback
  check_cancel(cancel)
  temporary=cache.with_suffix('.pending');temporary.write_text(json.dumps({'version':2,'source':signature,'pcm_bytes':len(pcm),'words':schedule}));temporary.replace(cache)
  return schedule
 def play(self,backend,key,text,volume,cancel,on_play):
  from muse_voice_backend import check_cancel
  with self.lock:
   item=self.items.get(key) or self._load(key,text)
   if item is not None:self.active.add(key)
  if item is None or item.text!=text:raise NativeAudioError('Native reply cache is unavailable')
  if backend.mic:backend.mic.pause_output(True)
  total=0;blocks=0
  try:
   deadline=time.monotonic()+1200
   while not item.done:
    check_cancel(cancel)
    if item.error:raise NativeAudioError(item.error)
    if time.monotonic()>deadline:raise NativeAudioError('Complete reply buffering timed out')
    time.sleep(.025)
   if item.error:raise NativeAudioError(item.error)
   size=item.size()
   if size<320 or size%2 or size>32*1024*1024:raise NativeAudioError('Invalid complete decoded audio')
   schedule=self._schedule(item,cancel)
   # Fit the native IPC budget. Prefer complete sentence endings only for
   # exceptionally long replies; ordinary replies play in a single native call.
   maximum=5*1024*1024;position=0
   with item.pcm.open('rb') as stream:
    while position<size:
     check_cancel(cancel);end=min(size,position+maximum)
     if end<size:
      choices=[int(t*16000)*2 for t,a,b in schedule if position<t*32000<end and item.text.encode()[max(0,a-3):a].rstrip().endswith((b'.',b'!',b'?'))]
      if choices:end=choices[-1]
     stream.seek(position);pcm=stream.read(end-position)
     if len(pcm)!=end-position:raise NativeAudioError('Cached audio was shortened')
     backend.word_schedule=[(t-position/32000,a,b) for t,a,b in schedule if position/32000<=t<end/32000]
     if not blocks and on_play:on_play()
     backend.play(pcm,volume,cancel)
     total+=len(pcm);blocks+=1;position=end
   if total!=size:raise NativeAudioError('Not all cached audio was played')
   result={'pcm_bytes':size,'played_pcm_bytes':total,'blocks':blocks,'complete':True,'words':len(schedule)}
   (item.folder/'last-playback.json').write_text(json.dumps(result))
   return result
  finally:
   with self.lock:self.active.discard(key)
   if backend.mic:backend.mic.pause_output(False)
