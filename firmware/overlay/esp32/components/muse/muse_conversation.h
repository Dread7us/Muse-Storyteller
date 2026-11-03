/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "muse_conversation_model.h"
#ifdef __cplusplus
extern "C" {
#endif
void muse_conversation_init(void);
void muse_conversation_clear(void);
uint32_t muse_conversation_add(bool user, const char *text);
uint32_t muse_conversation_add_transient(bool user,const char *text);
void muse_conversation_bind_speech(uint32_t id);
void muse_conversation_diagnostic(uint32_t *version,unsigned *count,size_t *bytes);
bool muse_conversation_word_position(uint32_t version, unsigned start, unsigned end, size_t *out_start, size_t *out_end);
bool muse_conversation_copy(char *out, size_t cap, uint32_t *version, size_t *latest);
#ifdef __cplusplus
}
#endif
