# Native Muse narration

Edition 0.3.0 adds an optional native Muse audio path. It was built and tested on
the reference Core2 v1.1 + Module LLM installation. The portable installer does
not yet provision its additional timing assets automatically; fresh installations
should keep Local selected until those prerequisites have been installed.

## On the device

Open Sound, choose **Muse / online**, then select **Helio** or **Aria**. Both
voices were tested through native generation and device playback. Source, voice
and Slow / Normal / Fast apply to the next reply. Volume remains live. The local
Piper voice remains installed, and native preparation failures use it before
playback starts. An interrupted native stream is stopped instead of repeating
the whole reply with another voice. Stop and Restart use the current reply cache;
replay does not generate a new Muse response.

The raw Muse catalogue is not an authoritative account-eligible list. These two
verified voices are offered; the other catalogue entries are not advertised as
available. The currently tested native connection does not expose an authoritative
voice-options interface. No account restrictions are bypassed.

## How it works

Muse writes the exact response script and launches `tools/muse_vm_tts_job.py`
using its existing built-in speech facility. Core2 prefetches the growing MP3
through its existing authenticated native file connection while Muse prepares
the reply. It transfers bounded blocks to the Module, which decodes and aligns
them against the exact script. Word offsets refer to the original UTF-8 text.
The UI, volume and playback-control path remain separate from network/analysis work.

No PC, browser extension or phone is required for this voice path after setup.
Muse speech generation uses the existing account allowance; it is not unlimited
offline synthesis. Piper and Whisper recognition continue to run locally.

## Additional prerequisites

- The paired Muse VM must provide its existing `/opt/hatch/bin/tts` facility and
  native `GET /fs/raw/` file transport. HTTP failures are not treated as audio.
- Install the reviewed job helper as `~/muse-storyteller/tools/muse_vm_tts_job.py`
  on that VM, with the matching `module/runtime/muse_speech_text.py` beside it.
  Use an immutable repository revision and verify SHA-256 after downloading;
  do not have an LLM retype or reconstruct the helper.
- The Module requires compatible ARM64 Python CTranslate2 and tokenizers packages,
  NumPy, a Tiny English CTranslate2 Whisper model, faster-whisper's matching
  feature extractor/tokenizer helpers, and the SDK's CC0 minimp3 decoder compiled
  for the Module. The reference asset root is `/opt/muse-cloud-voice-v41/`, with
  `vendor/`, `tiny-model/`, `feature_extractor.py`, `native_tokenizer.py`, and
  `decode_mp3`. These model/native dependencies are not embedded in this repo.
- Deploy `muse_native_audio.py` and `muse_cloud_worker.py` with the matching
  `muse_voice_bridge.py`. Stop/save Recorder and finish playback first, retain
  exact backups, and validate both local startup and native playback afterward.

## Bounds and validation

Each VM job has a fresh 24-character hexadecimal ID, with `source.md` under
`~/muse-storyteller/replies/<job-id>/`. The helper accepts at most 8 KiB of script,
produces at most 32 MiB of audio, times out after 180 seconds, and exclusively
claims a job to prevent duplicate synthesis. No listener, new provider, account
credential export or new paid subscription is introduced.

The helper atomically replaces `meta.json`. `streaming` allows early buffering;
only `complete`, matching byte counts and SHA-256 prove a complete transfer.
The device checks the script hash and requested voice/speed as well. Cached audio
uses separate storage from conversations; low free space rejects cache writes
rather than consuming the last space needed by archives/Recorder. Old process
cache folders are not conversation archives and are not automatically purged.

Tests cover incremental output, integrity, duplicate jobs, partial-output failure,
input limits, timeouts, real Module MP3 decoding and original-text word offsets.
Hardware tests cover native playback, live volume, pause/resume, Settings, stop,
replay, healthy reboot and advancing UI frames. Full-response generation and
buffering still add latency; the helper's first-byte time is not end-to-end reply
latency. See VALIDATION.md for measured reference results.
