"""Whisper-base adapter retaining the verified Muse voice/audio protocol."""
import threading,time,uuid
from muse_voice_backend import Backend as LegacyBackend,BridgeError,check_cancel
from muse_whisper_asr import WhisperASR
class MicProxy:
 def __init__(self,meter,asr):self.meter,self.asr=meter,asr
 def __getattr__(self,name):return getattr(self.meter,name)
 def record(self,on):
  self.meter.record(False);self.meter.pause_output(True)
  if not on:self.asr.finish()
 def pause_output(self,on):self.meter.pause_output(True);self.asr.output_guard(on)
 def close(self):self.meter.close()
class Backend:
 recognizer_name='whisper-base';batch_asr=True
 def __init__(self,capture_endpoint='ipc:///tmp/llm/pcm.cap.socket',state_dir='/var/lib/muse-core2-bridge'):
  object.__setattr__(self,'_legacy',LegacyBackend(state_dir=state_dir));object.__setattr__(self,'_asr',None);object.__setattr__(self,'_capture_endpoint',capture_endpoint);object.__setattr__(self,'_closed',False)
 def __getattr__(self,name):return getattr(self._legacy,name)
 def __setattr__(self,name,value):
  if name.startswith('_'):object.__setattr__(self,name,value)
  else:setattr(self._legacy,name,value)
 def initialize(self):
  self._legacy.initialize();self._legacy.mic.record(False);self._legacy.mic.pause_output(True)
  self._asr=WhisperASR(lambda:self._legacy.mic_level,self._capture_endpoint)
  self._legacy.mic=MicProxy(self._legacy.mic,self._asr)
 def start_asr(self):
  try:
   if self._asr.failure:
    self._asr.close();self._asr=WhisperASR(lambda:self._legacy.mic_level,self._capture_endpoint);self._legacy.mic.asr=self._asr
   self._asr.begin();self._legacy.asr_request='whisper-base-'+uuid.uuid4().hex
  except RuntimeError as exc:raise BridgeError(-12,'English speech recognition is unavailable') from exc
 def pause_asr(self):
  if self._asr:self._asr.finish()
  if self._legacy.mic:self._legacy.mic.record(False);getattr(self._legacy.mic,"meter",self._legacy.mic).pause_output(True)
 def stop_asr(self):
  if self._asr:self._asr.abort()
 def asr_finished(self):return self._asr.done()
 def recv(self,timeout=.1):
  end=time.monotonic()+timeout
  while True:
   try:text=self._asr.poll()
   except RuntimeError as exc:raise BridgeError(-12,'English recognition did not finish; please try again') from exc
   if text is not None:return dict(work_id=self.asr_id,request_id=self.asr_request,object='asr.utf-8.stream',data=dict(delta=text,finish=True),error=dict(code=0,message=''))
   reply=self._legacy.recv(min(.025,max(0,end-time.monotonic())))
   if reply is not None and reply.get('work_id')!=self.asr_id:return reply
   if time.monotonic()>=end:return None
 def listen(self,cancel,end,on_listening=None,on_level=None,max_seconds=90):
  self.stop_audio();check_cancel(cancel);chunks=[];start=time.monotonic();released=None;flushed=False;next_level=0
  try:
   self.start_asr();self.mic.record(True)
   if on_listening:on_listening()
   while True:
    check_cancel(cancel);now=time.monotonic()
    if on_level and now>=next_level:on_level();next_level=now+.15
    reply=self.recv(.05)
    if reply and reply.get('work_id')==self.asr_id and reply.get('request_id')==self.asr_request:
     self.checked(reply);chunks.append(reply['data']['delta'])
    if end.is_set() and released is None:released=now
    if released is not None and now-released>=.25 and not flushed:self.mic.record(False);flushed=True
    if flushed and self.asr_finished():
     text=' '.join(chunks).strip()
     if not text:raise BridgeError(-12,'No speech heard')
     return text
    if released is not None and now-released>60:raise BridgeError(-12,'English recognition timed out')
    if released is None and now-start>max_seconds:raise BridgeError(-12,'Recording exceeded the 90-second limit')
  finally:self.pause_asr()
 def close(self):
  if self._closed:return
  self._closed=True
  try:
   if self._asr:self._asr.close()
  finally:self._legacy.close()
