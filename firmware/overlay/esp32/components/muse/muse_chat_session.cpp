/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * The Muse chat session: one HTTP-over-Noise connection to the VM (the transport
 * Home Link uses: TLS, WebSocket upgrade on /v1/noise, Noise handshake, then
 * multiplexed request streams), and the push-to-talk turns that run over it.
 *
 * A turn:
 *   1. POST /api/voice/dictation, request body left open; the mic is streamed
 *      up as 24 kHz PCM16 while the button is held, then half-closed. The VM
 *      answers with NDJSON transcripts (cumulative partials, then a final).
 *   2. POST /chat/stream with the transcript. The reply arrives as events on
 *      the connection's long-lived POST /chat/subscribe stream: one or more
 *      assistant messages, each delta.message_start / text_append / message_done.
 *   3. Each finished message is shown at reading pace (see start_tts to
 *      speak it with a TTS API of your own; Muse doesn't speak gadget replies).
 * A turn has no explicit end event; like Sidekick, it settles once every
 * message is done and nothing has arrived for a few seconds.
 *
 * Everything network-side runs on one task. The voice task talks to it through
 * a command queue, a stream buffer of mic audio, an event queue (captions) and
 * a stream buffer of reply audio. A generation number tags each turn so that
 * events and audio of a cancelled turn are dropped.
 *
 * A typed turn (muse_hatch_text_turn, from the serial console) skips steps 1
 * and 3: the text goes to /chat/stream and the reply streams back to the
 * console as "@chat" lines instead of to the voice task.
 */

#include <atomic>
#include <ctime>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

extern "C" {
#include "cJSON.h"
#include "minimp3.h"
#include "muse_account_api.h"
#include "muse_link.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "freertos/semphr.h"
#include "muse_device_commands.h"
#include "muse_wifi.h"
}
#include "muse_chat_priv.h"
#if CONFIG_MUSE_MODULE_LLM
#include "muse_conversation.h"
#include "muse_native_voice.h"
#include "muse_memory.h"
#endif
#if CONFIG_MUSE_MODULE_LLM
#include "muse_module_llm.h"
#include "muse_module_llm_protocol.h"
#endif

#include <xplat/noise/core/ClientSession.h>
#include <xplat/noise/core/PsaCryptoBackend.h>
#include <xplat/noise/core/ServiceCodec.h>

using namespace musegadgets::noise::core;

static const char *TAG = "muse_chat_session";

#define NOISE_PATH "/v1/noise"
#define NOISE_PORT 443
#define IO_TIMEOUT_US (15 * 1000000LL)

#define MIC_RATE 16000
#define DICT_RATE 24000
#define DICT_CHUNK_BYTES 8192              /* ~170 ms at 24 kHz, as hatch-test sends */
#define SCRATCH (64 * 1024)                /* inbound frames reassemble up to this */
#define NDJSON_LINE_MAX (16 * 1024)               /* one NDJSON line / the chat ack */
#define SUB_LINE_MAX (256 * 1024)          /* an event line grows its buffer up to this */
#define CHAT_PART (16 * 1024)              /* one body chunk of a long typed message */
#if CONFIG_MUSE_HATCH_COMPACT_BUFFERS       /* 2 MB of PSRAM: see its Kconfig help */
#define MP3_BUF (128 * 1024)
#define IN_BYTES (MIC_RATE * 2 * 2)        /* 2 s of mic backlog while connecting */
#define OUT_BYTES (MIC_RATE * 2 * 1)       /* 1 s of decoded reply */
#else
#define MP3_BUF (512 * 1024)
#define IN_BYTES (MIC_RATE * 2 * 8)        /* 8 s of mic backlog while connecting */
#define OUT_BYTES (MIC_RATE * 2 * 2)       /* 2 s of decoded reply */
#endif
#define MP3_HOLD (1441 + 4)               /* the largest MP3 frame and the next header */
#define MP3_POLL_ROOM (SCRATCH + 8192)     /* stop reading the socket below this much MP3 room */
static_assert(MP3_BUF > MP3_POLL_ROOM + MP3_HOLD, "reads would stall with the MP3 buffer empty");
#define EV_TEXT 72
#if CONFIG_MUSE_MODULE_LLM
#define TEXT_MAX (MUSE_LLM_TEXT_MAX + 1)    /* bounded local narration/caption text */
#else
#define TEXT_MAX 1024                      /* a message's text, for captions timed to its speech */
#endif
#define SPEECH_CHARS_PER_S 14              /* until the speech's length is known */
#define TEXT_CHARS_PER_S 16                /* speaker off: reading pace, a little over speech */
#define TEXT_HOLD_S 2                      /* speaker off: how long a message's last lines stay up */

#define PING_US (20 * 1000000LL)
#define DEAD_US (60 * 1000000LL)           /* nothing from the server, pongs included */
#define IDLE_CLOSE_US (10 * 60 * 1000000LL)
#define AUTO_RETRY_MIN_US (5 * 1000000LL)  /* connecting without a turn, after a failure */
#define AUTO_RETRY_MAX_US (120 * 1000000LL)
#define FINAL_TIMEOUT_US (15 * 1000000LL)  /* release -> final transcript */
#define REPLY_TIMEOUT_US (60 * 1000000LL)  /* chat posted -> first assistant message */
#define TURN_CAP_US (180 * 1000000LL)
#define SETTLE_US (3 * 1000000LL)          /* quiet period that ends a turn */
#define TEXT_REPLY_TIMEOUT_US (5 * 60 * 1000000LL)   /* typed turns: agents can work a while */
#define TEXT_TURN_CAP_US (15 * 60 * 1000000LL)
/*
 * The VM's streaming dictation has no ASR behind it right now, so each press
 * goes to the chat as a voice note, the way the phone app sends them, and the
 * server transcribes it. Set to 0 to stream to /api/voice/dictation instead.
 */
#define VOICE_NOTE 1
#define NOTE_MAX_BYTES (MIC_RATE * 2 * 20) /* 20 s of 16 kHz PCM; Muse stops at 15 */
#define NOTE_PART_BYTES (DICT_CHUNK_BYTES / 4 * 3)   /* staged PCM that base64s to one body chunk */

#define MAX_MSGS 8

/* ---- Voice task <-> session task ---- */

enum cmd_type_t : uint8_t { CMD_CONNECT, CMD_FORGET, CMD_BEGIN, CMD_END, CMD_CANCEL, CMD_TEXT, CMD_TEXT_CANCEL, CMD_WAKE,
                          CMD_LOCAL_TEXT, CMD_PLAYBACK_STOP, CMD_PLAYBACK_RESTART, CMD_BENCH_PLAYBACK, CMD_DEVICE_TEST, CMD_BENCH_CLEAR, CMD_NATIVE_PROBE, CMD_NATIVE_FILE, CMD_NATIVE_FIXTURE, CMD_NATIVE_MESSAGE_FIXTURE, CMD_NATIVE_RANGE };

struct cmd_t {
    cmd_type_t type;
    uint32_t gen;
    char *text;              /* CMD_TEXT: malloc'd, freed by the session task */
};

struct ev_t {
    muse_hatch_ev_t type;
    uint32_t gen;
    char text[EV_TEXT];
};

static QueueHandle_t s_cmds, s_events;
static StreamBufferHandle_t s_in, s_out;
static std::atomic<uint32_t> s_gen{0};
static std::atomic<bool> s_can_restart{false},s_transport_pending{false},s_playback_active{false};
static bool s_restart_wait,s_bench_reply,s_local_only,s_replaying;
static std::atomic<bool> s_resting{false};

/* ---- Connection ---- */

struct conn_t {
    esp_tls_t *tls;
    PsaCryptoBackend *crypto;
    ClientSession *session;
    uint8_t *ws, *rx, *tf, *sr, *svc, *env;   /* PSRAM scratch */
    int64_t next_id;
    int64_t sub_id;
    int64_t last_rx_us, last_ping_us, last_use_us;
};

static conn_t s_conn;
static bool s_connected;
static muse_hatch_vm_t s_vm;       /* cached per-VM credentials */
static bool s_vm_direct;           /* s_vm.vm_token is the device token itself */
static char s_host[MUSE_HOST_MAX + 1];
/* When to connect without a turn asking: once Wi-Fi is up, again with backoff
 * if that fails or the connection drops, not after an idle close. */
static int64_t s_auto_next_us;
static int64_t s_auto_backoff_us = AUTO_RETRY_MIN_US;

/* Isolated native admission diagnostic. No microphone, greeting or audio upload. */
extern "C" const char *identity_node_id(void);
extern "C" const char *identity_sdk_token(void);
static bool s_native_app, s_native_active, s_native_ready, s_native_close, s_native_finished, s_native_ack;
static int64_t s_native_stream, s_native_end, s_native_at, s_native_deadline;
static char s_native_id[40],s_native_thread[160],s_native_started[32];
static void native_tick(void);
/* ---- Streams on the connection ---- */

enum kind_t : uint8_t { K_NONE, K_SUB, K_DICT, K_CHAT, K_TTS, K_NATIVE, K_NATIVE_END, K_NATIVE_FILE };

struct stream_t {
    int64_t id;
    kind_t kind;
    int status;
    int msg;                 /* K_TTS: index into the turn's messages */
    char *line;              /* NDJSON line / buffered body */
    size_t cap;              /* line's size: NDJSON_LINE_MAX, grown for long event lines */
    size_t len;
    bool overflow;
};

#define MAX_STREAMS 6
static stream_t s_streams[MAX_STREAMS];

/* ---- The current turn ---- */

enum phase_t : uint8_t { P_IDLE, P_LISTEN, P_WAIT_FINAL, P_WAIT_REPLY };

enum tts_t : uint8_t { TTS_NONE, TTS_QUEUED, TTS_ACTIVE, TTS_FINISHED };

struct msg_t {
    char id[80];
    char external_key[25];
    bool native_skip;
    size_t len;              /* reply text length so far */
    char tail[128];          /* its last characters, for the caption */
    bool done;
    uint32_t conversation_id;
    tts_t tts;
    uint32_t pcm_start;      /* where its speech starts in the reply audio */
    uint32_t pcm_frames;     /* how long it is; 0 until the MP3 has all arrived */
};

struct resampler_t {
    uint32_t step;           /* Q16 input samples per output sample */
    uint32_t pos;
    int16_t prev;
};

/* Reply work tracking: a short acknowledgement does not finish a task. */
struct task_progress_t {
    char running[8][80]; unsigned count;
    bool activity_busy,finished,failed,overflow;
};
static bool task_progress_busy(const task_progress_t &p){return p.activity_busy||p.count||p.overflow;}
static bool task_progress_update(task_progress_t &p,bool task,const char *id,const char *status,const char *activity,bool linked){
    bool terminal=status&&(!strcmp(status,"completed")||!strcmp(status,"failed")||!strcmp(status,"errored")||!strcmp(status,"cancelled")||!strcmp(status,"canceled"));
    if(task&&status){
        unsigned found=p.count;
        for(unsigned i=0;id&&i<p.count;i++)if(!strcmp(id,p.running[i])){found=i;break;}
        if(terminal){
            // An unrelated/replayed completion cannot finish the current work.
            if(found==p.count&&!linked)return false;
            if(found<p.count){p.count--;memmove(p.running[found],p.running[found+1],(p.count-found)*sizeof(p.running[0]));}
            if(!p.count&&!p.overflow){p.finished=true;p.failed=strcmp(status,"completed")!=0;p.activity_busy=false;}
        }else if(status[0]&&strcmp(status,"idle")){
            if(id&&id[0]&&found==p.count){
                if(p.count==8||strlen(id)>=sizeof(p.running[0]))p.overflow=true;
                else{memcpy(p.running[p.count++],id,strlen(id)+1);}
            }
            p.activity_busy=true;p.finished=p.failed=false;
        }
    }
    if(!task&&!activity&&status)activity=status;
    if(activity&&!(task&&terminal))p.activity_busy=activity[0]&&strcmp(activity,"online")&&strcmp(activity,"idle")&&strcmp(activity,"completed")&&strcmp(activity,"failed")&&strcmp(activity,"errored")&&strcmp(activity,"cancelled")&&strcmp(activity,"canceled");
    return true;
}
/* End reply work tracking. */

struct turn_t {
    phase_t phase;
    uint32_t gen;
    bool text;               /* typed at the console: the reply goes there, unspoken */
    bool end_requested, end_sent, chat_posted, acked;
    char native_job[25];
    bool native_started,native_claimed;
    int native_voice,native_speed;
    bool native_direct;
    int64_t dict_id, chat_id;
    int64_t start_us, end_sent_us, chat_us, last_event_us, last_content_us;
    int64_t playback_paused_us, playback_pause_check;
    uint64_t sent24;         /* 24 kHz frames sent to dictation */
    resampler_t up;
    uint8_t *chunk;          /* DICT_CHUNK_BYTES, plus the note's tail */
    size_t chunk_len;
    uint8_t *note;           /* NOTE_PART_BYTES of 16 kHz PCM waiting for base64 */
    size_t note_len;
    size_t pcm_bytes;        /* the note so far */
    size_t body_sent;
    char *texts;             /* MAX_MSGS * TEXT_MAX: each message's text */
    uint32_t pcm_out;        /* reply audio frames handed to the voice task */
    char committed[512];     /* finals that arrived before the half-close */
    char partial[512];
    char user_ids[2][80];
    muse_chat_rejected_t rejected;
    msg_t msgs[MAX_MSGS];
    int nmsgs;
    bool agent_busy;
    task_progress_t task_progress;
    /* TTS */
    int tts_msg;             /* message being fetched (or shown, speaker off), or -1 */
    bool silent;             /* speaker off: tts_msg is paced by silence, not fetched */
    uint8_t *mp3;            /* MP3_BUF */
    size_t mp3_len;
    bool mp3_ended;
    mp3dec_t dec;
    resampler_t down;
    int kbps;
    int down_rate;
};

/* 10 KB, most of it the MP3 decoder: in PSRAM on boards that let static data go
 * there (the AIPI), so Wi-Fi setup and TLS have the internal RAM. */
EXT_RAM_BSS_ATTR static turn_t s_turn;

/* Turn milestones, logged together when the turn ends. */
enum mark_t : uint8_t { M_RELEASE, M_SENT, M_ACK, M_TEXT, M_DONE, M_TTS, M_MP3, M_AUDIO, M_COUNT };
static const char *const MARK_NAMES[M_COUNT] = { "release", "sent", "ack", "text", "done", "tts", "mp3", "audio" };
static int64_t s_marks[M_COUNT];
static char s_reply_shown[EV_TEXT];   /* the pre-speech caption last sent */

static void mark(mark_t m);
static bool open_note(void);
static void log_marks(void);
static int16_t *s_pcm;       /* MINIMP3_MAX_SAMPLES_PER_FRAME */
static int16_t *s_pcm16;     /* resampled output */
static int64_t s_last_seq;

static int64_t now_us(void)
{
    return esp_timer_get_time();
}

static void mark(mark_t m)
{
    if (!s_marks[m]) {
        s_marks[m] = now_us();
    }
}

static void log_marks(void)
{
    char line[160];
    int n = 0;
    for (int i = M_SENT; i < M_COUNT && n < (int)sizeof(line); i++) {
        if (s_marks[M_RELEASE] && s_marks[i]) {
            n += snprintf(line + n, sizeof(line) - n, " %s +%d", MARK_NAMES[i],
                          (int)((s_marks[i] - s_marks[M_RELEASE]) / 1000));
        }
    }
    ESP_LOGI(TAG, "timing (ms after release):%s", n ? line : " none");
}

static void *psram_alloc(size_t n)
{
    return heap_caps_malloc_prefer(n, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
}

static void *json_alloc(size_t n)
{
    return psram_alloc(n);
}

/* ---- Events to the voice task ---- */

static void emit(muse_hatch_ev_t type, const char *text)
{
    if (s_turn.text) {
        return;   /* typed turns report to the console instead */
    }
    ev_t ev = {};
    ev.type = type;
    ev.gen = s_turn.gen;
    if (type == MUSE_HATCH_EV_HEARD) {
        muse_hatch_tail_words(text ? text : "", ev.text, sizeof(ev.text));
    } else if (text) {
        strlcpy(ev.text, text, sizeof(ev.text));
    }
    /* Captions are lossy; the end of a turn, and whether the note got there, must get through. */
    TickType_t wait = (type == MUSE_HATCH_EV_DONE || type == MUSE_HATCH_EV_ERROR || type == MUSE_HATCH_EV_SENT)
                          ? pdMS_TO_TICKS(200)
                          : 0;
    xQueueSend(s_events, &ev, wait);
}

/* ---- TLS / WebSocket (from hatch-link's noise_control.cpp) ---- */

static bool write_all(esp_tls_t *tls, const void *buf, size_t len)
{
    const uint8_t *p = static_cast<const uint8_t *>(buf);
    int64_t deadline = now_us() + IO_TIMEOUT_US;
    while (len) {
        if (now_us() >= deadline) {
            return false;
        }
        ssize_t n = esp_tls_conn_write(tls, p, len);
        if (n > 0) {
            p += n;
            len -= n;
        } else if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) {
            vTaskDelay(1);
        } else {
            ESP_LOGW(TAG, "tls write: %d (errno %d)", (int)n, errno);
            return false;
        }
    }
    return true;
}

/* Reads exactly len bytes, waiting up to the I/O timeout. */
static bool read_all(esp_tls_t *tls, uint8_t *buf, size_t len)
{
    int64_t deadline = now_us() + IO_TIMEOUT_US;
    while (len) {
        if (now_us() >= deadline) {
            return false;
        }
        ssize_t n = esp_tls_conn_read(tls, buf, len);
        if (n > 0) {
            buf += n;
            len -= n;
        } else if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) {
            vTaskDelay(1);
        } else {
            return false;
        }
    }
    return true;
}

static bool ws_send_binary(esp_tls_t *tls, const uint8_t *payload, size_t len)
{
    uint8_t hdr[14];
    size_t hl;
    hdr[0] = 0x82;   /* FIN | binary */
    if (len < 126) {
        hdr[1] = 0x80 | len;
        hl = 2;
    } else if (len <= 65535) {
        hdr[1] = 0x80 | 126;
        hdr[2] = len >> 8;
        hdr[3] = len & 0xff;
        hl = 4;
    } else {
        hdr[1] = 0x80 | 127;
        for (int i = 0; i < 8; i++) {
            hdr[2 + i] = (uint64_t)len >> (56 - 8 * i);
        }
        hl = 10;
    }
    uint32_t key = esp_random();
    memcpy(hdr + hl, &key, 4);
    hl += 4;
    if (!write_all(tls, hdr, hl)) {
        return false;
    }
    const uint8_t *mk = reinterpret_cast<const uint8_t *>(&key);
    uint8_t chunk[512];
    for (size_t off = 0; off < len;) {
        size_t n = len - off < sizeof(chunk) ? len - off : sizeof(chunk);
        for (size_t i = 0; i < n; i++) {
            chunk[i] = payload[off + i] ^ mk[(off + i) & 3];
        }
        if (!write_all(tls, chunk, n)) {
            return false;
        }
        off += n;
    }
    return true;
}

static bool ws_send_ping(esp_tls_t *tls)
{
    uint8_t frame[6] = { 0x89, 0x80 };
    uint32_t key = esp_random();
    memcpy(frame + 2, &key, 4);
    return write_all(tls, frame, sizeof(frame));
}

/*
 * Reads one WebSocket frame. With `wait` false, returns -2 at once if nothing
 * is pending. Returns the payload length, -3 for a ping/pong (answered here),
 * 0 when the peer closed, -1 on error.
 */
static ssize_t ws_recv(esp_tls_t *tls, uint8_t *buf, size_t cap, bool wait)
{
    uint8_t hdr[2];
    int64_t deadline = now_us() + IO_TIMEOUT_US;
    for (;;) {
        ssize_t n = esp_tls_conn_read(tls, hdr, 1);
        if (n == 1) {
            break;
        }
        if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) {
            if (!wait) {
                return -2;
            }
            if (now_us() >= deadline) {
                return -1;
            }
            vTaskDelay(1);
            continue;
        }
        return n == 0 ? 0 : -1;
    }
    if (!read_all(tls, hdr + 1, 1)) {
        return -1;
    }
    uint8_t opcode = hdr[0] & 0x0f;
    bool masked = hdr[1] & 0x80;
    uint64_t plen = hdr[1] & 0x7f;
    uint8_t ext[8];
    if (plen == 126) {
        if (!read_all(tls, ext, 2)) {
            return -1;
        }
        plen = (ext[0] << 8) | ext[1];
    } else if (plen == 127) {
        if (!read_all(tls, ext, 8)) {
            return -1;
        }
        plen = 0;
        for (int i = 0; i < 8; i++) {
            plen = (plen << 8) | ext[i];
        }
    }
    uint8_t mask[4] = { 0 };
    if (masked && !read_all(tls, mask, 4)) {
        return -1;
    }
    if (plen > cap) {
        ESP_LOGE(TAG, "WS frame too large: %llu", (unsigned long long)plen);
        return -1;
    }
    if (!read_all(tls, buf, plen)) {
        return -1;
    }
    if (masked) {
        for (size_t i = 0; i < plen; i++) {
            buf[i] ^= mask[i & 3];
        }
    }
    if (opcode == 0x8) {
        ESP_LOGW(TAG, "WS close frame");
        return -1;
    }
    if (opcode == 0x9) {
        uint8_t pong[6] = { 0x8A, (uint8_t)(0x80 | plen), 0, 0, 0, 0 };   /* mask key 0 */
        write_all(tls, pong, sizeof(pong));
        if (plen) {
            write_all(tls, buf, plen);
        }
        return -3;
    }
    if (opcode == 0xA) {
        return -3;
    }
    return (ssize_t)plen;
}

/* Query escaping as hatch-link's noise_upgrade.h: keeps RFC 3986 unreserved plus !~*'(). */
static void query_escape(const char *in, char *out, size_t cap)
{
    static const char *hex = "0123456789ABCDEF";
    size_t o = 0;
    for (; *in && o + 4 < cap; in++) {
        unsigned char c = *in;
        if (isalnum(c) || strchr("-_.!~*'()", c)) {
            out[o++] = c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }
    out[o] = '\0';
}

/* Returns the HTTP status of the upgrade (101 on success), or 0. */
static int ws_upgrade(esp_tls_t *tls, const char *vm_id, const char *token)
{
    char id[3 * 128 + 1];
    query_escape(vm_id, id, sizeof(id));
    const char *app=s_native_app?"&app_id=hatch-web":"";
    size_t cap = 512 + strlen(id) + strlen(app) + strlen(token) + strlen(s_host);
    char *req = static_cast<char *>(malloc(cap));
    if (!req) {
        return 0;
    }
    int len = snprintf(req, cap,
                       "GET " NOISE_PATH "?vm_id=%s%s HTTP/1.1\r\n"
                       "Host: %s\r\n"
                       "Authorization: Bearer %s\r\n"
                       "Upgrade: websocket\r\n"
                       "Connection: Upgrade\r\n"
                       "Sec-WebSocket-Version: 13\r\n"
                       "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                       "\r\n",
                       id, app, s_host, token);
    bool sent = write_all(tls, req, len);
    free(req);
    if (!sent) {
        return 0;
    }
    /* Edge error responses carry a ~1.4 KB Proxy-Status header. */
    const size_t kCap = 4096;
    char *hdr = static_cast<char *>(malloc(kCap));
    if (!hdr) {
        return 0;
    }
    size_t have = 0;
    int status = 0;
    int64_t deadline = now_us() + IO_TIMEOUT_US;
    while (now_us() < deadline && have < kCap) {
        ssize_t n = esp_tls_conn_read(tls, hdr + have, kCap - have);
        if (n > 0) {
            size_t from = have >= 3 ? have - 3 : 0;
            have += n;
            bool end = false;
            for (size_t i = from; i + 3 < have; i++) {
                if (!memcmp(hdr + i, "\r\n\r\n", 4)) {
                    end = true;
                    break;
                }
            }
            if (end) {
                break;
            }
        } else if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) {
            vTaskDelay(1);
        } else {
            break;
        }
    }
    if (have >= 12 && !memcmp(hdr, "HTTP/", 5)) {
        status = atoi(hdr + 9);
    }
    free(hdr);
    return status;
}

static bool noise_handshake(conn_t *c)
{
    ByteSpan out(c->ws, ClientSession::kMaxOutboundWebSocketPayloadSize);
    auto m1 = c->session->WriteHandshakeMessage1(out);
    if (!m1.ok() || !ws_send_binary(c->tls, c->ws, m1.size())) {
        ESP_LOGE(TAG, "handshake msg1 failed");
        return false;
    }
    ssize_t n = ws_recv(c->tls, c->rx, SCRATCH, true);
    if (n <= 0) {
        ESP_LOGE(TAG, "handshake msg2 not received (%d)", (int)n);
        return false;
    }
    uint8_t extra[256];
    size_t extra_len = 0;
    Status st = c->session->ReadHandshakeMessage2(ConstByteSpan(c->rx, n), ByteSpan(extra, sizeof(extra)), extra_len);
    if (!st.ok()) {
        ESP_LOGE(TAG, "handshake msg2: %s", st.str());
        return false;
    }
    /* The owner's msg3 payload is an empty protobuf; the bearer header authenticated us. */
    auto m3 = c->session->WriteHandshakeMessage3(ConstByteSpan(), out);
    if (!m3.ok() || !ws_send_binary(c->tls, c->ws, m3.size())) {
        ESP_LOGE(TAG, "handshake msg3 failed");
        return false;
    }
    return c->session->isEstablished();
}

static bool flush_outbound(conn_t *c)
{
    while (c->session->HasOutboundWebSocketPayload()) {
        auto r = c->session->WriteNextOutboundWebSocketPayload(
            ByteSpan(c->ws, ClientSession::kMaxOutboundWebSocketPayloadSize));
        if (!r.ok() || !ws_send_binary(c->tls, c->ws, r.size())) {
            ESP_LOGE(TAG, "send failed");
            return false;
        }
    }
    c->last_use_us = now_us();
    return true;
}

/* ---- Streams ---- */

static stream_t *find_stream(int64_t id)
{
    for (auto &s : s_streams) {
        if (s.kind != K_NONE && s.id == id) {
            return &s;
        }
    }
    return nullptr;
}

/* A handler may end a stream and a new one may take its slot. */
static bool alive(const stream_t *s, int64_t id)
{
    return s->kind != K_NONE && s->id == id;
}

static void close_stream(stream_t *s)
{
    if (s) {
        s->kind = K_NONE;
    }
}

static bool send_reset(int64_t id)
{
    if (!s_connected || id <= 0) {
        return true;
    }
    close_stream(find_stream(id));
    ResetView rv{ ResetCode::Cancelled, StringView("cancelled") };
    auto r = s_conn.session->StartOutboundReset(ServiceType::Daemon, id, rv, ByteSpan(s_conn.svc, SCRATCH),
                                                ByteSpan(s_conn.env, SCRATCH));
    return r.ok() && flush_outbound(&s_conn);
}

/* Opens a request stream. Returns its id, or 0 if the connection failed. */
static int64_t open_stream(kind_t kind, const char *verb, const char *path, const char *content_type,
                           const char *accept, const char *body, bool end_body)
{
    stream_t *slot = nullptr;
    for (auto &s : s_streams) {
        if (s.kind == K_NONE) {
            slot = &s;
            break;
        }
    }
    if (!slot) {
        ESP_LOGE(TAG, "no free stream slot");
        return 0;
    }
    char req_id[40];
    snprintf(req_id, sizeof(req_id), "muse-%08lx-%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
    HeaderView hdrs[4];
    size_t nh = 0;
    hdrs[nh++] = { StringView("x-request-id"), StringView(req_id) };
    hdrs[nh++] = { StringView("x-app-id"), StringView("hatch-web") };
    if (content_type) {
        hdrs[nh++] = { StringView("Content-Type"), StringView(content_type) };
    }
    if (accept) {
        hdrs[nh++] = { StringView("accept"), StringView(accept) };
    }
    int64_t id = s_conn.next_id++;
    ApplicationRequestView req;
    req.verb = StringView(verb);
    req.path = StringView(path);
    req.headers = Span<const HeaderView>(hdrs, nh);
    req.body = body ? ConstByteSpan(reinterpret_cast<const uint8_t *>(body), strlen(body)) : ConstByteSpan();
    req.end_body = end_body;
    auto r = s_conn.session->StartOutboundApplicationRequest(ServiceType::Daemon, id, req,
                                                             ByteSpan(s_conn.svc, SCRATCH),
                                                             ByteSpan(s_conn.env, SCRATCH));
    if (!r.ok()) {
        ESP_LOGE(TAG, "%s %s: %s", verb, path, r.status().str());
        return 0;
    }
    if (!flush_outbound(&s_conn)) {
        return 0;
    }
    slot->id = id;
    slot->kind = kind;
    slot->status = 0;
    slot->msg = -1;
    slot->len = 0;
    slot->overflow = false;
    ESP_LOGI(TAG, "stream %lld: %s %s", (long long)id, verb, path);
    return id;
}

static bool send_body(int64_t id, const uint8_t *data, size_t len, bool end_body)
{
    BodyChunkView chunk{ ConstByteSpan(data, len), end_body };
    auto r = s_conn.session->StartOutboundBodyChunk(ServiceType::Daemon, id, chunk, ByteSpan(s_conn.svc, SCRATCH),
                                                    ByteSpan(s_conn.env, SCRATCH));
    if (!r.ok()) {
        ESP_LOGE(TAG, "body chunk: %s", r.status().str());
        return false;
    }
    return flush_outbound(&s_conn);
}

/* ---- Connect / disconnect ---- */

static void disconnect(const char *why)
{
    if (s_conn.tls) {
        ESP_LOGI(TAG, "disconnect: %s", why);
        esp_tls_conn_destroy(s_conn.tls);
    }
    if (s_conn.session) {
        s_conn.session->~ClientSession();
        heap_caps_free(s_conn.session);
    }
    if (s_conn.crypto) {
        s_conn.crypto->~PsaCryptoBackend();
        heap_caps_free(s_conn.crypto);
    }
    uint8_t *bufs[] = { s_conn.ws, s_conn.rx, s_conn.tf, s_conn.sr, s_conn.svc, s_conn.env };
    for (uint8_t *b : bufs) {
        heap_caps_free(b);
    }
    s_conn = conn_t{};
    for (auto &s : s_streams) {
        s.kind = K_NONE;
    }
    s_connected = false;
}

static void forget_vm(void)
{
    free(s_vm.vm_token);
    s_vm = muse_hatch_vm_t{};
    s_vm_direct = false;
}

/* Fills s_vm from the account API (or the token itself). */
static bool resolve_vm(char *err, size_t err_cap)
{
    if (s_vm.vm_token) {
        return true;
    }
    char want[MUSE_VM_MAX + 1];
    muse_settings_hatch_vm(want);
    if (!muse_settings_hatch_token_len()) {
        /* No token of our own: use Link's Hatch account. */
        if (muse_link_hatch_vm(want, s_vm.vm_id, sizeof(s_vm.vm_id), s_vm.vm_name, sizeof(s_vm.vm_name),
                               &s_vm.vm_token)) {
            ESP_LOGI(TAG, "VM %s (%s) via Link", s_vm.vm_id, s_vm.vm_name);
            return true;
        }
        strlcpy(err, muse_link_hatch_linked() ? "Can't reach Muse's server" : "Not paired", err_cap);
        return false;
    }
    char *token = static_cast<char *>(malloc(MUSE_TOKEN_MAX + 1));
    if (!token) {
        strlcpy(err, "Out of memory", err_cap);
        return false;
    }
    muse_settings_hatch_token(token);
    int rc = muse_hatch_api_find_vm(token, want, &s_vm);
    if (rc == 0) {
        ESP_LOGI(TAG, "VM %s (%s)", s_vm.vm_id, s_vm.vm_name);
        free(token);
        return true;
    }
    if (want[0]) {
        /* Not a device token (or the API is down): try it as the VM's own token. */
        ESP_LOGW(TAG, "account API %s; using the token for VM %s directly",
                 rc == MUSE_HATCH_API_AUTH ? "rejected the token" : "failed", want);
        strlcpy(s_vm.vm_id, want, sizeof(s_vm.vm_id));
        s_vm.vm_token = token;
        s_vm_direct = true;
        return true;
    }
    free(token);
    strlcpy(err, rc == MUSE_HATCH_API_AUTH ? "Token rejected" : "Can't reach Muse's server", err_cap);
    return false;
}

static bool open_subscription(void)
{
    s_last_seq = 0;
    s_conn.sub_id = open_stream(K_SUB, "POST", "/chat/subscribe", "application/json", "application/x-ndjson", "{}",
                                true);
    return s_conn.sub_id != 0;
}

static bool connect_once(char *err, size_t err_cap, int *http_status)
{
    *http_status = 0;
    conn_t &c = s_conn;
    c.tls = esp_tls_init();
    if (!c.tls) {
        strlcpy(err, "Out of memory", err_cap);
        return false;
    }
    esp_tls_cfg_t cfg = {};
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 15000;
    int64_t t0 = now_us();
    if (esp_tls_conn_new_sync(s_host, strlen(s_host), NOISE_PORT, &cfg, c.tls) != 1) {
        snprintf(err, err_cap, "Can't reach %s", s_host);
        return false;
    }
    int status = ws_upgrade(c.tls, s_vm.vm_id, s_vm.vm_token);
    *http_status = status;
    if (status != 101) {
        ESP_LOGW(TAG, "upgrade rejected: HTTP %d", status);
        if (status == 401 || status == 403) {
            strlcpy(err, "VM refused the token", err_cap);
        } else {
            snprintf(err, err_cap, "VM connect failed (HTTP %d)", status);
        }
        return false;
    }
    void *mem = psram_alloc(sizeof(PsaCryptoBackend));
    c.crypto = mem ? new (mem) PsaCryptoBackend() : nullptr;
    mem = c.crypto ? psram_alloc(sizeof(ClientSession)) : nullptr;
    c.session = mem ? new (mem) ClientSession(*c.crypto) : nullptr;
    c.ws = static_cast<uint8_t *>(psram_alloc(ClientSession::kMaxOutboundWebSocketPayloadSize));
    c.rx = static_cast<uint8_t *>(psram_alloc(SCRATCH));
    c.tf = static_cast<uint8_t *>(psram_alloc(SCRATCH));
    c.sr = static_cast<uint8_t *>(psram_alloc(SCRATCH));
    c.svc = static_cast<uint8_t *>(psram_alloc(SCRATCH));
    c.env = static_cast<uint8_t *>(psram_alloc(SCRATCH));
    if (!c.session || !c.ws || !c.rx || !c.tf || !c.sr || !c.svc || !c.env) {
        strlcpy(err, "Out of memory", err_cap);
        return false;
    }
    if (!noise_handshake(&c)) {
        strlcpy(err, "Noise handshake failed", err_cap);
        return false;
    }
    int fd = -1;
    if (esp_tls_get_conn_sockfd(c.tls, &fd) != ESP_OK || fd < 0) {
        strlcpy(err, "Socket error", err_cap);
        return false;
    }
    lwip_fcntl(fd, F_SETFL, lwip_fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    c.next_id = 1;
    c.last_rx_us = c.last_ping_us = c.last_use_us = now_us();
    s_connected = true;
    if (!open_subscription()) {
        strlcpy(err, "Subscribe failed", err_cap);
        return false;
    }
    ESP_LOGI(TAG, "connected to VM %s in %d ms", s_vm.vm_id, (int)((now_us() - t0) / 1000));
    return true;
}

static bool ensure_connected(void)
{
    if (s_connected) {
        return true;
    }
    if (!muse_hatch_configured() || !muse_wifi_connected()) {
        return false;
    }
    muse_hatch_report(MUSE_HATCH_TESTING, "Connecting...");
    muse_settings_hatch_host(s_host);
    if (!s_host[0]) {
        strlcpy(s_host, "hatch.metaaivm.com", sizeof(s_host));
    }
    char err[48] = "";
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!resolve_vm(err, sizeof(err))) {
            // Link serializes startup/account work behind a bounded gate.
            // One transient lookup failure must not discard the first question.
            if(attempt==0&&muse_link_hatch_linked()&&muse_wifi_connected()){
                muse_hatch_report(MUSE_HATCH_TESTING,"Retrying connection...");
                vTaskDelay(pdMS_TO_TICKS(250));continue;
            }
            break;
        }
        int status;
        if (connect_once(err, sizeof(err), &status)) {
            /* The settings page shows the VM in its VM ID row. */
            muse_hatch_report_vm(s_vm.vm_id);
            muse_hatch_report(MUSE_HATCH_REACHABLE, "");
            return true;
        }
        disconnect(err);
        /* A refused VM token may just be stale: fetch a fresh one once. */
        if ((status == 401 || status == 403) && !s_vm_direct) {
            forget_vm();
            continue;
        }
        break;
    }
    ESP_LOGW(TAG, "connect failed: %s", err);
    muse_hatch_report(MUSE_HATCH_UNREACHABLE, err);
    return false;
}

/* ---- Resampling ---- */

static void resampler_init(resampler_t *r, int in_rate, int out_rate)
{
    r->step = (uint32_t)(((uint64_t)in_rate << 16) / out_rate);
    r->pos = 0;
    r->prev = 0;
}

/* Linear interpolation; state carries across calls. out must hold n*out/in + 2. */
static size_t resample(resampler_t *r, const int16_t *in, size_t n, int16_t *out)
{
    size_t o = 0;
    if (!n) {
        return 0;
    }
    /* Position 0 is the previous call's last sample, k is in[k-1]. */
    while ((r->pos >> 16) < n) {
        size_t i = r->pos >> 16;
        int32_t a = i ? in[i - 1] : r->prev;
        int32_t b = in[i];
        /* (b - a) spans 17 bits and the fraction 16, so the product needs 64. */
        out[o++] = (int16_t)(a + (int32_t)(((int64_t)(b - a) * (int64_t)(r->pos & 0xffff)) >> 16));
        r->pos += r->step;
    }
    r->pos -= n << 16;
    r->prev = in[n - 1];
    return o;
}

/* ---- Turn: dictation ---- */

static void turn_reset_streams(void)
{
    send_reset(s_turn.dict_id);
    send_reset(s_turn.chat_id);
    for (auto &s : s_streams) {
        if (s.kind == K_TTS) {
            send_reset(s.id);
        }
    }
}

EXT_RAM_BSS_ATTR static char s_pending_reply[TEXT_MAX];
static int s_pending_reply_msg=-1;

static void turn_finish(void)
{
    s_pending_reply_msg=-1;s_pending_reply[0]=0;
    s_playback_active=false;
#if CONFIG_MUSE_MODULE_LLM
    muse_module_llm_cancel(s_turn.gen);
#endif
    turn_reset_streams();
    s_turn.phase = P_IDLE;
    s_turn.dict_id = s_turn.chat_id = 0;
    s_turn.tts_msg = -1;
    s_turn.silent = false;
    s_turn.mp3_len = 0;
}

static void turn_fail(const char *why)
{
    ESP_LOGW(TAG, "turn failed: %s", why);
    if (s_turn.text) {
        muse_hatch_console("error", why, nullptr);
    }
    emit(MUSE_HATCH_EV_ERROR, why);
    turn_finish();
}

/* Ends the turn once its reply is in; `complete` is false when it was cut off. */
static void turn_done(bool complete)
{
    if (s_turn.text) {
        muse_hatch_console("done", nullptr, "\"messages\":%d,\"complete\":%s", s_turn.nmsgs,
                           complete ? "true" : "false");
    }
    emit(MUSE_HATCH_EV_DONE, nullptr);
    turn_finish();
}

/* Ends any turn in progress and starts a fresh one. False if Hatch can't be reached. */
static bool turn_start(uint32_t gen, bool text, bool network=true)
{
    if (s_turn.phase != P_IDLE) {
        if (s_turn.text) {
            turn_fail("INTERRUPTED");
        } else {
            turn_finish();
        }
    }
    uint8_t *chunk = s_turn.chunk, *mp3 = s_turn.mp3, *note = s_turn.note;
    char *texts = s_turn.texts;
    muse_native_voice_cancel();
    s_turn.native_job[0]=0;s_turn.native_started=s_turn.native_claimed=false;
    s_can_restart=false;s_restart_wait=false;s_bench_reply=false;s_local_only=false;s_replaying=false;
    s_turn = turn_t{};
    s_turn.chunk = chunk;
    s_turn.mp3 = mp3;
    s_turn.note = note;
    s_turn.texts = texts;
    s_turn.gen = gen;
    s_turn.text = text;
    s_turn.tts_msg = -1;
    memset(s_marks, 0, sizeof(s_marks));
    s_reply_shown[0] = '\0';
    s_turn.start_us = now_us();
    resampler_init(&s_turn.up, MIC_RATE, DICT_RATE);
    if (network && !ensure_connected()) {
        turn_fail(muse_hatch_configured() ? "CAN'T REACH MUSE" : "MUSE NOT SET UP");
        return false;
    }
    return true;
}

static void turn_begin(uint32_t gen)
{
#if CONFIG_MUSE_MODULE_LLM
    /* Local mode accepts recognized text only, including if an old caller
     * accidentally invokes the legacy microphone-upload entry point. */
    turn_finish();
    s_turn.gen = gen;
    s_turn.text = false;
    turn_fail("LOCAL SPEECH RECOGNITION REQUIRED");
    return;
#endif
    if (!turn_start(gen, false)) {
        return;
    }
    if (VOICE_NOTE) {
        if (!open_note()) {
            disconnect("chat open failed");
            turn_fail("CAN'T REACH MUSE");
            return;
        }
        s_turn.phase = P_LISTEN;
        return;
    }
    char path[64];
    snprintf(path, sizeof(path), "/api/voice/dictation?sample_rate_hz=%d", DICT_RATE);
    s_turn.dict_id = open_stream(K_DICT, "POST", path, nullptr, "application/x-ndjson", nullptr, false);
    if (!s_turn.dict_id) {
        disconnect("dictation open failed");
        turn_fail("CAN'T REACH MUSE");
        return;
    }
    s_turn.phase = P_LISTEN;
}

/* Moves mic audio to the dictation stream, paced like a live mic. */
static bool pump_mic(void)
{
    static int16_t in[MIC_RATE / 50];
    static int16_t up[MIC_RATE / 50 * DICT_RATE / MIC_RATE + 4];
    bool did = false;
    for (;;) {
        /* Never run more than a second ahead of real time: the ASR upstream drops floods. */
        double elapsed = (now_us() - s_turn.start_us) / 1e6;
        if ((s_turn.sent24 + DICT_CHUNK_BYTES / 2) / (double)DICT_RATE > 1.0 + 1.5 * elapsed) {
            return did;
        }
        size_t got = xStreamBufferReceive(s_in, in, sizeof(in), 0) / sizeof(int16_t);
        if (!got) {
            break;
        }
        did = true;
        size_t n = resample(&s_turn.up, in, got, up);
        const uint8_t *p = reinterpret_cast<const uint8_t *>(up);
        size_t bytes = n * sizeof(int16_t);
        while (bytes) {
            size_t take = DICT_CHUNK_BYTES - s_turn.chunk_len < bytes ? DICT_CHUNK_BYTES - s_turn.chunk_len : bytes;
            memcpy(s_turn.chunk + s_turn.chunk_len, p, take);
            s_turn.chunk_len += take;
            p += take;
            bytes -= take;
            if (s_turn.chunk_len == DICT_CHUNK_BYTES) {
                if (!send_body(s_turn.dict_id, s_turn.chunk, DICT_CHUNK_BYTES, false)) {
                    return false;
                }
                s_turn.sent24 += DICT_CHUNK_BYTES / 2;
                s_turn.chunk_len = 0;
            }
        }
    }
    if (s_turn.end_requested && !s_turn.end_sent) {
        if (s_turn.chunk_len && !send_body(s_turn.dict_id, s_turn.chunk, s_turn.chunk_len, false)) {
            return false;
        }
        s_turn.sent24 += s_turn.chunk_len / 2;
        s_turn.chunk_len = 0;
        if (!send_body(s_turn.dict_id, nullptr, 0, true)) {
            return false;
        }
        s_turn.end_sent = true;
        s_turn.end_sent_us = now_us();
        s_turn.phase = P_WAIT_FINAL;
        ESP_LOGI(TAG, "sent %.2fs of speech", (double)s_turn.sent24 / DICT_RATE);
    }
    return true;
}

/* ---- Turn: voice note ---- */

/* Base64-encodes the staged PCM into one body chunk; `last` pads and closes the request. */
static bool send_note_part(bool last)
{
    char *o = reinterpret_cast<char *>(s_turn.chunk);
    o += muse_hatch_base64(s_turn.note, s_turn.note_len, o);
    if (last) {
        memcpy(o, MUSE_HATCH_NOTE_TAIL, sizeof(MUSE_HATCH_NOTE_TAIL) - 1);
        o += sizeof(MUSE_HATCH_NOTE_TAIL) - 1;
    }
    size_t n = o - reinterpret_cast<char *>(s_turn.chunk);
    s_turn.body_sent += n;
    s_turn.note_len = 0;
    return send_body(s_turn.chat_id, s_turn.chunk, n, last);
}

/*
 * Opens the chat request at the press and streams the note while it's
 * recorded, so only the last chunk goes out after the release. The length
 * isn't known until the end, so the WAV header gives the streaming "unknown"
 * size and the server reads to the end of the data.
 */
static bool open_note(void)
{
    s_turn.chat_id = open_stream(K_CHAT, "POST", "/chat/stream", "application/json", nullptr, nullptr, false);
    if (!s_turn.chat_id) {
        return false;
    }
    s_turn.body_sent = sizeof(MUSE_HATCH_NOTE_HEAD) - 1;
    if (!send_body(s_turn.chat_id, reinterpret_cast<const uint8_t *>(MUSE_HATCH_NOTE_HEAD), sizeof(MUSE_HATCH_NOTE_HEAD) - 1, false)) {
        return false;
    }
    muse_hatch_wav_header(s_turn.note, MIC_RATE);
    s_turn.note_len = MUSE_HATCH_WAV_HEADER;
    return true;
}

/* Moves the mic into the note request; on release, sends the rest and waits for the reply. */
static bool record_note(void)
{
    for (;;) {
        size_t room = NOTE_PART_BYTES - s_turn.note_len;
        size_t left = NOTE_MAX_BYTES - s_turn.pcm_bytes;
        room = (room < left ? room : left) & ~(size_t)1;
        size_t got = room ? xStreamBufferReceive(s_in, s_turn.note + s_turn.note_len, room, 0) : 0;
        if (!got) {
            break;
        }
        s_turn.note_len += got;
        s_turn.pcm_bytes += got;
        if (s_turn.note_len == NOTE_PART_BYTES && !send_note_part(false)) {
            return false;
        }
    }
    if (!s_turn.end_requested && s_turn.pcm_bytes < NOTE_MAX_BYTES) {
        return true;
    }
    mark(M_RELEASE);
    double secs = (double)s_turn.pcm_bytes / (MIC_RATE * 2);
    if (secs < 0.3) {
        turn_fail("DIDN'T CATCH THAT");   /* resets the half-sent request */
        return true;
    }
    if (!send_note_part(true)) {
        return false;
    }
    ESP_LOGI(TAG, "voice note: %.2fs, %u byte request", secs, (unsigned)s_turn.body_sent);
    mark(M_SENT);
    s_turn.chat_posted = true;
    s_turn.chat_us = s_turn.last_event_us = now_us();
    s_turn.phase = P_WAIT_REPLY;
    return true;
}

/*
 * Posts `text` to the chat and waits for the reply. A long message goes up in
 * parts, since each frame has to fit SCRATCH.
 */
static void send_chat(const char *text, const char *modality)
{
    s_turn.chat_posted = true;
    cJSON *body = cJSON_CreateObject();
#if CONFIG_MUSE_MODULE_LLM
    char *remembered = muse_memory_prompt(text);
    const char *base=remembered ? remembered : text;
    const char *preferences="My standing response preferences: Use U.S. customary measurements only: Fahrenheit, miles, feet/inches, pounds/ounces and U.S. gallons. Convert metric measurements before replying. Use clear U.S. dates and 12-hour AM/PM times. Keep SCP identifiers unchanged. This gadget renders Markdown emphasis, lists, quotes and code; links display an address rather than opening a browser. The current question may be an imperfect offline speech transcript. Use the recent conversation to resolve obvious phonetic mistakes or missing spaces, especially names and acronyms. For example, in an established SCP-story conversation, a garbled request for a new Foundation story likely means an SCP Foundation story. Do not invent missing details, alter identifier digits, or reinterpret a clear request. If two interpretations materially differ, ask one short clarifying question. Apply these preferences while answering my current question below.\n\n";
    char native_prompt[1400]={};
    if(!s_turn.text && !s_local_only && muse_settings_voice_source()==1 && muse_settings_muse_voice()==2){
        s_turn.native_direct=true;modality="voice";
        strlcpy(native_prompt,"\nDevice narration for THIS turn replaces older file-delivery instructions: the device retrieves your native voice by response message ID automatically. Return the complete requested answer or story directly as your normal response text with native voice. Do not create audio files, run speech/podcast tools, write source.md, or launch a delivery helper for this turn. Keep the full requested story and its paragraphs; do not shorten it for delivery. These delivery instructions are not part of the answer.\n",sizeof(native_prompt));
    }
    if(!s_turn.text && !s_local_only && muse_settings_voice_source()==1 && !s_turn.native_direct){
        for(unsigned i=0;i<3;i++)snprintf(s_turn.native_job+8*i,9,"%08lx",(unsigned long)esp_random());
        s_turn.native_voice=muse_settings_muse_voice();s_turn.native_speed=muse_settings_speech_speed();
        const char *voice=s_turn.native_voice==1?"avocado_v2:MAI_01":"avocado_v2:MAI_03";
        const char *speeds[]={"slow","normal","fast"};
        snprintf(native_prompt,sizeof(native_prompt),"\nDevice narration delivery: Answer the question below normally, without shortening stories or lists. Also write the exact complete answer (Markdown allowed, at most 8192 UTF-8 bytes) to ~/muse-storyteller/replies/%s/source.md, creating this fresh directory. Use ONLY the installed built-in Muse helper: launch ~/muse-storyteller/voice-env/bin/python3 ~/muse-storyteller/tools/muse_vm_tts_job.py %s --voice %s --speed %s --compact --sections as a detached background job with stdin /dev/null and stdout/stderr in that job directory. Do not wait for synthesis to finish. Do not change global voice/settings or install a provider. Then return the exact same answer as normal text, without paths, tool acknowledgements or delivery instructions. If the helper cannot launch, return the answer text and briefly report that audio could not be created. This paragraph is device delivery configuration, not part of the answer.\n",s_turn.native_job,s_turn.native_job,voice,speeds[s_turn.native_speed]);
    }
    size_t pref_cap=strlen(preferences)+strlen(native_prompt)+strlen(base)+1;char *preferred=(char *)malloc(pref_cap);
    if(preferred)snprintf(preferred,pref_cap,"%s%s%s",preferences,native_prompt,base);
    cJSON_AddStringToObject(body, "message", preferred ? preferred : base);free(preferred);
    free(remembered);
#else
    cJSON_AddStringToObject(body, "message", text);
#endif
    cJSON_AddStringToObject(body, "output_modality", modality);
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    size_t len = json ? strlen(json) : 0;
    bool whole = len <= CHAT_PART;
    s_turn.chat_id = json ? open_stream(K_CHAT, "POST", "/chat/stream", "application/json", nullptr,
                                        whole ? json : nullptr, whole) : 0;
    bool ok = s_turn.chat_id != 0;
    for (size_t off = 0; ok && !whole && off < len; off += CHAT_PART) {
        size_t n = len - off < CHAT_PART ? len - off : CHAT_PART;
        ok = send_body(s_turn.chat_id, reinterpret_cast<const uint8_t *>(json) + off, n, off + n == len);
    }
    cJSON_free(json);
    if (!ok) {
        disconnect("chat/stream failed");
        turn_fail("CAN'T REACH MUSE");
        return;
    }
    if(s_turn.native_job[0])s_turn.native_started=muse_native_voice_begin(s_turn.native_job,s_turn.gen,s_turn.native_voice,s_turn.native_speed);
    s_turn.body_sent = len;
    s_turn.chat_us = s_turn.last_event_us = now_us();
    s_turn.phase = P_WAIT_REPLY;
}

static void post_chat(const char *text)
{
    if (s_turn.chat_posted) {
        return;
    }
    s_turn.chat_posted = true;
    send_reset(s_turn.dict_id);
    s_turn.dict_id = 0;
    while (*text == ' ') {
        text++;
    }
    if (!*text) {
        turn_fail("DIDN'T CATCH THAT");
        return;
    }
    ESP_LOGI(TAG, "heard: \"%s\"", text);
    emit(MUSE_HATCH_EV_HEARD, text);
    send_chat(text, "text");
}

/* A typed turn: the text goes straight to the chat. */
static void text_begin(const char *text)
{
    if (!turn_start(0, true)) {
        return;
    }
    ESP_LOGI(TAG, "typed turn: %u bytes", (unsigned)strlen(text));
    send_chat(text, "text");
    if (s_turn.phase == P_WAIT_REPLY) {
        mark(M_SENT);
        muse_hatch_console("sent", nullptr, "\"bytes\":%u", (unsigned)strlen(text));
    }
}

static void on_dictation_line(cJSON *line)
{
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(line, "type"));
    const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(line, "text"));
    if (!type) {
        return;
    }
    char heard[1024];
    if (!strcmp(type, "partial") && text) {
        strlcpy(s_turn.partial, text, sizeof(s_turn.partial));
        snprintf(heard, sizeof(heard), "%s%s%s", s_turn.committed, s_turn.committed[0] ? " " : "", text);
        emit(MUSE_HATCH_EV_HEARD, heard);
    } else if (!strcmp(type, "final")) {
        snprintf(heard, sizeof(heard), "%s%s%s", s_turn.committed, s_turn.committed[0] && text ? " " : "",
                 text ? text : "");
        s_turn.partial[0] = '\0';
        if (s_turn.end_sent) {
            post_chat(heard);
        } else {
            /* A mid-utterance segment; the rest follows. */
            strlcpy(s_turn.committed, heard, sizeof(s_turn.committed));
            emit(MUSE_HATCH_EV_HEARD, heard);
        }
    } else if (!strcmp(type, "error")) {
        char *s = cJSON_PrintUnformatted(line);
        ESP_LOGW(TAG, "dictation: %s", s ? s : "error");
        cJSON_free(s);
    }
}

static void on_dictation_end(bool ok)
{
    s_turn.dict_id = 0;
    if (s_turn.chat_posted || s_turn.phase == P_IDLE) {
        return;
    }
    if (!s_turn.end_sent) {
        turn_fail(ok ? "MUSE STOPPED LISTENING" : "MUSE COULDN'T LISTEN");
        return;
    }
    char heard[1024];
    snprintf(heard, sizeof(heard), "%s%s%s", s_turn.committed, s_turn.committed[0] ? " " : "", s_turn.partial);
    post_chat(heard);
}

/* ---- Turn: reply ---- */

static int find_msg(const char *id)
{
    for (int i = 0; i < s_turn.nmsgs; i++) {
        if (!strcmp(s_turn.msgs[i].id, id)) {
            return i;
        }
    }
    return -1;
}

static bool is_user_id(const char *id)
{
    return id && id[0] && (!strcmp(id, s_turn.user_ids[0]) || !strcmp(id, s_turn.user_ids[1]));
}

/* The message `id` if it belongs to this turn, binding it on first sight; else -1. */
static int bind_msg(const char *id, cJSON *payload)
{
    if (muse_chat_is_rejected(&s_turn.rejected, id)) {
        return -1;
    }
    int i = find_msg(id);
    if (i >= 0 || s_turn.phase != P_WAIT_REPLY) {
        return i;
    }
    const char *parent = cJSON_GetStringValue(cJSON_GetObjectItem(payload, "reply_to_message_id"));
    if (!parent) {
        parent = cJSON_GetStringValue(cJSON_GetObjectItem(payload, "parent_message_id"));
    }
    /* Once the ack names our message, replies to anything else are someone else's. */
    if (parent && parent[0] && s_turn.acked && !is_user_id(parent) && find_msg(parent) < 0) {
        muse_chat_reject(&s_turn.rejected, id);
        return -1;
    }
    if ((!parent || !parent[0]) && s_turn.rejected.overflow) {
        return -1;
    }
    if (s_turn.nmsgs == MAX_MSGS) {
        return -1;
    }
    msg_t &m = s_turn.msgs[s_turn.nmsgs];
    m = msg_t{};
    strlcpy(m.id, id, sizeof(m.id));
    return s_turn.nmsgs++;
}

static void append_text(msg_t &m, const char *text)
{
    size_t add = strlen(text);
    if (s_turn.texts) {
        char *full = s_turn.texts + (&m - s_turn.msgs) * TEXT_MAX;
        if (!m.len) {
            full[0] = '\0';
        }
        strlcat(full, text, TEXT_MAX);
    }
    m.len += add;
    size_t have = strlen(m.tail);
    if (add >= sizeof(m.tail) - 1) {
        strlcpy(m.tail, text + add - (sizeof(m.tail) - 1), sizeof(m.tail));
        return;
    }
    if (have + add >= sizeof(m.tail)) {
        size_t drop = have + add - (sizeof(m.tail) - 1);
        memmove(m.tail, m.tail + drop, have - drop + 1);
    }
    strlcat(m.tail, text, sizeof(m.tail));
}

/*
 * Shows a reply that hasn't started speaking: its opening lines, which the
 * speech starts with, so they can be read while the audio is on its way.
 * The spoken captions carry on from there.
 */
static void show_reply_start(const msg_t &m)
{
    char line[EV_TEXT];
    if (s_turn.texts) {
        if (!muse_hatch_caption_at(s_turn.texts + (&m - s_turn.msgs) * TEXT_MAX, 0, line, sizeof(line))) {
            return;
        }
    } else {
        muse_hatch_tail_words(m.tail, line, sizeof(line));
    }
    if (strcmp(line, s_reply_shown) != 0) {
        strlcpy(s_reply_shown, line, sizeof(s_reply_shown));
        emit(MUSE_HATCH_EV_REPLY, line);
    }
}

// Muse may deliver a structured final transcript after a short acknowledgement.
static const char *reply_final_text(cJSON *payload,const char *id,char *buffer,size_t capacity){
    const char *text=cJSON_GetStringValue(cJSON_GetObjectItem(payload,"display_text"));
    if(text)return text;
    cJSON *transcript=cJSON_GetObjectItem(payload,"transcript");
    text=cJSON_GetStringValue(transcript);if(text)return text;
    cJSON *messages=cJSON_GetObjectItem(transcript,"messages"),*chosen=nullptr;
    if(cJSON_IsArray(messages))for(int i=0;i<cJSON_GetArraySize(messages);i++){
        cJSON *m=cJSON_GetArrayItem(messages,i);
        const char *role=cJSON_GetStringValue(cJSON_GetObjectItem(m,"role"));
        const char *mid=cJSON_GetStringValue(cJSON_GetObjectItem(m,"id"));
        if(role&&!strcmp(role,"assistant")&&mid&&id&&!strcmp(mid,id))chosen=m;
    }
    cJSON *content=cJSON_GetObjectItem(chosen?chosen:payload,"content");
    text=cJSON_GetStringValue(content);if(text)return text;
    if(!cJSON_IsArray(content))return nullptr;
    size_t used=0;buffer[0]=0;
    for(int i=0;i<cJSON_GetArraySize(content);i++){
        cJSON *item=cJSON_GetArrayItem(content,i);
        const char *type=cJSON_GetStringValue(cJSON_GetObjectItem(item,"type"));
        const char *part=cJSON_GetStringValue(cJSON_GetObjectItem(item,"text"));
        if(!type||strcmp(type,"text")||!part)continue;
        size_t n=strlen(part),separator=used?1:0;
        if(n+separator>=capacity-used){turn_fail("REPLY TEXT EXCEEDS DEVICE LIMIT");return nullptr;}
        if(separator){buffer[used++]='\n';}
        memcpy(buffer+used,part,n+1);used+=n;
    }
    return used?buffer:nullptr;
}
EXT_RAM_BSS_ATTR static char s_final_reply[TEXT_MAX];

static void message_done(int i, const char *final_text)
{
    msg_t &m = s_turn.msgs[i];
    if(m.done){
        const char *previous=s_turn.texts?s_turn.texts+i*TEXT_MAX:nullptr;
        if(!final_text||!final_text[0]||!previous||!strcmp(previous,final_text))return;
        if(strlen(final_text)>=TEXT_MAX){turn_fail("REPLY TEXT EXCEEDS DEVICE LIMIT");return;}
        if(m.tts==TTS_ACTIVE&&s_turn.tts_msg==i){
            strlcpy(s_pending_reply,final_text,sizeof(s_pending_reply));s_pending_reply_msg=i;return;
        }
        // An authoritative revision gets a fresh audio cache. A duplicate does
        // not replay; an active short acknowledgement is allowed to finish.
        if(s_turn.native_started){muse_native_voice_cancel();s_turn.native_started=false;}
        m.done=false;m.tts=TTS_NONE;m.external_key[0]=0;m.conversation_id=0;
    }
    m.done = true;
    mark(M_DONE);
    if (s_turn.text) {
        /* The whole text if the pieces didn't add up to it (a line skipped, say): the reader uses it instead. */
        size_t n = m.len;
        if (final_text && final_text[0] && strlen(final_text) != m.len) {
            n = strlen(final_text);
            muse_hatch_console("final", final_text, "\"msg\":%d", i);
        }
        muse_hatch_console("message_done", nullptr, "\"msg\":%d,\"bytes\":%u", i, (unsigned)n);
        ESP_LOGI(TAG, "message %s done (%u chars)", m.id, (unsigned)n);
        return;
    }
    if (final_text && final_text[0] && s_turn.texts &&
        (!m.len || strcmp(s_turn.texts + i * TEXT_MAX, final_text))) {
        /* Keep narration, the readable message and word offsets on the same
         * authoritative text if the final event corrects streamed pieces. */
        m.len = 0;
        m.tail[0] = '\0';
        s_turn.texts[i * TEXT_MAX] = '\0';
        append_text(m, final_text);
    } else if (!m.len && final_text && final_text[0]) {
        append_text(m, final_text);
    }
#if CONFIG_MUSE_MODULE_LLM
    if(s_turn.native_direct&&!m.len){turn_fail("MUSE SENT NO READABLE TEXT");return;}
    const char *conversation_text = final_text && final_text[0] ? final_text :
        (s_turn.texts ? s_turn.texts + i * TEXT_MAX : nullptr);
    if (conversation_text && !s_turn.native_job[0]) m.conversation_id = s_bench_reply?muse_conversation_add_transient(false,conversation_text):muse_conversation_add(false, conversation_text);
    if(m.len && m.len<TEXT_MAX && s_turn.texts)s_can_restart=true;
    if(s_turn.native_job[0])muse_native_voice_answer_ready(s_turn.gen,false);
#endif
    if (m.len && m.tts == TTS_NONE) {
        m.tts = TTS_QUEUED;s_playback_active=true;
    }
    ESP_LOGI(TAG, "message %s done (%u chars)", m.id, (unsigned)m.len);
}

static const char *msg_id(cJSON *payload, cJSON *event)
{
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(payload, "message_id"));
    if (!id || !id[0]) {
        id = cJSON_GetStringValue(cJSON_GetObjectItem(event, "message_id"));
    }
    if (!id || !id[0]) {
        id = cJSON_GetStringValue(cJSON_GetObjectItem(payload, "id"));
    }
    return id && id[0] ? id : nullptr;
}

// Persisted messages use chat_event_seq; live deltas/status use the bus sequence.
// A saved final reply can therefore have a LOWER seq than the last live update.
// Authoritative full messages are deduplicated by ID and final text in message_done,
// where a revised final with the same ID remains eligible for delivery.
static bool accept_event_sequence(const char *event,int64_t value){
    if(!strcmp(event,"message.assistant")||!strcmp(event,"message.user"))return true;
    if(value>0&&value<=s_last_seq)return false;
    if(value>s_last_seq)s_last_seq=value;
    return true;
}

static void on_event(cJSON *line)
{
    if (strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(line, "type")) ?: "", "event") != 0) {
        return;   /* the subscription ack */
    }
    const char *event = cJSON_GetStringValue(cJSON_GetObjectItem(line, "event")) ?: "";
    cJSON *seq = cJSON_GetObjectItem(line, "seq");
    if(cJSON_IsNumber(seq)&&!accept_event_sequence(event,(int64_t)seq->valuedouble))return;
    if (s_turn.phase != P_WAIT_REPLY) {
        return;
    }
    cJSON *payload = cJSON_GetObjectItem(line, "payload");

    if (!strcmp(event,"agent.status")||!strcmp(event,"task.status")) {
        const char *parent=cJSON_GetStringValue(cJSON_GetObjectItem(payload,"reply_to_message_id"));
        if(!parent)parent=cJSON_GetStringValue(cJSON_GetObjectItem(payload,"parent_message_id"));
        bool linked=parent&&parent[0]&&(is_user_id(parent)||find_msg(parent)>=0);
        if(parent&&parent[0]&&!linked)return;
        const char *message=cJSON_GetStringValue(cJSON_GetObjectItem(payload,"message_id"));
        linked=linked||(message&&(is_user_id(message)||find_msg(message)>=0));
        const char *id=cJSON_GetStringValue(cJSON_GetObjectItem(payload,"task_id"));
        const char *status=cJSON_GetStringValue(cJSON_GetObjectItem(payload,"status"));
        const char *activity=cJSON_GetStringValue(cJSON_GetObjectItem(payload,"activity_code"));
        bool was=s_turn.agent_busy;
        if(!task_progress_update(s_turn.task_progress,!strcmp(event,"task.status"),id,status,activity,linked))return;
        s_turn.agent_busy=task_progress_busy(s_turn.task_progress);
        if(was!=s_turn.agent_busy){
            printf("@chat_work {\"busy\":%s,\"running\":%u,\"finished\":%s,\"failed\":%s}\n",s_turn.agent_busy?"true":"false",s_turn.task_progress.count,s_turn.task_progress.finished?"true":"false",s_turn.task_progress.failed?"true":"false");fflush(stdout);
            if(s_turn.text)muse_hatch_console("busy",nullptr,"\"on\":%s",s_turn.agent_busy?"true":"false");
            else if(s_turn.agent_busy&&s_turn.tts_msg<0){emit(MUSE_HATCH_EV_SENT,nullptr);muse_state_set_caption("Muse is working...");}
        }
        if(s_turn.native_job[0])muse_native_voice_answer_ready(s_turn.gen,s_turn.agent_busy);
        s_turn.last_event_us=now_us();return;
    }
    bool start = !strcmp(event, "delta.message_start");
    bool append = !strcmp(event, "delta.text_append");
    bool done = !strcmp(event, "delta.message_done");
    bool full = !strcmp(event, "message.assistant");
    if (!start && !append && !done && !full) {
        return;
    }
    const char *id = msg_id(payload, line);
    int i = id ? bind_msg(id, payload) : -1;
    if (i < 0) {
        return;
    }
    s_turn.last_event_us = s_turn.last_content_us = now_us();
    msg_t &m = s_turn.msgs[i];
    if (append && !m.done) {
        const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(payload, "text"));
        if (text && text[0] && s_turn.text) {
            mark(M_TEXT);
            m.len += strlen(text);
            muse_hatch_console("text", text, "\"msg\":%d", i);
        } else if (text && text[0]) {
            mark(M_TEXT);
            append_text(m, text);
            show_reply_start(m);   /* ignored once the speech starts */
        }
    } else if (done || full) {
        const char *text=reply_final_text(payload,id,s_final_reply,sizeof(s_final_reply));
        if(s_turn.phase!=P_WAIT_REPLY)return;
        cJSON *ready = cJSON_GetObjectItem(payload, "display_text_ready");
        if (done || !cJSON_IsFalse(ready)) {
            message_done(i, text);
        }
    }
}

static void on_chat_ack(stream_t *s)
{
    s->line[s->len] = '\0';
    cJSON *root = cJSON_Parse(s->line);
    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *obj = cJSON_IsObject(result) ? result : root;
    const char *keys[] = { "message_id", "reply_to_message_id" };
    for (int k = 0; k < 2; k++) {
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(obj, keys[k]));
        if (id) {
            strlcpy(s_turn.user_ids[k], id, sizeof(s_turn.user_ids[k]));
        }
    }
    s_turn.acked = true;
    mark(M_ACK);
    ESP_LOGI(TAG, "chat/stream ack: user message %s", s_turn.user_ids[0]);
    cJSON_Delete(root);
    emit(MUSE_HATCH_EV_SENT, nullptr);
}

/* ---- Turn: speech ---- */

static void start_tts(void)
{
#if CONFIG_MUSE_MODULE_LLM
    if(s_restart_wait){muse_module_llm_status_t st;muse_module_llm_status(&st);if(st.state!=MUSE_MODULE_LLM_READY)return;s_restart_wait=false;s_transport_pending=false;}
#endif
    if (s_turn.tts_msg >= 0) {
        return;
    }
    for (int i = 0; i < s_turn.nmsgs; i++) {
        msg_t &m = s_turn.msgs[i];
        if (m.tts != TTS_QUEUED) {
            continue;
        }
#if CONFIG_MUSE_MODULE_LLM
        const char *local_text = s_turn.texts ? s_turn.texts + i * TEXT_MAX : nullptr;
        if (muse_settings_speaker_on()) {
            muse_conversation_bind_speech(m.conversation_id);
            if (!local_text || m.len >= TEXT_MAX) {
                turn_fail("REPLY TOO LONG FOR LOCAL VOICE");
                return;
            }
            // Ask for native speech after task work completes, so a progress
            // acknowledgement cannot become the cached audio for the final result.
            if(s_turn.native_direct&&(s_turn.agent_busy||(s_turn.task_progress.finished&&now_us()-s_turn.last_event_us<SETTLE_US)))return;
            if(s_turn.native_direct && !m.external_key[0]){
                if(!s_turn.native_started){
                    s_turn.native_started=muse_native_voice_begin_message(m.id,local_text,s_turn.gen);
                    if(!s_turn.native_started){muse_state_set_caption("Preparing Muse audio...");return;}
                }
                char key[25];int ready=muse_native_voice_copy(s_turn.gen,s_turn.texts+i*TEXT_MAX,TEXT_MAX,key);
                if(ready==0){muse_state_set_caption("Buffering Muse audio...");return;}
                if(ready!=1){s_can_restart=true;turn_fail(muse_native_voice_error_title());return;}
                strlcpy(m.external_key,key,sizeof(m.external_key));s_turn.native_started=false;
                // Each finished assistant message gets its own immutable clip.
                // Do not skip later messages, paragraphs or list items.
                // Continue polling chat so additional assistant messages are
                // retained while this complete clip plays.
            }
            if(s_turn.native_job[0] && !m.external_key[0] && !s_replaying){
                if(s_turn.native_claimed){m.native_skip=true;m.tts=TTS_FINISHED;continue;}
                if(!s_turn.native_started){
                    s_turn.native_started=muse_native_voice_begin(s_turn.native_job,s_turn.gen,s_turn.native_voice,s_turn.native_speed);
                    if(!s_turn.native_started){muse_state_set_caption("Preparing Muse audio...");return;}
                }
                if(s_turn.native_job[0]){
                    char key[25];int ready=muse_native_voice_copy(s_turn.gen,s_turn.texts+i*TEXT_MAX,TEXT_MAX,key);
                    if(ready==0){
                        muse_native_voice_status_t p;muse_native_voice_status(&p);
                        if(p.total)muse_state_set_caption("Loading Muse voice: %u%%",(unsigned)((uint64_t)p.received*100/p.total));
                        else muse_state_set_caption("Muse is preparing your voice...");
                        return;
                    }
                    if(ready==1){
                        strlcpy(m.external_key,key,sizeof(m.external_key));m.len=strlen(local_text);s_turn.native_claimed=true;
                        // The authenticated script and complete clip are now local.
                        // Later chat/socket activity must not stop cached narration.
                        s_local_only=true;s_turn.agent_busy=false;s_can_restart=true;
                        for(int other=0;other<s_turn.nmsgs;other++)if(other!=i){s_turn.msgs[other].done=true;s_turn.msgs[other].native_skip=true;s_turn.msgs[other].tts=TTS_FINISHED;}
                    }
                    else{
                        // Selecting Muse voice must never silently speak in Piper.
                        if(!m.conversation_id)m.conversation_id=s_bench_reply?muse_conversation_add_transient(false,local_text):muse_conversation_add(false,local_text);
                        muse_conversation_bind_speech(m.conversation_id);show_reply_start(m);
                        s_can_restart=true;
                        turn_fail("MUSE AUDIO FAILED - TAP RESTART");return;
                    }
                }
            }
            if(!m.conversation_id)m.conversation_id=s_bench_reply?muse_conversation_add_transient(false,local_text):muse_conversation_add(false,local_text);
            muse_conversation_bind_speech(m.conversation_id);
            esp_err_t result = m.external_key[0] ? muse_module_llm_speak_external(s_turn.gen,local_text,muse_settings_volume(),m.external_key) : muse_module_llm_speak(s_turn.gen, local_text, muse_settings_volume());
            if (result == ESP_ERR_TIMEOUT) return;   /* bounded local backpressure */
            if (result != ESP_OK) {
                turn_fail("LOCAL VOICE IS NOT READY");
                return;
            }
            m.pcm_start = s_turn.pcm_out;
            m.pcm_frames = (uint32_t)(strlen(local_text) * MIC_RATE / TEXT_CHARS_PER_S);
            m.tts = TTS_ACTIVE;
            s_turn.tts_msg = i;
            s_turn.silent = false;
            show_reply_start(m);
            return;
        }
#endif
        /*
         * Replies are text, shown at reading pace: silence in place of speech
         * paces the captions and ends the turn. To speak them instead, send
         * the message's text (s_turn.texts + i * TEXT_MAX, if texts was
         * allocated; up to TEXT_MAX - 1 bytes) to a TTS API of your choice and
         * play the MP3 it returns. In place of the silence below: keep
         * m.tts = TTS_ACTIVE and s_turn.tts_msg = i, set s_turn.silent = false,
         * m.pcm_start = s_turn.pcm_out, m.pcm_frames = 0, s_turn.mp3_len = 0,
         * s_turn.mp3_ended = false, s_turn.kbps = 0, s_turn.down_rate = 0 and
         * mp3dec_init(&s_turn.dec). Then, on this task, pass the MP3 to
         * tts_data() as it arrives (it buffers up to MP3_BUF and drops the
         * rest, so hold off while it's full) and set s_turn.mp3_ended at the
         * end. decode() plays it at the speaker's volume, captions following,
         * and finishes the message once it's drained.
         */
        m.pcm_start = s_turn.pcm_out;
#if CONFIG_MUSE_MODULE_LLM
        size_t shown_len = local_text ? strlen(local_text) : strlen(m.tail);
        m.pcm_frames = (uint32_t)(shown_len * MIC_RATE / TEXT_CHARS_PER_S);
#else
        m.pcm_frames = (uint32_t)(m.len * MIC_RATE / TEXT_CHARS_PER_S);
#endif
        m.tts = TTS_ACTIVE;
        s_turn.tts_msg = i;
        s_turn.silent = true;
        ESP_LOGI(TAG, "showing message %s (%u chars)", m.id, (unsigned)m.len);
        show_reply_start(m);
        return;
    }
}

static void tts_data(const uint8_t *data, size_t len)
{
    if (s_turn.mp3_len + len > MP3_BUF) {
        ESP_LOGW(TAG, "MP3 buffer full, dropping %u bytes", (unsigned)len);
        len = MP3_BUF - s_turn.mp3_len;
    }
    mark(M_MP3);
    memcpy(s_turn.mp3 + s_turn.mp3_len, data, len);
    s_turn.mp3_len += len;
}

static void tts_end(stream_t *s, bool ok)
{
    int i = s->msg;
    close_stream(s);
    if (i < 0 || i != s_turn.tts_msg) {
        return;
    }
    if (ok) {
        s_turn.mp3_ended = true;   /* decode() drains the rest, then finishes */
    } else {
        s_turn.msgs[i].tts = TTS_FINISHED;
        s_turn.tts_msg = -1;
    }
}

/* Speaker off: queues the shown message's silence while the reply buffer has room. */
static void pace_silently(bool finish_when_queued = true)
{
    static const int16_t zeros[256] = {};
    msg_t &m = s_turn.msgs[s_turn.tts_msg];
    uint32_t end = m.pcm_start + m.pcm_frames + TEXT_HOLD_S * MIC_RATE;
    while (s_turn.pcm_out < end && xStreamBufferSpacesAvailable(s_out) >= sizeof(zeros)) {
        uint32_t n = end - s_turn.pcm_out < 256 ? end - s_turn.pcm_out : 256;
        xStreamBufferSend(s_out, zeros, n * sizeof(int16_t), 0);
        s_turn.pcm_out += n;
    }
    if (finish_when_queued && s_turn.pcm_out >= end) {
        m.tts = TTS_FINISHED;
        s_turn.tts_msg = -1;
        s_turn.silent = false;
    }
}

/* Decodes buffered MP3 while the reply buffer has room. */
static void decode(void)
{
    if (s_turn.tts_msg < 0) {
        return;
    }
    if (s_turn.silent) {
        pace_silently();
        return;
    }
#if CONFIG_MUSE_MODULE_LLM
    if (!muse_settings_speaker_on()) {
        muse_module_llm_cancel(s_turn.gen);
        s_turn.silent = true;
        pace_silently();
        return;
    }
    muse_module_llm_playback_t playback = muse_module_llm_playback(s_turn.gen);
    if (playback == MUSE_MODULE_LLM_PLAYBACK_FAILED || playback == MUSE_MODULE_LLM_PLAYBACK_NONE) {
        turn_fail("LOCAL VOICE FAILED");
        return;
    }
    if (playback == MUSE_MODULE_LLM_PLAYBACK_DONE) {
        msg_t &local = s_turn.msgs[s_turn.tts_msg];
        local.pcm_frames = s_turn.pcm_out - local.pcm_start;
        local.tts = TTS_FINISHED;
        int completed=s_turn.tts_msg;s_turn.tts_msg=-1;
        if(s_pending_reply_msg==completed){s_pending_reply_msg=-1;message_done(completed,s_pending_reply);s_pending_reply[0]=0;}
        if(s_turn.agent_busy){emit(MUSE_HATCH_EV_SENT,nullptr);muse_state_set_caption("Muse is still working...");}
        return;
    }
    if (playback == MUSE_MODULE_LLM_PLAYBACK_CANCELLED) {
        turn_fail("LOCAL VOICE CANCELLED");
        return;
    }
    pace_silently(false);
    return;
#endif
    /*
     * minimp3 only takes a frame once it can see the next one's header. Given
     * less, it resets and says to skip all of it, which drops speech and clicks.
     * So until the stream ends, leave the last MP3_HOLD bytes for more to arrive.
     */
    size_t hold = s_turn.mp3_ended ? 0 : MP3_HOLD;
    size_t off = 0;
    while (s_turn.mp3_len - off > hold &&
           xStreamBufferSpacesAvailable(s_out) >= (MINIMP3_MAX_SAMPLES_PER_FRAME / 2 + 8) * sizeof(int16_t)) {
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(&s_turn.dec, s_turn.mp3 + off, s_turn.mp3_len - off, s_pcm, &info);
        if (!info.frame_bytes) {
            if (s_turn.mp3_ended) {
                off = s_turn.mp3_len;   /* trailing junk */
            }
            break;
        }
        off += info.frame_bytes;
        if (!samples) {
            continue;
        }
        if (info.channels == 2) {
            for (int k = 0; k < samples; k++) {
                s_pcm[k] = (s_pcm[2 * k] + s_pcm[2 * k + 1]) / 2;
            }
        }
        if (s_turn.down_rate != info.hz) {
            ESP_LOGI(TAG, "reply audio: %d Hz, %d ch, %d kbps", info.hz, info.channels, info.bitrate_kbps);
            s_turn.down_rate = info.hz;
            resampler_init(&s_turn.down, info.hz, MIC_RATE);
        }
        size_t n = resample(&s_turn.down, s_pcm, samples, s_pcm16);
        if (s_turn.gen == s_gen.load()) {
            mark(M_AUDIO);
            xStreamBufferSend(s_out, s_pcm16, n * sizeof(int16_t), 0);
        }
        s_turn.pcm_out += n;
        s_turn.kbps = info.bitrate_kbps;
    }
    if (off) {
        memmove(s_turn.mp3, s_turn.mp3 + off, s_turn.mp3_len - off);
        s_turn.mp3_len -= off;
    }
    msg_t &m = s_turn.msgs[s_turn.tts_msg];
    if (s_turn.mp3_ended && s_turn.kbps > 0) {
        /* All of it is here: what's decoded plus what the bitrate says the rest holds. */
        uint32_t rest = (uint32_t)((uint64_t)s_turn.mp3_len * 8 * MIC_RATE / (s_turn.kbps * 1000));
        m.pcm_frames = s_turn.pcm_out - m.pcm_start + rest;
    }
    if (s_turn.mp3_ended && !s_turn.mp3_len) {
        m.pcm_frames = s_turn.pcm_out - m.pcm_start;
        m.tts = TTS_FINISHED;
        s_turn.tts_msg = -1;
    }
}

/* Ends the turn once the reply is complete and spoken, or on timeouts. */
static void check_turn(void)
{
    int64_t t = now_us();
#if CONFIG_MUSE_MODULE_LLM
    muse_module_llm_status_t playback;muse_module_llm_status(&playback);
    if(s_turn.phase==P_WAIT_REPLY && playback.paused){
        if(s_turn.playback_pause_check)s_turn.playback_paused_us+=t-s_turn.playback_pause_check;
        s_turn.playback_pause_check=t;return;
    }
    if(s_turn.playback_pause_check){s_turn.playback_paused_us+=t-s_turn.playback_pause_check;s_turn.playback_pause_check=0;}
#endif
    if (s_turn.phase == P_WAIT_FINAL && t - s_turn.end_sent_us > FINAL_TIMEOUT_US) {
        on_dictation_end(true);
        return;
    }
    if (s_turn.phase != P_WAIT_REPLY) {
        return;
    }
    bool text = s_turn.text;
    #if CONFIG_MUSE_MODULE_LLM
    /* Long local narration uses the same bounded allowance as typed turns;
     * reply/network timeouts below still apply while Muse has not answered. */
    int64_t turn_cap = TEXT_TURN_CAP_US;
#else
    int64_t turn_cap = text ? TEXT_TURN_CAP_US : TURN_CAP_US;
#endif
    if (t - s_turn.start_us - s_turn.playback_paused_us > turn_cap) {
        if(s_turn.agent_busy){turn_fail("MUSE TASK TIMED OUT");return;}
        ESP_LOGW(TAG, "turn hit the time cap");
        /* A voice turn that waited out the cap on a busy agent got no reply at all. */
        if (!text && !s_turn.nmsgs) {
            turn_fail("NO REPLY FROM MUSE");
            return;
        }
        turn_done(false);
        return;
    }
    if (!s_turn.nmsgs) {
        /* Wait as long as the agent says it's working; the turn cap still applies. */
        if (t - s_turn.chat_us > (text ? TEXT_REPLY_TIMEOUT_US : REPLY_TIMEOUT_US) && !s_turn.agent_busy) {
            turn_fail("NO REPLY FROM MUSE");
        }
        return;
    }
    for (int i = 0; i < s_turn.nmsgs; i++) {
        if (!s_turn.msgs[i].done || s_turn.msgs[i].tts == TTS_QUEUED || s_turn.msgs[i].tts == TTS_ACTIVE) {
            return;
        }
    }
    if (t - s_turn.last_event_us < SETTLE_US) {
        return;
    }
    // Working survives quiet gaps between an acknowledgement and the result.
    // Only an idle/terminal status or the explicit overall deadline ends it.
    if(s_turn.agent_busy)return;
    if(s_turn.task_progress.failed){turn_fail("MUSE TASK FAILED - CHECK CHAT");return;}
    ESP_LOGI(TAG, "turn done: %d message(s) in %.1fs", s_turn.nmsgs, (t - s_turn.start_us) / 1e6);
    log_marks();
    turn_done(true);
}

/* ---- Inbound dispatch ---- */

/* Splits NDJSON; calls fn for each complete line. */
static void feed_lines(stream_t *s, const uint8_t *data, size_t len, void (*fn)(cJSON *))
{
    int64_t id = s->id;
    for (size_t i = 0; i < len; i++) {
        char ch = data[i];
        if (ch != '\n') {
            if (s->len == s->cap - 1 && !s->overflow && s->cap < SUB_LINE_MAX) {
                /* A long reply's done event carries its whole text. */
                char *grown = static_cast<char *>(heap_caps_realloc(s->line, s->cap * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                if (grown) {
                    s->line = grown;
                    s->cap *= 2;
                }
            }
            if (s->len < s->cap - 1) {
                s->line[s->len++] = ch;
            } else {
                s->overflow = true;
            }
            continue;
        }
        if (s->overflow) {
            ESP_LOGW(TAG, "stream %lld: skipped a line over %u bytes", (long long)s->id, (unsigned)s->cap);
        } else if (s->len) {
            cJSON *j = cJSON_ParseWithLength(s->line, s->len);
            if (j) {
                fn(j);
                cJSON_Delete(j);
            }
        }
        if (!alive(s, id)) {
            return;   /* fn ended the stream */
        }
        s->len = 0;
        s->overflow = false;
    }
}

static void native_event(cJSON *event){
    const char *type=cJSON_GetStringValue(cJSON_GetObjectItem(event,"type"));if(!type)return;
    if(!strcmp(type,"hatch.voice.session")){
        const char *id=cJSON_GetStringValue(cJSON_GetObjectItem(event,"call_id"));
        const char *session=cJSON_GetStringValue(cJSON_GetObjectItem(event,"session_id"));
        if(id&&!strcmp(id,s_native_id)&&session&&*session){
            const char *thread=cJSON_GetStringValue(cJSON_GetObjectItem(event,"thread_id"));
            if(thread)strlcpy(s_native_thread,thread,sizeof(s_native_thread));
            s_native_ready=true;s_native_close=true;
            printf("@native {\"event\":\"admitted\",\"milliseconds\":%lld}\n",(long long)((now_us()-s_native_at)/1000));
        }else{s_native_close=true;printf("@native {\"event\":\"invalid_session\"}\n");}
    }else if(!strcmp(type,"error")){s_native_close=true;printf("@native {\"event\":\"admission_error\"}\n");}
    fflush(stdout);
}
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

void native_rfc3339(int64_t unix_ms, char out[25])
{
    if (unix_ms < 0) {
        unix_ms = 0;
    }
    int64_t secs = unix_ms / 1000;
    int ms = (int)(unix_ms % 1000);
    int64_t z = secs / 86400 + 719468;
    int64_t sod = secs % 86400;
    int64_t era = z / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
    snprintf(out, 25, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", (unsigned)((uint64_t)y % 10000), m % 100, d % 100,
             (unsigned)(sod / 3600), (unsigned)(sod / 60 % 60), (unsigned)(sod % 60), (unsigned)ms);
}

static int digits(const char *p, int n)
{
    int v = 0;
    for (int i = 0; i < n; i++) {
        if (p[i] < '0' || p[i] > '9') {
            return -1;
        }
        v = v * 10 + (p[i] - '0');
    }
    return v;
}

int64_t native_http_date(const char *s, size_t len)
{
    /* "Sun, 06 Nov 1994 08:49:37 GMT" */
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    if (len != 29 || s[3] != ',' || s[4] != ' ' || s[7] != ' ' || s[11] != ' ' || s[16] != ' ' || s[19] != ':' ||
        s[22] != ':' || memcmp(s + 25, " GMT", 4) != 0) {
        return -1;
    }
    int day = digits(s + 5, 2), year = digits(s + 12, 4);
    int hh = digits(s + 17, 2), mm = digits(s + 20, 2), ss = digits(s + 23, 2);
    int mon = -1;
    for (int i = 0; i < 12; i++) {
        if (!memcmp(s + 8, months + 3 * i, 3)) {
            mon = i + 1;
        }
    }
    if (day < 1 || day > 31 || year < 1970 || mon < 0 || hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 || ss > 60) {
        return -1;
    }
    int64_t days = days_from_civil(year, (unsigned)mon, (unsigned)day);
    return ((days * 86400) + hh * 3600 + mm * 60 + ss) * 1000;
}


static int64_t s_native_wall_offset_ms;
static void native_stamp(char *out,size_t cap){(void)cap;int64_t wall=s_native_wall_offset_ms?now_us()/1000+s_native_wall_offset_ms:(int64_t)time(nullptr)*1000;native_rfc3339(wall,out);}
static void native_start(void){
    if(s_turn.phase!=P_IDLE||s_native_active){printf("@native {\"event\":\"busy\"}\n");return;}
    disconnect("native admission diagnostic");s_native_app=true;
    if(!ensure_connected()){s_native_app=false;s_auto_next_us=0;printf("@native {\"event\":\"connection_failed\"}\n");return;}
    snprintf(s_native_id,sizeof(s_native_id),"%08lx-%04lx-4%03lx-8%03lx-%08lx%04lx",(unsigned long)esp_random(),(unsigned long)(esp_random()&65535),(unsigned long)(esp_random()&4095),(unsigned long)(esp_random()&4095),(unsigned long)esp_random(),(unsigned long)(esp_random()&65535));
    s_native_active=true;s_native_ready=s_native_close=s_native_finished=s_native_ack=false;s_native_thread[0]=0;s_native_end=0;s_native_at=now_us();s_native_deadline=s_native_at+20000000LL;native_stamp(s_native_started,sizeof(s_native_started));
    cJSON *j=cJSON_CreateObject();cJSON_AddStringToObject(j,"type","hatch.voice.call.start");cJSON_AddStringToObject(j,"call_id",s_native_id);cJSON_AddStringToObject(j,"device","mcu-wearable");cJSON_AddStringToObject(j,"device_id",identity_node_id());cJSON_AddStringToObject(j,"call_media_type","audio");cJSON_AddBoolToObject(j,"proactive_greeting_enabled",false);
    char *body=cJSON_PrintUnformatted(j);cJSON_Delete(j);char path[120];snprintf(path,sizeof(path),"/voice/realtime?call_id=%s",s_native_id);
    char admission[700];snprintf(admission,sizeof(admission),"%s\n{\"type\":\"hatch.voice.capability.offer\",\"end_call_stop\":1}\n",body?body:"");cJSON_free(body);
    s_native_stream=open_stream(K_NATIVE,"POST",path,"application/x-ndjson; charset=utf-8","application/x-ndjson",admission,false);
    if(!s_native_stream)s_native_close=true;
    printf("@native {\"event\":\"started\",\"sdk_token_configured\":%s}\n",identity_sdk_token()?"true":"false");fflush(stdout);
}
struct native_file_reply_t{char body[6144];size_t len;int status;bool overflow;std::atomic<bool>done{false};};
static native_file_reply_t s_nf;static int64_t s_nf_id,s_nf_deadline;static bool s_nf_cancelled;static uint32_t s_nf_offset;
static void native_file_cb(void *ctx,int status,const uint8_t *data,size_t len,bool end){
    auto *r=static_cast<native_file_reply_t *>(ctx);if(status)r->status=status;
    size_t room=sizeof(r->body)-1-r->len,take=len<room?len:room;
    if(take){memcpy(r->body+r->len,data,take);}
    r->len+=take;if(take<len)r->overflow=true;
    if(end||status<0)r->done.store(true,std::memory_order_release);
}
static void native_file_start(uint32_t offset,const char *message=nullptr){
    if(s_turn.phase!=P_IDLE||s_native_active||s_nf_id||offset>1024*1024){printf("@native_file {\"error\":\"busy_or_bound\"}\n");return;}
    if(!muse_link_req_ready()){printf("@native_file {\"error\":\"link_not_ready\"}\n");return;}
    s_nf.len=0;s_nf.status=0;s_nf.overflow=false;s_nf.done=false;s_nf_cancelled=false;
    s_nf_offset=offset;char range[48];snprintf(range,sizeof(range),"bytes=%lu-%lu",(unsigned long)offset,(unsigned long)(offset+4095));
    const char *headers[]={"Range",range,"Accept","audio/mpeg",nullptr};
    char path[256];snprintf(path,sizeof(path),"/api/voice/tts-stream?message_id=%s",message?message:"");
    s_nf_id=muse_link_req_open("GET",message?path:"/fs/raw/muse-storyteller-tests/blue-lantern-20261104.mp3",headers,true,native_file_cb,&s_nf);
    s_nf_deadline=now_us()+60000000LL;
    if(!s_nf_id){printf("@native_file {\"error\":\"link_open\"}\n");fflush(stdout);return;}

}
static void native_file_tick(void){
    if(!s_nf_id)return;
    if(s_nf.done.load(std::memory_order_acquire)){
        cJSON *out=cJSON_CreateObject();bool ok=(s_nf.status==200||s_nf.status==206)&&!s_nf.overflow;
        cJSON_AddNumberToObject(out,"status",s_nf.status);cJSON_AddBoolToObject(out,"ok",ok);cJSON_AddNumberToObject(out,"offset",s_nf_offset);
        if(ok){char *encoded=(char *)malloc(4*((s_nf.len+2)/3)+1);if(encoded){size_t n=muse_hatch_base64((const uint8_t *)s_nf.body,s_nf.len,encoded);encoded[n]=0;cJSON_AddStringToObject(out,"audio",encoded);free(encoded);}}
        else cJSON_AddStringToObject(out,"error",s_nf.overflow?"response_bound":"native_file_refused");
        char *text=cJSON_PrintUnformatted(out);if(text){printf("@native_file %s\n",text);cJSON_free(text);}cJSON_Delete(out);s_nf_id=0;fflush(stdout);
    }else if(now_us()>s_nf_deadline&&!s_nf_cancelled){s_nf_cancelled=true;muse_link_req_cancel(s_nf_id);}
}
static void native_tick(void){
    native_file_tick();
    if(!s_native_active)return;
    if(now_us()>s_native_deadline){if(s_native_end)s_native_finished=true;else s_native_close=true;}
    if(s_native_close&&!s_native_end&&!s_native_finished){
        s_native_close=false;char ended[32];native_stamp(ended,sizeof(ended));
        cJSON *j=cJSON_CreateObject();cJSON_AddStringToObject(j,"call_id",s_native_id);cJSON_AddStringToObject(j,"call_started_at",s_native_started);cJSON_AddStringToObject(j,"call_ended_at",ended);if(s_native_thread[0])cJSON_AddStringToObject(j,"thread_id",s_native_thread);
        char *body=cJSON_PrintUnformatted(j);cJSON_Delete(j);s_native_end=open_stream(K_NATIVE_END,"POST","/voice/end-call","application/json","application/json",body,true);cJSON_free(body);
        s_native_deadline=now_us()+12000000LL;if(!s_native_end)s_native_finished=true;
    }
    if(s_native_finished){
        printf("@native {\"event\":\"complete\",\"admitted\":%s,\"end_acknowledged\":%s}\n",s_native_ready?"true":"false",s_native_ack?"true":"false");fflush(stdout);
        if(s_native_stream){send_reset(s_native_stream);}
        s_native_active=false;s_native_app=false;disconnect("native test finished");s_auto_next_us=0;s_auto_backoff_us=AUTO_RETRY_MIN_US;
    }
}

static void stream_data(stream_t *s, ConstByteSpan data)
{
    if (data.empty()) {
        return;
    }
    switch (s->kind) {
    case K_NATIVE:
        feed_lines(s,data.data(),data.size(),native_event);break;
    case K_NATIVE_FILE:
    case K_NATIVE_END: {
        size_t take=data.size()<s->cap-1-s->len?data.size():s->cap-1-s->len;memcpy(s->line+s->len,data.data(),take);s->len+=take;break;
    }
    case K_SUB:
        feed_lines(s, data.data(), data.size(), on_event);
        break;
    case K_DICT:
        feed_lines(s, data.data(), data.size(), on_dictation_line);
        break;
    case K_CHAT: {
        size_t take = data.size() < s->cap - 1 - s->len ? data.size() : s->cap - 1 - s->len;
        memcpy(s->line + s->len, data.data(), take);
        s->len += take;
        break;
    }
    case K_TTS:
        if (s->msg == s_turn.tts_msg) {
            tts_data(data.data(), data.size());
        }
        break;
    default:
        break;
    }
}

/* Returns false when the connection has to go. */
static bool stream_end(stream_t *s, bool ok)
{
    switch (s->kind) {
    case K_NATIVE_FILE:{
        s->line[s->len]=0;cJSON *j=cJSON_Parse(s->line),*out=cJSON_CreateObject();cJSON_AddNumberToObject(out,"status",s->status);cJSON_AddBoolToObject(out,"ok",ok);if(j)cJSON_AddItemToObject(out,"body",j);else cJSON_AddStringToObject(out,"error","invalid_json");char *text=cJSON_PrintUnformatted(out);if(text){printf("@native_file %s\n",text);cJSON_free(text);}cJSON_Delete(out);close_stream(s);fflush(stdout);return true;
    }
    case K_NATIVE:close_stream(s);s_native_stream=0;s_native_close=true;return true;
    case K_NATIVE_END:{
        s->line[s->len]=0;cJSON *j=cJSON_Parse(s->line);const cJSON *result=cJSON_GetObjectItem(j,"result");
        s_native_ack=ok&&s->status>=200&&s->status<300&&cJSON_IsTrue(cJSON_GetObjectItem(j,"ok"))&&cJSON_IsTrue(cJSON_GetObjectItem(result,"ok"));cJSON_Delete(j);close_stream(s);s_native_finished=true;return true;
    }
    case K_SUB:
        ESP_LOGW(TAG, "subscription ended");
        return false;
    case K_DICT:
        close_stream(s);
        on_dictation_end(ok);
        break;
    case K_CHAT:
        if (ok) {
            on_chat_ack(s);
        }
        close_stream(s);
        s_turn.chat_id = 0;
        if (!ok) {
            turn_fail("MUSE DIDN'T TAKE IT");
        }
        break;
    case K_TTS:
        tts_end(s, ok);
        break;
    default:
        break;
    }
    return true;
}

static bool on_http_error(stream_t *s, const ApplicationResponseView &resp)
{
    char body[160];
    size_t n = resp.body.size() < sizeof(body) - 1 ? resp.body.size() : sizeof(body) - 1;
    memcpy(body, resp.body.data(), n);
    body[n] = '\0';
    ESP_LOGW(TAG, "stream %lld: HTTP %d %s", (long long)s->id, (int)resp.status, body);
    return stream_end(s, false);
}

static bool on_frame(const DecodedServiceFrame &f)
{
    stream_t *s = find_stream(f.stream_id);
    if (!s) {
        return true;
    }
    switch (f.kind) {
    case ServiceFrameKind::Response:
        s->status = f.response.status;
        if(s->kind==K_NATIVE){
            for(size_t k=0;k<f.response.headers.size();k++){
                const HeaderView &h=f.response.headers[k];
                if(h.key.size()==4&&!strncasecmp(h.key.data(),"date",4)){
                    int64_t wall=native_http_date(h.value.data(),h.value.size());
                    if(wall>0){s_native_wall_offset_ms=wall-now_us()/1000;native_rfc3339(wall-(now_us()-s_native_at)/1000,s_native_started);}
                }
            }
        }
        if(s->kind==K_NATIVE_FILE&&s->status>=400)return stream_end(s,false);
        if(s->kind==K_NATIVE||s->kind==K_NATIVE_END){printf("@native {\"event\":\"http\",\"ending\":%s,\"status\":%d}\n",s->kind==K_NATIVE_END?"true":"false",s->status);fflush(stdout);if(s->status>=400)return stream_end(s,false);}
        if (f.response.status >= 400) {
            return on_http_error(s, f.response);
        }
        stream_data(s, f.response.body);
        if (f.response.end_body && alive(s, f.stream_id)) {
            return stream_end(s, true);
        }
        break;
    case ServiceFrameKind::BodyChunk:
        stream_data(s, f.body_chunk.data);
        if (f.body_chunk.end_body && alive(s, f.stream_id)) {
            return stream_end(s, true);
        }
        break;
    case ServiceFrameKind::Reset:
        ESP_LOGW(TAG, "stream %lld reset: %.*s", (long long)f.stream_id, (int)f.reset.reason.size(),
                 f.reset.reason.data());
        return stream_end(s, false);
    default:
        break;
    }
    return true;
}

/* Handles whatever the server sent. Returns false if the connection failed. */
static bool poll_socket(void)
{
    static HeaderView hdrs[16];
    for (int budget = 0; budget < 8; budget++) {
        /* Hold off while the MP3 buffer is nearly full: TCP pushes back on the VM. */
        if (s_turn.tts_msg >= 0 && MP3_BUF - s_turn.mp3_len < MP3_POLL_ROOM) {
            return true;
        }
        ssize_t n = ws_recv(s_conn.tls, s_conn.rx, SCRATCH, false);
        if (n == -2) {
            return true;
        }
        if (n == 0 || n == -1) {
            ESP_LOGW(TAG, "%s", n ? "receive error" : "server closed the connection");
            return false;
        }
        s_conn.last_rx_us = now_us();
        if (n == -3) {
            continue;
        }
        auto in = s_conn.session->ProcessInboundWebSocketPayload(ConstByteSpan(s_conn.rx, n),
                                                                 ByteSpan(s_conn.tf, SCRATCH),
                                                                 ByteSpan(s_conn.sr, SCRATCH),
                                                                 Span<HeaderView>(hdrs, 16));
        if (!in.ok()) {
            ESP_LOGW(TAG, "inbound frame: %s", in.status.str());
            return false;
        }
        if (in.frame_status == InboundFrameStatus::Complete && !on_frame(in.frame)) {
            return false;
        }
        if (!s_connected) {
            return false;   /* a handler hit a send failure */
        }
    }
    return true;
}

/* ---- Task ---- */

static void drop_connection(const char *why)
{
    bool in_turn = s_turn.phase != P_IDLE;
    bool cached=s_turn.native_direct&&s_turn.tts_msg>=0&&s_turn.msgs[s_turn.tts_msg].external_key[0];
    disconnect(why);
    s_auto_next_us = now_us() + s_auto_backoff_us;
    if(in_turn&&cached){s_local_only=true;s_turn.agent_busy=false;}
    else if (in_turn) {
        turn_fail("LOST CONNECTION TO MUSE");
    }
    muse_hatch_report(MUSE_HATCH_UNTESTED, "");
}

static void drain_out(void);
static QueueHandle_t s_device_commands;
static SemaphoreHandle_t s_device_finished;
static char s_device_answer[140];
static void device_setting_worker(void *)
{
    muse_device_command_t action;
    while(xQueueReceive(s_device_commands,&action,portMAX_DELAY)==pdTRUE){
        char answer[140];
    switch(action.kind){
    case MUSE_DEVICE_VOLUME:
    case MUSE_DEVICE_VOLUME_STEP:{int v=action.kind==MUSE_DEVICE_VOLUME?action.value:muse_settings_volume()+action.value;v=v<0?0:v>100?100:v;muse_settings_set_volume(v);muse_settings_set_speaker_on(true);muse_module_llm_set_playback_volume(v);snprintf(answer,sizeof(answer),"Speaker volume is now %d percent.",muse_settings_volume());break;}
    case MUSE_DEVICE_BRIGHTNESS:muse_settings_set_brightness(action.value);snprintf(answer,sizeof(answer),"Screen brightness is now %d percent.",muse_settings_brightness());break;
    case MUSE_DEVICE_SPEED:{muse_settings_set_speech_speed(action.value);muse_module_llm_set_speech_speed(action.value);const char *names[]={"slow","normal","fast"};snprintf(answer,sizeof(answer),"Speech speed is now %s.",names[muse_settings_speech_speed()]);break;}
    case MUSE_DEVICE_MUTE:muse_settings_set_speaker_on(!action.value);muse_module_llm_set_playback_volume(action.value?0:muse_settings_volume());snprintf(answer,sizeof(answer),"Speaker is now %s.",muse_settings_speaker_on()?"unmuted":"muted");break;
    default:strlcpy(answer,"Please use volume zero to one hundred percent, brightness ten to one hundred percent, or speech speed slow, normal, or fast.",sizeof(answer));break;
    }
        strlcpy(s_device_answer,answer,sizeof(s_device_answer));xSemaphoreGive(s_device_finished);
    }
}
static bool device_setting_apply(muse_device_command_t action,char *answer,size_t cap)
{
    /* NVS disables the flash/PSRAM cache. Its caller must have an internal
     * stack; the chat task deliberately uses PSRAM for its large network stack. */
    if(!s_device_commands){
        s_device_commands=xQueueCreate(1,sizeof(action));s_device_finished=xSemaphoreCreateBinary();
        if(!s_device_commands || !s_device_finished){if(s_device_commands)vQueueDelete(s_device_commands);if(s_device_finished)vSemaphoreDelete(s_device_finished);s_device_commands=nullptr;s_device_finished=nullptr;return false;}
        if(xTaskCreatePinnedToCoreWithCaps(device_setting_worker,"muse_device_cfg",4096,nullptr,5,nullptr,tskNO_AFFINITY,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)!=pdPASS){vQueueDelete(s_device_commands);vSemaphoreDelete(s_device_finished);s_device_commands=nullptr;s_device_finished=nullptr;return false;}
    }
    if(xQueueSend(s_device_commands,&action,0)!=pdTRUE)return false;
    if(xSemaphoreTake(s_device_finished,portMAX_DELAY)!=pdTRUE)return false;
    strlcpy(answer,s_device_answer,cap);return true;
}
static void local_device_reply(muse_device_command_t action)
{
    s_local_only=true;char answer[140];
    if(!device_setting_apply(action,answer,sizeof(answer)))strlcpy(answer,"Device setting could not be applied. Please use Settings and try again.",sizeof(answer));
    s_turn.phase=P_WAIT_REPLY;s_turn.chat_posted=true;s_turn.chat_us=s_turn.last_event_us=s_turn.last_content_us=now_us();s_turn.nmsgs=1;s_turn.msgs[0]=msg_t{};strlcpy(s_turn.msgs[0].id,"local-device-setting",sizeof(s_turn.msgs[0].id));
    emit(MUSE_HATCH_EV_HEARD,"Updating device setting...");message_done(0,answer);
    printf("@device_setting {\"volume\":%d,\"brightness\":%d,\"speed\":%d,\"speaker\":%s,\"local\":true}\n",muse_settings_volume(),muse_settings_brightness(),muse_settings_speech_speed(),muse_settings_speaker_on()?"true":"false");fflush(stdout);
}
static void restart_reply(void)
{
    if(!s_can_restart || !s_turn.texts || s_turn.text){s_transport_pending=false;return;}
    uint32_t previous=s_turn.gen;
    muse_module_llm_cancel(s_turn.gen);drain_out();s_turn.gen=++s_gen;s_turn.phase=P_WAIT_REPLY;s_turn.tts_msg=-1;s_turn.pcm_out=0;s_turn.silent=false;s_turn.mp3_len=0;s_turn.playback_paused_us=s_turn.playback_pause_check=0;
    s_turn.start_us=s_turn.last_event_us=s_turn.last_content_us=s_turn.chat_us=now_us();s_reply_shown[0]=0;
    for(int i=0;i<s_turn.nmsgs;i++){msg_t &m=s_turn.msgs[i];if(m.done&&m.len&&m.len<TEXT_MAX&&!m.native_skip)m.tts=TTS_QUEUED;m.pcm_start=m.pcm_frames=0;}
    s_playback_active=true;s_replaying=true;
    if(s_turn.native_job[0] && muse_native_voice_has_pending_sections()){
        // Stop can leave a healthy verified opening with later sections not
        // downloaded yet. Restart must resume that producer as well as audio.
        for(int i=0;i<s_turn.nmsgs;i++)s_turn.msgs[i].external_key[0]=0;
        s_turn.native_claimed=false;s_turn.native_started=false;s_replaying=false;
        muse_native_voice_cancel();
    }
    // Stop during buffering must reuse the in-flight complete-download job,
    // rather than fall back to a placeholder or a different voice.
    if(s_turn.native_job[0] && !s_turn.native_claimed){
        s_replaying=false;
        if(!muse_native_voice_rebind(previous,s_turn.gen))s_turn.native_started=false;
    }
    // A failed/cancelled direct fetch cannot be copied under the new generation.
    // Rebind a healthy in-flight job; otherwise allow a fresh fetch of the same ID.
    if(s_turn.native_direct && s_turn.native_started && !muse_native_voice_rebind(previous,s_turn.gen))s_turn.native_started=false;
    s_restart_wait=true;emit(MUSE_HATCH_EV_HEARD,"Restarting reply...");
}
static void handle(const cmd_t &cmd)
{
    switch (cmd.type) {
    case CMD_NATIVE_MESSAGE_FIXTURE:
        if(cmd.text && s_turn.phase==P_IDLE && !muse_memory_recorder_active()){
            char *text=strchr(cmd.text,'\n');
            if(text){*text++=0;
                if(turn_start(++s_gen,false,true)){
                    s_bench_reply=true;s_local_only=true;s_turn.native_direct=true;s_turn.phase=P_WAIT_REPLY;s_turn.chat_posted=true;
                    s_turn.chat_us=s_turn.last_event_us=s_turn.last_content_us=now_us();s_turn.nmsgs=1;s_turn.msgs[0]=msg_t{};
                    strlcpy(s_turn.msgs[0].id,cmd.text,sizeof(s_turn.msgs[0].id));
                    emit(MUSE_HATCH_EV_HEARD,"Replaying saved Muse reply...");message_done(0,text);
                }
            }
        }free(cmd.text);break;
    case CMD_NATIVE_FIXTURE:
        if(turn_start(++s_gen,false,false)){
            s_bench_reply=true;s_local_only=true;s_turn.phase=P_WAIT_REPLY;s_turn.chat_posted=true;s_turn.chat_us=s_turn.last_event_us=s_turn.last_content_us=now_us();s_turn.nmsgs=1;s_turn.msgs[0]=msg_t{};
            strlcpy(s_turn.native_job,cmd.text?cmd.text:"44aabbccddeeff0011223302",sizeof(s_turn.native_job));s_turn.native_voice=cmd.gen==1?1:0;s_turn.native_speed=1;
            strlcpy(s_turn.msgs[0].id,"native-controls-test",sizeof(s_turn.msgs[0].id));
            emit(MUSE_HATCH_EV_HEARD,"Native Muse playback test");message_done(0,"Preparing the verified Muse audio sample.");
        }free(cmd.text);break;
    case CMD_NATIVE_FILE:native_file_start(cmd.gen);break;
    case CMD_NATIVE_RANGE:native_file_start(cmd.gen,cmd.text);free(cmd.text);break;
    case CMD_NATIVE_PROBE:native_start();break;
    case CMD_CONNECT:
        if (s_turn.phase == P_IDLE) {
            disconnect("reconnect requested");
            ensure_connected();
        }
        break;
    case CMD_FORGET:
        if (s_turn.phase != P_IDLE) {
            turn_fail("SETTINGS CHANGED");
        }
        disconnect("settings changed");
        forget_vm();
        s_auto_next_us = 0;
        s_auto_backoff_us = AUTO_RETRY_MIN_US;
        break;
    case CMD_BEGIN:
        if (cmd.gen == s_gen.load()) {
            turn_begin(cmd.gen);
        }
        break;
    case CMD_END:
        if (cmd.gen == s_turn.gen && !s_turn.text && s_turn.phase == P_LISTEN) {
            s_turn.end_requested = true;
        }
        break;
    case CMD_CANCEL:
        if (cmd.gen == s_turn.gen && !s_turn.text && s_turn.phase != P_IDLE) {
            ESP_LOGI(TAG, "turn cancelled");
            turn_finish();
        }
        break;
    case CMD_TEXT:
        if (s_turn.phase != P_IDLE && !s_turn.text) {
            muse_hatch_console("error", "BUSY WITH A VOICE TURN", nullptr);
        } else {
            text_begin(cmd.text);
        }
        free(cmd.text);
        break;
    case CMD_TEXT_CANCEL:
        if (s_turn.text && s_turn.phase != P_IDLE) {
            turn_fail("CANCELLED");
        }
        break;
    case CMD_WAKE:   /* only ends hatch_task's resting wait */
        break;
    case CMD_BENCH_CLEAR:
        if(s_bench_reply && s_turn.phase==P_IDLE){s_can_restart=false;s_bench_reply=false;muse_conversation_clear();muse_state_set_caption("Ready");}break;
    case CMD_BENCH_PLAYBACK:
        if(turn_start(++s_gen,false,false)){
            s_bench_reply=true;s_local_only=true;s_turn.phase=P_WAIT_REPLY;s_turn.chat_posted=true;s_turn.chat_us=s_turn.last_event_us=s_turn.last_content_us=now_us();s_turn.nmsgs=1;s_turn.msgs[0]=msg_t{};strlcpy(s_turn.msgs[0].id,"local-controls-test",sizeof(s_turn.msgs[0].id));
            emit(MUSE_HATCH_EV_HEARD,"Local playback test");message_done(0,"**Local playback test.** The blue lantern glowed beside the quiet river. This reply is generated on the device, with no Muse request. You can pause it, resume it, stop it, and restart it from the beginning. The text follows the voice as the story continues.\n\nThe lantern keeper walked slowly along the path. Beyond the trees, the village lights were shining. She smiled, knowing that the warm house was waiting for her. The path curved past a garden, and the night was calm.");
        }break;
    case CMD_PLAYBACK_STOP:
        if(s_turn.phase!=P_IDLE&&!s_turn.text){muse_native_voice_cancel();drain_out();turn_done(false);muse_state_set_caption("Playback stopped");}
        s_restart_wait=false;s_transport_pending=false;break;
    case CMD_PLAYBACK_RESTART:restart_reply();break;
    case CMD_DEVICE_TEST:
    case CMD_LOCAL_TEXT:
#if CONFIG_MUSE_MODULE_LLM
        if(cmd.gen==s_gen.load()){
            muse_device_command_t action=muse_device_parse(cmd.text);
            if(turn_start(cmd.gen,false,action.kind==MUSE_DEVICE_NONE)){
                mark(M_RELEASE);
                if(cmd.type==CMD_DEVICE_TEST)s_bench_reply=true;
                if(action.kind!=MUSE_DEVICE_NONE)local_device_reply(action);else post_chat(cmd.text);
                if(s_turn.phase==P_WAIT_REPLY)mark(M_SENT);
            }
        }
#endif
        free(cmd.text);break;
    }
}

static void hatch_task(void *arg)
{
    (void)arg;
    for (;;) {
        cmd_t cmd;
        /* Resting, the socket's keepalive (PING_US) needs no quicker polls.
         * Resting unconnected, Wi-Fi may nap (muse_wifi_nap): wait for a
         * turn or for waking (CMD_WAKE) instead of watching it. */
        int wait_ms = !s_connected ? (s_resting ? -1 : 200) : s_turn.phase != P_IDLE ? 2 : s_resting ? 500 : 20;
        TickType_t wait = wait_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(wait_ms);
        if (xQueueReceive(s_cmds, &cmd, wait) == pdTRUE) {
            handle(cmd);
            while (xQueueReceive(s_cmds, &cmd, 0) == pdTRUE) {
                handle(cmd);
            }
        }
        if(s_turn.phase==P_WAIT_REPLY && (s_local_only || (s_replaying&&!s_connected))){
            start_tts();decode();check_turn();vTaskDelay(pdMS_TO_TICKS(2));continue;
        }
        native_tick();
        if (!s_connected) {
            if (!muse_wifi_connected()) {
                s_auto_next_us = 0;
                s_auto_backoff_us = AUTO_RETRY_MIN_US;
            } else if (muse_hatch_configured() && now_us() >= s_auto_next_us) {
                if (ensure_connected()) {
                    s_auto_next_us = INT64_MAX;   /* until it drops */
                    s_auto_backoff_us = AUTO_RETRY_MIN_US;
                } else {
                    s_auto_next_us = now_us() + s_auto_backoff_us;
                    s_auto_backoff_us = s_auto_backoff_us * 2 < AUTO_RETRY_MAX_US ? s_auto_backoff_us * 2
                                                                                   : AUTO_RETRY_MAX_US;
                }
            }
            continue;
        }

        /* Resting, Wi-Fi may nap; don't wait for the server to go quiet. */
        if (s_resting && !muse_wifi_connected()) {
            drop_connection("Wi-Fi down");
            continue;
        }
        if (!poll_socket()) {
            drop_connection("receive failed");
            continue;
        }
        bool sent = true;
        if (s_turn.phase == P_LISTEN) {
            sent = VOICE_NOTE ? record_note() : pump_mic();
        }
        if (!sent) {
            drop_connection("send failed");
            continue;
        }
        if (s_turn.phase == P_WAIT_REPLY) {
            start_tts();
            decode();
        }
        if (!s_connected) {
            continue;
        }
        check_turn();

        int64_t t = now_us();
        if (t - s_conn.last_rx_us > DEAD_US) {
            drop_connection("server went quiet");
        } else if (s_turn.phase == P_IDLE && t - s_conn.last_use_us > IDLE_CLOSE_US) {
            disconnect("idle");
            muse_hatch_report(MUSE_HATCH_UNTESTED, "");
            s_auto_next_us = INT64_MAX;   /* the next turn connects */
        } else if (t - s_conn.last_ping_us > PING_US) {
            s_conn.last_ping_us = t;
            if (!ws_send_ping(s_conn.tls)) {
                drop_connection("ping failed");
            }
        }
    }
}

/* ---- Public API ---- */

static void post(cmd_type_t type, uint32_t gen)
{
    if (s_cmds) {
        cmd_t cmd{ type, gen, nullptr };
        xQueueSend(s_cmds, &cmd, pdMS_TO_TICKS(100));
    }
}

static void drain_out(void)
{
    static int16_t junk[256];
    while (xStreamBufferReceive(s_out, junk, sizeof(junk), 0)) {
    }
}

extern "C" void muse_hatch_start(void)
{
    if (s_cmds) {
        return;
    }
    cJSON_Hooks hooks = { json_alloc, heap_caps_free };
    cJSON_InitHooks(&hooks);
    /* Keep the command/event queues out of internal DRAM, which Wi-Fi, BLE and
     * mbedTLS can exhaust: when the allocation failed here hatch_task never
     * started and every turn was silently dropped. Fall back to internal RAM
     * on parts without usable PSRAM. */
    s_cmds = xQueueCreateWithCaps(16, sizeof(cmd_t), MALLOC_CAP_SPIRAM);
    if (!s_cmds) {
        s_cmds = xQueueCreate(16, sizeof(cmd_t));
    }
    s_events = xQueueCreateWithCaps(16, sizeof(ev_t), MALLOC_CAP_SPIRAM);
    if (!s_events) {
        s_events = xQueueCreate(16, sizeof(ev_t));
    }
    s_in = xStreamBufferCreateWithCaps(IN_BYTES, 1, MALLOC_CAP_SPIRAM);
    s_out = xStreamBufferCreateWithCaps(OUT_BYTES, 1, MALLOC_CAP_SPIRAM);
    s_turn.chunk = static_cast<uint8_t *>(psram_alloc(DICT_CHUNK_BYTES + sizeof(MUSE_HATCH_NOTE_TAIL)));
    s_turn.mp3 = static_cast<uint8_t *>(psram_alloc(MP3_BUF));
    s_turn.note = VOICE_NOTE ? static_cast<uint8_t *>(psram_alloc(NOTE_PART_BYTES)) : nullptr;
    s_turn.texts = static_cast<char *>(psram_alloc(MAX_MSGS * TEXT_MAX));   /* captions just stay untimed without it */
    s_pcm = static_cast<int16_t *>(psram_alloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t)));
    s_pcm16 = static_cast<int16_t *>(psram_alloc((MINIMP3_MAX_SAMPLES_PER_FRAME + 8) * sizeof(int16_t)));
    for (auto &s : s_streams) {
        s.line = static_cast<char *>(psram_alloc(NDJSON_LINE_MAX));
        s.cap = NDJSON_LINE_MAX;
    }
    s_turn.tts_msg = -1;
    /* Stack in PSRAM: TLS, Noise and the MP3 decoder (~16 KB of scratch) all run here. */
    if (!s_cmds || !s_events || !s_in || !s_out || !s_turn.chunk || !s_turn.mp3 || (VOICE_NOTE && !s_turn.note) || !s_pcm || !s_pcm16 ||
        xTaskCreatePinnedToCoreWithCaps(hatch_task, "muse_chat", 48 * 1024, nullptr, 5, nullptr, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "start failed");
    }
}

extern "C" void muse_hatch_chat_connect(void)
{
    post(CMD_CONNECT, 0);
}

extern "C" void muse_hatch_chat_forget(void)
{
    post(CMD_FORGET, 0);
}

extern "C" bool muse_hatch_ready(void)
{
    return s_cmds && muse_hatch_configured() && muse_wifi_connected();
}

extern "C" void muse_hatch_turn_begin(void)
{
    uint32_t gen = ++s_gen;
    xStreamBufferReset(s_in);
    drain_out();
    post(CMD_BEGIN, gen);
}

extern "C" void muse_hatch_turn_audio(const int16_t *pcm, size_t frames)
{
    size_t bytes = frames * sizeof(int16_t);
    if (xStreamBufferSend(s_in, pcm, bytes, 0) != bytes) {
        ESP_LOGW(TAG, "mic backlog full, dropped audio");
    }
}

/* Both ends move whole frames, so the buffer never splits one. */
extern "C" size_t muse_hatch_turn_audio_wait(const int16_t *pcm, size_t frames, int wait_ms)
{
    return xStreamBufferSend(s_in, pcm, frames * sizeof(int16_t), pdMS_TO_TICKS(wait_ms)) / sizeof(int16_t);
}

extern "C" void muse_hatch_turn_end(void)
{
    post(CMD_END, s_gen.load());
}

extern "C" void muse_hatch_turn_cancel(void)
{
    s_can_restart=false;
    uint32_t gen = s_gen.load();
    ++s_gen;
    post(CMD_CANCEL, gen);
    drain_out();
}

extern "C" void muse_hatch_set_resting(bool resting)
{
    if (s_resting.exchange(resting) && !resting) {
        post(CMD_WAKE, 0);
    }
}

extern "C" void muse_hatch_text_turn(char *text)
{
    cmd_t cmd{ CMD_TEXT, 0, text };
    if (!s_cmds || !muse_hatch_configured()) {
        muse_hatch_console("error", "MUSE NOT SET UP", nullptr);
        free(text);
    } else if (xQueueSend(s_cmds, &cmd, pdMS_TO_TICKS(1000)) != pdTRUE) {
        muse_hatch_console("error", "BUSY", nullptr);
        free(text);
    }
}

extern "C" void muse_hatch_text_cancel(void)
{
    post(CMD_TEXT_CANCEL, 0);
}

extern "C" bool muse_chat_recognized_turn(const char *text)
{
#if CONFIG_MUSE_MODULE_LLM
    if (!s_cmds || !text || !*text || (!muse_hatch_ready() && muse_device_parse(text).kind==MUSE_DEVICE_NONE) || strnlen(text, MUSE_LLM_TEXT_MAX + 1) > MUSE_LLM_TEXT_MAX) {
        return false;
    }
    char *owned = strdup(text);
    if (!owned) return false;
    uint32_t gen = ++s_gen;
    drain_out();
    cmd_t cmd{ CMD_LOCAL_TEXT, gen, owned };
    if (xQueueSend(s_cmds, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
        free(owned);
        return false;
    }
    return true;
#else
    (void)text;
    return false;
#endif
}

extern "C" muse_hatch_ev_t muse_hatch_turn_event(char *text, size_t cap)
{
    if (!s_events) return MUSE_HATCH_EV_NONE;
    ev_t ev;
    while (xQueueReceive(s_events, &ev, 0) == pdTRUE) {
        if (ev.gen == s_gen.load()) {
            strlcpy(text, ev.text, cap);
            return ev.type;
        }
    }
    return MUSE_HATCH_EV_NONE;
}

extern "C" bool muse_hatch_turn_caption(size_t played, char *out, size_t cap)
{
    /* The message being spoken: the last one whose speech has started. */
    const msg_t *m = nullptr;
    const char *text = nullptr;
    for (int i = 0; i < s_turn.nmsgs && s_turn.texts; i++) {
        const msg_t &c = s_turn.msgs[i];
        if (c.tts >= TTS_ACTIVE && c.len && c.pcm_start <= played) {
            m = &c;
            text = s_turn.texts + i * TEXT_MAX;
        }
    }
    if (!m) {
        /* Nothing said yet: the reply's opening page, to read while the speech is on its way. */
        for (int i = 0; i < s_turn.nmsgs && s_turn.texts; i++) {
            if (s_turn.msgs[i].len) {
                return muse_hatch_caption_at(s_turn.texts + i * TEXT_MAX, 0, out, cap);
            }
        }
        return false;
    }
    size_t len = strlen(text);
    uint32_t frames = m->pcm_frames ? m->pcm_frames : (uint32_t)(len * MIC_RATE / SPEECH_CHARS_PER_S);
    size_t at = frames ? (size_t)((uint64_t)(played - m->pcm_start) * len / frames) : 0;
    if (at >= len) {
        at = len ? len - 1 : 0;
    }

    return muse_hatch_caption_at(text, at, out, cap);
}

extern "C" size_t muse_hatch_turn_read(int16_t *pcm, size_t frames, int wait_ms)
{
    if (!s_out) return 0;
    return xStreamBufferReceive(s_out, pcm, frames * sizeof(int16_t), pdMS_TO_TICKS(wait_ms)) / sizeof(int16_t);
}

/* Bench test: decodes the embedded test_reply.mp3 exactly as a reply is decoded. */
static size_t mp3_selftest(int16_t **pcm_out)
{
    extern const uint8_t mp3_start[] asm("_binary_test_reply_mp3_start");
    extern const uint8_t mp3_end[] asm("_binary_test_reply_mp3_end");
    size_t len = mp3_end - mp3_start;
    mp3dec_t *dec = (mp3dec_t *)psram_alloc(sizeof(mp3dec_t));
    int16_t *pcm = (int16_t *)psram_alloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t));
    size_t cap = MIC_RATE * 10;
    int16_t *out = (int16_t *)psram_alloc(cap * sizeof(int16_t));
    if (!dec || !pcm || !out) {
        free(dec);
        free(pcm);
        free(out);
        return 0;
    }
    mp3dec_init(dec);
    resampler_t rs{};
    int rate = 0, frames = 0;
    size_t off = 0, n = 0;
    int64_t t0 = now_us();
    while (off < len) {
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(dec, mp3_start + off, len - off, pcm, &info);
        if (!info.frame_bytes) {
            break;
        }
        off += info.frame_bytes;
        if (!samples) {
            continue;
        }
        if (info.hz <= 0) {
            break;   /* Reject an invalid rate before ratio arithmetic. */
        }
        frames++;
        if (info.channels == 2) {
            for (int k = 0; k < samples; k++) {
                pcm[k] = (pcm[2 * k] + pcm[2 * k + 1]) / 2;
            }
        }
        if (rate != info.hz) {
            rate = info.hz;
            resampler_init(&rs, info.hz, MIC_RATE);
        }
        if (n + (size_t)samples * MIC_RATE / rate + 2 > cap) {
            break;
        }
        n += resample(&rs, pcm, samples, out + n);
    }
    ESP_LOGI(TAG, "mp3 selftest: %u bytes, %d frames at %d Hz -> %u samples (%.2f s) in %lld ms",
             (unsigned)len, frames, rate, (unsigned)n, n / (double)MIC_RATE, (now_us() - t0) / 1000);
    free(dec);
    free(pcm);
    *pcm_out = out;
    return n;
}

struct selftest_t {
    TaskHandle_t caller;
    int16_t *pcm;
    size_t n;
};

/* minimp3 wants ~16 KB of stack, more than the voice task has. */
extern "C" size_t muse_hatch_mp3_selftest(int16_t **pcm_out)
{
    selftest_t st = { xTaskGetCurrentTaskHandle(), nullptr, 0 };
    auto body = [](void *arg) {
        auto *st = (selftest_t *)arg;
        st->n = mp3_selftest(&st->pcm);
        xTaskNotifyGive(st->caller);
        vTaskSuspend(NULL);   /* the caller deletes it, which frees the PSRAM stack */
    };
    TaskHandle_t task;
    if (xTaskCreatePinnedToCoreWithCaps(body, "mp3_selftest", 32 * 1024, &st, 5, &task, 0,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        return 0;
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    vTaskDeleteWithCaps(task);
    *pcm_out = st.pcm;
    return st.n;
}

extern "C" bool muse_chat_playback_can_restart(void){return s_can_restart.load();}
extern "C" bool muse_chat_playback_busy(void){return s_transport_pending.load();}
extern "C" bool muse_chat_playback_control(bool restart){
 if(!s_cmds || (restart&&!s_can_restart.load()) || s_transport_pending.exchange(true))return false;
 cmd_t command{restart?CMD_PLAYBACK_RESTART:CMD_PLAYBACK_STOP,0,nullptr};
 if(xQueueSend(s_cmds,&command,0)!=pdTRUE){s_transport_pending=false;return false;}return true;
}
extern "C" bool muse_chat_device_command(const char *text){
 if(!s_cmds || muse_device_parse(text).kind==MUSE_DEVICE_NONE)return false;
 char *owned=strdup(text);if(!owned)return false;uint32_t gen=++s_gen;drain_out();cmd_t c{CMD_DEVICE_TEST,gen,owned};
 if(xQueueSend(s_cmds,&c,0)!=pdTRUE){free(owned);return false;}return true;
}

extern "C" void muse_chat_playback_clear(void){s_can_restart=false;}
extern "C" bool muse_chat_bench_playback(void){if(!s_cmds)return false;cmd_t c{CMD_BENCH_PLAYBACK,0,nullptr};return xQueueSend(s_cmds,&c,0)==pdTRUE;}

extern "C" bool muse_chat_clear_bench(void){if(!s_cmds)return false;cmd_t c{CMD_BENCH_CLEAR,0,nullptr};return xQueueSend(s_cmds,&c,0)==pdTRUE;}

extern "C" bool muse_chat_playback_active(void){return s_playback_active.load();}

extern "C" bool muse_chat_native_probe(void){if(!s_cmds)return false;cmd_t c{CMD_NATIVE_PROBE,0,nullptr};return xQueueSend(s_cmds,&c,0)==pdTRUE;}

extern "C" bool muse_chat_native_file(uint32_t offset){if(!s_cmds)return false;cmd_t c{CMD_NATIVE_FILE,offset,nullptr};return xQueueSend(s_cmds,&c,0)==pdTRUE;}

extern "C" bool muse_chat_native_range(const char *id,uint32_t offset){
    if(!s_cmds||s_turn.phase!=P_IDLE||!id||!*id||strlen(id)>127||offset>1024*1024||muse_memory_recorder_active())return false;
    for(const char *p=id;*p;p++)if(!isalnum((unsigned char)*p)&&*p!='-'&&*p!='_')return false;
    muse_native_voice_status_t native;muse_native_voice_status(&native);if(native.active)return false;
    muse_module_llm_status_t voice;muse_module_llm_status(&voice);if(voice.state!=MUSE_MODULE_LLM_READY)return false;
    char *owned=strdup(id);if(!owned)return false;cmd_t c{CMD_NATIVE_RANGE,offset,owned};
    if(xQueueSend(s_cmds,&c,0)!=pdTRUE){free(owned);return false;}return true;
}

extern "C" bool muse_chat_native_fixture(void){if(!s_cmds)return false;cmd_t c{CMD_NATIVE_FIXTURE,0,nullptr};return xQueueSend(s_cmds,&c,0)==pdTRUE;}

extern "C" bool muse_chat_native_message_fixture(const char *id,const char *text){
    if(!s_cmds||s_turn.phase!=P_IDLE||!id||!*id||strlen(id)>=sizeof(s_turn.msgs[0].id)||!text||!*text||strlen(text)>=TEXT_MAX||muse_memory_recorder_active())return false;
    for(const char *p=id;*p;p++)if(!isalnum((unsigned char)*p)&&*p!='-'&&*p!='_')return false;
    muse_module_llm_status_t voice;muse_module_llm_status(&voice);muse_native_voice_status_t native;muse_native_voice_status(&native);
    if(voice.state!=MUSE_MODULE_LLM_READY||native.active)return false;
    size_t n=strlen(id)+strlen(text)+2;char *owned=(char *)malloc(n);if(!owned)return false;
    snprintf(owned,n,"%s\n%s",id,text);cmd_t c{CMD_NATIVE_MESSAGE_FIXTURE,0,owned};
    if(xQueueSend(s_cmds,&c,0)!=pdTRUE){free(owned);return false;}return true;
}

extern "C" bool muse_chat_native_fixture_voice_key(const char *key,unsigned voice){if(!s_cmds||voice>1||!key||strlen(key)!=24||muse_memory_recorder_active())return false;for(const char *p=key;*p;p++)if(!isxdigit((unsigned char)*p))return false;char *copy=strdup(key);if(!copy)return false;cmd_t c{CMD_NATIVE_FIXTURE,voice,copy};if(xQueueSend(s_cmds,&c,0)!=pdTRUE){free(copy);return false;}return true;}
extern "C" bool muse_chat_native_fixture_key(const char *key){return muse_chat_native_fixture_voice_key(key,0);}
