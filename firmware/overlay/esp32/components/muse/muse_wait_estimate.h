/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {uint32_t version,samples;float response_s,seconds_per_word,boot_s;} muse_wait_model_t;
typedef struct {float remaining_percent;bool started,late;} muse_wait_countdown_t;
typedef struct {unsigned progress_percent,seconds;bool late;} muse_wait_view_t;
void muse_wait_defaults(muse_wait_model_t *model);
bool muse_wait_model_valid(const muse_wait_model_t *model);
/* Estimate only the opening buffer, never the duration of the entire story. */
float muse_wait_budget(const muse_wait_model_t *model,unsigned opening_words);
muse_wait_view_t muse_wait_countdown(muse_wait_countdown_t *state,float elapsed,float budget,bool ready);
void muse_wait_learn(muse_wait_model_t *model,float response_s,float preparation_s,unsigned words);
muse_wait_model_t *muse_wait_model(void);
void muse_wait_save(void);
#ifdef __cplusplus
}
#endif
