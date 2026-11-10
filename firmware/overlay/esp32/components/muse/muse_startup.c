/* Optional non-blocking edition welcome screen. */
#include "muse_startup.h"
#include "muse_settings.h"
#include "muse_module_llm.h"
#include "muse_storyteller_version.h"
#include "muse_wait_estimate.h"
#include "lvgl.h"
#include "esp_log.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
extern const lv_image_dsc_t muse_splash_avatar;
static lv_obj_t *s_cover;
static lv_timer_t *s_timer;
static lv_obj_t *s_loading;
static lv_obj_t *s_frame,*s_orbit[3],*s_track,*s_glow;
static bool s_preview;
static unsigned s_ticks;
static uint32_t s_started;
static uint32_t s_ready_at;
static muse_wait_countdown_t s_countdown;
bool muse_startup_visible(void){return s_cover!=NULL;}
void muse_startup_hide(void){
    if(s_timer){lv_timer_delete(s_timer);s_timer=NULL;}
    if(s_cover){lv_obj_delete_async(s_cover);s_cover=NULL;}s_loading=s_frame=s_track=s_glow=NULL;
    for(unsigned i=0;i<3;i++)s_orbit[i]=NULL;
}
static void expired(lv_timer_t *timer){
    (void)timer;s_ticks++;uint32_t elapsed=lv_tick_elaps(s_started);
    float angle=elapsed*.0028f;
    int center=lv_obj_get_width(s_cover)/2;
    for(unsigned i=0;i<3;i++)if(s_orbit[i]){
        float a=angle-i*.28f;lv_obj_set_pos(s_orbit[i],center+(int)(64*cosf(a))-3,69+(int)(64*sinf(a))-3);
    }
    if(s_frame)lv_obj_set_style_border_opa(s_frame,(lv_opa_t)(140+100*(.5f+.5f*sinf(angle))),0);
    if(s_ticks%10)return;
    muse_module_llm_status_t st;muse_module_llm_status(&st);
    bool ready=st.state==MUSE_MODULE_LLM_READY;
    muse_wait_model_t *model=muse_wait_model();
    muse_wait_view_t view=muse_wait_countdown(&s_countdown,elapsed/1000.f,model->boot_s,ready);
    if(s_glow)lv_obj_set_width(s_glow,view.progress_percent*lv_obj_get_width(s_track)/100);
    if(s_loading&&!s_preview){
        char text[80];if(ready)strlcpy(text,"Voice and mic ready | 100%",sizeof(text));
        else if(view.late)snprintf(text,sizeof(text),"Still starting | %u%%* | %us elapsed",view.progress_percent,(unsigned)(elapsed/1000));
        else snprintf(text,sizeof(text),"Starting voice | %u%%* | ~%us",view.progress_percent,view.seconds);
        if(strcmp(lv_label_get_text(s_loading),text))lv_label_set_text(s_loading,text);
    }
    if(ready&&!s_ready_at){s_ready_at=lv_tick_get();if(!s_preview&&elapsed>=2000&&elapsed<=90000){model->boot_s=.8f*model->boot_s+.2f*(elapsed/1000.f);muse_wait_save();}}
    /* Splash is skippable; leaving it never claims voice readiness. */
    if(elapsed>=2200 && (s_preview || (s_ready_at&&lv_tick_elaps(s_ready_at)>=600) || elapsed>=90000))muse_startup_hide();
}
static void skipped(lv_event_t *event){(void)event;muse_startup_hide();}
static void text_at(lv_obj_t *parent,const char *text,const lv_font_t *font,uint32_t color,int y){
    lv_obj_t *l=lv_label_create(parent);lv_obj_set_width(l,lv_pct(100));
    lv_obj_set_style_text_font(l,font,0);lv_obj_set_style_text_color(l,lv_color_hex(color),0);
    lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_CENTER,0);lv_label_set_text(l,text);lv_obj_set_y(l,y);
}
void muse_startup_show(bool preview){
    if(!preview&&!muse_settings_startup_splash())return;
    muse_startup_hide();s_preview=preview;s_ticks=0;s_started=lv_tick_get();s_ready_at=0;s_countdown=(muse_wait_countdown_t){0};
    s_cover=lv_obj_create(lv_screen_active());lv_obj_remove_style_all(s_cover);
    lv_obj_set_size(s_cover,lv_pct(100),lv_pct(100));lv_obj_set_style_bg_opa(s_cover,LV_OPA_COVER,0);
    lv_obj_set_style_bg_color(s_cover,lv_color_hex(0x0e0d1c),0);
    lv_obj_remove_flag(s_cover,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(s_cover,LV_OBJ_FLAG_CLICKABLE);lv_obj_add_event_cb(s_cover,skipped,LV_EVENT_CLICKED,NULL);
    lv_obj_t *frame=lv_obj_create(s_cover);lv_obj_remove_style_all(frame);lv_obj_set_size(frame,114,114);
    s_frame=frame;
    lv_obj_align(frame,LV_ALIGN_TOP_MID,0,12);lv_obj_set_style_radius(frame,LV_RADIUS_CIRCLE,0);
    lv_obj_set_style_clip_corner(frame,true,0);lv_obj_set_style_border_width(frame,1,0);
    lv_obj_set_style_border_color(frame,lv_color_hex(0x9f7bcf),0);lv_obj_remove_flag(frame,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *art=lv_image_create(frame);lv_image_set_src(art,&muse_splash_avatar);lv_obj_center(art);
    lv_obj_remove_flag(art,LV_OBJ_FLAG_CLICKABLE);
    for(unsigned i=0;i<3;i++){
        s_orbit[i]=lv_obj_create(s_cover);lv_obj_remove_style_all(s_orbit[i]);lv_obj_set_size(s_orbit[i],6,6);
        lv_obj_set_style_radius(s_orbit[i],LV_RADIUS_CIRCLE,0);
        lv_obj_set_style_bg_color(s_orbit[i],lv_color_hex(i?0x9f7bcf:0x80e0c8),0);
        lv_obj_set_style_bg_opa(s_orbit[i],(lv_opa_t)(255-i*65),0);
        lv_obj_remove_flag(s_orbit[i],LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
    }
    text_at(s_cover,"MUSE STORYTELLER",&lv_font_montserrat_20,0xeee5ff,139);
    text_at(s_cover,"v" MUSE_STORYTELLER_VERSION "  /  Dread7us",&lv_font_montserrat_14,0xbda8ff,170);

    text_at(s_cover,"Tap to skip | * Estimated",&lv_font_montserrat_14,0x998db4,218);
    s_loading=lv_label_create(s_cover);lv_obj_set_width(s_loading,lv_pct(100));
    lv_obj_set_style_text_align(s_loading,LV_TEXT_ALIGN_CENTER,0);lv_obj_set_style_text_font(s_loading,&lv_font_montserrat_14,0);
    lv_obj_set_style_text_color(s_loading,lv_color_hex(0x80e0c8),0);lv_obj_set_y(s_loading,190);
    lv_label_set_text(s_loading,preview?"LOCAL VOICE. CONNECTED STORIES.":"Starting voice | 0%*");
    s_track=lv_obj_create(s_cover);lv_obj_remove_style_all(s_track);lv_obj_set_size(s_track,288,23);lv_obj_align(s_track,LV_ALIGN_TOP_MID,0,190);
    lv_obj_set_style_radius(s_track,6,0);lv_obj_set_style_clip_corner(s_track,true,0);lv_obj_set_style_bg_color(s_track,lv_color_hex(0x21152e),0);lv_obj_set_style_bg_opa(s_track,255,0);
    lv_obj_set_style_border_width(s_track,1,0);lv_obj_set_style_border_color(s_track,lv_color_hex(0x9f7bcf),0);
    lv_obj_remove_flag(s_track,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
    s_glow=lv_obj_create(s_track);lv_obj_remove_style_all(s_glow);lv_obj_set_size(s_glow,0,23);lv_obj_set_style_radius(s_glow,6,0);
    lv_obj_set_style_bg_color(s_glow,lv_color_hex(0x51336e),0);lv_obj_set_style_bg_opa(s_glow,255,0);lv_obj_remove_flag(s_glow,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_parent(s_loading,s_track);lv_obj_set_pos(s_loading,3,3);lv_obj_set_width(s_loading,282);lv_label_set_long_mode(s_loading,LV_LABEL_LONG_MODE_DOTS);lv_obj_set_style_text_color(s_loading,lv_color_hex(0xf7f0ff),0);lv_obj_move_foreground(s_loading);
    s_timer=lv_timer_create(expired,40,NULL);
    ESP_LOGI("muse_startup","Storyteller %s splash shown (%s)",MUSE_STORYTELLER_VERSION,preview?"preview":"boot");
}
