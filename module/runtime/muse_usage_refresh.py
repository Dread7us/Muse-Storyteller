"""Small persistent usage-check queue; no browser, archive, or speech access."""
import contextlib,fcntl,json,os,re,time,uuid
from pathlib import Path
class Refresh:
 def __init__(self,directory='/var/lib/muse-core2-bridge'):
  self.root=Path(directory);self.root.mkdir(parents=True,exist_ok=True);self.path=self.root/'usage-refresh.json'
 @contextlib.contextmanager
 def state(self):
  with (self.root/'usage-refresh.lock').open('a') as lock:
   fcntl.flock(lock,fcntl.LOCK_EX)
   try:
    try:s=json.loads(self.path.read_text())
    except (OSError,ValueError):s={'status':'idle','id':'','last_sync':''}
    before=json.dumps(s,sort_keys=True)
    yield s
    if json.dumps(s,sort_keys=True)==before:return
    tmp=self.path.with_suffix('.tmp');tmp.write_text(json.dumps(s));os.chmod(tmp,0o600);os.replace(tmp,self.path)
   finally:fcntl.flock(lock,fcntl.LOCK_UN)
 def request(self):
  with self.state() as s:
   if s.get('status') not in ('queued','checking') or time.monotonic()-s.get('started',0)>180:
    s.update(status='queued',id=uuid.uuid4().hex,started=time.monotonic(),boot=self.boot(),note='Open Chrome on your PC; connect Module USB.')
  return self.snapshot()
 @staticmethod
 def boot():
  try:return Path('/proc/sys/kernel/random/boot_id').read_text().strip()
  except OSError:return 'test'
 def snapshot(self):
  with self.state() as s:
   if s.get('status') in ('queued','checking') and (s.get('boot')!=self.boot() or time.monotonic()-s.get('started',0)>180):s.update(status='error',note='PC check timed out. Connect Module USB and retry.')
   return {'supported':True,'id':s.get('id',''),'status':s.get('status','idle'),'pending':s.get('status') in ('queued','checking'),'note':s.get('note','Ready for a website check.'),'last_sync':s.get('last_sync','')}
 def update(self,identity,payload):
  if not re.fullmatch('[0-9a-f]{32}',identity):raise ValueError('Invalid check identity')
  with self.state() as s:
   if s.get('id')!=identity or s.get('status') not in ('queued','checking'):raise ValueError('Check is no longer pending')
   action=payload.get('status')
   if action=='checking':s.update(status='checking',note='Checking Muse in Chrome...')
   elif action=='done':
    checked=payload.get('checked','')
    if not re.fullmatch(r'[A-Za-z]{3} [0-9]{1,2} [0-9]{2}:[0-9]{2}',checked):raise ValueError('Invalid sync date')
    from muse_usage import Usage
    actual=Usage().snapshot()
    if not actual or actual['checked']!=checked or actual['observed']!=payload.get('used') or actual['reset']!=payload.get('reset'):raise ValueError('Calibration was not applied')
    label=payload.get('synced',checked)
    if label!=checked and not re.fullmatch(r'[A-Za-z]{3} [0-9]{1,2}, 20[0-9]{2} [0-9]{2}:[0-9]{2}',label):raise ValueError('Invalid sync label')
    s.update(status='done',last_sync=label,note='Website checked and device updated.')
   elif action=='error':
    notes={'login':'Sign in to Muse in Chrome, then retry.','offline':'Connect Module USB and open Chrome, then retry.','page':'Muse usage card could not be read. Try again.','transfer':'Website checked; USB sync failed. Try again.'}
    s.update(status='error',note=notes.get(payload.get('reason'),'Check failed; previous calibration retained.'))
   else:raise ValueError('Invalid check status')
  return self.snapshot()
 def synced(self,payload):
  from muse_usage import Usage
  actual=Usage().snapshot();checked=payload.get('checked','');label=payload.get('synced','')
  if not actual or actual['checked']!=checked or actual['observed']!=payload.get('used') or actual['reset']!=payload.get('reset'):raise ValueError('Sync was not applied')
  if not isinstance(label,str) or not re.fullmatch(r'[A-Za-z]{3} [0-9]{1,2}, 20[0-9]{2} [0-9]{2}:[0-9]{2}',label):raise ValueError('Invalid sync date')
  with self.state() as s:
   s['last_sync']=label
   if s.get('status') not in ('queued','checking'):s.update(status='done',note='Device synced from a website check.')
  return self.snapshot()

if __name__=='__main__':
 import sys
 q=Refresh()
 if sys.argv[1]=='status':result=q.snapshot()
 elif sys.argv[1]=='request':result=q.request()
 elif sys.argv[1]=='sync':result=q.synced(json.loads(Path(sys.argv[2]).read_text()))
 elif sys.argv[1]=='update':result=q.update(sys.argv[2],json.loads(Path(sys.argv[3]).read_text()))
 else:raise SystemExit('Unsupported operation')
 print(json.dumps(result))
