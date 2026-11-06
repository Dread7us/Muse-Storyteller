#!/usr/bin/env python3
"""Muse/Core2 local voice service for the verified Module LLM configuration.

Transport: the stock StackFlow UART router -> ipc:///tmp/rpc.musevoice.
Runtime dependencies: Python >=3.8 and the existing system libzmq. Nothing is
sent to an Internet speech provider. User text is never interpreted as code.
"""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import queue
import re
import signal
import stat
import sys
import threading
import time
import uuid

import muse_zmq as zmq
from muse_voice_backend import (ASR_MODEL, TTS_MODEL, Backend, BridgeError,
                                Cancelled, encode, pack_pair, unpack_pair)

PROTOCOL = 1
SERVICE = "musevoice"
CAPABILITIES = ["local_asr", "local_tts", "playback_join", "generation_cancel", "playback_control"]
ACTIONS = {"inspect", "hello", "status", "listen_begin", "listen_end", "speak", "cancel", "memory", "control", "cloud"}
REQUEST_LIMIT = 64 * 1024  # permits JSON escaping of the bounded 8192-byte text


def parse_request(action, envelope, allowed_route):
    if len(envelope) > REQUEST_LIMIT + 128:
        raise BridgeError(-2, "Request exceeds bound")
    route, payload = unpack_pair(envelope)
    try:
        route = route.decode("ascii")
        action = action.decode("ascii")
        req = json.loads(payload)
    except (ValueError, UnicodeError) as exc:
        raise BridgeError(-2, "Invalid request encoding") from exc
    local_client = re.fullmatch(r"ipc:///tmp/musevoice/client-[0-9a-f]{24}\.sock", route)
    if route != allowed_route and not local_client:
        raise BridgeError(-2, "Reply route is not an approved local IPC endpoint")
    if action == "inspect" and not local_client:
        raise BridgeError(-2, "Inspection requires a local diagnostic route")
    if action not in ACTIONS or not isinstance(req, dict) or req.get("action") != action:
        raise BridgeError(-2, "Invalid bridge action")
    if req.get("work_id") != SERVICE or req.get("object") != "musevoice.v1":
        raise BridgeError(-2, "Unsupported bridge protocol")
    request_id, data = req.get("request_id"), req.get("data")
    # StackFlow RAW framing converts only the binary body to base64 locally.
    # Metadata remains outside data in its bounded JSON header.
    if action == "cloud" and isinstance(data, str) and isinstance(req.get("raw_info"), dict):
        data = dict(req['raw_info'],op='raw_block',audio=data)
    if not isinstance(request_id, str) or not re.fullmatch(r"[A-Za-z0-9_.:-]{1,80}", request_id) or not isinstance(data, dict):
        raise BridgeError(-2, "Invalid request identity")
    generation, session = data.get("generation"), data.get("session")
    if type(generation) is not int or not 0 <= generation <= 0xffffffff:
        raise BridgeError(-2, "Invalid generation")
    if action not in ("inspect", "hello", "status", "memory", "cloud") and generation == 0:
        raise BridgeError(-2, "Operation generation must be nonzero")
    if not isinstance(session, str) or not re.fullmatch(r"[0-9a-f]{8}", session):
        raise BridgeError(-2, "Invalid boot session")
    if action == "control":
        if type(data.get("paused")) is not bool or type(data.get("volume")) is not int or not 0<=data["volume"]<=100:
            raise BridgeError(-2,"Invalid playback control")
    if action == "speak":
        if data.get("speed","normal") not in ("slow","normal","fast"):raise BridgeError(-2,"Invalid speech speed")
        text, volume = data.get("text"), data.get("volume", 70)
        if not isinstance(text, str) or not text.strip() or len(text.encode("utf-8")) > 8192 or "\x00" in text:
            raise BridgeError(-2, "Speech text must contain 1 to 8192 UTF-8 bytes")
        if type(volume) is not int or not 0 <= volume <= 100:
            raise BridgeError(-2, "Volume must be an integer from 0 to 100")
    if action == "listen_begin":
        level=data.get("mic_level",100)
        if type(level) is not int or not 0 <= level <= 100:
            raise BridgeError(-2,"Microphone level must be an integer from 0 to 100")
    return {"route": route, "request_id": request_id, "action": action,
            "data": data, "generation": generation, "session": session}


def response(req, event, code=0, message="", **fields):
    data = {"protocol": PROTOCOL, "session": req["session"], "generation": req["generation"], "event": event}
    data.update(fields)
    return {"request_id": req["request_id"], "work_id": SERVICE,
            "object": "musevoice.event", "data": data,
            "error": {"code": code, "message": message[:180]}}


class Operation:
    def __init__(self, req):
        self.req = req
        self.cancel, self.end = threading.Event(), threading.Event()
        from muse_playback_control import PlaybackControl
        self.control=PlaybackControl(req["data"].get("volume",70))
        self.actual_paused=False
        self.applied_volume=None
        self.end_req, self.cancel_reqs = None, []


def default_backend(state_dir="/var/lib/muse-core2-bridge"):
    from muse_whisper_backend import Backend as WhisperBackend
    return WhisperBackend(state_dir=state_dir)

class Engine:
    """Single native worker; wire requests stay responsive during inference."""
    def __init__(self, emit, backend_factory=default_backend):
        self.emit, self.backend_factory = emit, backend_factory
        from muse_recorder import Recorder,UnavailableRecorder
        try:self.recorder=Recorder()
        except (OSError,ValueError,TypeError):self.recorder=UnavailableRecorder()
        self.meter = None
        self.lock = threading.RLock()
        self.wake, self.stop = threading.Event(), threading.Event()
        self.pending, self.current = None, None
        self.ready, self.failure = False, None
        self.native_store = None
        self.session, self.retired_sessions, self.used_generations = None, set(), set()
        self.finished_generations = set()
        self.thread = threading.Thread(target=self.run, name="muse-native-worker", daemon=True)
        self.thread.start()

    def send(self, req, event, **fields):
        self.emit(req["route"], response(req, event, **fields))

    def fail(self, req, error):
        self.send(req, "error", code=error.code, message=error.message)

    def ready_event(self, req, busy=False):
        health=getattr(getattr(self,'backend',None),'startup_health',None)
        if health and not health():
            self.fail(req,BridgeError(-23,'Voice decoder or microphone is not ready'));return
        current=self.current
        speech=current if current and current.req["action"]=="speak" else None
        self.send(req, "ready", ready=True, busy=busy, asr_model=ASR_MODEL,
                  tts_model=TTS_MODEL, asr_engine=getattr(self,"recognizer_name",ASR_MODEL), capabilities=CAPABILITIES, playback_paused=speech.actual_paused if speech else False, playback_volume=speech.control.read()[1] if speech else None, playback_applied_volume=speech.applied_volume if speech else None, playback_generation=speech.req["generation"] if speech else None, **self.meter.fields())

    def recorder_command(self,req):
        with self.lock:
            op=req['data'].get('op')
            if not self.ready or self.failure or req['session']!=self.session:raise ValueError('Wait for the voice module to be ready')
            if op=='recstart':
                if self.recorder.snapshot()['active']:return self.recorder.snapshot()
                if self.current or self.pending:raise ValueError('Finish Muse speaking before recording')
                level=req['data'].get('offset',100)
                if type(level) is not int or not 0<=level<=100:raise ValueError('Invalid microphone level')
                self.recorder.prepare(req['data'].get('page',1)==1)
                internal=dict(req);internal.update(action='record_begin',request_id='rec-'+uuid.uuid4().hex,generation=0)
                internal['data']=dict(req['data'],mic_level=level)
                self.pending=Operation(internal);self.wake.set()
            elif op=='recstop':
                current=self.current or self.pending
                if current and current.req['action']=='record_begin':current.end.set()
            elif op in ('recprev','recnext'):return self.recorder.select(-1 if op=='recprev' else 1)
            self.recorder.review_if_idle(lambda:self.ready and not self.stop.is_set() and not (self.current or self.pending))
            return self.recorder.snapshot(page=req['data'].get('page',0) if op=='recpage' else None,original=req['data'].get('offset',0)==1)

    def submit(self, req):
        with self.lock:
            action, generation, session = req["action"], req["generation"], req["session"]
            if self.failure:
                self.fail(req, self.failure)
                return
            if action == "inspect":
                if self.ready: self.ready_event(req, bool(self.current or self.pending))
                else: self.fail(req, BridgeError(-23, "Local voice is still starting"))
                return
            if action == "hello":
                if session in self.retired_sessions:
                    self.fail(req, BridgeError(-21, "Retired boot session"))
                    return
                if self.pending:
                    self.fail(req, BridgeError(-22, "Ownership claim already pending"))
                    return
                if self.current:
                    self.current.cancel.set()
                self.pending = Operation(req)
                self.wake.set()
                return
            if not self.ready:
                self.fail(req, BridgeError(-23, "Local voice is still starting"))
                return
            if session != self.session:
                self.fail(req, BridgeError(-21, "Request belongs to a stale boot session"))
                return
            if action == "status":
                monitor = req["data"].get("monitor")
                if type(monitor) is bool: self.meter.request(monitor)
                self.ready_event(req, bool(self.current or self.pending))
                return
            current = self.current
            if action == "control":
                target=current if current and current.req["generation"]==generation else self.pending
                if not target or target.req["action"]!="speak" or target.req["generation"]!=generation or target.req["session"]!=session:
                    self.fail(req,BridgeError(-21,"No matching narration is active"));return
                target.control.update(req["data"]["paused"],req["data"]["volume"])
                self.send(req,"control",accepted=True)
                self.send(target.req,"playback",paused=target.actual_paused);return
            if action == "listen_end":
                current = current if current and current.req["generation"] == generation else self.pending
                if not current or current.req["action"] != "listen_begin" or current.req["generation"] != generation or current.req["session"] != session:
                    self.fail(req, BridgeError(-21, "No matching recording is active"))
                    return
                if current.end_req:
                    self.fail(req, BridgeError(-22, "Recording release already received"))
                    return
                current.end_req = req
                current.end.set()
                return
            if action == "cancel":
                target = current if current and current.req["generation"] == generation else self.pending
                if not target or target.req["generation"] != generation or target.req["session"] != session:
                    if generation in self.finished_generations:
                        self.send(req, "cancelled", joined=True)
                        return
                    self.fail(req, BridgeError(-21, "No matching generation is active"))
                    return
                target.cancel_reqs.append(req)
                target.cancel.set()
                self.wake.set()
                return
            if self.recorder.snapshot()['active']:
                self.fail(req,BridgeError(-22,'Local recorder owns the microphone; Stop and Save first'));return
            if generation in self.used_generations:
                self.fail(req, BridgeError(-21, "Generation already used in this boot session"))
                return
            if self.pending or (current and action != "listen_begin"):
                self.fail(req, BridgeError(-22, "Another voice operation is active"))
                return
            if len(self.used_generations) >= 100000:
                self.fail(req, BridgeError(-21, "Start a new boot session"))
                return
            self.used_generations.add(generation)
            if current:
                current.cancel.set()
            self.pending = Operation(req)
            self.send(req, "accepted")
            self.wake.set()

    def run(self):
        backend = None
        try:
            backend = self.backend_factory()
            startup_at=time.monotonic()
            backend.initialize()
            self.backend=backend
            print(json.dumps(dict(phase='startup_ready',seconds=round(time.monotonic()-startup_at,3),recognizer=getattr(backend,'recognizer_name',ASR_MODEL))),flush=True)
            self.recognizer_name=getattr(backend,"recognizer_name",ASR_MODEL)
            self.meter = backend.mic
            with self.lock:
                self.ready = True
            while not self.stop.is_set():
                with self.lock:
                    operation, self.pending = self.pending, None
                    if operation:
                        self.current = operation
                if not operation:
                    self.wake.wait(0.1)
                    self.wake.clear()
                    continue
                req = operation.req
                self.meter.block(req["action"] != "listen_begin")
                cleanup_ok = False
                try:
                    if operation.cancel.is_set():
                        raise Cancelled()
                    if req["action"] == "hello":
                        prepare=getattr(backend,'prepare_ready',None)
                        if prepare:prepare()
                        backend.pause_asr()
                        backend.stop_audio()
                        backend.assert_exclusive()
                        with self.lock:
                            if self.session and self.session != req["session"]:
                                self.retired_sessions.add(self.session)
                            if self.session != req["session"]:
                                self.used_generations.clear()
                                self.finished_generations.clear()
                            self.session = req["session"]
                        cleanup_ok = True
                        self.ready_event(req)
                    elif req["action"] == "speak":
                        backend.playback_control=operation.control
                        backend.playback_pause_total=0.
                        def playback_state(paused):
                            operation.actual_paused=paused
                            self.send(req,"playback",paused=paused)
                        backend.playback_state_callback=playback_state
                        backend.playback_volume_callback=lambda volume:setattr(operation,"applied_volume",volume)
                        backend.speech_speed = req["data"].get("speed","normal")
                        backend.word_callback = lambda start, end: self.send(req, "word", start=start, end=end)
                        if req['data'].get('external_key'):
                            from muse_native_audio import NativeAudioError
                            try:
                                self.native_store.play(backend,req['data']['external_key'],req['data']['text'],req['data'].get('volume',70),operation.cancel,lambda:self.send(req,'speaking'),lambda:self.send(req,'buffering'))
                            except Cancelled:raise
                            except Exception as exc:
                                raise BridgeError(-35,str(exc) if isinstance(exc,NativeAudioError) else 'Muse audio playback failed') from exc
                        else:
                            backend.speak(req["data"]["text"], req["data"].get("volume", 70), operation.cancel,
                                          lambda: self.send(req, "speaking"))
                        if operation.cancel.is_set():
                            raise Cancelled()
                        cleanup_ok = True
                        self.send(req, "done", joined=True)
                    elif req["action"] == "record_begin":
                        backend.set_mic_level(req['data'].get('mic_level',100))
                        self.recorder.run(backend,operation.cancel,operation.end)
                        cleanup_ok=True
                    elif req["action"] == "listen_begin":
                        self.meter.suspend()
                        backend.set_mic_level(req["data"].get("mic_level",100))
                        text = backend.listen(operation.cancel, operation.end,
                                              lambda: self.send(req, "listening"),
                                              lambda: self.send(req, "level", **self.meter.fields()))
                        if operation.cancel.is_set():
                            raise Cancelled()
                        if not operation.end_req:
                            raise BridgeError(-12, "Recording completed without a release request")
                        cleanup_ok = True
                        self.send(operation.end_req, "transcript", text=text, final=True)
                except Cancelled:
                    # Cancellation is acknowledged only after both native
                    # audio and this operation's recognizer have been joined.
                    backend.pause_asr()
                    backend.stop_audio()
                    cleanup_ok = True
                    if not operation.cancel_reqs:
                        self.send(req, "cancelled", joined=True)
                except BridgeError as exc:
                    if exc.code in (-12,-35): backend.pause_asr()
                    else: backend.stop_asr()
                    backend.stop_audio()
                    cleanup_ok = True
                    self.fail(operation.end_req or req, exc)
                finally:
                    backend.word_callback = None
                    backend.word_schedule = []
                    backend.word_timing = None
                    with self.lock:
                        # submit() can append a cancellation while cleanup is
                        # finishing; hold this lock through final notification.
                        for cancel_req in operation.cancel_reqs:
                            if cleanup_ok:
                                self.send(cancel_req, "cancelled", joined=True)
                            else:
                                self.fail(cancel_req, BridgeError(-31, "Native cancellation could not be confirmed"))
                        if not cleanup_ok:
                            self.fail(operation.end_req or req, BridgeError(-31, "Native voice cleanup could not be confirmed"))
                        elif req["action"] in ("speak", "listen_begin"):
                            self.finished_generations.add(req["generation"])
                        self.current = None
                        if req["action"]=="record_begin":
                            self.recorder.released()
                            self.recorder.review_if_idle(lambda:self.ready and not self.stop.is_set() and not (self.current or self.pending))
                    self.meter.block(False)
        except Exception as exc:
            failure = exc if isinstance(exc, BridgeError) else BridgeError(-99, "Local voice service failed")
            with self.lock:
                self.ready, self.failure = False, failure
                if self.current:
                    self.fail(self.current.end_req or self.current.req, failure)
                if self.pending:
                    self.fail(self.pending.req, failure)
                self.current, self.pending = None, None
        finally:
            if backend:
                try:
                    backend.close()
                except Exception:
                    with self.lock:
                        self.ready = False
                        # A secondary cleanup error must not hide the original
                        # initialization/worker fault needed for recovery.
                        if self.failure is None:self.failure = BridgeError(-31, "Local speech cleanup did not complete")
                        print(json.dumps(dict(phase='cleanup_failed',error_code=-31,original_error_code=self.failure.code)),flush=True)

    def shutdown(self):
        self.stop.set()
        with self.lock:
            if self.current:
                self.current.cancel.set()
            if self.pending:
                self.pending.cancel.set()
        self.wake.set()
        self.thread.join(100)
        if self.meter: self.meter.close()
        if self.thread.is_alive():
            raise BridgeError(-31, "Native worker did not stop within its bound")

    def raise_if_failed(self):
        """Let the process supervisor restart a dead native worker."""
        with self.lock:
            failure = self.failure
        if failure is not None:
            raise failure


def read_route(context):
    with context.socket(zmq.REQ, timeout_ms=3000, max_bytes=4096) as sock:
        sock.connect("ipc:///tmp/rpc.sys")
        sock.send_multipart([b"sql_select", b"serial_zmq_url"])
        route = sock.recv().decode("ascii")
    if not re.fullmatch(r"ipc:///tmp/llm/[0-9]{1,10}\.sock", route):
        raise BridgeError(-13, "The vendor UART callback route is not supported")
    return route


def runtime_check():
    # Report the failing prerequisite without exposing arbitrary native error
    # text, paths, settings, credentials or user speech. The original installer
    # collapsed library/transport exceptions to -99, making a second attempt
    # impossible to diagnose from its otherwise privacy-preserving report.
    stage, reason, service, context = "python", "unsupported_python", None, None
    try:
        if sys.version_info < (3, 8):
            raise BridgeError(-13, "Python 3.8 or newer is required")
        stage, reason = "platform", "unsupported_architecture"
        if platform.machine().lower() not in ("aarch64", "arm64"):
            raise BridgeError(-13, "This is not the ARM64 Module LLM runtime")
        stage, reason = "account", "root_required"
        if os.geteuid() != 0:
            raise BridgeError(-13, "The module service requires its local root account")
        stage, reason = "vendor-sockets", "vendor_service_unavailable"
        for service in ("sys", "audio", "asr", "melotts"):
            path = Path("/tmp/rpc." + service)
            if not path.exists() or not stat.S_ISSOCK(path.stat().st_mode):
                raise BridgeError(-13, "Required local %s service is unavailable" % service)
        service = None
        stage, reason = "zmq-library", "library_initialization_failed"
        library = zmq.load_library()
        stage, reason = "zmq-context", "context_initialization_failed"
        context = zmq.Context(library=library)
        stage, reason = "uart-route", "unexpected_exception"
        read_route(context)
    except Exception as exc:
        if stage == "zmq-library":
            if isinstance(exc, OSError):
                reason = "library_load_failed"
            elif isinstance(exc, AttributeError):
                reason = "library_symbol_missing"
        elif stage == "uart-route":
            if isinstance(exc, zmq.ZmqTimeout):
                reason = "rpc_timeout"
            elif isinstance(exc, zmq.ZmqError):
                reason = "rpc_failed"
            elif isinstance(exc, UnicodeError):
                reason = "route_decode_failed"
            elif isinstance(exc, BridgeError):
                reason = "route_invalid"
        allowed_types = {"BridgeError", "ZmqError", "ZmqTimeout", "OSError",
                         "FileNotFoundError", "PermissionError", "AttributeError",
                         "UnicodeDecodeError", "ValueError", "TypeError"}
        exception_type = type(exc).__name__
        diagnostic = {"stage": stage, "reason": reason,
                      "exception_type": exception_type if exception_type in allowed_types else "OtherError"}
        error_number = getattr(exc, "errno", None)
        if type(error_number) is int and -65535 <= error_number <= 65535:
            diagnostic["errno"] = error_number
        if service is not None:
            diagnostic["service"] = service
        error = exc if isinstance(exc, BridgeError) else BridgeError(-99, "Module bridge prerequisite check failed")
        error.diagnostic = diagnostic
        if error is exc:
            raise
        raise error from exc
    finally:
        if context is not None:
            context.close()
    return {"ok": True, "phase": "check", "protocol": PROTOCOL,
            "python": list(sys.version_info[:3]), "libzmq": True, "vendor_rpc": True,
            "asr_model": ASR_MODEL, "tts_model": TTS_MODEL}


def serve(state_dir):
    runtime_check()
    context = zmq.Context()
    route = read_route(context)
    output = queue.Queue(maxsize=128)
    def emit(destination, payload):
        try:
            output.put_nowait((destination, payload))
        except queue.Full as exc:
            raise BridgeError(-24, "Voice response queue is full") from exc
    server = context.socket(zmq.REP, timeout_ms=100, max_bytes=REQUEST_LIMIT + 128)
    server.bind("ipc:///tmp/rpc.musevoice")
    engine = Engine(emit, lambda: default_backend(state_dir=state_dir))
    from muse_memory_worker import MemoryWorker
    memory = MemoryWorker(emit, response, engine=engine)
    from muse_cloud_worker import CloudWorker,reply_poll_ms
    cloud = CloudWorker(emit,response,engine)
    stop = threading.Event()
    def terminate(*_):
        stop.set()
    signal.signal(signal.SIGTERM, terminate)
    signal.signal(signal.SIGINT, terminate)
    pushes = {}
    try:
        while not stop.is_set():
            if server.readable(reply_poll_ms(cloud)):
                try:
                    frames = server.recv_multipart(2)
                    if len(frames) != 2:
                        raise BridgeError(-2, "Invalid native RPC framing")
                    req = parse_request(frames[0], frames[1], route)
                except (BridgeError, ValueError):
                    # No request is reflected to an unverified callback route.
                    server.send(b"Invalid")
                else:
                    # The stock router must be released before any inference.
                    server.send(b"None")
                    if req["action"] == "cloud":
                        cloud.submit(req)
                    elif req["action"] == "memory":
                        with engine.lock:
                            authorized = engine.ready and (req["session"] == engine.session or (req["route"].startswith("ipc:///tmp/musevoice/client-") and req["data"].get("op") == "diagnostic"))
                        if authorized: memory.submit(req)
                        else: engine.fail(req, BridgeError(-21, "Archive waiting for device session"))
                    else: engine.submit(req)
            for _ in range(16):
                try:
                    destination, payload = output.get_nowait()
                except queue.Empty:
                    break
                if destination not in pushes:
                    if len(pushes) >= 8:
                        oldest = next(iter(pushes))
                        pushes.pop(oldest).close()
                    sock = context.socket(zmq.PUSH, timeout_ms=1000, max_bytes=8192)
                    sock.connect(destination)
                    pushes[destination] = sock
                pushes[destination].send(encode(payload) + b"\n")
            # Fatal initialization/worker errors exit for the systemd retry.
            # A recoverable operation error leaves the native worker healthy.
            engine.raise_if_failed()
    finally:
        cloud.close()
        memory.close()
        engine.shutdown()
        server.close()
        for sock in pushes.values():
            sock.close()
        context.close()


def _ping_once(timeout=3):
    context = zmq.Context()
    directory = Path("/tmp/musevoice")
    directory.mkdir(mode=0o700, exist_ok=True)
    route = "ipc://" + str(directory / ("client-" + uuid.uuid4().hex[:24] + ".sock"))
    replies = context.socket(zmq.PULL, timeout_ms=1000, max_bytes=8192)
    replies.bind(route)
    session, request_id = uuid.uuid4().hex[:8], "ping-" + uuid.uuid4().hex[:16]
    request = {"request_id": request_id, "work_id": SERVICE, "action": "inspect",
               "object": "musevoice.v1", "data": {"generation": 0, "session": session}}
    try:
        with context.socket(zmq.REQ, timeout_ms=3000, max_bytes=REQUEST_LIMIT) as sock:
            sock.connect("ipc:///tmp/rpc.musevoice")
            sock.send_multipart([b"inspect", pack_pair(route.encode(), encode(request))])
            if sock.recv() != b"None":
                raise BridgeError(-11, "Installed bridge rejected the local hello")
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if not replies.readable(100):
                continue
            reply = json.loads(replies.recv())
            data, error = reply.get("data", {}), reply.get("error", {})
            if reply.get("request_id") != request_id or reply.get("work_id") != SERVICE or data.get("session") != session or data.get("generation") != 0:
                continue
            if error.get("code") != 0:
                raise BridgeError(-11, "Installed bridge is not ready")
            if data.get("event") == "ready" and data.get("protocol") == 1 and data.get("ready") is True and data.get("asr_model") == ASR_MODEL and data.get("tts_model") == TTS_MODEL and set(CAPABILITIES) <= set(data.get("capabilities", [])):
                return {"ok": True, "phase": "ping", "protocol": PROTOCOL, "ready": True,
                        "asr_model": ASR_MODEL, "tts_model": TTS_MODEL, "capabilities": CAPABILITIES}
        raise BridgeError(-12, "Installed bridge readiness timed out")
    finally:
        replies.close()
        context.close()
        try:
            Path(route[6:]).unlink()
        except FileNotFoundError:
            pass


def ping(timeout=100):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            return _ping_once(timeout=min(3, max(.1, deadline-time.monotonic())))
        except BridgeError:
            time.sleep(.2)
    raise BridgeError(-12, "Installed bridge readiness timed out")


def selftest(state_dir, microphone):
    backend = Backend(state_dir=state_dir)
    try:
        backend.initialize()
        cancel = threading.Event()
        result = backend.speak("Hello care rick. Muse local voice is ready.", 70, cancel)
        output = {"ok": True, "phase": "selftest", "protocol": PROTOCOL,
                  "models_loaded": True, "pcm_generated": result["pcm_bytes"] > 0,
                  "playback_joined": result["joined"], "microphone_final": False,
                  "physical_hearing_requires_user_confirmation": True}
        if microphone:
            end = threading.Event()
            timer = None
            def listening():
                nonlocal timer
                print("LISTENING: Say 'Hello Muse' now, then remain quiet.", file=sys.stderr, flush=True)
                timer = threading.Timer(8, end.set)
                timer.start()
            try:
                text = backend.listen(cancel, end, listening, max_seconds=20)
            finally:
                if timer:
                    timer.cancel()
            print("RECOGNIZED: " + text, file=sys.stderr, flush=True)
            output.update({"microphone_final": bool(text), "microphone_text_bytes": len(text.encode("utf-8"))})
        return output
    finally:
        backend.close()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    for flag in ("check", "serve", "takeover", "selftest", "ping", "cleanup"):
        group.add_argument("--" + flag, action="store_true")
    parser.add_argument("--microphone", action="store_true")
    parser.add_argument("--snapshot")
    parser.add_argument("--state-dir", default="/var/lib/muse-core2-bridge")
    args = parser.parse_args(argv)
    lock = None
    try:
        if args.check:
            result = runtime_check()
        elif args.ping:
            result = ping()
        else:
            runtime_check()
            Path("/run/muse-core2-bridge").mkdir(mode=0o700, parents=True, exist_ok=True)
            lock = open("/run/muse-core2-bridge/owner.lock", "a")
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as exc:
                raise BridgeError(-22, "Another local bridge or preflight owns the module") from exc
            if args.serve:
                serve(args.state_dir)
                return 0
            if args.cleanup:
                backend = Backend(state_dir=args.state_dir)
                try:
                    backend.cleanup_previous()
                    result = {"ok": True, "phase": "cleanup", "protocol": PROTOCOL}
                finally:
                    backend.close()
            elif args.takeover:
                if not args.snapshot:
                    raise BridgeError(-2, "Takeover requires a private snapshot path")
                backend = Backend(state_dir=args.state_dir)
                try:
                    result = {"ok": True, "phase": "takeover", "protocol": PROTOCOL,
                              **backend.takeover(args.snapshot)}
                finally:
                    backend.close()
            else:
                result = selftest(args.state_dir, args.microphone)
        print(json.dumps(result, sort_keys=True), flush=True)
        return 0
    except Exception as exc:
        error = exc if isinstance(exc, BridgeError) else BridgeError(-99, "Module bridge operation failed")
        phase = next((name for name in ("check", "serve", "takeover", "selftest", "ping", "cleanup") if getattr(args, name)), "unknown")
        result = {"ok": False, "phase": phase, "protocol": PROTOCOL,
                  "error_code": error.code, "error": error.message}
        diagnostic = getattr(error, "diagnostic", None)
        if diagnostic is not None:
            result["diagnostic"] = diagnostic
        print(json.dumps(result, sort_keys=True), flush=True)
        return 1
    finally:
        if lock:
            lock.close()


if __name__ == "__main__":
    sys.exit(main())
