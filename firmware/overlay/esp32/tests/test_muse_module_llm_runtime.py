# SPDX-License-Identifier: Apache-2.0
"""Exercise the actual local UART client against deterministic hardware fakes."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
JSON = Path(os.environ.get('CJSON_SOURCE_DIR', ROOT / 'managed_components/espressif__cjson/cJSON'))

COMMON = r'''
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_TIMEOUT 3
#define ESP_ERR_NO_MEM 4
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(x) (x)
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGI(tag, ...) ((void)(tag))
#define UART_DATA_8_BITS 8
#define UART_PARITY_DISABLE 0
#define UART_STOP_BITS_1 1
#define UART_HW_FLOWCTRL_DISABLE 0
#define UART_SCLK_DEFAULT 0
#define UART_PIN_NO_CHANGE -1
typedef struct {int baud_rate,data_bits,parity,stop_bits,flow_ctrl,source_clk;} uart_config_t;
#define CONFIG_MUSE_MODULE_LLM_UART_NUM 2
#define CONFIG_MUSE_MODULE_LLM_BAUD_RATE 115200
#define CONFIG_MUSE_MODULE_LLM_TX_GPIO 14
#define CONFIG_MUSE_MODULE_LLM_RX_GPIO 13
#define CONFIG_MUSE_MODULE_LLM_ASR_MODEL "sherpa-ncnn-streaming-zipformer-20M-2023-02-17"
#define CONFIG_MUSE_MODULE_LLM_TTS_MODEL "melotts_zh-cn"
extern char fake_uart[65536];
extern size_t fake_uart_n;
extern int64_t fake_time;
static inline size_t test_strlcpy(char *d,const char *s,size_t cap) {
    size_t n=strlen(s); if(cap){size_t m=n<cap-1?n:cap-1;memcpy(d,s,m);d[m]=0;}return n;
}
#define strlcpy test_strlcpy
static inline int uart_write_bytes(int port,const void *b,size_t n) {
    (void)port;assert(fake_uart_n+n<sizeof(fake_uart));memcpy(fake_uart+fake_uart_n,b,n);fake_uart_n+=n;fake_uart[fake_uart_n]=0;return (int)n;
}
static inline int uart_read_bytes(int p,void *b,size_t n,int ticks){(void)p;(void)b;(void)n;(void)ticks;return 0;}
static inline int uart_param_config(int p,const uart_config_t *c){(void)p;(void)c;return 0;}
static inline int uart_set_pin(int a,int b,int c,int d,int e){(void)a;(void)b;(void)c;(void)d;(void)e;return 0;}
static inline int uart_driver_install(int a,int b,int c,int d,void *e,int f){(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;return 0;}
static inline int uart_driver_delete(int p){(void)p;return 0;}
static inline void *heap_caps_malloc(size_t n,int caps){(void)caps;return malloc(n);}
static inline int64_t esp_timer_get_time(void){return fake_time;}
static inline uint32_t esp_random(void){return 0x1234abcd;}
typedef struct {size_t size;unsigned count,head;unsigned char bytes[8][64];} fake_queue;
typedef fake_queue *QueueHandle_t;
static inline QueueHandle_t xQueueCreate(unsigned n,size_t size){assert(n<=8&&size<=64);fake_queue*q=calloc(1,sizeof(*q));q->size=size;return q;}
static inline int xQueueSend(QueueHandle_t q,const void *item,int wait){(void)wait;if(q->count==8)return 0;memcpy(q->bytes[(q->head+q->count)%8],item,q->size);q->count++;return 1;}
static inline int xQueueReceive(QueueHandle_t q,void *item,int wait){(void)wait;if(!q->count)return 0;memcpy(item,q->bytes[q->head],q->size);q->head=(q->head+1)%8;q->count--;return 1;}
static inline unsigned uxQueueMessagesWaiting(QueueHandle_t q){return q->count;}
static inline void vQueueDelete(QueueHandle_t q){free(q);}
static inline int xTaskCreate(void(*fn)(void*),const char*n,unsigned stack,void*a,int pri,void*out){(void)fn;(void)n;(void)stack;(void)a;(void)pri;(void)out;return 1;}
static inline void vTaskDelete(void *t){(void)t;}
'''

HARNESS = r'''
#include "fake_common.h"
char fake_uart[65536]; size_t fake_uart_n; int64_t fake_time=100;
#include "muse_module_llm.c"
static bool fake_recorder;
bool muse_memory_recorder_active(void){return fake_recorder;}
void muse_memory_tick(const char *s,bool idle){(void)s;(void)idle;}
bool muse_memory_reply(const cJSON *r,const char *s){(void)r;(void)s;return false;}

static void reset(runtime_t *r) {
    memset(r,0,sizeof(*r));r->nonce=0x1234abcd;strcpy(r->session,"1234abcd");
    s_ready=true;s_recovering=false;s_playback=MUSE_MODULE_LLM_PLAYBACK_NONE;s_play_generation=0;
    s_cancel_requested=s_cancel_valid=false;s_record_epoch=0;s_transcript[0]=0;
    s_commands=xQueueCreate(8,sizeof(command_t));fake_uart_n=0;fake_uart[0]=0;fake_time=100;
}
static void drain_command(runtime_t *r) {
    command_t c;assert(xQueueReceive(s_commands,&c,0));handle(r,&c);
}
static void event_to(runtime_t *r,runtime_t *target,const char *name,const char *extra) {
    char body[8192];int n=snprintf(body,sizeof(body),
      "{\"request_id\":\"%s\",\"work_id\":\"musevoice\",\"object\":\"musevoice.event\",\"error\":{\"code\":0},\"data\":{\"protocol\":1,\"session\":\"%s\",\"generation\":%u,\"event\":\"%s\"%s}}",
      r->request_id,r->session,r->generation,name,extra?extra:"");
    assert(n>0&&(size_t)n<sizeof(body));on_frame(body,(size_t)n,target);
}
static void event(runtime_t *r,const char *name,const char *extra) {event_to(r,r,name,extra);}
static void hello(void) {
    runtime_t r;reset(&r);s_ready=false;r.operation=OP_HELLO;
    assert(send_command(&r,"hello",NULL,0));
    const char *extra=",\"ready\":true,\"asr_model\":\"sherpa-ncnn-streaming-zipformer-20M-2023-02-17\",\"tts_model\":\"melotts_zh-cn\",\"capabilities\":[\"local_asr\",\"local_tts\",\"playback_join\",\"generation_cancel\"]";
    event(&r,"ready",extra);assert(s_ready&&r.operation==OP_NONE);
    cJSON *wire=cJSON_Parse(fake_uart);assert(wire);
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(wire,"data"),"session")),"1234abcd"));
    cJSON_Delete(wire);vQueueDelete(s_commands);
}
static void speech_lifecycle(void) {
    runtime_t r;reset(&r);
    muse_module_llm_set_speech_speed(0);
    assert(muse_module_llm_speak(44,"One complete story. 🎵",70)==ESP_OK);
    assert(muse_module_llm_playback(44)==MUSE_MODULE_LLM_PLAYBACK_QUEUED);
    muse_module_llm_set_speech_speed(2); /* queued request keeps its own speed */
    drain_command(&r);assert(r.operation==OP_SPEAK);
    cJSON *wire=cJSON_Parse(fake_uart);assert(wire);
    cJSON *data=cJSON_GetObjectItem(wire,"data");
    assert(cJSON_GetObjectItem(data,"volume")->valueint==70);
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(data,"speed")),"slow"));
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(data,"text")),"One complete story. 🎵"));
    cJSON_Delete(wire);
    assert(s_status.state==MUSE_MODULE_LLM_PREPARING);
    event(&r,"done",",\"joined\":true");assert(r.operation==OP_SPEAK); /* ACK/fake early done cannot drain */
    event(&r,"accepted",NULL);event(&r,"done",",\"joined\":true");assert(r.operation==OP_SPEAK);
    event(&r,"speaking",NULL);assert(s_playback==MUSE_MODULE_LLM_PLAYBACK_ACTIVE);assert(s_status.state==MUSE_MODULE_LLM_SPEAKING);
    runtime_t stale=r;stale.generation++;event_to(&stale,&r,"done",",\"joined\":true");
    assert(s_playback==MUSE_MODULE_LLM_PLAYBACK_ACTIVE);
    event(&r,"done",",\"joined\":true");assert(s_playback==MUSE_MODULE_LLM_PLAYBACK_DONE&&r.operation==OP_NONE);
    vQueueDelete(s_commands);
}
static void cancel_before_submission(void) {
    runtime_t r;reset(&r);
    assert(muse_module_llm_speak(77,"Must never leave the queue",50)==ESP_OK);
    muse_module_llm_cancel(77);
    drain_command(&r);
    assert(fake_uart_n==0&&s_playback==MUSE_MODULE_LLM_PLAYBACK_CANCELLED);
    assert(muse_module_llm_speak(78,"New turn",50)==ESP_OK);
    muse_module_llm_cancel(77); /* late old cancel must not overwrite new cancellation mailbox */
    drain_command(&r);assert(r.operation==OP_SPEAK&&r.chat_generation==78);
    cancel_operation(&r,false);assert(s_playback==MUSE_MODULE_LLM_PLAYBACK_CANCELLING);
    event(&r,"cancelled",",\"joined\":true");assert(s_playback==MUSE_MODULE_LLM_PLAYBACK_CANCELLED);
    vQueueDelete(s_commands);
}
static void recognition_and_release(void) {
    runtime_t r;reset(&r);
    assert(muse_module_llm_listen_begin()==ESP_OK);drain_command(&r);
    assert(r.operation==OP_LISTEN_BEGIN);
    assert(muse_module_llm_listen_end()==ESP_OK);drain_command(&r);
    assert(r.end_requested&&r.operation==OP_LISTEN_BEGIN);
    event(&r,"listening",NULL);assert(r.operation==OP_LISTEN_END);
    event(&r,"transcript",",\"final\":false,\"text\":\"partial\"");assert(!s_transcript[0]);
    event(&r,"transcript",",\"final\":true,\"text\":\"Tell me a complete story\"");
    char out[2049];assert(muse_module_llm_transcript(out,sizeof(out)));
    assert(!strcmp(out,"Tell me a complete story"));assert(!muse_module_llm_transcript(out,sizeof(out)));
    /* A new press invalidates an older final even before its command is processed. */
    assert(muse_module_llm_listen_begin()==ESP_OK);drain_command(&r);event(&r,"listening",NULL);
    assert(muse_module_llm_listen_end()==ESP_OK);drain_command(&r);
    assert(muse_module_llm_listen_begin()==ESP_OK);
    event(&r,"transcript",",\"final\":true,\"text\":\"Stale previous question\"");
    assert(!muse_module_llm_transcript(out,sizeof(out)));
    drain_command(&r);vQueueDelete(s_commands);
}
static void timeout_is_failure(void) {
    runtime_t r;reset(&r);
    assert(muse_module_llm_speak(99,"Never report silence as done",90)==ESP_OK);drain_command(&r);
    event(&r,"accepted",NULL);event(&r,"speaking",NULL);
    fake_time=r.deadline;check_deadline(&r,fake_time);
    assert(r.operation==OP_CANCEL&&s_playback==MUSE_MODULE_LLM_PLAYBACK_FAILED);
    fake_time=r.deadline;check_deadline(&r,fake_time);
    assert(r.stop_unconfirmed&&!s_ready);
    assert(s_playback==MUSE_MODULE_LLM_PLAYBACK_FAILED);
    assert(muse_module_llm_listen_begin()==ESP_ERR_INVALID_STATE);
    vQueueDelete(s_commands);
}
static void malformed_and_stale(void) {
    runtime_t r;reset(&r);
    assert(muse_module_llm_speak(3,"Safe narration",30)==ESP_OK);drain_command(&r);
    runtime_t old=r;strcpy(old.session,"11111111");event_to(&old,&r,"accepted",NULL);
    assert(!r.accepted);
    old=r;strcpy(old.request_id,"old-request");event_to(&old,&r,"accepted",NULL);
    assert(!r.accepted);
    const char *native="{\"request_id\":\"whatever\",\"work_id\":\"melotts.1004\",\"object\":\"melotts.pcm\",\"error\":{\"code\":0},\"data\":\"PCM\"}";
    on_frame(native,strlen(native),&r);assert(!r.accepted);
    assert(muse_module_llm_speak(4,"Overlap",30)==ESP_ERR_TIMEOUT);
    vQueueDelete(s_commands);
}

static void active_error_requires_join(void) {
    runtime_t r;reset(&r);
    assert(muse_module_llm_speak(104,"Audio must be joined after an error",70)==ESP_OK);drain_command(&r);
    event(&r,"accepted",NULL);event(&r,"speaking",NULL);
    char body[1024];snprintf(body,sizeof(body),
      "{\"request_id\":\"%s\",\"work_id\":\"musevoice\",\"object\":\"musevoice.event\",\"error\":{\"code\":-5},\"data\":{\"protocol\":1,\"session\":\"%s\",\"generation\":%u,\"event\":\"error\"}}",
      r.request_id,r.session,r.generation);
    on_frame(body,strlen(body),&r);
    assert(r.operation==OP_CANCEL&&s_recovering&&s_playback==MUSE_MODULE_LLM_PLAYBACK_FAILED);
    assert(muse_module_llm_listen_begin()==ESP_ERR_INVALID_STATE);
    command_t queued={.kind=CMD_LISTEN_BEGIN,.record_epoch=99};
    size_t before=fake_uart_n;handle(&r,&queued);assert(fake_uart_n==before&&r.operation==OP_CANCEL);
    event(&r,"cancelled",",\"joined\":true");assert(!s_recovering&&s_ready&&r.operation==OP_NONE);
    assert(muse_module_llm_listen_begin()==ESP_OK);drain_command(&r);assert(r.operation==OP_LISTEN_BEGIN);
    vQueueDelete(s_commands);
}

static void loading_recovers_after_join(void) {
    runtime_t r;reset(&r);s_ready=false;s_recovering=true;r.stop_unconfirmed=true;
    r.operation=OP_HELLO;assert(send_command(&r,"hello",NULL,0));
    const char *extra=",\"ready\":true,\"asr_model\":\"sherpa-ncnn-streaming-zipformer-20M-2023-02-17\",\"tts_model\":\"melotts_zh-cn\",\"capabilities\":[\"local_asr\",\"local_tts\",\"playback_join\",\"generation_cancel\"]";
    event(&r,"ready",extra);
    assert(s_ready&&!s_recovering&&!r.stop_unconfirmed&&r.operation==OP_NONE);
    assert(muse_module_llm_listen_begin()==ESP_OK);
    vQueueDelete(s_commands);
}
static void heartbeat_timeout_shows_loading(void) {
    runtime_t r;reset(&r);r.operation=OP_STATUS;r.deadline=fake_time+10;
    fake_time=r.deadline;check_deadline(&r,fake_time);
    assert(!s_ready&&r.operation==OP_NONE);
    assert(s_status.state==MUSE_MODULE_LLM_DISCOVERING);
    assert(!strcmp(s_status.detail,"LOADING VOICE..."));
    assert(muse_module_llm_listen_begin()==ESP_ERR_INVALID_STATE);
    vQueueDelete(s_commands);
}
static void heartbeat_does_not_drop_speech(void) {
    runtime_t r;reset(&r);r.operation=OP_STATUS;
    assert(muse_module_llm_speak(88,"Keep this narration",40)==ESP_OK);
    drain_command(&r);assert(r.operation==OP_SPEAK);
    assert(strstr(fake_uart,"Keep this narration"));
    vQueueDelete(s_commands);
}
static void heartbeat_never_reuses_a_turn_generation(void) {
    runtime_t r;reset(&r);
    const char *extra=",\"ready\":true,\"asr_model\":\"sherpa-ncnn-streaming-zipformer-20M-2023-02-17\",\"tts_model\":\"melotts_zh-cn\",\"capabilities\":[\"local_asr\",\"local_tts\",\"playback_join\",\"generation_cancel\"]";
    uint32_t last=0;
    for(unsigned i=1;i<=20;i++) {
        r.operation=OP_STATUS;r.generation=0;
        assert(send_command(&r,"status",NULL,0));event(&r,"ready",extra);
        assert(muse_module_llm_speak(i,"A fresh conversation",40)==ESP_OK);drain_command(&r);
        assert(r.generation>last);last=r.generation;
        event(&r,"accepted",NULL);event(&r,"speaking",NULL);event(&r,"done",",\"joined\":true");
        assert(r.operation==OP_NONE);
    }
    vQueueDelete(s_commands);
}
static void meter_reports_only_fresh_valid_levels(void) {
    runtime_t r;reset(&r);r.operation=OP_LISTEN;r.generation=7;
    assert(send_command(&r,"listen_begin",NULL,0));
    event(&r,"level",",\"mic_db\":-23.5,\"mic_active\":true");
    muse_module_llm_status_t status;muse_module_llm_status(&status);
    assert(status.mic_active&&status.mic_db==-23.5f);
    event(&r,"level",",\"mic_db\":500,\"mic_active\":true");
    muse_module_llm_status(&status);assert(status.mic_db==-23.5f);
    fake_time+=1100000;muse_module_llm_status(&status);assert(!status.mic_active);
    vQueueDelete(s_commands);
}
static void delayed_reply_after_recognition_keeps_a_fresh_generation(void) {
    runtime_t r;reset(&r);
    assert(muse_module_llm_listen_begin()==ESP_OK);drain_command(&r);
    uint32_t listening_generation=r.generation;
    event(&r,"listening",NULL);
    assert(muse_module_llm_listen_end()==ESP_OK);drain_command(&r);
    event(&r,"transcript",",\"final\":true,\"text\":\"Tell me a short story\"");
    assert(r.operation==OP_NONE);
    r.operation=OP_STATUS;r.generation=0;assert(send_command(&r,"status",NULL,0));
    event(&r,"ready",",\"ready\":true,\"asr_model\":\"sherpa-ncnn-streaming-zipformer-20M-2023-02-17\",\"tts_model\":\"melotts_zh-cn\",\"capabilities\":[\"local_asr\",\"local_tts\",\"playback_join\",\"generation_cancel\"]");
    assert(muse_module_llm_speak(50,"Here is the reply after network delay.",40)==ESP_OK);drain_command(&r);
    assert(r.generation>listening_generation);
    event(&r,"accepted",NULL);event(&r,"speaking",NULL);event(&r,"done",",\"joined\":true");
    assert(muse_module_llm_playback(50)==MUSE_MODULE_LLM_PLAYBACK_DONE);
    vQueueDelete(s_commands);
}
static void word_events_follow_active_playback_only(void) {
    runtime_t r;reset(&r);
    assert(muse_module_llm_speak(88,"One complete story",70)==ESP_OK);drain_command(&r);
    event(&r,"word",",\"start\":0,\"end\":3");assert(!s_status.word_active);
    event(&r,"accepted",NULL);event(&r,"speaking",NULL);
    event(&r,"word",",\"start\":0,\"end\":3");
    assert(s_status.word_active && s_status.word_start==0 && s_status.word_end==3);
    uint32_t version=s_status.word_version;
    event(&r,"word",",\"start\":-1,\"end\":3");assert(s_status.word_version==version);
    event(&r,"word",",\"start\":1.5,\"end\":3");assert(s_status.word_version==version);
    event(&r,"word",",\"start\":2,\"end\":8193");assert(s_status.word_version==version);
    runtime_t old=r;old.generation++;event_to(&old,&r,"word",",\"start\":4,\"end\":12");
    assert(s_status.word_version==version);
    assert(s_playback==MUSE_MODULE_LLM_PLAYBACK_ACTIVE);
    event(&r,"done",",\"joined\":true");assert(!s_status.word_active);
    assert(s_playback==MUSE_MODULE_LLM_PLAYBACK_DONE);
    vQueueDelete(s_commands);
}

static void pause_controls_keep_identity_and_exclude_paused_time(void){
 runtime_t r;reset(&r);assert(muse_module_llm_speak(90,"The blue lantern glowed.",20)==ESP_OK);
 command_t command;assert(xQueueReceive(s_commands,&command,0)==pdTRUE);handle(&r,&command);
 event(&r,"accepted",NULL);event(&r,"speaking",NULL);char original[48];strcpy(original,r.request_id);
 fake_time=1000000;muse_module_llm_toggle_pause();fake_uart_n=0;fake_uart[0]=0;playback_controls(&r,fake_time);
 assert(!strcmp(original,r.request_id));cJSON *wire=cJSON_Parse(fake_uart);assert(wire);
 assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(wire,"action")),"control"));
 const cJSON *d=cJSON_GetObjectItem(wire,"data");assert(cJSON_IsTrue(cJSON_GetObjectItem(d,"paused")));assert(cJSON_GetNumberValue(cJSON_GetObjectItem(d,"generation"))==r.generation);cJSON_Delete(wire);
 event(&r,"playback",",\"paused\":true");assert(s_status.paused && r.pause_since);
 fake_time+=20LL*60*1000000;check_deadline(&r,fake_time);assert(r.operation==OP_SPEAK);
 muse_module_llm_toggle_pause();event(&r,"playback",",\"paused\":false");assert(!s_status.paused && !r.pause_since && r.deadline>fake_time);
 event(&r,"done",",\"joined\":true");assert(r.operation==OP_NONE && !s_status.pause_requested);vQueueDelete(s_commands);
}
int main(int argc,char **argv) {
    assert(argc==2);
    switch(atoi(argv[1])) {
      case 0:hello();break;
      case 1:speech_lifecycle();break;
      case 2:cancel_before_submission();break;
      case 3:recognition_and_release();break;
      case 4:timeout_is_failure();break;
      case 5:malformed_and_stale();break;
      case 6:active_error_requires_join();break;
      case 7:loading_recovers_after_join();break;
      case 8:heartbeat_timeout_shows_loading();break;
      case 9:heartbeat_does_not_drop_speech();break;
      case 10:heartbeat_never_reuses_a_turn_generation();break;
      case 11:meter_reports_only_fresh_valid_levels();break;
      case 12:delayed_reply_after_recognition_keeps_a_fresh_generation();break;
      case 13:word_events_follow_active_playback_only();break;
      case 14:pause_controls_keep_identity_and_exclude_paused_time();break;
      case 15:{
    s_ready=true;fake_recorder=true;assert(muse_module_llm_listen_begin()==ESP_ERR_INVALID_STATE);assert(muse_module_llm_speak(90,"Wait until recorder stops",20)==ESP_ERR_TIMEOUT);assert(!fake_uart_n);break;
 }
 default:return 2;
    }
    return 0;
}
'''


class ModuleRuntime(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        out = Path(cls.tmp.name)
        (out / 'fake_common.h').write_text(COMMON)
        for name in ['driver/uart.h', 'esp_err.h', 'esp_heap_caps.h', 'esp_log.h', 'esp_random.h',
                     'esp_timer.h', 'freertos/FreeRTOS.h', 'freertos/queue.h', 'freertos/task.h', 'sdkconfig.h']:
            path = out / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#include "fake_common.h"\n')
        (out / 'harness.c').write_text(HARNESS)
        cls.binary = out / 'runtime'
        command = [*shlex.split(os.environ.get('CC', 'cc')), '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                   '-I', str(out), '-I', str(JSON), '-I', str(ROOT / 'components/muse'),
                   str(out / 'harness.c'), str(ROOT / 'components/muse/muse_module_llm_protocol.c'),
                   str(JSON / 'cJSON.c'), '-lm', '-o', str(cls.binary)]
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_case(self, number):
        result = subprocess.run([str(self.binary), str(number)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_ready_requires_model_and_native_join_capabilities(self): self.run_case(0)
    def test_full_text_volume_and_explicit_joined_completion(self): self.run_case(1)
    def test_cancel_invalidates_queued_text_and_waits_for_join(self): self.run_case(2)
    def test_release_waits_for_final_and_new_press_discards_old_transcript(self): self.run_case(3)
    def test_timeout_never_means_done_or_microphone_permission(self): self.run_case(4)
    def test_old_sessions_and_native_inference_ack_cannot_advance_playback(self): self.run_case(5)

    def test_active_error_blocks_new_work_until_confirmed_join(self): self.run_case(6)
    def test_joined_hello_recovers_after_module_restart(self): self.run_case(7)
    def test_heartbeat_timeout_shows_loading_and_blocks_mic(self): self.run_case(8)
    def test_heartbeat_never_drops_queued_speech(self): self.run_case(9)
    def test_twenty_conversations_remain_unique_across_idle_heartbeats(self): self.run_case(10)
    def test_real_meter_values_are_bounded_and_expire(self): self.run_case(11)
    def test_microphone_then_network_delay_then_spoken_reply(self): self.run_case(12)

    def test_word_events_are_bounded_and_cannot_finish_or_resume_playback(self): self.run_case(13)

    def test_pause_controls_keep_original_identity_and_do_not_expire_while_paused(self): self.run_case(14)

    def test_recorder_blocks_ptt_and_defers_tts_without_native_cancellation(self):self.run_case(15)
