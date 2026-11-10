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

class LocalAligner:
 def __init__(self):
  # UART routing and playback controls must win over optional word analysis
  # on the Module's two CPU cores. Native model threads inherit this priority.
  try:os.setpriority(os.PRIO_PROCESS,threading.get_native_id(),10)
  except (AttributeError,OSError):pass
  import sys
  sys.path.insert(0,str(HOME/'vendor'));sys.path.insert(0,str(HOME))
  import ctranslate2,tokenizers
  from native_tokenizer import Tokenizer
  from feature_extractor import FeatureExtractor
  self.ct=ctranslate2;self.extract=FeatureExtractor();self.tokenizer=Tokenizer(tokenizers.Tokenizer.from_file(str(HOME/'tiny-model/tokenizer.json')),False,task='transcribe',language='en')
  self.model=ctranslate2.models.Whisper(str(HOME/'tiny-model'),device='cpu',compute_type='int8',intra_threads=1,inter_threads=1)
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

class Aligner:
 def __init__(self):
  from muse_alignment_worker import AlignmentWorker
  self.worker=AlignmentWorker()
 def align(self,pcm,text):return self.worker.align(pcm,text)
 def close(self):self.worker.close()

class Store:
 def __init__(self,root=CACHE):
  self.root=Path(root)/uuid.uuid4().hex;self.root.mkdir(mode=0o700,parents=True,exist_ok=True);self.lock=threading.RLock();self.items={};self.series={};self.active=set();self.aligner=None;self.pool=ThreadPoolExecutor(max_workers=1,thread_name_prefix='native-word-timing');self.warming=None;self.warm_lock=threading.Lock()
 def warm(self):
  with self.warm_lock:
   if self.aligner is None:self.aligner=Aligner()
   return self.aligner
 def _partial(self,key,text):
  signature=hashlib.sha256(text.encode()).hexdigest();candidates=[]
  for folder in self.root.parent.glob('*/'+key):
   try:
    mp3=folder/'voice.mp3'
    if folder.is_symlink() or folder.parent.is_symlink() or mp3.is_symlink():continue
    if (folder/'complete.json').exists() or (folder/'invalid.json').exists():continue
    manifest=folder/'transfer.json'
    if not manifest.exists():continue
    record=json.loads(manifest.read_text())
    if record.get('source_sha256')!=signature or record.get('transport_integrity')!=1:continue
    size=mp3.stat().st_size
    if 0<size<=MAX_AUDIO:candidates.append((size,mp3))
   except (OSError,ValueError,TypeError):continue
  return max(candidates,key=lambda entry:entry[0])[1] if candidates else None
 def request(self,data,session):
  op,key=data.get('op'),data.get('key')
  if not isinstance(key,str) or not re.fullmatch('[0-9a-f]{24}',key):raise NativeAudioError('Invalid native audio key')
  with self.lock:
   if data.get('op')=='series_begin' or key in self.series:
    from muse_narration_series import Series,SeriesError
    try:
     if data.get('_transport_verified') is not True:raise SeriesError('Narration requires checked requests')
     if op=='series_begin':
      text=data.get('text');plan=data.get('plan')
      if not isinstance(text,str) or not text.strip() or len(text.encode())>8192 or not isinstance(plan,dict):raise SeriesError('Invalid narration text or plan')
      if shutil.disk_usage(self.root).free<128*1024*1024:raise SeriesError('Narration cache needs free storage')
      value=self.series.get(key) or self._load_series(key,text)
      if value is None:
       if len(self.series)>=4:
        oldest=next((k for k in self.series if k not in self.active),None)
        if oldest is None:raise SeriesError('Narration cache is busy')
        self.series.pop(oldest).close()
       value=Series(self,key,text,plan,session);self.series[key]=value
      if value.text!=text or value.parts!=plan.get('sections'):raise SeriesError('Narration identity changed')
      if key in self.active and value.session!=session:raise SeriesError('Narration is playing for another session')
      value.session=session
      if key not in self.active:value.error=None
      return value.status()
     return self.series[key].request(data,session)
    except SeriesError as exc:raise NativeAudioError(str(exc)) from exc
   item=self.items.get(key)
   if op=='begin':
    text=data.get('text')
    verified=data.get('_transport_verified') is True
    if not isinstance(text,str) or not text.strip() or len(text.encode())>8192 or '\x00' in text:raise NativeAudioError('Native speech needs its original script')
    item=item or self._load(key,text)
    prefix=None;force_fresh=False
    if item:
     if item.text!=text:raise NativeAudioError('Native audio identity changed')
     if item.session!=session and key in self.active:raise NativeAudioError('Native playback cache is busy')
     item.session=session
     if not item.error:return {'received':item.received,'complete':item.done,'sha256':item.digest.hexdigest()}
     if key in self.active:raise NativeAudioError('Native playback cache is busy')
     item.abort();previous=item.folder.with_name(key+'.partial-'+uuid.uuid4().hex[:8]);item.folder.rename(previous)
     if not (previous/'invalid.json').exists():prefix=previous/'voice.mp3'
     else:force_fresh=True
     del self.items[key]
    # Only paired device uploads are admitted by the caller. Retain at most eight recent replies.
    if shutil.disk_usage(self.root).free<128*1024*1024:raise NativeAudioError('Native audio cache needs free storage')
    if len(self.items)>=8:
     oldest=next((k for k in self.items if k not in self.active),None)
     if oldest is None:raise NativeAudioError('Native playback cache is busy')
     old=self.items.pop(oldest)
     if not old.done:old.abort()
     shutil.rmtree(old.folder)
    prefix=prefix or (None if force_fresh else self._partial(key,text))
    item=Item(self.root,key,text,session);self.items[key]=item;item.integrity=verified
    item.prefix_origin=prefix
    (item.folder/'transfer.json').write_text(json.dumps({'source_sha256':hashlib.sha256(text.encode()).hexdigest(),'transport_integrity':int(verified)}))
    if prefix:
     try:
      with prefix.open('rb') as old:
       while True:
        block=old.read(8192)
        if not block:break
        item.file.write(block);item.decoder.stdin.write(block);item.digest.update(block);item.received+=len(block)
      item.file.flush();item.decoder.stdin.flush()
     except BaseException:item.abort();raise
    if self.warming is None:self.warming=self.pool.submit(self.warm)
    # Analyze decoded windows while the remaining MP3 is arriving. The result
    # cannot be published or played until end verifies the entire transfer.
    item.timing_future=self.pool.submit(self._schedule,item,None)
    return {'received':item.received,'complete':False,'sha256':item.digest.hexdigest()}
   if item is None or item.session!=session:raise NativeAudioError('Stale native audio transfer')
   if getattr(item,'integrity',False) and data.get('_transport_verified') is not True:raise NativeAudioError('Native transfer requires verified requests')
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
    if 'bytes' in data or 'sha256' in data:
     if data.get('bytes')!=len(raw) or data.get('sha256')!=hashlib.sha256(raw).hexdigest():raise NativeAudioError('Native audio block integrity check failed')
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
    if data.get('bytes')!=item.received or data.get('sha256')!=item.digest.hexdigest():
     (item.folder/'invalid.json').write_text('{"reason":"integrity"}')
     if getattr(item,'prefix_origin',None):(item.prefix_origin.parent/'invalid.json').write_text('{"reason":"integrity"}')
     item.abort();raise NativeAudioError('Native audio integrity check failed')
    item.file.close();item.decoder.stdin.close();item.decoder.wait(timeout=8)
    if item.mp3.stat().st_size!=item.received or hashlib.sha256(item.mp3.read_bytes()).hexdigest()!=data.get('sha256'):item.error='Native cache write integrity failed';raise NativeAudioError(item.error)
    if item.decoder.returncode or item.size()<320: item.error='Native audio decoding failed';raise NativeAudioError(item.error)
    meta={'source_sha256':hashlib.sha256(item.text.encode()).hexdigest(),'audio_bytes':item.received,'audio_sha256':item.digest.hexdigest(),'pcm_bytes':item.size(),'transport_integrity':int(getattr(item,'integrity',False))}
    try:
     target=item.folder/'complete.json';temp=target.with_suffix('.pending');temp.write_text(json.dumps(meta));temp.replace(target)
    except OSError:
     item.error='Native cache finalization failed';raise NativeAudioError(item.error)
    item.done=True
    return {'received':item.received}
   raise NativeAudioError('Unknown native audio operation')
 def _load_series(self,key,text):
  from muse_narration_series import Series,SeriesError
  for path in self.root.parent.glob('*/'+key+'/series.json'):
   try:
    if path.is_symlink() or path.parent.is_symlink() or path.parent.parent.is_symlink():continue
    manifest=json.loads(path.read_text())
    if manifest.get('source')!=hashlib.sha256(text.encode()).hexdigest():continue
    value=Series(self,key,text,manifest['plan'],None,path.parent)
    for i in range(len(value.parts)):value._load_part(i)
    sealed=path.parent/'sealed.json'
    if sealed.exists() and len(value.timings)==len(value.parts):
     summary=json.loads(sealed.read_text())
     mixed=summary.get('mode')=='mixed-pcm'
     try:value.sealed=summary==value.seal_value(mixed)
     except (OSError,ValueError,KeyError,SeriesError):value.sealed=False
    self.series[key]=value;return value
   except (OSError,ValueError,KeyError,TypeError):continue
  return None
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
    digest=hashlib.sha256(mp3.read_bytes())
    if digest.hexdigest()!=meta['audio_sha256']:continue
    item=object.__new__(Item)
    item.key,item.text,item.session=key,text,None
    item.folder,item.mp3,item.pcm=folder,mp3,pcm
    item.received,item.done,item.error=meta['audio_bytes'],True,None
    item.digest=digest
    item.integrity=meta.get('transport_integrity')==1
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
  spoken=SpeechText(item.text)
  cache=item.folder/'timing.json'
  signature=hashlib.sha256(item.text.encode()).hexdigest()
  try:
   stored=json.loads(cache.read_text())
   if item.done and stored.get('version')==2 and stored.get('source')==signature and stored.get('pcm_bytes')==item.size():
    values=stored['words']
    if len(values)==len(spoken.words) and all(0<=t<item.size()/32000 and 0<=a<b<=len(item.text.encode()) for t,a,b in values):return values
  except (OSError,ValueError,TypeError,KeyError):pass
  words=spoken.words;anchors={};position=0.;cursor=0;deadline=time.monotonic()+1200
  def wait_for_window(seconds):
   while not item.done and item.size()<seconds*32000:
    check_cancel(cancel)
    if item.error:raise NativeAudioError(item.error)
    if time.monotonic()>deadline:raise NativeAudioError('Timing preparation timed out')
    time.sleep(.025)
   check_cancel(cancel)
   if item.error:raise NativeAudioError(item.error)
  try:
   aligner=self.warm()
   while cursor<len(words):
    wait_for_window(position+24)
    duration=item.size()/32000.
    if position>=duration-.04:break
    check_cancel(cancel)
    end=min(duration,position+24);final_window=item.done and end==duration
    limit=end if final_window else end-.8
    candidates=words[cursor:cursor+120];a=candidates[0][0];z=candidates[-1][1]
    with item.pcm.open('rb') as stream:
     stream.seek(int(position*16000)*2);pcm_window=stream.read(int(end*16000)*2-int(position*16000)*2)
    aligned=aligner.align(pcm_window,spoken.text[a:z])
    progress=[]
    for begin,finish,lo,hi in aligned:
     if finish-begin<.025 or position+finish>limit:continue
     for index in range(cursor,min(len(words),cursor+len(candidates))):
      word=words[index]
      if word[0]<a+hi and word[1]>a+lo:
       anchors.setdefault(index,position+begin);progress.append((index,position+begin))
    if final_window:break
    if not progress:break
    index,at=max(progress,key=lambda x:x[0])
    if index<=cursor or at<=position+.2:break
    # Overlap the last stable word for analysis only. Playback is one uncut file.
    cursor,position=index,at
  except Exception:
   check_cancel(cancel) # cancellation remains authoritative; alignment is optional
  # Even failed/partial alignment waits for final integrity before using audio.
  wait_for_window(float('inf'))
  size=item.size()
  if size<320 or size%2 or size>32*1024*1024:raise NativeAudioError('Invalid complete decoded audio')
  # Use true timings only when every word has a stable monotonic anchor. A
  # collapsed alignment can otherwise make the reader race to the last word.
  good=len(anchors)==len(words) and all(anchors[i]<=anchors[i+1] for i in range(len(words)-1))
  if good:schedule=[(anchors[i],w[2],w[3]) for i,w in enumerate(words)]
  else:
   pcm=item.pcm.read_bytes();timing=WordTiming(item.text);schedule=timing.phrase(timing.normalized,pcm)
  check_cancel(cancel)
  try:
   temporary=cache.with_suffix('.pending');temporary.write_text(json.dumps({'version':2,'source':signature,'pcm_bytes':size,'aligned':good,'words':schedule}));temporary.replace(cache)
  except OSError:print(json.dumps({'phase':'timing_cache_unavailable','audio_preserved':True}),flush=True)
  return schedule
 def _fallback_schedule(self,item,cancel):
  from muse_voice_backend import check_cancel
  from muse_word_timing import WordTiming
  check_cancel(cancel)
  if not item.done or item.error:raise NativeAudioError('Complete native audio is unavailable')
  pcm=item.pcm.read_bytes();timing=WordTiming(item.text)
  return timing.phrase(timing.normalized,pcm)
 def close(self):
  # Do not strand an analysis worker waiting for the end of an interrupted
  # upload during a service restart. Keep its protected MP3 prefix on disk.
  with self.lock:
   for series in self.series.values():series.close()
   for item in self.items.values():
    if not item.done:item.abort()
    future=getattr(item,'timing_future',None)
    if future:future.cancel()
  self.pool.shutdown(wait=False)
  if self.aligner is not None and hasattr(self.aligner,'close'):self.aligner.close()
 def play(self,backend,key,text,volume,cancel,on_play,on_buffering=None):
  from muse_voice_backend import check_cancel
  with self.lock:series=self.series.get(key) or self._load_series(key,text)
  if series is not None:
   if series.text!=text:raise NativeAudioError('Narration script changed')
   with self.lock:self.active.add(key)
   if backend.mic:backend.mic.pause_output(True)
   try:
    from muse_narration_series import SeriesError
    try:return series.play(backend,volume,cancel,on_play,on_buffering)
    except SeriesError as exc:raise NativeAudioError(str(exc)) from exc
   finally:
    with self.lock:self.active.discard(key)
    if backend.mic:backend.mic.pause_output(False)
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
   prepared=getattr(item,'timing_future',None)
   try:
    if prepared is not None:
     timing_deadline=time.monotonic()+2
     while not prepared.done():
      check_cancel(cancel)
      if time.monotonic()>timing_deadline:raise NativeAudioError('Word timing is taking too long')
      time.sleep(.025)
     schedule=prepared.result()
    else:schedule=self._schedule(item,cancel)
   except Exception:
    check_cancel(cancel)
    print(json.dumps({'phase':'timing_fallback','audio_preserved':True}),flush=True)
    schedule=self._fallback_schedule(item,cancel)
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
   try:(item.folder/'last-playback.json').write_text(json.dumps(result))
   except OSError:print(json.dumps({'phase':'playback_report_unavailable','audio_complete':True}),flush=True)
   return result
  finally:
   with self.lock:self.active.discard(key)
   if backend.mic:backend.mic.pause_output(False)
