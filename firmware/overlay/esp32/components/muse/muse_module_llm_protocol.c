/* SPDX-License-Identifier: Apache-2.0 */
#include "muse_module_llm_protocol.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

void muse_llm_framer_init(muse_llm_framer_t *f, char *data, size_t cap)
{
    *f = (muse_llm_framer_t){.data = data, .cap = cap};
}

void muse_llm_framer_feed(muse_llm_framer_t *f, const void *data, size_t len,
                          muse_llm_frame_fn frame, void *ctx)
{
    const unsigned char *bytes = data;
    if (!f || !f->data || f->cap < 2 || !frame || (!data && len)) return;
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = bytes[i];
        if (ch == '\n') {
            if (!f->dropping && f->len) {
                if (f->data[f->len - 1] == '\r') f->len--;
                f->data[f->len] = '\0';
                if (f->len) frame(f->data, f->len, ctx);
            }
            f->len = 0;
            f->dropping = false;
        } else if (!f->dropping) {
            if (!ch || f->len + 1 >= f->cap) {
                f->dropping = true;
                f->len = 0;
            } else {
                f->data[f->len++] = (char)ch;
            }
        }
    }
}

bool muse_llm_reply_parse(const char *frame, size_t len, muse_llm_reply_t *out)
{
    if (!out) return false;
    *out = (muse_llm_reply_t){0};
    if (!frame || !len || len > MUSE_LLM_FRAME_MAX || memchr(frame, '\0', len)) return false;
    /* cJSON strings are NUL-terminated. Do not let an escaped NUL turn an
     * untrusted ID/model into the prefix of a different identifier. */
    for (size_t i = 0; i + 6 <= len; i++) {
        if (!memcmp(frame + i, "\\u0000", 6)) return false;
    }
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(frame, len, &end, false);
    if (!root) return false;
    while (end < frame + len && (*end == ' ' || *end == '\t' || *end == '\r')) end++;
    cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
    cJSON *code = cJSON_GetObjectItemCaseSensitive(error, "code");
    const char *request = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "request_id"));
    const char *work = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "work_id"));
    if (end != frame + len || !cJSON_IsObject(root) || !request || !*request || strlen(request) > 63 ||
        !work || !*work || strlen(work) > 63 || !cJSON_IsNumber(code) || !isfinite(code->valuedouble) ||
        code->valuedouble < INT_MIN || code->valuedouble > INT_MAX ||
        (double)(int)code->valuedouble != code->valuedouble) {
        cJSON_Delete(root);
        return false;
    }
    out->root = root;
    out->request_id = request;
    out->work_id = work;
    out->object = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "object"));
    out->error = (int)code->valuedouble;
    out->data = cJSON_GetObjectItemCaseSensitive(root, "data");
    return true;
}

void muse_llm_reply_free(muse_llm_reply_t *reply)
{
    if (!reply) return;
    cJSON_Delete(reply->root);
    *reply = (muse_llm_reply_t){0};
}

bool muse_llm_reply_matches(const muse_llm_reply_t *reply, const char *request_id, const char *work_id)
{
    return reply && reply->root && request_id && work_id &&
           !strcmp(reply->request_id, request_id) && !strcmp(reply->work_id, work_id);
}

bool muse_llm_model_name_valid(const char *model)
{
    if (!model || !*model || strlen(model) > MUSE_LLM_MODEL_MAX) return false;
    for (const unsigned char *p = (const unsigned char *)model; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '.' || *p == '-' || *p == '_')) return false;
    }
    return strcmp(model, ".") && strcmp(model, "..");
}

bool muse_llm_service_valid(const char *service, bool recognition)
{
    if (!service) return false;
    return recognition ? (!strcmp(service, "asr") || !strcmp(service, "whisper"))
                       : (!strcmp(service, "tts") || !strcmp(service, "melotts"));
}

bool muse_llm_model_present(const cJSON *inventory, const char *service, const char *model)
{
    if (!service || !muse_llm_model_name_valid(model) || !cJSON_IsArray(inventory)) return false;
    const cJSON *entry;
    cJSON_ArrayForEach(entry, inventory) {
        const cJSON *modern = cJSON_GetObjectItemCaseSensitive(entry, "model");
        const cJSON *legacy = cJSON_GetObjectItemCaseSensitive(entry, "mode");
        const char *a = cJSON_GetStringValue(modern);
        const char *b = cJSON_GetStringValue(legacy);
        /* Actual v1.3 configuration records use mode. A present malformed or
         * conflicting second field must not be hidden by the valid one. */
        if ((modern && !muse_llm_model_name_valid(a)) ||
            (legacy && !muse_llm_model_name_valid(b)) ||
            (a && b && strcmp(a, b))) continue;
        const char *name = a ? a : b;
        const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "type"));
        if (name && type && !strcmp(name, model) && !strcmp(type, service)) return true;
    }
    return false;
}

char *muse_llm_probe_request(const char *request_id, const char *action)
{
    if (!request_id || !*request_id || strlen(request_id) > 63 || !action ||
        (strcmp(action, "ping") && strcmp(action, "version") && strcmp(action, "lsmode"))) return NULL;
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    bool ok = cJSON_AddStringToObject(root, "request_id", request_id) &&
              cJSON_AddStringToObject(root, "work_id", "sys") &&
              cJSON_AddStringToObject(root, "action", action);
    char *json = ok ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    return json;
}

/* Strict Unicode scalar validation; reject a truncated or overlong encoding. */
static size_t scalar_len(const unsigned char *p, size_t n)
{
    unsigned c = *p;
    if (c < 0x80) return c ? 1 : 0;
    unsigned count = c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3 :
                     c >= 0xf0 && c <= 0xf4 ? 4 : 0;
    if (!count || n < count) return 0;
    for (unsigned i = 1; i < count; i++) if ((p[i] & 0xc0) != 0x80) return 0;
    if ((c == 0xe0 && p[1] < 0xa0) || (c == 0xed && p[1] >= 0xa0) ||
        (c == 0xf0 && p[1] < 0x90) || (c == 0xf4 && p[1] >= 0x90)) return 0;
    return count;
}

static bool space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* Prefer a sentence end, then a word boundary; a long word may span chunks. */
static size_t chunk_len(const char *text, size_t len)
{
    size_t n = 0, word = 0, sentence = 0;
    while (n < len) {
        size_t s = scalar_len((const unsigned char *)text + n, len - n);
        if (!s || n + s > MUSE_LLM_CHUNK_MAX) break;
        n += s;
        if (space((unsigned char)text[n - 1])) word = n;
        if ((text[n - 1] == '.' || text[n - 1] == '?' || text[n - 1] == '!') &&
            (n == len || space((unsigned char)text[n]))) sentence = n;
    }
    if (n == len) return n;
    return sentence ? sentence : word ? word : n;
}

void muse_llm_speech_queue_reset(muse_llm_speech_queue_t *queue, uint32_t generation)
{
    if (!queue) return;
    queue->generation = generation;
    queue->head = queue->count = 0;
}

bool muse_llm_speech_queue_add(muse_llm_speech_queue_t *queue, uint32_t generation, const char *text)
{
    if (!queue || queue->generation != generation || !text) return false;
    size_t len = strnlen(text, MUSE_LLM_TEXT_MAX + 1);
    if (!len || len > MUSE_LLM_TEXT_MAX) return false;
    for (size_t n = 0; n < len;) {
        size_t s = scalar_len((const unsigned char *)text + n, len - n);
        if (!s || (s == 1 && (unsigned char)text[n] < 32 && !space((unsigned char)text[n]))) return false;
        n += s;
    }
    unsigned needed = 0;
    for (size_t n = 0; n < len;) {
        size_t part = chunk_len(text + n, len - n);
        if (!part) return false;
        n += part;
        needed++;
    }
    if (queue->count > MUSE_LLM_QUEUE_MAX || needed > MUSE_LLM_QUEUE_MAX - queue->count) return false;
    for (size_t n = 0; n < len;) {
        size_t part = chunk_len(text + n, len - n);
        char *out = queue->chunks[(queue->head + queue->count) % MUSE_LLM_QUEUE_MAX];
        memcpy(out, text + n, part);
        out[part] = '\0';
        queue->count++;
        n += part;
    }
    return true;
}

bool muse_llm_speech_queue_take(muse_llm_speech_queue_t *queue, uint32_t generation,
                              char *text, size_t cap)
{
    if (!queue || generation != queue->generation || !queue->count || !text) return false;
    const char *chunk = queue->chunks[queue->head];
    size_t len = strlen(chunk);
    if (cap <= len) return false;
    memcpy(text, chunk, len + 1);
    queue->head = (queue->head + 1) % MUSE_LLM_QUEUE_MAX;
    queue->count--;
    return true;
}

bool muse_llm_text_valid(const char *text, size_t max_bytes)
{
    if (!text || !max_bytes || max_bytes > MUSE_LLM_TEXT_MAX) return false;
    size_t len = strnlen(text, max_bytes + 1);
    if (!len || len > max_bytes) return false;
    bool content = false;
    for (size_t n = 0; n < len;) {
        size_t count = scalar_len((const unsigned char *)text + n, len - n);
        if (!count || (count == 1 && (unsigned char)text[n] < 32 && !space((unsigned char)text[n]))) return false;
        if (count > 1 || !space((unsigned char)text[n])) content = true;
        n += count;
    }
    return content;
}

static bool session_valid(const char *session)
{
    if (!session || strlen(session) != 8) return false;
    for (unsigned i = 0; i < 8; i++)
        if (!((session[i] >= '0' && session[i] <= '9') || (session[i] >= 'a' && session[i] <= 'f'))) return false;
    return true;
}

char *muse_llm_bridge_request(const char *request_id, const char *session, const char *action,
                             uint32_t generation, const char *text, unsigned volume)
{
    if (!request_id || !*request_id || strlen(request_id) > 63 || !action || volume > 100 || !session_valid(session)) return NULL;
    bool speech = !strcmp(action, "speak");
    bool passive = !strcmp(action, "hello") || !strcmp(action, "status");
    if (!speech && !passive && strcmp(action, "listen_begin") && strcmp(action, "listen_end") &&
        strcmp(action, "cancel")) return NULL;
    if ((!passive && !generation) || (speech ? !muse_llm_text_valid(text, MUSE_LLM_TEXT_MAX) : text != NULL)) return NULL;
    cJSON *root = cJSON_CreateObject();
    cJSON *data = root ? cJSON_AddObjectToObject(root, "data") : NULL;
    bool ok = data && cJSON_AddStringToObject(root, "request_id", request_id) &&
        cJSON_AddStringToObject(root, "work_id", "musevoice") &&
        cJSON_AddStringToObject(root, "action", action) &&
        cJSON_AddStringToObject(root, "object", "musevoice.v1") &&
        cJSON_AddNumberToObject(data, "generation", generation) && cJSON_AddStringToObject(data, "session", session);
    if (ok && speech) ok = cJSON_AddStringToObject(data, "text", text) &&
        cJSON_AddNumberToObject(data, "volume", volume);
    char *json = ok ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    return json;
}

static bool exact_uint(const cJSON *value, uint32_t expected)
{
    return cJSON_IsNumber(value) && isfinite(value->valuedouble) &&
        value->valuedouble == (double)expected;
}

bool muse_llm_bridge_event(const muse_llm_reply_t *reply, const char *session, const char *request_id,
                           uint32_t generation, muse_llm_bridge_event_t *event)
{
    if (!event || !muse_llm_reply_matches(reply, request_id, "musevoice") ||
        !reply->object || strcmp(reply->object, "musevoice.event") || !cJSON_IsObject(reply->data) ||
        !exact_uint(cJSON_GetObjectItemCaseSensitive(reply->data, "protocol"), 1) ||
        !exact_uint(cJSON_GetObjectItemCaseSensitive(reply->data, "generation"), generation)) return false;
    const char *reply_session = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(reply->data, "session"));
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(reply->data, "event"));
    if (!session_valid(session) || !reply_session || strcmp(reply_session, session) || !name) return false;
    static const char *const names[] = {"ready", "listening", "accepted", "speaking", "transcript", "done", "cancelled", "error", "level", "word", "playback"};
    unsigned kind;
    for (kind = 0; kind < sizeof(names) / sizeof(names[0]); kind++) if (!strcmp(name, names[kind])) break;
    if (kind == sizeof(names) / sizeof(names[0]) ||
        (kind == MUSE_LLM_EVENT_ERROR ? reply->error >= 0 : reply->error != 0)) return false;
    if ((kind == MUSE_LLM_EVENT_DONE || kind == MUSE_LLM_EVENT_CANCELLED) &&
        !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(reply->data, "joined"))) return false;
    *event = (muse_llm_bridge_event_t){.kind = (muse_llm_event_kind_t)kind};
    if (kind == MUSE_LLM_EVENT_WORD) {
        const cJSON *start = cJSON_GetObjectItemCaseSensitive(reply->data, "start");
        const cJSON *end = cJSON_GetObjectItemCaseSensitive(reply->data, "end");
        if (!cJSON_IsNumber(start) || !cJSON_IsNumber(end) || !isfinite(start->valuedouble) ||
            !isfinite(end->valuedouble) || start->valuedouble < 0 || end->valuedouble > MUSE_LLM_TEXT_MAX ||
            start->valuedouble >= end->valuedouble || floor(start->valuedouble) != start->valuedouble ||
            floor(end->valuedouble) != end->valuedouble) return false;
        event->word_start = (unsigned)start->valuedouble;
        event->word_end = (unsigned)end->valuedouble;
    }
    if (kind == MUSE_LLM_EVENT_TRANSCRIPT) {
        event->text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(reply->data, "text"));
        if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(reply->data, "final")) ||
            !muse_llm_text_valid(event->text, MUSE_LLM_TRANSCRIPT_MAX)) return false;
    }
    return true;
}

bool muse_llm_bridge_ready(const muse_llm_reply_t *reply, const char *asr_model,
                           const char *tts_model)
{
    if (!reply || !reply->data || !muse_llm_model_name_valid(asr_model) ||
        !muse_llm_model_name_valid(tts_model) ||
        !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(reply->data, "ready"))) return false;
    const char *asr = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(reply->data, "asr_model"));
    const char *tts = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(reply->data, "tts_model"));
    if (!asr || !tts || strcmp(asr, asr_model) || strcmp(tts, tts_model)) return false;
    const cJSON *caps = cJSON_GetObjectItemCaseSensitive(reply->data, "capabilities");
    if (!cJSON_IsArray(caps)) return false;
    static const char *const required[] = {"local_asr", "local_tts", "playback_join", "generation_cancel"};
    for (unsigned i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
        unsigned found = 0;
        const cJSON *cap;
        cJSON_ArrayForEach(cap, caps) {
            const char *s = cJSON_GetStringValue(cap);
            if (s && !strcmp(s, required[i])) found++;
        }
        if (found != 1) return false;
    }
    return true;
}
