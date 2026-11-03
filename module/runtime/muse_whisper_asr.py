"""English Whisper-base batches, bounded capture, owned private offline audio path.
No cloud requests or production audio/router reconfiguration. No transcript logs.
"""
import array,collections,io,json,math,os,queue,re,subprocess,threading,time,uuid,wave
from pathlib import Path
MODEL='whisper-base';BASE_HOME=Path('/opt/muse-whisper-base');RATE=32000
class Segmenter:
 def __init__(self,emit,max_seconds=28.,silence=.7):self.emit=emit;self.cap=int(max_seconds*RATE);self.silence=silence;self.active=False;self.pre=bytearray();self.buffer=bytearray();self.voiced=0.;self.quiet=0.;self.overlap=False
 def begin(self):self.active=True;self.buffer=bytearray(self.pre);self.voiced=0.;self.quiet=0.;self.overlap=False
 def flush(self,forced=False):
  if self.buffer and self.voiced>=.12:self.emit(bytes(self.buffer),self.overlap)
  carry=self.buffer[-int(.45*RATE):] if forced else b''
  self.buffer=bytearray(carry);self.voiced=0.;self.quiet=0.;self.overlap=forced
 def feed(self,pcm,db):
  self.pre.extend(pcm);self.pre=self.pre[-int(.25*RATE):]
  if not self.active:return
  self.buffer.extend(pcm);duration=len(pcm)/RATE
  if db>-48:self.voiced+=duration;self.quiet=0.
  else:self.quiet+=duration
  if not self.voiced and len(self.buffer)>RATE//2:self.buffer=self.buffer[-RATE//2:]
  if len(self.buffer)>=self.cap:self.flush(True)
  elif self.voiced and self.quiet>=self.silence:self.flush()
 def finish(self):
  if self.active:self.flush();self.active=False

def dedupe(previous,text):
 old=re.findall(r'\w+',previous.lower());matches=list(re.finditer(r'\w+',text));new=[m.group().lower() for m in matches]
 for n in range(min(12,len(old),len(new)),0,-1):
  if old[-n:]==new[:n]:return text[matches[n-1].end():].lstrip(' ,.;:!?')
 return text.strip()

class WhisperASR:
 def __init__(self,gain=lambda:100,capture_endpoint='ipc:///tmp/llm/pcm.cap.socket',startup_timeout=90):
  self.gain=gain;self.endpoint=capture_endpoint;self.stop=threading.Event();self.ready=threading.Event();self.frame=threading.Event();self.lock=threading.RLock();self.jobs=queue.Queue(6);self.outputs=queue.Queue();self.epoch=0;self.finished=threading.Event();self.finished.set();self.failure=None;self.busy=False;self.proc=None;self.count=0;self.last_decode=0.;self.finish_at=None;self.preroll_blocked=False;self.segmenter=Segmenter(self._enqueue)
  self.worker=threading.Thread(target=self._work,name='whisper-base-worker',daemon=True);self.worker.start()
  if not self.ready.wait(startup_timeout):self.close();raise RuntimeError('Whisper-base startup timed out')
  if self.failure:self.close();raise RuntimeError('Whisper-base could not start: '+self.failure)
  self.capture=threading.Thread(target=self._capture,name='whisper-pcm-capture',daemon=True);self.capture.start()
 def _status(self):
  try:
   p=Path('/var/lib/muse-core2-bridge/whisper-asr-status.json');tmp=p.with_suffix('.new');tmp.write_text(json.dumps(dict(model=MODEL,ready=self.ready.is_set() and not self.failure and not self.stop.is_set(),busy=self.busy,error=self.failure,chunks=self.count,last_decode_seconds=round(self.last_decode,3))));os.chmod(tmp,0o600);os.replace(tmp,p)
  except OSError:pass
 def _enqueue(self,pcm,overlap):
  try:self.jobs.put_nowait((self.epoch,pcm,overlap))
  except queue.Full:self.failure='Whisper recognition backlog is full';self.finished.set()
 def begin(self):
  if self.failure:raise RuntimeError(self.failure)
  if not self.frame.wait(3):raise RuntimeError('Whisper microphone has no PCM frames')
  with self.lock:
   self.epoch+=1;self.finish_at=None;self.finished.clear();self.segmenter.begin()
   while True:
    try:self.outputs.get_nowait()
    except queue.Empty:break
  return self.epoch
 def output_guard(self,on):
  with self.lock:self.preroll_blocked=bool(on);self.segmenter.pre.clear()
 def finish(self):
  with self.lock:
   if self.segmenter.active and self.finish_at is None:self.finish_at=time.monotonic()+.12
 def abort(self):
  with self.lock:self.epoch+=1;self.finish_at=None;self.segmenter.active=False;self.segmenter.buffer.clear();self.finished.set()
 def done(self):return self.finished.is_set() and self.outputs.empty()
 def poll(self):
  if self.failure:raise RuntimeError(self.failure)
  while True:
   try:epoch,text=self.outputs.get_nowait()
   except queue.Empty:return None
   if epoch==self.epoch:return text
 def _capture(self):
  import muse_zmq as z
  from muse_mic_meter import scaled_pcm,pcm_db
  ctx=z.Context();sub=ctx.socket(z.SUB,timeout_ms=100,max_bytes=1024*1024);sub.connect(self.endpoint)
  try:
   while not self.stop.is_set():
    if sub.readable(50):
     raw=sub.recv();pcm=scaled_pcm(raw,self.gain());db=pcm_db(pcm)
     with self.lock:
      if self.preroll_blocked and not self.segmenter.active:self.segmenter.pre.clear()
      else:self.segmenter.feed(pcm,db if db is not None else -100.)
     self.frame.set()
    with self.lock:
     if self.finish_at is not None and time.monotonic()>=self.finish_at:self.segmenter.finish();self._enqueue(None,False);self.finish_at=None
  except Exception as exc:self.failure='Whisper microphone capture failed: '+type(exc).__name__;self.finished.set();self._status()
  finally:sub.close();ctx.close()
 def _work(self):
  import muse_zmq as z
  from muse_voice_backend import encode,pack_pair
  ctx=z.Context();identity=uuid.uuid4().hex[:16];route='ipc:///tmp/musevoice/client-'+uuid.uuid4().hex[:24]+'.sock';rpc='ipc:///tmp/musevoice/whisper-audio-'+identity+'.sock';feed='ipc:///tmp/musevoice/whisper-pcm-'+identity+'.sock';pull=ctx.socket(z.PULL,timeout_ms=100,max_bytes=65536);pull.bind(route);pub=ctx.socket(z.XPUB,timeout_ms=1000,max_bytes=2*1024*1024);pub.bind(feed);proxy_stop=threading.Event();proxy_ready=threading.Event();task=None;proxy=None
  def serve_audio():
   rep=ctx.socket(z.REP,timeout_ms=100,max_bytes=65536);rep.bind(rpc);proxy_ready.set()
   try:
    while not proxy_stop.is_set():
     if not rep.readable(100):continue
     frames=rep.recv_multipart(2);rep.send(feed.encode() if frames[0]==b'cap' else b'None')
   finally:rep.close()
  def command(action,data=None,kind='None',timeout=60):
   request='wb33-'+uuid.uuid4().hex[:16];q=ctx.socket(z.REQ,timeout_ms=3000,max_bytes=65536);msg=dict(request_id=request,work_id=task or 'whisper',action=action,object=kind,data=data if data is not None else 'None')
   try:q.connect('ipc:///tmp/rpc.whisper');q.send_multipart([action.encode(),pack_pair(route.encode(),encode(msg))]);assert q.recv()==b'None'
   finally:q.close()
   end=time.monotonic()+timeout
   while time.monotonic()<end and not self.stop.is_set():
    if not pull.readable(100):continue
    reply=json.loads(pull.recv())
    if reply.get('request_id')!=request:continue
    if reply.get('error',{}).get('code')!=0:raise RuntimeError('Whisper native code '+str(reply.get('error',{}).get('code')))
    return reply
   raise RuntimeError('Whisper command did not finish')
  try:
   # A competing recognizer must not be taken over.
   for proc in Path('/proc').iterdir():
    if proc.name.isdigit():
     try:
      if 'llm_whisper' in (proc/'comm').read_text():raise RuntimeError('Another Whisper process is already running')
     except FileNotFoundError:pass
   proxy=threading.Thread(target=serve_audio,name='whisper-private-audio-rpc',daemon=True);proxy.start();assert proxy_ready.wait(3)
   env=dict(os.environ,LD_PRELOAD=str(BASE_HOME/'audio_rpc_shim.so'),MUSE_STT_PRIVATE_AUDIO_RPC=rpc,LD_LIBRARY_PATH=str(BASE_HOME/'lib')+':/opt/m5stack/lib:/opt/lib:/usr/local/m5stack/lib')
   self.proc=subprocess.Popen([str(BASE_HOME/'bin/llm_whisper-1.8')],stdin=subprocess.DEVNULL,cwd=str(BASE_HOME),env=env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
   end=time.monotonic()+10
   while not Path('/tmp/rpc.whisper').exists():
    if self.proc.poll() is not None or time.monotonic()>end:raise RuntimeError('Whisper process did not start')
    time.sleep(.05)
   time.sleep(.1)
   reply=command('setup',dict(model=MODEL,language='en',response_format='asr.utf-8',input=['sys.pcm','whisper.wav'],enoutput=True),'whisper.setup');task=reply.get('work_id');assert re.fullmatch(r'whisper\.[0-9]{1,10}',str(task))
   end=time.monotonic()+5
   while time.monotonic()<end:
    if pub.readable(100) and pub.recv()==b'\x01':break
   else:raise RuntimeError('Whisper private PCM subscription missing')
   self.ready.set();self._status();previous='';previous_epoch=None
   while not self.stop.is_set():
    try:epoch,pcm,overlap=self.jobs.get(timeout=.1)
    except queue.Empty:continue
    if epoch!=self.epoch:continue
    if pcm is None:self.finished.set();continue
    if previous_epoch!=epoch:previous='';previous_epoch=epoch
    self.busy=True;self._status();start=time.monotonic();pub.send(pcm);end=start+60;reply=None
    while time.monotonic()<end and not self.stop.is_set():
     if self.proc.poll() is not None:raise RuntimeError('Whisper process stopped')
     if not pull.readable(100):continue
     candidate=json.loads(pull.recv())
     if candidate.get('work_id')==task and candidate.get('object')=='asr.utf-8':reply=candidate;break
    if self.stop.is_set():break
    if reply is None:raise RuntimeError('Whisper transcription timed out')
    if reply.get('error',{}).get('code')!=0 or not isinstance(reply.get('data'),str):raise RuntimeError('Whisper returned invalid recognition')
    text=reply['data'].strip();text=re.sub(r'\.{2,}$','.',text);text=dedupe(previous,text) if overlap else text
    self.last_decode=time.monotonic()-start;self.count+=1;self.busy=False
    if epoch==self.epoch and re.search(r'[A-Za-z0-9]',text):self.outputs.put((epoch,text));previous=(previous+' '+text).strip()[-1200:]
    self._status()
  except Exception as exc:self.failure=type(exc).__name__+': '+str(exc)[:120];self.ready.set();self.finished.set();self._status()
  finally:
   if self.proc:
    self.proc.terminate()
    try:self.proc.wait(timeout=4)
    except subprocess.TimeoutExpired:self.proc.kill();self.proc.wait(timeout=3)
   proxy_stop.set()
   if proxy:proxy.join(3)
   pub.close();pull.close();ctx.close();self.busy=False;self._status()
   for name in (route,rpc,feed):
    try:Path(name[6:]).unlink()
    except FileNotFoundError:pass
 def close(self):
  self.stop.set();self.abort()
  if hasattr(self,'capture'):self.capture.join(3)
  self.worker.join(10)
  if self.worker.is_alive():raise RuntimeError('Whisper worker did not close')

