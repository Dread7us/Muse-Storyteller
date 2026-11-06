"""Play only complete, hash-checked and word-aligned narration sections."""
import base64,hashlib,json,re,threading,time
from pathlib import Path
from muse_speech_text import SpeechText

class SeriesError(RuntimeError):pass

def digest(data):return hashlib.sha256(data).hexdigest()

def checked_plan(text,plan):
    spoken=SpeechText(text);parts=plan.get('sections') if isinstance(plan,dict) else None
    if not isinstance(plan,dict) or plan.get('version')!=2 or plan.get('source_sha256')!=digest(text.encode()) or not isinstance(parts,list) or not 1<=len(parts)<=128:
        raise SeriesError('Invalid narration plan')
    cursor=0;sentence=0
    for i,p in enumerate(parts):
        if not isinstance(p,dict) or p.get('index')!=i or p.get('word_start')!=cursor or type(p.get('word_count')) is not int or not 1<=p['word_count']<=36:
            raise SeriesError('Invalid narration word range')
        if p.get('sentence')!=sentence or type(p.get('sentence_end')) is not bool:raise SeriesError('Invalid narration sentence boundary')
        end=cursor+p['word_count']
        if end>len(spoken.words):raise SeriesError('Narration word range exceeds its text')
        section=spoken.text[spoken.words[cursor][0]:spoken.words[end-1][1]]
        if p.get('spoken_sha256')!=digest(section.encode()):raise SeriesError('Narration pronunciation changed')
        cursor=end
        if p['sentence_end']:sentence+=1
    if cursor!=len(spoken.words) or not parts[-1]['sentence_end']:raise SeriesError('Narration plan omitted text')
    return spoken,parts

def aligned_schedule(spoken,part,result,duration):
    """No duration-proportional fallback: every spoken word needs a real anchor."""
    import math
    first=part['word_start'];last=first+part['word_count']
    indices=[i for i in range(first,last) if re.search(r'\w',spoken.words[i][4])]
    aligned=result.get('words')
    if result.get('aligned') is not True or not isinstance(aligned,list) or len(aligned)!=len(indices):raise SeriesError('Narration word timing is incomplete')
    schedule=[];previous=-1.
    for expected,row in zip(indices,aligned):
        if not isinstance(row,list) or len(row)!=3:raise SeriesError('Invalid narration timing')
        begin,end,index=row
        if type(index) is not int or index!=expected or not all(type(x) in (int,float) and math.isfinite(x) for x in (begin,end)) or not 0<=begin<end<=duration+.04 or begin<previous:
            raise SeriesError('Narration word timing collapsed')
        word=spoken.words[index];schedule.append((begin,word[2],word[3]));previous=begin
    if not schedule:raise SeriesError('Narration has no aligned words')
    return schedule

class Series:
    def __init__(self,store,key,text,plan,session,folder=None):
        self.store,self.key,self.text,self.session=store,key,text,session
        self.spoken,self.parts=checked_plan(text,plan)
        self.folder=folder or store.root/key;self.folder.mkdir(mode=0o700,parents=True,exist_ok=True)
        self.items={};self.timings={};self.error=None;self.upload=None;self.sealed=False
        self.lock=threading.RLock()
        manifest=self.folder/'series.json'
        if manifest.exists():
            old=json.loads(manifest.read_text())
            if old.get('source')!=digest(text.encode()) or old.get('plan')!=plan:raise SeriesError('Narration identity changed')
        else:manifest.write_text(json.dumps(dict(version=1,source=digest(text.encode()),plan=plan)))

    def _part_text(self,index):
        p=self.parts[index];words=self.spoken.words;a=p['word_start'];b=a+p['word_count']
        return self.spoken.text[words[a][0]:words[b-1][1]]

    def _load_part(self,index):
        from muse_native_audio import Item
        folder=self.folder/('part-%03d'%index)
        try:
            meta=json.loads((folder/'complete.json').read_text());timing=json.loads((folder/'aligned.json').read_text())
            pcm=folder/'voice.pcm';mp3=folder/'voice.mp3'
            if folder.is_symlink() or mp3.is_symlink() or pcm.is_symlink():return None
            if meta.get('spoken_sha256')!=self.parts[index]['spoken_sha256'] or mp3.stat().st_size!=meta['audio_bytes'] or digest(mp3.read_bytes())!=meta['audio_sha256'] or pcm.stat().st_size!=meta['pcm_bytes'] or not 320<=meta['pcm_bytes']<=960000 or digest(pcm.read_bytes())!=meta.get('pcm_sha256'):
                return None
            if timing.get('version')!=1 or timing.get('audio_sha256')!=meta['audio_sha256'] or not timing.get('checked'):return None
            schedule=aligned_schedule(self.spoken,self.parts[index],dict(aligned=True,words=timing['anchors']),meta['pcm_bytes']/32000.)
            item=object.__new__(Item);item.key=str(index);item.text=self._part_text(index);item.session=self.session;item.folder=folder;item.mp3=mp3;item.pcm=pcm
            item.received=meta['audio_bytes'];item.done=True;item.error=None;item.digest=hashlib.sha256(mp3.read_bytes());item.file=None;item.decoder=None
            self.items[index]=item;self.timings[index]=schedule
            return item
        except (OSError,ValueError,TypeError,KeyError,SeriesError):return None

    def status(self):
        with self.lock:
            count=0;seconds=0.;safe=0;safe_seconds=0.
            for i in range(len(self.parts)):
                if i not in self.timings:break
                count+=1;seconds+=self.items[i].size()/32000.
                if self.parts[i]['sentence_end']:safe=count;safe_seconds=seconds
            ready=safe>0 and (safe==len(self.parts) or (safe>=2 and safe_seconds>=12))
            return dict(received=sum(x.received for x in self.items.values()),complete=self.sealed,
                        ready=ready,ready_sections=count,error=bool(self.error))

    def request(self,data,session):
        from muse_native_audio import Item
        if session!=self.session:raise SeriesError('Stale narration session')
        op=data.get('op')
        with self.lock:
            if op=='series_status':
                if self.error:raise SeriesError(self.error)
                return self.status()
            if op=='abort':
                if self.upload is not None:
                    item=self.items[self.upload]
                    if not item.done:item.abort()
                return self.status()
            if op=='series_abort':
                self.error='Narration transfer stopped';return self.status()
            if op=='part_begin':
                index=data.get('index');size=data.get('bytes');sha=data.get('sha256')
                if type(index) is not int or not 0<=index<len(self.parts) or type(size) is not int or not 144<=size<=120000 or not re.fullmatch('[0-9a-f]{64}',str(sha)):
                    raise SeriesError('Invalid narration section')
                item=self.items.get(index) or self._load_part(index)
                if item and item.done:
                    if item.received!=size or item.digest.hexdigest()!=sha:raise SeriesError('Narration audio changed')
                    self.upload=index;return dict(received=size,complete=True,sha256=sha)
                if item:item.abort()
                folder=self.folder/('part-%03d'%index)
                if folder.exists():
                    previous=folder.with_name(folder.name+'.interrupted-'+str(time.monotonic_ns()))
                    folder.rename(previous)
                item=Item(self.folder,'part-%03d'%index,self._part_text(index),session)
                item.expected_bytes=size;item.expected_hash=sha;self.items[index]=item;self.upload=index
                return dict(received=0,complete=False)
            if op=='chunk':
                if self.upload is None:raise SeriesError('No narration upload is active')
                item=self.items[self.upload];offset=data.get('offset')
                try:raw=base64.b64decode(data.get('audio',''),validate=True)
                except (ValueError,TypeError):raise SeriesError('Invalid narration audio')
                if not raw or len(raw)>8192 or data.get('bytes')!=len(raw) or data.get('sha256')!=digest(raw):raise SeriesError('Narration block failed its check')
                if type(offset) is not int or offset<0:raise SeriesError('Invalid narration offset')
                if offset+len(raw)<=item.received:
                    with item.mp3.open('rb') as f:f.seek(offset);old=f.read(len(raw))
                    if old==raw:return dict(received=item.received)
                if item.done or offset!=item.received or offset+len(raw)>item.expected_bytes:raise SeriesError('Narration section is out of order')
                item.file.write(raw);item.file.flush();item.decoder.stdin.write(raw);item.decoder.stdin.flush();item.digest.update(raw);item.received+=len(raw)
                return dict(received=item.received)
            if op=='part_end':
                if self.upload is None:raise SeriesError('No narration upload is active')
                index=self.upload;item=self.items[index]
                if item.done:return dict(received=item.received)
                if item.received!=item.expected_bytes or item.digest.hexdigest()!=item.expected_hash:raise SeriesError('Narration section failed its hash')
                item.file.close();item.decoder.stdin.close();item.decoder.wait(timeout=8)
                if item.decoder.returncode or item.size()<320 or item.size()>960000:raise SeriesError('Narration section did not decode completely')
                if type(data.get('pcm_samples')) is not int or data['pcm_samples']*2!=item.size():raise SeriesError('Narration timing audio length changed')
                schedule=aligned_schedule(self.spoken,self.parts[index],data,item.size()/32000.)
                (item.folder/'complete.json').write_text(json.dumps(dict(spoken_sha256=self.parts[index]['spoken_sha256'],audio_bytes=item.received,audio_sha256=item.digest.hexdigest(),pcm_bytes=item.size(),pcm_sha256=digest(item.pcm.read_bytes()))))
                value=dict(version=1,checked=True,audio_sha256=item.digest.hexdigest(),anchors=data['words'])
                path=item.folder/'aligned.json';tmp=path.with_suffix('.pending');tmp.write_text(json.dumps(value));tmp.replace(path)
                item.done=True
                self.timings[index]=schedule
                return dict(received=item.received)
            if op=='series_end':
                if len(self.items)!=len(self.parts) or any(not self.items[i].done for i in range(len(self.parts))):raise SeriesError('Narration omitted a section')
                combined=hashlib.sha256();size=0
                for i in range(len(self.parts)):
                    block=self.items[i].mp3.read_bytes();combined.update(block);size+=len(block)
                if data.get('bytes')!=size or data.get('sha256')!=combined.hexdigest():raise SeriesError('Complete narration failed its hash')
                (self.folder/'sealed.json').write_text(json.dumps(dict(bytes=size,sha256=combined.hexdigest())))
                self.sealed=True;return self.status()
        raise SeriesError('Unknown narration operation')

    def close(self):
        with self.lock:
            for item in self.items.values():
                if not item.done:item.abort()
            self.error='Narration service stopped'

    def play(self,backend,volume,cancel,on_play,on_buffering=None):
        from muse_voice_backend import check_cancel
        position=0;total=0;blocks=0;words=0;paused=False;buffering=False
        try:
            while position<len(self.parts):
                until=time.monotonic()+180
                while True:
                    check_cancel(cancel)
                    control=getattr(backend,'playback_control',None)
                    requested=control.read()[0] if control else False
                    if requested!=paused:
                        paused=requested
                        callback=getattr(backend,'playback_state_callback',None)
                        if callback:callback(paused)
                    with self.lock:
                        if self.error:raise SeriesError(self.error)
                        end=position
                        # Never wait for data in the middle of one sentence.
                        while end<len(self.parts) and end in self.timings:
                            end+=1
                            if self.parts[end-1]['sentence_end']:break
                        ready=end>position and self.parts[end-1]['sentence_end']
                    if position==0:ready=ready and self.status()['ready']
                    if ready and not paused:break
                    if not buffering and on_buffering:on_buffering();buffering=True
                    if time.monotonic()>until and not paused:raise SeriesError('Next narration sentence timed out')
                    if cancel:cancel.wait(.02)
                    else:time.sleep(.02)
                    if paused:until=time.monotonic()+180
                # Join adjacent ready sentences, bounded below the native IPC limit.
                with self.lock:
                    safe=end;size=sum(self.items[i].size() for i in range(position,end))
                    while end<len(self.parts) and end in self.timings and size+self.items[end].size()<=1536000:
                        size+=self.items[end].size();end+=1
                        if self.parts[end-1]['sentence_end']:safe=end
                    end=safe
                    # An exceptionally long sentence is fully buffered first,
                    # then played in bounded native calls without waiting for
                    # more network data inside that sentence.
                    if sum(self.items[i].size() for i in range(position,end))>1536000:
                        end=position;size=0
                        while end<safe and size+self.items[end].size()<=1536000:
                            size+=self.items[end].size();end+=1
                    pcm=[];schedule=[];seconds=0.
                    for i in range(position,end):
                        data=self.items[i].pcm.read_bytes()
                        if len(data)!=self.items[i].size():raise SeriesError('Narration cache was shortened')
                        pcm.append(data);schedule.extend((t+seconds,a,b) for t,a,b in self.timings[i]);seconds+=len(data)/32000.
                backend.word_schedule=schedule
                if on_play:on_play()
                buffering=False
                backend.play(b''.join(pcm),volume,cancel)
                total+=sum(map(len,pcm));blocks+=1;words+=len(schedule);position=end
            until=time.monotonic()+90
            while not self.sealed:
                check_cancel(cancel)
                if self.error:raise SeriesError(self.error)
                if time.monotonic()>until:raise SeriesError('Final narration check timed out')
                time.sleep(.02)
            result=dict(complete=True,pcm_bytes=total,played_pcm_bytes=total,blocks=blocks,words=words,sections=position,aligned=True)
            (self.folder/'last-playback.json').write_text(json.dumps(result));return result
        finally:
            callback=getattr(backend,'playback_state_callback',None)
            if paused and callback:callback(False)
