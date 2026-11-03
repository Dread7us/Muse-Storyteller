"""Publication guard: report filenames/categories, never print matched secrets."""
import re
import hashlib
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[1]
IGNORE={'.git','.local','private','assets-downloads','__pycache__'}
FORBIDDEN={'.pem','.key','.bin','.elf','.wav','.mp3','.pcm','.db','.sqlite','.sqlite3','.onnx','.deb','.zip','.gz','.exe','.dll','.so'}
RULES={
    'credential-like token':r'\b(?:gh[pousr]_[A-Za-z0-9]{24,}|github_pat_[A-Za-z0-9_]{30,}|mgst_[A-Za-z0-9_-]{12,}|sk-[A-Za-z0-9]{24,})',
    'private key material':r'-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----',
    'personal workstation path':r'(?:[A-Za-z]:[\\/]'+('Us'+'ers')+r'[\\/][^\s"\']+|/'+('Us'+'ers')+r'/[^/\s]+/)',
    'account/contact address':r'[A-Za-z0-9_.+-]+@(?:gmail|hotmail|outlook)\.com',
    'private network password':r'CONFIG_.*(?:WIFI_PASSWORD|WIFI_PASS)\s*=\s*"[^"\n]+"',
}

# Exact publication allowlist: reviewed device UI only, no account or transcript data.
REVIEWED_SCREENSHOTS={
    'docs/assets/screenshots/idle.png': '71bd26e1215db102a7318db67b0008c5c2f7859f94b7a6add00bb8f905ca3b21',
    'docs/assets/screenshots/splash.png': 'befdcbc1c8cd59c5829ce382d5562a95ec0fbbe286bc21a3044debe1f5565286',
    'docs/assets/screenshots/voice-commands.png': '9b4bd853b37e6458861eeb708efa1972efdd1ca42268174ea308f6e48c15c47f',
    'docs/assets/screenshots/sound.png': 'd666a32d665dbf3e54d76965e034cbe533c53fcb19a24134e846ca81ee9ffeca',
}

def paths():
    # Do not traverse generated SDKs, model downloads or private directories.
    def walk(directory):
        for p in directory.iterdir():
            if p.name in IGNORE: continue
            if p.is_dir(): yield from walk(p)
            else: yield p
    return walk(ROOT)

def main():
    problems=[];count=0
    for path in paths():
        count+=1; relative=path.relative_to(ROOT).as_posix()
        if relative in REVIEWED_SCREENSHOTS:
            if hashlib.sha256(path.read_bytes()).hexdigest()!=REVIEWED_SCREENSHOTS[relative]:
                problems.append((relative,'screenshot changed; requires visual privacy review'))
            continue
        if path.suffix.lower() in FORBIDDEN: problems.append((relative,'device data/binary or private key file'));continue
        try:text=path.read_text(encoding='utf-8-sig')
        except UnicodeError:problems.append((relative,'unreviewed binary'));continue
        for category,pattern in RULES.items():
            if re.search(pattern,text):problems.append((relative,category))
    # Ignoring a file does not make an accidentally tracked copy safe.
    result=subprocess.run(['git','ls-files','-z'],cwd=ROOT,capture_output=True)
    if result.returncode==0:
        for name in result.stdout.decode().split('\0'):
            if name and any(part in IGNORE- {'.git'} for part in Path(name).parts):problems.append((name,'tracked private/generated path'))
    if problems:
        for name,reason in problems:print(name+': '+reason)
        raise SystemExit('Publication check failed; inspect locally. No matched values were printed.')
    print('PUBLIC_TREE_CHECK_PASSED: '+str(count)+' reviewed source/documentation files; private and generated directories excluded.')

if __name__=='__main__': main()
