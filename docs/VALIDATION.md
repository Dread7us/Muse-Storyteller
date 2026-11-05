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
- Replay controls use the current reply text retained in Core2 RAM. Complete native
  audio caches and timing can survive a Module service restart, while the Core2 still
  holds that reply. This does not restore playback controls after a Core2 reboot,
  regenerate a missing answer, fetch an old web chat, or remove response-size bounds.
- Local library recall and bounded recent context are not unlimited or perfect memory.
- AI transcript review is a suggestion. Raw transcription remains available and the
  reviewer can reject or defer uncertain edits.
- Usage checks are PC-assisted; the local estimate is not authoritative billing data.
- There is no auto-rotation feature or claim of a sustained 60 fps on this hardware.

The CI checks source integrity, explicit-command boundaries, portable runtime behavior
and publication hygiene. It does not record audio, run NPU models, or test a real board.

## Edition 0.2.1 startup checks

The firmware compiled and the application-only update passed digest and healthy-boot
checks, with advancing UI heartbeats and the saved 30% volume retained. The installed
Module passed actual Whisper synthetic-speech warm-up and readiness inspection.
A guarded voice-service cold restart changed the real Core2 header to Loading Voice
and back to Ready after recovery. Stale-capture and failed-health rejection tests passed.
The owner confirmed the first press goes straight to Listening and captures the beginning.


## Edition 0.3.0 native voice checks

The reference application compiled, was written only to its application slot,
and passed digest/healthy-boot checks, retaining pairing and 50% volume. The
Module bridge recovered after guarded updates with exact source backups.

A fixed native clip passed pause/resume, Settings, stop and cached restart, with
advancing reader frames and word offsets through the complete script. Fresh
native replies passed for Aria and Helio. The final Helio test had no bridge error,
word offsets reached the end of its 201-byte script, and reader frames continued.
The UI did not block on download or local forced alignment.

A short isolated VM job measured first audio bytes at 948 ms and total synthesis
at 3,051 ms. That excludes Muse's response generation and device buffering.
The final fresh reference test began speaking at about 18.0 seconds from its
recognized-text request, versus 24.7 seconds before prefetch. Its final text
arrived at about 13.1 seconds. These are individual observations, not a service
latency guarantee. Longer response generation can still increase the initial wait.

Four isolated Linux helper tests passed. Real Module cache tests verified block
ordering rejection, final hashes, MP3 decoding and original-text timing offsets.
The portable suite passed with platform-specific tests skipped on Windows;
the four helper tests were run separately on the Linux Module. The sanitized
public profile retains its separate weather configuration; the hardware build
above refers to the reference installation.

## Edition 0.3.1 playback checks

The reference Core2 application compiled, passed application-slot digest and
healthy-boot checks, and retained its pairing and 50% volume. The factory
115200-baud Module link was restored after higher-rate reconnect checks failed.

The previously shortened native reply was transferred completely: 1,173,696 MP3
bytes, with matching SHA-256 and a decoded duration of 146.712 seconds. Actual
Core2 cached playback then played all 4,694,784 PCM bytes in one native call.
All 463 scheduled words were retained; the reader reached byte 2,232, the end of
the original script. No playback error was reported. The speaking interval was
152.63 seconds including local setup/volume scaling. Two cached restarts entered
Speaking in 3.059 and 2.954 seconds; these are observations, not latency promises.

Additional reference checks exercised Pause/Resume, Settings, Stop and Restart,
including a muted 32-second real-audio replay after a fresh cache-store instance.
Real Module decoding tests checked ordered transfers, duplicate chunk/end
acknowledgements, hashes, aborted-transfer recovery and original word offsets.
Portable tests cover the actual volume scaler with a 147-second reply and verify
that a larger reply chooses an earlier sentence boundary without omitting PCM.

The final Windows source suite passed 24 tests with five platform skips; Linux
CI runs all 29. Personal audio, scripts and device reports are excluded from the
public repository. Full initial buffering can still take minutes for long clips
on the factory serial link; cached replay avoids that transfer. Timing falls
back to complete-audio estimates if local alignment fails.

## Edition 0.3.2 recovery checks

Investigation reproduced a shortened audio block that still formed valid JSON/base64.
The receiver had fewer bytes than the sender counted. The previous final hash check
prevented corrupt playback, but the preparation failure selected Piper. Requests now
have an exact-payload SHA-256 envelope; blocks have their own length/hash, and the
sender requires the exact receiver byte acknowledgement before advancing. Legacy
unprotected partial caches are preserved but are not resumed.

The reference application compiled and passed application-slot digest and healthy-boot
checks, retaining the selected Muse/Helio voice and 50% volume. Both sides start at a
matched fixed 460800 baud. Two Core2 cold starts reached voice-ready in 2.386 and
2.435 seconds with the Module already warm. A coordinated Module/Core2 restart
reached ready within the 50.6-second observation interval, including screenshot
capture. Two actual boot frames showed different animated dot/rail positions.
The public profile keeps its factory 115200-baud default; serial configuration
must be migrated and rolled back on both sides together.

Two new normal recognized-text story requests—not just fixture/cache replays—
transferred and played completely in native Muse voice. With preparation stages
performed serially, the first started at 148.433 seconds. After overlapping compact
encoding, transfer and decoded-window analysis, the second started at 91.175 seconds.
Its first received audio was at 27.571 seconds and complete transfer at 83.034 seconds.
These are separate roughly 340-word stories, not a controlled service benchmark.
They do not promise the same wait for other lengths or server conditions.

The optimized run verified all 414,720 MP3 bytes against SHA-256, decoded 3,317,760
PCM bytes (103.68 seconds), and played every sample in one continuous native call.
The reader reached the final offset of all 341 scheduled words. That run used the
complete-audio timing fallback because forced alignment did not yield anchors for
every word; exact per-word timing is not claimed. Timing failure did not shorten
speech or freeze the reader.

Cached restart entered Speaking in 0.479 seconds from completion and 0.472 seconds
after Stop. Actual Module pause acknowledgement, Resume, Settings and reader resize
callbacks passed during playback. All 279 pre-update archived messages remained
present. The final Core2 had advancing UI frames, no speech error, Helio selected
at 50%, and the Module/tablet remained connected to the existing SuperDisplay server
with its Windows service running. No ADB server restart or NVS erase was performed.

Linux CI passed all 39 tests, including compact output becoming downloadable before
synthesis finishes, stalled-encoder timeout, exact payload/block corruption rejection,
and timing analysis before completion without publishing unverified results.
Reference Module tests also exercised actual MP3 resume/decoding and volume math.

## Edition 0.3.3 recovery checks

A failed reference job had stopped at 229,376 received bytes while its Muse VM
already held the complete, hash-verified 662,400-byte MP3. The Module had since
restarted; the evidence did not establish exactly what caused the original
interruption. The prior firmware could retain the text but did not automatically
reopen and resume a failed transfer after reconnecting.

The recovery candidate compiled and passed application-only write/digest and
healthy-boot checks, retaining 50% volume and the matched serial link. A fresh
normal 102-word request was deliberately interrupted after 16,384 received bytes
by restarting only the idle Module speech service. Core2 showed recovery,
rejoined the service and resumed the same job without another synthesis request.
It verified all 140,256 MP3 bytes and played all 1,122,048 PCM bytes (35.064 seconds)
in one call, with highlighting reaching the final byte of all 102 scheduled words.
One reconnect attempt was needed. The temporary not-ready/busy responses during
the deliberate restart cleared before playback; they were not playback errors.

A permanent metadata-error fixture failed without an automatic retry loop and
without starting Piper. Its local Piper-result fingerprint stayed unchanged.
An actual screen capture showed the persistent **Voice error – Restart** header
after the transient error caption, rather than a misleading Ready label.

The original failed reply then resumed from its protected 229,376-byte prefix.
It verified all 662,400 MP3 bytes and played every one of its 5,299,200 PCM bytes
across two bounded calls (165.6 seconds of audio). All 462 scheduled words reached
the final original-script offset. No new answer or synthesis was requested for
that recovery. On the final firmware, cached Restart entered Speaking in 0.481
seconds from completion and 0.450 seconds after Stop. Pause/Resume, Settings and
reader resizing passed during playback; the final view was restored to Max.
All 293 messages in the pre-repair archive backup remained present, Helio stayed
selected at 50%, UI frames advanced, and the existing Module/tablet connections
and SuperDisplay service remained healthy.

Public regression checks cover failed background analysis, optional timing/report
write failures, shutdown with an incomplete protected prefix, and preservation
of the original initialization error when cleanup also fails. These checks do
not claim to repair absent Module power, a permanent provider outage, or every
possible hardware fault. Exact alignment remains optional; estimates preserve
continuous complete audio when analysis is unavailable.

The final Linux source suite passed all 43 tests. Private job files, screenshots,
device reports and archive backups are not part of the public repository.
