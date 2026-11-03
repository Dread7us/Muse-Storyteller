"""Speech-only formatting and pronunciation, mapped to original UTF-8 offsets."""
import datetime
import html
import json
import re
from pathlib import Path

RULE_PATH=Path('/var/lib/muse-core2-bridge/pronunciation-rules.json')
DEFAULT={'version':1,'identifier_prefixes':['SCP'],'plain_numbers':'natural','words':{}}
DIGITS='zero one two three four five six seven eight nine'.split()
SMALL='zero one two three four five six seven eight nine ten eleven twelve thirteen fourteen fifteen sixteen seventeen eighteen nineteen'.split()
TENS='zero ten twenty thirty forty fifty sixty seventy eighty ninety'.split()
MONTHS='January February March April May June July August September October November December'.split()

def cardinal(n):
    if n<0:return 'minus '+cardinal(-n)
    if n<20:return SMALL[n]
    if n<100:return TENS[n//10]+(' '+SMALL[n%10] if n%10 else '')
    if n<1000:return SMALL[n//100]+' hundred'+(' '+cardinal(n%100) if n%100 else '')
    for scale,name in ((10**12,'trillion'),(10**9,'billion'),(10**6,'million'),(1000,'thousand')):
        if n>=scale:return cardinal(n//scale)+' '+name+(' '+cardinal(n%scale) if n%scale else '')
    return str(n)

def ordinal(n):
    special={0:'zeroth',1:'first',2:'second',3:'third',5:'fifth',8:'eighth',9:'ninth',12:'twelfth',20:'twentieth',30:'thirtieth',40:'fortieth',50:'fiftieth',60:'sixtieth',70:'seventieth',80:'eightieth',90:'ninetieth'}
    if n in special:return special[n]
    if n<20:return cardinal(n)+'th'
    if n<100:return cardinal(n//10*10)+' '+ordinal(n%10)
    for scale,name in ((10**12,'trillion'),(10**9,'billion'),(10**6,'million'),(1000,'thousand'),(100,'hundred')):
        if n>=scale:return cardinal(n//scale)+' '+(name+' '+ordinal(n%scale) if n%scale else name+'th')

NUM=r'(?:[0-9]{1,3}(?:,[0-9]{3})+|[0-9]{1,15})(?:\.[0-9]{1,9})?'
CURRENCY=r'(?:US\$|CA\$|CAD\s*\$?|AU\$|AUD\s*\$?|NZ\$|NZD\s*\$?|C\$|A\$|USD\s*\$?|EUR\s*|GBP\s*|JPY\s*|INR\s*|[$£€¥₹])'
END_NUMBER=r'(?![\w]|[.,][0-9])'

def digit_replacement(obj,m,group=0):
    words=[];mapping=[]
    for i,ch in enumerate(m.group(group)):
        word='point' if ch=='.' else DIGITS[int(ch)]
        a,b,_=obj.map[m.start(group)+i]
        if words:words.append(' ');mapping.append((a,b,True))
        words.append(word);mapping.extend([(a,b,True)]*len(word))
    return ''.join(words),mapping

def number_words(raw):
    whole,_,fraction=raw.replace(',','').partition('.')
    if len(whole)>15:return None
    return cardinal(int(whole))+(' point '+' '.join(DIGITS[int(x)] for x in fraction) if fraction else '')

def money_words(symbol,raw,scale=None):
    symbol=''.join(symbol.upper().split())
    major,singular,minor,one_minor='dollars','dollar','cents','cent'
    prefix=''
    if symbol in ('€','EUR'):major,singular='euros','euro'
    elif symbol in ('£','GBP'):major,singular,minor,one_minor='pounds','pound','pence','penny'
    elif symbol in ('¥','JPY'):major=singular='yen';minor=one_minor=None
    elif symbol in ('₹','INR'):major,singular,minor,one_minor='rupees','rupee','paise','paisa'
    elif symbol.startswith(('CAD','CA$','C$')):prefix='Canadian '
    elif symbol.startswith(('AUD','AU$','A$')):prefix='Australian '
    elif symbol.startswith(('NZD','NZ$')):prefix='New Zealand '
    elif symbol.startswith(('USD','US$')):prefix='U S '
    value=number_words(raw)
    if value is None:return None
    if scale:
        scale={'k':'thousand','m':'million','b':'billion'}.get(scale.lower(),scale.lower())
        return value+' '+scale+' '+prefix+major
    whole,_,fraction=raw.replace(',','').partition('.');n=int(whole)
    if fraction and len(fraction)<=2 and minor:
        cents=int(fraction.ljust(2,'0'));parts=[]
        if n:parts.append(cardinal(n)+' '+prefix+(singular if n==1 else major))
        if cents:parts.append(cardinal(cents)+' '+(one_minor if cents==1 else minor))
        return ' and '.join(parts) or 'zero '+prefix+major
    return value+' '+prefix+(singular if n==1 and not fraction else major)

def contextual_numbers(obj,sub):
    # Structural identifiers precede clocks, quantities and currency rules.
    def literal(m):
        words=[];mapping=[]
        for i,ch in enumerate(m.group()):
            word=DIGITS[int(ch)] if ch.isascii() and ch.isdigit() else 'dot' if ch=='.' else 'plus' if ch=='+' else ch if ch.isascii() and ch.isalpha() else ''
            if not word:continue
            a,b,_=obj.map[m.start()+i]
            if words:words.append(' ');mapping.append((a,b,True))
            words.append(word);mapping.extend([(a,b,True)]*len(word))
        return ''.join(words),mapping
    sub(r'(?<![\w.])(?:[0-9]{1,3}\.){3}[0-9]{1,3}(?![\w.])',lambda m:literal(m) if all(int(x)<=255 for x in m.group().split('.')) else None,protect=True)
    sub(r'(?<!\w)(?:\+?1[ .-]?)?(?:\([0-9]{3}\)|[0-9]{3})[ .-]?[0-9]{3}[ .-][0-9]{4}(?!\d)',literal,protect=True)
    sub(r'\b[0-9]{5}[-–][0-9]{4}\b',literal,protect=True)
    sub(r'\b(?:ext\.?|extension)\s*([0-9]{1,8})\b',lambda m:'extension '+' '.join(DIGITS[int(x)] for x in m.group(1)),protect=True,flags=re.I)
    def version(m):
        return m.group(1)+' '+' point '.join((' '.join(DIGITS[int(c)] for c in part) if len(part)>1 and part.startswith('0') else cardinal(int(part))) for part in m.group(2).split('.'))
    sub(r'\b(version\s+|v)([0-9]{1,5}(?:\.[0-9]{1,5}){1,4})(?![\w.])',version,protect=True,flags=re.I)
    def tagged(m):
        start=m.start(1);prefix=obj.text[m.start():start];spoken,mapping=digit_replacement(obj,m,1)
        return prefix+spoken,[(a,b,True) for a,b,_ in obj.map[m.start():start]]+mapping
    sub(r'\b(?:ZIP(?:\s+code)?|postal\s+code|PIN|code|ID|serial(?:\s+number)?|ticket)\s*[:#]?\s+([0-9]{1,15})(?!\w|[.,][0-9])',tagged,protect=True,flags=re.I)
    sub(r'\b(GPT|USB)[- ]([0-9]+(?:\.[0-9]+)+)(?![\w.])',lambda m:' '.join(m.group(1))+' '+number_words(m.group(2)),protect=True)
    sub(r'\b[A-Z]{1,12}[-‐‑][0-9]{1,15}(?:-[A-Z])?(?![\w.])',literal,protect=True)
    # Bounded adjacent score/range pairs exclude dates, phones and ID chains.
    def pair(m):
        left,right=m.group(1),m.group(2);tail=obj.text[m.end():m.end()+40]
        before=obj.text[max(0,m.start()-50):m.start()]
        if re.match(r'\s*=',tail):join='minus'
        elif re.match(r'\s+(?:overall|in\s+.+?\s+play|record)\b',tail,re.I) or re.search(r'\b(?:record(?:\s+of)?|went|finished)\s*$',before,re.I):join='and'
        else:join='to'
        words=[];mapped=[]
        for text,a,b in [(number_words(left),m.start(1),m.end(1)),(' '+join+' ',m.end(1),m.start(2)),(number_words(right),m.start(2),m.end(2))]:
            span=(obj.map[a][0],obj.map[b-1][1],True);words.append(text);mapped.extend([span]*len(text))
        return ''.join(words),mapped
    sub(r'\bNo\.\s*([0-9]{1,3})\b',lambda m:'number '+cardinal(int(m.group(1))),protect=True,flags=re.I)

    sub(r'\b(?:SCP|LLM|GPT|USB|FBI|CPU|GPU|API|USA|AI|UTC)\b',lambda m:' '.join(m.group()),protect=True)
    amount=CURRENCY+r'\s*'+NUM
    def price_range(m):
        left=money_words(m.group(1),m.group(2));right=money_words(m.group(3) or m.group(1),m.group(4))
        if not left or not right:return None
        end=m.start(3) if m.group(3) else m.start(4)
        spans=[(m.start(1),m.end(2),left),(m.end(2),end,' to '),(end,m.end(4),right)]
        text='';mapping=[]
        for start,stop,words in spans:
            a=obj.map[start][0];b=obj.map[max(start,stop-1)][1];text+=words;mapping.extend([(a,b,True)]*len(words))
        return text,mapping
    # A price range remains a range; no separator is pronounced as minus.
    sub(r'(?<!\w)('+CURRENCY+r')\s*('+NUM+r')\s*[-–—]\s*('+CURRENCY+r')?\s*('+NUM+r')'+END_NUMBER,
        price_range,protect=True,flags=re.I)
    sub(r'\(\s*('+CURRENCY+r')\s*('+NUM+r')\s*\)',lambda m:'negative '+money_words(m.group(1),m.group(2)) if money_words(m.group(1),m.group(2)) else None,protect=True,flags=re.I)
    def money(m):
        spoken=money_words(m.group(2),m.group(3),m.group(4))
        return ('negative ' if m.group(1) else '')+spoken if spoken else None
    sub(r'(?<!\w)([-−])?('+CURRENCY+r')\s*('+NUM+r')(?:\s*(thousand|million|billion|trillion|[kmb])(?!\w))?'+END_NUMBER,money,protect=True,flags=re.I)
    sub(r'(?<![\w.])('+NUM+r')\s+(USD|CAD|AUD|NZD|EUR|GBP|JPY|INR)\b',lambda m:money_words(m.group(2),m.group(1)),protect=True)
    sub(r'(?<![\w.,/\-–])('+NUM+r')[-–]('+NUM+r')(?![\w/\-–]|[.,][0-9])',pair,protect=True)
    sub(r'(?<![\w.])('+NUM+r')\s*%',lambda m:number_words(m.group(1))+' percent' if number_words(m.group(1)) else None,protect=True)
    def celsius(m):
        from decimal import Decimal
        value=Decimal(m.group(1).replace('−','-'))*Decimal('1.8')+32
        rounded=value.quantize(Decimal('.01'));raw=format(abs(rounded),'f').rstrip('0').rstrip('.') if '.' in format(abs(rounded),'f') else str(abs(rounded))
        return ('approximately ' if value!=rounded else '')+('negative ' if rounded<0 else '')+number_words(raw)+' degrees Fahrenheit'
    sub(r'(?<![\w.])([-−]?[0-9]{1,6}(?:\.[0-9]{1,6})?)\s*(?:°\s*C\b|degrees?\s+Celsius\b)',celsius,protect=True,flags=re.I)
    metric={'km/h':('0.621371192','miles per hour'),'kph':('0.621371192','miles per hour'),'km':('0.621371192','miles'),'kilometers':('0.621371192','miles'),'kilometer':('0.621371192','miles'),'cm':('0.393700787','inches'),'centimeters':('0.393700787','inches'),'mm':('0.0393700787','inches'),'meters':('3.280839895','feet'),'meter':('3.280839895','feet'),'kg':('2.204622622','pounds'),'kilograms':('2.204622622','pounds'),'g':('0.0352739619','ounces'),'grams':('0.0352739619','ounces'),'liters':('33.8140227','U S fluid ounces'),'liter':('33.8140227','U S fluid ounces'),'ml':('0.0338140227','U S fluid ounces')}
    def customary(m):
        from decimal import Decimal
        factor,unit=metric[m.group(2).lower()];value=Decimal(m.group(1).replace(',','').replace('−','-'))*Decimal(factor);value=value.quantize(Decimal('.01'))
        raw=format(abs(value),'f').rstrip('0').rstrip('.');spoken=number_words(raw)
        if not spoken:return None
        return 'approximately '+('negative ' if value<0 else '')+spoken+' '+unit
    sub(r'(?<![\w.])([-−]?'+NUM+r')\s*('+'|'.join(re.escape(x) for x in sorted(metric,key=len,reverse=True))+r')\b',customary,protect=True,flags=re.I)
    aliases={'ft':'feet','lb':'pounds','lbs':'pounds','oz':'ounces','fl oz':'fluid ounces','mph':'miles per hour','sq ft':'square feet','cu ft':'cubic feet','hr':'hours','hrs':'hours','min':'minutes','mins':'minutes','sec':'seconds','secs':'seconds','km':'kilometers','cm':'centimeters','kg':'kilograms'}
    unit_pattern='|'.join(re.escape(k) for k in sorted(aliases,key=len,reverse=True))
    sub(r'\b([0-9]{1,3})\s*ft\s*([0-9]{1,2})\s*in\b',lambda m:cardinal(int(m.group(1)))+' feet '+cardinal(int(m.group(2)))+' inches',protect=True,flags=re.I)
    sub(r'(?<![\w.])('+NUM+r')\s*('+unit_pattern+r')\b',lambda m:number_words(m.group(1))+' '+aliases[m.group(2).lower()] if number_words(m.group(1)) else None,protect=True,flags=re.I)
    sub(r'\b([0-9]{1,7})(?:st|nd|rd|th)\b',lambda m:ordinal(int(m.group(1))),protect=True,flags=re.I)
    sub(r'\b(chapter|page|part|room|area|route|Windows|iPhone)\s+([0-9]{1,7})\b',lambda m:m.group(1)+' '+cardinal(int(m.group(2))),protect=True,flags=re.I)
    sub(r'\b('+NUM+r')(?:\s+|[-‑])+(items?|people|days?|weeks?|months?|years?|dollars?|cents?|euros?|pence|yen|rupees?|chapters?|pages?|yards?|points?|touchdowns?|wins?|losses?)\b',lambda m:number_words(m.group(1))+' '+m.group(2) if number_words(m.group(1)) else None,protect=True,flags=re.I)

def year(n):
    if 2000<=n<2010:return 'two thousand'+(' '+cardinal(n-2000) if n>2000 else '')
    if n%100==0:return cardinal(n//100)+' hundred'
    return cardinal(n//100)+' '+('oh '+cardinal(n%100) if n%100<10 else cardinal(n%100))

def rules():
    try:
        if RULE_PATH.stat().st_size>16384:return DEFAULT
        data=json.loads(RULE_PATH.read_text())
        prefixes=data.get('identifier_prefixes',['SCP']);words=data.get('words',{})
        if not isinstance(prefixes,list) or len(prefixes)>16 or any(not isinstance(p,str) or not re.fullmatch('[A-Za-z]{2,12}',p) for p in prefixes):return DEFAULT
        if not isinstance(words,dict) or len(words)>64:return DEFAULT
        if any(not isinstance(k,str) or not re.fullmatch(r"[A-Za-z][A-Za-z'-]{0,47}",k) or not isinstance(v,str) or not re.fullmatch(r"[A-Za-z ,'-]{1,96}",v) for k,v in words.items()):return DEFAULT
        return {'identifier_prefixes':prefixes,'words':words,'plain_numbers':data.get('plain_numbers','natural')}
    except (OSError,ValueError,TypeError):return DEFAULT

class SpeechText:
    def __init__(self,raw,config=None):
        self.text=raw;self.map=[];offset=0
        for char in raw:
            size=len(char.encode('utf8'));self.map.append((offset,offset+size,False));offset+=size
        self.config=rules() if config is None else config
        # Protected replacements never pass through the later generic number rule.
        def sub(pattern,fn,protect=False,group=None,flags=0):
            old=self.text;oldmap=self.map;pieces=[];mapped=[];at=0
            for m in re.finditer(pattern,old,flags):
                if any(p[2] for p in oldmap[m.start():m.end()]):continue
                replacement=fn(m) if callable(fn) else fn
                if replacement is None:continue
                replacement_map=None
                if isinstance(replacement,tuple):replacement,replacement_map=replacement
                pieces.append(old[at:m.start()]);mapped.extend(oldmap[at:m.start()]);pieces.append(replacement)
                if replacement_map is not None:mapped.extend(replacement_map)
                elif group is not None:mapped.extend(oldmap[m.start(group):m.end(group)])
                elif replacement:
                    span=(oldmap[m.start()][0],oldmap[m.end()-1][1],protect)
                    mapped.extend([span]*len(replacement))
                at=m.end()
            pieces.append(old[at:]);mapped.extend(oldmap[at:]);self.text=''.join(pieces);self.map=mapped
        sub(r'<(script|style)\b[^>]*>.*?</\1\s*>',' ',flags=re.I|re.S)
        sub(r'<[^>]{0,256}>',' ')
        sub(r'&(?:#[xX]?[0-9a-fA-F]+|[A-Za-z]+);',lambda m:html.unescape(m.group()))
        sub(r'!?\[([^\]]+)\]\([^)]*\)',lambda m:m.group(1),group=1)
        sub(r'(?m)^[ \t]*(?:[-*_][ \t]*){3,}$',' ')
        sub(r'(?m)^\s*```[^\n]*',' ')
        sub(r'(?m)^[ \t]*[-*+]\s+',' ')
        sub(r'(?m)^[ \t]*[\u2022\u25cf\u25e6\u2023]\s*',' ')
        sub(r'(?m)^[ \t]*\[([xX ])\]\s*',lambda m:'checked ' if m.group(1).lower()=='x' else 'unchecked ')
        sub(r'(?m)^[ \t]*>\s?',' ')
        sub(r'(?<=\d)\*(?=\d)',' times ')
        sub(r'[*_`~#]','')
        sub(r'[\U0001f000-\U0001ffff\u2600-\u27ff\ufe0e\ufe0f\u200d]',' ')
        prefixes=self.config.get('identifier_prefixes',['SCP'])
        if prefixes:
            pattern=r'\b('+'|'.join(re.escape(p) for p in prefixes)+r')\s*[-\u2010-\u2015]?\s*([0-9]{1,12})(?:-([A-Za-z]))?\b'
            def identifier(m):
                chunks=[];mapping=[]
                for group in (1,2,3):
                    if m.group(group) is None:continue
                    for index,char in enumerate(m.group(group)):
                        word=DIGITS[int(char)] if group==2 else char.upper()
                        a,b,_=self.map[m.start(group)+index]
                        if chunks:chunks.append(' ');mapping.append((a,b,True))
                        chunks.append(word);mapping.extend([(a,b,True)]*len(word))
                return ''.join(chunks),mapping
            sub(pattern,identifier,protect=True,flags=re.I)
        contextual_numbers(self,sub)
        sub(r'(?<![\w])[-−](?=[0-9])','minus ')
        def date_words(m,iso=False):
            a,b,c=map(int,m.groups())
            y,month,day=(a,b,c) if iso else (c,a,b)
            try:datetime.date(y,month,day)
            except ValueError:return None
            return MONTHS[month-1]+' '+ordinal(day)+' '+year(y)
        sub(r'\b([12][0-9]{3})[-/]([01]?[0-9])[-/]([0-3]?[0-9])\b',lambda m:date_words(m,True),protect=True)
        sub(r'\b([01]?[0-9])/([0-3]?[0-9])/([12][0-9]{3})\b',date_words,protect=True)
        month_pattern='|'.join(MONTHS+[m[:3]+r'\.?' for m in MONTHS if len(m)>3])
        def month_date(m):
            month=next((x for x in MONTHS if x.lower().startswith(m.group(1).lower().rstrip('.'))),m.group(1))
            day=int(m.group(2));y=int(m.group(3)) if m.group(3) else None
            if not 1<=day<=31:return None
            return month+' '+ordinal(day)+(' '+year(y) if y else '')
        sub(r'\b('+month_pattern+r')\s+([0-3]?[0-9])(?:st|nd|rd|th)?(?:(?:\s*,\s*|\s+)([12][0-9]{3}))?\b',month_date,protect=True,flags=re.I)
        def fraction(m):
            whole=m.group(1);a=int(m.group(2));b=int(m.group(3))
            if not b:return None
            unit={2:('half','halves'),4:('quarter','quarters')}.get(b,(ordinal(b),ordinal(b)+'s'))
            return (cardinal(int(whole))+' and ' if whole else '')+cardinal(a)+' '+unit[a!=1]
        sub(r'\b(?:([0-9]{1,4})\s+)?([0-9]{1,3})/([0-9]{1,3})\b',fraction,protect=True)
        def clock(m):
            h,minute=int(m.group(1)),int(m.group(2));suffix=m.group(3)
            if h>23 or minute>59:return None
            if suffix:
                if not 1<=h<=12:return None
                suffix=' '+suffix.replace('.','').replace(' ','').upper()
                suffix=' '+' '.join(suffix.strip())
            else:suffix=' A M' if h<12 else ' P M';h=h%12 or 12
            return cardinal(h)+((' oh '+cardinal(minute)) if 0<minute<10 else ' '+cardinal(minute) if minute else ' o clock')+suffix
        sub(r'\b([0-2]?[0-9]):([0-5][0-9])\s*([aApP]\.?\s*[mM]\.?)?',clock,protect=True)
        sub(r'\b(1[0-2]|[1-9])\s*([aApP])\.?[mM]\.?',lambda m:cardinal(int(m.group(1)))+' '+m.group(2).upper()+' M',protect=True)
        sub(r'(?i)\b(in|since|during|year|born|until|by|from)\s+(1[6-9][0-9]{2}|20[0-9]{2}|21[0-9]{2})\b',lambda m:m.group(1)+' '+year(int(m.group(2))),protect=True)
        # Comma-grouped quantities retain ordinary cardinal pronunciation.
        sub(r'(?<![\w.])[0-9]{1,3}(?:,[0-9]{3})+(?:\.[0-9]+)?\b',lambda m:cardinal(int(m.group().split('.')[0].replace(',','')))+(' point '+' '.join(DIGITS[int(x)] for x in m.group().split('.')[1]) if '.' in m.group() else ''),protect=True)
        # Natural time/measurement expressions must not sound like identifiers.
        sub(r'\b([0-9]{1,6})(?:\.([0-9]+))?\s*(°\s*F|degrees?\s+Fahrenheit|feet|foot|inches|inch|miles?|pounds?|ounces?|gallons?|mph|minutes?|hours?|seconds?)\b',lambda m:cardinal(int(m.group(1)))+(' point '+' '.join(DIGITS[int(x)] for x in m.group(2)) if m.group(2) else '')+' '+('degrees Fahrenheit' if re.match(r'°',m.group(3)) else m.group(3)),protect=True,flags=re.I)
        # Leading-zero codes are protected only after date/time classification.
        sub(r'(?<![\w.])0[0-9]+(?!\w|[.,][0-9])',lambda m:digit_replacement(self,m),protect=True)
        sub(r'%',' percent')
        sub(r'=',' equals ')
        sub(r'×',' times ')
        sub(r'÷',' divided by ')
        if self.config.get('plain_numbers','natural')=='digits':
            def digits(m):
                words=[];mapping=[]
                for index,char in enumerate(m.group()):
                    word='point' if char=='.' else DIGITS[int(char)];a,b,_=self.map[m.start()+index]
                    if words:words.append(' ');mapping.append((a,b,True))
                    words.append(word);mapping.extend([(a,b,True)]*len(word))
                return ''.join(words),mapping
            sub(r'(?<![\w.])[0-9]+(?:\.[0-9]+)?\b',digits,protect=True)
        else:
            sub(r'(?<![\w.])'+NUM+END_NUMBER,lambda m:number_words(m.group()),protect=True)
        for word,replacement in self.config.get('words',{}).items():sub(r'\b'+re.escape(word)+r'\b',replacement,protect=True,flags=re.I)
        # Commas are visual punctuation, not a request for a long spoken pause.
        # Grouped quantities were already expanded above.
        sub(r',',' ')
        # Normalize while retaining original ranges for every spoken word.
        words=[];normalized=[];at=0
        for m in re.finditer(r'\S+',self.text):
            word=m.group();span=self.map[m.start():m.end()]
            words.append((at,at+len(word),min(p[0] for p in span),max(p[1] for p in span),word));normalized.append(word);at+=len(word)+1
        self.words=words;self.text=' '.join(normalized)
