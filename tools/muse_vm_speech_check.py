"""Local English speech validation; no audio or text leaves the Muse VM."""
import collections,difflib,re,threading

def lexical(text):return re.findall(r"[a-z0-9']+",text.lower())

def repeated_speech(expected,heard):
    a,b=lexical(expected),lexical(heard)
    if a and len(b)>=2*len(a) and any(b[i:i+len(a)]==a and b[i+len(a):i+2*len(a)]==a for i in range(len(b)-2*len(a)+1)):return True
    for width in (4,6,8):
        reference=collections.Counter(tuple(a[i:i+width]) for i in range(len(a)-width+1))
        observed=collections.Counter(tuple(b[i:i+width]) for i in range(len(b)-width+1))
        if any(count>max(1,reference[phrase]) for phrase,count in observed.items()):return True
    return False

class SpeechCheck:
    def __init__(self,model_path,alignment_path=None):
        from faster_whisper import WhisperModel
        self.model=WhisperModel(str(model_path),device='cpu',compute_type='int8',cpu_threads=2,num_workers=1,local_files_only=True)
        from pathlib import Path
        from muse_ctc_alignment import AcousticAligner
        self.acoustic=AcousticAligner(alignment_path or Path(model_path).parent/'ctc-english')
        self.lock=threading.Lock()
    def check(self,pcm,text,first_word=0):
        import numpy as np
        from muse_speech_text import SpeechText
        audio=np.frombuffer(pcm,dtype='<i2').astype(np.float32)/32768.
        duration=len(audio)/16000.
        if not .2<duration<=29:raise ValueError('Speech section exceeds its alignment window')
        with self.lock:
            expected=lexical(text)
            def recognize(samples,beam):
                segments,_=self.model.transcribe(samples,language='en',beam_size=beam,condition_on_previous_text=False,
                    temperature=0,vad_filter=False,word_timestamps=False)
                return SpeechText(' '.join(segment.text.strip() for segment in segments)).text
            # Recognition checks what the generator actually said. Alignment
            # alone must not force unrelated or looping audio onto the script.
            original_repeated=False
            for beam in (1,3):
                normalized_heard=recognize(audio,beam)
                agreement=difflib.SequenceMatcher(None,expected,lexical(normalized_heard),autojunk=False).ratio()
                repeats=repeated_speech(text,normalized_heard)
                original_repeated|=repeats
                wrong=repeats or agreement<.55
                if not wrong:break
            if wrong and not original_repeated and 1<=len(expected)<=4 and duration<=3:
                # Whisper sometimes drops a single isolated word. Recheck
                # copies of the actual waveform, and require exact recognition
                # of at least two copies. Only the original clip is delivered.
                quiet=np.zeros(4800,dtype=np.float32)
                repeated=np.concatenate((quiet,audio,quiet,audio,quiet,audio,quiet))
                for beam in (1,3):
                    heard=lexical(recognize(repeated,beam))
                    if heard==expected*2 or heard==expected*3:
                        wrong=False;agreement=1.;break
            if wrong:raise ValueError('Generated speech differs from its text')
            words,confidence=self.acoustic.align(audio,text,first_word)
            return dict(aligned=True,words=words,agreement=round(agreement,4),pcm_samples=len(audio),
                        alignment_method='ctc-english',acoustic_confidence=confidence,alignment_padding=0.,alignment_filter=0)

def word_anchors(aligned,text,duration,first_word=0,lead=0.):
        located=[];cursor=0;probabilities=[]
        for entry in aligned:
            word=entry['word'].strip()
            if not word:continue
            at=text.find(word,cursor)
            if at<0:raise ValueError('Speech alignment changed its text')
            cursor=at+len(word);probabilities.append(float(entry['probability']))
            located.append((at,cursor,float(entry['start'])-lead,float(entry['end'])-lead))
        if not probabilities or sum(probabilities)/len(probabilities)<.25:raise ValueError('Generated speech has low text confidence')
        result=[];previous=-1.;matches=list(re.finditer(r'\S+',text))
        for index,match in enumerate(matches):
            if not re.search(r'\w',match.group()):continue
            hits=[w for w in located if w[0]<match.end() and w[1]>match.start()]
            if not hits:raise ValueError('Speech alignment omitted a word')
            begin=max(0.,min(w[2] for w in hits));end=min(duration,max(w[3] for w in hits))
            if begin<previous or end-begin<.01:raise ValueError('Speech alignment collapsed')
            result.append([round(begin,4),round(end,4),first_word+index]);previous=begin
        if not result:raise ValueError('Speech alignment has no words')
        return result
