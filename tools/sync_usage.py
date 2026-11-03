"""Apply a real website observation through an already-running ADB server.

Only the Module is targeted. No server, driver, voice or account operations.
"""
import argparse
import json
from pathlib import Path
import socket
import subprocess

ROOT=Path(__file__).resolve().parents[1]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('observation',type=Path)
    args=parser.parse_args()
    config=json.loads((ROOT/'private/usage-host.json').read_text())
    observation=json.loads(args.observation.read_text())
    with socket.create_connection(('127.0.0.1',5037),timeout=2): pass
    adb=config['adb']
    def call(*parts):
        result=subprocess.run([adb,'-s','axera-ax620e',*parts],capture_output=True,text=True,encoding='utf8',timeout=20,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        if result.returncode: raise RuntimeError('Module transfer unavailable')
        return result.stdout
    call('push',str(args.observation),'/tmp/storyteller-usage-observation.json')
    result=json.loads(call('shell','python3 -B /opt/muse-core2-bridge/muse_usage.py /tmp/storyteller-usage-observation.json'))
    if result.get('observed')!=observation.get('used') or result.get('checked')!=observation.get('checked') or result.get('reset')!=observation.get('reset'):
        raise RuntimeError('Module did not confirm the exact observation')
    print(json.dumps(result))

if __name__=='__main__': main()
