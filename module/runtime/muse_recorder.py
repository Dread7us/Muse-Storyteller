"""Explicit local recorder: append-only transcripts, bounded compressed WAV chunks.
No cloud/network calls, archive access, or automatic recording after restart.
"""
import array,contextlib,gzip,io,json,math,os,re,shutil,threading,time,uuid,wave
from pathlib import Path
ROOT='/var/lib/muse-recordings'
RESERVE=1024**3
AUDIO_BUDGET=2*1024**3
class Recorder:
 def __init__(self,root=ROOT):
  self.root=Path(root);self.root.mkdir(mode=0o700,parents=True,exist_ok=True);self.lock=threading.RLock();self.folder=None;self.state={'state':'idle','active':False,'note':'Ready for local recording','id':'','seconds':0,'lines':0,'audio':False,'audio_bytes':0};self.partial='';self.start_at=0.;self.audio_pcm=bytearray();self.audio_seq=0;self.last_voice=0.
  self.review_thread=None;self.recover()
 def atomic(self,path,value):
  tmp=path.with_name(path.name+'.new')
  with tmp.open('w',encoding='utf8') as f:os.chmod(tmp,0o600);json.dump(value,f,ensure_ascii=False);f.flush();os.fsync(f.fileno())
  os.replace(tmp,path)
 def persist(self):
  if self.folder:self.atomic(self.folder/'session.json',self.state)
 def recover(self):
  folders=sorted(p for p in self.root.iterdir() if p.is_dir() and re.fullmatch(r'rec-[0-9]{6}-[0-9a-f]{8}',p.name))
  if not folders:return
  self.folder=folders[-1]
  try:self.state=json.loads((self.folder/'session.json').read_text(encoding='utf8'))
  except (OSError,ValueError):return
  if self.state.get('active'):
   try:
    pending=json.loads((self.folder/'pending.json').read_text(encoding='utf8'))
    if pending.get('text') and (self.folder/'transcript.txt').stat().st_size==pending.get('offset'):
     self.append(pending['text'],partial=True,elapsed=pending.get('seconds',0))
   except (OSError,ValueError):pass
   self.state.update(active=False,state='interrupted',note='Interrupted recording recovered');self.persist()
 def prepare(self,audio=True):
  with self.lock:
   if self.state.get('active'):raise ValueError('Recorder already active')
   if shutil.disk_usage(self.root).free<RESERVE:raise ValueError('Not enough free storage; existing files kept')
   numbers=[int(p.name.split('-')[1]) for p in self.root.iterdir() if re.fullmatch(r'rec-[0-9]{6}-[0-9a-f]{8}',p.name)]
   n=max(numbers,default=0)+1
   if n>999999:raise ValueError('Recording index full')
   identity='rec-%06d-%s'%(n,uuid.uuid4().hex[:8]);self.folder=self.root/identity;self.folder.mkdir(mode=0o700)
   p=self.folder/'transcript.txt'
   with p.open('x',encoding='utf8',newline='\n') as f:os.chmod(p,0o600);f.write('Recording %06d - local transcript\nTimes below are elapsed from the start.\n\n'%n);f.flush();os.fsync(f.fileno())
   self.state=dict(state='starting',active=True,note='Preparing offline microphone...',id=identity,seconds=0,lines=0,audio=bool(audio),audio_bytes=0)
   self.partial='';self.start_at=time.monotonic();self.audio_pcm=bytearray();self.audio_seq=0;self.last_voice=0.;self.persist();return self.snapshot()
 def append(self,text,partial=False,elapsed=None):
  text=' '.join(str(text).replace('\x00','').split())
  if not text:return
  if len(text.encode('utf8'))>65536:raise ValueError('Recognizer response too long')
  with self.lock:
   seconds=int(self.state.get('seconds',0) if elapsed is None else elapsed);stamp='%02d:%02d:%02d'%(seconds//3600,(seconds//60)%60,seconds%60)
   with (self.folder/'transcript.txt').open('a',encoding='utf8',newline='\n') as f:f.write('[%s%s] %s\n'%(stamp,' partial' if partial else '',text));f.flush();os.fsync(f.fileno())
   self.state['lines']=int(self.state.get('lines',0))+1
 def checkpoint(self):
  with self.lock:
   if not self.folder:return
   self.atomic(self.folder/'pending.json',dict(text=self.partial,offset=(self.folder/'transcript.txt').stat().st_size,seconds=self.state['seconds']));self.persist()
 def audio(self,pcm,final=False):
  if not self.state.get('audio'):return
  self.audio_pcm.extend(pcm)
  if len(self.audio_pcm)<960000 and not final:return
  if not self.audio_pcm:return
  if self.state['audio_bytes']+len(self.audio_pcm)>AUDIO_BUDGET or shutil.disk_usage(self.root).free<RESERVE+len(self.audio_pcm):
   self.state.update(audio=False,note='Audio budget reached; text continues');self.audio_pcm.clear();return
  raw=bytes(self.audio_pcm);self.audio_pcm.clear();self.audio_seq+=1;path=self.folder/('audio-%06d.wav.gz'%self.audio_seq);tmp=path.with_suffix(path.suffix+'.part')
  buf=io.BytesIO()
  with wave.open(buf,'wb') as w:w.setnchannels(1);w.setsampwidth(2);w.setframerate(16000);w.writeframes(raw)
  with tmp.open('xb') as f:
   os.chmod(tmp,0o600)
   with gzip.GzipFile(fileobj=f,mode='wb',compresslevel=3,mtime=0) as g:g.write(buf.getvalue())
   f.flush();os.fsync(f.fileno())
  os.replace(tmp,path);self.state['audio_bytes']+=path.stat().st_size
 def snapshot(self,page=None,selection=None,original=False):
  with self.lock:
   state=dict(self.state)
   folder=self.folder
   if selection:
    if not re.fullmatch(r'rec-[0-9]{6}-[0-9a-f]{8}',selection):raise ValueError('Invalid recording')
    folder=self.root/selection
   folders=sorted(p.name for p in self.root.iterdir() if re.fullmatch(r'rec-[0-9]{6}-[0-9a-f]{8}',p.name))
   index=folders.index(folder.name) if folder and folder.name in folders else 0
   state['older']=bool(folders and index>0);state['newer']=bool(folders and index+1<len(folders))
   state['audio_mb']=int(state.get('audio_bytes',0))//1048576
   state['supported']=True;state['text']='';state['page']=0;state['pages']=1
   if folder and not state.get('active'):
    from muse_transcript_review import status
    review_state=status(folder)
    if review_state.get('status')=='running':state['note']='Reviewing offline: %s/%s chunks'%(review_state.get('chunks',0),review_state.get('total',0))
    elif review_state.get('status')=='deferred':state['note']='Review waiting for Muse idle; original available'
    elif review_state.get('status')=='complete':state['note']='AI review ready; Original is always retained'
    elif review_state.get('status')=='error':state['note']='Review unavailable; original transcript retained'
   if page is not None and folder:
    path=folder/('reviewed.txt' if not original and (folder/'reviewed.txt').exists() else 'transcript.txt')
    size=path.stat().st_size
    if size>64*1024**2:raise ValueError('Transcript too large for device preview; use USB export')
    # Bound reads to requested page; do not load a long recording into RAM.
    pages=max(1,(size+767)//768);page=max(0,min(int(page),pages-1))
    with path.open('rb') as f:
     def boundary(pos):
      if pos>=size:return size
      base=max(0,pos-3);f.seek(base);chunk=f.read(4);at=pos-base
      while at>0 and chunk[at]&0xc0==0x80:at-=1
      return base+at
     start=boundary(page*768);finish=boundary((page+1)*768);f.seek(start);data=f.read(finish-start)
    state.update(text=data.decode('utf8',errors='ignore'),page=page,pages=max(1,(size+767)//768))
   return state
 def select(self,step):
  with self.lock:
   if self.state.get('active'):raise ValueError('Finish recording before browsing')
   folders=sorted(p for p in self.root.iterdir() if p.is_dir() and re.fullmatch(r'rec-[0-9]{6}-[0-9a-f]{8}',p.name))
   if not folders:return self.snapshot()
   i=folders.index(self.folder) if self.folder in folders else len(folders)-1
   self.folder=folders[max(0,min(len(folders)-1,i+step))];self.state=json.loads((self.folder/'session.json').read_text(encoding='utf8'));return self.snapshot(page=0)
 def review_if_idle(self,idle):
  from muse_transcript_review import status,review
  with self.lock:
   if not self.folder or self.state.get('active') or not idle():return
   if self.review_thread and self.review_thread.is_alive():return
   if status(self.folder).get('status') in ('complete','error'):return
   folder=self.folder
   if not self.state.get('lines'):return
   self.review_thread=threading.Thread(target=review,args=(folder,idle),name='offline-transcript-review',daemon=True);self.review_thread.start()
 def released(self):
  with self.lock:
   self.state['active']=False
   if self.state['state'] in ('recording','saving','starting'):self.state.update(state='saved',note='Saved locally; Muse microphone restored')
   try:self.persist()
   except OSError:pass
 def run(self,backend,cancel,end):
  raw=None;ctx=None;last_checkpoint=0.;draining=None;failure=False
  try:
   if self.state['audio']:
    import muse_zmq as zmq
    from muse_mic_meter import scaled_pcm
    ctx=zmq.Context();raw=ctx.socket(zmq.SUB,timeout_ms=50,max_bytes=1024*1024);raw.connect('ipc:///tmp/llm/pcm.cap.socket')
   backend.start_asr();backend.mic.record(True)
   with self.lock:self.state.update(state='recording',note='Recording and transcribing offline',recognizer=getattr(backend,'recognizer_name','legacy-asr'));self.persist()
   while not cancel.is_set():
    now=time.monotonic()
    with self.lock:self.state['seconds']=int(now-self.start_at)
    fields=backend.mic.fields()
    if fields.get('mic_db',-100)>-58:self.last_voice=now
    if raw and draining is None:
     for _ in range(64):
      if not raw.readable(0):break
      pcm=raw.recv()
      if len(pcm)%2:raise ValueError('Invalid microphone audio frame')
      self.audio(scaled_pcm(pcm,getattr(backend,'mic_level',100)))
    reply=backend.recv(.02)
    if reply is not None and reply.get('work_id')==backend.asr_id and reply.get('request_id')==backend.asr_request:
     backend.checked(reply)
     if reply.get('object')!='asr.utf-8.stream':raise ValueError('Unexpected speech response')
     data=reply.get('data')
     if not isinstance(data,dict) or type(data.get('finish')) is not bool or not isinstance(data.get('delta'),str):raise ValueError('Invalid speech response')
     text=data['delta'].strip()
     if len(text.encode('utf8'))>65536:raise ValueError('Speech response too long')
     if data['finish']:
      if text and (getattr(backend,'batch_asr',False) or now-self.last_voice<12):self.append(text)
      self.partial=''
     else:self.partial=text if now-self.last_voice<12 else ''
    if end.is_set() and draining is None:
     backend.mic.record(False);draining=now+2.;self.state.update(state='saving',note='Transcribing final words and saving...')
    if draining:
     finished=getattr(backend,'asr_finished',None)
     if (finished() if finished else now>=draining):break
     if now>=draining+58:raise RuntimeError('Final English transcription timed out')
    if now-last_checkpoint>=1:
     if shutil.disk_usage(self.root).free<RESERVE:raise OSError('Low storage')
     self.checkpoint();last_checkpoint=now
   if self.partial:self.append(self.partial,partial=True);self.partial=''
   if cancel.is_set():self.state.update(state='interrupted',note='Stopped by device restart; text recovered')
  except Exception as exc:
   if getattr(exc,'code',None) not in (None,-12):backend.stop_asr()
   failure=True
   with self.lock:self.state.update(state='error',note='Recorder stopped safely; saved text retained')
   if self.partial:
    try:self.append(self.partial,partial=True);self.partial=''
    except OSError:pass
  finally:
   backend.pause_asr()
   if raw:raw.close()
   if ctx:ctx.close()
   try:self.audio(b'',final=True);self.checkpoint()
   except OSError:
    failure=True;self.state.update(state='error',note='Storage error; earlier saved text retained')
   if not failure and self.state['state']!='interrupted':self.state.update(state='saving',note='Saved; releasing microphone...')

class UnavailableRecorder:
 def snapshot(self,**_):return dict(supported=False,active=False,state='error',id='',note='Recorder storage unavailable; Muse still works',seconds=0,lines=0,audio=False,audio_mb=0,text='',page=0,pages=1,older=False,newer=False)
 def review_if_idle(self,idle):pass
 def prepare(self,*_):raise ValueError('Recorder storage unavailable; Muse still works')
 def select(self,*_):return self.snapshot()
 def released(self):pass
