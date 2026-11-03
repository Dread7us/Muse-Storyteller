/* Debounced, press-once navigation; sliding from PTT never selects a side. */
#pragma once
typedef struct {int stable,candidate;unsigned samples;bool side_latched;} core2_strip_t;
static inline unsigned core2_strip_update(core2_strip_t *s,int zone){
 if(zone<0 || zone>3)return 0;
 if(zone!=s->candidate){s->candidate=zone;s->samples=1;return 0;}
 if(s->samples<3)s->samples++;
 if(s->samples<3 || zone==s->stable)return 0;
 unsigned result=0;int old=s->stable;s->stable=zone;
 if(old==2)result|=MUSE_BTN_TALK_RELEASE;
 if(zone==2)result|=MUSE_BTN_TALK_PRESS;
 if(!zone)s->side_latched=false;
 else if(!old && !s->side_latched){s->side_latched=true;if(zone==1)result|=MUSE_BTN_NAV_BACK;else if(zone==3)result|=MUSE_BTN_NAV_FORWARD;}
 return result;
}
