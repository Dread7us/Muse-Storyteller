import queue
import threading
import collections
import json
from muse_memory_store import Store

class MemoryWorker:
    def __init__(self,emit,response,directory='/var/lib/muse-conversations',engine=None):
        self.engine=engine
        self.emit,self.response,self.directory=emit,response,directory
        self.jobs=queue.Queue(16)
        self.thread=threading.Thread(target=self.run,name='muse-archive',daemon=True)
        self.thread.start()

    def submit(self,req):
        try:self.jobs.put_nowait(req)
        except queue.Full:self.emit(req['route'],self.response(req,'memory',code=-40,message='Archive busy'))

    def run(self):
        store=None;usage=None;refresh=None;sd=None;cache=collections.OrderedDict()
        try:
            try:store=Store(self.directory)
            except Exception:pass # archive failure must never stop working voice
            try:
                from muse_usage import Usage
                usage=Usage();usage.sync(store.db)
                from muse_usage_refresh import Refresh
                refresh=Refresh()
            except Exception:pass # usage must never impair archive or voice
            try:
                from muse_sd import SdWorker
                sd=SdWorker(self.directory,self.engine.recorder if self.engine else None)
            except Exception:pass # removable media is optional
            while True:
                req=self.jobs.get()
                if req is None:break
                identity=(req['session'],req['request_id'])
                signature=json.dumps(req['data'],sort_keys=True)
                try:
                    if identity in cache:
                        old_signature,reply=cache[identity]
                        if old_signature!=signature:raise ValueError('Archive retry differs')
                    else:
                        if req['data'].get('op')=='sd':
                            if not sd:raise ValueError('microSD support not installed on Module')
                            result={'sd':sd.submit(req['data'])}
                        elif store is None:raise ValueError('Archive unavailable')
                        elif req['data'].get('op') in ('recstart','recstop','recstatus','recpage','recprev','recnext'):
                            if not self.engine:raise ValueError('Recorder support not installed')
                            result={'recorder':self.engine.recorder_command(req)}
                        elif req['data'].get('op')=='usageapply':
                            if not usage or not refresh:raise ValueError('Usage storage unavailable')
                            _,label=usage.calibrate_paired(req['data'].get('observation'))
                            refresh.paired_sync(label)
                            result={'paired_applied':True}
                        elif req['data'].get('op') in ('usage','usagecheck'):
                            result={}
                            if refresh and req['data'].get('op')=='usagecheck':refresh.request()
                        else:result=store.handle(req['data'])
                        if usage and 'sd' not in result:
                            try:
                                if req['data'].get('op')=='put' and req['data'].get('finish'):usage.sync(store.db)
                                result['usage']=usage.snapshot()
                                if refresh:result['usage_refresh']=refresh.snapshot()
                            except Exception:pass
                        if self.engine and 'recorder' not in result and 'sd' not in result:result['recorder']=self.engine.recorder.snapshot()
                        reply=self.response(req,'memory',result=result)
                        cache[identity]=(signature,reply)
                        if len(cache)>128:cache.popitem(last=False)
                except Exception as exc:
                    message=str(exc) if isinstance(exc,ValueError) else 'Archive storage error; existing chats retained'
                    reply=self.response(req,'memory',code=-40,message=message)
                try:self.emit(req['route'],reply)
                except Exception:pass # client can retry; completed writes deduplicate
        finally:
            if store:store.close()

    def close(self):
        self.jobs.put(None)
        self.thread.join(5)
