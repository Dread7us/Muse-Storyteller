"""Conservative, offline, separate transcript review. Raw text is never modified."""
import difflib,json,os,re,threading,time
from pathlib import Path
# Explicit vocabulary correction supplied by the owner, not a model inference.
LEARNED=[(r'\btheir blue land turn\b','the blue lantern')]
class ReviewDeferred(Exception):pass

def seed(text):
 for heard,said in LEARNED:text=re.sub(heard,said,text,flags=re.I)
 return text

def words(text):return re.findall(r"[\w']+",text.lower())
def acceptable(raw,out):
 a,b=words(raw),words(out)
 if not a or not b or len(out)>max(80,len(raw)*1.5) or len(b)<len(a)*.8 or len(b)>len(a)*1.3:return False
 if re.findall(r'\d+(?:[.,]\d+)*',raw)!=re.findall(r'\d+(?:[.,]\d+)*',out):return False
 neg=lambda w:[x for x in w if x in ('not','no','never','cannot') or x.endswith("n't")]
 if neg(a)!=neg(b):return False
 numbers=set('zero one two three four five six seven eight nine ten eleven twelve thirteen fourteen fifteen sixteen seventeen eighteen nineteen twenty thirty forty fifty sixty seventy eighty ninety hundred thousand million billion'.split())
 if [x for x in a if x in numbers]!=[x for x in b if x in numbers]:return False
 if any(x in out.lower() for x in ('corrected sentence:','transcript:','assistant:','as an ai','<|','```','->')):return False
 return difflib.SequenceMatcher(None,a,b,autojunk=False).ratio()>=.72

def chunks(text,limit=24):
 # Bounded context; preserve all words and order, never silently truncate.
 tokens=text.split();current=[];size=0
 for token in tokens:
  if current and (len(current)>=limit or size+len(token)>180):yield ' '.join(current);current=[];size=0
  current.append(token);size+=len(token)+1
 if current:yield ' '.join(current)

def atomic(path,obj):
 tmp=path.with_name(path.name+'.new')
 with tmp.open('w',encoding='utf8') as f:os.chmod(tmp,0o600);json.dump(obj,f);f.flush();os.fsync(f.fileno())
 os.replace(tmp,path)

def review(folder,idle,client_factory=None):
 from muse_local_cleaner import LocalCleaner
 client_factory=client_factory or LocalCleaner
 source=folder/'transcript.txt';target=folder/'reviewed.txt';progress=folder/'review.json';partial=folder/'reviewed.txt.part'
 fingerprint=[source.stat().st_size,source.stat().st_mtime_ns]
 previous=status(folder)
 resume=previous.get('source')==fingerprint and partial.exists() and partial.stat().st_size>=previous.get('offset',0)
 report=previous if resume else dict(chunks=0,rejected=0,learned=0,offset=0,source=fingerprint)
 with source.open(encoding='utf8') as src:
  total=sum(sum(1 for _ in chunks(match[2])) for line in src if (match:=re.match(r'^(\[[0-9:]+(?: partial)?\]) (.*)$',line.rstrip('\n'))))
 report['total']=total
 report.update(status='running',model='qwen2.5-0.5B-prefill-20e',raw_preserved=True)
 def check():
  if not idle():raise ReviewDeferred()
 client=None
 try:
  check();atomic(progress,report);client=client_factory(check)
  with source.open(encoding='utf8') as src,partial.open('r+b' if resume else 'w+b') as dst:
   os.chmod(partial,0o600)
   if resume:dst.truncate(report['offset']);dst.seek(report['offset'])
   else:dst.write(b'AI-reviewed transcript - verify against Original.\nOffline; original words and audio are kept.\n\n')
   completed=report['chunks'];unit=0
   for line in src:
    match=re.match(r'^(\[[0-9:]+(?: partial)?\]) (.*)$',line.rstrip('\n'))
    if not match:continue
    parts=list(chunks(match[2]))
    for index,raw in enumerate(parts):
     unit+=1
     if unit<=completed:continue
     check();prepared=seed(raw);candidate=client.correct(prepared)
     rejected=not acceptable(prepared,candidate)
     if rejected:candidate=prepared
     prefix=match[1]+' ' if index==0 else ' '
     suffix='\n' if index+1==len(parts) else ''
     dst.write((prefix+candidate+suffix).encode('utf8'));dst.flush();os.fsync(dst.fileno())
     report['chunks']+=1;report['learned']+=int(prepared!=raw);report['rejected']+=int(rejected);report['offset']=dst.tell();atomic(progress,report)
   dst.flush();os.fsync(dst.fileno())
  check();os.replace(partial,target);report['status']='complete';atomic(progress,report)
 except ReviewDeferred:
  report['status']='deferred';atomic(progress,report)
 except Exception:
  report['status']='error';atomic(progress,report)
 finally:
  if client:
   try:client.close()
   except Exception:pass

def status(folder):
 try:return json.loads((folder/'review.json').read_text())
 except (OSError,ValueError):return {}
