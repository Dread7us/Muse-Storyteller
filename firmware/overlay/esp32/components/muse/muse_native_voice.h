/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "cJSON.h"
#ifdef __cplusplus
extern "C" {
#endif
bool muse_native_voice_reply(const cJSON *root,const char *session);
bool muse_native_voice_begin(const char *key,uint32_t generation,int voice,int speed);
/* 0 preparing; 1 script/cache ready; -1 unavailable; -2 another job stopping. */
int muse_native_voice_copy(uint32_t generation,char *text,size_t capacity,char key[25]);
bool muse_native_voice_rebind(uint32_t previous,uint32_t current);
void muse_native_voice_cancel(void);
typedef struct {
    unsigned phase,received,total,elapsed_ms,error;
    bool active;
    char job[25];
} muse_native_voice_status_t;
/* 1 waiting for Muse, 2 buffering, 3 verifying, 4 ready, 5 failed, 6 cancelled. */
void muse_native_voice_status(muse_native_voice_status_t *out);
void muse_native_voice_answer_ready(uint32_t generation,bool pending);
#ifdef __cplusplus
}
#endif
