/* SPDX-License-Identifier: Apache-2.0 */
#include "muse_wait_estimate.h"
#include <math.h>
#include <stddef.h>
#ifdef ESP_PLATFORM
#include "nvs.h"
#endif
static muse_wait_model_t model;
static bool loaded;
static float bound(float x,float low,float high){return fminf(high,fmaxf(low,x));}
void muse_wait_defaults(muse_wait_model_t *m){*m=(muse_wait_model_t){1,0,36.f,.27f,30.f};}
bool muse_wait_model_valid(const muse_wait_model_t *m){
    return m&&m->version==1&&isfinite(m->response_s)&&m->response_s>=2&&m->response_s<=180&&
        isfinite(m->seconds_per_word)&&m->seconds_per_word>=.03f&&m->seconds_per_word<=2&&
        isfinite(m->boot_s)&&m->boot_s>=2&&m->boot_s<=90;
}
float muse_wait_budget(const muse_wait_model_t *m,unsigned words){
    return 2.f+bound((float)(words?words:45),1,400)*m->seconds_per_word;
}
muse_wait_view_t muse_wait_countdown(muse_wait_countdown_t *s,float elapsed,float budget,bool ready){
    if(!s->started){s->remaining_percent=100;s->started=true;}
    if(!isfinite(elapsed)||elapsed<0)elapsed=0;
    if(!isfinite(budget)||budget<1)budget=1;
    float fraction=bound(100.f*(1.f-elapsed/budget),3,100);
    if(fraction<s->remaining_percent)s->remaining_percent=fraction;
    s->late=elapsed>=budget;
    if(ready){s->remaining_percent=0;s->late=false;}
    return (muse_wait_view_t){100u-(unsigned)ceilf(s->remaining_percent),
        ready||s->late?0:(unsigned)ceilf(fmaxf(0,budget-elapsed)),s->late};
}
void muse_wait_learn(muse_wait_model_t *m,float response,float prep,unsigned words){
    if(!isfinite(response)||!isfinite(prep)||response<2||response>180||prep<1||prep>120||!words||words>400)return;
    m->response_s=.8f*m->response_s+.2f*response;
    m->seconds_per_word=.8f*m->seconds_per_word+.2f*bound((prep-2.f)/words,.03f,2.f);
    if(m->samples<1000000)m->samples++;
}
muse_wait_model_t *muse_wait_model(void){
    if(!loaded){
        loaded=true;muse_wait_defaults(&model);
#ifdef ESP_PLATFORM
        nvs_handle_t handle;
        if(nvs_open("muse-wait",NVS_READONLY,&handle)==ESP_OK){
            muse_wait_model_t saved;size_t size=sizeof(saved);
            if(nvs_get_blob(handle,"model",&saved,&size)==ESP_OK&&size==sizeof(saved)&&muse_wait_model_valid(&saved))model=saved;
            nvs_close(handle);
        }
#endif
    }
    return &model;
}
void muse_wait_save(void){
#ifdef ESP_PLATFORM
    nvs_handle_t handle;
    if(loaded&&muse_wait_model_valid(&model)&&nvs_open("muse-wait",NVS_READWRITE,&handle)==ESP_OK){
        if(nvs_set_blob(handle,"model",&model,sizeof(model))==ESP_OK)nvs_commit(handle);
        nvs_close(handle);
    }
#endif
}
