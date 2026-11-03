/* SPDX-License-Identifier: Apache-2.0 */
#include "muse_rich_text.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>
typedef struct {char *out;size_t cap,n;unsigned chars,list_number;bool ok,bold,italic,strike,heading,code,fence,quote,link,ordered;muse_rich_meta_t *meta;} writer_t;
static void emit(writer_t *w,unsigned cp)
{
    if(!cp)return;
    unsigned style=((w->bold || w->heading)?MUSE_RICH_BOLD:0)|(w->italic?MUSE_RICH_ITALIC:0)|(w->strike?MUSE_RICH_STRIKE:0)|((w->code||w->fence)?MUSE_RICH_CODE:0)|(w->link?MUSE_RICH_LINK:0)|(w->quote?MUSE_RICH_QUOTE:0);
    if(cp>=32 && cp<=126){
        if(style&MUSE_RICH_CODE)cp+=0xe400;
        else if(cp>=33){if((style&3)==3)cp+=0xe300;else if(style&MUSE_RICH_ITALIC)cp+=0xe200;else if(style&MUSE_RICH_BOLD)cp+=MUSE_RICH_BOLD_BASE;}
    }
    unsigned char bytes[4];unsigned n;
    if(cp<0x80){bytes[0]=cp;n=1;}
    else if(cp<0x800){bytes[0]=0xc0|(cp>>6);bytes[1]=0x80|(cp&63);n=2;}
    else if(cp<0x10000){bytes[0]=0xe0|(cp>>12);bytes[1]=0x80|((cp>>6)&63);bytes[2]=0x80|(cp&63);n=3;}
    else {bytes[0]='?';n=1;}
    if(w->n+n>=w->cap || w->chars>=65534){w->ok=false;return;}
    if(w->meta){if(w->chars>=w->meta->cap){w->ok=false;return;}w->meta->styles[w->chars]=(uint8_t)style;}
    memcpy(w->out+w->n,bytes,n);w->n+=n;w->chars++;
}
static void literal(writer_t *w,const char *s){for(;*s;s++)emit(w,(unsigned char)*s);}
static unsigned decode(const char *s,size_t remain,size_t *n)
{
    unsigned c=(unsigned char)s[0];*n=1;
    if(c<128)return c;
    unsigned count=c>=0xf0?4:c>=0xe0?3:c>=0xc2?2:0;if(!count || count>remain)return '?';
    unsigned value=c&((1u<<(7-count))-1);
    for(unsigned i=1;i<count;i++){unsigned b=(unsigned char)s[i];if((b&0xc0)!=0x80)return '?';value=(value<<6)|(b&63);}
    if(value>0x10ffff || (value>=0xd800 && value<=0xdfff) || (count==2 && value<128) || (count==3 && value<2048) || (count==4 && value<65536))return '?';
    *n=count;return value;
}
static void readable(writer_t *w,unsigned cp)
{
    if(cp>=32 && cp<127){emit(w,cp);return;}
    if(cp=='\n' || cp=='\t'){emit(w,cp=='\t'?' ':cp);return;}
    if(cp=='\r' || cp==0x200d || cp==0xfe0f || cp==0xfe0e || cp==0x200b || (cp>=0x1f3fb && cp<=0x1f3ff))return;
    if(cp==0xa0){emit(w,' ');return;}
    if(cp==0x2018 || cp==0x2019){emit(w,'\'');return;}
    if(cp==0x201c || cp==0x201d){emit(w,'"');return;}
    if(cp>=0x2010 && cp<=0x2015){emit(w,'-');return;}
    if(cp==0x2026){literal(w,"...");return;}
    if(cp==0x2022 || cp==0xb0){emit(w,cp);return;}
    if(cp==0x2192 || cp==0x27a1){literal(w," -> ");return;}
    if(cp==0x2190){literal(w," <- ");return;}
    if(cp==0x2264){literal(w,"<=");return;}if(cp==0x2265){literal(w,">=");return;}
    if(cp==0x2260){literal(w,"!=");return;}if(cp==0xd7){emit(w,'x');return;}
    if(cp==0xf7){emit(w,'/');return;}if(cp==0xa9){literal(w,"(c)");return;}
    if(cp==0xae){literal(w,"(R)");return;}if(cp==0x2122){literal(w,"(TM)");return;}
    if(cp==0x2713 || cp==0x2705){literal(w,"[ok]");return;}
    if(cp==0x274c){literal(w,"[x]");return;}
    /* English typography and accented Latin letters have readable fallbacks. */
    const char *accent="AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    if(cp>=0xc0 && cp<=0xff){emit(w,(unsigned char)accent[cp-0xc0]);return;}
    if(cp>=0x300 && cp<=0x36f)return;
    if((cp>=0x1f000 && cp<=0x1ffff) || (cp>=0x2600 && cp<=0x27ff)){emit(w,' ');return;}
    /* Never draw a missing-glyph box or execute embedded markup. */
    emit(w,'?');
}
static bool tag(const char *s,const char *name){return !strcmp(s,name) || (s[0]=='/' && !strcmp(s+1,name));}
bool muse_rich_render(const char *raw,char *out,size_t cap,uint16_t *positions,size_t position_cap)
{return muse_rich_render_style(raw,out,cap,positions,position_cap,false,false);}
bool muse_rich_render_style(const char *raw,char *out,size_t cap,uint16_t *positions,size_t position_cap,bool bold,bool code)
{return muse_rich_render_full(raw,out,cap,positions,position_cap,bold,code,NULL);}
bool muse_rich_render_full(const char *raw,char *out,size_t cap,uint16_t *positions,size_t position_cap,bool bold,bool code,muse_rich_meta_t *meta)
{
    if(!raw || !out || !positions || !cap)return false;
    if(meta && (!meta->styles || !meta->cap))return false;
    size_t length=strlen(raw);if(length>=MUSE_RICH_RAW_CAP || position_cap<=length)return false;
    if(meta){meta->count=0;meta->link_count=0;}
    writer_t w={.out=out,.cap=cap,.ok=true,.bold=bold,.fence=code,.meta=meta};
    if(meta){w.italic=!!(meta->initial_style&2);w.strike=!!(meta->initial_style&4);w.quote=!!(meta->initial_style&32);w.code=!!(meta->initial_style&64);}bool line_start=true;size_t i=0;
    while(i<length && w.ok){
        size_t start=i,end=i+1;unsigned before=w.chars;char c=raw[i];
        if(line_start && !w.fence){
            size_t p=i;while(p<length && (raw[p]==' ' || raw[p]=='\t'))p++;
            if(raw[p]=='#'){
                size_t q=p;while(raw[q]=='#' && q-p<6)q++;
                if(raw[q]==' '){end=q+1;w.heading=true;goto consumed;}
            }
            if((raw[p]=='-' || raw[p]=='*' || raw[p]=='+') && raw[p+1]==' '){
                for(size_t k=i;k<p && k<i+12;k++){emit(&w,' ');}
                end=p+2;
                if(raw[end]=='[' && raw[end+1] && raw[end+2]==']' && (raw[end+1]==' ' || raw[end+1]=='x' || raw[end+1]=='X')){emit(&w,'[');emit(&w,raw[end+1]==' '?' ':'x');emit(&w,']');end+=3;if(raw[end]==' ')end++;}
                else emit(&w,0x2022);
                emit(&w,' ');line_start=false;goto consumed;
            }
            if(isdigit((unsigned char)raw[p])){
                size_t q=p;while(isdigit((unsigned char)raw[q]) && q-p<9)q++;
                if((raw[q]=='.' || raw[q]==')') && raw[q+1]==' '){for(size_t k=i;k<p && k<i+12;k++)emit(&w,' ');for(size_t k=p;k<q;k++)emit(&w,(unsigned char)raw[k]);emit(&w,'.');emit(&w,' ');end=q+2;line_start=false;goto consumed;}
            }
            if(raw[p]=='>' && raw[p+1]==' '){w.quote=true;emit(&w,' ');emit(&w,' ');end=p+2;line_start=false;goto consumed;}
        }
        if(!strncmp(raw+i,"```",3)){
            w.fence=!w.fence;end=i+3;while(end<length && raw[end]!='\n')end++;goto consumed;
        }
        if(c=='`' && !w.fence){w.code=!w.code;goto consumed;}
        if(c=='\\' && raw[i+1] && strchr("*_`#[]<>~\\",raw[i+1])){readable(&w,(unsigned char)raw[i+1]);end=i+2;goto consumed;}
        if(!w.code && !w.fence){
            if(c=='*' || (c=='_' && (raw[i+1]=='_' || i==0 || !isalnum((unsigned char)raw[i-1]) || !isalnum((unsigned char)raw[i+1])))){
                char mark=c;while(end<length && raw[end]==mark && end-i<3)end++;
                if(end-i==3){w.bold=!w.bold;w.italic=!w.italic;}else if(end-i==2)w.bold=!w.bold;else w.italic=!w.italic;goto consumed;
            }
            if(c=='~' && raw[i+1]=='~'){w.strike=!w.strike;end=i+2;goto consumed;}
            if(c=='!' && raw[i+1]=='['){goto consumed;}
            if(c=='['){
                const char *closing=strstr(raw+i,"]("),*url_end=closing?strchr(closing+2,')'):NULL;
                if(closing && url_end){
                    w.link=true;
                    if(meta && meta->link_count<16){muse_rich_link_t *l=&meta->links[meta->link_count];size_t n=(size_t)(url_end-closing-2);l->start=(uint16_t)w.chars;l->end=l->start;
                        if(n<sizeof(l->url) && (!strncmp(closing+2,"https://",8)||!strncmp(closing+2,"http://",7))){memcpy(l->url,closing+2,n);l->url[n]=0;meta->link_count++;}
                    }goto consumed;
                }
            }
            if(c==']' && raw[i+1]=='('){
                if(meta && w.link && meta->link_count)meta->links[meta->link_count-1].end=(uint16_t)w.chars;
                w.link=false;end=i+2;unsigned depth=1;while(end<length && depth){if(raw[end]=='(')depth++;if(raw[end]==')')depth--;end++;}goto consumed;
            }
            if(c==']'){goto consumed;}
            if(c=='<'){
                const char *close=strchr(raw+i,'>');
                if(close && close-(raw+i)<96){
                    char name[96];size_t n=(size_t)(close-(raw+i)-1);memcpy(name,raw+i+1,n);name[n]=0;
                    for(size_t k=0;k<n;k++){name[k]=(char)tolower((unsigned char)name[k]);if(name[k]==' '){name[k]=0;break;}}
                    if(tag(name,"script") || tag(name,"style")){
                        const char *closing=strstr(close+1,name[0]=='s' && name[1]=='c'?"</script>":"</style>");end=closing?(size_t)(closing-raw)+(name[1]=='c'?9:8):length;goto consumed;
                    }
                    if(tag(name,"b") || tag(name,"strong") || (name[0]=='h' && name[1]>='1' && name[1]<='6') || (name[0]=='/' && name[1]=='h'))w.bold=name[0]!='/';
                    if(tag(name,"i") || tag(name,"em"))w.italic=name[0]!='/';
                    if(tag(name,"s") || tag(name,"del") || tag(name,"strike"))w.strike=name[0]!='/';
                    if(tag(name,"ol")){w.ordered=name[0]!='/';w.list_number=0;}
                    if(tag(name,"br") || tag(name,"br/") || tag(name,"p") || tag(name,"div") || tag(name,"li")){
                        if(w.n && w.out[w.n-1]!='\n')emit(&w,'\n');
                        if(!strcmp(name,"li")){if(w.ordered){char number[16];snprintf(number,sizeof(number),"%u. ",++w.list_number);literal(&w,number);}else {emit(&w,0x2022);emit(&w,' ');}}
                    }
                    end=(size_t)(close-raw)+1;goto consumed;
                }
            }
            if(c=='&'){
                const char *semi=strchr(raw+i,';');
                if(semi && semi-(raw+i)<16){
                    char entity[17];size_t n=(size_t)(semi-raw-i-1);memcpy(entity,raw+i+1,n);entity[n]=0;unsigned value=0;
                    if(!strcmp(entity,"amp"))value='&';else if(!strcmp(entity,"lt"))value='<';else if(!strcmp(entity,"gt"))value='>';else if(!strcmp(entity,"quot"))value='"';else if(!strcmp(entity,"apos"))value='\'';else if(!strcmp(entity,"nbsp"))value=' ';else if(!strcmp(entity,"mdash"))value='-';else if(!strcmp(entity,"ndash"))value='-';else if(!strcmp(entity,"hellip"))value=0x2026;
                    else if(!strcmp(entity,"lsquo") || !strcmp(entity,"rsquo"))value='\'';
                    else if(!strcmp(entity,"ldquo") || !strcmp(entity,"rdquo"))value='"';
                    else if(!strcmp(entity,"bull"))value=0x2022;
                    else if(!strcmp(entity,"copy"))value=0xa9;
                    else if(!strcmp(entity,"reg"))value=0xae;
                    else if(!strcmp(entity,"trade"))value=0x2122;
                    else if(entity[0]=='#')value=(unsigned)strtoul(entity+(entity[1]=='x'?2:1),NULL,entity[1]=='x'?16:10);
                    if(value){readable(&w,value);end=(size_t)(semi-raw)+1;goto consumed;}
                }
            }
        }
        {size_t n;unsigned cp=decode(raw+i,length-i,&n);end=i+n;readable(&w,cp);}
        if(c=='\n'){line_start=true;w.heading=false;w.code=false;w.quote=false;}else line_start=false;
consumed:
        for(size_t k=start;k<end;k++)positions[k]=(uint16_t)before;
        positions[end]=(uint16_t)w.chars;i=end;
    }
    out[w.n]=0;positions[length]=(uint16_t)w.chars;if(meta)meta->count=w.chars;return w.ok;
}
