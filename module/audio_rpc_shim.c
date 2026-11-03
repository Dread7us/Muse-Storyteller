#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
/* Scope: only the trial Whisper process. Its audio RPC receives recorded test
   PCM from our private replay publisher, never the production microphone. */
int zmq_connect(void *socket,const char *endpoint){
 static int (*real_connect)(void *,const char *);
 if(!real_connect)real_connect=dlsym(RTLD_NEXT,"zmq_connect");
 const char *private_rpc=getenv("MUSE_STT_PRIVATE_AUDIO_RPC");
 if(private_rpc && !strcmp(endpoint,"ipc:///tmp/rpc.audio"))endpoint=private_rpc;
 return real_connect(socket,endpoint);
}
