/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { char id[33],title[96]; bool saved; } muse_memory_item_t;
typedef struct {
    uint32_t version,content_version;
    char status[96],chat[33];
    bool library,more,busy,resumed,bold_start,code_start;
    unsigned page,pages,library_offset,count;
    uint8_t style_start;
    uint32_t action_version;
    char action[12],action_chat[33],suggestion[96],title_token[25];
    unsigned title_variant,title_choices;
    muse_memory_item_t items[5];
} muse_memory_view_t;
typedef struct {bool supported,active,audio,older,newer;unsigned seconds,lines,audio_mb,page,pages;uint32_t version;char state[16],note[96],id[33],text[1025];} muse_recorder_view_t;
void muse_memory_recorder_copy(muse_recorder_view_t *out);
bool muse_memory_recorder_active(void);
bool muse_memory_recorder_action(const char *op,unsigned page,unsigned level);
void muse_memory_init(void);
typedef struct {
    bool valid,learned,pending,refresh_supported,refresh_pending;
    char refresh_note[96],last_sync[32],refresh_state[12],refresh_id[33];
    unsigned used,observed,age;
    uint32_t received_ms;
    char reset[20],checked[24];
} muse_usage_view_t;
bool muse_memory_usage_copy(muse_usage_view_t *out);
bool muse_memory_usage_check(void);
void muse_memory_usage_poll(void);
bool muse_memory_switching(void);
void muse_memory_record(bool user,const char *text);
bool muse_memory_action(const char *op,const char *chat,unsigned page);
bool muse_memory_rename(const char *chat,const char *token);
bool muse_memory_copy(muse_memory_view_t *out,char *text,size_t cap);
/* Only the UART owner calls wire functions. They never touch voice request IDs. */
void muse_memory_tick(const char *session,bool idle);
bool muse_memory_reply(const cJSON *root,const char *session);
/* Chat worker only; bounded wait, never the UI or audio task. Caller frees. */
char *muse_memory_prompt(const char *question);
#ifdef __cplusplus
}
#endif
