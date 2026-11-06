"""Restricted local queue for device-posted speech scripts, without chat access."""
import argparse,base64,hashlib,json,os,re,subprocess,sys,time
from pathlib import Path

VOICES={'avocado_v2:MAI_01','avocado_v2:MAI_03'}

def accept(root,path):
    """Admit only a bounded, explicitly addressed and hash-checked device job."""
    name=path.stem
    if path.is_symlink() or not re.fullmatch('[0-9a-f]{24}',name) or path.stat().st_size>12000:raise ValueError('Invalid queue request')
    request=json.loads(path.read_text())
    if request.get('version')!=1 or request.get('job')!=name or request.get('voice') not in VOICES or request.get('speed') not in ('slow','normal','fast'):raise ValueError('Invalid queue configuration')
    raw=base64.b64decode(request.get('source_base64',''),validate=True)
    if not 1<=len(raw)<=8192 or b'\0' in raw or hashlib.sha256(raw).hexdigest()!=request.get('source_sha256'):raise ValueError('Queue script integrity failed')
    raw.decode('utf8')
    folder=root/'replies'/name
    if folder.is_symlink():raise ValueError('Invalid job directory')
    folder.mkdir(mode=0o700,parents=True,exist_ok=True)
    source=folder/'source.md'
    if source.exists():
        if source.is_symlink() or source.read_bytes()!=raw:raise ValueError('Queue identity changed')
    else:
        with source.open('xb') as output:output.write(raw)
        source.chmod(0o600)
    return folder,request

def run(root):
    import fcntl
    inbox=root/'inbox';inbox.mkdir(mode=0o700,parents=True,exist_ok=True)
    lock=(inbox/'worker.lock').open('a');fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
    active={};stamp=0
    while True:
        for key,process in list(active.items()):
            if process.poll() is not None:active.pop(key)
        if time.monotonic()>stamp:
            report=dict(version=1,ready=True,pid=os.getpid(),updated_unix=round(time.time()),active=len(active))
            temporary=inbox/'status.pending';temporary.write_text(json.dumps(report));temporary.replace(inbox/'status.json');stamp=time.monotonic()+2
        for path in sorted(inbox.glob('*.json')):
            if path.name=='status.json' or len(active)>=2:continue
            try:
                folder,request=accept(root,path)
                if not (folder/'claimed').exists():
                    output=(folder/'stdout.log').open('ab');error=(folder/'stderr.log').open('ab')
                    try:
                        process=subprocess.Popen([str(root/'voice-env/bin/python3'),str(root/'tools/muse_vm_tts_job.py'),request['job'],'--voice',request['voice'],'--speed',request['speed'],'--compact','--sections'],stdin=subprocess.DEVNULL,stdout=output,stderr=error,start_new_session=True)
                    finally:output.close();error.close()
                    active[request['job']]=process
                path.rename(path.with_suffix('.accepted'))
            except Exception as exc:
                # Reject this one request; never inspect any other directory.
                failure=dict(version=1,error=type(exc).__name__)
                path.with_suffix('.error').write_text(json.dumps(failure));path.rename(path.with_suffix('.rejected'))
        time.sleep(.1)

def install(root):
    """Prefer an existing user service manager; retain unrelated startup entries."""
    root=root.resolve();python=root/'voice-env/bin/python3';script=root/'tools/muse_vm_queue.py'
    command=[str(python),str(script),'--root',str(root)]
    startup='manual'
    if subprocess.run(['systemctl','--user','show-environment'],capture_output=True).returncode==0:
        import shlex
        directory=Path.home()/'.config/systemd/user';directory.mkdir(parents=True,exist_ok=True)
        unit='[Unit]\nDescription=Muse Storyteller narration queue\n[Service]\nExecStart='+shlex.join(command)+'\nRestart=on-failure\nRestartSec=2\n[Install]\nWantedBy=default.target\n'
        (directory/'muse-storyteller-narration.service').write_text(unit)
        subprocess.run(['systemctl','--user','daemon-reload'],check=True)
        subprocess.run(['systemctl','--user','enable','--now','muse-storyteller-narration.service'],check=True);startup='user-service'
    else:
        import shutil,shlex
        if shutil.which('crontab'):
            old=subprocess.run(['crontab','-l'],capture_output=True,text=True)
            if old.returncode not in (0,1):raise RuntimeError('Cannot preserve existing startup entries')
            entries=[line for line in old.stdout.splitlines() if '# muse-storyteller-narration' not in line]
            entries.append('* * * * * '+shlex.join(command)+' >/dev/null 2>&1 # muse-storyteller-narration')
            subprocess.run(['crontab','-'],input='\n'.join(entries)+'\n',text=True,check=True);startup='user-cron'
        subprocess.Popen(command,stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,start_new_session=True)
    until=time.monotonic()+8
    status=root/'inbox/status.json'
    while time.monotonic()<until:
        if status.exists():
            result=json.loads(status.read_text())
            if result.get('ready') and abs(time.time()-result['updated_unix'])<5:
                print(json.dumps(dict(result,startup=startup)),flush=True);return
        time.sleep(.1)
    raise RuntimeError('Narration queue did not become ready')

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--root',type=Path,default=Path.home()/'muse-storyteller');parser.add_argument('--install',action='store_true');args=parser.parse_args()
    if args.install:install(args.root)
    else:run(args.root)
