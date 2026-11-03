# Validation scope

Development reference: one physical Core2 v1.1 + Module LLM stack, tested during
October–November 2026. These statements describe observed development checks, not
certification of every installation or a promise about all future upstream versions.

## What was exercised

- English Whisper Base through hold-to-talk and Recorder, including final-word draining.
- Local Piper narration, sentence-aware buffering, speed choices and punctuation handling.
- Pause/Resume; Stop from paused; Restart from paused and after Stop; native word-position reset.
- The full reader text, word highlighting, three reader sizes, and scrolling.
- Actual volume changes, out-of-range rejection, brightness, speed and mute/unmute.
- Muted-header screenshot checks: MUTED and its icon appear, then the retained volume returns on unmute.
- Settings restoration, archive integrity and protected Module-source checks after tests.
- The usage popup’s on-demand website-to-device sync in the maintainer’s configured PC setup.
- Owner feedback on weather, library save/open/resume, Recorder capture and transcription.

The control tests used a synthetic local story and transient reader entries, without
posting a new Muse question or adding test messages to the saved archive. Public tests
also use synthetic fixtures. Personal clips, transcripts and device reports are not published.

## Edition 0.2.0 reference checks

The application-only image was written with digest verification, then observed through
one healthy Core2 boot, a ready Module bridge and advancing UI heartbeats. The saved
30% volume was retained. No NVS erase was performed.

Device callback checks confirmed that a muted-header tap unmutes without opening
Settings, while an enabled-header tap opens Sound. The exact “on mute” alias was
handled locally. The Voice Commands guide and compact Startup page were captured
from the device. Both splash preferences survived separate guarded reboots; the final
color-avatar preview and visible version were inspected on the corrected image.

Pause and Resume were acknowledged by the playback owner; Stop returned it to Ready,
and Restart began local playback again. The final transport screenshot was inspected
for equal spacing, purple colors and the paused Resume label. Test entries were cleared
and the previous mute state was restored.

The public source passed publication checks, portable tests and ESP32 compiler checks
for the changed UI/settings sources plus the new startup and encoded-avatar sources.
Native C command-boundary cases passed on the Module as an additional check.

## Recognition comparison

The same roughly thirty-second English clip was processed by the original recognizer,
Whisper Base and Whisper Small. Under the comparison’s normalization of equivalent
numbers/currency and acronym spelling, the original had 11 errors in 47 reference words;
Base and Small each had zero. Base decoding was roughly 3.4 seconds versus Small’s
9.3 seconds on that clip. Base was selected for the better speed/accuracy tradeoff there.

This is **one controlled comparison**, not a broad word-error-rate benchmark. Accent,
distance, noise, speech length and hardware load can change results. Model loading adds
startup time; keeping Base loaded avoids repeating that load for every recording.

## Public source checks

The isolated public SDK preparation passed its pinned-source checks. ESP32 compiler
checks passed for the sanitized UI, input, settings, weather and LVGL renderer sources.
The GitHub portable suite also passed, including native command-parser checks. A full
cold build/link of the sanitized profile was not completed for this source preview;
the complete built/flashed application checks above refer to the reference installation.

## Current boundaries

- The public weather configuration and generic self-test greeting differ from the private
  reference build; no maintainer location is embedded.
- Factory provisioning, automatic installation/rollback on every vendor firmware, and
  other M5 hosts are not validated by this release.
- Replay uses received text retained in RAM. It does not regenerate a missing answer,
  fetch an old web chat, guarantee a response longer than the existing bounded buffers,
  or retain the replay cache across reboot/new turns.
- Local library recall and bounded recent context are not unlimited or perfect memory.
- AI transcript review is a suggestion. Raw transcription remains available and the
  reviewer can reject or defer uncertain edits.
- Usage checks are PC-assisted; the local estimate is not authoritative billing data.
- There is no auto-rotation feature or claim of a sustained 60 fps on this hardware.

The CI checks source integrity, explicit-command boundaries, portable runtime behavior
and publication hygiene. It does not record audio, run NPU models, or test a real board.
