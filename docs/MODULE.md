# Module runtime reference

The Python runtime in `module/runtime` is the application source, not a dump of a Module.
It excludes state directories, ownership ledgers, native binaries, model weights, transcripts,
account observations and backups.

## Tested arrangement

The reference Module used a StackFlow 1.3 base with privately extracted Whisper 1.8
runtime/libraries and the vendor’s Base model package. No system-wide framework upgrade
was needed. Newer vendor firmware or other boards need their own compatibility check.
The original recognizer resources were retained for rollback.

Expected resources include:

```text
/opt/muse-core2-bridge/
  muse_*.py                 application runtime
  piper/                    external aarch64 Piper distribution
  en_US-lessac-low.onnx      external voice model
  en_US-lessac-low.onnx.json external voice configuration
/opt/muse-whisper-base/
  bin/llm_whisper-1.8
  lib/                      isolated vendor libraries
  data/whisper-base/
  mode_whisper-base.json     English configuration
  audio_rpc_shim.so
/opt/m5stack/data/whisper-base -> /opt/muse-whisper-base/data/whisper-base
```

Existing vendor audio/ASR services and compatibility model resources are still used for
audio ownership and protocol setup. This is not a self-contained replacement for StackFlow.
The optional local transcript reviewer uses the existing Qwen 0.5B service/model; the local
model reviews transcripts and does not replace Muse as the answer generator.

## Inspect before installing

```sh
python3 module/doctor.py
```

Run that on the Module. It reads resource existence, architecture, libzmq availability and
disk space. It does not initialize audio, record speech, access account state or change a service.

Review the upstream package and model licenses, then preview pinned downloads:

```sh
python tools/fetch_assets.py
```

Add `--download` only after reviewing those terms. Downloads go to ignored
`assets-downloads/` and must match the recorded size and SHA-256. This command does not
extract packages, install them, or enable a service.

## Manual resource staging

Use a separate staging directory for the downloaded vendor packages. Inspect `.deb`
contents with `dpkg-deb -c`; extract privately with `dpkg-deb -x`, not a blanket framework
upgrade. Retain the runtime’s library layout and model data. Set the extracted model
configuration to English (`language: en`) and inspect the actual model-specific fields
rather than replacing the whole configuration with an invented example.

Build `module/audio_rpc_shim.c` on the Module as a shared object, for example:

```sh
cc -shared -fPIC -O2 module/audio_rpc_shim.c -ldl -o audio_rpc_shim.so
```

Place the result in the dedicated Whisper directory after review. The shim is loaded only
into the adapter-owned Whisper child process; it redirects that child’s audio RPC to a
private PCM publisher. Do not configure a global `LD_PRELOAD`.

Stage the external Piper archive, voice model and matching JSON configuration at the
expected paths. Validate that PCM is mono, signed 16-bit, 16 kHz. Check native library
dependencies before attempting playback. Do not redistribute those assets as project code.

## Ownership and service activation

The bridge refuses to take over unrelated speech tasks during ordinary initialization.
If a previous application is active, stop it through its own workflow and inspect the
runtime’s explicit ownership/takeover requirements. Do not manufacture an `owned-tasks.json`
ledger or copy one from another device. Keep the original ledger and backup private.

`module/muse-core2-bridge.service.example` documents the service. Existing installations
must retain their **original state-directory path**. Inspect the example and all resources
before adapting it. Service activation and first-time task ownership are intentionally
manual in this source release; no automatic installer has been validated on a factory Module.

Keep systemd’s `KillMode=control-group` so its owned child processes are cleaned up with
the bridge. Preserve the existing UART router. Do not use the vendor TCP test interface
as an account-independent substitute for the UART route; it can change routing metadata.

## First checks

1. Read-only bridge check after the prerequisites are in place.
2. Module ready + Core2 UI heartbeat, without a reboot loop.
3. A short hold-to-talk question, then Recorder Stop & Save.
4. A brief narration with Pause, Stop and Restart; confirm word-following resets.
5. Confirm volume, mute, library contents and pairing survived.

Retain rollback resources until the actual device passes—not merely a source compilation.
