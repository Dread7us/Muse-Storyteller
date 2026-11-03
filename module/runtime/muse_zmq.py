"""Small stdlib-only binding to the system libzmq ABI used by StackFlow.

Sockets are deliberately thread-affine. No TCP endpoint is opened by this
module. Every caller must impose its own application message limit.
"""
import ctypes as C
import ctypes.util
import errno

PUB, SUB, REQ, REP, PULL, PUSH, XPUB = 1, 2, 3, 4, 7, 8, 9
SNDMORE, POLLIN = 2, 1
SUBSCRIBE, RCVMORE, LINGER, RCVTIMEO, SNDTIMEO = 6, 13, 17, 27, 28
MAXMSGSIZE = 22
# Official Module LLM image layout. Its bash_env.sh adds this directory to
# LD_LIBRARY_PATH; non-interactive ADB and systemd may not inherit that setting.
# Use the already-installed library directly; do not edit the vendor linker
# configuration, import arbitrary shell environment, or download a replacement.
# https://github.com/m5stack/LLM_buildroot-external-m5stack/tree/a9499a15ae0b4a079b12c261f0c20f38a82185b6
VENDOR_ZMQ_PATH = "/usr/local/m5stack/lib/libzmq.so.5"


class ZmqError(RuntimeError):
    def __init__(self, message, error_number=None):
        super().__init__(message)
        self.errno = error_number


class ZmqTimeout(ZmqError):
    pass


class _Message(C.Union):
    _fields_ = [("opaque", C.c_ubyte * 64), ("alignment", C.c_void_p)]


class _PollItem(C.Structure):
    _fields_ = [("socket", C.c_void_p), ("fd", C.c_int),
                ("events", C.c_short), ("revents", C.c_short)]


def load_library():
    try:
        discovered = ctypes.util.find_library("zmq")
    except OSError:
        discovered = None
    candidates = []
    for path in (discovered, "libzmq.so.5", VENDOR_ZMQ_PATH):
        if path and path not in candidates:
            candidates.append(path)
    last_error = None
    for path in candidates:
        try:
            lib = C.CDLL(path, use_errno=True)
            break
        except OSError as exc:
            last_error = exc
    else:
        raise last_error
    signatures = {
        "zmq_ctx_new": (C.c_void_p, []),
        "zmq_ctx_term": (C.c_int, [C.c_void_p]),
        "zmq_socket": (C.c_void_p, [C.c_void_p, C.c_int]),
        "zmq_close": (C.c_int, [C.c_void_p]),
        "zmq_bind": (C.c_int, [C.c_void_p, C.c_char_p]),
        "zmq_connect": (C.c_int, [C.c_void_p, C.c_char_p]),
        "zmq_setsockopt": (C.c_int, [C.c_void_p, C.c_int, C.c_void_p, C.c_size_t]),
        "zmq_getsockopt": (C.c_int, [C.c_void_p, C.c_int, C.c_void_p, C.POINTER(C.c_size_t)]),
        "zmq_send": (C.c_int, [C.c_void_p, C.c_void_p, C.c_size_t, C.c_int]),
        "zmq_msg_init": (C.c_int, [C.POINTER(_Message)]),
        "zmq_msg_recv": (C.c_int, [C.POINTER(_Message), C.c_void_p, C.c_int]),
        "zmq_msg_data": (C.c_void_p, [C.POINTER(_Message)]),
        "zmq_msg_size": (C.c_size_t, [C.POINTER(_Message)]),
        "zmq_msg_close": (C.c_int, [C.POINTER(_Message)]),
        "zmq_poll": (C.c_int, [C.POINTER(_PollItem), C.c_int, C.c_long]),
        "zmq_errno": (C.c_int, []),
        "zmq_strerror": (C.c_char_p, [C.c_int]),
    }
    for name, (restype, argtypes) in signatures.items():
        fn = getattr(lib, name)
        fn.restype, fn.argtypes = restype, argtypes
    return lib


class Context:
    def __init__(self, library=None):
        self.lib = library if library is not None else load_library()
        self.handle = self.lib.zmq_ctx_new()
        if not self.handle:
            raise ZmqError("Unable to create local ZeroMQ context", self.lib.zmq_errno())

    def check(self, result):
        if result < 0:
            code = self.lib.zmq_errno()
            if code == errno.EAGAIN:
                raise ZmqTimeout("Local ZeroMQ operation timed out", code)
            raise ZmqError("Local ZeroMQ failure %d" % code, code)
        return result

    def socket(self, kind, timeout_ms=3000, max_bytes=6 * 1024 * 1024):
        return Socket(self, kind, timeout_ms, max_bytes)

    def close(self):
        if self.handle:
            self.lib.zmq_ctx_term(self.handle)
            self.handle = None


class Socket:
    def __init__(self, context, kind, timeout_ms, max_bytes):
        self.context, self.lib = context, context.lib
        self.handle = self.lib.zmq_socket(context.handle, kind)
        if not self.handle:
            raise ZmqError("Unable to create local ZeroMQ socket", self.lib.zmq_errno())
        self.max_bytes = max_bytes
        try:
            self.option(LINGER, 0)
            self.option(RCVTIMEO, timeout_ms)
            self.option(SNDTIMEO, timeout_ms)
            self.option(MAXMSGSIZE, max_bytes, C.c_int64)
            if kind == SUB:
                self.context.check(self.lib.zmq_setsockopt(self.handle, SUBSCRIBE, b"", 0))
        except BaseException:
            self.close()
            raise

    def option(self, name, value, ctype=C.c_int):
        item = ctype(value)
        self.context.check(self.lib.zmq_setsockopt(self.handle, name, C.byref(item), C.sizeof(item)))

    def _endpoint(self, value):
        if not value.startswith("ipc:///") or len(value.encode()) > 106:
            raise ValueError("Only bounded absolute local IPC endpoints are supported")
        return value.encode("utf-8")

    def bind(self, endpoint):
        self.context.check(self.lib.zmq_bind(self.handle, self._endpoint(endpoint)))

    def connect(self, endpoint):
        self.context.check(self.lib.zmq_connect(self.handle, self._endpoint(endpoint)))

    def send(self, payload, more=False):
        if not isinstance(payload, bytes) or len(payload) > self.max_bytes:
            raise ValueError("Invalid local IPC payload")
        self.context.check(self.lib.zmq_send(self.handle, payload, len(payload), SNDMORE if more else 0))

    def send_multipart(self, parts):
        for index, payload in enumerate(parts):
            self.send(payload, index < len(parts) - 1)

    def recv(self):
        msg = _Message()
        self.context.check(self.lib.zmq_msg_init(C.byref(msg)))
        try:
            self.context.check(self.lib.zmq_msg_recv(C.byref(msg), self.handle, 0))
            size = self.lib.zmq_msg_size(C.byref(msg))
            if size > self.max_bytes:
                raise ValueError("Local IPC message exceeds bound")
            return C.string_at(self.lib.zmq_msg_data(C.byref(msg)), size)
        finally:
            self.lib.zmq_msg_close(C.byref(msg))

    def recv_multipart(self, max_parts=2):
        parts = []
        while True:
            parts.append(self.recv())
            more, size = C.c_int(), C.c_size_t(C.sizeof(C.c_int))
            self.context.check(self.lib.zmq_getsockopt(self.handle, RCVMORE, C.byref(more), C.byref(size)))
            if not more.value:
                return parts
            if len(parts) >= max_parts:
                raise ValueError("Too many local IPC frames")

    def readable(self, timeout_ms=0):
        item = _PollItem(self.handle, 0, POLLIN, 0)
        result = self.lib.zmq_poll(C.byref(item), 1, timeout_ms)
        if result < 0 and self.lib.zmq_errno() == errno.EINTR:
            return False
        self.context.check(result)
        return bool(item.revents & POLLIN)

    def close(self):
        if self.handle:
            self.lib.zmq_close(self.handle)
            self.handle = None

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()
