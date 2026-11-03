"""Small offline topic titles derived from existing conversation text."""
import collections
import html
import re
import unicodedata

STOP=set('a an the and or but for of to in on at from with without into about as is are was were be been being i me my mine we us our you your yours it its this that these those they them their he she his her please can could would should will may might do does did have has had get got make made tell give show want need know think thanks thank okay ok yes no new another one more really just also very much some any all what how when where why which who something anything everything let lets now next again continue story stories say said conversation question reply answer'.split())
GENERIC=set('help information things thing great good nice sure certainly happy here there today time use using used way ways first second third last like well little'.split())

def clean(text):
    text=re.sub(r'<[^>]{0,200}>',' ',html.unescape(text))
    text=re.sub(r'!?\[([^\]]+)\]\([^)]*\)',r'\1',text)
    text=re.sub(r'[`*_#~]','',text)
    text=''.join(c if unicodedata.category(c)[0] in 'LN' or c in " '-,:&" else ' ' for c in text)
    return ' '.join(text.split())

def title(messages):
    messages=list(messages)[-14:]
    if not messages:return 'New conversation'
    scores=collections.Counter();display={};heads=[]
    for index,(role,text) in enumerate(messages):
        weight=(2.8 if role=='user' else 1.0)*(0.7+0.6*(index+1)/len(messages))
        words=re.findall(r"[A-Za-z][A-Za-z'-]{2,}",clean(text))
        seen=collections.Counter(w.lower() for w in words)
        for word in words:display.setdefault(word.lower(),word)
        for word,count in seen.items():
            if word not in STOP|GENERIC:scores[word]+=weight*min(count,3)
        if role=='assistant':
            for line in text.splitlines()[:8]:
                stripped=line.strip()
                if stripped.startswith('#') or (stripped.startswith('**') and stripped.endswith('**')):
                    heading=clean(stripped)
                    tokens=[w.lower() for w in re.findall(r'[A-Za-z]{3,}',heading) if w.lower() not in STOP|GENERIC]
                    if 8<=len(heading)<=54 and len(tokens)>=2 and not re.match(r'(?i)(tip|step|part|chapter|sure|here|welcome)\b',heading):heads.append((heading,tokens))
    if not scores:return 'A conversation with Muse'
    # A naturally supplied story/topic heading is preferable to keyword fragments.
    if heads:
        heading,_=max(heads,key=lambda item:sum(scores[t] for t in item[1])/max(1,len(item[1]))**.4)
        return heading[:54]
    ordered=[w for w,_ in scores.most_common(8)]
    # Remove trivial inflection duplicates so a library row stays meaningful.
    chosen=[]
    for word in ordered:
        if any(word.rstrip('s')==old.rstrip('s') for old in chosen):continue
        chosen.append(word)
        if len(chosen)==3:break
    def pretty(word):
        original=display.get(word,word)
        return original if original.isupper() else original.capitalize()
    parts=[pretty(w) for w in chosen]
    result=parts[0] if len(parts)==1 else ' & '.join(parts) if len(parts)==2 else parts[0]+', '+parts[1]+' & '+parts[2]
    raw=result.encode('utf8')[:80]
    return raw.decode('utf8','ignore').strip(' ,&') or 'A conversation with Muse'

def suggestions(messages):
    """Bounded extractive choices only: no network, inference, or invented facts."""
    messages=list(messages)[-14:]; choices=[]
    def add(value):
        value=clean(value).strip(' -,:&')
        if len(value)<5:return
        if len(value)>54:value=value[:55].rsplit(' ',1)[0]
        value=value.encode('utf8')[:80].decode('utf8','ignore').strip()
        if value and value.casefold() not in {x.casefold() for x in choices}:choices.append(value)
    add(title(messages))
    for role,text in reversed(messages):
        if role!='assistant':continue
        for line in text.splitlines()[:8]:
            line=line.strip()
            if line.startswith('#') or (line.startswith('**') and line.endswith('**')):
                if not re.match(r'(?i)(tip|step|part|chapter|sure|here|welcome)\b',clean(line)):add(line)
    for role,text in reversed(messages):
        if role!='user':continue
        sentence=re.split(r'[.!?\n]',text[:700])[0]
        sentence=re.sub(r"(?i)^(?:please\s+)?(?:can you\s+|could you\s+)?(?:tell me\s+|explain\s+|help me (?:with\s+)?|show me\s+|i (?:want|need)\s+)", '',sentence).strip()
        sentence=re.sub(r'(?i)^(?:a |an )?(?:brand[- ]new |new |another )?(?:story |one )?(?:about|on)\s+', '',sentence)
        words=re.findall(r'[A-Za-z]{3,}',sentence)
        if sum(w.lower() not in STOP|GENERIC for w in words)>=2:add(sentence[:1].upper()+sentence[1:])
    for start in range(max(0,len(messages)-6),len(messages)):
        add(title(messages[start:]))
    return choices[:8] or ['A conversation with Muse']
