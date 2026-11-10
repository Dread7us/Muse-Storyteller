"""Repair missing sections of a failed job while preserving its checked audio."""
import argparse,hashlib,json,re,shutil,subprocess,time
from pathlib import Path
from muse_vm_tts_job import SPEEDS,atomic_json,read_source
from muse_vm_sections import _render,plan_sections
from muse_vm_speech_check import SpeechCheck
from muse_speech_text import SpeechText

def repair(folder,checker=None,rerender=False):
    folder=Path(folder)
    if folder.is_symlink() or not re.fullmatch('[0-9a-f]{24}',folder.name):raise ValueError('Invalid failed job')
    raw,text=read_source(folder);spoken=SpeechText(text)
    meta=json.loads((folder/'meta.json').read_text());plan=json.loads((folder/'plan.json').read_text())
    signature=hashlib.sha256(raw).hexdigest()
    parts=plan.get('sections')
    if (meta.get('version')!=2 or meta.get('state')!='error' or meta.get('source_sha256')!=signature
            or plan.get('source_sha256')!=signature or parts not in (plan_sections(spoken),plan_sections(spoken,minimum_words=1))):
        raise ValueError('Failed job identity changed')
    voice,speed=meta.get('voice'),meta.get('speed')
    if not re.fullmatch(r'avocado_v2:[A-Za-z0-9_.-]{1,80}',voice or '') or speed not in SPEEDS:
        raise ValueError('Invalid original voice configuration')
    # Check complete cache hashes before changing any metadata.
    for part in parts:
        i=part['index'];record=json.loads((folder/('part-%03d.json'%i)).read_text())
        if record.get('state')=='complete':
            data=(folder/('part-%03d.mp3'%i)).read_bytes()
            if (record.get('audio_sha256')!=hashlib.sha256(data).hexdigest()
                    or record.get('audio_bytes')!=len(data) or record.get('aligned') is not True
                    or any(record.get(key)!=value for key,value in part.items())):
                raise ValueError('Existing checked speech changed')
    backup=folder/('before-realignment-'+str(time.time_ns()));backup.mkdir(mode=0o700)
    for name in ['meta.json']+['part-%03d.json'%p['index'] for p in parts]:shutil.copy2(folder/name,backup/name)
    checker=checker or SpeechCheck(Path.home()/'muse-storyteller/models/tiny.en')
    joined=bytearray();checked=[];generated=[];deadline=time.monotonic()+180
    for part in parts:
        if time.monotonic()>deadline:raise RuntimeError('Speech recheck timed out')
        i=part['index'];delivery=folder/('part-%03d.mp3'%i)
        record=json.loads((folder/('part-%03d.json'%i)).read_text())
        if record.get('state')=='complete':
            joined.extend(delivery.read_bytes())
            continue
        original=folder/('part-%03d-original.mp3'%i)
        temporary=folder/('part-%03d.recheck'%i)
        try:
            if original.is_symlink() or not 128<=original.stat().st_size<=2*1024*1024:raise ValueError('Missing original section audio')
            subprocess.run(['ffmpeg','-nostdin','-v','error','-i',str(original),'-map','0:a:0','-vn','-ac','1','-ar','16000','-c:a','libmp3lame','-b:a','32k','-write_xing','0','-id3v2_version','0','-f','mp3','-y',str(temporary)],check=True,timeout=15)
            data=temporary.read_bytes();duration=len(data)/4000.
            if len(data)%144 or not .2<duration<=29:raise ValueError('Invalid bounded section audio')
            pcm=subprocess.run(['ffmpeg','-nostdin','-v','error','-i',str(temporary),'-f','s16le','-ac','1','-ar','16000','pipe:1'],capture_output=True,check=True,timeout=15).stdout
            if len(pcm)!=len(data)//144*1152:raise ValueError('Decoded audio length changed')
            a=part['word_start'];b=a+part['word_count'];phrase=spoken.text[spoken.words[a][0]:spoken.words[b-1][1]]
            result=checker.check(pcm,phrase,a)
            temporary.replace(delivery)
            updated=dict(part,version=2,state='complete',audio_bytes=len(data),audio_sha256=hashlib.sha256(data).hexdigest(),duration_seconds=duration,rechecked=True,**result)
            atomic_json(folder/('part-%03d.json'%i),updated)
        except (OSError,ValueError,RuntimeError,subprocess.SubprocessError) as exc:
            if not rerender:raise RuntimeError('Recheck failed at section %d'%i) from exc
            if original.is_file() and not original.is_symlink():shutil.copy2(original,backup/original.name)
            updated=_render(folder,part,spoken,voice,speed,'/opt/hatch/bin/tts','ffmpeg',deadline,checker)
            data=delivery.read_bytes();generated.append(i)
        joined.extend(data)
        checked.append(dict(index=i,agreement=updated['agreement'],padding=updated['alignment_padding'],filter=updated['alignment_filter']))
    temporary=folder/'audio.rechecked';temporary.write_bytes(joined);temporary.replace(folder/'audio.mp3')
    meta.update(state='complete',ready_sections=len(parts),audio_bytes=len(joined),audio_sha256=hashlib.sha256(joined).hexdigest(),rechecked=True);meta.pop('error',None)
    atomic_json(folder/'meta.json',meta)
    return dict(complete=True,sections=len(parts),generated_new_audio=bool(generated),rerendered=generated,audio_bytes=len(joined),checks=checked)

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('job');parser.add_argument('--rerender',action='store_true');args=parser.parse_args()
    if not re.fullmatch('[0-9a-f]{24}',args.job):raise ValueError('Invalid job key')
    print(json.dumps(repair(Path.home()/'muse-storyteller/replies'/args.job,rerender=args.rerender)),flush=True)
