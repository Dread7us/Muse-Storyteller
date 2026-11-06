# Native Muse narration

Edition 0.3.8 repairs and extends an optional native Muse audio path. It was built and tested on
the reference Core2 v1.1 + Module LLM installation. The portable installer does
not yet provision its additional timing assets automatically; fresh installations
should keep Local selected until those prerequisites have been installed.

## On the device

Open Sound, choose **Muse / online**, then select **Muse app voice – direct**,
**Helio**, or **Aria**. Direct uses the voice and pace selected in Muse; the device
speed slider is disabled in that mode and shows **App pace**. Helio and Aria
keep the device's Slow / Normal / Fast presets. Both
voices were tested through native generation and device playback. Source, voice
and Slow / Normal / Fast apply to the next reply. Volume remains live. The local
Piper voice remains installed as the separate Local option. A native preparation
failure now shows an explicit error and keeps Muse selected; it never silently
switches the requested voice. Failed native jobs can be retried with Restart.
Stop and Restart use the current reply cache;
replay does not generate a new Muse response.

The raw Muse catalogue is not an authoritative account-eligible list. These two
verified voices are offered; the other catalogue entries are not advertised as
available. The currently tested native connection does not expose an authoritative
voice-options interface. No account restrictions are bypassed.

## Direct narration

The direct option sends the normal chat request with `output_modality: "voice"`
and retrieves the finished message's MP3 over the existing paired connection:
`GET /api/voice/tts-stream?message_id=…`. Only a 404 retries `/voice/tts-stream`.
This follows the [official Linux client](https://github.com/facebookincubator/muse-gadget-sdk/blob/main/linux/src/musegadget/link_client.py).
It needs no separate account token export, phone, PC, file-generation prompt or
VM speech helper. It still uses the Muse account allowance and Internet access.

The background downloader uses a 256 KiB Core2 transfer buffer with bounded
backpressure. The Module stores and decodes the MP3; verified HTTP completion,
an end-to-end transfer hash and successful complete decoding are required before
speaking. The direct route allows one retry, with the same response message ID.
Each assistant message retains its own full text and audio cache. Ongoing tasks remain open across progress acknowledgements; native preparation waits for task completion and final-text settling. A revised final response receives fresh audio instead of reusing its acknowledgement cache. The current
text limit is 8 KiB per message and the audio bound is 32 MiB. A voice response
without readable text produces an explicit error rather than silently losing
the reader. Native pronunciation follows Muse; the local normalization rules
continue to apply to Piper and the prepared Helio/Aria clips.

Full buffering prevents network gaps during narration, but still adds a wait
proportional to clip length and the Core2-to-Module transfer rate. It is not a
promise of immediate playback or a sustained UI frame rate.

Edition 0.3.7 sends each 4 KiB audio block as one base64 string in a JSON packet,
with its existing `raw_info` metadata. It avoids the vendor's binary RAW framing
state, which stalled during a longer real reply. Each block's key, offset, size
and audio digest remain protected by a header checksum; the decoded body is checked
separately, then the complete MP3 is verified at EOF. The receiver uses its existing
verified admission path; this does not weaken either checksum.

A missing block acknowledgement is retried within a bounded three-second window.
At most two transfer framing recoveries are allowed per job, coordinated on the
UART operation owner; recording, playback and queued user commands take priority.
Identical acknowledged blocks can be retried safely. Failure keeps the text and
offers Restart, with separate cache, connection, download, transfer and final-check
messages. A failed direct job is replaced on Restart; a healthy running job is
rebound to the new playback generation. Text/control packets retain their protected
ASCII envelope. Ordinary JSON strings escape literal braces as Unicode escapes,
preserving their decoded text while keeping quoted code out of the vendor's brace
counter. Recovery also clears accumulated nesting left by damaged legacy binary
traffic; it does not erase pairing, voice settings or archived conversations.
This requires the matching Core2 firmware and Module bridge. Optional timing
runs on one low-priority CPU thread so it cannot monopolize both Module cores.

The reference installation also wraps its factory startup cue with
`module/runtime/muse_boot_chime.sh`. A direct Python atomic `/run` directory claim allows one playback
per boot, even if the startup script is invoked repeatedly. Installing that
wrapper requires an exact backup and a reviewed change to the factory startup
script; it is not applied automatically by the source preparation helper.

## Prepared Helio / Aria narration

Muse writes the exact response script and launches `tools/muse_vm_tts_job.py`
using its existing built-in speech facility. Core2 downloads the complete MP3 through its existing authenticated native file
connection while Muse prepares the reply. The Module verifies the complete
transfer, decodes it, and prepares word timing before speaking. Ordinary replies
use one continuous playback call; unusually long replies use bounded blocks
from the already complete local cache. Alignment errors cannot discard valid
audio. Word offsets refer to the original UTF-8 text.
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
- Deploy `muse_native_audio.py`, `muse_alignment_worker.py`, `muse_cloud_worker.py`, `muse_voice_backend.py`
  and `muse_playback_control.py` with the matching `muse_voice_bridge.py`.
  Stop/save Recorder and finish playback first, retain
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


## Buffering and preparation time

Edition 0.3.2 adds `--compact` to the VM helper. It preserves
`original.mp3` and uses FFmpeg/libmp3lame to create a 32 kbps, 16 kHz mono
`audio.mp3` for the Module's 16 kHz playback path. It publishes the delivery
as append-only bytes while synthesis is still running. Its final metadata is
published only after both synthesis and encoding complete, with the actual hash.
This is an additional lossy encoding of the same voice; the original remains
available. FFmpeg must be installed in the Muse VM. See its
[audio option documentation](https://ffmpeg.org/ffmpeg.html#Audio-Options).

Partial caches are untrusted until the Module verifies their complete MP3
against the VM's final SHA-256. Resume does not permit partial playback.
Selecting Muse voice no longer silently falls back to Piper on a preparation
failure; Local voice remains a separate Sound setting. It also shows live preparation
progress and animates the welcome screen.
Prepared Helio/Aria downloads use bounded 32 KiB network ranges and separate
1536-byte checked Module packets. Network batching does not enlarge the UART
frames. Each request retains its own capacity limit; late callbacks cannot
overwrite a later request. A known final byte count avoids an extra EOF lookup.

The Module analyzes decoded windows during transfer, then waits for complete
integrity verification before publishing timings or starting playback.
Optional neural alignment runs in a separate, low-priority process. Its pipe
messages and execution time are bounded; failure falls back to complete-audio
timing without discarding or shortening the voice clip. The analysis worker
does not inherit the router's sockets or UART handles.

Full buffering trades some initial preparation time for steady narration. A
complete cached reply restarts without downloading or generating speech again.
Audio is read from the Module's local cache; large replies need not stay entirely
in Python RAM. The native IPC playback budget is 5 MiB per call, below the
adapter's 6 MiB message bound. Longer replies keep all samples across blocks.

The public profile retains the factory 115200-baud Core2-to-Module setting.
The reference installation uses a matched fixed 460800-baud link: both devices
start at that rate. Starting at one rate and switching after discovery was
unreliable and is not used. Two Core2 cold starts and a coordinated Module/Core2
restart passed with the matched initial rate. This is a reference configuration,
not an automatic migration for every Module firmware.

Large UART requests are paced in bounded bursts because this connection has no
hardware flow control. A SHA-256 envelope protects the exact serialized request,
and audio blocks have their own byte count/hash. The Core2 checks the exact
receiver acknowledgement and retries rejected blocks. Only protected partial
transfers are eligible for resume; the complete MP3 must still match the VM hash.

Changing the serial rate requires backing up the Module configuration and
matching the Core2 build setting before restarting both devices. Restore both
sides together when returning to an older image. USB/ADB and SuperDisplay are
separate connections and are not restarted by this voice path.

Full buffering improves continuity after speech starts; it is not a promise of
immediate first playback. A network, timing or transfer failure cannot turn a
partial download into a successfully completed reply.

## Recovery behavior

Temporary transfer or Module-service failures get up to two automatic reconnect
attempts. Recovery reopens the same immutable job and discovers the receiver's
protected prefix; it does not generate another answer or request paid synthesis.
The display shows **Recovering voice** while rejoining. Corrupt audio is never
admitted merely because a retry succeeded: complete byte counts and hashes still
have to match. Permanent synthesis/voice-identity failures stop immediately.

An exhausted failure stays visible as **Voice error – Restart**. Restart retries
the current job; a permanently failed Muse synthesis may require a new request.
Recovery cannot power an unplugged Module or repair a permanent upstream outage.
Stopping playback does not launch new speech automatically.

Background alignment and timing-cache persistence are optional. If either fails,
or background analysis adds more than 2 seconds after audio verification, the
device uses complete-audio word estimates and keeps valid narration. Estimated
highlighting is not exact forced alignment. Complete-audio integrity and playback
completion checks remain mandatory.
