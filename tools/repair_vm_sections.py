"""Recheck an interrupted/failed job using its existing audio, never new synthesis."""
import argparse,hashlib,json,re,shutil,subprocess,time
from pathlib import Path
from muse_vm_tts_job import atomic_json,read_source
from muse_vm_sections import plan_sections
from muse_vm_speech_check import SpeechCheck
from muse_speech_text import SpeechText

def repair(folder,checker=None):
    folder=Path(folder)
    if folder.is_symlink() or not re.fullmatch('[0-9a-f]{24}',folder.name):raise ValueError('Invalid failed job')
    raw,text=read_source(folder);spoken=SpeechText(text);parts=plan_sections(spoken)
    meta=json.loads((folder/'meta.json').read_text());plan=json.loads((folder/'plan.json').read_text())
    signature=hashlib.sha256(raw).hexdigest()
    if meta.get('version')!=2 or meta.get('state')!='error' or meta.get('source_sha256')!=signature or plan.get('source_sha256')!=signature or plan.get('sections')!=parts:raise ValueError('Failed job identity changed')
    # Check complete cache hashes before changing any metadata.
    for part in parts:
        i=part['index'];record=json.loads((folder/('part-%03d.json'%i)).read_text())
        if record.get('state')=='complete':
            data=(folder/('part-%03d.mp3'%i)).read_bytes()
            if record.get('audio_sha256')!=hashlib.sha256(data).hexdigest() or record.get('audio_bytes')!=len(data):raise ValueError('Existing speech hash changed')
    backup=folder/('before-realignment-'+str(time.time_ns()));backup.mkdir(mode=0o700)
    for name in ['meta.json']+['part-%03d.json'%p['index'] for p in parts]:shutil.copy2(folder/name,backup/name)
    checker=checker or SpeechCheck(Path.home()/'muse-storyteller/models/tiny.en')
    joined=bytearray();checked=[];deadline=time.monotonic()+180
    for part in parts:
        if time.monotonic()>deadline:raise RuntimeError('Speech recheck timed out')
        i=part['index'];delivery=folder/('part-%03d.mp3'%i)
        record=json.loads((folder/('part-%03d.json'%i)).read_text())
        if record.get('state')!='complete':
            original=folder/('part-%03d-original.mp3'%i)
            if original.is_symlink() or not 128<=original.stat().st_size<=2*1024*1024:raise ValueError('Missing original section audio')
            temporary=folder/('part-%03d.recheck'%i)
            subprocess.run(['ffmpeg','-nostdin','-v','error','-i',str(original),'-map','0:a:0','-vn','-ac','1','-ar','16000','-c:a','libmp3lame','-b:a','32k','-write_xing','0','-id3v2_version','0','-f','mp3','-y',str(temporary)],check=True,timeout=15)
            audio=temporary
        else:audio=delivery
        data=audio.read_bytes();duration=len(data)/4000.
        if len(data)%144 or not .2<duration<=29:raise ValueError('Invalid bounded section audio')
        pcm=subprocess.run(['ffmpeg','-nostdin','-v','error','-i',str(audio),'-f','s16le','-ac','1','-ar','16000','pipe:1'],capture_output=True,check=True,timeout=15).stdout
        if len(pcm)!=len(data)//144*1152:raise ValueError('Decoded audio length changed')
        a=part['word_start'];b=a+part['word_count'];phrase=spoken.text[spoken.words[a][0]:spoken.words[b-1][1]]
        result=checker.check(pcm,phrase,a)
        if audio!=delivery:audio.replace(delivery)
        updated=dict(part,version=2,state='complete',audio_bytes=len(data),audio_sha256=hashlib.sha256(data).hexdigest(),duration_seconds=duration,rechecked=True,**result)
        atomic_json(folder/('part-%03d.json'%i),updated);joined.extend(data)
        checked.append(dict(index=i,agreement=result['agreement'],padding=result['alignment_padding'],filter=result['alignment_filter']))
    temporary=folder/'audio.rechecked';temporary.write_bytes(joined);temporary.replace(folder/'audio.mp3')
    meta.update(state='complete',ready_sections=len(parts),audio_bytes=len(joined),audio_sha256=hashlib.sha256(joined).hexdigest(),rechecked=True);meta.pop('error',None)
    atomic_json(folder/'meta.json',meta)
    return dict(complete=True,sections=len(parts),generated_new_audio=False,audio_bytes=len(joined),checks=checked)

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('job');args=parser.parse_args()
    if not re.fullmatch('[0-9a-f]{24}',args.job):raise ValueError('Invalid job key')
    print(json.dumps(repair(Path.home()/'muse-storyteller/replies'/args.job)),flush=True)
