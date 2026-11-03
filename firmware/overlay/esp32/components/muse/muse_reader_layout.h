/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef struct {uint16_t x,line;} muse_reader_point_t;
typedef size_t (*muse_reader_next_line_fn)(const char *,size_t,int32_t,void *);
typedef int32_t (*muse_reader_glyph_width_fn)(uint32_t,uint32_t,void *);
static inline uint32_t muse_reader_decode(const char *s,size_t remain,size_t *bytes)
{
    unsigned c=(unsigned char)s[0];*bytes=1;if(c<128)return c;
    unsigned n=c>=0xf0?4:c>=0xe0?3:c>=0xc2?2:0;if(!n || n>remain)return '?';
    uint32_t cp=c&((1u<<(7-n))-1);
    for(unsigned i=1;i<n;i++){unsigned b=(unsigned char)s[i];if((b&0xc0)!=0x80)return '?';cp=(cp<<6)|(b&63);}
    *bytes=n;return cp;
}
/* Bounded, forward-only layout. A zero-progress wrap result cannot hang the UI. */
static inline bool muse_reader_layout_build(const char *text,int32_t width,muse_reader_point_t *out,size_t cap,size_t *count,unsigned *fallbacks,muse_reader_next_line_fn next_line,muse_reader_glyph_width_fn glyph,void *context)
{
    if(!text || !out || cap<2 || !count || width<1 || !next_line || !glyph)return false;
    size_t len=strlen(text),byte=0,chars=0;unsigned line=0;int32_t x=0;uint32_t last=0;
    if(fallbacks)*fallbacks=0;
    while(byte<len){
        size_t step=next_line(text+byte,len-byte,width,context);
        if(!step || step>len-byte){size_t n;muse_reader_decode(text+byte,len-byte,&n);step=n;if(fallbacks)(*fallbacks)++;}
        size_t end=byte+step;x=0;
        while(byte<end){
            if(chars+1>=cap || line>65534)return false;
            size_t n;uint32_t cp=muse_reader_decode(text+byte,len-byte,&n);
            if(byte+n>end)end=byte+n;
            size_t next_n;uint32_t next=byte+n<len?muse_reader_decode(text+byte+n,len-byte-n,&next_n):0;
            out[chars++]=(muse_reader_point_t){(uint16_t)(x<0?0:x>65535?65535:x),(uint16_t)line};
            if(cp!='\n' && cp!='\r'){int32_t advance=glyph(cp,next,context);if(advance>0)x+=advance;}
            byte+=n;last=cp;
        }
        if(byte<len){line++;x=0;}
    }
    if(last=='\n' || last=='\r'){line++;x=0;}
    if(chars>=cap || line>65535)return false;
    out[chars]=(muse_reader_point_t){(uint16_t)(x<0?0:x>65535?65535:x),(uint16_t)line};*count=chars;return true;
}
static inline size_t muse_reader_layout_hit(const muse_reader_point_t *points,size_t count,int32_t x,int32_t y,int32_t line_height)
{
    if(!points || !count || line_height<1)return 0;
    unsigned target=y<0?0:(unsigned)(y/line_height);size_t lo=0,hi=count;
    while(lo<hi){size_t mid=lo+(hi-lo)/2;if(points[mid].line<target)lo=mid+1;else hi=mid;}
    if(lo>=count)return count-1;
    size_t best=lo;int32_t best_distance=INT32_MAX;
    for(size_t i=lo;i<count && points[i].line==points[lo].line;i++){
        int32_t dx=(int32_t)points[i].x-x;if(dx<0)dx=-dx;
        if(dx<best_distance){best=i;best_distance=dx;}
    }
    return best;
}
