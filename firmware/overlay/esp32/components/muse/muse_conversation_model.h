/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define MUSE_CONVERSATION_CAP 32768
#define MUSE_CONVERSATION_MESSAGES 8
#define MUSE_CONVERSATION_TEXT_MAX 8192
typedef struct {
    char text[MUSE_CONVERSATION_CAP];
    size_t len, lengths[MUSE_CONVERSATION_MESSAGES];
    unsigned count;
    uint32_t ids[MUSE_CONVERSATION_MESSAGES], serial;
} muse_conversation_model_t;
/* Retains complete recent messages, evicting only whole oldest messages. */
bool muse_conversation_model_add(muse_conversation_model_t *m, bool user, const char *text);
