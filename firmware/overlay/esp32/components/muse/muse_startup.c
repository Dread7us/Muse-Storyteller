/* Optional non-blocking edition welcome screen. */
#include "muse_startup.h"
#include "muse_settings.h"
#include "muse_module_llm.h"
#include "muse_storyteller_version.h"
#include "lvgl.h"
#include "esp_log.h"
extern const lv_image_dsc_t muse_splash_avatar;
static lv_obj_t *s_cover;
static lv_timer_t *s_timer;
static lv_obj_t *s_loading;
static bool s_preview;
static unsigned s_ticks;
bool muse_startup_visible(void){return s_cover!=NULL;}
void muse_startup_hide(void){
    if(s_timer){lv_timer_delete(s_timer);s_timer=NULL;}
    if(s_cover){lv_obj_delete_async(s_cover);s_cover=NULL;}s_loading=NULL;
}
static void expired(lv_timer_t *timer){
    (void)timer;s_ticks++;
    muse_module_llm_status_t st;muse_module_llm_status(&st);
    bool ready=st.state==MUSE_MODULE_LLM_READY;
    if(s_loading)lv_label_set_text(s_loading,ready?"Voice ready":"Loading voice and microphone...");
    /* Splash is skippable; leaving it never claims voice readiness. */
    if(s_ticks>=11 && (s_preview || ready || s_ticks>=150))muse_startup_hide();
}
static void skipped(lv_event_t *event){(void)event;muse_startup_hide();}
static void text_at(lv_obj_t *parent,const char *text,const lv_font_t *font,uint32_t color,int y){
    lv_obj_t *l=lv_label_create(parent);lv_obj_set_width(l,lv_pct(100));
    lv_obj_set_style_text_font(l,font,0);lv_obj_set_style_text_color(l,lv_color_hex(color),0);
    lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_CENTER,0);lv_label_set_text(l,text);lv_obj_set_y(l,y);
}
void muse_startup_show(bool preview){
    if(!preview&&!muse_settings_startup_splash())return;
    muse_startup_hide();s_preview=preview;s_ticks=0;
    s_cover=lv_obj_create(lv_screen_active());lv_obj_remove_style_all(s_cover);
    lv_obj_set_size(s_cover,lv_pct(100),lv_pct(100));lv_obj_set_style_bg_opa(s_cover,LV_OPA_COVER,0);
    lv_obj_set_style_bg_color(s_cover,lv_color_hex(0x100f1c),0);
    lv_obj_remove_flag(s_cover,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(s_cover,LV_OBJ_FLAG_CLICKABLE);lv_obj_add_event_cb(s_cover,skipped,LV_EVENT_CLICKED,NULL);
    lv_obj_t *frame=lv_obj_create(s_cover);lv_obj_remove_style_all(frame);lv_obj_set_size(frame,114,114);
    lv_obj_align(frame,LV_ALIGN_TOP_MID,0,12);lv_obj_set_style_radius(frame,LV_RADIUS_CIRCLE,0);
    lv_obj_set_style_clip_corner(frame,true,0);lv_obj_set_style_border_width(frame,1,0);
    lv_obj_set_style_border_color(frame,lv_color_hex(0xa484cf),0);lv_obj_remove_flag(frame,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *art=lv_image_create(frame);lv_image_set_src(art,&muse_splash_avatar);lv_obj_center(art);
    lv_obj_remove_flag(art,LV_OBJ_FLAG_CLICKABLE);
    text_at(s_cover,"MUSE STORYTELLER",&lv_font_montserrat_20,0xf0e8ff,139);
    text_at(s_cover,"v" MUSE_STORYTELLER_VERSION "  /  Dread7us",&lv_font_montserrat_14,0xc4b2ff,170);

    text_at(s_cover,"Tap anywhere to skip",&lv_font_montserrat_14,0x9c91b4,218);
    s_loading=lv_label_create(s_cover);lv_obj_set_width(s_loading,lv_pct(100));
    lv_obj_set_style_text_align(s_loading,LV_TEXT_ALIGN_CENTER,0);lv_obj_set_style_text_font(s_loading,&lv_font_montserrat_14,0);
    lv_obj_set_style_text_color(s_loading,lv_color_hex(0x8be0cb),0);lv_obj_set_y(s_loading,197);
    lv_label_set_text(s_loading,preview?"LOCAL VOICE. CONNECTED STORIES.":"Loading voice and microphone...");
    s_timer=lv_timer_create(expired,200,NULL);
    ESP_LOGI("muse_startup","Storyteller %s splash shown (%s)",MUSE_STORYTELLER_VERSION,preview?"preview":"boot");
}
