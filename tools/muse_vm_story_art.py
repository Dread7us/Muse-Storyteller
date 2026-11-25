"""One native Muse image job for an illustrated story; no listener or credentials.

Run beside muse_vm_tts_job.py in the Muse VM. Speech is a separate job and is
never waited on, cancelled or reconfigured here. Failure leaves local graphics.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import time

MAX_SOURCE = 8192
MAX_JPEG = 32768
NAMES = ('cover.jpg', 'scene-1.jpg', 'scene-2.jpg', 'scene-3.jpg')

def atomic_json(path, value):
    temporary = path.with_suffix('.pending')
    with temporary.open('w', encoding='utf8') as output:
        os.chmod(temporary, 0o600)
        json.dump(value, output, ensure_ascii=False, separators=(',', ':'))
        output.flush()
        os.fsync(output.fileno())
    temporary.replace(path)

def terminate(process):
    if process.poll() is not None:
        return
    os.killpg(process.pid, signal.SIGTERM)
    try:process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.wait(timeout=3)

def native_image(directory, query, timeout=150, executable='/opt/hatch/bin/media-generation'):
    """Use the already installed, account-native media tool. Never add a provider."""
    args=[executable,'--media-subagent-output-type','image',
          '--image-output-format','jpeg','--orientation','landscape',
          '--output-dir',str(directory),'--timeout-secs',str(timeout),
          '--conversation-json',json.dumps([{'text':query}],ensure_ascii=False)]
    with (directory.parent/'generation-private.log').open('wb') as log:
        os.chmod(log.name,0o600)
        process=subprocess.Popen(args,stdin=subprocess.DEVNULL,stdout=log,
                                 stderr=subprocess.STDOUT,start_new_session=True)
        try:code=process.wait(timeout=timeout+10)
        except BaseException:
            terminate(process)
            raise
    if code:raise RuntimeError('native_generation_failed')
    candidates=[]
    for path in directory.rglob('*'):
        if len(candidates)>64:raise ValueError('too_many_outputs')
        if path.is_symlink():raise ValueError('unsafe_output')
        if path.is_file() and path.suffix.lower() in ('.jpg','.jpeg','.png','.webp'):
            if not 128<=path.stat().st_size<=20*1024*1024:raise ValueError('invalid_image_size')
            candidates.append(path)
    if not candidates:raise RuntimeError('native_image_missing')
    return max(candidates,key=lambda p:p.stat().st_mtime_ns)

def thumbnail(source, output, index, ffmpeg='ffmpeg'):
    # One 2x2 image job keeps character/style consistency and uses one native
    # generation rather than four. All output JPEGs are baseline, bounded views.
    x,y=index%2,index//2
    crop='crop=iw/2:ih/2:%s:%s,scale=320:180:force_original_aspect_ratio=increase,crop=320:180'%(('iw/2' if x else '0'),('ih/2' if y else '0'))
    temporary=output.with_suffix('.pending.jpg')
    for quality in (6,12,19):
        subprocess.run([ffmpeg,'-hide_banner','-loglevel','error','-nostdin','-y',
                        '-i',str(source),'-vf',crop,'-frames:v','1','-q:v',str(quality),
                        '-pix_fmt','yuvj420p',str(temporary)],stdin=subprocess.DEVNULL,
                       stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=30,check=True)
        if 128<=temporary.stat().st_size<=MAX_JPEG:break
    else:raise ValueError('thumbnail_too_large')
    with temporary.open('rb') as stream:
        data=stream.read(MAX_JPEG+1)
    if not data.startswith(b'\xff\xd8') or len(data)>MAX_JPEG:raise ValueError('invalid_thumbnail')
    os.chmod(temporary,0o600);temporary.replace(output)
    return dict(name=output.name,bytes=len(data),sha256=hashlib.sha256(data).hexdigest(),width=320,height=180)

def prompt(text):
    paragraphs=[p.strip() for p in re.split(r'\n\s*\n',text) if p.strip()]
    picks=[paragraphs[min(len(paragraphs)-1,i*(len(paragraphs)-1)//3)][:420] for i in range(4)]
    return ('Create ONE polished 2 by 2 contact sheet with exactly four equally sized '
            'illustrated storybook panels, no gutters, no lettering, no logos. '
            'Use a consistent cast, clothing, palette and beautiful painterly style '
            'across all panels. Top left is a striking book-cover illustration; '
            'top right, bottom left and bottom right illustrate the beginning, '
            'middle and ending of the following story. Respect the actual setting '
            'and genre; do not invent unrelated scenes. Use rich colors and clear '
            'shapes that read well on a small screen. Story excerpts:\n'+
            '\n'.join('%d. %s'%(i+1,p) for i,p in enumerate(picks)))

def generate(job, runner=native_image, render=thumbnail):
    job=Path(job)
    if job.is_symlink() or not job.is_dir():raise ValueError('invalid_job')
    source=job/'source.md'
    if source.is_symlink():raise ValueError('invalid_source')
    with source.open('rb') as stream:raw=stream.read(MAX_SOURCE+1)
    if not raw or len(raw)>MAX_SOURCE or b'\0' in raw:raise ValueError('invalid_source')
    text=raw.decode('utf8')
    if not text.strip():raise ValueError('invalid_source')
    digest=hashlib.sha256(raw).hexdigest()
    art=job/'art'
    if art.is_symlink():raise ValueError('unsafe_art_directory')
    art.mkdir(mode=0o700,exist_ok=True)
    manifest=art/'art.json'
    try:fd=os.open(art/'claimed',os.O_CREAT|os.O_EXCL|os.O_WRONLY,0o600)
    except FileExistsError:
        if manifest.exists():
            old=json.loads(manifest.read_text())
            if old.get('source_sha256')==digest:return old
        raise ValueError('art_job_already_claimed')
    os.close(fd)
    state=dict(version=1,state='generating',source_sha256=digest,source_bytes=len(raw),files=[])
    atomic_json(manifest,state);started=time.monotonic()
    try:
        native=art/'native'
        native.mkdir(mode=0o700,exist_ok=True)
        image=runner(native,prompt(text))
        if not image.resolve().is_relative_to(native.resolve()) or image.is_symlink():raise ValueError('unsafe_output')
        state['state']='preparing';atomic_json(manifest,state)
        files=[render(image,art/name,i) for i,name in enumerate(NAMES)]
        if source.read_bytes()!=raw:raise ValueError('source_changed')
        state.update(state='complete',files=files,method='native_muse',elapsed_ms=int((time.monotonic()-started)*1000))
    except Exception as error:
        # Keep provider details in the private native log, not in device captions.
        kind='timeout' if isinstance(error,(TimeoutError,subprocess.TimeoutExpired)) else 'generation_failed'
        state.update(state='error',error=kind)
    atomic_json(manifest,state)
    return state

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('job',help='Existing 24-hex narration job identifier')
    args=parser.parse_args()
    if not re.fullmatch('[0-9a-f]{24}',args.job):parser.error('invalid job identifier')
    os.umask(0o077)
    result=generate(Path.home()/'muse-storyteller'/'replies'/args.job)
    print(json.dumps(dict(state=result['state'],files=len(result['files']))))
if __name__=='__main__':main()
