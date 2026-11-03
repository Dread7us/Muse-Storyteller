/* SPDX-License-Identifier: Apache-2.0 */
#include "muse_conversation_model.h"
#include <string.h>
bool muse_conversation_model_add(muse_conversation_model_t *m, bool user, const char *text)
{
    if (!m || !text || !*text) return false;
    size_t len = 0;
    while (len <= MUSE_CONVERSATION_TEXT_MAX && text[len]) len++;
    if (len > MUSE_CONVERSATION_TEXT_MAX) return false;
    const char *prefix = user ? "You:\n" : "Muse:\n";
    size_t head = strlen(prefix), needed = head + len + 2;
    while (m->count && (m->count == MUSE_CONVERSATION_MESSAGES || m->len + needed >= sizeof(m->text))) {
        size_t drop = m->lengths[0];
        memmove(m->text, m->text + drop, m->len - drop + 1);
        m->len -= drop;
        memmove(m->lengths, m->lengths + 1, (--m->count) * sizeof(m->lengths[0]));
        memmove(m->ids, m->ids + 1, m->count * sizeof(m->ids[0]));
    }
    memcpy(m->text + m->len, prefix, head);
    memcpy(m->text + m->len + head, text, len);
    memcpy(m->text + m->len + head + len, "\n\n", 2);
    m->len += needed;
    m->text[m->len] = '\0';
    if (!++m->serial) ++m->serial;
    m->ids[m->count] = m->serial;
    m->lengths[m->count++] = needed;
    return true;
}
