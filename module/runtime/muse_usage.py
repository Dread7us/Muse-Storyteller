"""Local, explicitly approximate allowance meter; never a billing formula.
Only counters and website observations are persisted, never conversation text.
"""
import contextlib,json,math,os,re,time
from pathlib import Path
try:import fcntl
except ImportError:fcntl=None

class Usage:
 def __init__(self,directory='/var/lib/muse-core2-bridge'):
  self.directory=Path(directory);self.directory.mkdir(parents=True,exist_ok=True)
  self.path=self.directory/'usage-estimate.json'
 def boot(self):
  try:return Path('/proc/sys/kernel/random/boot_id').read_text().strip()
  except OSError:return None
 @contextlib.contextmanager
 def state(self):
  with (self.directory/'usage-estimate.lock').open('a+') as lock:
   if fcntl:fcntl.flock(lock,fcntl.LOCK_EX)
   try:
    if self.path.exists():s=json.loads(self.path.read_text(encoding='utf8'))
    else:s={'v':1,'chars':0,'last_seq':None,'anchor':None,'rate':None}
    yield s
    tmp=self.path.with_suffix('.tmp')
    with tmp.open('w',encoding='utf8') as f:
     if os.name=='posix':os.fchmod(f.fileno(),0o600)
     json.dump(s,f);f.flush();os.fsync(f.fileno())
    os.replace(tmp,self.path)
   finally:
    if fcntl:fcntl.flock(lock,fcntl.LOCK_UN)
 def sync(self,db):
  with self.state() as s:
   last=db.execute('SELECT coalesce(max(seq),0) FROM messages').fetchone()[0]
   if s['last_seq'] is not None and last>s['last_seq']:
    s['chars']+=int(db.execute("SELECT coalesce(sum(length(m.text)),0) FROM messages m WHERE m.seq>? AND NOT EXISTS(SELECT 1 FROM settings x WHERE x.key='v12_fixture:'||m.chat AND x.value='owned')",(s['last_seq'],)).fetchone()[0])
   s['last_seq']=max(last,s['last_seq'] or 0)
 def calibrate(self,observation):
  used=observation.get('used');reset=observation.get('reset');checked=observation.get('checked')
  if type(used) is not int or not 0<=used<=100:raise ValueError('Usage must be a whole percent from the visible website')
  if not isinstance(reset,str) or not re.fullmatch(r'[A-Za-z]{3,9} [0-9]{1,2}',reset):raise ValueError('Invalid visible reset date')
  if not isinstance(checked,str) or not re.fullmatch(r'[A-Za-z]{3} [0-9]{1,2} [0-9]{2}:[0-9]{2}',checked):raise ValueError('Invalid check label')
  observed_at=observation.get('observed_at',time.time())
  if isinstance(observed_at,bool) or not isinstance(observed_at,(int,float)) or not math.isfinite(observed_at) or not 0<observed_at<4102444800:raise ValueError('Invalid observation time')
  with self.state() as s:
   a=s['anchor'];rate=s['rate'];sample=s.get('sample',a)
   if a and observed_at<=a.get('observed_at',0):
    if observed_at<a['observed_at'] or used!=a['used'] or reset!=a['reset']:raise ValueError('Older or conflicting observation rejected')
   else:
    if a and a['reset']==reset and used>=a['used']:
     delta=s['chars']-sample['chars']
     if used>sample['used'] and delta>0:
      learned=(used-sample['used'])/delta
      rate=learned if rate is None else (rate+learned)/2
      sample={'used':used,'chars':s['chars']}
    else:
     rate=None;sample={'used':used,'chars':s['chars']}
    s['sample']=sample;s['rate']=rate;s['anchor']={'used':used,'reset':reset,'checked':checked,'chars':s['chars'],'received':time.time(),'observed_at':observed_at,'boot':self.boot()}
  return self.snapshot()
 def snapshot(self):
  with self.state() as s:
   a=s['anchor']
   if not a:return None
   delta=max(0,s['chars']-a['chars']);rate=s['rate']
   used=min(100,max(a['used'],int(math.ceil(a['used']+delta*(rate or 0)))))
   age=time.time()-a['received'];age=604800 if age<0 or a.get('boot')!=self.boot() else min(604800,int(age))
   return {'used':used,'observed':a['used'],'reset':a['reset'],'checked':a['checked'],'age':age,'learned':rate is not None,'pending':delta>0 and rate is None}

if __name__=='__main__':
 import sys
 observation=json.loads(Path(sys.argv[1]).read_text(encoding='utf8'))
 print(json.dumps(Usage().calibrate(observation)))
