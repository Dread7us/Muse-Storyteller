/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MUSE_MODULE_LLM_DISCOVERING,
    MUSE_MODULE_LLM_READY,
    MUSE_MODULE_LLM_LISTENING,
    MUSE_MODULE_LLM_FINALIZING,
    MUSE_MODULE_LLM_SPEAKING,
    MUSE_MODULE_LLM_CANCELLING,
    MUSE_MODULE_LLM_NEEDS_MODELS,
    MUSE_MODULE_LLM_MODELS_MISSING,
    MUSE_MODULE_LLM_PROTOCOL_UNVERIFIED,
    MUSE_MODULE_LLM_UNAVAILABLE,
    MUSE_MODULE_LLM_ERROR,
    MUSE_MODULE_LLM_PREPARING,
} muse_module_llm_state_t;

typedef struct {
    muse_module_llm_state_t state;
    char version[32];
    char detail[96];
    bool asr_model_found, tts_model_found;
    float mic_db;
    bool mic_active;
    uint32_t issued_generations;
    int last_error;
    bool word_active;
    bool paused,pause_requested;
    unsigned word_start, word_end;
    uint32_t word_version;
} muse_module_llm_status_t;

typedef enum {
    MUSE_MODULE_LLM_PLAYBACK_NONE,
    MUSE_MODULE_LLM_PLAYBACK_QUEUED,
    MUSE_MODULE_LLM_PLAYBACK_ACTIVE,
    MUSE_MODULE_LLM_PLAYBACK_CANCELLING,
    MUSE_MODULE_LLM_PLAYBACK_DONE,
    MUSE_MODULE_LLM_PLAYBACK_CANCELLED,
    MUSE_MODULE_LLM_PLAYBACK_FAILED,
} muse_module_llm_playback_t;

/* Requires the additive musevoice v1 service on Module LLM. It uses the exact
 * configured local models, owns native audio, and never calls a paid service.
 * No full model catalog is required: hello validates the configured tasks. */
esp_err_t muse_module_llm_start(void);
void muse_module_llm_status(muse_module_llm_status_t *out);
void muse_module_llm_set_monitor(bool on);
void muse_module_llm_set_mic_level(int pct);
void muse_module_llm_set_speech_speed(int speed);
void muse_module_llm_set_playback_volume(int volume);
void muse_module_llm_toggle_pause(void);

/* Non-blocking command submission. Listening starts only after the module has
 * joined any previous speaker playback. Release requests a native ASR final. */
esp_err_t muse_module_llm_listen_begin(void);
esp_err_t muse_module_llm_listen_end(void);
/* One complete, final transcript; false leaves out unchanged. */
bool muse_module_llm_transcript(char *out, size_t cap);

/* One complete Muse message, with backpressure. generation belongs to Muse's
 * turn; the bridge receives a separate fresh generation for each utterance. */
esp_err_t muse_module_llm_speak(uint32_t generation, const char *text, unsigned volume);
muse_module_llm_playback_t muse_module_llm_playback(uint32_t generation);
/* Asynchronous: playback stays CANCELLING until the module confirms its join. */
void muse_module_llm_cancel(uint32_t generation);

#ifdef __cplusplus
}
#endif
