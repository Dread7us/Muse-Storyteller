#include <assert.h>
#include "muse_device_commands.h"
#define CHECK(s,k,v) do{muse_device_command_t c=muse_device_parse(s);assert(c.kind==k);assert(c.value==v);}while(0)
int main(void){
CHECK("set your volume to 35 percent",MUSE_DEVICE_VOLUME,35);
CHECK("turn volume down",MUSE_DEVICE_VOLUME_STEP,-10);
CHECK("raise the volume",MUSE_DEVICE_VOLUME_STEP,10);
CHECK("Set volume to 35%.",MUSE_DEVICE_VOLUME,35);
CHECK("Muse, please set the speaker volume to forty five percent!",MUSE_DEVICE_VOLUME,45);
CHECK("Can you set my volume to zero percent?",MUSE_DEVICE_VOLUME,0);
CHECK("set volume to one hundred percent",MUSE_DEVICE_VOLUME,100);
CHECK("volume down",MUSE_DEVICE_VOLUME_STEP,-10);
CHECK("turn the volume up",MUSE_DEVICE_VOLUME_STEP,10);
CHECK("set screen brightness to twenty-five percent",MUSE_DEVICE_BRIGHTNESS,25);
CHECK("set speech speed to normal",MUSE_DEVICE_SPEED,1);
CHECK("speak slower",MUSE_DEVICE_SPEED,0);CHECK("speak faster",MUSE_DEVICE_SPEED,2);
CHECK("mute the speaker",MUSE_DEVICE_MUTE,1);CHECK("unmute",MUSE_DEVICE_MUTE,0);
CHECK("on mute",MUSE_DEVICE_MUTE,0);
CHECK("On mute.",MUSE_DEVICE_MUTE,0);
CHECK("Muse, please on mute",MUSE_DEVICE_MUTE,0);
CHECK("on mute the speaker",MUSE_DEVICE_MUTE,0);
CHECK("on mute speaker",MUSE_DEVICE_MUTE,0);
assert(muse_device_parse("do not on mute").kind==MUSE_DEVICE_NONE);
assert(muse_device_parse("tell me a story about on mute").kind==MUSE_DEVICE_NONE);
assert(muse_device_parse("on mute and delete everything").kind==MUSE_DEVICE_NONE);
assert(muse_device_parse("set volume to 101").kind==MUSE_DEVICE_INVALID);
assert(muse_device_parse("set volume to -5").kind==MUSE_DEVICE_INVALID);
assert(muse_device_parse("set brightness to zero").kind==MUSE_DEVICE_INVALID);
assert(muse_device_parse("set volume to 35 and delete everything").kind==MUSE_DEVICE_INVALID);
assert(muse_device_parse("tell me a story about setting volume to 35").kind==MUSE_DEVICE_NONE);
assert(muse_device_parse("do not set volume to 35").kind==MUSE_DEVICE_NONE);
assert(muse_device_parse("What is the volume?").kind==MUSE_DEVICE_NONE);
assert(muse_device_parse("\"set volume to 35\"").kind==MUSE_DEVICE_NONE);
return 0;
}
