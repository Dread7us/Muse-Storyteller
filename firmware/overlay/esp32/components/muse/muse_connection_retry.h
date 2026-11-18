#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Connection attempts only. Never use this to repeat a submitted chat task.
 * Equal jitter: 1–2, 2–4, 4–8, 8–16, 16–32, then 30–60 seconds. */
static inline unsigned muse_connection_retry_delay_ms(unsigned failures, uint32_t random)
{
    unsigned cap = 2000;
    for (unsigned i = 1; i < failures && cap < 60000; ++i) {
        cap = cap > 30000 ? 60000 : cap * 2;
    }
    return cap / 2 + random % (cap / 2 + 1);
}

static inline bool muse_connection_retry_automatic(int status)
{
    return status <= 0 || status == 408 || status == 425 || status == 429 ||
           (status >= 500 && status < 600);
}
