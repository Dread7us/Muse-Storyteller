"""Chrome native host. Reads only the Module check queue and writes usage metadata."""
import json,os,re,socket,struct,subprocess,sys,threading,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
# Set by the packaging script to this authorized workspace, not by browser messages.
CONFIG=json.loads((ROOT/'private/usage-host.json').read_text())
WORK=ROOT/'private/usage';WORK.mkdir(parents=True,exist_ok=True);ADB=CONFIG['adb'];PYTHON=CONFIG['python'];SCRIPT='/opt/muse-core2-bridge/muse_usage_refresh.py'
LOCK=threading.Lock();STOP=threading.Event();SEEN=set()
def send(value):
 data=json.dumps(value,separators=(',',':')).encode()
 with LOCK:sys.stdout.buffer.write(struct.pack('<I',len(data))+data);sys.stdout.buffer.flush()
def adb(*args):
 with socket.create_connection(('127.0.0.1',5037),timeout=2):pass
 p=subprocess.run([ADB,'-s','axera-ax620e',*args],capture_output=True,text=True,encoding='utf8',timeout=15,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
 if p.returncode:raise OSError('Module is unavailable')
 return p.stdout

def remote_status():
 value=json.loads(adb('shell','python3 -B '+SCRIPT+' status'))
 if not value.get('supported') or not re.fullmatch('[0-9a-f]{32}',value.get('id','')):return None
 return value

def remote_update(identity,value):
 if not re.fullmatch('[0-9a-f]{32}',identity):raise ValueError('Invalid identity')
 p=WORK/'usage-refresh-result.json';p.write_text(json.dumps(value),encoding='utf8');adb('push',str(p),'/tmp/muse-refresh-result.json')
 answer=json.loads(adb('shell','python3 -B '+SCRIPT+' update '+identity+' /tmp/muse-refresh-result.json'))
 if answer.get('id')!=identity or answer.get('status')!=value['status']:raise ValueError('Module did not acknowledge check status')
 return answer

def timestamp():
 code="$d=[DateTimeOffset]::UtcNow;$p=[TimeZoneInfo]::ConvertTime($d,[TimeZoneInfo]::FindSystemTimeZoneById('Pacific Standard Time'));@{observed_at=$d.ToUnixTimeSeconds();checked=$p.ToString('MMM d HH:mm',[Globalization.CultureInfo]::InvariantCulture);synced=$p.ToString('MMM d, yyyy HH:mm',[Globalization.CultureInfo]::InvariantCulture)}|ConvertTo-Json -Compress"
 p=subprocess.run(['powershell.exe','-NoProfile','-NonInteractive','-Command',code],capture_output=True,text=True,check=True,timeout=15,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0));return json.loads(p.stdout)

def observe(identity,used,reset):
 if type(used) is not int or not 0<=used<=100 or not isinstance(reset,str) or not re.fullmatch('[A-Za-z]{3,9} [0-9]{1,2}',reset):raise ValueError('Invalid website reading')
 current=remote_status()
 if not current or current['id']!=identity or current['status'] not in ('queued','checking'):raise ValueError('Expired request')
 stamp=timestamp();observation=dict(observed_at=stamp['observed_at'],checked=stamp['checked'],used=used,reset=reset);p=WORK/'usage-latest-observation.json';p.write_text(json.dumps(observation,indent=2),encoding='utf8')
 state_path=WORK/'usage-calibration-status.json'
 try:state=json.loads(state_path.read_text())
 except (OSError,ValueError):state={}
 state.update(last_web_epoch=observation['observed_at'],used=used,reset=reset,pending=True);state_path.write_text(json.dumps(state,indent=2),encoding='utf8')
 # Use the established synchronization helper, never launch or restart ADB.
 with socket.create_connection(('127.0.0.1',5037),timeout=2):pass
 result=subprocess.run([PYTHON,'-I','-X','utf8','-B',str(ROOT/'tools/sync_usage.py'),str(p)],capture_output=True,text=True,timeout=30,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
 if result.returncode:raise OSError('Usage transfer failed')
 # Older ADB ignores remote exit codes: require the actual JSON observation.
 answers=[]
 for line in result.stdout.splitlines():
  try:item=json.loads(line)
  except ValueError:continue
  if isinstance(item,dict):answers.append(item)
 if not any(a.get('observed')==used and a.get('reset')==reset and a.get('checked')==observation['checked'] for a in answers):raise OSError('Usage transfer did not verify')
 finished=timestamp();completed=remote_update(identity,dict(status='done',used=used,reset=reset,checked=observation['checked'],synced=finished.get('synced',finished['checked'])))
 state.update(last_transfer_epoch=int(time.time()),pending=False,failure_since=None,failure_reason=None);state_path.write_text(json.dumps(state,indent=2),encoding='utf8')
 return completed

def poll():
 while not STOP.wait(5):
  try:
   job=remote_status()
   if job and job['status'] in ('queued','checking') and job['id'] not in SEEN:
    remote_update(job['id'],{'status':'checking'});SEEN.add(job['id']);send({'type':'check','id':job['id']})
  except (OSError,ValueError,subprocess.SubprocessError):pass

def main():
 if len(sys.argv)<2 or sys.argv[1]!=CONFIG['origin']:return 4
 if os.name=='nt':
  import msvcrt
  msvcrt.setmode(sys.stdin.fileno(),os.O_BINARY);msvcrt.setmode(sys.stdout.fileno(),os.O_BINARY)
 threading.Thread(target=poll,daemon=True).start()
 try:
  while True:
   header=sys.stdin.buffer.read(4)
   if not header:break
   if len(header)!=4:raise ValueError('Incomplete frame')
   size=struct.unpack('<I',header)[0]
   if not 0<size<=4096:raise ValueError('Invalid frame size')
   raw=sys.stdin.buffer.read(size)
   if len(raw)!=size:raise ValueError('Incomplete message')
   msg=json.loads(raw);identity=msg.get('id','')
   if not re.fullmatch('[0-9a-f]{32}',identity):continue
   try:
    if msg.get('type')=='observation':observe(identity,msg.get('used'),msg.get('reset'))
    elif msg.get('type')=='error':
     if msg.get('stage') in ('menu-button','menu-open','settings-item','usage-card'):(WORK/'usage-helper-diagnostic.json').write_text(json.dumps({'id':identity,'stage':msg['stage'],'time':int(time.time())}))
     remote_update(identity,{'status':'error','reason':msg.get('reason') if msg.get('reason') in ('login','page') else 'page'})
   except (OSError,ValueError,subprocess.SubprocessError):
    try:remote_update(identity,{'status':'error','reason':'transfer'})
    except (OSError,ValueError,subprocess.SubprocessError):pass
 finally:STOP.set()
 return 0
if __name__=='__main__':raise SystemExit(main())
