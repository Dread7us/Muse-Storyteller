# SPDX-License-Identifier: Apache-2.0
"""Run the actual UART framing, model validation and narration queue on the host."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
JSON = Path(os.environ.get('CJSON_SOURCE_DIR', ROOT / 'managed_components/espressif__cjson/cJSON'))


class ModuleSpeechProtocol(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        out = Path(cls.tmp.name)
        harness = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "muse_module_llm_protocol.h"

static int frames;
static char received[128];
static void capture(const char *frame, size_t len, void *ctx) {
    (void)ctx;
    assert(len < sizeof(received));
    memcpy(received, frame, len); received[len] = 0; frames++;
}
static void framing(void) {
    const char *input = "{\"text\":\"escaped\\nnewline\"}\r\n";
    for (size_t split = 1; split <= strlen(input); split++) {
        char buffer[64]; muse_llm_framer_t f;
        muse_llm_framer_init(&f, buffer, sizeof(buffer)); frames = 0;
        size_t len = strlen(input);
        for (size_t n = 0; n < len; n += split)
            muse_llm_framer_feed(&f, input + n, len - n < split ? len - n : split, capture, NULL);
        assert(frames == 1 && !strcmp(received, "{\"text\":\"escaped\\nnewline\"}"));
    }
    char small[8]; muse_llm_framer_t f;
    muse_llm_framer_init(&f, small, sizeof(small)); frames = 0;
    const char *overflow = "123456789012\n{}\n";
    muse_llm_framer_feed(&f, overflow, strlen(overflow), capture, NULL);
    assert(frames == 1 && !strcmp(received, "{}"));
    const char nul[] = {'{',0,'}', '\n', '{','}', '\n'};
    muse_llm_framer_feed(&f, nul, sizeof(nul), capture, NULL);
    assert(frames == 2 && !f.dropping);
}
static bool parse(const char *s) {
    muse_llm_reply_t r;
    bool ok = muse_llm_reply_parse(s, strlen(s), &r);
    if (ok) muse_llm_reply_free(&r);
    return ok;
}
static void replies(void) {
    const char *valid = "{\"request_id\":\"a\",\"work_id\":\"sys\",\"error\":{\"code\":0},\"data\":\"v1.3\"}";
    muse_llm_reply_t r;
    assert(muse_llm_reply_parse(valid, strlen(valid), &r));
    assert(muse_llm_reply_matches(&r, "a", "sys"));
    assert(!muse_llm_reply_matches(&r, "late", "sys"));
    assert(!muse_llm_reply_matches(&r, "a", "melotts.1000"));
    assert(!r.error); muse_llm_reply_free(&r);
    assert(parse("{\"request_id\":\"a\",\"work_id\":\"sys\",\"error\":{\"code\":-6}}"));
    assert(!parse("{\"request_id\":\"a\",\"work_id\":\"sys\"}"));
    assert(!parse("{\"request_id\":\"a\",\"work_id\":\"sys\",\"error\":{\"code\":0.5}}"));
    assert(!parse("{\"request_id\":\"a\",\"work_id\":\"sys\",\"error\":{\"code\":1e999}}"));
    assert(!parse("{\"request_id\":\"a\",\"work_id\":\"sys\",\"error\":{\"code\":\"0\"}}"));
    assert(!parse("{\"request_id\":\"a\\u0000bad\",\"work_id\":\"sys\",\"error\":{\"code\":0}}"));
    char trailing[256]; snprintf(trailing, sizeof(trailing), "%s{}", valid);
    assert(!parse(trailing));
    assert(!parse("module boot log")); assert(!parse("[]"));
}
static void inventory(void) {
    cJSON *models = cJSON_Parse("[{\"type\":\"tts\",\"model\":\"single_speaker_english_fast\"},"
        "{\"type\":\"asr\",\"model\":\"observed-asr-model\"}]");
    assert(models);
    assert(muse_llm_model_present(models, "tts", "single_speaker_english_fast"));
    assert(!muse_llm_model_present(models, "tts", "single-speaker-english-fast"));
    assert(!muse_llm_model_present(models, "melotts", "single_speaker_english_fast"));
    assert(!muse_llm_model_present(models, "asr", ""));
    assert(!muse_llm_model_present(models, "asr", "SINGLE_SPEAKER_ENGLISH_FAST"));
    assert(!muse_llm_model_name_valid("../model"));
    assert(!muse_llm_model_name_valid("model\n"));
    assert(!muse_llm_model_name_valid(".."));
    assert(muse_llm_service_valid("asr", true));
    assert(!muse_llm_service_valid("llm", true));
    assert(!muse_llm_service_valid("https://api.example.com", false));
    assert(!muse_llm_probe_request("a", "reset"));
    assert(!muse_llm_probe_request("a", "setup"));
    assert(!muse_llm_probe_request("a", "inference"));
    char *request = muse_llm_probe_request("a\"b", "lsmode");
    assert(request);
    cJSON *json = cJSON_Parse(request);
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(json, "work_id")), "sys"));
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(json, "request_id")), "a\"b"));
    cJSON_Delete(json); cJSON_free(request); cJSON_Delete(models);
}
static void queue_chunks(void) {
    muse_llm_speech_queue_t *q = calloc(1, sizeof(*q)); assert(q);
    muse_llm_speech_queue_reset(q, 42);
    char source[4096] = "";
    for (int i = 0; i < 12; i++) strcat(source, "English sentence. Français été. 日本語の文。 An emoji: 🎵! ");
    assert(muse_llm_speech_queue_add(q, 42, source));
    assert(q->count > 1);
    char result[4096] = "", part[MUSE_LLM_CHUNK_MAX + 1];
    while (muse_llm_speech_queue_take(q, 42, part, sizeof(part))) {
        assert(strlen(part) <= MUSE_LLM_CHUNK_MAX);
        assert(((unsigned char)part[0] & 0xc0) != 0x80);
        strcat(result, part);
    }
    assert(!strcmp(source, result));
    assert(!muse_llm_speech_queue_add(q, 42, "bad\xc0\xaf"));
    assert(!muse_llm_speech_queue_add(q, 42, "bad\xed\xa0\x80"));
    assert(!muse_llm_speech_queue_add(q, 42, "bad\xf4\x90\x80\x80"));
    assert(!muse_llm_speech_queue_add(q, 42, "bad\xe2\x82"));
    assert(!muse_llm_speech_queue_add(q, 42, "bad\001"));
    char long_text[MUSE_LLM_TEXT_MAX + 2];
    memset(long_text, 'x', sizeof(long_text)); long_text[sizeof(long_text)-1] = 0;
    assert(!muse_llm_speech_queue_add(q, 42, long_text)); assert(!q->count);
    long_text[MUSE_LLM_TEXT_MAX] = 0;
    assert(muse_llm_speech_queue_add(q, 42, long_text));
    size_t total = 0;
    while (muse_llm_speech_queue_take(q, 42, part, sizeof(part))) total += strlen(part);
    assert(total == MUSE_LLM_TEXT_MAX);
    free(q);
}
static void cancel_and_backpressure(void) {
    muse_llm_speech_queue_t *q = calloc(1, sizeof(*q)); assert(q);
    muse_llm_speech_queue_reset(q, 1);
    for (int i = 0; i < MUSE_LLM_QUEUE_MAX; i++) assert(muse_llm_speech_queue_add(q, 1, "sentence"));
    assert(!muse_llm_speech_queue_add(q, 1, "overflow"));
    assert(q->count == MUSE_LLM_QUEUE_MAX);
    char part[MUSE_LLM_CHUNK_MAX + 1];
    assert(!muse_llm_speech_queue_take(q, 2, part, sizeof(part)));
    assert(!muse_llm_speech_queue_take(q, 1, part, 2)); assert(q->count == MUSE_LLM_QUEUE_MAX);
    muse_llm_speech_queue_reset(q, 2);
    assert(!q->count);
    assert(!muse_llm_speech_queue_add(q, 1, "late reply"));
    assert(muse_llm_speech_queue_add(q, 2, "new reply"));
    assert(muse_llm_speech_queue_take(q, 2, part, sizeof(part)));
    assert(!strcmp(part, "new reply"));
    free(q);
}

static void legacy_inventory(void) {
    const char *fixtures[] = {
      "[{\"type\":\"asr\",\"mode\":\"sherpa-ncnn-streaming-zipformer-20M-2023-02-17\"},{\"type\":\"tts\",\"mode\":\"melotts_zh-cn\"}]",
      "[{\"type\":\"tts\",\"model\":\"melotts_zh-cn\",\"mode\":\"melotts_zh-cn\"}]"
    };
    for (unsigned i = 0; i < 2; i++) {
        cJSON *m = cJSON_Parse(fixtures[i]); assert(m);
        assert(muse_llm_model_present(m, "tts", "melotts_zh-cn"));
        assert(!muse_llm_model_present(m, "melotts", "melotts_zh-cn"));
        cJSON_Delete(m);
    }
    const char *bad[] = {
      "[{\"type\":\"tts\",\"model\":\"other\",\"mode\":\"melotts_zh-cn\"}]",
      "[{\"type\":\"tts\",\"model\":null,\"mode\":\"melotts_zh-cn\"}]",
      "[{\"type\":\"tts\",\"model\":\"melotts_zh-cn\",\"mode\":false}]",
      "[{\"type\":\"tts\",\"model\":\"melotts_zh-cn\",\"mode\":\"\"}]",
      "[{\"type\":\"asr\",\"mode\":\"melotts_zh-cn\"}]"
    };
    for (unsigned i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) {
        cJSON *m = cJSON_Parse(bad[i]); assert(m);
        assert(!muse_llm_model_present(m, "tts", "melotts_zh-cn"));
        cJSON_Delete(m);
    }
}
static void bridge_protocol(void) {
    assert(!muse_llm_bridge_request("x", "0123abcd", "reset", 1, NULL, 0));
    assert(!muse_llm_bridge_request("x", "0123abcd", "speak", 0, "Hi", 50));
    assert(!muse_llm_bridge_request("x", "0123abcd", "speak", 1, "Hi", 101));
    assert(!muse_llm_bridge_request("x", "0123abcd", "speak", 1, "  \n", 50));
    assert(!muse_llm_bridge_request("x", "0123abcd", "hello", 0, "hidden", 0));
    char *request = muse_llm_bridge_request("x", "0123abcd", "speak", 4294967295U, "Hello 🎵", 70); assert(request);
    cJSON *q = cJSON_Parse(request); assert(q);
    cJSON *d = cJSON_GetObjectItem(q, "data");
    assert(cJSON_GetObjectItem(d, "generation")->valuedouble == 4294967295.0);
    assert(cJSON_GetObjectItem(d, "volume")->valueint == 70);
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(q,"work_id")),"musevoice"));
    cJSON_Delete(q); cJSON_free(request);
    const char *good = "{\"request_id\":\"x\",\"work_id\":\"musevoice\",\"object\":\"musevoice.event\",\"error\":{\"code\":0},\"data\":{\"protocol\":1,\"session\":\"0123abcd\",\"generation\":9,\"event\":\"transcript\",\"text\":\"Tell me a story.\",\"final\":true}}";
    muse_llm_reply_t r; muse_llm_bridge_event_t event;
    assert(muse_llm_reply_parse(good, strlen(good), &r));
    assert(muse_llm_bridge_event(&r,"0123abcd","x",9,&event));
    assert(!strcmp(event.text,"Tell me a story."));
    assert(!muse_llm_bridge_event(&r,"0123abcd","old",9,&event));
    assert(!muse_llm_bridge_event(&r,"0123abcd","x",8,&event));
    cJSON_ReplaceItemInObject((cJSON *)r.data,"final",cJSON_CreateFalse());
    assert(!muse_llm_bridge_event(&r,"0123abcd","x",9,&event));
    muse_llm_reply_free(&r);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    switch (atoi(argv[1])) {
    case 0: framing(); break;
    case 1: replies(); break;
    case 2: inventory(); break;
    case 3: queue_chunks(); break;
    case 4: cancel_and_backpressure(); break;
    case 5: legacy_inventory(); break;
    case 6: bridge_protocol(); break;
    default: return 2;
    }
}
'''
        (out / 'protocol.c').write_text(harness)
        command = [*shlex.split(os.environ.get('CC', 'cc')), '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                   '-I', str(JSON), '-I', str(ROOT / 'components/muse'),
                   str(out / 'protocol.c'), str(ROOT / 'components/muse/muse_module_llm_protocol.c'),
                   str(JSON / 'cJSON.c'), '-lm', '-o', str(out / 'protocol')]
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        cls.binary = out / 'protocol'

    def run_case(self, number):
        result = subprocess.run([str(self.binary), str(number)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_fragmented_oversized_and_nul_uart_frames(self):
        self.run_case(0)

    def test_strict_reply_and_request_correlation(self):
        self.run_case(1)

    def test_exact_listed_model_matching_and_read_only_probes(self):
        self.run_case(2)

    def test_sentence_queue_preserves_unicode_and_rejects_invalid_or_long_text(self):
        self.run_case(3)

    def test_cancellation_backpressure_and_no_silent_truncation(self):
        self.run_case(4)

    def test_v13_catalog_mode_and_conflicting_identifiers(self):
        self.run_case(5)

    def test_bridge_wire_validation_and_final_transcript_contract(self):
        self.run_case(6)
