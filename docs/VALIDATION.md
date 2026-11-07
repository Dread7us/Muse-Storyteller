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

## Edition 0.3.4 startup and direct-voice checks

The cold-start failure was traced to the vendor audio router rejecting an
idempotent stop while its handlers were still registering. Startup retries that
specific temporary rejection within a bounded window; Ready still requires the
actual microphone capture and Whisper decoder warm-up. Repeated reference
Module cold boots reached readiness on the first service attempt, with roughly
44–49 seconds from reboot to warmed readiness. Runtime stop retries remain short.

The factory startup script now invokes a once-per-boot wrapper on the reference
installation. Its atomic lock was observed after a real cold boot; a repeated
invocation was suppressed and the factory audio asset was unchanged. A separate
Linux concurrency test also verifies twelve simultaneous calls admit one cue.

The direct Muse option uses the SDK's message-addressed speech endpoint through
the existing paired connection. Real requests returned HTTP 200 and native MP3
without asking an agent to create files or run a speech helper. Local Piper also
completed a normal recognized-text question/reply path. Those development requests
injected recognized text; they do not constitute a new physical microphone
accuracy benchmark.

A roughly 122-second native story verified all 973,056 MP3 bytes and played all
3,892,224 decoded PCM bytes in one continuous playback call. All 301 scheduled
words were retained, with the final highlight reaching the final script offset.
Its timing used complete-audio estimates; this is not a claim of exact alignment.

Protected 8 KiB direct transfers, removal of a Core2 base64 round trip, shorter
missing-acknowledgement retries and a two-second optional-timing budget reduced
clip preparation. Approximately 0.97 MB clips took 71.5 seconds with the earlier
4 KiB path and 44.0 seconds with the optimized 8 KiB path. The latter story began
speaking 89.1 seconds after request submission. These were separate generations
from the same 300-word prompt, not an identical-file controlled benchmark. Full
buffering and answer generation still impose substantial latency on long stories.

A higher 921600-baud trial lost transfer acknowledgements and was rejected. The
reference installation was returned to its proven matched 460800-baud setting;
the public profile retains the factory 115200-baud default. Both ends must match.
No reliability claim is made for the rejected faster configuration.

The actual vendor framing extractor recovered at 222 interrupted JSON/binary
positions using the startup resynchronization procedure. Portable checks cover
both 4 KiB and 8 KiB header/body corruption, optional timing delays and complete
playback retention. The 51-case suite passed locally with ten Windows platform
skips, and the corresponding Linux source workflow passed. The reference
firmware compiled fully and passed application-only write/digest and healthy-UI
boot checks. This remains a reference build, distinct from a full cold build of
the sanitized public installation profile.

After returning to the stable configuration, a fresh two-sentence direct Muse
reply began speaking in 9.165 seconds, verified and played its complete 52,800-byte
MP3, and reached the final reader word. Cached Restart began in 0.248 seconds
after completion and 0.245 seconds after Stop, resetting to the opening word.
Module-acknowledged Pause/Resume, Settings and reader resize passed during audio.
These are individual observations, not worst-case latency guarantees.

The final Core2 retained direct Muse voice at 50%, reported no speech error and
had advancing UI frames. All 310 original pre-repair archive messages remained
present. The tablet and Module stayed connected to the pre-existing ADB server,
and the SuperDisplay Windows service remained running. No ADB restart, tablet
routing change or NVS erase was performed. Private clips, device reports,
account state, archive backups and rejected candidate binaries remain unpublished.

A follow-up GitHub run exposed an intermittent failure in the shell-command-based
startup claim. Trace output showed multiple callers proceeding past the claim
in that runner environment. The wrapper now uses Python's direct atomic
`os.mkdir` call instead of depending on the platform `mkdir` utility's exit
behavior. The Linux test repeats twelve concurrent starts over twenty fresh
lock directories (240 calls), with diagnostic traces retained on failure. All
51 checks passed with this implementation. The reference Module received the
boot-only wrapper after a silent isolated first/repeat check; no service restart
or device reboot was requested for this follow-up. Its next cold boot was not
part of that follow-up verification.

## Edition 0.3.5 reply delivery

A harmless delayed-tool test reproduced the missing-result bug: the initial
acknowledgement arrived through live deltas, but the saved final message used a
lower chat-event sequence than the most recent live status. Comparing those
different sequence spaces incorrectly discarded the completed result.

The fix keeps persisted assistant messages out of the live-event replay filter.
Authoritative messages remain deduplicated by message ID and final text, and
changed final text remains eligible for playback. Running work is retained beyond
the old short acknowledgement timeout; task failure and the overall deadline
are reported explicitly.

Fifteen native tests exercise the production event dispatch, task lifecycle,
structured final text, revisions and playback completion. They passed on the
Module and in the 66-test Linux suite. In the physical reference check, a request
containing an intentional 35-second tool delay delivered both the acknowledgement
and final result. Both native clips downloaded successfully; playback reached the
last word of the final result and the turn completed with two messages. The final
result was also verified in the local archive.

Pause/Resume, Stop, Restart, Settings and reader resizing passed against the repaired
reply. Application-only write/digest verification, a healthy UI heartbeat, retained
voice/volume settings and read-only archive integrity checks passed. Original
archive entries were preserved. No ADB restart, SuperDisplay change, pairing reset,
Module service restart or NVS erase was used. Private task confirmations and
diagnostic captures are excluded from the public repository.

## Edition 0.3.6 idle-link recovery

Follow-up checks also encountered a stalled Module UART parser. Re-synchronizing
the Core2 restored communication, but the old runtime only performed that operation
at startup. This release adds bounded recovery following idle status/hello timeouts,
and paces small JSON messages as well as large ones. The exact trigger of every
observed serial stall has not been established; this is recovery protection, not a
claim that all transport failures are prevented.

Eleven additional native tests exercise the production timeout handler and UART
writer, including Recorder protection, paused playback, listening, attempt limits,
cooldown, unchanged bytes and failed writes. They passed on the Module and in the
77-test Linux suite. The reference application compiled, passed application-slot
write/digest verification and retained its settings.

A controlled physical test injected an incomplete frame with no executable action
or work target. The runtime detected the timeout and restored Ready automatically
in 29.8 seconds, including the recovery cooldown. UI frame counts confirmed that
Core2 did not reboot. Recorder was idle throughout; Module services and SuperDisplay
were not restarted. This fault injection was performed through a guarded serial
maintenance fixture, which refuses recording, playback and active native transfers.

An existing public native-voice fixture then played its complete 57,600-byte clip,
reached its final reader word and returned to Ready. Pause/Resume, Stop, Restart,
Settings and reader resizing passed on that saved clip. Cached Restart began in
about 0.24 seconds in two observations. These are fixture observations, not network
latency guarantees. No new Muse question or private conversation was submitted for
this final fixture check.

## Edition 0.3.7 transfer and replay recovery

The reference stack played a previously failing 2,980-byte story through its last
word. Its complete 1,602,624-byte MP3 decoded to roughly 200 seconds of speech;
final hashes and playback completion were checked. The initial transfer still
took minutes, so this was a completeness/recovery check, not a latency success.

The native endpoint returned HTTP 200 with the whole body when asked for a later
byte range. Accordingly, direct recovery re-reads and hashes the retained prefix
locally, then uploads only the missing suffix to the Module. This is not a claim
of HTTP range support. A physical partial-cache check uploaded only the remaining
suffix and reached verified playback. Forced failed-job Restart checks retained
an explicit failure instead of silently changing the selected voice.

## Edition 0.3.8 Aria preparation checks

The reference application compiled and passed application-slot digest and boot
checks, retaining Aria, 60% volume and pairing. The network returned online with
about 19–20 KiB of DMA-capable memory available. An experimental internal-RAM
interrupt/stack configuration failed validation and was discarded; the installed
build preserves the working allocation policy and network reserve.

A fixed, already generated 58-second Aria clip was transferred into a cold Module
cache. First speech improved from 40.2 seconds on the initial batching candidate
to 24.8 seconds with prompt reply handling and the safer receive configuration.
The latter transferred all 232,992 MP3 bytes with no recovery retries. Its
preparation profile recorded 10 file requests, 157 checked packets, 12.8 seconds
in UART writes and 2.9 seconds waiting for acknowledgements. These profile
intervals are diagnostic observations, not throughput guarantees.

Both copies had identical MP3/source hashes. All 1,863,936 decoded PCM bytes played
in one block, with true local alignment retained for 161 words; the final reader
offset was 899, the end of the script. This controlled replay excludes a new
Muse response and new voice generation. It must not be compared directly to a
fresh-question response time.

A separate fresh 201-byte reply began speaking in 17.0 seconds and finished through
its final word. An earlier observation of the same requested passage was 21.9
seconds. Muse's text arrived sooner in the later run, so the difference cannot be
attributed wholly to this firmware. Server generation, reply length and network
conditions still affect first speech; complete buffering remains mandatory.

A fresh longer public story began speaking in 35.8 seconds and finished through
byte 815, its final word, with no voice error or transfer recovery.
That fresh story used complete-audio timing estimates rather than true alignment;
the fixed reference clip above retained true alignment.
Pause/Resume, Stop, Sound access and reader resizing passed during cached playback. Restart
from stopped began at the opening words in 0.67 seconds. Local Piper then spoke
and completed a local setting confirmation; Aria and 60% volume were restored,
and the reference device returned to Ready with advancing UI frames.

The Linux source/runtime suite passed all 99 tests, including capacity-limited
file callbacks, stale generations, acknowledgement fences, subprocess failure and
active/idle reply polling. The Windows run passed 60 tests with 19 platform skips.
The physical firmware build refers to the reference installation; the sanitized
public weather/profile differences described above remain applicable.

## Edition 0.3.9 palette and sustained-use headroom

A later repeated-use check exposed a remaining failure after successful initial
playback: DMA-capable free memory had fallen to about 15 KiB, below the existing
16 KiB send reserve. The next online reply could receive text but stall while
preparing its voice. Initial cold-boot success alone did not cover that sequence.

The update moves 5,485 bytes of task-only transcript and archive/Recorder/usage
view state from internal BSS into external RAM. The linker's allocation map
confirmed the placement. UART buffers, task stacks and the network safety floor
retain their working configuration. This is additional headroom, not removal
of the safety check or a claim of unlimited memory.

The interface's palette saturation is increased by 12.5%, with hue and peak RGB
brightness retained. Grayscale remains unchanged. Palette values are precomputed;
the renderer does no extra per-pixel conversion. Actual Sound and Ready captures
were inspected for legible text, controls and the battery indicator.

The reference application compiled, passed application-slot digest and boot
checks, and retained Aria, 60% volume and pairing. After using Sound and the
voice-selection screen, a fresh reply started in 21.2 seconds, played completely
and reached its final word. Available DMA memory during preparation was about
24–25 KiB. Muse's text generation took longer in this run, illustrating the
server-dependent portion of the initial wait.

Pause/Resume, Stop, Sound access and reader resizing then passed again. Cached
Restart began at the opening words in 0.69 seconds; local Piper completed its
confirmation and Aria/60% were restored. After an idle interval, another fresh
reply started in 23.8 seconds and finished through its final word without a
transfer retry. DMA headroom remained around 20 KiB, and the raw/control links
stayed up through the sequence that had previously stalled.

## Editions 0.4.0–0.4.1 verified sentence narration

The reported repeated sentence was present in generated audio, rather than being
only a display problem. The previous long-clip timing fallback could also drift
independently of speech. The new Helio/Aria route prepares bounded, sentence-aware
sections with two generation workers. Tiny English Whisper checks actual speech
against the script and rejects unexpected repetition. A separate quantized English
Wav2Vec2 acoustic model assigns word boundaries using a bounded CTC path; missing,
zero-length or low-confidence anchors fail explicitly. Accepted sections carry
source, MP3 and decoded-audio checksums. Playback uses complete sentence buffers.

The reference v0.4.1 application compiled, passed application-slot write/digest and
boot checks, and retained Aria, 60% volume and pairing. A complete 144.252-second
public story played all 22 sections and 460 word anchors, reaching its final text
byte. Another 173.808-second story played all 28 sections and reached its final
word; all of those saved sections also passed the final acoustic analyzer.
Independent whole-file recognition checks found no unexpected repeated phrases
in either generated story. These are controlled observations, not an accuracy
guarantee for all future generated speech.

With the screen awake, Pause/Resume, live volume adjustment, Sound access, reader
resizing, Stop and Restart passed. Cached Restart began at the opening words in
0.60 seconds. A separate partial-transfer Restart check took 1.04 seconds and
continued the producer rather than treating a partial cache as complete. Pausing
and live volume changes now bookmark the actual audio clock, and delayed display
events skip obsolete words rather than rapidly emitting a catch-up sequence.
Local Piper also spoke and completed; Aria and 60% volume were restored.

Startup remains a material limit. Replaying an already generated long story into
a cold device cache began in 6.27 seconds of buffering, which excludes Muse text
and speech generation. A genuinely fresh public reply took 50.02 seconds to first
speech: its text completed around 36 seconds. It then played all eight sections,
176 word anchors and 55.548 seconds of audio through its final word, completing
about 55.83 seconds after first speech. The faster documented native file-write
route returned HTTP 403 on this installation. That experiment was removed; the
installed path keeps the supported Muse-generated-file workflow. Neither a fresh
six-second start nor elimination of the server delay has been demonstrated.

Final read-only checks matched the installed Module sources, confirmed Recorder
and playback idle, preserved all 382 original archive messages and passed SQLite
integrity. The existing ADB server, tablet and SuperDisplay remained connected;
no ADB restart, tablet routing change, pairing reset or NVS erase was used.

The Linux source/runtime suite passed all 129 tests, including real bounded CTC
paths, repeated-letter handling, unexpected audio repetition, audio-clock replay,
section ordering, exact decoded length, hash validation, buffering and cancellation.
The Windows run with the isolated analysis dependencies passed 88 tests with 19
platform skips. The initial branch workflow failed because a new test called
`.case` instead of `.run_case`; correcting that test restored successful checks.
Private recordings, transcripts, logs, account state and device backups remain local.

## Editions 0.5.0–0.5.1 loading feedback

The reference 0.5.0 application compiled, passed application-only flash/digest and
boot checks, and retained Aria, 60% volume and pairing. Its real loading screen was
captured and visually inspected. The reader's new bar started at 100% work remaining,
decreased without refilling, reached zero when speech started, then disappeared.
The complete public test reply finished. Pause/Resume, Sound access, reader resizing,
Stop and cached Restart passed; Restart began in 0.73 seconds.

That fresh reply began in 30.45 seconds, including a diagnostic screen capture
during preparation. Server response time, test length and capture overhead differ
from earlier runs, so this is not a controlled measurement of the speedup. The
model-loading overlap is separately covered by a test that blocks model loading
until synthesis starts and checks that no playable section bypasses verification.
Actual future startup times remain variable.

The countdown model is tested against word-count scaling, invalid saved values,
bounded learning, revised estimates, overdue responses and new sessions. Zero is
reserved for readiness, rather than the estimate's expiry. The Linux suite passed
all 137 tests; Windows passed 90 with 20 platform skips. The build also caught a
formatting warning in the first candidate, corrected before installation.

A subsequent first-page check found that the reader could retain its hint until
a Settings visit because the tile view's initial active page was not explicit.
Edition 0.5.1 selects that page during construction, before navigation callbacks.
The corrected application passed flash readback and boot checks. Its first cached
reply rendered 910 characters and advanced its word highlight before any navigation;
Pause/Resume and Stop passed again. Aria, 60% volume and the native network connection
were retained. Final read-only checks matched every deployed Module runtime source,
preserved all 382 original archived messages and passed database integrity. Recorder
and playback were idle, both USB devices were connected and SuperDisplay was running.

## Editions 0.5.2–0.5.3 Resume repair

A deterministic playback test reproduced a missing unpaused acknowledgment when
Pause landed at the exact end of a PCM block. That path now acknowledges Resume
before returning to the sentence player. The Core2 acknowledgment handler also
retains Buffering when the next sentence is not yet available.

The first physical long-pause test exposed an additional failure: while audio was
paused, checked upload replies stopped arriving and Resume remained requested but
unacknowledged. The existing ASCII framing repair was prohibited during playback,
including acknowledged pauses. Edition 0.5.3 permits that bounded delimiter repair
while paused and adds at most two repair attempts for a stuck Resume transition.
It preserves the session, audio position and checked cache. Recorder activity,
listening/cancellation states and queued user commands remain protected.

Both application-only writes passed readback and boot checks. In the final cold-cache
test, speech began while later sections were still transferring. Audio was paused
for 65.57 seconds; the native speaker reported stopped and the highlight stayed at
the same word. All 22 sections and 577,008 MP3 bytes finished transferring during
that pause. Resume cleared the pending state in 1.56 seconds, the native speaker
reported running and highlighting advanced from the paused position. Another
Pause/Resume, Sound access, temporary volume adjustment, Stop and Restart passed.
The device returned to Ready with Aria and 60% volume.

The passing run retried rejected damaged blocks but did not need a delimiter
repair. Attempt limits, cooldown, acknowledged-pause admission and Recorder/queued
command protection are covered by the production-code host tests; this is not a
claim that every source of physical serial corruption has been eliminated.
The Linux suite passed all 150 tests. The Windows run passed 91 with 21 platform
skips. The playback boundary regression also passed on the actual Module.
Final source checks matched the deployed Module files. All 393 messages present
before this repair, including the earlier 382-message baseline, were preserved;
database integrity passed. The devices returned to idle and SuperDisplay remained
running on its existing USB server. No pairing/Wi-Fi erase or tablet routing change
was performed. Private test reports and cache backups remain outside the repository.
