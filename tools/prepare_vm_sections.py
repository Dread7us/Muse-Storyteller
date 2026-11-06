"""One-time isolated setup for free local checks in the existing Muse VM."""
import hashlib,json,os,shutil,subprocess,sys,venv
from pathlib import Path

REVISION='0d3d19a32d3338f10357c0889762bd8d64bbdeba'

def main():
    root=Path.home()/'muse-storyteller'
    if not shutil.which('ffmpeg'):raise RuntimeError('FFmpeg must be installed first')
    environment=root/'voice-env'
    python=environment/'bin'/'python3'
    if not python.exists():venv.EnvBuilder(with_pip=False).create(environment)
    if subprocess.run([str(python),'-m','pip','--version'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode==0:
        installer=[str(python),'-m','pip','--disable-pip-version-check','install']
    elif shutil.which('uv'):installer=['uv','pip','install','--python',str(python)]
    else:installer=[sys.executable,'-m','pip','--disable-pip-version-check','--python',str(python),'install']
    subprocess.run(installer+['--only-binary=:all:',
        'faster-whisper==1.2.1','ctranslate2==4.8.5'],check=True,timeout=240)
    model=root/'models'/'tiny.en';model.mkdir(parents=True,exist_ok=True)
    code="from huggingface_hub import snapshot_download;snapshot_download('Systran/faster-whisper-tiny.en',revision="+repr(REVISION)+",local_dir="+repr(str(model))+",allow_patterns=['model.bin','config.json','tokenizer.json','vocabulary.txt'],token=False)"
    # The optional Xet path stalled the reference VM's initial public download.
    # Standard HTTPS resumes the same verified revision/cache without a token.
    env=dict(os.environ,HF_HUB_DISABLE_TELEMETRY='1',HF_HUB_DISABLE_XET='1',
             HF_HUB_DOWNLOAD_TIMEOUT='45',HF_HUB_ETAG_TIMEOUT='15')
    subprocess.run([str(python),'-c',code],check=True,timeout=240,env=env)
    subprocess.run([str(python),'-c',"import sys;sys.path.insert(0,"+repr(str(root/'tools'))+");from muse_vm_speech_check import SpeechCheck;SpeechCheck("+repr(str(model))+")"],check=True,timeout=60)
    report=dict(version=2,ready=True,model_revision=REVISION,python=str(python),
        helpers={n:hashlib.sha256((root/'tools'/n).read_bytes()).hexdigest() for n in ['muse_vm_tts_job.py','muse_vm_sections.py','muse_vm_speech_check.py']})
    (root/'tools'/'sections-ready.json').write_text(json.dumps(report,indent=2))
    print(json.dumps(report))

if __name__=='__main__':main()
