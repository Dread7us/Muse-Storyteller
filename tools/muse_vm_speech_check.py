"""Local English speech validation; no audio or text leaves the Muse VM."""
import collections,difflib,re,threading

def lexical(text):return re.findall(r"[a-z0-9']+",text.lower())

def repeated_speech(expected,heard):
    a,b=lexical(expected),lexical(heard)
    for width in (4,6,8):
        reference=collections.Counter(tuple(a[i:i+width]) for i in range(len(a)-width+1))
        observed=collections.Counter(tuple(b[i:i+width]) for i in range(len(b)-width+1))
        if any(count>max(2,reference[phrase]+1) for phrase,count in observed.items()):return True
    return False

class SpeechCheck:
    def __init__(self,model_path):
        from faster_whisper import WhisperModel
        from faster_whisper.tokenizer import Tokenizer
        self.model=WhisperModel(str(model_path),device='cpu',compute_type='int8',cpu_threads=2,num_workers=1,local_files_only=True)
        self.tokenizer=Tokenizer(self.model.hf_tokenizer,self.model.model.is_multilingual,task='transcribe',language='en')
        self.lock=threading.Lock()
    def check(self,pcm,text,first_word=0):
        import numpy as np
        from muse_speech_text import SpeechText
        audio=np.frombuffer(pcm,dtype='<i2').astype(np.float32)/32768.
        duration=len(audio)/16000.
        if not .2<duration<=29:raise ValueError('Speech section exceeds its alignment window')
        with self.lock:
            # Recognition checks what the generator actually said. Alignment
            # alone must not force unrelated or looping audio onto the script.
            segments,_=self.model.transcribe(audio,language='en',beam_size=1,condition_on_previous_text=False,
                temperature=0,vad_filter=False,word_timestamps=False)
            heard=' '.join(segment.text.strip() for segment in segments)
            normalized_heard=SpeechText(heard).text
            expected_tokens=lexical(text);heard_tokens=lexical(normalized_heard)
            agreement=difflib.SequenceMatcher(None,expected_tokens,heard_tokens,autojunk=False).ratio()
            if repeated_speech(text,normalized_heard) or agreement<.55:raise ValueError('Generated speech differs from its text')
            last=None
            for lead,tail in ((0.,0.),(.24,.48),(.5,.8)):
                padded=np.pad(audio,(round(lead*16000),round(tail*16000)))
                features=self.model.feature_extractor(padded)
                frames=min(3000,features.shape[-1],round(len(padded)/160))
                encoded=self.model.encode(features[:,:3000])
                aligned=self.model.find_alignment(self.tokenizer,[self.tokenizer.encode(text)],encoded,frames)[0]
                try:
                    result=word_anchors(aligned,text,duration,first_word,lead)
                    return dict(aligned=True,words=result,agreement=round(agreement,4),pcm_samples=len(audio),alignment_padding=lead)
                except ValueError as exc:last=exc
            raise last

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
