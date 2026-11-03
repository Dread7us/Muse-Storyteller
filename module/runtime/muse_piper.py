"""Persistent offline Piper worker. PCM stays 16 kHz; voice model is unchanged."""
import atexit
import json
import os
from pathlib import Path
import select
import subprocess
import tempfile
import threading
import time
import wave

ROOT=Path('/opt/muse-core2-bridge')
MAX_PCM=4*1024*1024
SMOOTH_NARRATION=True
MAX_STARTUP_PCM=8*1024*1024
MAX_STARTUP_SECONDS=14.0
MAX_STARTUP_SEGMENTS=3
TEMP_DIR=Path('/tmp/musevoice')

def story_phrases(text,limit=None,first_limit=None):
    from muse_narration_plan import sentences
    return list(sentences(' '.join(text.split())))


class PiperWorker:
    def __init__(self):
        self.lock=threading.Lock()
        self.proc=None
        self.pid=None
        self.speed=None

    def close(self):
        proc,self.proc=self.proc,None
        if proc is not None:
            if proc.poll() is None:proc.kill()
            proc.wait(timeout=5)
            for stream in (proc.stdin,proc.stdout):
                if stream:stream.close()

    def start(self,speed="normal"):
        if speed not in ("slow","normal","fast"):raise ValueError("Invalid speech speed")
        if self.proc is not None and self.proc.poll() is None and self.speed==speed:return
        self.close()
        TEMP_DIR.mkdir(mode=0o700,parents=True,exist_ok=True)
        env=dict(os.environ,LD_LIBRARY_PATH=str(ROOT/'piper'),OMP_NUM_THREADS='2',OPENBLAS_NUM_THREADS='1')
        self.proc=subprocess.Popen([str(ROOT/'piper/piper'),'--model',str(ROOT/'en_US-lessac-low.onnx'),
            '--json-input','--quiet','--length_scale',str({'slow':1.7,'normal':1.25,'fast':.88}[speed]),'--sentence_silence','0.07'],
            stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,bufsize=0,env=env)
        self.pid=self.proc.pid;self.speed=speed

    def synthesize(self,text,cancel,check_cancel,speed="normal"):
        check_cancel(cancel)
        if not isinstance(text,str) or not 0<len(text.encode('utf-8'))<=2048:
            raise ValueError('Invalid offline speech input')
        while not self.lock.acquire(timeout=0.05):check_cancel(cancel)
        path=None
        try:
            check_cancel(cancel);self.start(speed)
            fd,name=tempfile.mkstemp(prefix='piper-',suffix='.wav',dir=str(TEMP_DIR))
            os.close(fd);path=Path(name)
            request=json.dumps({'text':text,'output_file':str(path)},ensure_ascii=False).encode()+b'\n'
            self.proc.stdin.write(request);self.proc.stdin.flush()
            deadline=time.monotonic()+90;reply=b''
            while b'\n' not in reply:
                check_cancel(cancel)
                if time.monotonic()>deadline:raise TimeoutError('Offline speech timed out')
                if self.proc.poll() is not None:raise RuntimeError('Offline speech worker exited')
                if select.select([self.proc.stdout],[],[],0.05)[0]:
                    chunk=os.read(self.proc.stdout.fileno(),1024)
                    if not chunk:raise RuntimeError('Offline speech output closed')
                    reply+=chunk
                    if len(reply)>4096:raise RuntimeError('Unexpected offline speech reply')
            if reply.strip().decode()!=str(path):raise RuntimeError('Offline speech response mismatch')
            # The CLI prints the path just before its output stream closes.
            for attempt in range(20):
                check_cancel(cancel)
                try:
                    with wave.open(str(path),'rb') as stream:
                        if (stream.getframerate(),stream.getnchannels(),stream.getsampwidth())!=(16000,1,2):
                            raise RuntimeError('Offline speech format changed')
                        count=stream.getnframes()*2
                        if not 0<count<=MAX_PCM:raise RuntimeError('Invalid offline speech size')
                        pcm=stream.readframes(stream.getnframes())
                    if len(pcm)==count:return pcm
                except (EOFError,wave.Error):pass
                time.sleep(0.01)
            raise RuntimeError('Offline speech output incomplete')
        except BaseException:
            # Cancellation invalidates the in-flight reply and restarts cleanly
            # on demand. It must never leak old audio into a later utterance.
            self.close();raise
        finally:
            if path is not None:path.unlink(missing_ok=True)
            self.lock.release()

WORKER=PiperWorker()
atexit.register(WORKER.close)

def synthesize(text,cancel,check_cancel,speed="normal"):
    return WORKER.synthesize(text,cancel,check_cancel,speed)

def warmup():
    def prime():
        try:synthesize('Ready.',None,lambda _:None)
        except Exception:pass
    threading.Thread(target=prime,name='piper-warmup',daemon=True).start()

def speak(backend,text,volume,cancel,on_play,check_cancel):
    """Prepare complete sentences/items; only their boundaries may wait for synthesis."""
    import queue
    from collections import deque
    from muse_speech_text import SpeechText
    from muse_narration_plan import playback_units,render_parts,MAX_UNIT_PCM
    timing=getattr(backend,"word_timing",None)
    normalized=SpeechText(text) if timing is None else timing
    spoken=normalized.text if timing is None else timing.normalized
    units=playback_units(text,spoken,normalized.words)
    speed=getattr(backend,"speech_speed","normal")
    if not units:
        if on_play:on_play()
        return dict(segments=0,pcm_bytes=0,joined=True,startup_buffer_segments=0,sentence_safe=True)
    pending=queue.Queue(maxsize=2);stop=threading.Event()
    class CombinedCancel:
        def is_set(self):return stop.is_set() or (cancel is not None and cancel.is_set())
    combined=CombinedCancel()
    def produce():
        failure=None
        for unit in units:
            if combined.is_set():return
            began=time.monotonic()
            try:
                if failure:raise failure
                pieces=[];size=0
                for part in render_parts(unit):
                    pcm=synthesize(part,combined,check_cancel,speed);size+=len(pcm)
                    if size>MAX_UNIT_PCM:raise ValueError('Narration item exceeds safe audio budget')
                    pieces.append((part,pcm))
                value=pieces
            except Exception as exc:failure=exc;value=exc
            while not combined.is_set():
                try:pending.put((unit,value,time.monotonic()-began),timeout=.05);break
                except queue.Full:pass
    producer=threading.Thread(target=produce,name='piper-prefetch',daemon=True)
    startup_began=time.monotonic();producer.start()
    buffered=deque();buffered_audio=0.;buffered_generation=0.;buffered_chars=0;startup_count=0
    deadline=time.monotonic()+1200;total=0;first=True;waits=0;max_wait=0.
    def audio_bytes(value):return sum(len(pcm) for _,pcm in value) if not isinstance(value,Exception) else 0
    def take():
        while True:
            check_cancel(cancel)
            if time.monotonic()-getattr(backend,'playback_pause_total',0.)>deadline:raise TimeoutError('Narration timed out')
            try:return pending.get(timeout=.05)
            except queue.Empty:
                check_cancel(cancel)
                if not producer.is_alive():raise RuntimeError('Narration producer stopped')
    def needs_buffer():
        if not buffered:return True
        if not SMOOTH_NARRATION or len(buffered)>=min(MAX_STARTUP_SEGMENTS,len(units)):return False
        if any(isinstance(x[1],Exception) for x in buffered):return False
        elapsed=time.monotonic()-startup_began
        if elapsed>=MAX_STARTUP_SECONDS or buffered_audio*32000>=MAX_STARTUP_PCM:return False
        next_chars=len(units[len(buffered)])
        estimate=next_chars*buffered_generation/max(1,buffered_chars)
        if elapsed+estimate>MAX_STARTUP_SECONDS:return False
        remain_chars=sum(map(len,units))-buffered_chars
        ratio=max(1.30,buffered_generation/max(.001,buffered_audio))
        deficit=(ratio-1)*remain_chars*buffered_audio/max(1,buffered_chars)*1.25
        return buffered_audio<deficit+.25
    def play_ready(pieces):
        # Preserve each rendered fragment's own timing anchor before joining PCM.
        pcm=b''.join(value for _,value in pieces);schedule=[];offset=0.
        prepare=getattr(backend,'prepare_phrase_words',None)
        for part,value in pieces:
            if prepare:
                prepare(part,value)
                schedule.extend((t+offset,a,b) for t,a,b in getattr(backend,'word_schedule',[]))
            offset+=len(value)/32000
        # Very long items can exceed the native 4 MiB RPC limit. All their audio
        # is already ready here: these transport blocks never wait for synthesis.
        for start in range(0,len(pcm),MAX_PCM):
            end=min(len(pcm),start+MAX_PCM);base=start/32000
            backend.word_schedule=[(t-base,a,b) for t,a,b in schedule if base<=t<end/32000]
            backend.play(pcm[start:end],volume,cancel)
        return len(pcm)
    try:
        while needs_buffer():
            item=take();buffered.append(item);startup_count+=1
            unit,value,seconds=item;buffered_chars+=len(unit);buffered_generation+=seconds;buffered_audio+=audio_bytes(value)/32000
        # A partial look-ahead credit absorbs the measured production deficit
        # without requiring the whole next item (or a whole story) up front.
        if SMOOTH_NARRATION and buffered and len(buffered)<len(units) and not any(isinstance(x[1],Exception) for x in buffered):
            elapsed=time.monotonic()-startup_began
            next_seconds=len(units[len(buffered)])*buffered_generation/max(1,buffered_chars)*1.25
            credit=max(0.,next_seconds-buffered_audio+.15)
            hold_until=time.monotonic()+min(max(0.,MAX_STARTUP_SECONDS-elapsed),credit)
            while time.monotonic()<hold_until:
                check_cancel(cancel)
                try:item=pending.get(timeout=min(.05,max(.001,hold_until-time.monotonic())))
                except queue.Empty:continue
                buffered.append(item);startup_count+=1
                unit,value,seconds=item;buffered_chars+=len(unit);buffered_generation+=seconds;buffered_audio+=audio_bytes(value)/32000
                break
        for unit_index in range(len(units)):
            before=time.monotonic();unit,value,_=buffered.popleft() if buffered else take()
            waited=time.monotonic()-before
            if not first and waited>.15:waits+=1;max_wait=max(max_wait,waited)
            if isinstance(value,Exception):
                check_cancel(cancel)
                # Retain the native fallback, fully prepared before this item plays.
                from muse_voice_backend import split_phrases
                parts=[backend._synthesize_native(p,cancel) for p in split_phrases(unit)]
                value=[(unit,b''.join(parts))];engine='original-native-fallback'
            else:engine='piper-en_US-lessac-low'
            size=audio_bytes(value)
            if size>MAX_UNIT_PCM:raise ValueError('Narration audio exceeds item budget')
            backend.record_synthesis(engine,size);check_cancel(cancel)
            if first and on_play:on_play()
            first=False;total+=play_ready(value)
        return dict(segments=len(units),pcm_bytes=total,joined=True,startup_buffer_segments=startup_count,startup_buffer_pcm_seconds=round(buffered_audio,3),smooth_narration=SMOOTH_NARRATION,sentence_safe=True,boundary_waits=waits,max_boundary_wait_seconds=round(max_wait,3))
    finally:
        stop.set();producer.join(timeout=6)
        if producer.is_alive():raise RuntimeError('Offline narration did not stop')
