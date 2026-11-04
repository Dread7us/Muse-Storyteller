"""Bounded one-shot Muse VM synthesis job; no listener, credentials or new provider.

The native device reads source.md, meta.json and growing audio.mp3 using its
existing authenticated VM file transport. This helper does not implement that
device transport. Install beside the reviewed muse_speech_text.py normalizer.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import selectors
import signal
import subprocess
import time

MAX_TEXT = 8192
MAX_AUDIO = 32 * 1024 * 1024
SPEEDS = {"slow": 85, "normal": 100, "fast": 115}


def atomic_json(path, value):
    temporary = path.with_suffix(".pending")
    with temporary.open("w", encoding="utf-8") as stream:
        json.dump(value, stream, ensure_ascii=False, separators=(",", ":"))
        stream.flush()
        os.fsync(stream.fileno())
    temporary.replace(path)


def read_source(job):
    source = job / "source.md"
    if source.is_symlink():
        raise ValueError("Source must be a regular job file")
    with source.open("rb") as stream:
        raw = stream.read(MAX_TEXT + 1)
    if not raw or len(raw) > MAX_TEXT or b"\0" in raw:
        raise ValueError("Source must contain 1 to 8192 UTF-8 bytes")
    text = raw.decode("utf-8")
    if not text.strip():
        raise ValueError("Source is empty")
    return raw, text


def terminate(process):
    if process.poll() is not None:
        return
    # start_new_session gives this job exclusive ownership of this process group.
    os.killpg(process.pid, signal.SIGTERM)
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.wait(timeout=3)


def synthesize(job, voice, speed="normal", executable="/opt/hatch/bin/tts",
               deadline_seconds=180, normalize=None):
    if not re.fullmatch(r"avocado_v2:[A-Za-z0-9_.-]{1,80}", voice):
        raise ValueError("Invalid voice identifier")
    if speed not in SPEEDS:
        raise ValueError("Invalid speech speed")
    job = Path(job)
    if job.is_symlink() or not job.is_dir():
        raise ValueError("Invalid job directory")
    if normalize is None:
        from muse_speech_text import SpeechText
        normalize = lambda text: SpeechText(text).text
    raw, text = read_source(job)
    spoken = normalize(text)
    if not spoken.strip() or len(spoken.encode("utf-8")) > 65536:
        raise ValueError("Normalized speech exceeds its bound")
    # Exclusive creation prevents concurrent duplicate synthesis or silent replay
    # over an existing growing file. Failed jobs need a fresh device job nonce.
    claim = os.open(job / "claimed", os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    os.close(claim)
    meta = {"version": 1, "state": "preparing", "voice": voice, "speed": speed,
            "source_bytes": len(raw), "source_sha256": hashlib.sha256(raw).hexdigest(),
            "audio_bytes": 0}
    started = time.monotonic()
    process = None
    digest = hashlib.sha256()
    total = 0
    try:
        atomic_json(job / "meta.json", meta)
        with (job / "audio.mp3").open("xb") as audio:
            process = subprocess.Popen(
                [str(executable), "speak", "--text", spoken, "--voice", voice,
                 "--language", "en", "--speed", str(SPEEDS[speed]), "--stream"],
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL, start_new_session=True)
            with selectors.DefaultSelector() as selector:
                selector.register(process.stdout, selectors.EVENT_READ)
                last_update = started
                while True:
                    if time.monotonic() - started > deadline_seconds:
                        raise TimeoutError("Muse synthesis timed out")
                    if not selector.select(timeout=.1):
                        if process.poll() is not None:
                            # An exited process can still have unread stdout;
                            # EOF must be consumed rather than assuming success.
                            continue
                        continue
                    block = os.read(process.stdout.fileno(), 8192)
                    if not block:
                        break
                    if total + len(block) > MAX_AUDIO:
                        raise ValueError("Muse audio exceeds cache bound")
                    audio.write(block)
                    audio.flush()
                    digest.update(block)
                    total += len(block)
                    now = time.monotonic()
                    if "first_byte_ms" not in meta:
                        meta["first_byte_ms"] = round((now-started)*1000)
                    if meta["state"] == "preparing" or now-last_update >= .5:
                        meta.update(state="streaming", audio_bytes=total)
                        atomic_json(job / "meta.json", meta)
                        last_update = now
            remaining = max(.1, deadline_seconds-(time.monotonic()-started))
            if process.wait(timeout=remaining) != 0 or total < 128:
                raise RuntimeError("Muse synthesis did not produce usable audio")
            os.fsync(audio.fileno())
        meta.update(state="complete", audio_bytes=total,
                    audio_sha256=digest.hexdigest(),
                    elapsed_ms=round((time.monotonic()-started)*1000))
        atomic_json(job / "meta.json", meta)
        return meta
    except BaseException as error:
        if process is not None:
            terminate(process)
        # Never publish exception details, stderr, prompts or credentials.
        meta.update(state="error", audio_bytes=total,
                    error="timeout" if isinstance(error, (TimeoutError, subprocess.TimeoutExpired)) else "synthesis_failed")
        atomic_json(job / "meta.json", meta)
        raise
    finally:
        if process is not None and process.stdout is not None:
            process.stdout.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("job", help="24-character device-generated hexadecimal job ID")
    parser.add_argument("--voice", default="avocado_v2:MAI_03")
    parser.add_argument("--speed", choices=SPEEDS, default="normal")
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9a-f]{24}", args.job):
        parser.error("Invalid device job ID")
    root = (Path.home() / "muse-storyteller" / "replies").resolve()
    folder = root / args.job
    if folder.resolve().parent != root:
        parser.error("Job escapes the Muse Storyteller reply directory")
    synthesize(folder, args.voice, args.speed)


if __name__ == "__main__":
    main()
