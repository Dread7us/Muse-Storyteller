"""Paired-device native audio uploads, off the voice/control router thread."""
import queue
import threading
import base64,hashlib,json,re
from muse_native_audio import Store, NativeAudioError

def checked_payload(data):
    if data.get('op')=='raw_block':
        key,offset,size,digest=data.get('key'),data.get('offset'),data.get('bytes'),data.get('sha256')
        if not isinstance(key,str) or not re.fullmatch('[0-9a-f]{24}',key) or type(offset) is not int or not 0<=offset<=32*1024*1024 or type(size) is not int or not 1<=size<=8192 or not isinstance(digest,str) or not re.fullmatch('[0-9a-f]{64}',digest):
            raise NativeAudioError('Invalid binary block header')
        signature=hashlib.sha256(('%s:%d:%d:%s'%(key,offset,size,digest)).encode()).hexdigest()
        if data.get('packet_sha256')!=signature:raise NativeAudioError('Binary block header integrity failed')
        encoded=data.get('audio')
        if not isinstance(encoded,str) or len(encoded)>11000:raise NativeAudioError('Invalid binary block body')
        try:body=base64.b64decode(encoded,validate=True)
        except ValueError:raise NativeAudioError('Invalid binary block encoding')
        if len(body)!=size or hashlib.sha256(body).hexdigest()!=digest:raise NativeAudioError('Binary block body integrity failed')
        return dict(op='chunk',key=key,offset=offset,bytes=size,sha256=digest,audio=encoded,_transport_verified=True)
    if data.get('op')!='packet':return {k:v for k,v in data.items() if k!='_transport_verified'}
    payload=data.get('payload');digest=data.get('sha256')
    if 'payload_b64' in data:
        encoded=data['payload_b64']
        if payload is not None or not isinstance(encoded,str) or len(encoded)>43692:
            raise NativeAudioError('Invalid native request envelope')
        try:payload=base64.b64decode(encoded,validate=True).decode('utf8')
        except (ValueError,UnicodeError):raise NativeAudioError('Invalid native request encoding')
    if not isinstance(payload,str) or not isinstance(digest,str) or len(payload.encode())>32768 or hashlib.sha256(payload.encode()).hexdigest()!=digest:
        raise NativeAudioError('Native request integrity check failed')
    try:decoded=json.loads(payload)
    except ValueError:raise NativeAudioError('Invalid native request payload')
    if not isinstance(decoded,dict) or decoded.get('op')=='packet':raise NativeAudioError('Invalid native request payload')
    decoded['_transport_verified']=True
    return decoded


class CloudWorker:
    def __init__(self, emit, response, engine):
        self.emit, self.response, self.engine = emit, response, engine
        self.jobs = queue.Queue(4)
        self.active = threading.Event()
        self.store = Store()
        engine.native_store = self.store
        # Section timings arrive checked with their exact audio. Load the
        # optional local aligner only if a legacy whole-clip job needs it.
        self.thread = threading.Thread(target=self.run, name='muse-cloud-cache', daemon=True)
        self.thread.start()

    def submit(self, req):
        try:self.jobs.put_nowait(req)
        except queue.Full:self.emit(req['route'],self.response(req,'cloud',code=-35,message='Audio cache busy'))

    def run(self):
        while True:
            req = self.jobs.get()
            if req is None:return
            self.active.set()
            try:
                try:
                    with self.engine.lock:
                        allowed = self.engine.ready and req['session']==self.engine.session
                    if not allowed:raise NativeAudioError('Wait for the paired device session')
                    result = self.store.request(checked_payload(req['data']),req['session'])
                    reply = self.response(req,'cloud',result=result)
                except Exception as error:
                    message=str(error) if isinstance(error,NativeAudioError) else 'Native audio cache failed'
                    reply = self.response(req,'cloud',code=-35,message=message)
                self.emit(req['route'],reply)
            finally:self.active.clear()

    def close(self):
        # Router teardown must not acknowledge a still-running speaker.
        self.jobs.put(None)
        self.thread.join(timeout=12)
        self.store.close()

def reply_poll_ms(cloud):
    # A reply produced just after submit must not wait through another idle
    # 20 ms receive poll. Hundreds of sequential packets amplify that delay.
    return 2 if cloud.active.is_set() or not cloud.jobs.empty() else 20
