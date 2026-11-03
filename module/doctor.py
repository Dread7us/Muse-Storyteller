"""Read-only readiness report. Run on the Module LLM, not the Core2."""
import ctypes.util
import json
from pathlib import Path
import platform
import shutil

def main():
    required=['/opt/muse-core2-bridge/piper/piper','/opt/muse-core2-bridge/en_US-lessac-low.onnx','/opt/muse-core2-bridge/en_US-lessac-low.onnx.json','/opt/muse-whisper-base/bin/llm_whisper-1.8','/opt/muse-whisper-base/audio_rpc_shim.so','/opt/muse-whisper-base/mode_whisper-base.json','/opt/m5stack/data/whisper-base']
    report={'machine':platform.machine(),'libzmq_available':bool(ctypes.util.find_library('zmq')),'free_bytes':shutil.disk_usage('/').free,'resources':{p:Path(p).exists() for p in required},'mutations':False,'audio_captured':False,'account_accessed':False}
    print(json.dumps(report,indent=2))
    return 0 if report['machine'] in ('aarch64','arm64') and report['libzmq_available'] and all(report['resources'].values()) else 1

if __name__=='__main__': raise SystemExit(main())

