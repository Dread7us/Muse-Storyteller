"""Create an isolated, pinned SDK checkout and apply reviewed source files.

Never changes a connected device or resets an existing checkout.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else None

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', help='Optional local upstream Git checkout; uncommitted files are not copied')
    args = parser.parse_args()
    manifest = json.loads((ROOT/'firmware/upstream.json').read_text())
    sdk = ROOT/'.local/sdk'
    marker = sdk/'.storyteller-overlay.json'
    if sdk.exists():
        if marker.exists():
            previous = json.loads(marker.read_text())
            if previous.get('commit') == manifest['commit'] and all(digest(sdk/name) == item['overlay_sha256'] for name,item in manifest['files'].items()):
                print('Prepared SDK already matches this edition.'); return
        raise SystemExit('Existing .local/sdk differs. Preserve your changes and choose a new checkout; nothing was overwritten.')
    sdk.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(['git','clone','--no-hardlinks','--no-checkout',args.source or manifest['repository'],str(sdk)], check=True)
    subprocess.run(['git','-C',str(sdk),'config','core.autocrlf','false'], check=True)
    subprocess.run(['git','-C',str(sdk),'config','core.eol','lf'], check=True)
    subprocess.run(['git','-C',str(sdk),'checkout','--detach',manifest['commit']], check=True)
    for name,item in manifest['files'].items():
        if digest(sdk/name) != item['base_sha256']:
            raise SystemExit('Upstream file differs: '+name+'. No overlay files were copied.')
        if digest(ROOT/'firmware/overlay'/name) != item['overlay_sha256']:
            raise SystemExit('Overlay integrity mismatch: '+name)
    for name in manifest['files']:
        target=sdk/name; target.parent.mkdir(parents=True,exist_ok=True)
        shutil.copyfile(ROOT/'firmware/overlay'/name,target)
    marker.write_text(json.dumps({'commit':manifest['commit']},indent=2))
    print('Prepared pinned firmware source in .local/sdk; no hardware was changed.')

if __name__ == '__main__': main()
