import os,shlex,subprocess,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
class Tests(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.tmp=tempfile.TemporaryDirectory();cls.addClassCleanup(cls.tmp.cleanup);out=Path(cls.tmp.name)
  (out/'test.c').write_text(r'''
#include "muse_reader_layout.h"
#include <assert.h>
#include <stdlib.h>
static muse_reader_point_t points[65536];
static int32_t glyph(uint32_t cp,uint32_t next,void *ctx){(void)next;(void)ctx;return cp=='\n'?0:8;}
static size_t wrap(const char *s,size_t remain,int32_t width,void *ctx){(void)ctx;size_t bytes=0,chars=0;while(bytes<remain && chars<(size_t)width/8){size_t n;unsigned cp=muse_reader_decode(s+bytes,remain-bytes,&n);bytes+=n;chars++;if(cp=='\n')break;}return bytes;}
static size_t zero(const char *s,size_t n,int32_t w,void *c){(void)s;(void)n;(void)w;(void)c;return 0;}
int main(int argc,char **argv){assert(argc==2);size_t count=0;unsigned fallback=0;
switch(atoi(argv[1])){
case 0:assert(muse_reader_layout_build("abcdEF",32,points,65536,&count,&fallback,wrap,glyph,NULL));assert(count==6 && points[4].line==1 && points[5].x==8);break;
case 1:assert(muse_reader_layout_build("Caf\xc3\xa9 \xee\x85\xa4""ark",40,points,65536,&count,&fallback,wrap,glyph,NULL));assert(count==9 && points[5].line==1);break;
case 2:{static char long_text[32768];memset(long_text,'x',32767);assert(muse_reader_layout_build(long_text,200,points,65536,&count,&fallback,zero,glyph,NULL));assert(count==32767 && fallback==32767);break;}
case 3:assert(!muse_reader_layout_build("text",0,points,65536,&count,&fallback,wrap,glyph,NULL));assert(!muse_reader_layout_build("long",32,points,2,&count,&fallback,wrap,glyph,NULL));break;
case 4:assert(muse_reader_layout_build("abcd\nefgh\n",80,points,65536,&count,&fallback,wrap,glyph,NULL));assert(points[count].line==2);assert(muse_reader_layout_hit(points,count,8,20,20)==6);break;
default:assert(0);}return 0;}
''')
  cls.binary=out/'layout'
  cmd=[*shlex.split(os.environ.get('CC','cc')),'-std=gnu11','-Wall','-Wextra','-Werror','-I',str(ROOT/'components/muse'),str(out/'test.c'),'-o',str(cls.binary)]
  r=subprocess.run(cmd,capture_output=True,text=True)
  if r.returncode:raise AssertionError(r.stdout+r.stderr)
 def case(self,n):
  r=subprocess.run([str(self.binary),str(n)],capture_output=True,text=True,timeout=5);self.assertEqual(r.returncode,0,r.stdout+r.stderr)
 def test_wrapping_positions(self):self.case(0)
 def test_utf8_and_styled_glyphs(self):self.case(1)
 def test_zero_progress_from_wrapper_cannot_freeze(self):self.case(2)
 def test_zero_width_and_insufficient_memory_fail_safely(self):self.case(3)
 def test_newlines_and_bounded_link_hit_test(self):self.case(4)
