"""Bounded English character alignment from real acoustic emissions (CPU ONNX)."""
import json,re,unicodedata
from pathlib import Path

def viterbi(log_probs,tokens,blank=0):
    import numpy as np
    if log_probs.ndim!=2 or not tokens or len(tokens)>1000 or len(log_probs)>1600 or len(log_probs)<len(tokens):raise ValueError('Acoustic alignment exceeds its bound')
    states=np.full(len(tokens)*2+1,blank,dtype=np.int32);states[1::2]=tokens
    count=len(states);skip=np.zeros(count,dtype=bool);skip[2:]=(states[2:]!=blank)&(states[2:]!=states[:-2])
    previous=np.full(count,-np.inf,dtype=np.float32);previous[0]=0
    trace=np.zeros((len(log_probs),count),dtype=np.uint8);columns=np.arange(count)
    for frame,probability in enumerate(log_probs):
        one=np.full(count,-np.inf,dtype=np.float32);one[1:]=previous[:-1]
        two=np.full(count,-np.inf,dtype=np.float32);two[2:]=previous[:-2];two[~skip]=-np.inf
        candidates=np.stack((previous,one,two));choice=candidates.argmax(axis=0)
        trace[frame]=choice;previous=candidates[choice,columns]+probability[states]
    state=count-1 if previous[-1]>=previous[-2] else count-2
    if not np.isfinite(previous[state]):raise ValueError('Speech cannot cover the complete script')
    path=np.empty(len(log_probs),dtype=np.int32)
    for frame in range(len(log_probs)-1,-1,-1):path[frame]=state;state-=int(trace[frame,state])
    result=[]
    for index,token in enumerate(tokens):
        frames=np.flatnonzero(path==index*2+1)
        if not len(frames):raise ValueError('Acoustic alignment omitted a character')
        result.append((int(frames[0]),int(frames[-1])+1,float(np.exp(log_probs[frames,token]).mean())))
    return result

class AcousticAligner:
    def __init__(self,folder):
        import onnxruntime as ort
        folder=Path(folder);self.vocab=json.loads((folder/'vocab.json').read_text());config=json.loads((folder/'config.json').read_text())
        self.blank=config['pad_token_id'];self.jump=1;self.receptive=1
        for kernel,stride in zip(config['conv_kernel'],config['conv_stride']):self.receptive+=(kernel-1)*self.jump;self.jump*=stride
        options=ort.SessionOptions();options.intra_op_num_threads=2;options.inter_op_num_threads=1
        self.model=ort.InferenceSession(str(folder/'onnx/model_quantized.onnx'),options,providers=['CPUExecutionProvider'])
        self.inputs={value.name:value.type for value in self.model.get_inputs()}
        if set(self.inputs)-{'input_values','attention_mask'}:raise ValueError('Unknown acoustic model inputs')
    def align(self,audio,text,first_word=0):
        import numpy as np
        if not .2<len(audio)/16000.<=29:raise ValueError('Invalid acoustic alignment window')
        tokens=[];owners=[];words=[]
        for index,match in enumerate(re.finditer(r'\S+',text)):
            if not re.search(r'\w',match.group()):continue
            normalized=unicodedata.normalize('NFKD',match.group().replace('’',"'")).encode('ascii','ignore').decode().upper()
            letters=''.join(c for c in normalized if 'A'<=c<='Z' or c=="'").strip("'")
            if not letters:raise ValueError('Acoustic model needs normalized English words')
            if tokens:tokens.append(self.vocab['|']);owners.append(-1)
            words.append(first_word+index)
            for char in letters:tokens.append(self.vocab[char]);owners.append(first_word+index)
        values=((audio-audio.mean())/np.sqrt(audio.var()+1e-7))[None,:].astype(np.float32)
        feeds={'input_values':values}
        if 'attention_mask' in self.inputs:feeds['attention_mask']=np.ones_like(values,dtype=np.int64)
        logits=self.model.run(['logits'],feeds)[0][0].astype(np.float32)
        logits-=logits.max(axis=-1,keepdims=True);log_probs=logits-np.log(np.exp(logits).sum(axis=-1,keepdims=True))
        located=viterbi(log_probs,tokens,self.blank);result=[];confidence=[]
        for word in words:
            spans=[located[i] for i,owner in enumerate(owners) if owner==word]
            start=(min(s[0] for s in spans)*self.jump+self.receptive/2)/16000.
            end=min(len(audio)/16000.,(max(s[1] for s in spans)*self.jump+self.receptive/2)/16000.)
            if not 0<=start<end:raise ValueError('Invalid acoustic word boundary')
            result.append([round(start,4),round(end,4),word]);confidence.extend(s[2] for s in spans)
        score=sum(confidence)/len(confidence) if confidence else 0.
        if not result or score<.08:raise ValueError('Speech has low acoustic alignment confidence')
        return result,round(score,4)
