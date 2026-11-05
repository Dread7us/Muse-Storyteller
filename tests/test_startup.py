import ast,threading,time,types,unittest,json
from pathlib import Path
root=Path(__file__).resolve().parents[1]
def method(file,cls,name):
 tree=ast.parse(file.read_text());c=next(n for n in tree.body if isinstance(n,ast.ClassDef) and n.name==cls);f=next(n for n in c.body if isinstance(n,ast.FunctionDef) and n.name==name);m=ast.Module(body=[f],type_ignores=[]);scope={'time':time,'BridgeError':Error,'ASR_MODEL':'asr','TTS_MODEL':'tts','CAPABILITIES':[]};exec(compile(m,str(file),'exec'),scope);return scope[name]
class Error(Exception):
 def __init__(self,code,message):self.code,self.message=code,message
class Ready(unittest.TestCase):
 def test_secondary_cleanup_error_does_not_hide_the_original_fault(self):
  fn=method(root/'module/runtime/muse_voice_bridge.py','Engine','run');fn.__globals__.update(json=json)
  original=Error(-10,'Audio unavailable')
  class Backend:
   def initialize(self):raise original
   def close(self):raise Error(-31,'Cleanup failed')
  obj=types.SimpleNamespace(backend_factory=Backend,lock=threading.RLock(),ready=False,failure=None,current=None,pending=None)
  fn(obj);self.assertIs(obj.failure,original);self.assertFalse(obj.ready)
 def test_false_health_never_emits_ready(self):
  fn=method(root/'module/runtime/muse_voice_bridge.py','Engine','ready_event');events=[];obj=types.SimpleNamespace(backend=types.SimpleNamespace(startup_health=lambda:False),fail=lambda req,e:events.append(e.code),send=lambda *a,**k:self.fail('False readiness emitted'))
  fn(obj,{});self.assertEqual(events,[-23])
 def test_fresh_capture_and_warm_required(self):
  fn=method(root/'module/runtime/muse_whisper_asr.py','WhisperASR','healthy');obj=types.SimpleNamespace(warmed=True,failure=None,stop=threading.Event(),worker=types.SimpleNamespace(is_alive=lambda:True),proc=types.SimpleNamespace(poll=lambda:None),last_frame=time.monotonic());self.assertTrue(fn(obj))
  obj.last_frame-=3;self.assertFalse(fn(obj));obj.last_frame=time.monotonic();obj.warmed=False;self.assertFalse(fn(obj));obj.warmed=True;obj.failure='capture failed';self.assertFalse(fn(obj))
if __name__=='__main__':unittest.main()
