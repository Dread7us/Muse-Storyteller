/* SPDX-License-Identifier: Apache-2.0 */
#include "muse_conversation.h"
#include "muse_memory.h"
#include "muse_mem.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
static muse_conversation_model_t *s_model;
static SemaphoreHandle_t s_lock;
static uint32_t s_version;
static uint32_t s_speaking_id;
void muse_conversation_clear(void)
{
    if(!s_lock || !s_model)return;
    xSemaphoreTake(s_lock,portMAX_DELAY);
    uint32_t serial=s_model->serial;memset(s_model,0,sizeof(*s_model));s_model->serial=serial;
    s_speaking_id=0;s_version++;
    xSemaphoreGive(s_lock);
}
void muse_conversation_init(void)
{
    if (s_lock) return;
    static StaticSemaphore_t storage;
    s_lock = xSemaphoreCreateMutexStatic(&storage);
    s_model = heap_caps_calloc(1, sizeof(*s_model), MUSE_BIG_CAPS);
}
static uint32_t conversation_add(bool user, const char *text, bool persist)
{
    if (!s_lock || !s_model) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t id = 0;
    if (muse_conversation_model_add(s_model, user, text)) {
        s_version++;
        id = s_model->ids[s_model->count - 1];
    }
    xSemaphoreGive(s_lock);
    if (id && persist) muse_memory_record(user,text);
    return id;
}
void muse_conversation_bind_speech(uint32_t id)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_speaking_id = id;
    xSemaphoreGive(s_lock);
}
bool muse_conversation_word_position(uint32_t version, unsigned start, unsigned end, size_t *out_start, size_t *out_end)
{
    if (!s_lock || !s_model || !out_start || !out_end || start >= end) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool found = false;
    size_t offset = 0;
    if (version == s_version) for (unsigned i = 0; i < s_model->count; i++) {
        if (s_model->ids[i] == s_speaking_id && !memcmp(s_model->text + offset, "Muse:\n", 6) &&
            end <= s_model->lengths[i] - 8) {
            *out_start = offset + 6 + start;
            *out_end = offset + 6 + end;
            found = true;
            break;
        }
        offset += s_model->lengths[i];
    }
    xSemaphoreGive(s_lock);
    return found;
}
bool muse_conversation_copy(char *out, size_t cap, uint32_t *version, size_t *latest)
{
    if (!out || !version || cap < MUSE_CONVERSATION_CAP || !s_lock || !s_model) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool changed = *version != s_version;
    if (changed) {
        memcpy(out, s_model->text, s_model->len + 1);
        *version = s_version;
        if (latest) *latest = s_model->count ? s_model->len - s_model->lengths[s_model->count - 1] : 0;
    }
    xSemaphoreGive(s_lock);
    return changed;
}

uint32_t muse_conversation_add(bool user,const char *text){return conversation_add(user,text,true);}
uint32_t muse_conversation_add_transient(bool user,const char *text){return conversation_add(user,text,false);}

void muse_conversation_diagnostic(uint32_t *version,unsigned *count,size_t *bytes){if(!s_lock||!s_model)return;xSemaphoreTake(s_lock,portMAX_DELAY);*version=s_version;*count=s_model->count;*bytes=s_model->len;xSemaphoreGive(s_lock);}
