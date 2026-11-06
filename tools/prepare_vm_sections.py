"""One-time isolated setup for free local checks in the existing Muse VM."""
import hashlib,json,os,shutil,subprocess,sys,venv
from pathlib import Path

REVISION='0d3d19a32d3338f10357c0889762bd8d64bbdeba'
CTC_REVISION='a19f851b3d42865797e410752b4c570c871e4825'
CTC_HASHES={'config.json':'ceff8f92e6670ad3633779bd43b3559628c1a64b0868ef6046b23d62b85c69d5',
    'preprocessor_config.json':'8cdfd65ff4115423185a1512bdae100e2e0cd744f5b322417429944aaafd0827',
    'vocab.json':'4178db26b3c7570f6a47f14ac6a1c7b32950b8c2800fb097287e53776934f1c5',
    'onnx/model_quantized.onnx':'cd5040c147381580ed73258143dd8e0c28e800a09e74ee42ee2b3e8cb4d760a3'}

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
        'faster-whisper==1.2.1','ctranslate2==4.8.5','onnxruntime==1.23.2'],check=True,timeout=240)
    model=root/'models'/'tiny.en';model.mkdir(parents=True,exist_ok=True)
    code="from huggingface_hub import snapshot_download;snapshot_download('Systran/faster-whisper-tiny.en',revision="+repr(REVISION)+",local_dir="+repr(str(model))+",allow_patterns=['model.bin','config.json','tokenizer.json','vocabulary.txt'],token=False)"
    # The optional Xet path stalled the reference VM's initial public download.
    # Standard HTTPS resumes the same verified revision/cache without a token.
    env=dict(os.environ,HF_HUB_DISABLE_TELEMETRY='1',HF_HUB_DISABLE_XET='1',
             HF_HUB_DOWNLOAD_TIMEOUT='45',HF_HUB_ETAG_TIMEOUT='15')
    subprocess.run([str(python),'-c',code],check=True,timeout=240,env=env)
    acoustic=root/'models'/'ctc-english';acoustic.mkdir(parents=True,exist_ok=True)
    code="from huggingface_hub import snapshot_download;snapshot_download('Xenova/wav2vec2-base-960h',revision="+repr(CTC_REVISION)+",local_dir="+repr(str(acoustic))+",allow_patterns="+repr(list(CTC_HASHES))+",token=False)"
    subprocess.run([str(python),'-c',code],check=True,timeout=240,env=env)
    for name,expected in CTC_HASHES.items():
        if hashlib.sha256((acoustic/name).read_bytes()).hexdigest()!=expected:raise RuntimeError('Acoustic asset failed its hash: '+name)
    subprocess.run([str(python),'-c',"import sys;sys.path.insert(0,"+repr(str(root/'tools'))+");from muse_vm_speech_check import SpeechCheck;SpeechCheck("+repr(str(model))+")"],check=True,timeout=60)
    report=dict(version=3,ready=True,model_revision=REVISION,alignment='ctc-english',alignment_revision=CTC_REVISION,python=str(python),
        helpers={n:hashlib.sha256((root/'tools'/n).read_bytes()).hexdigest() for n in ['muse_vm_tts_job.py','muse_vm_sections.py','muse_vm_speech_check.py','muse_ctc_alignment.py','muse_speech_text.py']})
    (root/'tools'/'sections-ready.json').write_text(json.dumps(report,indent=2))
    print(json.dumps(report))

if __name__=='__main__':main()
