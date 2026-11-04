"""Paired-device native audio uploads, off the voice/control router thread."""
import queue
import threading
from muse_native_audio import Store, NativeAudioError


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
                result = self.store.request(req['data'],req['session'])
                reply = self.response(req,'cloud',result=result)
            except Exception as error:
                message=str(error) if isinstance(error,NativeAudioError) else 'Native audio cache failed'
                reply = self.response(req,'cloud',code=-35,message=message)
            self.emit(req['route'],reply)

    def close(self):
        # Router teardown must not acknowledge a still-running speaker.
        self.jobs.put(None)
        self.thread.join(timeout=12)
