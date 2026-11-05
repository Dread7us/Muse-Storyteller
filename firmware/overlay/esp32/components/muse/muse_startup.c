/* Optional non-blocking edition welcome screen. */
#include "muse_startup.h"
#include "muse_settings.h"
#include "muse_module_llm.h"
#include "muse_storyteller_version.h"
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
    if(s_glow)lv_obj_set_x(s_glow,(int)((elapsed%1800)*84/1800));
    if(s_ticks%10)return;
    muse_module_llm_status_t st;muse_module_llm_status(&st);
    bool ready=st.state==MUSE_MODULE_LLM_READY;
    if(s_loading&&!s_preview){
        char text[64];if(ready)strlcpy(text,"Microphone and voice ready",sizeof(text));else snprintf(text,sizeof(text),"Starting microphone and voice  |  %us",elapsed/1000);
        if(strcmp(lv_label_get_text(s_loading),text))lv_label_set_text(s_loading,text);
    }
    /* Splash is skippable; leaving it never claims voice readiness. */
    if(elapsed>=2200 && (s_preview || ready || elapsed>=90000))muse_startup_hide();
}
static void skipped(lv_event_t *event){(void)event;muse_startup_hide();}
static void text_at(lv_obj_t *parent,const char *text,const lv_font_t *font,uint32_t color,int y){
    lv_obj_t *l=lv_label_create(parent);lv_obj_set_width(l,lv_pct(100));
    lv_obj_set_style_text_font(l,font,0);lv_obj_set_style_text_color(l,lv_color_hex(color),0);
    lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_CENTER,0);lv_label_set_text(l,text);lv_obj_set_y(l,y);
}
void muse_startup_show(bool preview){
    if(!preview&&!muse_settings_startup_splash())return;
    muse_startup_hide();s_preview=preview;s_ticks=0;s_started=lv_tick_get();
    s_cover=lv_obj_create(lv_screen_active());lv_obj_remove_style_all(s_cover);
    lv_obj_set_size(s_cover,lv_pct(100),lv_pct(100));lv_obj_set_style_bg_opa(s_cover,LV_OPA_COVER,0);
    lv_obj_set_style_bg_color(s_cover,lv_color_hex(0x100f1c),0);
    lv_obj_remove_flag(s_cover,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(s_cover,LV_OBJ_FLAG_CLICKABLE);lv_obj_add_event_cb(s_cover,skipped,LV_EVENT_CLICKED,NULL);
    lv_obj_t *frame=lv_obj_create(s_cover);lv_obj_remove_style_all(frame);lv_obj_set_size(frame,114,114);
    s_frame=frame;
    lv_obj_align(frame,LV_ALIGN_TOP_MID,0,12);lv_obj_set_style_radius(frame,LV_RADIUS_CIRCLE,0);
    lv_obj_set_style_clip_corner(frame,true,0);lv_obj_set_style_border_width(frame,1,0);
    lv_obj_set_style_border_color(frame,lv_color_hex(0xa484cf),0);lv_obj_remove_flag(frame,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *art=lv_image_create(frame);lv_image_set_src(art,&muse_splash_avatar);lv_obj_center(art);
    lv_obj_remove_flag(art,LV_OBJ_FLAG_CLICKABLE);
    for(unsigned i=0;i<3;i++){
        s_orbit[i]=lv_obj_create(s_cover);lv_obj_remove_style_all(s_orbit[i]);lv_obj_set_size(s_orbit[i],6,6);
        lv_obj_set_style_radius(s_orbit[i],LV_RADIUS_CIRCLE,0);
        lv_obj_set_style_bg_color(s_orbit[i],lv_color_hex(i?0xa484cf:0x8be0cb),0);
        lv_obj_set_style_bg_opa(s_orbit[i],(lv_opa_t)(255-i*65),0);
        lv_obj_remove_flag(s_orbit[i],LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
    }
    text_at(s_cover,"MUSE STORYTELLER",&lv_font_montserrat_20,0xf0e8ff,139);
    text_at(s_cover,"v" MUSE_STORYTELLER_VERSION "  /  Dread7us",&lv_font_montserrat_14,0xc4b2ff,170);

    text_at(s_cover,"Tap anywhere to skip",&lv_font_montserrat_14,0x9c91b4,218);
    s_loading=lv_label_create(s_cover);lv_obj_set_width(s_loading,lv_pct(100));
    lv_obj_set_style_text_align(s_loading,LV_TEXT_ALIGN_CENTER,0);lv_obj_set_style_text_font(s_loading,&lv_font_montserrat_14,0);
    lv_obj_set_style_text_color(s_loading,lv_color_hex(0x8be0cb),0);lv_obj_set_y(s_loading,190);
    lv_label_set_text(s_loading,preview?"LOCAL VOICE. CONNECTED STORIES.":"Loading voice and microphone...");
    s_track=lv_obj_create(s_cover);lv_obj_remove_style_all(s_track);lv_obj_set_size(s_track,120,3);lv_obj_align(s_track,LV_ALIGN_TOP_MID,0,210);
    lv_obj_set_style_radius(s_track,2,0);lv_obj_set_style_clip_corner(s_track,true,0);lv_obj_set_style_bg_color(s_track,lv_color_hex(0x332746),0);lv_obj_set_style_bg_opa(s_track,255,0);
    lv_obj_remove_flag(s_track,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
    s_glow=lv_obj_create(s_track);lv_obj_remove_style_all(s_glow);lv_obj_set_size(s_glow,36,3);lv_obj_set_style_radius(s_glow,2,0);
    lv_obj_set_style_bg_color(s_glow,lv_color_hex(0x8be0cb),0);lv_obj_set_style_bg_opa(s_glow,255,0);lv_obj_remove_flag(s_glow,LV_OBJ_FLAG_CLICKABLE);
    s_timer=lv_timer_create(expired,40,NULL);
    ESP_LOGI("muse_startup","Storyteller %s splash shown (%s)",MUSE_STORYTELLER_VERSION,preview?"preview":"boot");
}
