/* Free personal-use Open-Meteo forecast; no Muse credentials or account calls.
 * https://open-meteo.com/en/docs ; https://open-meteo.com/en/pricing */
#include "muse_weather.h"
#include "storyteller_weather_config.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "muse_wifi.h"
#define BODY_CAP 8192
static portMUX_TYPE lock=portMUX_INITIALIZER_UNLOCKED;
static muse_weather_view_t view;
static TaskHandle_t worker;
static int64_t observed,attempt;
typedef struct {char *text;size_t count;bool overflow;} body_t;
static esp_err_t receive(esp_http_client_event_t *event){
 body_t *b=event->user_data;
 if(event->event_id==HTTP_EVENT_ON_DATA){
  if(event->data_len<0 || (size_t)event->data_len>=BODY_CAP-b->count){b->overflow=true;return ESP_FAIL;}
  memcpy(b->text+b->count,event->data,event->data_len);b->count+=event->data_len;b->text[b->count]=0;
 }return ESP_OK;
}
static bool number(const cJSON *n,double low,double high){return cJSON_IsNumber(n)&&isfinite(n->valuedouble)&&n->valuedouble>=low&&n->valuedouble<=high;}
static bool date(const char *s){if(!s || strlen(s)<10)return false;for(int i=0;i<10;i++)if((i==4||i==7)?s[i]!='-':(s[i]<'0'||s[i]>'9'))return false;return true;}
static bool parse(const char *text,muse_weather_view_t *v){
 cJSON *root=cJSON_Parse(text);if(!root)return false;bool ok=false;
 const cJSON *units=cJSON_GetObjectItemCaseSensitive(root,"current_units"),*current=cJSON_GetObjectItemCaseSensitive(root,"current"),*daily=cJSON_GetObjectItemCaseSensitive(root,"daily");
 const cJSON *tu=cJSON_GetObjectItemCaseSensitive(units,"temperature_2m"),*wu=cJSON_GetObjectItemCaseSensitive(units,"wind_speed_10m");
 if(!cJSON_IsString(tu)||strcmp(tu->valuestring,"\xc2\xb0" "F")||!cJSON_IsString(wu)||strcmp(wu->valuestring,"mp/h"))goto done;
 const cJSON *t=cJSON_GetObjectItemCaseSensitive(current,"temperature_2m"),*f=cJSON_GetObjectItemCaseSensitive(current,"apparent_temperature"),*w=cJSON_GetObjectItemCaseSensitive(current,"wind_speed_10m"),*code=cJSON_GetObjectItemCaseSensitive(current,"weather_code"),*time=cJSON_GetObjectItemCaseSensitive(current,"time");
 if(!number(t,-150,160)||!number(f,-180,200)||!number(w,0,300)||!number(code,0,99)||!cJSON_IsString(time)||!date(time->valuestring)||(strlen(time->valuestring)!=16 || time->valuestring[10]!='T' || time->valuestring[13]!=':'))goto done;
 v->temperature=(float)t->valuedouble;v->feels=(float)f->valuedouble;v->wind=(float)w->valuedouble;v->code=code->valueint;snprintf(v->time,sizeof(v->time),"%s",time->valuestring);
 const char *keys[]={"time","temperature_2m_max","temperature_2m_min","precipitation_probability_max","weather_code"};const cJSON *arrays[5];
 for(int i=0;i<5;i++){arrays[i]=cJSON_GetObjectItemCaseSensitive(daily,keys[i]);if(!cJSON_IsArray(arrays[i])||cJSON_GetArraySize(arrays[i])!=MUSE_WEATHER_DAYS)goto done;}
 for(int i=0;i<MUSE_WEATHER_DAYS;i++){
  const cJSON *d=cJSON_GetArrayItem(arrays[0],i),*hi=cJSON_GetArrayItem(arrays[1],i),*lo=cJSON_GetArrayItem(arrays[2],i),*rain=cJSON_GetArrayItem(arrays[3],i),*c=cJSON_GetArrayItem(arrays[4],i);
  if(!cJSON_IsString(d)||!date(d->valuestring)||strlen(d->valuestring)!=10||!number(hi,-150,160)||!number(lo,-150,160)||lo->valuedouble>hi->valuedouble||!number(rain,0,100)||!number(c,0,99))goto done;
  snprintf(v->days[i].date,sizeof(v->days[i].date),"%s",d->valuestring);v->days[i].high=(float)hi->valuedouble;v->days[i].low=(float)lo->valuedouble;v->days[i].rain=rain->valueint;v->days[i].code=c->valueint;
 }v->valid=true;ok=true;
 done:cJSON_Delete(root);return ok;
}
static void run(void *unused){
 (void)unused;
 for(;;){ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(1000));
  muse_wifi_status_t wifi;muse_wifi_status(&wifi);
  int64_t now=esp_timer_get_time();
  portENTER_CRITICAL(&lock);
  if(wifi.state!=MUSE_WIFI_CONNECTED){
   if(view.busy || !view.valid){view.busy=false;snprintf(view.note,sizeof(view.note),"Waiting for Wi-Fi...");}
   portEXIT_CRITICAL(&lock);continue;
  }
  bool due=!view.valid || (observed && now-observed>=900000000);
  if(!view.busy && due && (!attempt || now-attempt>=30000000)){
   view.busy=true;view.version++;attempt=now;snprintf(view.note,sizeof(view.note),"Updating forecast...");
  }
  bool fetch=view.busy;portEXIT_CRITICAL(&lock);if(!fetch)continue;
  muse_weather_view_t fresh={0};bool ok=false;const char *error="Check Wi-Fi, then Refresh.";
  body_t body={.text=heap_caps_calloc(1,BODY_CAP,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)};
  if(!body.text)error="Not enough memory. Try again.";
  else {
   esp_http_client_config_t cfg={.url=STORYTELLER_WEATHER_URL,.timeout_ms=12000,.crt_bundle_attach=esp_crt_bundle_attach,.event_handler=receive,.user_data=&body,.disable_auto_redirect=true};
   esp_http_client_handle_t client=esp_http_client_init(&cfg);
   if(client){esp_err_t result=esp_http_client_perform(client);int status=esp_http_client_get_status_code(client);esp_http_client_cleanup(client);
    if(result==ESP_OK&&status==200&&!body.overflow){ok=parse(body.text,&fresh);if(!ok)error="Forecast format changed. Retry later.";}
    else if(status==429)error="Weather service busy. Retry later.";
   }free(body.text);
  }
  portENTER_CRITICAL(&lock);
  if(ok){fresh.version=view.version+1;view=fresh;observed=esp_timer_get_time();snprintf(view.note,sizeof(view.note),"From Open-Meteo");}
  else{view.busy=false;view.version++;snprintf(view.note,sizeof(view.note),"%s",error);}
  portEXIT_CRITICAL(&lock);
 }
}
void muse_weather_init(void){
 if(!STORYTELLER_WEATHER_URL[0]){snprintf(view.note,sizeof(view.note),"Configure your weather location first.");return;}
 if(!worker && xTaskCreate(run,"muse_weather",8192,NULL,2,&worker)!=pdPASS){
  portENTER_CRITICAL(&lock);view.busy=false;snprintf(view.note,sizeof(view.note),"Weather worker unavailable.");view.version++;portEXIT_CRITICAL(&lock);
 }
}
void muse_weather_request(void){
 if(!STORYTELLER_WEATHER_URL[0]){muse_weather_init();return;}
 int64_t now=esp_timer_get_time();
 portENTER_CRITICAL(&lock);bool skip=view.busy||(attempt && now-attempt<30000000);if(!skip){view.busy=true;view.version++;attempt=now;snprintf(view.note,sizeof(view.note),"Updating forecast...");}portEXIT_CRITICAL(&lock);if(skip)return;
 muse_weather_init();
 if(worker)xTaskNotifyGive(worker);
}
void muse_weather_copy(muse_weather_view_t *out){
 if(!out)return;
 int64_t now=esp_timer_get_time();portENTER_CRITICAL(&lock);*out=view;
 out->age_seconds=observed?(now-observed)/1000000:0;
 out->retry_seconds=attempt && now-attempt<30000000?(30000000-(now-attempt)+999999)/1000000:0;
 portEXIT_CRITICAL(&lock);
}
const char *muse_weather_description(int c){
 if(c==0)return "Clear";
 if(c==1)return "Mostly clear";
 if(c==2)return "Partly cloudy";
 if(c==3)return "Overcast";
 if(c==45||c==48)return "Fog";
 if(c>=51&&c<=57)return "Drizzle";
 if(c>=61&&c<=67)return "Rain";
 if(c>=71&&c<=77)return "Snow";
 if(c>=80&&c<=82)return "Rain showers";
 if(c==85||c==86)return "Snow showers";
 if(c>=95)return "Thunderstorms";
 return "Conditions";
}
