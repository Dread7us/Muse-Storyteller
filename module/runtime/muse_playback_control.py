"""Bounded playback controls; all native audio RPC stays on its owner thread."""
import threading,time

class PlaybackControl:
 def __init__(self,volume):self.lock=threading.Lock();self.paused=False;self.volume=volume
 def update(self,paused,volume):
  if type(paused) is not bool or type(volume) is not int or not 0<=volume<=100:raise ValueError('Invalid playback control')
  with self.lock:self.paused,self.volume=paused,volume
 def read(self):
  with self.lock:return self.paused,self.volume

def play(backend,pcm,volume,cancel,check_cancel,scale_pcm,rate,Error):
 control=getattr(backend,'playback_control',None) or PlaybackControl(volume)
 schedule=list(backend.word_schedule);backend.word_schedule=[]
 expected=len(pcm)/(rate*2);offset=0.;index=0;active=False;saw=False;was_paused=False
 beginning=time.monotonic();paused_total=0.;started=beginning;applied=volume
 callback=getattr(backend,'playback_state_callback',None)
 def bookmark():
  # Restart the current highlighted word, never the whole message. This
  # deliberately permits a short word repeat instead of dropping its opening.
  return schedule[max(0,index-1)][0] if schedule and index else offset
 try:
  while True:
   check_cancel(cancel);paused,wanted=control.read()
   if paused:
    if active:offset=bookmark();backend.stop_audio();active=False
    if not was_paused:
     was_paused=True
     if callback:callback(True)
    at=time.monotonic()
    while control.read()[0]:
     check_cancel(cancel)
     if cancel:cancel.wait(.02)
     else:time.sleep(.02)
    elapsed=time.monotonic()-at;paused_total+=elapsed
    backend.playback_pause_total=getattr(backend,'playback_pause_total',0.)+elapsed
    continue
   if active and wanted!=applied:
    offset=bookmark();backend.stop_audio();active=False
   if not active:
    offset=max(0.,min(offset,expected));sample=int(offset*rate)*2
    if sample>=len(pcm):backend.stop_audio();return {'pcm_bytes':len(pcm),'observed_running':True,'joined':True}
    index=next((i for i,w in enumerate(schedule) if w[0]>=offset),len(schedule))
    chunk=scale_pcm(pcm[sample:],wanted)
    backend.stop_audio()
    if backend.rpc('audio','play_raw',chunk)!=b'None':raise Error(-11,'Native playback was rejected')
    backend.active_play=True;active=True;saw=False;started=time.monotonic();applied=wanted
    applied_callback=getattr(backend,'playback_volume_callback',None)
    if applied_callback:applied_callback(wanted)
    if was_paused:
     was_paused=False
     if callback:callback(False)
   status=backend.rpc('audio','audio_status',b'play')
   if status==b'Runing':
    saw=True;position=offset+time.monotonic()-started
    while backend.word_callback and index<len(schedule) and schedule[index][0]<=position:
     _,a,b=schedule[index];backend.word_callback(a,b);index+=1
   elif status==b'None':
    if saw:
     if time.monotonic()-started<max(0.,(expected-offset)*.8-.25):raise Error(-14,'Native audio ended too early')
     backend.stop_audio();return {'pcm_bytes':len(pcm),'observed_running':True,'joined':True}
   else:raise Error(-3,'Unexpected native playback status')
   if time.monotonic()-beginning-paused_total>expected*3+30:raise Error(-12,'Native playback completion was not observed')
   if cancel:cancel.wait(.01)
   else:time.sleep(.01)
 except BaseException:
  backend.stop_audio();raise
