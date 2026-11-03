/* Explicit owner speech commands only. Never interpret assistant replies as commands. */
#pragma once
#include <stdbool.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
typedef enum {MUSE_DEVICE_NONE,MUSE_DEVICE_INVALID,MUSE_DEVICE_VOLUME,MUSE_DEVICE_VOLUME_STEP,MUSE_DEVICE_BRIGHTNESS,MUSE_DEVICE_SPEED,MUSE_DEVICE_MUTE} muse_device_kind_t;
typedef struct {muse_device_kind_t kind;int value;} muse_device_command_t;
static inline int muse_device_number(const char *text){
 if(!*text)return -1;
 bool digits=true;for(const char *p=text;*p;p++)if(!isdigit((unsigned char)*p))digits=false;
 if(digits)return strlen(text)<=3?atoi(text):-1;
 const char *ones[]={"zero","one","two","three","four","five","six","seven","eight","nine","ten","eleven","twelve","thirteen","fourteen","fifteen","sixteen","seventeen","eighteen","nineteen"};
 for(int i=0;i<20;i++)if(!strcmp(text,ones[i]))return i;
 if(!strcmp(text,"one hundred")||!strcmp(text,"a hundred"))return 100;
 const char *tens[]={"twenty","thirty","forty","fifty","sixty","seventy","eighty","ninety"};
 for(int i=0;i<8;i++){size_t n=strlen(tens[i]);if(!strcmp(text,tens[i]))return (i+2)*10;if(!strncmp(text,tens[i],n)&&text[n]==' ')for(int j=1;j<10;j++)if(!strcmp(text+n+1,ones[j]))return (i+2)*10+j;}
 return -1;
}
static inline muse_device_command_t muse_device_parse(const char *input){
 muse_device_command_t out={MUSE_DEVICE_NONE,0};char text[192];size_t n=0;
 if(!input||strlen(input)>=sizeof(text))return out;
 for(const unsigned char *p=(const unsigned char *)input;*p;p++){
  if(isalnum(*p)){text[n++]=(char)tolower(*p);}
  else if((*p=='-'||*p=='.')&&isdigit(p[1])){text[n++]=(char)*p;}
  else if(*p=='%'||isspace(*p)||*p=='-'||*p==','){if(n&&text[n-1]!=' ')text[n++]=' ';}
  else if(*p!='.'&&*p!='!'&&*p!='?')return out;
 }
 while(n&&text[n-1]==' '){n--;}
 text[n]=0;char *p=text;
 if(!strncmp(p,"hey muse ",9))p+=9;else if(!strncmp(p,"muse ",5))p+=5;
 if(!strncmp(p,"please ",7))p+=7;
 if(!strncmp(p,"can you ",8))p+=8;else if(!strncmp(p,"could you ",10))p+=10;
 if(!strncmp(p,"please ",7))p+=7;
 if(!strcmp(p,"mute")||!strcmp(p,"mute the speaker")||!strcmp(p,"mute speaker"))return (muse_device_command_t){MUSE_DEVICE_MUTE,1};
 /* Common English ASR split of "unmute", accepted only as a complete command. */
 if(!strcmp(p,"unmute")||!strcmp(p,"unmute the speaker")||!strcmp(p,"unmute speaker")||!strcmp(p,"on mute")||!strcmp(p,"on mute the speaker")||!strcmp(p,"on mute speaker"))return (muse_device_command_t){MUSE_DEVICE_MUTE,0};
 if(!strcmp(p,"turn the volume up")||!strcmp(p,"volume up")||!strcmp(p,"turn volume up")||!strcmp(p,"raise the volume"))return (muse_device_command_t){MUSE_DEVICE_VOLUME_STEP,10};
 if(!strcmp(p,"turn the volume down")||!strcmp(p,"volume down")||!strcmp(p,"turn volume down")||!strcmp(p,"lower the volume"))return (muse_device_command_t){MUSE_DEVICE_VOLUME_STEP,-10};
 if(!strcmp(p,"speak slower"))return (muse_device_command_t){MUSE_DEVICE_SPEED,0};
 if(!strcmp(p,"speak faster"))return (muse_device_command_t){MUSE_DEVICE_SPEED,2};
 if(strncmp(p,"set ",4)){return out;}
 p+=4;if(!strncmp(p,"the ",4))p+=4;if(!strncmp(p,"my ",3))p+=3;else if(!strncmp(p,"your ",5))p+=5;
 if(!strncmp(p,"speaker volume ",15)){out.kind=MUSE_DEVICE_VOLUME;p+=15;}
 else if(!strncmp(p,"volume ",7)){out.kind=MUSE_DEVICE_VOLUME;p+=7;}
 else if(!strncmp(p,"screen brightness ",18)){out.kind=MUSE_DEVICE_BRIGHTNESS;p+=18;}
 else if(!strncmp(p,"brightness ",11)){out.kind=MUSE_DEVICE_BRIGHTNESS;p+=11;}
 else if(!strncmp(p,"speech speed ",13)){out.kind=MUSE_DEVICE_SPEED;p+=13;}
 else return out;
 if(!strncmp(p,"to ",3))p+=3;else if(!strncmp(p,"at ",3))p+=3;
 if(out.kind==MUSE_DEVICE_SPEED){if(!strcmp(p,"slow"))out.value=0;else if(!strcmp(p,"normal"))out.value=1;else if(!strcmp(p,"fast"))out.value=2;else out.kind=MUSE_DEVICE_INVALID;return out;}
 size_t len=strlen(p);if(len>8&&!strcmp(p+len-8," percent"))p[len-8]=0;
 int value=muse_device_number(p);if(value<0||value>100||(out.kind==MUSE_DEVICE_BRIGHTNESS&&value<10)){out.kind=MUSE_DEVICE_INVALID;return out;}out.value=value;return out;
}
