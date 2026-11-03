"""List pinned third-party assets; download only after an explicit choice."""
import argparse
import hashlib
import json
from pathlib import Path
import urllib.request

ROOT=Path(__file__).resolve().parents[1]

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--download',action='store_true',help='Download after reviewing docs/LICENSING.md and upstream model terms')
    args=p.parse_args(); spec=json.loads((ROOT/'module/assets.json').read_text())
    print(spec['notice'])
    for asset in spec['assets']:
        print(asset['name'],asset['bytes'],'bytes',asset['url'])
        if not args.download: continue
        target=ROOT/'assets-downloads'/Path(asset['url']).name
        target.parent.mkdir(exist_ok=True)
        if target.exists():
            if hashlib.sha256(target.read_bytes()).hexdigest()==asset['sha256']: continue
            raise SystemExit('Existing asset differs; retained for inspection: '+target.name)
        partial=target.with_name(target.name+'.part')
        if partial.exists(): raise SystemExit('Previous partial download exists; inspect it first: '+partial.name)
        with urllib.request.urlopen(asset['url'],timeout=60) as response, partial.open('xb') as f:
            while chunk:=response.read(1024*1024): f.write(chunk)
        if partial.stat().st_size!=asset['bytes'] or hashlib.sha256(partial.read_bytes()).hexdigest()!=asset['sha256']: raise SystemExit('Asset verification failed; partial retained: '+partial.name)
        partial.rename(target)
    if not args.download: print('Preview only. No files downloaded, packages installed or services changed.')

if __name__=='__main__': main()
