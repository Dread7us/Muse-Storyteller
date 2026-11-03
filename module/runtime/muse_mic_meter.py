"""Bounded local PCM gain/gate. No audio files, network or idle recognition."""
import array,math,sys,threading,time
import muse_zmq as zmq
ENDPOINT='ipc:///tmp/musevoice/mic-input.sock'
def scaled_pcm(pcm,level):
    if len(pcm)%2 or len(pcm)>1024*1024:raise ValueError('Invalid microphone frame')
    if not 0<=level<=100:raise ValueError('Invalid input level')
    if level==100:return pcm
    a=array.array('h');a.frombytes(pcm)
    if sys.byteorder!='little':a.byteswap()
    for i,x in enumerate(a):a[i]=round(x*level/100)
    if sys.byteorder!='little':a.byteswap()
    return a.tobytes()
def pcm_db(pcm):
    if not pcm or len(pcm)%2:return None
    a=array.array('h');a.frombytes(pcm)
    if sys.byteorder!='little':a.byteswap()
    return max(-100.,min(0.,10*math.log10(max(sum(x*x for x in a)/len(a),.000001)/32768**2)))
class MicMeter:
    def __init__(self):
        self.lock=threading.Lock();self.stop=threading.Event();self.ready=threading.Event()
        self.subscribed=threading.Event();self.first_frame=threading.Event();self.prime_pcm=b""
        self.output_paused=False
        self.level=100;self.recording=False;self.db=-100.;self.sample_at=0;self.failure=None
        self.thread=threading.Thread(target=self.run,name='gated-local-microphone',daemon=True);self.thread.start()
        if not self.ready.wait(3):raise RuntimeError('Microphone gate did not start')
        self.check()
    def check(self):
        if self.failure:raise RuntimeError('Microphone gate failed')
    def request(self,on):pass
    def block(self,on):pass
    def suspend(self):pass
    def set_level(self,level):
        if type(level) is not int or not 0<=level<=100:raise ValueError('Invalid microphone level')
        with self.lock:self.level=level
    def record(self,on):
        self.check()
        with self.lock:self.recording=on
    def wait_ready(self,timeout):
        if not self.subscribed.wait(timeout) or not self.first_frame.wait(timeout):
            raise RuntimeError('Microphone PCM route is not ready')
        self.check()
    def prime(self,pcm):
        if len(pcm)>1024*1024 or len(pcm)%2:raise ValueError('Invalid warm-up PCM')
        with self.lock:self.prime_pcm=pcm
    def finish_prime(self):
        with self.lock:self.prime_pcm=b''
    def pause_output(self,on):
        # Keep the sensor/meter and recognizer loaded. During narration the
        # recognizer needs no idle silence; avoid competing with synthesis.
        with self.lock:self.output_paused=bool(on)
    def fields(self):
        with self.lock:
            active=time.monotonic()-self.sample_at<.6
            return {'mic_active':active,'mic_db':round(self.db,1) if active else -100.}
    def close(self):
        self.stop.set();self.thread.join(3)
        if self.thread.is_alive():raise RuntimeError('Microphone gate did not join')
    def run(self):
        ctx=None;sub=None;pub=None
        try:
            ctx=zmq.Context();sub=ctx.socket(zmq.SUB,timeout_ms=100,max_bytes=1024*1024)
            sub.connect('ipc:///tmp/llm/pcm.cap.socket')
            pub=ctx.socket(zmq.XPUB,timeout_ms=100,max_bytes=1024*1024);pub.bind(ENDPOINT)
            self.ready.set()
            while not self.stop.is_set():
                if pub.readable(0):
                    event=pub.recv()
                    if event==b"\1":self.subscribed.set()
                if not sub.readable(50):continue
                raw=sub.recv()
                with self.lock:level=self.level;recording=self.recording
                pcm=scaled_pcm(raw,level);db=pcm_db(pcm)
                if db is not None:
                    with self.lock:self.db=db;self.sample_at=time.monotonic()
                with self.lock:
                    publish=not self.output_paused or recording or bool(self.prime_pcm)
                    if self.prime_pcm:
                        outgoing=self.prime_pcm[:len(pcm)]
                        self.prime_pcm=self.prime_pcm[len(pcm):]
                        outgoing=outgoing.ljust(len(pcm),b'\0')
                    else:outgoing=pcm if recording else b'\0'*len(pcm)
                if publish:pub.send(outgoing)
                self.first_frame.set()
        except Exception as exc:self.failure=type(exc).__name__;self.ready.set()
        finally:
            if sub:sub.close()
            if pub:pub.close()
            if ctx:ctx.close()
