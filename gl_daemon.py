#!/usr/bin/env python3
"""
gl_daemon.py — host-native GL execution daemon for Pokémon LE / Spike 3.

Phase 2a (this revision): native EGL surfaceless context + actual command
execution via PyOpenGL. Implemented opcodes:
  GL_VIEWPORT, GL_CLEAR_COLOR, GL_CLEAR, GL_GET_ERROR, GL_FINISH, GL_READ_PIXELS.

Single-connection assumption: one client (the shim) at a time. EGL context
is bound to whichever Session thread is active.

Wire format: see gl_proto.h / gl_proto.py.
"""
import argparse
import ctypes
import ctypes.util
import logging
import socket
import struct
import sys
import threading

import gl_proto as P

EGL_PLATFORM_SURFACELESS_MESA = 0x31DD


def _load_libegl():
    lib = ctypes.CDLL(ctypes.util.find_library("EGL") or "libEGL.so.1",
                      mode=ctypes.RTLD_GLOBAL)
    lib.eglGetPlatformDisplay.argtypes = [ctypes.c_uint, ctypes.c_void_p, ctypes.c_void_p]
    lib.eglGetPlatformDisplay.restype = ctypes.c_void_p
    return lib


class Renderer:
    """Native EGL + GLES context. Created in the main thread; eglMakeCurrent
    is deferred to bind() so the Session thread that calls it owns the
    context. Single-Session lifecycle for now."""

    def __init__(self, width: int = 1920, height: int = 1080):
        self.log = logging.getLogger("renderer")
        self.width = width
        self.height = height
        self._libEGL = _load_libegl()
        from OpenGL import EGL as E
        self._E = E

        self.display = self._libEGL.eglGetPlatformDisplay(
            EGL_PLATFORM_SURFACELESS_MESA, None, None)
        if not self.display:
            raise RuntimeError(f"eglGetPlatformDisplay returned NULL (err=0x{E.eglGetError():x})")
        major = ctypes.c_int(); minor = ctypes.c_int()
        if not E.eglInitialize(self.display, ctypes.byref(major), ctypes.byref(minor)):
            raise RuntimeError(f"eglInitialize failed (err=0x{E.eglGetError():x})")
        self.log.info("EGL %d.%d  vendor=%s",
                      major.value, minor.value,
                      ctypes.string_at(E.eglQueryString(self.display, E.EGL_VENDOR)).decode())

        if not E.eglBindAPI(E.EGL_OPENGL_ES_API):
            raise RuntimeError(f"eglBindAPI(GLES) failed (err=0x{E.eglGetError():x})")

        attribs = (ctypes.c_int * 17)(
            E.EGL_SURFACE_TYPE,    E.EGL_PBUFFER_BIT,
            E.EGL_RED_SIZE,        8,
            E.EGL_GREEN_SIZE,      8,
            E.EGL_BLUE_SIZE,       8,
            E.EGL_ALPHA_SIZE,      8,
            E.EGL_DEPTH_SIZE,      24,
            E.EGL_STENCIL_SIZE,    8,
            E.EGL_RENDERABLE_TYPE, E.EGL_OPENGL_ES2_BIT,
            E.EGL_NONE,
        )
        cfg = (E.EGLConfig * 1)()
        n = ctypes.c_int()
        if not E.eglChooseConfig(self.display, attribs, cfg, 1, ctypes.byref(n)) or n.value < 1:
            raise RuntimeError(f"eglChooseConfig found 0 configs (err=0x{E.eglGetError():x})")
        self.config = cfg[0]

        ctx_attribs = (ctypes.c_int * 3)(
            E.EGL_CONTEXT_CLIENT_VERSION, 2,
            E.EGL_NONE,
        )
        self.context = E.eglCreateContext(self.display, self.config,
                                          E.EGL_NO_CONTEXT, ctx_attribs)
        if not self.context:
            raise RuntimeError(f"eglCreateContext failed (err=0x{E.eglGetError():x})")

        pbuf_attribs = (ctypes.c_int * 5)(
            E.EGL_WIDTH,  self.width,
            E.EGL_HEIGHT, self.height,
            E.EGL_NONE,
        )
        self.surface = E.eglCreatePbufferSurface(self.display, self.config, pbuf_attribs)
        if not self.surface:
            raise RuntimeError(f"eglCreatePbufferSurface failed (err=0x{E.eglGetError():x})")

        self.log.info("renderer ready: %dx%d pbuffer, GLES2 context", self.width, self.height)

    def bind(self) -> None:
        """Make the context current on the calling thread."""
        E = self._E
        if not E.eglMakeCurrent(self.display, self.surface, self.surface, self.context):
            raise RuntimeError(f"eglMakeCurrent failed (err=0x{E.eglGetError():x})")
        from OpenGL import GL
        self._GL = GL
        v = GL.glGetString(GL.GL_VERSION)
        r = GL.glGetString(GL.GL_RENDERER)
        self.log.info("context current: GL_VERSION=%s GL_RENDERER=%s",
                      v.decode() if v else "?", r.decode() if r else "?")

    def execute_clear(self, mask: int) -> None:
        self._GL.glClear(mask)

    def execute_clear_color(self, r: float, g: float, b: float, a: float) -> None:
        self._GL.glClearColor(r, g, b, a)

    def execute_viewport(self, x: int, y: int, w: int, h: int) -> None:
        self._GL.glViewport(x, y, w, h)

    def execute_finish(self) -> None:
        self._GL.glFinish()

    def execute_get_error(self) -> int:
        return int(self._GL.glGetError())

    def execute_read_pixels(self, x: int, y: int, w: int, h: int,
                            fmt: int, typ: int) -> bytes:
        return bytes(self._GL.glReadPixels(x, y, w, h, fmt, typ))


def read_all(sock: socket.socket, n: int) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return bytes(buf)
        buf.extend(chunk)
    return bytes(buf)


def send_reply(sock: socket.socket, op: int, payload: bytes = b"") -> None:
    sock.sendall(P.pack_header(op, 0, len(payload)) + payload)


class Session:
    _next_id = 1
    _next_id_lock = threading.Lock()

    def __init__(self, sock: socket.socket, renderer: Renderer) -> None:
        with Session._next_id_lock:
            self.id = Session._next_id
            Session._next_id += 1
        self.sock = sock
        self.renderer = renderer
        self.log = logging.getLogger(f"conn-{self.id}")
        self.log.info("connected")

    def serve(self) -> None:
        try:
            self.renderer.bind()
        except Exception as e:
            self.log.error("renderer bind failed: %s", e)
            return
        try:
            while True:
                hdr = read_all(self.sock, P.HEADER_LEN)
                if len(hdr) < P.HEADER_LEN:
                    self.log.info("disconnected: peer closed")
                    return
                op, flags, plen = P.unpack_header(hdr)
                payload = read_all(self.sock, plen) if plen else b""
                if len(payload) < plen:
                    self.log.warning("short payload (got %d of %d)", len(payload), plen)
                    return
                self.handle(op, flags, payload)
        except OSError as e:
            self.log.info("disconnected: %s", e)
        finally:
            try:
                self.sock.close()
            except OSError:
                pass

    def handle(self, op: int, flags: int, payload: bytes) -> None:
        sync = bool(flags & P.GL_FLAG_SYNC)
        if op == P.GL_OP_PING:
            self.log.info("PING (sync=%s)", sync)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)

        elif op == P.GL_OP_GL_VIEWPORT:
            x, y, w, h = struct.unpack(P.VIEWPORT_FMT, payload)
            self.renderer.execute_viewport(x, y, w, h)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)

        elif op == P.GL_OP_GL_CLEAR_COLOR:
            r, g, b, a = struct.unpack(P.CLEAR_COLOR_FMT, payload)
            self.renderer.execute_clear_color(r, g, b, a)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)

        elif op == P.GL_OP_GL_CLEAR:
            (mask,) = struct.unpack(P.CLEAR_FMT, payload)
            self.renderer.execute_clear(mask)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)

        elif op == P.GL_OP_GL_GET_ERROR:
            err = self.renderer.execute_get_error()
            if sync: send_reply(self.sock, P.GL_OP_REP_VALUE,
                                struct.pack(P.REP_VALUE_FMT, err))

        elif op == P.GL_OP_GL_FINISH:
            self.renderer.execute_finish()
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)

        elif op == P.GL_OP_GL_READ_PIXELS:
            x, y, w, h, fmt, typ = struct.unpack(P.READ_PIXELS_FMT, payload)
            data = self.renderer.execute_read_pixels(x, y, w, h, fmt, typ)
            hdr = struct.pack(P.REP_PIXELS_FMT, w, h, fmt, typ, len(data))
            send_reply(self.sock, P.GL_OP_REP_PIXELS, hdr + data)

        else:
            self.log.warning("unknown op 0x%04x flags=0x%04x payload=%db",
                             op, flags, len(payload))
            if sync: send_reply(self.sock, P.GL_OP_REP_ERR,
                                struct.pack(P.REP_ERR_FMT, -1, 0))


def serve(addr_name: str, renderer: Renderer) -> int:
    log = logging.getLogger("daemon")
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    addr = "\x00" + addr_name
    try:
        srv.bind(addr)
    except OSError as e:
        log.error("bind(@%s) failed: %s", addr_name, e)
        return 1
    srv.listen(1)
    log.info("listening on @%s (Phase 2a: native GL execution)", addr_name)

    try:
        while True:
            client, _ = srv.accept()
            sess = Session(client, renderer)
            sess.serve()
    except KeyboardInterrupt:
        log.info("interrupt — shutting down")
    finally:
        try:
            srv.close()
        except OSError:
            pass
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="gl_daemon — Atomic Horizon Phase 2a")
    ap.add_argument("--socket", default=P.GL_PROTO_SOCKET_NAME)
    ap.add_argument("--width",  type=int, default=1920)
    ap.add_argument("--height", type=int, default=1080)
    args = ap.parse_args()

    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s [%(name)s] %(message)s",
        datefmt="%H:%M:%S",
    )
    try:
        renderer = Renderer(width=args.width, height=args.height)
    except Exception as e:
        logging.error("renderer init failed: %s", e)
        return 1
    return serve(args.socket, renderer)


if __name__ == "__main__":
    sys.exit(main())
