/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Limits apply to bytes, not Unicode characters. UART input is untrusted. */
#define MUSE_LLM_FRAME_MAX 32768
#define MUSE_LLM_MODEL_MAX 127
#define MUSE_LLM_CHUNK_MAX 192
#define MUSE_LLM_TEXT_MAX 8192
#define MUSE_LLM_QUEUE_MAX 64
#define MUSE_LLM_TRANSCRIPT_MAX 2048

typedef struct {
    char *data;
    size_t cap, len;
    bool dropping;
} muse_llm_framer_t;

typedef void (*muse_llm_frame_fn)(const char *frame, size_t len, void *ctx);

void muse_llm_framer_init(muse_llm_framer_t *f, char *data, size_t cap);
/* Newline-delimited frames; an oversized frame is dropped through its newline. */
void muse_llm_framer_feed(muse_llm_framer_t *f, const void *data, size_t len,
                          muse_llm_frame_fn frame, void *ctx);

typedef struct {
    cJSON *root;
    const char *request_id, *work_id, *object;
    int error;
    const cJSON *data;
} muse_llm_reply_t;

/* All pointers remain owned by root; free it with muse_llm_reply_free(). */
bool muse_llm_reply_parse(const char *frame, size_t len, muse_llm_reply_t *out);
void muse_llm_reply_free(muse_llm_reply_t *reply);
bool muse_llm_reply_matches(const muse_llm_reply_t *reply, const char *request_id, const char *work_id);

/* Exact configuration model AND category matching. Accepts v1.3 mode and
 * documented model, but rejects conflicting identifiers. This is a catalog
 * listing only, not proof that its model binary loads or performs inference. */
bool muse_llm_model_name_valid(const char *model);
bool muse_llm_model_present(const cJSON *inventory, const char *service, const char *model);
bool muse_llm_service_valid(const char *service, bool recognition);

/* Read-only commands used by the inventory task. Caller owns returned JSON. */
char *muse_llm_probe_request(const char *request_id, const char *action);

/* Additive module-side bridge, protocol v1. It owns native audio and reports
 * done/cancelled only after joining playback. UART carries text, never PCM. */
typedef enum {
    MUSE_LLM_EVENT_READY, MUSE_LLM_EVENT_LISTENING, MUSE_LLM_EVENT_ACCEPTED,
    MUSE_LLM_EVENT_SPEAKING, MUSE_LLM_EVENT_TRANSCRIPT, MUSE_LLM_EVENT_DONE,
    MUSE_LLM_EVENT_CANCELLED, MUSE_LLM_EVENT_ERROR, MUSE_LLM_EVENT_LEVEL,
    MUSE_LLM_EVENT_WORD, MUSE_LLM_EVENT_PLAYBACK,
} muse_llm_event_kind_t;
typedef struct {
    muse_llm_event_kind_t kind;
    const char *text;                 /* owned by the parsed reply */
    unsigned word_start, word_end;
} muse_llm_bridge_event_t;

bool muse_llm_text_valid(const char *text, size_t max_bytes);
char *muse_llm_bridge_request(const char *request_id, const char *session, const char *action,
                             uint32_t generation, const char *text, unsigned volume);
bool muse_llm_bridge_event(const muse_llm_reply_t *reply, const char *session, const char *request_id,
                           uint32_t generation, muse_llm_bridge_event_t *event);
bool muse_llm_bridge_ready(const muse_llm_reply_t *reply, const char *asr_model,
                           const char *tts_model);

/* A bounded narration queue. It does not equate inference ACK with playback end. */
typedef struct {
    uint32_t generation;
    unsigned head, count;
    char chunks[MUSE_LLM_QUEUE_MAX][MUSE_LLM_CHUNK_MAX + 1];
} muse_llm_speech_queue_t;

void muse_llm_speech_queue_reset(muse_llm_speech_queue_t *queue, uint32_t generation);
/* Atomic: invalid UTF-8, excessive length or a full queue leaves it unchanged. */
bool muse_llm_speech_queue_add(muse_llm_speech_queue_t *queue, uint32_t generation, const char *text);
bool muse_llm_speech_queue_take(muse_llm_speech_queue_t *queue, uint32_t generation,
                              char *text, size_t cap);

#ifdef __cplusplus
}
#endif
