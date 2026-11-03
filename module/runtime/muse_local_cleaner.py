"""Scoped vendor LLM client: only its own task is created and released."""
import json,os,re,time,uuid
from pathlib import Path
import muse_zmq as zmq
from muse_voice_backend import encode,pack_pair,unpack_pair
MODEL='qwen2.5-0.5B-prefill-20e'
SYSTEM='You edit speech transcripts. Fix spelling, grammar and misheard words. Keep every detail. Never shorten, answer or explain. Output only the complete corrected sentence. Join mis-split words, for example land turn may mean lantern. Use common sense, but do not invent details.'
class LocalCleaner:
 def __init__(self,check=lambda:None):
  self.check=check;self.context=zmq.Context();self.route='ipc:///tmp/musevoice/client-'+uuid.uuid4().hex[:24]+'.sock';self.pull=self.context.socket(zmq.PULL,timeout_ms=100,max_bytes=65536);self.pull.bind(self.route);self.task=None;self.feed=None;self.feed_id=None;self.setup_pending=False;self.owner=Path("/var/lib/muse-core2-bridge/transcript-review-owned.json");self.boot=Path("/proc/sys/kernel/random/boot_id").read_text().strip();self.recover_owned()
 def remember(self):
  tmp=self.owner.with_suffix('.new')
  with tmp.open('w') as f:os.chmod(tmp,0o600);json.dump(dict(boot=self.boot,feed=self.feed_id,task=self.task),f);f.flush();os.fsync(f.fileno())
  os.replace(tmp,self.owner)
 def recover_owned(self):
  if not self.owner.exists():return
  old=json.loads(self.owner.read_text())
  if old.get('boot')!=self.boot:self.owner.unlink();return
  feed=old.get('feed')
  if not re.fullmatch(r'muse_clean_asr\.[0-9]{1,10}',str(feed)):raise ValueError('Invalid local review owner')
  saved=self.check;self.check=lambda:None
  try:
   reply=self.receive(self.send('llm','taskinfo'),10)
   for task in reply.get('data',[]):
    if not re.fullmatch(r'llm\.[0-9]{1,10}',str(task)):continue
    info=self.receive(self.send(task,'taskinfo'),10).get('data',{})
    if info.get('model')==MODEL and info.get('inputs')==[feed]:
     self.task=task;self.release()
   self.sysrpc('release_unit',feed)
   if self.owner.exists():self.owner.unlink()
  finally:self.check=saved
 def send(self,target,action,data=None,kind='None'):
  identity='clean-'+uuid.uuid4().hex[:16];req=dict(request_id=identity,work_id=target,action=action,object=kind,data=data if data is not None else 'None')
  s=self.context.socket(zmq.REQ,timeout_ms=5000,max_bytes=8192)
  try:s.connect('ipc:///tmp/rpc.llm');s.send_multipart([action.encode(),pack_pair(self.route.encode(),encode(req))]);ack=s.recv();assert ack==b'None','LLM transport rejected request'
  finally:s.close()
  return identity
 def receive(self,identity,timeout=60):
  end=time.monotonic()+timeout
  while time.monotonic()<end:
   self.check()
   if not self.pull.readable(100):continue
   reply=json.loads(self.pull.recv())
   if reply.get('request_id')!=identity:continue
   if reply.get('error',{}).get('code')!=0:raise RuntimeError('Local LLM error '+str(reply.get('error',{}).get('code')))
   return reply
  raise TimeoutError('Local LLM timed out')
 def sysrpc(self,action,data):
  s=self.context.socket(zmq.REQ,timeout_ms=5000,max_bytes=8192)
  try:s.connect('ipc:///tmp/rpc.sys');s.send_multipart([action.encode(),data.encode()]);return s.recv()
  finally:s.close()
 def setup(self):
  if not self.feed:
   available=int(re.search(r'MemAvailable:\s+(\d+)',Path('/proc/meminfo').read_text())[1])*1024
   if available<200*1024**2:raise RuntimeError('Insufficient free memory for review')
   n,ports=unpack_pair(self.sysrpc('register_unit','muse_clean_asr'));out,_=unpack_pair(ports);assert n.isdigit() and out.startswith(b'ipc:///tmp/llm/');self.feed_id='muse_clean_asr.'+n.decode();self.remember()
   self.feed=self.context.socket(zmq.XPUB,timeout_ms=5000,max_bytes=16384);self.feed.bind(out.decode())
  self.setup_pending=True
  identity=self.send('llm','setup',dict(model=MODEL,response_format='llm.utf-8.stream',input=[self.feed_id],enoutput=True,max_token_len=256,prompt=SYSTEM),'llm.setup');saved=self.check;self.check=lambda:None
  try:reply=self.receive(identity,90)
  finally:self.check=saved
  task=reply.get('work_id');assert re.fullmatch(r'llm\.[0-9]{1,10}',str(task));self.task=task;self.setup_pending=False;self.remember();self.check()
  end=time.monotonic()+5
  while time.monotonic()<end:
   if self.feed.readable(100) and self.feed.recv()==b'\x01':return
  raise TimeoutError('Local cleaner subscription missing')
 def correct(self,text):
  self.check()
  if self.task:self.release()
  self.setup()
  identity='clean-'+uuid.uuid4().hex[:16];self.feed.send(encode(dict(request_id=identity,work_id=self.task,action='inference',zmq_com=self.route,object='llm.utf-8',data='Correct this transcript: '+text)));end=time.monotonic()+45;pieces=[]
  while time.monotonic()<end:
   self.check()
   if not self.pull.readable(100):continue
   r=json.loads(self.pull.recv())
   if r.get('work_id')!=self.task:continue
   if r.get('error',{}).get('code')!=0:raise RuntimeError('Local LLM inference failed')
   if r.get('object') not in ('llm.utf-8','llm.utf-8.stream'):continue
   d=r.get('data')
   if isinstance(d,str):return d.strip()
   if isinstance(d,dict) and isinstance(d.get('delta'),str):
    pieces.append(d['delta'])
    if sum(map(len,pieces))>4096:raise ValueError('Local LLM output too long')
    if d.get('finish'):return ''.join(pieces).strip()
  raise TimeoutError('Local cleanup timed out')
 def release(self):
  if self.task:
   # Pause only this client's own inference before releasing its model.
   self.send(self.task,'pause')
   identity=self.send(self.task,'exit')
   saved=self.check;self.check=lambda:None
   try:self.receive(identity,20)
   finally:self.check=saved
   self.task=None
   if self.feed:self.feed.close();self.feed=None
   if self.feed_id:self.sysrpc('release_unit',self.feed_id);self.feed_id=None
   if self.owner.exists():self.owner.unlink()
 def close(self):
  try:self.release()
  finally:
   if self.feed:self.feed.close()
   if self.feed_id:self.sysrpc('release_unit',self.feed_id)
   if not self.task and not self.setup_pending and self.owner.exists():self.owner.unlink()
   self.pull.close();self.context.close()
   try:Path(self.route[6:]).unlink()
   except FileNotFoundError:pass
