"""Estimated word timing anchored to actual offline PCM, without a new model."""
import array,bisect,re,sys
class WordTiming:
 def __init__(self,text):
  from muse_speech_text import SpeechText
  spoken=SpeechText(text)
  self.words=spoken.words;self.normalized=spoken.text;self.cursor=0
 def phrase(self,phrase,pcm):
  begin=self.normalized.find(phrase,self.cursor)
  if begin<0:return []
  end=begin+len(phrase);self.cursor=end
  words=[w for w in self.words if w[0]<end and w[1]>begin]
  if not words or not pcm or len(pcm)%2:return []
  samples=array.array('h');samples.frombytes(pcm)
  if sys.byteorder!='little':samples.byteswap()
  frame=160;energies=[]
  for i in range(0,len(samples),frame):
   part=samples[i:i+frame];energies.append(sum(x*x for x in part)/max(1,len(part)))
  threshold=max(400,max(energies)*.003)
  active=[i for i,e in enumerate(energies) if e>=threshold] or list(range(len(energies)))
  weights=[max(1,len(re.sub(r'[^\w]','',w[4]))**.65) for w in words]
  total=sum(weights);at=0.;result=[]
  for word,weight in zip(words,weights):
   index=min(len(active)-1,int(at/total*len(active)))
   result.append((active[index]*.01,word[2],word[3]));at+=weight
  return result
