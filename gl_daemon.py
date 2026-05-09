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

    def gen_buffers(self, n: int) -> list:
        return [int(i) for i in self._GL.glGenBuffers(n)] if n != 1 else [int(self._GL.glGenBuffers(1))]

    def delete_buffers(self, ids: list) -> None:
        self._GL.glDeleteBuffers(len(ids), ids)

    def bind_buffer(self, target: int, buf_id: int) -> None:
        self._GL.glBindBuffer(target, buf_id)

    def buffer_data(self, target: int, size: int, usage: int, data: bytes) -> None:
        self._GL.glBufferData(target, size, data if data else None, usage)

    def gen_vertex_arrays(self, n: int) -> list:
        return [int(i) for i in self._GL.glGenVertexArrays(n)] if n != 1 else [int(self._GL.glGenVertexArrays(1))]

    def delete_vertex_arrays(self, ids: list) -> None:
        self._GL.glDeleteVertexArrays(len(ids), ids)

    def bind_vertex_array(self, vao_id: int) -> None:
        self._GL.glBindVertexArray(vao_id)

    def create_shader(self, shader_type: int) -> int:
        return int(self._GL.glCreateShader(shader_type))

    def delete_shader(self, shader_id: int) -> None:
        self._GL.glDeleteShader(shader_id)

    def shader_source(self, shader_id: int, source: bytes) -> None:
        self._GL.glShaderSource(shader_id, [source.decode("utf-8", "replace")])

    def compile_shader(self, shader_id: int) -> None:
        self._GL.glCompileShader(shader_id)

    def get_shader_iv(self, shader_id: int, pname: int) -> int:
        return int(self._GL.glGetShaderiv(shader_id, pname))

    def create_program(self) -> int:
        return int(self._GL.glCreateProgram())

    def delete_program(self, prog: int) -> None:
        self._GL.glDeleteProgram(prog)

    def attach_shader(self, prog: int, shader: int) -> None:
        self._GL.glAttachShader(prog, shader)

    def link_program(self, prog: int) -> None:
        self._GL.glLinkProgram(prog)

    def get_program_iv(self, prog: int, pname: int) -> int:
        return int(self._GL.glGetProgramiv(prog, pname))

    def use_program(self, prog: int) -> None:
        self._GL.glUseProgram(prog)

    def get_attrib_location(self, prog: int, name: bytes) -> int:
        return int(self._GL.glGetAttribLocation(prog, name.decode("utf-8", "replace")))

    def get_uniform_location(self, prog: int, name: bytes) -> int:
        return int(self._GL.glGetUniformLocation(prog, name.decode("utf-8", "replace")))

    def vertex_attrib_pointer(self, idx: int, size: int, typ: int,
                              normalized: int, stride: int, offset: int) -> None:
        self._GL.glVertexAttribPointer(idx, size, typ, bool(normalized), stride,
                                       ctypes.c_void_p(offset))

    def enable_vertex_attrib_array(self, idx: int) -> None:
        self._GL.glEnableVertexAttribArray(idx)

    def uniform_1i(self, location: int, value: int) -> None:
        self._GL.glUniform1i(location, value)

    def uniform_4f(self, location: int, v0: float, v1: float, v2: float, v3: float) -> None:
        self._GL.glUniform4f(location, v0, v1, v2, v3)

    def uniform_matrix4fv(self, location: int, count: int, transpose: int, data: bytes) -> None:
        import numpy as np
        floats = np.frombuffer(data, dtype=np.float32, count=count*16).reshape(count, 4, 4)
        self._GL.glUniformMatrix4fv(location, count, bool(transpose), floats)

    def draw_arrays(self, mode: int, first: int, count: int) -> None:
        self._GL.glDrawArrays(mode, first, count)


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

        elif op == P.GL_OP_GL_GEN_BUFFERS:
            (n,) = struct.unpack(P.COUNT_FMT, payload)
            ids = self.renderer.gen_buffers(n)
            send_reply(self.sock, P.GL_OP_REP_IDS,
                       struct.pack(P.REP_IDS_FMT, len(ids)) +
                       b''.join(struct.pack("<I", i) for i in ids))
        elif op == P.GL_OP_GL_DELETE_BUFFERS:
            (n,) = struct.unpack(P.COUNT_FMT, payload[:4])
            ids = list(struct.unpack(f"<{n}I", payload[4:4+4*n]))
            self.renderer.delete_buffers(ids)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_BIND_BUFFER:
            target, bid = struct.unpack(P.BIND_FMT, payload)
            self.renderer.bind_buffer(target, bid)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_BUFFER_DATA:
            target, size, usage = struct.unpack(P.BUFFER_DATA_FMT, payload[:12])
            self.renderer.buffer_data(target, size, usage, payload[12:12+size])
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_GEN_VERTEX_ARRAYS:
            (n,) = struct.unpack(P.COUNT_FMT, payload)
            ids = self.renderer.gen_vertex_arrays(n)
            send_reply(self.sock, P.GL_OP_REP_IDS,
                       struct.pack(P.REP_IDS_FMT, len(ids)) +
                       b''.join(struct.pack("<I", i) for i in ids))
        elif op == P.GL_OP_GL_DELETE_VERTEX_ARRAYS:
            (n,) = struct.unpack(P.COUNT_FMT, payload[:4])
            ids = list(struct.unpack(f"<{n}I", payload[4:4+4*n]))
            self.renderer.delete_vertex_arrays(ids)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_BIND_VERTEX_ARRAY:
            (vid,) = struct.unpack(P.ID_FMT, payload)
            self.renderer.bind_vertex_array(vid)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)

        elif op == P.GL_OP_GL_CREATE_SHADER:
            (st,) = struct.unpack(P.CREATE_SHADER_FMT, payload)
            sid = self.renderer.create_shader(st)
            send_reply(self.sock, P.GL_OP_REP_VALUE, struct.pack(P.REP_VALUE_FMT, sid))
        elif op == P.GL_OP_GL_DELETE_SHADER:
            (sid,) = struct.unpack(P.ID_FMT, payload)
            self.renderer.delete_shader(sid)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_SHADER_SOURCE:
            sid, slen = struct.unpack(P.SHADER_SOURCE_FMT, payload[:8])
            self.renderer.shader_source(sid, payload[8:8+slen])
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_COMPILE_SHADER:
            (sid,) = struct.unpack(P.ID_FMT, payload)
            self.renderer.compile_shader(sid)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_GET_SHADER_IV:
            sid, pname = struct.unpack(P.GET_IV_FMT, payload)
            v = self.renderer.get_shader_iv(sid, pname)
            send_reply(self.sock, P.GL_OP_REP_VALUE, struct.pack(P.REP_VALUE_FMT, v))

        elif op == P.GL_OP_GL_CREATE_PROGRAM:
            pid = self.renderer.create_program()
            send_reply(self.sock, P.GL_OP_REP_VALUE, struct.pack(P.REP_VALUE_FMT, pid))
        elif op == P.GL_OP_GL_DELETE_PROGRAM:
            (pid,) = struct.unpack(P.ID_FMT, payload)
            self.renderer.delete_program(pid)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_ATTACH_SHADER:
            pid, sid = struct.unpack(P.TWO_ID_FMT, payload)
            self.renderer.attach_shader(pid, sid)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_LINK_PROGRAM:
            (pid,) = struct.unpack(P.ID_FMT, payload)
            self.renderer.link_program(pid)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_GET_PROGRAM_IV:
            pid, pname = struct.unpack(P.GET_IV_FMT, payload)
            v = self.renderer.get_program_iv(pid, pname)
            send_reply(self.sock, P.GL_OP_REP_VALUE, struct.pack(P.REP_VALUE_FMT, v))
        elif op == P.GL_OP_GL_USE_PROGRAM:
            (pid,) = struct.unpack(P.ID_FMT, payload)
            self.renderer.use_program(pid)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)

        elif op == P.GL_OP_GL_GET_ATTRIB_LOCATION:
            pid, nlen = struct.unpack(P.GET_LOC_FMT, payload[:8])
            loc = self.renderer.get_attrib_location(pid, payload[8:8+nlen])
            send_reply(self.sock, P.GL_OP_REP_LOC, struct.pack(P.REP_LOC_FMT, loc))
        elif op == P.GL_OP_GL_GET_UNIFORM_LOCATION:
            pid, nlen = struct.unpack(P.GET_LOC_FMT, payload[:8])
            loc = self.renderer.get_uniform_location(pid, payload[8:8+nlen])
            send_reply(self.sock, P.GL_OP_REP_LOC, struct.pack(P.REP_LOC_FMT, loc))
        elif op == P.GL_OP_GL_VERTEX_ATTRIB_PTR:
            idx, size, typ_, norm, stride, offset = struct.unpack(P.VAP_FMT, payload)
            self.renderer.vertex_attrib_pointer(idx, size, typ_, norm, stride, offset)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_ENABLE_VERTEX_ATTRIB:
            (idx,) = struct.unpack(P.ID_FMT, payload)
            self.renderer.enable_vertex_attrib_array(idx)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_UNIFORM_1I:
            loc, val = struct.unpack(P.UNIFORM_1I_FMT, payload)
            self.renderer.uniform_1i(loc, val)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_UNIFORM_4F:
            loc, v0, v1, v2, v3 = struct.unpack(P.UNIFORM_4F_FMT, payload)
            self.renderer.uniform_4f(loc, v0, v1, v2, v3)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_UNIFORM_MATRIX4FV:
            loc, count, transp = struct.unpack(P.UNIFORM_MAT4_FMT, payload[:12])
            self.renderer.uniform_matrix4fv(loc, count, transp, payload[12:])
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)

        elif op == P.GL_OP_GL_DRAW_ARRAYS:
            mode, first, count = struct.unpack(P.DRAW_ARRAYS_FMT, payload)
            self.renderer.draw_arrays(mode, first, count)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)

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
