# Experimental native Muse synthesis helper

`tools/muse_vm_tts_job.py` runs one bounded synthesis job using Muse's existing
built-in speech facility. It adds no provider, credentials, network listener or
PC service. It is development infrastructure, **not a finished firmware feature**.
Keep Piper installed and selected until the device integration is validated.

Install the matching `module/runtime/muse_speech_text.py` beside the helper.
A device job has a fresh 24-character hexadecimal ID, and its UTF-8 `source.md`
lives under `~/muse-storyteller/replies/<job-id>/`. Run the helper with that ID,
`--voice` and `--speed slow|normal|fast`. The voice must be permitted by Muse;
the helper does not discover or override account eligibility.

The helper writes a growing `audio.mp3` and atomically replaces `meta.json`.
Only `state: complete` with matching byte count and SHA-256 proves a complete
transfer. `streaming` allows a consumer to begin buffering earlier. Partial
output after an error must never be treated as successful. Jobs time out after
180 seconds, accept at most 8 KiB of source, and produce at most 32 MiB of audio.
An exclusive claim prevents duplicate synthesis under the same job ID.

The isolated Linux tests cover first bytes arriving before completion, final
integrity, process failure after partial output, duplicate jobs, input limits,
and terminating only the helper's own child on timeout. Normal device playback,
word-following, cancellation and fallback require separate integration tests.

Speech generation uses the existing Muse service and remains subject to its
account allowance. The raw voice catalogue is not an authoritative list of
voices available to an account.
