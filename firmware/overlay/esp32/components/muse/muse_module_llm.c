/* SPDX-License-Identifier: Apache-2.0 */
#include "muse_module_llm.h"
#include "muse_memory.h"
#include "muse_module_llm_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "muse_local_speech";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_started, s_ready, s_recovering;
static bool s_monitor, s_monitor_changed;
static int s_mic_level = 100;
static int s_speech_speed=1;
static int s_live_volume=-1;
static bool s_control_dirty;
void muse_module_llm_set_speech_speed(int speed){portENTER_CRITICAL(&s_lock);s_speech_speed=speed<0?0:speed>2?2:speed;portEXIT_CRITICAL(&s_lock);}

void muse_module_llm_set_mic_level(int pct)
{
    portENTER_CRITICAL(&s_lock);
    s_mic_level = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    portEXIT_CRITICAL(&s_lock);
}
static int64_t s_meter_at;
static QueueHandle_t s_commands;
static uint32_t s_play_generation, s_cancel_generation, s_record_epoch;
static bool s_cancel_requested, s_cancel_valid;
static muse_module_llm_playback_t s_playback;
static char s_transcript[MUSE_LLM_TRANSCRIPT_MAX + 1];
static muse_module_llm_status_t s_status = {
    .state = MUSE_MODULE_LLM_DISCOVERING,
    .detail = "LOADING VOICE...",
};

typedef enum { CMD_LISTEN_BEGIN, CMD_LISTEN_END, CMD_SPEAK, CMD_CANCEL } command_kind_t;
typedef struct {
    command_kind_t kind;
    uint32_t chat_generation, record_epoch;
    char *text;
    unsigned volume,speech_speed;
} command_t;
typedef enum { OP_NONE, OP_HELLO, OP_STATUS, OP_LISTEN_BEGIN, OP_LISTEN, OP_LISTEN_END, OP_SPEAK, OP_CANCEL } operation_t;
typedef struct {
    muse_llm_framer_t framer;
    operation_t operation;
    uint32_t nonce, serial, generation, generation_counter, chat_generation, record_epoch;
    char request_id[48], session[9];
    int64_t deadline, retry_at, next_status_at, waiting_since, pause_since, control_at;
    unsigned speech_speed;
    bool accepted, speaking, end_requested, cancel_failed, stop_unconfirmed, no_speech;
} runtime_t;

static const char *waiting_detail(runtime_t *r, int64_t now)
{
    return r->waiting_since && now - r->waiting_since >= 30000000LL ?
        "WAITING FOR VOICE MODULE..." : "LOADING VOICE...";
}

static void set_status(muse_module_llm_state_t state, const char *detail)
{
    portENTER_CRITICAL(&s_lock);
    s_status.state = state;
    strlcpy(s_status.detail, detail, sizeof(s_status.detail));
    portEXIT_CRITICAL(&s_lock);
}

static void set_playback(muse_module_llm_playback_t state)
{
    portENTER_CRITICAL(&s_lock);
    s_playback = state;
    if(state!=MUSE_MODULE_LLM_PLAYBACK_ACTIVE && state!=MUSE_MODULE_LLM_PLAYBACK_QUEUED){s_status.paused=false;s_status.pause_requested=false;s_control_dirty=false;}
    if (state != MUSE_MODULE_LLM_PLAYBACK_ACTIVE && s_status.word_active) {
        s_status.word_active = false;
        s_status.word_version++;
    }
    portEXIT_CRITICAL(&s_lock);
}

void muse_module_llm_status(muse_module_llm_status_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_lock);
    *out = s_status;
    if (esp_timer_get_time() - s_meter_at > 1000000LL) out->mic_active = false;
    portEXIT_CRITICAL(&s_lock);
}

void muse_module_llm_set_playback_volume(int volume)
{
    if(volume<0 || volume>100)return;
    portENTER_CRITICAL(&s_lock);if(volume!=s_live_volume){s_live_volume=volume;s_control_dirty=true;}portEXIT_CRITICAL(&s_lock);
}
void muse_module_llm_toggle_pause(void)
{
    portENTER_CRITICAL(&s_lock);
    if(s_playback==MUSE_MODULE_LLM_PLAYBACK_ACTIVE || s_playback==MUSE_MODULE_LLM_PLAYBACK_QUEUED){s_status.pause_requested=!s_status.pause_requested;s_control_dirty=true;}
    portEXIT_CRITICAL(&s_lock);
}

void muse_module_llm_set_monitor(bool on)
{
    portENTER_CRITICAL(&s_lock);
    if (s_monitor != on) s_monitor_changed = true;
    s_monitor = on;
    portEXIT_CRITICAL(&s_lock);
}

muse_module_llm_playback_t muse_module_llm_playback(uint32_t generation)
{
    portENTER_CRITICAL(&s_lock);
    muse_module_llm_playback_t state = generation == s_play_generation ? s_playback : MUSE_MODULE_LLM_PLAYBACK_NONE;
    portEXIT_CRITICAL(&s_lock);
    return state;
}

bool muse_module_llm_transcript(char *out, size_t cap)
{
    if (!out || !cap) return false;
    bool found = false;
    portENTER_CRITICAL(&s_lock);
    size_t length = strlen(s_transcript);
    if (length && cap > length) {
        memcpy(out, s_transcript, length + 1);
        s_transcript[0] = '\0';
        found = true;
    }
    portEXIT_CRITICAL(&s_lock);
    return found;
}

static bool send_command(runtime_t *r, const char *action, const char *text, unsigned volume)
{
    snprintf(r->request_id, sizeof(r->request_id), "muse-%08lx-%08lx", (unsigned long)r->nonce, (unsigned long)++r->serial);
    char *json = muse_llm_bridge_request(r->request_id, r->session, action, r->generation, text, volume);
    if (!json) return false;
    if (!strcmp(action, "status") || !strcmp(action, "listen_begin")) {
        cJSON *root = cJSON_Parse(json);
        cJSON_free(json);
        if (!root) return false;
        portENTER_CRITICAL(&s_lock); bool monitor = s_monitor; portEXIT_CRITICAL(&s_lock);
        cJSON_AddBoolToObject(cJSON_GetObjectItemCaseSensitive(root, "data"), "monitor", monitor);
        portENTER_CRITICAL(&s_lock); int mic_level = s_mic_level; portEXIT_CRITICAL(&s_lock);
        cJSON_AddNumberToObject(cJSON_GetObjectItemCaseSensitive(root, "data"), "mic_level", mic_level);
        json = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (!json) return false;
    }
    if(!strcmp(action,"speak")){
        cJSON *root=cJSON_Parse(json);cJSON_free(json);if(!root)return false;
        static const char *names[]={"slow","normal","fast"};
        cJSON_AddStringToObject(cJSON_GetObjectItemCaseSensitive(root,"data"),"speed",names[r->speech_speed<=2?r->speech_speed:1]);
        json=cJSON_PrintUnformatted(root);cJSON_Delete(root);if(!json)return false;
    }
    size_t length = strlen(json);
    bool sent = uart_write_bytes(CONFIG_MUSE_MODULE_LLM_UART_NUM, json, length) == (int)length &&
        uart_write_bytes(CONFIG_MUSE_MODULE_LLM_UART_NUM, "\n", 1) == 1;
    cJSON_free(json);
    return sent;
}

static void fail(runtime_t *r, const char *detail)
{
    if (r->operation == OP_SPEAK || r->operation == OP_CANCEL) set_playback(MUSE_MODULE_LLM_PLAYBACK_FAILED);
    r->operation = OP_NONE;
    r->deadline = 0;
    set_status(MUSE_MODULE_LLM_ERROR, detail);
}

static void cancel_operation(runtime_t *r, bool failed);

static void end_listening(runtime_t *r)
{
    if (r->operation == OP_LISTEN_BEGIN) {
        r->end_requested = true;
        return;
    }
    if (r->operation != OP_LISTEN) return;
    r->operation = OP_LISTEN_END;
    r->deadline = esp_timer_get_time() + 15000000LL;
    set_status(MUSE_MODULE_LLM_FINALIZING, "FINISHING YOUR WORDS...");
    if (!send_command(r, "listen_end", NULL, 0)) cancel_operation(r, true);
}

static void on_frame(const char *frame, size_t length, void *context)
{
    runtime_t *r = context;
    muse_llm_reply_t reply;
    if (!muse_llm_reply_parse(frame, length, &reply)) return;
    if (muse_memory_reply(reply.root, r->session)) { muse_llm_reply_free(&reply); return; }
    muse_llm_bridge_event_t event;
    if (!muse_llm_bridge_event(&reply, r->session, r->request_id, r->generation, &event)) {
        muse_llm_reply_free(&reply);
        return;
    }
    const cJSON *db = cJSON_GetObjectItemCaseSensitive(reply.data, "mic_db");
    const cJSON *active = cJSON_GetObjectItemCaseSensitive(reply.data, "mic_active");
    if (cJSON_IsNumber(db) && isfinite(db->valuedouble) && db->valuedouble >= -100 && db->valuedouble <= 0) {
        portENTER_CRITICAL(&s_lock);
        s_status.mic_db = (float)db->valuedouble;
        s_status.mic_active = cJSON_IsTrue(active);
        s_meter_at = esp_timer_get_time();
        portEXIT_CRITICAL(&s_lock);
    }
    if (event.kind == MUSE_LLM_EVENT_LEVEL) {
        muse_llm_reply_free(&reply);
        return;
    }
    if(event.kind==MUSE_LLM_EVENT_PLAYBACK){
        const cJSON *paused=cJSON_GetObjectItemCaseSensitive(reply.data,"paused");
        if(r->operation==OP_SPEAK && cJSON_IsBool(paused)){
            int64_t now=esp_timer_get_time();bool on=cJSON_IsTrue(paused);
            if(on && !r->pause_since)r->pause_since=now;
            if(!on && r->pause_since){r->deadline+=now-r->pause_since;r->pause_since=0;}
            portENTER_CRITICAL(&s_lock);s_status.paused=on;portEXIT_CRITICAL(&s_lock);
            if(r->speaking)set_status(MUSE_MODULE_LLM_SPEAKING,on?"PAUSED":"SPEAKING...");
        }
        muse_llm_reply_free(&reply);return;
    }
    if (event.kind == MUSE_LLM_EVENT_WORD) {
        if (r->operation == OP_SPEAK && r->accepted && r->speaking) {
            portENTER_CRITICAL(&s_lock);
            s_status.word_start = event.word_start;
            s_status.word_end = event.word_end;
            s_status.word_active = true;
            s_status.word_version++;
            portEXIT_CRITICAL(&s_lock);
        }
        muse_llm_reply_free(&reply);
        return;
    }
    if (event.kind == MUSE_LLM_EVENT_ERROR && r->operation != OP_NONE) {
        /* An error is not a proof of speaker stop. Recover by joining, and
         * block new work until cancellation is confirmed. */
        ESP_LOGW(TAG, "local request failed: op=%u code=%d", (unsigned)r->operation, reply.error);
        portENTER_CRITICAL(&s_lock); s_status.last_error = reply.error; portEXIT_CRITICAL(&s_lock);
        if (r->operation == OP_CANCEL) {
            r->stop_unconfirmed = true;
            portENTER_CRITICAL(&s_lock); s_ready = false; portEXIT_CRITICAL(&s_lock);
            fail(r, "WAITING FOR VOICE MODULE...");
            set_status(MUSE_MODULE_LLM_DISCOVERING, "WAITING FOR VOICE MODULE...");
        } else if (r->operation == OP_HELLO || r->operation == OP_STATUS) {
            portENTER_CRITICAL(&s_lock); s_ready = false; portEXIT_CRITICAL(&s_lock);
            fail(r, "LOADING VOICE...");
            set_status(MUSE_MODULE_LLM_DISCOVERING, waiting_detail(r, esp_timer_get_time()));
            /* A reboot or a previous client may retire the old session. */
            if (reply.error == -21) {
                r->nonce = esp_random();
                snprintf(r->session, sizeof(r->session), "%08lx", (unsigned long)r->nonce);
            }
            r->retry_at = esp_timer_get_time() + 2000000LL;
        } else {
            r->no_speech = (r->operation == OP_LISTEN || r->operation == OP_LISTEN_END) && reply.error == -12;
            set_playback(MUSE_MODULE_LLM_PLAYBACK_FAILED);
            cancel_operation(r, true);
        }
    } else if ((r->operation == OP_HELLO || r->operation == OP_STATUS) && event.kind == MUSE_LLM_EVENT_READY) {
        if (muse_llm_bridge_ready(&reply, CONFIG_MUSE_MODULE_LLM_ASR_MODEL, CONFIG_MUSE_MODULE_LLM_TTS_MODEL)) {
            portENTER_CRITICAL(&s_lock);
            s_ready = true;
            s_recovering = false;
            s_status.last_error = 0;
            s_status.asr_model_found = s_status.tts_model_found = true;
            strlcpy(s_status.version, "musevoice/1", sizeof(s_status.version));
            portEXIT_CRITICAL(&s_lock);
            r->operation = OP_NONE;
            r->stop_unconfirmed = false;
            r->waiting_since = 0;
            portENTER_CRITICAL(&s_lock); bool monitoring = s_monitor; portEXIT_CRITICAL(&s_lock);
            r->next_status_at = esp_timer_get_time() + (monitoring ? 200000LL : 3000000LL);
            r->deadline = 0;
            set_status(MUSE_MODULE_LLM_READY, "HOLD TO TALK");
            ESP_LOGI(TAG, "bridge ready: protocol=1");
        } else {
            fail(r, "LOCAL SPEECH SETUP DOES NOT MATCH");
            r->retry_at = esp_timer_get_time() + 5000000LL;
        }
    } else if (r->operation == OP_LISTEN_BEGIN && event.kind == MUSE_LLM_EVENT_LISTENING) {
        /* This event is sent after any older native playback was joined. */
        portENTER_CRITICAL(&s_lock);
        if (s_playback == MUSE_MODULE_LLM_PLAYBACK_CANCELLING)
            s_playback = MUSE_MODULE_LLM_PLAYBACK_CANCELLED;
        portEXIT_CRITICAL(&s_lock);
        r->operation = OP_LISTEN;
        r->deadline = esp_timer_get_time() + 90000000LL;
        set_status(MUSE_MODULE_LLM_LISTENING, "LISTENING...");
        if (r->end_requested) end_listening(r);
    } else if (r->operation == OP_LISTEN_END && event.kind == MUSE_LLM_EVENT_TRANSCRIPT) {
        portENTER_CRITICAL(&s_lock);
        if (r->record_epoch == s_record_epoch)
            strlcpy(s_transcript, event.text, sizeof(s_transcript));
        portEXIT_CRITICAL(&s_lock);
        r->operation = OP_NONE;
        r->deadline = 0;
        set_status(MUSE_MODULE_LLM_READY, "HOLD TO TALK");
    } else if (r->operation == OP_SPEAK) {
        if (event.kind == MUSE_LLM_EVENT_ACCEPTED && !r->accepted) {
            r->accepted = true;
        } else if (event.kind == MUSE_LLM_EVENT_SPEAKING && r->accepted) {
            r->speaking = true;
            set_playback(MUSE_MODULE_LLM_PLAYBACK_ACTIVE);
            set_status(MUSE_MODULE_LLM_SPEAKING, "SPEAKING...");
        } else if (event.kind == MUSE_LLM_EVENT_DONE && r->accepted && r->speaking) {
            /* Never an inference ACK or an estimated duration. */
            set_playback(MUSE_MODULE_LLM_PLAYBACK_DONE);
            r->operation = OP_NONE;
            r->deadline = 0;
            set_status(MUSE_MODULE_LLM_READY, "HOLD TO TALK");
        }
    } else if (r->operation == OP_CANCEL && event.kind == MUSE_LLM_EVENT_CANCELLED) {
        if (!r->cancel_failed) set_playback(MUSE_MODULE_LLM_PLAYBACK_CANCELLED);
        portENTER_CRITICAL(&s_lock); s_recovering = false; portEXIT_CRITICAL(&s_lock);
        r->operation = OP_NONE;
        r->deadline = 0;
        set_status(r->cancel_failed && !r->no_speech ? MUSE_MODULE_LLM_ERROR : MUSE_MODULE_LLM_READY,
                   r->no_speech ? "NO SPEECH HEARD; HOLD TO TALK" :
                   r->cancel_failed ? "LOCAL SPEECH FAILED; TRY AGAIN" : "HOLD TO TALK");
    }
    muse_llm_reply_free(&reply);
}

static void new_generation(runtime_t *r)
{
    portENTER_CRITICAL(&s_lock);
    s_status.word_active = false;
    s_status.word_version++;
    portEXIT_CRITICAL(&s_lock);
    /* Passive status/hello uses wire generation zero, never the turn counter.
     * Reusing a previous turn ID is rejected by the module's replay guard. */
    if (!++r->generation_counter) ++r->generation_counter;
    r->generation = r->generation_counter;
    portENTER_CRITICAL(&s_lock); s_status.issued_generations = r->generation_counter; portEXIT_CRITICAL(&s_lock);
    r->accepted = r->speaking = r->end_requested = r->cancel_failed = false;
    r->no_speech = false;
}

static void cancel_operation(runtime_t *r, bool failed)
{
    r->operation = OP_CANCEL;
    r->cancel_failed = failed;
    portENTER_CRITICAL(&s_lock); s_recovering = failed; portEXIT_CRITICAL(&s_lock);
    r->deadline = esp_timer_get_time() + 10000000LL;
    if (!failed) set_playback(MUSE_MODULE_LLM_PLAYBACK_CANCELLING);
    set_status(MUSE_MODULE_LLM_CANCELLING, "STOPPING LOCAL AUDIO...");
    if (!send_command(r, "cancel", NULL, 0)) {
        r->stop_unconfirmed = true;
        portENTER_CRITICAL(&s_lock); s_ready = false; portEXIT_CRITICAL(&s_lock);
        fail(r, "WAITING FOR VOICE MODULE...");
        set_status(MUSE_MODULE_LLM_DISCOVERING, "WAITING FOR VOICE MODULE...");
    }
}

static void handle(runtime_t *r, command_t *command)
{
    if (r->stop_unconfirmed || (r->operation == OP_CANCEL && r->cancel_failed)) {
        free(command->text);
        return;
    }
    /* A read-only heartbeat must never reject a newly queued voice request. */
    if (r->operation == OP_STATUS) {
        r->operation = OP_NONE;
        r->deadline = 0;
    }
    switch (command->kind) {
    case CMD_LISTEN_BEGIN:
        r->record_epoch = command->record_epoch;
        if (r->operation == OP_SPEAK || r->operation == OP_CANCEL)
            set_playback(MUSE_MODULE_LLM_PLAYBACK_CANCELLING);
        new_generation(r);
        r->operation = OP_LISTEN_BEGIN;
        r->deadline = esp_timer_get_time() + 90000000LL;
        portENTER_CRITICAL(&s_lock); s_transcript[0] = '\0'; portEXIT_CRITICAL(&s_lock);
        set_status(MUSE_MODULE_LLM_CANCELLING, "STARTING MICROPHONE...");
        if (!send_command(r, "listen_begin", NULL, 0)) cancel_operation(r, true);
        break;
    case CMD_LISTEN_END:
        end_listening(r);
        break;
    case CMD_SPEAK:
        portENTER_CRITICAL(&s_lock);
        bool cancelled = s_cancel_valid && s_cancel_generation == command->chat_generation;
        portEXIT_CRITICAL(&s_lock);
        if (cancelled) {
            set_playback(MUSE_MODULE_LLM_PLAYBACK_CANCELLED);
            break;
        }
        if (r->operation != OP_NONE) {
            set_playback(MUSE_MODULE_LLM_PLAYBACK_FAILED);
            break;
        }
        new_generation(r);
        r->operation = OP_SPEAK;
        r->pause_since=0;r->control_at=0;
        r->chat_generation = command->chat_generation;
        r->speech_speed=command->speech_speed;
        /* A failure ceiling, never a signal that the speaker finished. */
        r->deadline = esp_timer_get_time() + 12LL * 60 * 1000000;
        set_status(MUSE_MODULE_LLM_PREPARING, "PREPARING VOICE...");
        if (!send_command(r, "speak", command->text, command->volume)) {
            set_playback(MUSE_MODULE_LLM_PLAYBACK_FAILED);
            cancel_operation(r, true);
        }
        break;
    case CMD_CANCEL:
        if (r->operation == OP_SPEAK && r->chat_generation == command->chat_generation)
            cancel_operation(r, false);
        break;
    }
    free(command->text);
}

static void playback_controls(runtime_t *r,int64_t now)
{
if(r->operation==OP_SPEAK && r->accepted){
    portENTER_CRITICAL(&s_lock);bool dirty=s_control_dirty,paused=s_status.pause_requested,actual=s_status.paused;int volume=s_live_volume;portEXIT_CRITICAL(&s_lock);
    if(volume>=0 && (dirty || paused!=actual || (r->control_at && now-r->control_at>=3000000)) && now-r->control_at>=150000){
        cJSON *root=cJSON_CreateObject(),*d=root?cJSON_AddObjectToObject(root,"data"):NULL;
        if(d){char id[48];snprintf(id,sizeof(id),"ctl-%08lx-%08lx",(unsigned long)r->nonce,(unsigned long)++r->serial);
            cJSON_AddStringToObject(root,"request_id",id);cJSON_AddStringToObject(root,"work_id","musevoice");cJSON_AddStringToObject(root,"object","musevoice.v1");cJSON_AddStringToObject(root,"action","control");
            cJSON_AddStringToObject(d,"session",r->session);cJSON_AddNumberToObject(d,"generation",r->generation);cJSON_AddBoolToObject(d,"paused",paused);cJSON_AddNumberToObject(d,"volume",volume);
            char *json=cJSON_PrintUnformatted(root);if(json){size_t n=strlen(json);bool ok=uart_write_bytes(CONFIG_MUSE_MODULE_LLM_UART_NUM,json,n)==(int)n && uart_write_bytes(CONFIG_MUSE_MODULE_LLM_UART_NUM,"\n",1)==1;cJSON_free(json);
                if(ok){portENTER_CRITICAL(&s_lock);if(volume==s_live_volume && paused==s_status.pause_requested)s_control_dirty=false;portEXIT_CRITICAL(&s_lock);r->control_at=now;}}
        }cJSON_Delete(root);
    }
}
}

static void check_deadline(runtime_t *r, int64_t now)
{
    if(r->operation==OP_SPEAK && r->pause_since)return;
    if (!r->deadline || now < r->deadline) return;
    if (r->operation == OP_LISTEN) end_listening(r);
    else if (r->operation == OP_HELLO || r->operation == OP_STATUS) {
        portENTER_CRITICAL(&s_lock); s_ready = false; portEXIT_CRITICAL(&s_lock);
        fail(r, "LOADING VOICE...");
        set_status(MUSE_MODULE_LLM_DISCOVERING, waiting_detail(r, now));
        r->retry_at = now + 2000000LL;
    } else if (r->operation == OP_CANCEL) {
        r->stop_unconfirmed = true;
        portENTER_CRITICAL(&s_lock); s_ready = false; portEXIT_CRITICAL(&s_lock);
        fail(r, "WAITING FOR VOICE MODULE...");
        set_status(MUSE_MODULE_LLM_DISCOVERING, "WAITING FOR VOICE MODULE...");
    } else {
        set_playback(MUSE_MODULE_LLM_PLAYBACK_FAILED);
        cancel_operation(r, true);
    }
}

static void runtime_task(void *unused)
{
    (void)unused;
    runtime_t r = {.nonce = esp_random()};
    snprintf(r.session, sizeof(r.session), "%08lx", (unsigned long)r.nonce);
    char *frame = heap_caps_malloc(MUSE_LLM_FRAME_MAX + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!frame) {
        set_status(MUSE_MODULE_LLM_UNAVAILABLE, "LOCAL SPEECH MEMORY UNAVAILABLE");
        vTaskDelete(NULL);
        return;
    }
    muse_llm_framer_init(&r.framer, frame, MUSE_LLM_FRAME_MAX + 1);
    for (;;) {
        int64_t now = esp_timer_get_time();
        portENTER_CRITICAL(&s_lock); bool ready = s_ready; portEXIT_CRITICAL(&s_lock);
        portENTER_CRITICAL(&s_lock);
        bool monitor = s_monitor, monitor_changed = s_monitor_changed;
        s_monitor_changed = false;
        portEXIT_CRITICAL(&s_lock);
        if (monitor_changed) r.next_status_at = now;
        /* Hello joins old audio before it acknowledges readiness. It is safe
         * to retry after a module reboot even when the old stop was lost. */
        if (!ready && r.operation == OP_NONE && now >= r.retry_at) {
            if (!r.waiting_since) r.waiting_since = now;
            r.operation = OP_HELLO;
            r.generation = 0;
            r.deadline = now + 10000000LL;
            set_status(MUSE_MODULE_LLM_DISCOVERING, waiting_detail(&r, now));
            if (!send_command(&r, "hello", NULL, 0)) fail(&r, "WAITING FOR VOICE MODULE...");
            r.retry_at = now + 15000000LL;
        }
        if (ready && r.operation == OP_NONE && now >= r.next_status_at &&
            uxQueueMessagesWaiting(s_commands) == 0) {
            r.operation = OP_STATUS;
            r.generation = 0;
            r.deadline = now + 3000000LL;
            r.next_status_at = now + (monitor ? 200000LL : 3000000LL);
            if (!send_command(&r, "status", NULL, 0)) {
                portENTER_CRITICAL(&s_lock); s_ready = false; portEXIT_CRITICAL(&s_lock);
                fail(&r, "WAITING FOR VOICE MODULE...");
            }
        }
        portENTER_CRITICAL(&s_lock);
        bool cancel = s_cancel_requested;
        uint32_t cancel_generation = s_cancel_generation;
        s_cancel_requested = false;
        portEXIT_CRITICAL(&s_lock);
        if (cancel && r.operation == OP_SPEAK && r.chat_generation == cancel_generation)
            cancel_operation(&r, false);
        playback_controls(&r,now);
        command_t command;
        while (xQueueReceive(s_commands, &command, 0) == pdTRUE) handle(&r, &command);
        unsigned char bytes[512];
        int n = uart_read_bytes(CONFIG_MUSE_MODULE_LLM_UART_NUM, bytes, sizeof(bytes), pdMS_TO_TICKS(20));
        if (n > 0) muse_llm_framer_feed(&r.framer, bytes, (size_t)n, on_frame, &r);
        now = esp_timer_get_time();
        check_deadline(&r, now);
        muse_memory_tick(r.session, ready && (r.operation == OP_NONE || (r.operation == OP_SPEAK && !r.speaking)) && uxQueueMessagesWaiting(s_commands) == 0);
    }
}

static bool ready(void)
{
    portENTER_CRITICAL(&s_lock); bool value = s_ready && !s_recovering; portEXIT_CRITICAL(&s_lock);
    return value;
}

static esp_err_t post(command_t *command)
{
    if (!s_commands || !ready()) return ESP_ERR_INVALID_STATE;
    return xQueueSend(s_commands, command, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t muse_module_llm_start(void)
{
    if (s_started) return ESP_OK;
    if (!muse_llm_model_name_valid(CONFIG_MUSE_MODULE_LLM_ASR_MODEL) ||
        !muse_llm_model_name_valid(CONFIG_MUSE_MODULE_LLM_TTS_MODEL)) {
        set_status(MUSE_MODULE_LLM_NEEDS_MODELS, "LOCAL SPEECH MODELS NEED SETUP");
        return ESP_ERR_INVALID_ARG;
    }
    uart_config_t config = {.baud_rate = CONFIG_MUSE_MODULE_LLM_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT};
    esp_err_t err = uart_param_config(CONFIG_MUSE_MODULE_LLM_UART_NUM, &config);
    if (err != ESP_OK) return err;
    err = uart_set_pin(CONFIG_MUSE_MODULE_LLM_UART_NUM, CONFIG_MUSE_MODULE_LLM_TX_GPIO,
        CONFIG_MUSE_MODULE_LLM_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) return err;
    err = uart_driver_install(CONFIG_MUSE_MODULE_LLM_UART_NUM, 8192, 0, 0, NULL, 0);
    if (err != ESP_OK) return err;
    s_commands = xQueueCreate(8, sizeof(command_t));
    if (!s_commands || xTaskCreate(runtime_task, "muse_local_uart", 6144, NULL, 4, NULL) != pdPASS) {
        if (s_commands) vQueueDelete(s_commands);
        s_commands = NULL;
        uart_driver_delete(CONFIG_MUSE_MODULE_LLM_UART_NUM);
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    return ESP_OK;
}

esp_err_t muse_module_llm_listen_begin(void)
{
    if(muse_memory_recorder_active())return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_lock);
    uint32_t epoch = ++s_record_epoch;
    s_transcript[0] = '\0';
    portEXIT_CRITICAL(&s_lock);
    command_t command = {.kind = CMD_LISTEN_BEGIN, .record_epoch = epoch};
    return post(&command);
}

esp_err_t muse_module_llm_listen_end(void)
{
    command_t command = {.kind = CMD_LISTEN_END};
    return post(&command);
}

esp_err_t muse_module_llm_speak(uint32_t generation, const char *text, unsigned volume)
{
    if (!muse_llm_text_valid(text, MUSE_LLM_TEXT_MAX) || volume > 100) return ESP_ERR_INVALID_ARG;
    if(muse_memory_recorder_active())return ESP_ERR_TIMEOUT;
    if (!ready()) return ESP_ERR_INVALID_STATE;
    char *copy = strdup(text);
    if (!copy) return ESP_ERR_NO_MEM;
    portENTER_CRITICAL(&s_lock);
    bool busy = s_playback == MUSE_MODULE_LLM_PLAYBACK_QUEUED || s_playback == MUSE_MODULE_LLM_PLAYBACK_ACTIVE ||
        s_playback == MUSE_MODULE_LLM_PLAYBACK_CANCELLING;
    if (!busy) {
        s_play_generation = generation;
        s_playback = MUSE_MODULE_LLM_PLAYBACK_QUEUED;
        s_status.paused=false;s_status.pause_requested=false;s_live_volume=(int)volume;s_control_dirty=false;
    }
    portEXIT_CRITICAL(&s_lock);
    if (busy) { free(copy); return ESP_ERR_TIMEOUT; }
    portENTER_CRITICAL(&s_lock);unsigned speech_speed=s_speech_speed;portEXIT_CRITICAL(&s_lock);
    command_t command = {.speech_speed=speech_speed,.kind = CMD_SPEAK, .chat_generation = generation, .text = copy, .volume = volume};
    esp_err_t result = post(&command);
    if (result != ESP_OK) { free(copy); set_playback(MUSE_MODULE_LLM_PLAYBACK_FAILED); }
    return result;
}

void muse_module_llm_cancel(uint32_t generation)
{
    /* Cancellation has a dedicated mailbox, so a full command queue cannot
     * drop it. Its generation also invalidates an unsent queued speak. */
    portENTER_CRITICAL(&s_lock);
    if (s_play_generation == generation &&
        (s_playback == MUSE_MODULE_LLM_PLAYBACK_QUEUED || s_playback == MUSE_MODULE_LLM_PLAYBACK_ACTIVE ||
         s_playback == MUSE_MODULE_LLM_PLAYBACK_CANCELLING)) {
        s_cancel_generation = generation;
        s_cancel_valid = s_cancel_requested = true;
        s_playback = MUSE_MODULE_LLM_PLAYBACK_CANCELLING;
    }
    portEXIT_CRITICAL(&s_lock);
}
