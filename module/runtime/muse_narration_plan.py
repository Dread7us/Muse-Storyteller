"""Plan complete spoken sentences/list items; render limits never define playback pauses."""
import bisect,re
MAX_RENDER_BYTES=640
MAX_UNIT_PCM=32*1024*1024
ABBREVIATIONS={'mr','mrs','ms','dr','prof','sr','jr','st','vs','etc','e.g','i.e','fig','no','a.m','p.m'}
BULLET=re.compile(r'^\s*(?:[-*+•]|\d+[.)]|\[[ xX]\])\s+')
def blocks(raw):
 result=[];offset=0;start=None;end=0;listed=False
 for line in raw.splitlines(keepends=True):
  finish=offset+len(line.encode('utf8'));blank=not line.strip();item=bool(BULLET.match(line))
  if blank or item:
   if start is not None:result.append((start,end,listed));start=None
  if not blank:
   if start is None:start=offset;listed=item
   end=finish
  offset=finish
 if start is not None:result.append((start,end,listed))
 return result

def sentences(text):
 start=0
 for m in re.finditer(r'[.!?]+["\u201d\u2019\')\]]*(?=\s|$)',text):
  mark=m.start();prefix=text[:mark+1];word=prefix.split()[-1] if prefix.split() else ''
  token=word.lstrip('\"\'(').rstrip('.').lower()
  if m.group().startswith('.') and (token in ABBREVIATIONS or re.fullmatch(r'(?:[A-Za-z]\.){2,}',word) or re.fullmatch(r'[A-Z]\.',word)):continue
  end=m.end();unit=text[start:end].strip()
  if unit:yield unit
  start=end
 tail=text[start:].strip()
 if tail:yield tail

def playback_units(raw,spoken,words):
 layout=blocks(raw)
 if not layout:return list(sentences(spoken))
 starts=[b[0] for b in layout];groups=[]
 for w in words:
  i=max(0,bisect.bisect_right(starts,w[2])-1)
  if groups and groups[-1][0]==i:groups[-1]=(i,groups[-1][1],w[1])
  else:groups.append((i,w[0],w[1]))
 result=[]
 for i,a,b in groups:
  text=spoken[a:b]
  result.extend([text] if layout[i][2] else sentences(text))
 if ' '.join(result)!=spoken:raise ValueError('Narration planner would change spoken text')
 return result

def render_parts(unit,limit=MAX_RENDER_BYTES):
 remaining=unit
 while remaining:
  end=0;size=0
  for char in remaining:
   n=len(char.encode('utf8'))
   if size+n>limit:break
   size+=n;end+=1
  if end<len(remaining):
   split=remaining.rfind(' ',0,end+1)
   if split>0:end=split
  part=remaining[:end]
  if not part:raise ValueError('Cannot render narration text')
  yield part;remaining=remaining[end:].lstrip()
