"""Paired-device native audio uploads, off the voice/control router thread."""
import queue
import threading
import hashlib,json
from muse_native_audio import Store, NativeAudioError

def checked_payload(data):
    if data.get('op')!='packet':return {k:v for k,v in data.items() if k!='_transport_verified'}
    payload=data.get('payload');digest=data.get('sha256')
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
        self.store = Store()
        engine.native_store = self.store
        self.store.warming = self.store.pool.submit(self.store.warm)
        self.thread = threading.Thread(target=self.run, name='muse-cloud-cache', daemon=True)
        self.thread.start()

    def submit(self, req):
        try:self.jobs.put_nowait(req)
        except queue.Full:self.emit(req['route'],self.response(req,'cloud',code=-35,message='Audio cache busy'))

    def run(self):
        while True:
            req = self.jobs.get()
            if req is None:return
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

    def close(self):
        # Router teardown must not acknowledge a still-running speaker.
        self.jobs.put(None)
        self.thread.join(timeout=12)
        self.store.close()
