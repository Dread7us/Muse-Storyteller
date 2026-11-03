"""Private, bounded text archive. No audio, network access, or generated facts."""
import json
import os
import re
import shutil
import sqlite3
import time
import uuid
from pathlib import Path

PAGE = 1400  # Unicode characters; pages partition the complete transcript.
MAX_DB = 256 * 1024 * 1024


class Store:
    def __init__(self, directory, reserve=512*1024*1024):
        self.directory = Path(directory)
        self.directory.mkdir(mode=0o700, parents=True, exist_ok=True)
        if os.name == 'posix': os.chmod(str(self.directory), 0o700)
        self.reserve = reserve
        self.db = sqlite3.connect(str(self.directory / 'conversations.sqlite3'))
        self.db.row_factory = sqlite3.Row
        self.db.executescript('''
          PRAGMA journal_mode=WAL;
          PRAGMA synchronous=FULL;
          CREATE TABLE IF NOT EXISTS chats(id TEXT PRIMARY KEY,title TEXT NOT NULL,
             updated INTEGER NOT NULL,saved INTEGER NOT NULL DEFAULT 0);
          CREATE TABLE IF NOT EXISTS messages(seq INTEGER PRIMARY KEY AUTOINCREMENT,
             key TEXT UNIQUE NOT NULL,chat TEXT NOT NULL,role TEXT NOT NULL,text TEXT NOT NULL);
          CREATE INDEX IF NOT EXISTS message_chat ON messages(chat,seq);
          CREATE TABLE IF NOT EXISTS settings(key TEXT PRIMARY KEY,value TEXT NOT NULL);
        ''')
        self.uploads = {}
        self.proposals = {}
        self.active = self._get('active')
        if not self.active or not self.db.execute('SELECT 1 FROM chats WHERE id=?',(self.active,)).fetchone():
            self.new()

    def _get(self,key):
        row = self.db.execute('SELECT value FROM settings WHERE key=?',(key,)).fetchone()
        return row[0] if row else None

    def _set(self,key,value):
        self.db.execute('INSERT OR REPLACE INTO settings VALUES(?,?)',(key,value))

    def _room(self):
        size = sum(p.stat().st_size for p in self.directory.glob('conversations.sqlite3*'))
        if size > MAX_DB or shutil.disk_usage(str(self.directory)).free < self.reserve:
            raise ValueError('Archive full; existing chats are safe')

    def _stamp(self):
        # Embedded clock may reset when power is removed. Preserve library order.
        latest=self.db.execute('SELECT coalesce(max(updated),0) FROM chats').fetchone()[0]
        return max(int(time.time()),latest+1)

    def new(self):
        self._room()
        self.active = uuid.uuid4().hex
        with self.db:
            self.db.execute('INSERT INTO chats VALUES(?,?,?,0)',(self.active,'New conversation',self._stamp()))
            self._set('active',self.active)
        return self.active

    def _id(self,value):
        if not isinstance(value,str) or not re.fullmatch('[0-9a-f]{32}',value):
            raise ValueError('Invalid conversation')
        if not self.db.execute('SELECT 1 FROM chats WHERE id=?',(value,)).fetchone():
            raise ValueError('Conversation unavailable')
        return value

    def _retitle(self,cid):
        if self._get('title_lock:'+cid):return
        from muse_chat_titles import title
        old=self.db.execute('SELECT title FROM chats WHERE id=?',(cid,)).fetchone()
        if old and old[0].startswith('[Temporary display test]'):return
        rows=self.db.execute('SELECT role,text FROM messages WHERE chat=? ORDER BY seq DESC LIMIT 14',(cid,)).fetchall()
        if rows:self.db.execute('UPDATE chats SET title=? WHERE id=?',(title([(r['role'],r['text']) for r in reversed(rows)]),cid))

    def put(self,d):
        key,role,part,offset,finish = (d.get(k) for k in ('key','role','part','offset','finish'))
        if not isinstance(key,str) or not re.fullmatch('[0-9a-f]{24}',key) or role not in ('user','assistant'):
            raise ValueError('Invalid message')
        if not isinstance(part,str) or len(part.encode('utf8'))>256 or '\0' in part or type(offset) is not int or offset<0 or type(finish) is not bool:
            raise ValueError('Invalid message part')
        exists = self.db.execute('SELECT chat FROM messages WHERE key=?',(key,)).fetchone()
        if exists:
            return {'stored':True,'chat':exists[0]}
        # A bounded in-memory assembly never leaves a partial saved message.
        now=time.monotonic()
        self.uploads={k:v for k,v in self.uploads.items() if now-v[3]<180}
        if key not in self.uploads:
            if offset or len(self.uploads)>=12: raise ValueError('Message assembly unavailable')
            self.uploads[key]=[self.active,role,'',now]
        item=self.uploads[key]
        text=item[2]
        raw=text.encode('utf8'); incoming=part.encode('utf8')
        if item[1]!=role or offset>len(raw): raise ValueError('Message order mismatch')
        if offset<len(raw):
            if raw[offset:offset+len(incoming)]!=incoming: raise ValueError('Message retry differs')
        else:
            text+=part
            if len(text.encode('utf8'))>8192: raise ValueError('Message exceeds bound')
            item[2]=text
        item[3]=now
        if finish:
            if offset+len(incoming)!=len(item[2].encode('utf8')): raise ValueError('Incomplete final message')
            self._room()
            with self.db:
                self.db.execute('INSERT INTO messages(key,chat,role,text) VALUES(?,?,?,?)',(key,item[0],role,item[2]))
                self.db.execute('UPDATE chats SET updated=? WHERE id=?',(self._stamp(),item[0]))
                if role=='user':
                    self.db.execute("UPDATE chats SET title=? WHERE id=? AND title='New conversation'",(' '.join(item[2].split())[:54] or 'Conversation',item[0]))
                self._retitle(item[0])
            del self.uploads[key]
        return {'stored':finish,'chat':item[0]}

    def transcript_page(self,cid,page,with_style=False):
        # Walk rows without materializing the whole conversation in RAM.
        rows=self.db.execute('SELECT role,text FROM messages WHERE chat=? ORDER BY seq',(self._id(cid),))
        position=0; pieces=[]; first=True;style=(False,False,0)
        for r in rows:
            part=('' if first else '\n\n')+('You' if r['role']=='user' else 'Muse')+':\n'+r['text'];first=False
            begin=max(0,page*PAGE-position);end=min(len(part),(page+1)*PAGE-position)
            if position<=page*PAGE<position+len(part):
                bits=0;kind=0;prefix=part[:page*PAGE-position]
                for match in re.finditer(r'(?<!\\)(`{1,3}|\*{1,3}|_{1,3}|~~|\n)',prefix):
                    marker=match.group()
                    if marker=='\n':
                        if kind==1:kind=0
                    elif marker[0]=='`':kind=0 if kind else (3 if len(marker)==3 else 1)
                    elif not kind:
                        if marker=='~~':bits^=4
                        elif len(marker)==3:bits^=3
                        elif len(marker)==2:bits^=1
                        else:bits^=2
                if prefix.rsplit('\n',1)[-1].lstrip().startswith('> '):bits|=32
                if kind:bits|=8
                if kind==1:bits|=64
                style=(bool(bits&1),kind==3,bits)
            if end>begin:pieces.append(part[begin:end])
            position+=len(part)
        result=(''.join(pieces),max(1,(position+PAGE-1)//PAGE))
        return result+(style,) if with_style else result

    def context(self,cid,query=''):
        cid=self._id(cid)
        recent=list(self.db.execute('SELECT seq,role,text FROM messages WHERE chat=? ORDER BY seq DESC LIMIT 6',(cid,)))
        # Exact local excerpts, not an invented summary. Prefer recent turns,
        # then older user statements matching meaningful query words.
        tokens=set(re.findall(r'[a-zA-Z]{4,}',query.lower()))-{'what','that','this','have','with','please','tell','about'}
        older=[]
        if tokens:
            terms=sorted(tokens)[:8]
            sql="SELECT seq,role,text FROM messages WHERE chat=? AND role='user' AND ("+' OR '.join('lower(text) LIKE ?' for _ in terms)+') ORDER BY seq DESC LIMIT 200'
            for r in self.db.execute(sql,tuple([cid]+['%'+t+'%' for t in terms])):
                score=len(tokens & set(re.findall(r'[a-zA-Z]{4,}',r['text'].lower())))
                if score: older.append((score,r))
        selected={r['seq']:r for r in recent}
        for _,r in sorted(older,key=lambda p:p[0],reverse=True)[:3]:selected.setdefault(r['seq'],r)
        # Allocate the latest turns first; strictly bounded UTF-8 payload.
        chosen=[]; budget=4300
        for r in sorted(selected.values(),key=lambda r:(r['seq'] not in {x['seq'] for x in recent},-r['seq'])):
            part=('You' if r['role']=='user' else 'Muse')+': '+r['text']
            raw=part.encode('utf8')[:min(650 if r['seq'] in {x['seq'] for x in recent} else 950,budget)]
            part=raw.decode('utf8','ignore')
            if len(part)>8:chosen.append((r['seq'],part));budget-=len(part.encode('utf8'))+2
            if budget<100:break
        return '\n\n'.join(p for _,p in sorted(chosen))

    def handle(self,d):
        op=d.get('op')
        if op=='diagnostic':
            return {'ok':self.db.execute('PRAGMA quick_check').fetchone()[0]=='ok',
                    'sqlite':sqlite3.sqlite_version,'free_bytes':shutil.disk_usage(str(self.directory)).free,
                    'audio_stored':False,'automatic_draft':True}
        if op=='put':return self.put(d)
        if op=='new':
            if self.uploads:raise ValueError('Wait for pending messages')
            return {'chat':self.new()}
        if op=='save':
            cid=self._id(d.get('chat') or self.active)
            with self.db:self.db.execute('UPDATE chats SET saved=1 WHERE id=?',(cid,))
            return {'chat':cid,'saved':True}
        if op=='list':
            offset=d.get('offset',0)
            if type(offset) is not int or not 0<=offset<=100000:raise ValueError('Invalid library page')
            total=self.db.execute('SELECT count(*) FROM chats WHERE EXISTS(SELECT 1 FROM messages WHERE chat=chats.id)').fetchone()[0]
            if offset>=total:offset=max(0,((total-1)//5)*5)
            rows=self.db.execute('SELECT id,title,saved FROM chats WHERE EXISTS(SELECT 1 FROM messages WHERE chat=chats.id) ORDER BY updated DESC,id LIMIT 6 OFFSET ?',(offset,)).fetchall()
            with self.db:
                for row in rows[:5]:self._retitle(row['id'])
            items=[]
            for row in rows[:5]:
                item=dict(row);item['title']=self.db.execute('SELECT title FROM chats WHERE id=?',(row['id'],)).fetchone()[0];items.append(item)
            return {'items':items,'more':len(rows)>5,'active':self.active,'offset':offset}
        if op=='delete':
            if d.get('confirm') is not True:raise ValueError('Confirm deletion first')
            value=d.get('chat')
            if not isinstance(value,str) or not re.fullmatch('[0-9a-f]{32}',value):raise ValueError('Invalid conversation')
            previous=self._get('deleted:'+value)
            if previous:return {'chat':value,'deleted':True,'active_deleted':previous=='active','replacement':self.active}
            cid=self._id(value)
            if self.uploads:raise ValueError('Wait for pending messages')
            active=cid==self.active;replacement=uuid.uuid4().hex if active else self.active
            with self.db:
                if active:
                    self.db.execute('INSERT INTO chats VALUES(?,?,?,0)',(replacement,'New conversation',self._stamp()))
                    self._set('active',replacement)
                self.db.execute('DELETE FROM messages WHERE chat=?',(cid,))
                self.db.execute('DELETE FROM chats WHERE id=?',(cid,))
                self.db.execute('DELETE FROM settings WHERE key=?',('title_lock:'+cid,))
                self._set('deleted:'+cid,'active' if active else 'other')
            self.active=replacement
            self.proposals={k:v for k,v in self.proposals.items() if v[0]!=cid}
            return {'chat':cid,'deleted':True,'active_deleted':active,'replacement':replacement}
        cid=self._id(d.get('chat') or self.active)
        if op=='suggest':
            from muse_chat_titles import suggestions
            variant=d.get('page',0)
            if type(variant) is not int or not 0<=variant<=100000:raise ValueError('Invalid title choice')
            rows=self.db.execute('SELECT role,text FROM messages WHERE chat=? ORDER BY seq DESC LIMIT 14',(cid,)).fetchall()
            choices=suggestions([(r['role'],r['text']) for r in reversed(rows)])
            current=self.db.execute('SELECT title FROM chats WHERE id=?',(cid,)).fetchone()[0]
            alternatives=[t for t in choices if t.casefold()!=current.casefold()] or choices
            selected=variant%len(alternatives);now=time.monotonic()
            self.proposals={k:v for k,v in self.proposals.items() if now-v[2]<600}
            while len(self.proposals)>=16:del self.proposals[next(iter(self.proposals))]
            token=uuid.uuid4().hex[:24];self.proposals[token]=(cid,alternatives[selected],now)
            return {'chat':cid,'title':alternatives[selected],'token':token,'variant':variant,'choices':len(alternatives),'local':True}
        if op=='rename':
            proposal=self.proposals.get(d.get('token')) if isinstance(d.get('token'),str) else None
            if not proposal or proposal[0]!=cid or time.monotonic()-proposal[2]>=600:raise ValueError('Title preview expired; try another')
            with self.db:
                self.db.execute('UPDATE chats SET title=? WHERE id=?',(proposal[1],cid))
                self._set('title_lock:'+cid,'1')
            return {'chat':cid,'renamed':True,'title':proposal[1]}
        if op=='resume':
            if self.uploads:raise ValueError('Wait for pending messages')
            self.active=cid
            with self.db:self._set('active',cid)
            return {'chat':cid,'resumed':True}
        if op in ('page','context'):
            page=d.get('page',0);offset=d.get('offset',0)
            if type(page) is not int or page<0 or type(offset) is not int or not 0<=offset<=8192:raise ValueError('Invalid page')
            if op=='context':section=self.context(cid,d.get('query','')[:2048]);pages=1;style=(False,False,0)
            else:section,pages,style=self.transcript_page(cid,page,with_style=True)
            if page>=pages:raise ValueError('Page unavailable')
            part=section[offset:offset+180]
            return {'chat':cid,'text':part,'next':offset+len(part),'more':offset+len(part)<len(section),'pages':pages,'page':page,'bold_start':style[0],'code_start':style[1],'style_start':style[2]}
        raise ValueError('Unknown archive operation')

    def close(self):
        self.db.close()
