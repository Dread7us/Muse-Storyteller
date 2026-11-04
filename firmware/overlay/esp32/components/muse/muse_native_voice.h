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
void muse_native_voice_cancel(void);
#ifdef __cplusplus
}
#endif
