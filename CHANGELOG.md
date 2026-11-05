# Release history

## 0.3.4

- Wait for the vendor's audio handlers during cold startup, then complete the real microphone/decoder warm-up before Ready. Runtime stop retries remain short and bounded.
- Retry one temporary Muse lookup failure during account/setup startup without discarding the first question.
- Allow the factory startup cue only once per boot, using Python's direct atomic directory claim; preserve its original audio asset.
- Add the SDK's direct message-addressed Muse audio route, avoiding per-answer file/helper orchestration. Keep Local, Helio and Aria selectable.
- Clearly label direct narration as using the Muse app's voice and pace; preserve the stored device speed for the other voices.
- Transfer protected native requests in an ASCII envelope so punctuation, Unicode and unmatched code braces cannot confuse the vendor's brace-counting UART extractor.
- Send audio blocks with StackFlow RAW binary framing to avoid base64 on the wire; verify both block metadata and audio before admitting bytes.
- Keep optional alignment on one low-priority CPU thread, preserving time for microphone, UART and controls.
- Use bounded 8 KiB direct audio blocks without a local base64 round trip; retry missing block acknowledgements promptly.
- Limit the extra wait for optional word timing to two seconds after complete audio is ready.
- Re-synchronize interrupted JSON/binary framing when Core2 starts, before opening its voice session.
- Buffer and verify each finished message before playback; continue receiving subsequent messages while cached audio plays.
- Retain cached playback across a connection drop and keep Pause/Resume, Stop, Restart, volume and reader controls in the existing playback path.

## 0.3.3

- Automatically reconnect and resume the same protected native audio job after a temporary Module/service or transfer failure, with at most two recovery attempts.
- Show recovery and buffering status; keep exhausted voice errors visible instead of returning a misleading Ready label.
- Stop retries on permanent synthesis/voice-identity errors and on cancellation; never silently switch the selected voice.
- Keep valid complete audio playing when background timing or optional timing/report cache writes fail.
- Bound the extra wait for background timing; fall back to complete-audio word estimates when necessary.
- Release incomplete decoder/analysis work cleanly on service shutdown, preserving protected MP3 prefixes.
- Preserve the original startup/worker fault when secondary cleanup also fails.

## 0.3.2

- Protect exact UART requests and audio blocks with checksums and exact receiver byte acknowledgements; retry rejected blocks.
- Resume protected interrupted native downloads, including partial caches from before a Module restart.
- Reuse verified complete caches without uploading the same MP3 again.
- Keep the selected Muse voice explicit: preparation failures show an error instead of silently speaking with Piper.
- Give slow Muse replies time to create their audio, with progress and elapsed time visible.
- Add an animated, skippable welcome screen throughout microphone/voice startup.
- Preserve the original Muse MP3 and offer smaller 16 kHz mono delivery through the existing VM helper.
- Overlap native synthesis/encoding, downloading and word-timing analysis, while still requiring complete verified audio before speaking.
- Validate a matched fixed 460800-baud reference link through cold starts; retain the factory 115200-baud public build default.
- Speed up volume processing while preserving every sample and the existing rounding behavior.
- Add regression checks for resume integrity, complete-cache reuse, volume math and compact delivery failure.

Two fresh normal story requests completed in the selected Muse voice. The optimized
reference run began speaking at 91.2 seconds, versus 148.4 seconds before overlapping
preparation. All 103.7 seconds of cached PCM and all 341 scheduled words were retained.
Cached Restart, Pause/Resume, Stop, Settings and reader resizing passed. Linux CI passed
all 39 tests. See [validation scope](docs/VALIDATION.md) for limits and measured results.

## 0.3.1

- Fully download, hash-check and decode native Muse audio before speaking.
- Play ordinary replies in one continuous native call, with all timing prepared first.
- Match the volume scaler's 5 MiB ceiling to the native playback block budget.
- Keep valid audio playing if alignment fails; retain complete-text timing as fallback.
- Persist completed-cache identity and timing so Restart survives Module service restarts.
- Reuse an in-flight download when Stop is followed by Restart during buffering.
- Preserve all PCM through bounded playback blocks for exceptionally long replies.
- Reject early hardware completion instead of treating a shortened clip as successful.
- Handle terminal native-file errors without leaving a request falsely pending.
- Retry interrupted file ranges and lost transfer acknowledgements without duplicating audio.

## 0.3.0

- Corrected published overlay checksums to use canonical LF source bytes on all platforms.
- Added optional native Muse narration and Sound voice-source/voice selectors.
- Verified Helio and Aria without changing the Muse account's global voice.
- Prefetch native audio while Muse prepares its reply; upload/decode off the UI thread.
- Use local forced alignment against the exact script for word-following.
- Keep live volume, pause/resume, stop and cached replay in the existing playback path.
- Verify script identity, selected voice/speed, byte counts and final audio hashes.
- Retain Piper when native preparation is unavailable before playback.
- Keep the confirmed first-press startup repair from 0.2.1.

## 0.2.1

- Start Module discovery during the splash instead of after the boot-animation delay.
- Keep loading visible until local speech is ready; Wi-Fi alone no longer means Ready.
- Distinguish Connecting Muse, Muse Offline and pairing needs from speech readiness.
- Warm the actual Whisper Base decoder with fixed synthetic speech before accepting input.
- Verify live microphone frames and withdraw readiness if capture or the decoder fails.
- Recover the recognizer on the background hello worker, preserving UI responsiveness.

## 0.2.0

- Added the on-device Settings → Voice Commands guide.
- Tapping a muted header speaker now unmutes in place; an enabled speaker opens Sound.
- Added exact “on mute” transcription aliases for Unmute, with standalone-command checks.
- Added an optional, skippable color-avatar splash with a persisted Startup setting.
- Added a separate visible edition version and release-version rules.
- Matched the transport row to the README illustration: equal widths, consistent gaps,
  dark purple Stop/Restart and a lighter Pause/Resume, with correct disabled states.

The initial public source preview preceded numbered edition releases. See
[validation scope](docs/VALIDATION.md) for the distinction between reference builds,
source checks and fresh-device provisioning.
