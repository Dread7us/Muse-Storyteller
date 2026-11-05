"""Local StackFlow-v1.3 adapter. No cloud services or model downloads.

All methods are used by one worker thread. Vendor configuration files are
read, never edited. Only adapter-owned task IDs are released during normal
operation. The separate, explicit takeover command handles the verified
previous application task pair after the Core2 program has been paused.
"""
import array
import base64
import json
import os
from pathlib import Path
import re
import sys
import threading
import time
import uuid

import muse_zmq as zmq

ASR_MODEL = "sherpa-ncnn-streaming-zipformer-20M-2023-02-17"
TTS_MODEL = "melotts_zh-cn"
PCM_RATE = 16000
MAX_PCM = 5 * 1024 * 1024  # Leave 1 MiB below the native IPC message bound.
MAX_TRANSCRIPT = 2048
TASK_RE = re.compile(r"^(asr|melotts|kws|llm|muse_llm)\.[0-9]{1,10}$")
BREAKS = re.compile(r"[，、,。.!！?？;；]+")


class BridgeError(RuntimeError):
    def __init__(self, code, message):
        super().__init__(message)
        self.code, self.message = code, message[:180]


class Cancelled(BridgeError):
    def __init__(self):
        super().__init__(-20, "Operation cancelled")


def check_cancel(cancel):
    if cancel is not None and cancel.is_set():
        raise Cancelled()


def encode(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def unpack_pair(data):
    if not data or data[0] == 0 or data[0] > 127 or len(data) < data[0] + 1:
        raise BridgeError(-2, "Invalid StackFlow RPC envelope")
    return data[1:1 + data[0]], data[1 + data[0]:]


def pack_pair(first, second):
    if not 0 < len(first) < 128:
        raise BridgeError(-2, "Invalid StackFlow callback length")
    return bytes((len(first),)) + first + second


def split_phrases(text, limit=192):
    """One native MeloTTS callback per input; never discard meaningful text."""
    result = []
    for sentence in BREAKS.split(text):
        sentence = " ".join(sentence.split()).strip()
        while sentence:
            chunk, size = "", 0
            for char in sentence:
                n = len(char.encode("utf-8"))
                if size + n > limit - 1:
                    break
                chunk, size = chunk + char, size + n
            if not chunk:
                raise BridgeError(-2, "Invalid speech text")
            if len(chunk) < len(sentence):
                space = chunk.rfind(" ")
                if space > 0:
                    chunk = chunk[:space]
            result.append(chunk + ".")
            sentence = sentence[len(chunk):].lstrip()
    return result


def _scale_pcm_fast(data,volume):
    import numpy as np
    values=np.frombuffer(data,dtype='<i2').astype(np.int32)
    values*=volume
    # Integer division must truncate toward zero, matching the original scaler.
    np.add(values,99,out=values,where=values<0)
    values//=100
    return values.astype('<i2').tobytes()


def scale_pcm(data, volume):
    if type(volume) is not int or not 0 <= volume <= 100:
        raise BridgeError(-2, "Volume must be an integer from 0 to 100")
    if not data or len(data) % 2 or len(data) > MAX_PCM:
        raise BridgeError(-3, "Invalid synthesized PCM size")
    if volume == 100:
        return data
    if volume == 0:
        return bytes(len(data))
    if len(data)>=65536:
        try:return _scale_pcm_fast(data,volume)
        except (ImportError,MemoryError):pass
    samples = array.array("h")
    samples.frombytes(data)
    if sys.byteorder != "little":
        samples.byteswap()
    for i, value in enumerate(samples):
        samples[i] = int(value * volume / 100)
    if sys.byteorder != "little":
        samples.byteswap()
    return samples.tobytes()


def atomic_json(path, value):
    path = Path(path)
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    temp = path.with_name(path.name + ".new")
    with open(temp, "w", encoding="utf-8") as stream:
        os.chmod(temp, 0o600)
        json.dump(value, stream, ensure_ascii=False, indent=2)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temp, path)


class Transcript:
    def __init__(self):
        self.final_parts, self.partial = [], ""

    def accept(self, data):
        if not isinstance(data, dict) or type(data.get("finish")) is not bool or not isinstance(data.get("delta"), str):
            raise BridgeError(-3, "Malformed native ASR response")
        text = data["delta"].strip()
        if data["finish"]:
            if text:
                self.final_parts.append(text)
            self.partial = ""
        else:
            self.partial = text
        if len((" ".join(self.final_parts) + self.partial).encode("utf-8")) > MAX_TRANSCRIPT:
            raise BridgeError(-3, "Recording transcript exceeds 2048 UTF-8 bytes")

    def text(self):
        return " ".join(self.final_parts).strip()


class Backend:
    def __init__(self, state_dir="/var/lib/muse-core2-bridge", ipc_dir="/tmp/musevoice"):
        self.context = zmq.Context()
        self.state_dir, self.ipc_dir = Path(state_dir), Path(ipc_dir)
        self.ipc_dir.mkdir(mode=0o700, parents=True, exist_ok=True)
        self.route = "ipc://" + str(self.ipc_dir / ("backend-" + uuid.uuid4().hex[:12] + ".sock"))
        self.responses = self.context.socket(zmq.PULL, timeout_ms=100, max_bytes=6 * 1024 * 1024)
        self.responses.bind(self.route)
        self.feed = None
        self.feed_id = None
        self.tts_id, self.asr_id, self.asr_request = None, None, None
        self.boot_id = Path("/proc/sys/kernel/random/boot_id").read_text().strip()
        self.owner_path = self.state_dir / "owned-tasks.json"
        self.audio_original = None
        self.audio_configured = False
        self.audio_owned = False
        self.active_play = False
        self.sequence = 0
        self.closed = False
        self.mic_level = 100
        self.word_callback = None
        self.word_timing = None
        self.word_schedule = []
        self.mic = None
        self.mic_proxy_owned = False
        self.starting = True

    def request_id(self):
        self.sequence += 1
        return "mvb-%s-%d" % (os.getpid(), self.sequence)

    def rpc(self, unit, action, data=b"None", timeout_ms=3000):
        if unit not in ("sys", "asr", "melotts", "audio"):
            raise BridgeError(-2, "Unknown local service")
        if isinstance(data, str):
            data = data.encode("utf-8")
        try:
            with self.context.socket(zmq.REQ, timeout_ms=timeout_ms, max_bytes=6 * 1024 * 1024) as sock:
                sock.connect("ipc:///tmp/rpc." + unit)
                sock.send_multipart([action.encode("ascii"), data])
                return sock.recv()
        except zmq.ZmqError as exc:
            raise BridgeError(-10, "Local %s.%s request failed" % (unit, action)) from exc

    def recv(self, timeout=0.1):
        if not self.responses.readable(max(0, int(timeout * 1000))):
            return None
        raw = self.responses.recv()
        if len(raw) > 6 * 1024 * 1024:
            raise BridgeError(-3, "Native response exceeds bound")
        try:
            result = json.loads(raw)
        except (ValueError, UnicodeError) as exc:
            raise BridgeError(-3, "Malformed native response") from exc
        if not isinstance(result, dict):
            raise BridgeError(-3, "Invalid native response type")
        return result

    def checked(self, result):
        error = result.get("error")
        if not isinstance(error, dict) or type(error.get("code")) is not int:
            raise BridgeError(-3, "Native response lacks an error code")
        if error["code"] != 0:
            raise BridgeError(-11, "Native service returned code %d" % error["code"])
        return result

    def command(self, work_id, action, data="None", object_name="None", timeout=45):
        unit = work_id.split(".")[0]
        request = self.request_id()
        message = {"request_id": request, "work_id": work_id, "action": action,
                   "object": object_name, "data": data}
        ack = self.rpc(unit, action, pack_pair(self.route.encode(), encode(message)))
        if ack != b"None":
            raise BridgeError(-11, "Native command was not accepted")
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            result = self.recv(min(0.1, max(0, deadline - time.monotonic())))
            if result is None or result.get("request_id") != request:
                continue
            actual = result.get("work_id")
            if action == "setup":
                if actual != unit and not re.fullmatch(re.escape(unit) + r"\.[0-9]{1,10}", str(actual)):
                    continue
            elif actual != work_id:
                continue
            self.checked(result)
            return result
        raise BridgeError(-12, "Native %s.%s response timed out" % (unit, action))

    def task_list(self, unit):
        reply = self.command(unit, "taskinfo", timeout=8)
        if reply.get("object") != unit + ".tasklist" or not isinstance(reply.get("data"), list):
            raise BridgeError(-3, "Invalid native task list")
        items = reply["data"]
        if len(items) > 8 or any(not isinstance(item, str) or not re.fullmatch(unit + r"\.[0-9]{1,10}", item) for item in items):
            raise BridgeError(-3, "Unexpected native task list")
        return items

    def task_info(self, task):
        reply = self.command(task, "taskinfo", timeout=8)
        if reply.get("object") != task.split(".")[0] + ".taskinfo" or not isinstance(reply.get("data"), dict):
            raise BridgeError(-3, "Invalid native task configuration")
        return reply["data"]

    def persist_owned(self):
        atomic_json(self.owner_path, {"schema": 1, "boot_id": self.boot_id,
                    "feed_id": self.feed_id, "tts_id": self.tts_id, "asr_id": self.asr_id,
                    "audio_owned": self.audio_owned, "audio_configured": self.audio_configured,
                    "audio_original": self.audio_original if self.audio_configured else None, "mic_proxy_owned": self.mic_proxy_owned})

    def cleanup_previous(self):
        if not self.owner_path.exists():
            return
        if self.owner_path.stat().st_size > 64 * 1024:
            raise BridgeError(-30, "Invalid adapter ownership record")
        previous = json.loads(self.owner_path.read_text())
        if previous.get("boot_id") != self.boot_id:
            self.persist_owned()
            return
        feed = previous.get("feed_id")
        if feed is not None and not re.fullmatch(r"muse_llm\.[0-9]{1,10}", str(feed)):
            raise BridgeError(-30, "Invalid adapter producer record")
        tasks = []
        for field, unit, model, response, inputs in (
            ("asr_id", "asr", ASR_MODEL, "asr.utf-8.stream", ["sys.pcm"]),
            ("tts_id", "melotts", TTS_MODEL, "melotts.pcm", [feed]),
        ):
            task = previous.get(field)
            if task is None:
                continue
            if not re.fullmatch(unit + r"\.[0-9]{1,10}", str(task)):
                raise BridgeError(-30, "Invalid adapter task record")
            if task not in self.task_list(unit):
                continue
            data = self.task_info(task)
            if data.get("model") != model or data.get("response_format") != response or data.get("inputs") != inputs or data.get("enoutput") is not True:
                raise BridgeError(-30, "Recorded adapter task no longer matches")
            tasks.append(task)
        # Validate every task before releasing any of them.
        if previous.get("audio_configured") is True:
            original = previous.get("audio_original")
            if not isinstance(original, dict) or len(original) > 128:
                raise BridgeError(-30, "Invalid saved playback configuration")
            self.audio_original = original
            self.audio_configured = True
        self.audio_owned = previous.get("audio_owned") is True
        for task in reversed(tasks):
            self.command(task, "exit", timeout=60)
        if previous.get("mic_proxy_owned") is True:
            self.rpc("audio", "cap_stop")
            self.command("audio", "setup", self.config("audio")["cap_param"], "audio.cap", timeout=8)
        if self.audio_owned:
            self.stop_audio()
        if feed:
            self.rpc("sys", "release_unit", feed)
        if self.audio_configured:
            self.command("audio", "setup", self.audio_original, "audio.play", timeout=8)
            self.audio_configured = False
        self.audio_owned = False
        self.persist_owned()

    def config(self, name):
        defaults = [("config_base_mode_path", "/opt/m5stack/data/"),
                    ("config_base_mode_config_path", "/opt/m5stack/etc/"),
                    ("config_model_d", "/opt/m5stack/data/models/")]
        paths = []
        for key, default in defaults:
            val = self.rpc("sys", "sql_select", key).decode("utf-8").strip()
            if val.startswith('"'):
                val = json.loads(val)
            val = val or default
            if not isinstance(val, str) or not val.startswith("/") or "\x00" in val:
                raise BridgeError(-3, "Invalid local model configuration path")
            paths.append(Path(val))
        base, config, models = paths
        candidates = [Path.cwd() / (name + ".json"), Path.cwd() / ("mode_" + name + ".json"),
                      base / name / (name + ".json"), base / name / ("mode_" + name + ".json"),
                      models / (name + ".json"), models / ("mode_" + name + ".json"),
                      config / (name + ".json"), config / ("mode_" + name + ".json"),
                      base.parent / "share" / (name + ".json"), base.parent / "share" / ("mode_" + name + ".json")]
        for path in candidates:
            if path.is_file():
                if path.stat().st_size > 128 * 1024:
                    raise BridgeError(-3, "Model configuration exceeds bound")
                data = json.loads(path.read_text())
                if not isinstance(data, dict):
                    raise BridgeError(-3, "Invalid model configuration")
                return data
        raise BridgeError(-13, "Required local model configuration is missing")

    def configure_audio(self):
        model = self.config("melotts-zh-cn")
        if model.get("mode_param", {}).get("audio_rate") != PCM_RATE:
            raise BridgeError(-13, "Configured MeloTTS PCM rate is not 16000 Hz")
        audio_config = self.config("audio")
        original = audio_config.get("play_param")
        if not isinstance(original, dict):
            raise BridgeError(-13, "Audio playback configuration is missing")
        capture = audio_config.get("cap_param", {})
        if not isinstance(capture, dict) or capture.get("rate") != 16000 or capture.get("bit") != 16 or capture.get("aistAttr.u32PeriodSize") != 160:
            raise BridgeError(-13, "Microphone cadence differs from the verified v1.3 profile")
        self.audio_original = dict(original)
        # The vendor setup action changes in-memory settings; no files change.
        settings = dict(original)
        settings.update({"rate": PCM_RATE, "bit": 16, "channel": 1,
                         "stAttr.enSamplerate": PCM_RATE, "stAttr.enSoundmode": 0,
                         "stVqeAttr.s32SampleRate": PCM_RATE,
                         "enInSampleRate": PCM_RATE, "gResample": 0,
                         "gInstant": 0, "gInsertSilence": 0})
        self.audio_configured = True
        self.persist_owned()
        self.command("audio", "setup", settings, "audio.play", timeout=8)

    def initialize(self):
        # Load the CPU voice while native microphone/playback checks run.
        try:
            from muse_piper import warmup
            warmup()
        except Exception:
            pass
        self.cleanup_previous()
        if self.task_list("asr") or self.task_list("melotts"):
            raise BridgeError(-30, "Other speech tasks are active; pause the previous application before a guarded takeover")
        self.audio_owned = True
        self.stop_audio()
        self.configure_audio()
        self.command("audio", "setup", self.config("audio")["cap_param"], "audio.cap", timeout=8)
        raw = self.rpc("sys", "register_unit", "muse_llm")
        number, ports = unpack_pair(raw)
        output, unused_input = unpack_pair(ports)
        if not number.isdigit() or not output.startswith(b"ipc:///tmp/llm/") or b".." in output:
            raise BridgeError(-3, "Invalid producer allocation")
        self.feed_id = "muse_llm." + number.decode()
        self.persist_owned()
        self.feed = self.context.socket(zmq.XPUB, timeout_ms=3000, max_bytes=16384)
        self.feed.bind(output.decode())
        reply = self.command("melotts", "setup", {"model": TTS_MODEL, "response_format": "melotts.pcm",
                              "enoutput": True, "input": [self.feed_id]}, "melotts.setup", timeout=90)
        task = reply.get("work_id")
        if not re.fullmatch(r"melotts\.[0-9]{1,10}", str(task)):
            raise BridgeError(-3, "MeloTTS did not return an allocated task")
        self.tts_id = task
        self.persist_owned()
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if self.feed.readable(100) and self.feed.recv() == b"\x01":
                break
        else:
            raise BridgeError(-12, "MeloTTS producer subscription was not established")
        # Hold exactly one raw capture reference. Its existing PUB stays on
        # the original endpoint; only our ASR receives the gated local feed.
        self.mic_proxy_owned = True
        self.persist_owned()
        raw = self.rpc("audio", "cap")
        if raw != b"ipc:///tmp/llm/pcm.cap.socket":
            raise BridgeError(-3, "Unexpected raw capture endpoint")
        from muse_mic_meter import MicMeter, ENDPOINT
        self.mic = MicMeter()
        cap = dict(self.config("audio")["cap_param"])
        cap["sys_pcm_cap_channel"] = ENDPOINT
        self.command("audio", "setup", cap, "audio.cap", timeout=8)
        self.start_asr()
        self.mic.wait_ready(10)
        # Prime the actual decoder with a fixed public phrase, never ambient
        # microphone audio, before announcing readiness for the first turn.
        from muse_piper import synthesize
        warm_pcm = synthesize("Ready.", None, lambda _: None)
        self.mic.prime(warm_pcm + b"\0" * 64000)
        warm_deadline = time.monotonic() + 18
        while time.monotonic() < warm_deadline:
            result = self.recv(.1)
            if result and result.get("work_id") == self.asr_id:
                self.checked(result)
                data = result.get("data")
                if isinstance(data, dict) and data.get("finish") and data.get("delta"):
                    break
        else:
            raise BridgeError(-12, "Microphone recognizer warm-up timed out")
        self.mic.finish_prime()
        self.start_asr()
        self.pause_asr()
        self.starting = False

    def start_asr(self):
        if self.asr_id:
            # The vendor work action resets the utterance without reloading
            # its model. The PCM gate passes silence between turns.
            deadline = time.monotonic() + .5
            while time.monotonic() < deadline and self.recv(0) is not None:
                pass
            self.command(self.asr_id, "work", timeout=5)
            return
        if self.mic: self.mic.subscribed.clear()
        reply = self.command("asr", "setup", {"model": ASR_MODEL, "response_format": "asr.utf-8.stream",
                             "enoutput": True, "input": ["sys.pcm"]}, "asr.setup", timeout=45)
        task = reply.get("work_id")
        if not re.fullmatch(r"asr\.[0-9]{1,10}", str(task)):
            raise BridgeError(-3, "ASR did not return an allocated task")
        self.asr_id, self.asr_request = task, reply["request_id"]
        self.persist_owned()
        if self.mic:
            self.mic.wait_ready(10)

    def pause_asr(self):
        if self.mic:
            self.mic.record(False)

    def set_mic_level(self, level):
        if type(level) is not int or not 0 <= level <= 100:
            raise BridgeError(-2, "Microphone level must be 0 to 100")
        if self.mic:
            self.mic.set_level(level)
        self.mic_level = level

    def assert_exclusive(self):
        expected_asr = [self.asr_id] if self.asr_id else []
        if self.task_list("asr") != expected_asr or self.task_list("melotts") != [self.tts_id]:
            raise BridgeError(-30, "Speech task ownership changed; another application may have restarted")
        data = self.task_info(self.tts_id)
        if data.get("model") != TTS_MODEL or data.get("response_format") != "melotts.pcm" or data.get("inputs") != [self.feed_id] or data.get("enoutput") is not True:
            raise BridgeError(-30, "The owned speech task configuration changed")

    def stop_asr(self):
        if self.asr_id:
            self.command(self.asr_id, "exit", timeout=15)
            self.asr_id, self.asr_request = None, None
            self.persist_owned()

    def synthesize(self, phrase, cancel):
        check_cancel(cancel)
        try:
            from muse_piper import synthesize
            pcm = synthesize(phrase, cancel, check_cancel)
            engine = "piper-en_US-lessac-low"
        except Cancelled:
            raise
        except Exception:
            check_cancel(cancel)
            pcm = self._synthesize_native(phrase, cancel)
            engine = "original-native-fallback"
        try:
            atomic_json(self.state_dir / "piper-last-result.json",
                        {"engine": engine, "pcm_bytes": len(pcm), "offline": True})
        except OSError:
            pass
        return pcm

    def _synthesize_native(self, phrase, cancel):
        check_cancel(cancel)
        request = self.request_id()
        self.feed.send(encode({"request_id": request, "work_id": self.tts_id, "action": "inference",
                              "zmq_com": self.route, "object": "tts.utf-8", "data": phrase}))
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            check_cancel(cancel)
            reply = self.recv(0.1)
            if reply is None or reply.get("request_id") != request or reply.get("work_id") != self.tts_id:
                continue
            self.checked(reply)
            if reply.get("object") != "melotts.pcm" or not isinstance(reply.get("data"), str):
                raise BridgeError(-3, "Unexpected synthesized PCM format")
            try:
                pcm = base64.b64decode(reply["data"], validate=True)
            except ValueError as exc:
                raise BridgeError(-3, "Invalid synthesized PCM encoding") from exc
            if not pcm or len(pcm) % 2 or len(pcm) > MAX_PCM:
                raise BridgeError(-3, "Synthesized audio exceeds bound or is empty")
            return pcm
        raise BridgeError(-12, "Local speech synthesis timed out")

    def stop_audio(self):
        # queue_play_stop prevents legacy queued jobs from starting. All new
        # adapter audio is handled only through the tracked play_raw thread.
        # The vendor audio RPC can answer before its handlers are registered
        # during a cold boot. Retry only these idempotent stop operations; never
        # repeat a play request or mark playback stopped without both replies.
        for action, timeout, message in (
                ("queue_play_stop", 3000, "Legacy playback queue stop was rejected"),
                ("play_stop", 15000, "Native playback join was rejected")):
            attempts = 61 if getattr(self, "starting", False) else 4
            deadline = time.monotonic() + 30
            for attempt in range(attempts):
                try:
                    reply = self.rpc("audio", action, timeout_ms=timeout)
                    if reply == b"None":
                        break
                    raise BridgeError(-11, message)
                except BridgeError as exc:
                    if exc.code not in (-10, -11) or attempt == attempts - 1 or time.monotonic() >= deadline:
                        raise
                    if attempts > 4:
                        if attempt == 0:
                            print(json.dumps({"phase": "waiting_for_audio", "ready": False}), flush=True)
                        time.sleep(0.5)
                    else:
                        time.sleep((0.15, 0.3, 0.6)[attempt])
        self.active_play = False

    def play(self, pcm, volume, cancel):
        from muse_playback_control import play
        return play(self,pcm,volume,cancel,check_cancel,scale_pcm,PCM_RATE,BridgeError)

    def record_synthesis(self, engine, pcm_bytes):
        try:
            atomic_json(self.state_dir / "piper-last-result.json",
                        {"engine": engine, "pcm_bytes": pcm_bytes, "offline": True,
                         "persistent_worker": True, "prefetch": True})
        except OSError:
            pass

    def prepare_phrase_words(self, phrase, pcm):
        self.word_schedule = self.word_timing.phrase(phrase, pcm) if self.word_timing else []

    def speak(self, text, volume, cancel, on_play=None):
        if self.mic:self.mic.pause_output(True)
        try:
            return self._speak_active(text, volume, cancel, on_play)
        finally:
            if self.mic:self.mic.pause_output(False)

    def _speak_active(self, text, volume, cancel, on_play=None):
        from muse_word_timing import WordTiming
        self.word_timing = WordTiming(text) if self.word_callback else None
        try:
            from muse_piper import speak
        except ImportError:
            return self._speak_serial(text, volume, cancel, on_play)
        try:
            return speak(self, text, volume, cancel, on_play, check_cancel)
        except BridgeError:
            raise
        except Exception as exc:
            raise BridgeError(-12, "Offline narration did not complete") from exc

    def _speak_serial(self, text, volume, cancel, on_play=None):
        phrases = split_phrases(text)
        if not phrases:
            raise BridgeError(-2, "There is no pronounceable text")
        deadline = time.monotonic() + 1200
        total_pcm, first = 0, True
        for phrase in phrases:
            check_cancel(cancel)
            if time.monotonic() > deadline:
                raise BridgeError(-12, "Speech exceeded the total time limit")
            pcm = self.synthesize(phrase, cancel)
            check_cancel(cancel)
            if first and on_play:
                on_play()
            first = False
            total_pcm += len(pcm)
            self.prepare_phrase_words(phrase, pcm)
            self.play(pcm, volume, cancel)
        return {"segments": len(phrases), "pcm_bytes": total_pcm, "joined": True}

    def listen(self, cancel, end, on_listening=None, on_level=None, max_seconds=90):
        self.stop_audio()
        check_cancel(cancel)
        transcript, start, end_deadline, end_started = Transcript(), time.monotonic(), None, None
        try:
            self.start_asr()
            self.mic.record(True)
            check_cancel(cancel)
            if on_listening:
                on_listening()
            start = time.monotonic()
            next_level = 0
            while time.monotonic() - start < max_seconds:
                check_cancel(cancel)
                if on_level and time.monotonic() >= next_level:
                    on_level()
                    next_level = time.monotonic() + .15
                reply = self.recv(0.1)
                if reply is not None and reply.get("work_id") == self.asr_id and reply.get("request_id") == self.asr_request:
                    self.checked(reply)
                    if reply.get("object") != "asr.utf-8.stream":
                        raise BridgeError(-3, "Unexpected ASR format")
                    transcript.accept(reply.get("data"))
                if end.is_set():
                    if end_deadline is None:
                        end_started = time.monotonic()
                        end_deadline = end_started + 8
                    if time.monotonic() - end_started >= .25:
                        self.mic.record(False)
                    # v1.3 batches microphone frames before decoding. Retain
                    # capture briefly after release even if an earlier phrase
                    # was already final, so a trailing phrase can be decoded.
                    if transcript.final_parts and not transcript.partial and time.monotonic() - end_started >= 2:
                        return transcript.text()
                    if time.monotonic() >= end_deadline:
                        raise BridgeError(-12, "No final recognized speech after button release")
            raise BridgeError(-12, "Recording exceeded the 90-second limit")
        finally:
            self.pause_asr()

    def takeover(self, snapshot):
        asr_ids, tts_ids = self.task_list("asr"), self.task_list("melotts")
        if not asr_ids and not tts_ids:
            atomic_json(snapshot, {"schema": 1, "boot_id": self.boot_id, "tasks": [], "status": "already-clear"})
            return {"already_clear": True, "tasks_released": 0}
        if len(asr_ids) != 1 or len(tts_ids) != 1:
            raise BridgeError(-30, "Expected exactly one verified previous-application ASR and MeloTTS task")
        asr, tts = self.task_info(asr_ids[0]), self.task_info(tts_ids[0])
        a_inputs, t_inputs = asr.get("inputs"), tts.get("inputs")
        if not isinstance(a_inputs, list) or not isinstance(t_inputs, list) or any(not isinstance(v, str) for v in a_inputs + t_inputs):
            raise BridgeError(-30, "Previous-application input configuration is incomplete")
        kws = [v for v in a_inputs if isinstance(v, str) and re.fullmatch(r"kws\.[0-9]{1,10}", v)]
        llm = [v for v in t_inputs if isinstance(v, str) and re.fullmatch(r"llm\.[0-9]{1,10}", v)]
        matches = (asr.get("model") == ASR_MODEL and asr.get("response_format") == "asr.utf-8.stream"
                   and asr.get("enoutput") is True and len(kws) == 1 and sorted(a_inputs) == sorted(["sys.pcm", kws[0]])
                   and tts.get("model") == TTS_MODEL and tts.get("response_format") == "sys.pcm"
                   and tts.get("enoutput") is False and len(llm) == 1 and sorted(t_inputs) == sorted([llm[0], kws[0]]))
        if not matches:
            raise BridgeError(-30, "Current speech tasks do not match the verified previous-application configuration")
        record = {"schema": 1, "boot_id": self.boot_id, "status": "snapshot-written",
                  "tasks": [{"work_id": asr_ids[0], "config": asr}, {"work_id": tts_ids[0], "config": tts}]}
        atomic_json(snapshot, record)
        # Exit joins the old input subscribers. Stop the old TTS producer before
        # cancelling its queued audio; KWS and LLM tasks are deliberately kept.
        self.command(tts_ids[0], "exit", timeout=60)
        self.command(asr_ids[0], "exit", timeout=15)
        self.audio_owned = True
        self.stop_audio()
        # A tracked silent job acquires the native audio mutex after any old
        # queue job, providing a real join barrier before bridge ownership.
        self.play(b"\0" * (PCM_RATE // 2), 100, threading.Event())
        self.configure_audio()
        record["status"] = "released"
        atomic_json(snapshot, record)
        return {"already_clear": False, "tasks_released": 2}

    def close(self):
        if self.closed:
            return
        errors = []
        operations = [self.stop_asr]
        if self.audio_owned:
            operations.insert(0, self.stop_audio)
        for operation in operations:
            try:
                operation()
            except Exception as exc:
                errors.append(exc)
        if self.mic:
            try:
                self.mic.close()
                self.mic = None
            except Exception as exc:
                errors.append(exc)
        if self.mic_proxy_owned:
            try:
                self.rpc("audio", "cap_stop")
                self.command("audio", "setup", self.config("audio")["cap_param"], "audio.cap", timeout=8)
                self.mic_proxy_owned = False
                self.persist_owned()
            except Exception as exc:
                errors.append(exc)
        if self.tts_id:
            try:
                self.command(self.tts_id, "exit", timeout=60)
                self.tts_id = None
                self.persist_owned()
            except Exception as exc:
                errors.append(exc)
        if self.feed:
            self.feed.close()
            self.feed = None
        if self.feed_id and not self.tts_id:
            try:
                self.rpc("sys", "release_unit", self.feed_id)
                self.feed_id = None
                self.persist_owned()
            except Exception as exc:
                errors.append(exc)
        if self.audio_configured and self.audio_original and not errors:
            try:
                self.command("audio", "setup", self.audio_original, "audio.play", timeout=8)
                self.audio_configured = False
                self.audio_owned = False
                self.persist_owned()
            except Exception as exc:
                errors.append(exc)
        if not errors and not self.asr_id and not self.tts_id and not self.feed_id:
            self.audio_owned = False
            if self.owner_path.exists():
                self.persist_owned()
        self.responses.close()
        self.context.close()
        try:
            Path(self.route[6:]).unlink()
        except FileNotFoundError:
            pass
        self.closed = True
        if errors:
            raise BridgeError(-31, "Local speech cleanup did not complete") from errors[0]
