"""Bounded, independently verifiable Muse narration sections."""
import hashlib,json,os,re,subprocess,time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from muse_vm_tts_job import atomic_json,read_source,SPEEDS,MAX_AUDIO,terminate

def plan_sections(spoken,target=190,maximum_words=36):
    """Keep sentence boundaries; mark pieces of unusually long sentences."""
    words=spoken.words
    if not words:raise ValueError('Narration needs spoken words')
    result=[];start=0;sentence=0
    while start<len(words):
        stop=start+1
        while stop<len(words) and not re.search(r'[.!?]["”’\']?$',words[stop-1][4]):
            stop+=1
        # A sentence is prepared completely before any of its pieces play.
        at=start
        while at<stop:
            end=min(stop,at+maximum_words)
            while end>at+1 and words[end-1][1]-words[at][0]>target:end-=1
            result.append(dict(index=len(result),word_start=at,word_count=end-at,
                               sentence=sentence,sentence_end=end==stop,
                               spoken_sha256=hashlib.sha256(spoken.text[words[at][0]:words[end-1][1]].encode()).hexdigest()))
            at=end
        start=stop;sentence+=1
    if len(result)>128:raise ValueError('Narration needs too many sections')
    return result

def _render(folder,part,spoken,voice,speed,executable,ffmpeg,deadline,checker):
    i=part['index'];words=spoken.words;at=part['word_start'];end=at+part['word_count']
    text=spoken.text[words[at][0]:words[end-1][1]]
    original=folder/('part-%03d-original.mp3'%i)
    delivery=folder/('part-%03d.mp3'%i)
    temporary=delivery.with_suffix('.pending')
    last=None
    for attempt in range(2):
        process=None
        try:
            remaining=min(45,max(.1,deadline-time.monotonic()))
            with original.open('wb') as output:
                process=subprocess.Popen([str(executable),'speak','--text',text,'--voice',voice,
                    '--language','en','--speed',str(SPEEDS[speed]),'--stream'],
                    stdin=subprocess.DEVNULL,stdout=output,stderr=subprocess.DEVNULL,start_new_session=True)
                if process.wait(timeout=remaining)!=0:raise RuntimeError('Section synthesis failed')
            if not 128<=original.stat().st_size<=2*1024*1024:raise RuntimeError('Section audio exceeds its bound')
            subprocess.run([str(ffmpeg),'-nostdin','-v','error','-i',str(original),
                '-map','0:a:0','-vn','-ac','1','-ar','16000','-c:a','libmp3lame','-b:a','32k',
                '-write_xing','0','-id3v2_version','0','-f','mp3','-y',str(temporary)],
                stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,
                check=True,timeout=min(20,max(.1,deadline-time.monotonic())))
            data=temporary.read_bytes()
            # MPEG-2 Layer III, 32 kbps/16 kHz: 144 bytes / 576 samples.
            # A bounds check is not a substitute for the Module's text alignment.
            duration=len(data)/4000.
            maximum=min(29.,max(5.,part['word_count']*.95+3))
            if len(data)<144 or len(data)%144 or not .2<duration<=maximum:
                raise RuntimeError('Section duration is inconsistent with its text')
            decoded=subprocess.run([str(ffmpeg),'-nostdin','-v','error','-i',str(temporary),
                '-f','s16le','-ac','1','-ar','16000','pipe:1'],stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,check=True,timeout=15).stdout
            if len(decoded)!=len(data)//144*1152:raise RuntimeError('Unexpected section decoder padding')
            checked=checker.check(decoded,text,part['word_start'])
            temporary.replace(delivery)
            meta=dict(part,version=2,state='complete',audio_bytes=len(data),
                      audio_sha256=hashlib.sha256(data).hexdigest(),duration_seconds=duration,
                      attempts=attempt+1,**checked)
            atomic_json(folder/('part-%03d.json'%i),meta)
            return meta
        except Exception as exc:
            last=exc
            if process is not None:terminate(process)
            if time.monotonic()>=deadline:break
    atomic_json(folder/('part-%03d.json'%i),dict(part,version=2,state='error',error='section_synthesis_failed'))
    raise RuntimeError('Narration section could not be generated') from last

def synthesize_sections(folder,voice,speed='normal',executable='/opt/hatch/bin/tts',
                        ffmpeg='ffmpeg',deadline_seconds=300,checker=None):
    from muse_speech_text import SpeechText
    folder=Path(folder)
    if folder.is_symlink() or not folder.is_dir():raise ValueError('Invalid job directory')
    if not re.fullmatch(r'avocado_v2:[A-Za-z0-9_.-]{1,80}',voice) or speed not in SPEEDS:
        raise ValueError('Invalid voice configuration')
    raw,text=read_source(folder);spoken=SpeechText(text);parts=plan_sections(spoken)
    claim=os.open(folder/'claimed',os.O_CREAT|os.O_EXCL|os.O_WRONLY,0o600);os.close(claim)
    started=time.monotonic();deadline=started+deadline_seconds
    if checker is None:
        from muse_vm_speech_check import SpeechCheck
        checker=SpeechCheck(Path.home()/'muse-storyteller'/'models'/'tiny.en')
    plan=dict(version=2,source_sha256=hashlib.sha256(raw).hexdigest(),sections=parts)
    atomic_json(folder/'plan.json',plan)
    meta=dict(version=2,state='sections',voice=voice,speed=speed,source_bytes=len(raw),
              source_sha256=plan['source_sha256'],section_count=len(parts),ready_sections=0,audio_bytes=0)
    atomic_json(folder/'meta.json',meta)
    try:
        # Bounded parallelism; submit the opening sections first.
        with ThreadPoolExecutor(max_workers=2,thread_name_prefix='muse-section') as pool:
            pending=[pool.submit(_render,folder,p,spoken,voice,speed,executable,ffmpeg,deadline,checker) for p in parts]
            digest=hashlib.sha256();total=0
            with (folder/'audio.mp3').open('xb') as joined:
                for i,future in enumerate(pending):
                    section=future.result(timeout=max(.1,deadline-time.monotonic()))
                    data=(folder/('part-%03d.mp3'%i)).read_bytes()
                    total+=len(data)
                    if total>MAX_AUDIO:raise ValueError('Narration exceeds its cache bound')
                    joined.write(data);joined.flush();digest.update(data)
                    meta.update(ready_sections=i+1,audio_bytes=total,elapsed_ms=round((time.monotonic()-started)*1000))
                    atomic_json(folder/'meta.json',meta)
                os.fsync(joined.fileno())
        meta.update(state='complete',audio_sha256=digest.hexdigest(),elapsed_ms=round((time.monotonic()-started)*1000))
        atomic_json(folder/'meta.json',meta);return meta
    except BaseException:
        meta.update(state='error',error='section_synthesis_failed')
        atomic_json(folder/'meta.json',meta);raise
