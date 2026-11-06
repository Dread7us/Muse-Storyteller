"""Optional word alignment in a separate process, outside the voice router GIL."""
import base64,json,os,select,subprocess,sys,threading,time
from pathlib import Path

class AlignmentError(RuntimeError):pass

class AlignmentWorker:
 def __init__(self,command=None,timeout=30):
  self.lock=threading.Lock();self.buffer=bytearray();self.timeout=timeout
  self.process=subprocess.Popen(command or [sys.executable,'-u',str(Path(__file__).resolve())],
   stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,
   bufsize=0,start_new_session=True,close_fds=True)
  os.set_blocking(self.process.stdin.fileno(),False)
  os.set_blocking(self.process.stdout.fileno(),False)
  try:
   if json.loads(self._read_line(time.monotonic()+timeout))!={'ready':True}:raise AlignmentError('Alignment worker is unavailable')
  except BaseException:self.close();raise
 def _read_line(self,deadline):
  while b'\n' not in self.buffer:
   if time.monotonic()>=deadline:raise AlignmentError('Optional alignment timed out')
   if not select.select([self.process.stdout],[],[],min(.1,max(0,deadline-time.monotonic())))[0]:continue
   try:part=os.read(self.process.stdout.fileno(),8192)
   except BlockingIOError:continue
   if not part:raise AlignmentError('Optional alignment worker stopped')
   self.buffer.extend(part)
   if len(self.buffer)>131072:raise AlignmentError('Invalid alignment response size')
  line,_,rest=self.buffer.partition(b'\n');self.buffer=bytearray(rest);return line
 def align(self,pcm,text):
  if not isinstance(pcm,bytes) or not 0<len(pcm)<=960000 or len(pcm)%2 or not isinstance(text,str) or len(text.encode())>8192:raise AlignmentError('Invalid alignment window')
  request=json.dumps(dict(pcm=base64.b64encode(pcm).decode(),text=text),ensure_ascii=True).encode()+b'\n'
  with self.lock:
   try:
    deadline=time.monotonic()+self.timeout;pending=memoryview(request)
    while pending:
     if time.monotonic()>=deadline:raise AlignmentError('Optional alignment input timed out')
     if not select.select([],[self.process.stdin],[],.1)[1]:continue
     try:written=os.write(self.process.stdin.fileno(),pending)
     except BlockingIOError:continue
     if written<=0:raise AlignmentError('Optional alignment worker stopped')
     pending=pending[written:]
    response=json.loads(self._read_line(deadline));words=response.get('words')
    if not isinstance(words,list) or len(words)>1000 or any(not isinstance(w,list) or len(w)!=4 for w in words):raise AlignmentError('Optional alignment failed')
    return words
   except BaseException:self.close();raise
 def close(self):
  p=self.process
  if p.poll() is None:
   p.terminate()
   try:p.wait(1)
   except subprocess.TimeoutExpired:p.kill();p.wait(1)
  for stream in (p.stdin,p.stdout):
   if stream and not stream.closed:stream.close()

def serve():
 from muse_native_audio import LocalAligner
 aligner=LocalAligner()
 print(json.dumps({'ready':True}),flush=True)
 while True:
  line=sys.stdin.buffer.readline(1500000)
  if not line:return
  if not line.endswith(b'\n'):return
  try:
   request=json.loads(line);encoded=request.get('pcm');text=request.get('text')
   if not isinstance(encoded,str) or len(encoded)>1280000 or not isinstance(text,str) or len(text.encode())>8192:raise ValueError('window')
   pcm=base64.b64decode(encoded,validate=True)
   if not 0<len(pcm)<=960000 or len(pcm)%2:raise ValueError('pcm')
   response={'words':aligner.align(pcm,text)}
  except Exception:response={'error':'alignment_failed'}
  print(json.dumps(response,allow_nan=False),flush=True)

if __name__=='__main__':serve()
