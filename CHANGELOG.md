# Release history

## 0.3.0

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
