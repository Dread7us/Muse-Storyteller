import ctypes,os,shlex,subprocess,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
class Tests(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.tmp=tempfile.TemporaryDirectory();cls.addClassCleanup(cls.tmp.cleanup);out=Path(cls.tmp.name)
  (out/'test.c').write_text(r'''
#include "muse_rich_text.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
static char rendered[MUSE_RICH_TEXT_CAP];static uint16_t pos[MUSE_RICH_RAW_CAP+1];
static void plain(char *out,const char *s){unsigned n=0;while(*s){unsigned char c=*s++;unsigned cp=c;if(c>=0xe0){cp=(c&15)<<12;cp|=(*s++&63)<<6;cp|=*s++&63;}else if(c>=0xc0){cp=(c&31)<<6;cp|=*s++&63;}if(cp>=0xe120 && cp<=0xe47e)cp&=255;out[n++]=cp<128?(char)cp:'?';}out[n]=0;}
int main(int argc,char **argv){assert(argc==2);char decoded[4096];const char *raw;
switch(atoi(argv[1])){
case 0:raw="Muse:\n**Bold words** and *more*.";assert(muse_rich_render(raw,rendered,sizeof(rendered),pos,MUSE_RICH_RAW_CAP+1));plain(decoded,rendered);assert(!strcmp(decoded,"Muse:\nBold words and more."));assert(strstr(rendered,"**")==0);assert(pos[6]==6 && pos[8]==6 && pos[12]==10);assert(pos[strlen(raw)]==26);break;
case 1:raw="# Heading\n<p><strong>Great</strong> &amp; &#8212; clear.</p><script>bad()</script>Safe";assert(muse_rich_render(raw,rendered,sizeof(rendered),pos,MUSE_RICH_RAW_CAP+1));plain(decoded,rendered);assert(strstr(decoded,"Heading\nGreat & - clear."));assert(strstr(decoded,"Safe"));assert(!strstr(decoded,"bad()"));break;
case 2:raw="A \xe2\x80\x9cquote\xe2\x80\x9d \xe2\x80\x94 ellipsis\xe2\x80\xa6 \xf0\x9f\x8c\x9f";assert(muse_rich_render(raw,rendered,sizeof(rendered),pos,MUSE_RICH_RAW_CAP+1));plain(decoded,rendered);assert(!strcmp(decoded,"A \"quote\" - ellipsis...  "));for(size_t i=1;i<=strlen(raw);i++)assert(pos[i]>=pos[i-1]);break;
case 3:raw="[Read this](https://example.com) and `x*y` or \\*literal\\*";assert(muse_rich_render(raw,rendered,sizeof(rendered),pos,MUSE_RICH_RAW_CAP+1));plain(decoded,rendered);assert(!strcmp(decoded,"Read this and x*y or *literal*"));break;
case 4:raw="**Long text**";assert(!muse_rich_render(raw,rendered,4,pos,MUSE_RICH_RAW_CAP+1));assert(rendered[3]==0 || strlen(rendered)<4);break;
case 5:raw="Start **highlight this** finish";assert(muse_rich_render(raw,rendered,sizeof(rendered),pos,MUSE_RICH_RAW_CAP+1));assert(pos[8]==6);assert(pos[17]==15);assert(pos[23]==20);plain(decoded,rendered);assert(!strcmp(decoded,"Start highlight this finish"));break;
case 6:{uint8_t styles[4096];muse_rich_meta_t meta={.styles=styles,.cap=sizeof(styles)};
 raw="**Bold** *Italic* ~~Gone~~ `Code`\n> Quote\n[Link](https://example.com)\n- item\n  - nested\n1) step\n- [x] done";
 assert(muse_rich_render_full(raw,rendered,sizeof(rendered),pos,MUSE_RICH_RAW_CAP+1,false,false,&meta));
 plain(decoded,rendered);assert(strstr(decoded,"Bold Italic Gone Code"));assert(strstr(decoded,"1. step"));assert(strstr(decoded,"[x] done"));assert(meta.link_count==1 && !strcmp(meta.links[0].url,"https://example.com"));
 assert(styles[pos[2]]&MUSE_RICH_BOLD);assert(styles[pos[10]]&MUSE_RICH_ITALIC);
 bool strike=false,code=false,quote=false,link=false;for(unsigned i=0;i<meta.count;i++){strike|=!!(styles[i]&MUSE_RICH_STRIKE);code|=!!(styles[i]&MUSE_RICH_CODE);quote|=!!(styles[i]&MUSE_RICH_QUOTE);link|=!!(styles[i]&MUSE_RICH_LINK);}assert(strike&&code&&quote&&link);break;}
 default:assert(0);}return 0;}
''')
  cls.binary=out/'rich'
  cmd=[*shlex.split(os.environ.get('CC','cc')),'-std=gnu11','-Wall','-Wextra','-Werror','-I',str(ROOT/'components/muse'),str(out/'test.c'),str(ROOT/'components/muse/muse_rich_text.c'),'-o',str(cls.binary)]
  r=subprocess.run(cmd,capture_output=True,text=True)
  if r.returncode:raise AssertionError(r.stdout+r.stderr)
 def run_case(self,n):
  r=subprocess.run([str(self.binary),str(n)],capture_output=True,text=True);self.assertEqual(r.returncode,0,r.stdout+r.stderr)
 def test_markdown_styles_and_mapping(self):self.run_case(0)
 def test_html_entities_and_inert_script_content(self):self.run_case(1)
 def test_typography_emoji_and_monotonic_source_positions(self):self.run_case(2)
 def test_links_code_and_escaped_literal_asterisks(self):self.run_case(3)
 def test_output_bound(self):self.run_case(4)
 def test_word_offsets_across_removed_markers(self):self.run_case(5)
 def test_visible_styles_links_nested_and_task_lists(self):self.run_case(6)
