"""Bounded, independently verifiable Muse narration sections."""
import hashlib,json,os,re,subprocess,time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from muse_vm_tts_job import atomic_json,read_source,SPEEDS,MAX_AUDIO,terminate

FINAL_RETRY_SECONDS = 10


def _time_left(deadline, maximum):
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise TimeoutError('Narration section timed out')
    return min(maximum, remaining)


def plan_sections(spoken,target=190,maximum_words=36,minimum_words=7):
    """Keep complete sentences together and avoid synthesizing tiny fragments."""
    words=spoken.words
    if not words:raise ValueError('Narration needs spoken words')
    groups=[];start=0
    while start<len(words):
        stop=start+1
        while stop<len(words) and not re.search(r'[.!?]["”’\']?$',words[stop-1][4]):
            stop+=1
        if groups:
            previous,end=groups[-1]
            fits=stop-previous<=maximum_words and words[stop-1][1]-words[previous][0]<=target
            if end-previous<minimum_words or (stop-start<minimum_words and (fits or stop==len(words))):
                groups[-1]=(previous,stop)
            else:
                groups.append((start,stop))
        else:
            groups.append((start,stop))
        start=stop
    result=[]
    for sentence,(start,stop) in enumerate(groups):
        # Do not begin playing this group until all its pieces are ready.
        at=start
        while at<stop:
            end=min(stop,at+maximum_words)
            while end>at+1 and words[end-1][1]-words[at][0]>target:end-=1
            if 0<stop-end<minimum_words and end-at>minimum_words:
                end=max(at+minimum_words,stop-minimum_words)
            result.append(dict(index=len(result),word_start=at,word_count=end-at,
                               sentence=sentence,sentence_end=end==stop,
                               spoken_sha256=hashlib.sha256(spoken.text[words[at][0]:words[end-1][1]].encode()).hexdigest()))
            at=end
    if len(result)>128:raise ValueError('Narration needs too many sections')
    return result

def _render(folder,part,spoken,voice,speed,executable,ffmpeg,deadline,checker,*,retry_check=None):
    i=part['index'];words=spoken.words;at=part['word_start'];end=at+part['word_count']
    text=spoken.text[words[at][0]:words[end-1][1]]
    original=folder/('part-%03d-original.mp3'%i)
    delivery=folder/('part-%03d.mp3'%i)
    temporary=delivery.with_suffix('.pending')
    last=None
    attempts = [text, text.strip('"“”')]
    if retry_check is not None:
        # Simplify the delivery of short dialogue. Keep the original text and
        # word positions for verification, and keep the same voice and speed.
        plain = ' '.join(text.translate(str.maketrans('"“”', '   ')).split())
        attempts.append(re.sub(r'…|\.{2,}', '.', plain))
    for attempt, spoken_text in enumerate(attempts):
        process=None
        final_retry = attempt == 2
        attempt_deadline = min(deadline, time.monotonic() + FINAL_RETRY_SECONDS) if final_retry else deadline
        try:
            remaining=_time_left(attempt_deadline,45)
            with original.open('wb') as output:
                process=subprocess.Popen([str(executable),'speak','--text',spoken_text,'--voice',voice,
                    '--language','en','--speed',str(SPEEDS[speed]),'--stream'],
                    stdin=subprocess.DEVNULL,stdout=output,stderr=subprocess.DEVNULL,start_new_session=True)
                if process.wait(timeout=remaining)!=0:raise RuntimeError('Section synthesis failed')
            if not 128<=original.stat().st_size<=2*1024*1024:raise RuntimeError('Section audio exceeds its bound')
            subprocess.run([str(ffmpeg),'-nostdin','-v','error','-i',str(original),
                '-map','0:a:0','-vn','-ac','1','-ar','16000','-c:a','libmp3lame','-b:a','32k',
                '-write_xing','0','-id3v2_version','0','-f','mp3','-y',str(temporary)],
                stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,
                check=True,timeout=_time_left(attempt_deadline,20))
            data=temporary.read_bytes()
            # MPEG-2 Layer III, 32 kbps/16 kHz: 144 bytes / 576 samples.
            # A bounds check is not a substitute for the Module's text alignment.
            duration=len(data)/4000.
            maximum=min(29.,max(5.,part['word_count']*.95+3))
            if len(data)<144 or len(data)%144 or not .2<duration<=maximum:
                raise RuntimeError('Section duration is inconsistent with its text')
            decoded=subprocess.run([str(ffmpeg),'-nostdin','-v','error','-i',str(temporary),
                '-f','s16le','-ac','1','-ar','16000','pipe:1'],stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,check=True,
                timeout=_time_left(attempt_deadline,15)).stdout
            if len(decoded)!=len(data)//144*1152:raise RuntimeError('Unexpected section decoder padding')
            if final_retry:
                checked=retry_check(decoded,text,part['word_start'],attempt_deadline)
            else:
                checked=checker.check(decoded,text,part['word_start'])
            _time_left(attempt_deadline,1)
            temporary.replace(delivery)
            meta=dict(part,version=2,state='complete',audio_bytes=len(data),
                      audio_sha256=hashlib.sha256(data).hexdigest(),duration_seconds=duration,
                      attempts=attempt+1,**checked)
            atomic_json(folder/('part-%03d.json'%i),meta)
            return meta
        except Exception as exc:
            last=exc
            if final_retry:
                break
            if process is not None:terminate(process)
            if time.monotonic()>=deadline:break
    try:
        atomic_json(folder/('part-%03d.json'%i),dict(part,version=2,state='error',error='section_synthesis_failed'))
    finally:
        # The player can fall back immediately, even if stopping the last
        # timed-out provider process takes a little longer.
        if final_retry and process is not None:
            terminate(process)
        temporary.unlink(missing_ok=True)
    raise RuntimeError('Narration section could not be generated') from last

def synthesize_sections(folder,voice,speed='normal',executable='/opt/hatch/bin/tts',
                        ffmpeg='ffmpeg',deadline_seconds=300,checker=None,checker_factory=None):
    from muse_speech_text import SpeechText
    folder=Path(folder)
    if folder.is_symlink() or not folder.is_dir():raise ValueError('Invalid job directory')
    if not re.fullmatch(r'avocado_v2:[A-Za-z0-9_.-]{1,80}',voice) or speed not in SPEEDS:
        raise ValueError('Invalid voice configuration')
    raw,text=read_source(folder);spoken=SpeechText(text);parts=plan_sections(spoken)
    claim=os.open(folder/'claimed',os.O_CREAT|os.O_EXCL|os.O_WRONLY,0o600);os.close(claim)
    started=time.monotonic();deadline=started+deadline_seconds
    plan=dict(version=2,source_sha256=hashlib.sha256(raw).hexdigest(),sections=parts)
    atomic_json(folder/'plan.json',plan)
    meta=dict(version=2,state='sections',voice=voice,speed=speed,source_bytes=len(raw),
              source_sha256=plan['source_sha256'],section_count=len(parts),ready_sections=0,audio_bytes=0)
    atomic_json(folder/'meta.json',meta)
    try:
        # Model loading, the first synthesis calls and the device's script
        # transfer overlap. Admission still waits for the same full checks.
        with ThreadPoolExecutor(max_workers=1,thread_name_prefix='muse-checker') as loader, \
             ThreadPoolExecutor(max_workers=2,thread_name_prefix='muse-section') as pool:
            if checker is None:
                if checker_factory is None:
                    from muse_vm_speech_check import SpeechCheck
                    checker_factory=lambda:SpeechCheck(Path.home()/'muse-storyteller'/'models'/'tiny.en')
                loaded=loader.submit(checker_factory)
                class DeferredCheck:
                    def check(self,*args):return loaded.result(timeout=max(.1,deadline-time.monotonic())).check(*args)
                checker=DeferredCheck()

            def retry_check(pcm, text, first_word, retry_deadline):
                _time_left(retry_deadline, FINAL_RETRY_SECONDS)
                future = loader.submit(checker.check, pcm, text, first_word)
                try:
                    return future.result(timeout=_time_left(retry_deadline, FINAL_RETRY_SECONDS))
                finally:
                    future.cancel()

            pending=[pool.submit(_render,folder,p,spoken,voice,speed,executable,ffmpeg,deadline,checker,
                                 retry_check=retry_check) for p in parts]
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
