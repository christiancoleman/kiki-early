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


def _load_libgles_raw():
    """Load libGLESv2 with raw ctypes bindings for functions where PyOpenGL's
    Python-side converters misbehave (compressed textures: NumberHandler vs
    ArrayHandler bug on the imageSize parameter)."""
    lib = ctypes.CDLL(ctypes.util.find_library("GLESv2") or "libGLESv2.so.2",
                      mode=ctypes.RTLD_GLOBAL)
    lib.glCompressedTexImage2D.argtypes = [
        ctypes.c_uint, ctypes.c_int, ctypes.c_uint,
        ctypes.c_int, ctypes.c_int, ctypes.c_int,
        ctypes.c_int, ctypes.c_void_p,
    ]
    lib.glCompressedTexImage2D.restype = None
    lib.glCompressedTexSubImage2D.argtypes = [
        ctypes.c_uint, ctypes.c_int, ctypes.c_int, ctypes.c_int,
        ctypes.c_int, ctypes.c_int, ctypes.c_uint,
        ctypes.c_int, ctypes.c_void_p,
    ]
    lib.glCompressedTexSubImage2D.restype = None
    return lib


class Renderer:
    """Native EGL + GLES context. Created in the main thread; eglMakeCurrent
    is deferred to bind() so the Session thread that calls it owns the
    context. Single-Session lifecycle for now."""

    def __init__(self, width: int = 1920, height: int = 1080):
        import os
        self.log = logging.getLogger("renderer")
        self.width = width
        self.height = height
        self._first_bind = True
        self._draw_count = 0
        self._test_pattern = os.environ.get("SPIKE3_VIDEO_TESTPATTERN", "0") == "1"
        if self._test_pattern:
            self.log.info("SPIKE3_VIDEO_TESTPATTERN=1 — RGBA uploads >=256x256 will be replaced with checkerboard")
        self._force_magenta = os.environ.get("SPIKE3_FORCE_MAGENTA_CLEAR", "0") == "1"
        if self._force_magenta:
            self.log.info("SPIKE3_FORCE_MAGENTA_CLEAR=1 — glClearColor overridden to (1,0,1,1) magenta")
        self._disable_blend = os.environ.get("SPIKE3_DISABLE_BLEND", "0") == "1"
        if self._disable_blend:
            self.log.info("SPIKE3_DISABLE_BLEND=1 — glEnable(GL_BLEND) silently ignored")
        self._tracked = {k: set() for k in
                         ("programs", "shaders", "buffers", "vaos",
                          "textures", "framebuffers")}
        self._libEGL = _load_libegl()
        self._libGLESv2 = _load_libgles_raw()
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

        EGL_OPENGL_ES3_BIT = 0x00000040
        attribs = (ctypes.c_int * 17)(
            E.EGL_SURFACE_TYPE,    E.EGL_PBUFFER_BIT,
            E.EGL_RED_SIZE,        8,
            E.EGL_GREEN_SIZE,      8,
            E.EGL_BLUE_SIZE,       8,
            E.EGL_ALPHA_SIZE,      8,
            E.EGL_DEPTH_SIZE,      24,
            E.EGL_STENCIL_SIZE,    8,
            E.EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            E.EGL_NONE,
        )
        cfg = (E.EGLConfig * 1)()
        n = ctypes.c_int()
        if not E.eglChooseConfig(self.display, attribs, cfg, 1, ctypes.byref(n)) or n.value < 1:
            raise RuntimeError(f"eglChooseConfig found 0 configs (err=0x{E.eglGetError():x})")
        self.config = cfg[0]

        ctx_attribs = (ctypes.c_int * 3)(
            E.EGL_CONTEXT_CLIENT_VERSION, 3,
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
        """Make the context current on the calling thread. On every bind
        after the first, scrub all GL objects created by the prior session so
        the new client starts from a clean slate (the EGL context is shared
        across Sessions; PyOpenGL's GL state is process-global)."""
        E = self._E
        if not E.eglMakeCurrent(self.display, self.surface, self.surface, self.context):
            raise RuntimeError(f"eglMakeCurrent failed (err=0x{E.eglGetError():x})")
        from OpenGL import GL
        self._GL = GL
        if self._first_bind:
            v = GL.glGetString(GL.GL_VERSION)
            r = GL.glGetString(GL.GL_RENDERER)
            self.log.info("context current: GL_VERSION=%s GL_RENDERER=%s",
                          v.decode() if v else "?", r.decode() if r else "?")
            self._first_bind = False
        else:
            self.reset()

    def reset(self) -> None:
        GL = self._GL
        GL.glUseProgram(0)
        GL.glBindBuffer(0x8892, 0)
        GL.glBindBuffer(0x8893, 0)
        GL.glBindFramebuffer(0x8D40, 0)
        GL.glBindTexture(0x0DE1, 0)
        for prog in list(self._tracked["programs"]):     GL.glDeleteProgram(prog)
        for shd  in list(self._tracked["shaders"]):      GL.glDeleteShader(shd)
        if self._tracked["buffers"]:
            ids = list(self._tracked["buffers"])
            GL.glDeleteBuffers(len(ids), ids)
        if self._tracked["vaos"]:
            ids = list(self._tracked["vaos"])
            GL.glDeleteVertexArrays(len(ids), ids)
        if self._tracked["textures"]:
            ids = list(self._tracked["textures"])
            GL.glDeleteTextures(ids)
        if self._tracked["framebuffers"]:
            ids = list(self._tracked["framebuffers"])
            GL.glDeleteFramebuffers(len(ids), ids)
        counts = {k: len(v) for k, v in self._tracked.items()}
        self._tracked = {k: set() for k in self._tracked}
        self.log.info("renderer reset: scrubbed %s", counts)

    def execute_clear(self, mask: int) -> None:
        self._GL.glClear(mask)

    def execute_clear_color(self, r: float, g: float, b: float, a: float) -> None:
        if self._force_magenta:
            r, g, b, a = 1.0, 0.0, 1.0, 1.0
        self._GL.glClearColor(r, g, b, a)

    def execute_viewport(self, x: int, y: int, w: int, h: int) -> None:
        self._GL.glViewport(x, y, w, h)

    def execute_finish(self) -> None:
        self._GL.glFinish()

    def execute_get_error(self) -> int:
        return int(self._GL.glGetError())

    def execute_read_pixels(self, x: int, y: int, w: int, h: int,
                            fmt: int, typ: int) -> bytes:
        # Diagnostic: every Nth read, scan ALL known FBOs (0, 1, 2, 3, 4)
        # and report which one(s) have non-black content. Tells us if we're
        # reading from the right framebuffer.
        self._read_count = getattr(self, "_read_count", 0) + 1
        if self._read_count <= 4 or (self._read_count & 63) == 0:
            self._scan_all_fbos_for_content(x, y, w, h)
        return bytes(self._GL.glReadPixels(x, y, w, h, fmt, typ))

    def _scan_all_fbos_for_content(self, x, y, w, h):
        GL = self._GL
        prev_read = GL.glGetIntegerv(0x8CAA)  # GL_READ_FRAMEBUFFER_BINDING
        prev_draw = GL.glGetIntegerv(0x8CA6)  # GL_DRAW_FRAMEBUFFER_BINDING
        # Write-then-read sanity check on fb=1 + GL state probe.
        try:
            GL.glBindFramebuffer(0x8D40, 1)
            _ = GL.glGetError()
            GL.glClearColor(1.0, 0.0, 1.0, 1.0)
            GL.glEnable(0x0C11)
            GL.glScissor(200, 200, 4, 4)
            GL.glClear(0x4000)
            GL.glDisable(0x0C11)
            _ = GL.glGetError()
            GL.glBindFramebuffer(0x8CA8, 1)
            pix = GL.glReadPixels(202, 202, 1, 1, 0x1908, 0x1401)
            arr = bytes(pix)
            wrr_check = f"fb=1 wrote_magenta_at_200 read_back=({arr[0]:02x},{arr[1]:02x},{arr[2]:02x})"
        except Exception as e:
            wrr_check = f"wrr_check EXC {type(e).__name__}: {e}"
        # Probe game-side GL state observed by the daemon RIGHT NOW. Each
        # query individually try/except'd so one bad enum doesn't kill the rest.
        def safe(fn, *a, default="?"):
            try:
                _ = GL.glGetError()  # drain
                v = fn(*a)
                err = GL.glGetError()
                if err: return f"err{hex(err)}"
                return v
            except Exception as e:
                return f"EXC{type(e).__name__}"
        vp = safe(GL.glGetIntegerv, 0x0BA2)              # GL_VIEWPORT
        sc = safe(GL.glGetIntegerv, 0x0C10)              # GL_SCISSOR_BOX
        stest = safe(GL.glIsEnabled, 0x0C11)             # GL_SCISSOR_TEST
        dtest = safe(GL.glIsEnabled, 0x0B71)             # GL_DEPTH_TEST
        blend = safe(GL.glIsEnabled, 0x0BE2)             # GL_BLEND
        cull  = safe(GL.glIsEnabled, 0x0B44)             # GL_CULL_FACE
        bsrc  = safe(GL.glGetIntegerv, 0x80C9)           # GL_BLEND_SRC_RGB (ES)
        bdst  = safe(GL.glGetIntegerv, 0x80C8)           # GL_BLEND_DST_RGB (ES)
        bsrcA = safe(GL.glGetIntegerv, 0x80CB)           # GL_BLEND_SRC_ALPHA
        bdstA = safe(GL.glGetIntegerv, 0x80CA)           # GL_BLEND_DST_ALPHA
        prog  = safe(GL.glGetIntegerv, 0x8B8D)           # GL_CURRENT_PROGRAM
        dfbo  = safe(GL.glGetIntegerv, 0x8CA6)           # GL_DRAW_FRAMEBUFFER_BINDING
        cmask = safe(GL.glGetBooleanv, 0x0C23)           # GL_COLOR_WRITEMASK
        dmask = safe(GL.glGetBooleanv, 0x0B72)           # GL_DEPTH_WRITEMASK
        state_str = (
            f"vp={vp} scis={sc} stest={stest} dtest={dtest} dmask={dmask} "
            f"blend={blend}(rgb={bsrc}/{bdst},a={bsrcA}/{bdstA}) "
            f"cull={cull} prog={prog} draw_fbo={dfbo} colmask={cmask}"
        )
        report = [wrr_check, "STATE: " + state_str]
        for fb_id in (0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 15, 20):
            try:
                # Bind as DRAW first to query completeness as a draw target
                GL.glBindFramebuffer(0x8CA9, fb_id)  # GL_DRAW_FRAMEBUFFER
                _ = GL.glGetError()
                draw_status = GL.glCheckFramebufferStatus(0x8CA9)
                attach = GL.glGetFramebufferAttachmentParameteriv(
                    0x8CA9, 0x8CE0, 0x8CD1)  # COLOR_ATTACHMENT0, OBJECT_NAME
                _ = GL.glGetError()

                GL.glBindFramebuffer(0x8CA8, fb_id)  # GL_READ_FRAMEBUFFER
                _ = GL.glGetError()
                # Sample 8 corners + center
                samples = [
                    (0, 0), (w-1, 0), (0, h-1), (w-1, h-1),
                    (w//2, h//2), (w//4, h//4), (3*w//4, 3*h//4), (w//2, 0),
                ]
                non_black = 0
                px_summary = []
                for sx, sy in samples:
                    pix = GL.glReadPixels(sx, sy, 1, 1, 0x1908, 0x1401)
                    err2 = GL.glGetError()
                    if err2:
                        px_summary.append(f"err=0x{err2:x}")
                        continue
                    arr = bytes(pix)
                    r, g, b = arr[0], arr[1], arr[2]
                    if r > 8 or g > 8 or b > 8:
                        non_black += 1
                    px_summary.append(f"({r:02x},{g:02x},{b:02x})")
                report.append(
                    f"fb={fb_id}: stat=0x{int(draw_status):x} "
                    f"col0_tex={int(attach)} nonblack={non_black}/8 "
                    f"[{' '.join(px_summary)}]")
            except Exception as e:
                report.append(f"fb={fb_id}: EXC {type(e).__name__}")
        try:
            GL.glBindFramebuffer(0x8CA8, int(prev_read) if prev_read is not None else 0)
            GL.glBindFramebuffer(0x8CA9, int(prev_draw) if prev_draw is not None else 0)
        except Exception:
            pass
        self.log.info("FBO_SCAN #%d (read %dx%d): %s",
                      self._read_count, w, h, " | ".join(report))

    def gen_buffers(self, n: int) -> list:
        ids = [int(i) for i in self._GL.glGenBuffers(n)] if n != 1 else [int(self._GL.glGenBuffers(1))]
        self._tracked["buffers"].update(ids)
        return ids

    def delete_buffers(self, ids: list) -> None:
        self._GL.glDeleteBuffers(len(ids), ids)
        for i in ids: self._tracked["buffers"].discard(int(i))

    def bind_buffer(self, target: int, buf_id: int) -> None:
        self._GL.glBindBuffer(target, buf_id)

    def buffer_data(self, target: int, size: int, usage: int, data: bytes) -> None:
        self._GL.glBufferData(target, size, data if data else None, usage)

    def gen_vertex_arrays(self, n: int) -> list:
        ids = [int(i) for i in self._GL.glGenVertexArrays(n)] if n != 1 else [int(self._GL.glGenVertexArrays(1))]
        self._tracked["vaos"].update(ids)
        return ids

    def delete_vertex_arrays(self, ids: list) -> None:
        self._GL.glDeleteVertexArrays(len(ids), ids)
        for i in ids: self._tracked["vaos"].discard(int(i))

    def bind_vertex_array(self, vao_id: int) -> None:
        self._GL.glBindVertexArray(vao_id)

    def create_shader(self, shader_type: int) -> int:
        sid = int(self._GL.glCreateShader(shader_type))
        self._tracked["shaders"].add(sid)
        return sid

    def delete_shader(self, shader_id: int) -> None:
        self._GL.glDeleteShader(shader_id)
        self._tracked["shaders"].discard(int(shader_id))

    def shader_source(self, shader_id: int, source: bytes) -> None:
        src = source.decode("utf-8", "replace")
        self.log.info("shader_source(id=%d, %d bytes): %r", shader_id, len(source), src[:200])
        self._GL.glShaderSource(shader_id, [src])

    def compile_shader(self, shader_id: int) -> None:
        self._GL.glCompileShader(shader_id)
        status = self._GL.glGetShaderiv(shader_id, 0x8B81)
        if not status:
            log = self._GL.glGetShaderInfoLog(shader_id)
            self.log.warning("shader %d compile FAILED: %s", shader_id,
                             log.decode() if isinstance(log, bytes) else log)

    def get_shader_iv(self, shader_id: int, pname: int) -> int:
        return int(self._GL.glGetShaderiv(shader_id, pname))

    def create_program(self) -> int:
        pid = int(self._GL.glCreateProgram())
        self._tracked["programs"].add(pid)
        return pid

    def delete_program(self, prog: int) -> None:
        self._GL.glDeleteProgram(prog)
        self._tracked["programs"].discard(int(prog))

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
        self._u4f_count = getattr(self, "_u4f_count", 0) + 1
        if self._u4f_count <= 16 or (self._u4f_count & 63) == 0:
            self.log.info("u4f #%d loc=%d (%.2g, %.2g, %.2g, %.2g)",
                          self._u4f_count, location, v0, v1, v2, v3)
        self._GL.glUniform4f(location, v0, v1, v2, v3)

    def uniform_matrix4fv(self, location: int, count: int, transpose: int, data: bytes) -> None:
        import numpy as np
        floats = np.frombuffer(data, dtype=np.float32, count=count*16).reshape(count, 4, 4)
        self._GL.glUniformMatrix4fv(location, count, bool(transpose), floats)

    def draw_arrays(self, mode: int, first: int, count: int) -> None:
        self._draw_count += 1
        if self._draw_count <= 16 or (self._draw_count & 1023) == 0:
            self.log.info("draw #%d: glDrawArrays(mode=0x%x, count=%d)",
                          self._draw_count, mode, count)
        self._GL.glDrawArrays(mode, first, count)

    def draw_elements(self, mode: int, count: int, typ: int, offset: int) -> None:
        self._draw_count += 1
        if self._draw_count <= 16 or (self._draw_count & 1023) == 0:
            self.log.info("draw #%d: glDrawElements(mode=0x%x, count=%d)",
                          self._draw_count, mode, count)
        self._GL.glDrawElements(mode, count, typ, ctypes.c_void_p(offset))

    def enable(self, cap: int) -> None:
        if self._disable_blend and cap == 0x0BE2:  # GL_BLEND
            return
        self._GL.glEnable(cap)
    def disable(self, cap: int) -> None:   self._GL.glDisable(cap)
    def blend_func(self, s: int, d: int) -> None:
        self._blend_count = getattr(self, "_blend_count", 0) + 1
        if self._blend_count <= 32 or (self._blend_count & 255) == 0:
            self.log.info("blend #%d: glBlendFunc(0x%x, 0x%x)", self._blend_count, s, d)
        self._GL.glBlendFunc(s, d)
    def depth_func(self, f: int) -> None:  self._GL.glDepthFunc(f)
    def depth_mask(self, m: int) -> None:  self._GL.glDepthMask(bool(m))
    def cull_face(self, m: int) -> None:   self._GL.glCullFace(m)
    def scissor(self, x: int, y: int, w: int, h: int) -> None: self._GL.glScissor(x, y, w, h)
    def color_mask(self, r: int, g: int, b: int, a: int) -> None:
        self._GL.glColorMask(bool(r), bool(g), bool(b), bool(a))

    def gen_textures(self, n: int) -> list:
        ids = [int(i) for i in self._GL.glGenTextures(n)] if n != 1 else [int(self._GL.glGenTextures(1))]
        self._tracked["textures"].update(ids)
        return ids
    def delete_textures(self, ids: list) -> None:
        self._GL.glDeleteTextures(ids)
        for i in ids: self._tracked["textures"].discard(int(i))
    def active_texture(self, unit: int) -> None:  self._GL.glActiveTexture(unit)
    def bind_texture(self, target: int, tex: int) -> None: self._GL.glBindTexture(target, tex)
    def tex_image_2d(self, target, level, internal_format, w, h, border, fmt, typ, data: bytes) -> None:
        if (data and w >= 256 and h >= 256
                and fmt == 0x1908 and typ == 0x1401
                and self._test_pattern):
            import numpy as np
            yy, xx = np.indices((h, w))
            checker = ((xx // 32) + (yy // 32)) & 1
            buf = np.empty((h, w, 4), dtype=np.uint8)
            buf[..., 3] = 255
            buf[..., 0] = np.where(checker, 255, 0)
            buf[..., 1] = np.where(checker, 0, 255)
            buf[..., 2] = np.where(checker, 255, 0)
            data = buf.tobytes()
        self._GL.glTexImage2D(target, level, internal_format, w, h, border, fmt, typ,
                              data if data else None)
    def tex_sub_image_2d(self, target, level, x, y, w, h, fmt, typ, data: bytes) -> None:
        self._GL.glTexSubImage2D(target, level, x, y, w, h, fmt, typ,
                                 data if data else None)
    def tex_parameter_i(self, target: int, pname: int, param: int) -> None:
        self._GL.glTexParameteri(target, pname, param)
    def pixel_store_i(self, pname: int, param: int) -> None:
        self._GL.glPixelStorei(pname, param)

    def gen_framebuffers(self, n: int) -> list:
        ids = [int(i) for i in self._GL.glGenFramebuffers(n)] if n != 1 else [int(self._GL.glGenFramebuffers(1))]
        self._tracked["framebuffers"].update(ids)
        return ids
    def delete_framebuffers(self, ids: list) -> None:
        self._GL.glDeleteFramebuffers(len(ids), ids)
        for i in ids: self._tracked["framebuffers"].discard(int(i))
    def bind_framebuffer(self, target: int, fb: int) -> None:
        # Temporary unthrottled trace — confirms ground-truth FBO sequence the
        # daemon executes (vs the shim's throttled trace). Look for fb=0 binds
        # during attract; their presence would prove the game switched to the
        # default framebuffer and our frame_cap is reading the wrong target.
        self._bind_fb_count = getattr(self, "_bind_fb_count", 0) + 1
        tname = {0x8D40: "FB", 0x8CA8: "READ", 0x8CA9: "DRAW"}.get(target, "?")
        self.log.info("bind_fb #%d: %s fb=%d", self._bind_fb_count, tname, fb)
        self._GL.glBindFramebuffer(target, fb)
    def framebuffer_texture_2d(self, target, attachment, textarget, tex, level) -> None:
        self._GL.glFramebufferTexture2D(target, attachment, textarget, tex, level)
    def check_framebuffer_status(self, target: int) -> int:
        return int(self._GL.glCheckFramebufferStatus(target))

    # --- Phase 3d additions ---
    def disable_vertex_attrib_array(self, idx: int) -> None: self._GL.glDisableVertexAttribArray(idx)
    def blend_func_separate(self, srcRGB, dstRGB, srcA, dstA) -> None:
        self._blend_count = getattr(self, "_blend_count", 0) + 1
        if self._blend_count <= 32 or (self._blend_count & 255) == 0:
            self.log.info("blend #%d: glBlendFuncSeparate(0x%x, 0x%x, 0x%x, 0x%x)",
                          self._blend_count, srcRGB, dstRGB, srcA, dstA)
        self._GL.glBlendFuncSeparate(srcRGB, dstRGB, srcA, dstA)
    def blend_equation(self, mode: int) -> None: self._GL.glBlendEquation(mode)
    def blend_equation_separate(self, modeRGB, modeA) -> None: self._GL.glBlendEquationSeparate(modeRGB, modeA)
    def blend_color(self, r, g, b, a) -> None: self._GL.glBlendColor(r, g, b, a)
    def stencil_func(self, fn, ref, mask) -> None: self._GL.glStencilFunc(fn, ref, mask)
    def stencil_op(self, sf, df, dp) -> None: self._GL.glStencilOp(sf, df, dp)
    def stencil_mask(self, m: int) -> None: self._GL.glStencilMask(m)
    def depth_rangef(self, n, f) -> None: self._GL.glDepthRangef(n, f)
    def front_face(self, mode: int) -> None: self._GL.glFrontFace(mode)
    def line_width(self, w: float) -> None: self._GL.glLineWidth(w)
    def polygon_offset(self, factor, units) -> None: self._GL.glPolygonOffset(factor, units)
    def clear_depthf(self, d: float) -> None: self._GL.glClearDepthf(d)
    def clear_stencil(self, s: int) -> None: self._GL.glClearStencil(s)
    def tex_parameter_f(self, target, pname, param) -> None: self._GL.glTexParameterf(target, pname, param)
    def vertex_attrib_divisor(self, idx, div) -> None: self._GL.glVertexAttribDivisor(idx, div)
    def bind_buffer_base(self, target, idx, buf) -> None: self._GL.glBindBufferBase(target, idx, buf)
    def bind_buffer_range(self, target, idx, buf, offset, size) -> None: self._GL.glBindBufferRange(target, idx, buf, offset, size)
    def uniform_block_binding(self, prog, idx, binding) -> None: self._GL.glUniformBlockBinding(prog, idx, binding)
    def get_uniform_block_index(self, prog: int, name: bytes) -> int:
        return int(self._GL.glGetUniformBlockIndex(prog, name.decode("utf-8", "replace")))
    def gen_renderbuffers(self, n: int) -> list:
        ids = [int(i) for i in self._GL.glGenRenderbuffers(n)] if n != 1 else [int(self._GL.glGenRenderbuffers(1))]
        self._tracked["framebuffers"].update(ids)  # share tracking bucket
        return ids
    def delete_renderbuffers(self, ids: list) -> None:
        self._GL.glDeleteRenderbuffers(len(ids), ids)
        for i in ids: self._tracked["framebuffers"].discard(int(i))
    def bind_renderbuffer(self, target, rb) -> None: self._GL.glBindRenderbuffer(target, rb)
    def framebuffer_renderbuffer(self, target, attachment, rb_target, rb) -> None:
        self._GL.glFramebufferRenderbuffer(target, attachment, rb_target, rb)
    def renderbuffer_storage(self, target, internalformat, w, h) -> None:
        self._GL.glRenderbufferStorage(target, internalformat, w, h)
    def generate_mipmap(self, target: int) -> None: self._GL.glGenerateMipmap(target)
    def draw_buffers(self, bufs: list) -> None: self._GL.glDrawBuffers(len(bufs), bufs)
    def read_buffer(self, src: int) -> None: self._GL.glReadBuffer(src)
    def invalidate_framebuffer(self, target: int, attachments: list) -> None:
        self._GL.glInvalidateFramebuffer(target, len(attachments), attachments)
    def vertex_attrib_i_pointer(self, idx, size, typ, stride, offset) -> None:
        self._GL.glVertexAttribIPointer(idx, size, typ, stride, ctypes.c_void_p(offset))
    def clear_buffer_fv(self, buffer: int, drawbuffer: int, vals: list) -> None:
        # GL_COLOR = 0x1800; only override colour-buffer clears, leave depth
        # (GL_DEPTH=0x1801) / stencil (0x1802) values alone.
        if self._force_magenta and buffer == 0x1800:
            vals = [1.0, 0.0, 1.0, 1.0]
        self._GL.glClearBufferfv(buffer, drawbuffer, vals)
    def detach_shader(self, prog, shader) -> None: self._GL.glDetachShader(prog, shader)
    def bind_attrib_location(self, prog, idx, name: bytes) -> None:
        self._GL.glBindAttribLocation(prog, idx, name.decode("utf-8", "replace"))
    def validate_program(self, prog: int) -> None: self._GL.glValidateProgram(prog)
    def gl_flush(self) -> None: self._GL.glFlush()

    def buffer_sub_data(self, target, offset, data: bytes) -> None:
        self._GL.glBufferSubData(target, offset, len(data), data)
    def tex_image_3d(self, target, level, internal, w, h, d, border, fmt, typ, data: bytes) -> None:
        self._GL.glTexImage3D(target, level, internal, w, h, d, border, fmt, typ, data if data else None)
    def tex_sub_image_3d(self, target, level, x, y, z, w, h, d, fmt, typ, data: bytes) -> None:
        self._GL.glTexSubImage3D(target, level, x, y, z, w, h, d, fmt, typ, data if data else None)
    def tex_storage_2d(self, target, levels, internal, w, h) -> None:
        self._GL.glTexStorage2D(target, levels, internal, w, h)
    def tex_storage_3d(self, target, levels, internal, w, h, d) -> None:
        self._GL.glTexStorage3D(target, levels, internal, w, h, d)
    def compressed_tex_image_2d(self, target, level, internal, w, h, border, data: bytes) -> None:
        # Bypass PyOpenGL — its CompressedImageConverter resolves the
        # imageSize argument to a NumberHandler that has no arrayByteCount
        # method, raising AttributeError on every DXT5 upload. Call
        # libGLESv2's symbol directly via ctypes.
        if data:
            c_buf = (ctypes.c_ubyte * len(data)).from_buffer_copy(data)
            self._libGLESv2.glCompressedTexImage2D(
                target, level, internal, w, h, border, len(data),
                ctypes.cast(c_buf, ctypes.c_void_p))
        else:
            self._libGLESv2.glCompressedTexImage2D(
                target, level, internal, w, h, border, 0, None)
    def compressed_tex_sub_image_2d(self, target, level, x, y, w, h, fmt, data: bytes) -> None:
        if data:
            c_buf = (ctypes.c_ubyte * len(data)).from_buffer_copy(data)
            self._libGLESv2.glCompressedTexSubImage2D(
                target, level, x, y, w, h, fmt, len(data),
                ctypes.cast(c_buf, ctypes.c_void_p))
        else:
            self._libGLESv2.glCompressedTexSubImage2D(
                target, level, x, y, w, h, fmt, 0, None)

    def blit_framebuffer(self, sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filt) -> None:
        self._blit_count = getattr(self, "_blit_count", 0) + 1
        if self._blit_count <= 8 or (self._blit_count & 1023) == 0:
            self.log.info("blit #%d: src=(%d,%d,%d,%d) dst=(%d,%d,%d,%d) mask=0x%x",
                          self._blit_count, sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask)
        if self._force_magenta:
            # Skip the actual blit so any magenta we cleared into the destination
            # FBO survives. If attract was being blacked-out by a blit-from-black,
            # this will let the magenta show through.
            return
        self._GL.glBlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filt)
    def draw_range_elements(self, mode, start, end, count, typ, offset) -> None:
        self._GL.glDrawRangeElements(mode, start, end, count, typ, ctypes.c_void_p(offset))
    def draw_arrays_instanced(self, mode, first, count, inst) -> None:
        self._GL.glDrawArraysInstanced(mode, first, count, inst)
    def draw_elements_instanced(self, mode, count, typ, offset, inst) -> None:
        self._GL.glDrawElementsInstanced(mode, count, typ, ctypes.c_void_p(offset), inst)
    def copy_buffer_sub_data(self, rt, wt, ro, wo, sz) -> None:
        self._GL.glCopyBufferSubData(rt, wt, ro, wo, sz)

    def uniform_1f(self, loc, v0) -> None: self._GL.glUniform1f(loc, v0)
    def uniform_2f(self, loc, v0, v1) -> None: self._GL.glUniform2f(loc, v0, v1)
    def uniform_3f(self, loc, v0, v1, v2) -> None: self._GL.glUniform3f(loc, v0, v1, v2)
    def uniform_2i(self, loc, v0, v1) -> None: self._GL.glUniform2i(loc, v0, v1)
    def uniform_3i(self, loc, v0, v1, v2) -> None: self._GL.glUniform3i(loc, v0, v1, v2)
    def uniform_4i(self, loc, v0, v1, v2, v3) -> None: self._GL.glUniform4i(loc, v0, v1, v2, v3)
    def uniform_1iv(self, loc, count, data: bytes) -> None:
        import numpy as np
        ints = np.frombuffer(data, dtype=np.int32, count=count)
        self._GL.glUniform1iv(loc, count, ints)
    def uniform_xfv(self, n_per_elem: int, fn, loc, count, data: bytes) -> None:
        import numpy as np
        floats = np.frombuffer(data, dtype=np.float32, count=count * n_per_elem)
        # Log vec4 uniform sets so we can spot any "(_, _, _, 0)" patterns
        # (alpha-killing) — Stern's colorTransformMultiply is a vec4. Sample
        # first 8 + every 64th of vec4 sets for visibility without spam.
        if n_per_elem == 4:
            self._uxfv4_count = getattr(self, "_uxfv4_count", 0) + 1
            if self._uxfv4_count <= 16 or (self._uxfv4_count & 63) == 0:
                vals_str = ",".join(f"{floats[i*4]:.2g}/{floats[i*4+1]:.2g}/{floats[i*4+2]:.2g}/{floats[i*4+3]:.2g}"
                                    for i in range(min(count, 4)))
                self.log.info("u4fv #%d loc=%d count=%d: [%s]",
                              self._uxfv4_count, loc, count, vals_str)
        fn(loc, count, floats)
    def uniform_matrix_3fv(self, loc, count, transpose, data: bytes) -> None:
        import numpy as np
        floats = np.frombuffer(data, dtype=np.float32, count=count * 9).reshape(count, 3, 3)
        self._GL.glUniformMatrix3fv(loc, count, bool(transpose), floats)

    def uniform_1ui(self, loc, v0) -> None: self._GL.glUniform1ui(loc, v0)
    def uniform_1uiv(self, loc, count, data: bytes) -> None:
        import numpy as np
        uints = np.frombuffer(data, dtype=np.uint32, count=count)
        self._GL.glUniform1uiv(loc, count, uints)
    def uniform_2iv(self, loc, count, data: bytes) -> None:
        import numpy as np
        ints = np.frombuffer(data, dtype=np.int32, count=count * 2)
        self._GL.glUniform2iv(loc, count, ints)

    def vertex_attrib_4f(self, idx, x, y, z, w) -> None:
        self._GL.glVertexAttrib4f(idx, x, y, z, w)
    def vertex_attrib_i4ui(self, idx, x, y, z, w) -> None:
        self._GL.glVertexAttribI4ui(idx, x, y, z, w)
    def fb_tex_2d_ms(self, target, attachment, textarget, tex, level, samples) -> None:
        # GL_EXT_multisampled_render_to_texture entry point. Mesa exports it
        # via eglGetProcAddress; PyOpenGL may not have a pre-bound symbol.
        # Resolve through ctypes if needed.
        try:
            self._GL.glFramebufferTexture2DMultisampleEXT(target, attachment, textarget, tex, level, samples)
        except Exception:
            # Fallback to non-multisample version if EXT not available
            self._GL.glFramebufferTexture2D(target, attachment, textarget, tex, level)
    def compressed_tex_image_3d(self, target, level, internal, w, h, d, border, data: bytes) -> None:
        import numpy as np
        arr = np.frombuffer(data, dtype=np.uint8) if data else None
        self._GL.glCompressedTexImage3D(target, level, internal, w, h, d, border, len(data), arr)
    def compressed_tex_sub_image_3d(self, target, level, x, y, z, w, h, d, fmt, data: bytes) -> None:
        import numpy as np
        arr = np.frombuffer(data, dtype=np.uint8) if data else None
        self._GL.glCompressedTexSubImage3D(target, level, x, y, z, w, h, d, fmt, len(data), arr)
    def tex_storage_3d_ms(self, target, samples, internal, w, h, d, fixed) -> None:
        try:
            self._GL.glTexStorage3DMultisample(target, samples, internal, w, h, d, bool(fixed))
        except Exception:
            self._GL.glTexStorage3D(target, 1, internal, w, h, d)


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
                try:
                    self.handle(op, flags, payload)
                except Exception as e:
                    self.log.error("handle(op=0x%04x) raised %s: %s", op, type(e).__name__, e)
                    if flags & P.GL_FLAG_SYNC:
                        send_reply(self.sock, P.GL_OP_REP_ERR,
                                   struct.pack(P.REP_ERR_FMT, -1, 0))
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
        elif op == P.GL_OP_GL_DRAW_ELEMENTS:
            mode, count, typ_, offset = struct.unpack(P.DRAW_ELEMENTS_FMT, payload)
            self.renderer.draw_elements(mode, count, typ_, offset)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)

        elif op == P.GL_OP_GL_ENABLE:
            (cap,) = struct.unpack(P.ENABLE_FMT, payload); self.renderer.enable(cap)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_DISABLE:
            (cap,) = struct.unpack(P.ENABLE_FMT, payload); self.renderer.disable(cap)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_BLEND_FUNC:
            s, d = struct.unpack(P.BLEND_FUNC_FMT, payload); self.renderer.blend_func(s, d)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_DEPTH_FUNC:
            (f,) = struct.unpack(P.DEPTH_FUNC_FMT, payload); self.renderer.depth_func(f)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_DEPTH_MASK:
            (m,) = struct.unpack(P.BYTE_PARAM_FMT, payload); self.renderer.depth_mask(m)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_CULL_FACE:
            (m,) = struct.unpack(P.ENABLE_FMT, payload); self.renderer.cull_face(m)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_SCISSOR:
            x, y, w, h = struct.unpack(P.RECT_FMT, payload); self.renderer.scissor(x, y, w, h)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_COLOR_MASK:
            r, g, b, a = struct.unpack(P.COLOR_MASK_FMT, payload)
            self.renderer.color_mask(r, g, b, a)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)

        elif op == P.GL_OP_GL_GEN_TEXTURES:
            (n,) = struct.unpack(P.COUNT_FMT, payload)
            ids = self.renderer.gen_textures(n)
            send_reply(self.sock, P.GL_OP_REP_IDS,
                       struct.pack(P.REP_IDS_FMT, len(ids)) +
                       b''.join(struct.pack("<I", i) for i in ids))
        elif op == P.GL_OP_GL_DELETE_TEXTURES:
            (n,) = struct.unpack(P.COUNT_FMT, payload[:4])
            ids = list(struct.unpack(f"<{n}I", payload[4:4+4*n]))
            self.renderer.delete_textures(ids)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_ACTIVE_TEXTURE:
            (u,) = struct.unpack(P.ACTIVE_TEX_FMT, payload); self.renderer.active_texture(u)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_BIND_TEXTURE:
            t, tex = struct.unpack(P.BIND_FMT, payload); self.renderer.bind_texture(t, tex)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_TEX_IMAGE_2D:
            hsz = struct.calcsize(P.TEX_IMAGE_2D_FMT)
            t, lvl, ifmt, w, h, border, fmt, typ, dsz = struct.unpack(P.TEX_IMAGE_2D_FMT, payload[:hsz])
            self.renderer.tex_image_2d(t, lvl, ifmt, w, h, border, fmt, typ,
                                       payload[hsz:hsz+dsz] if dsz else b'')
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_TEX_SUB_IMAGE_2D:
            hsz = struct.calcsize(P.TEX_SUB_IMAGE_2D_FMT)
            t, lvl, x, y, w, h, fmt, typ, dsz = struct.unpack(P.TEX_SUB_IMAGE_2D_FMT, payload[:hsz])
            self.renderer.tex_sub_image_2d(t, lvl, x, y, w, h, fmt, typ,
                                           payload[hsz:hsz+dsz] if dsz else b'')
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_TEX_PARAMETER_I:
            t, pn, val = struct.unpack(P.TEX_PARAMETER_I_FMT, payload)
            self.renderer.tex_parameter_i(t, pn, val)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_PIXEL_STORE_I:
            pn, val = struct.unpack(P.PIXEL_STORE_I_FMT, payload)
            self.renderer.pixel_store_i(pn, val)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)

        elif op == P.GL_OP_GL_GEN_FRAMEBUFFERS:
            (n,) = struct.unpack(P.COUNT_FMT, payload)
            ids = self.renderer.gen_framebuffers(n)
            send_reply(self.sock, P.GL_OP_REP_IDS,
                       struct.pack(P.REP_IDS_FMT, len(ids)) +
                       b''.join(struct.pack("<I", i) for i in ids))
        elif op == P.GL_OP_GL_DELETE_FRAMEBUFFERS:
            (n,) = struct.unpack(P.COUNT_FMT, payload[:4])
            ids = list(struct.unpack(f"<{n}I", payload[4:4+4*n]))
            self.renderer.delete_framebuffers(ids)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_BIND_FRAMEBUFFER:
            t, fb = struct.unpack(P.BIND_FMT, payload); self.renderer.bind_framebuffer(t, fb)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_FRAMEBUFFER_TEX_2D:
            t, att, tt, tex, lvl = struct.unpack(P.FRAMEBUFFER_TEX_2D_FMT, payload)
            self.renderer.framebuffer_texture_2d(t, att, tt, tex, lvl)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_CHECK_FRAMEBUFFER:
            (t,) = struct.unpack(P.ENABLE_FMT, payload)
            v = self.renderer.check_framebuffer_status(t)
            send_reply(self.sock, P.GL_OP_REP_VALUE, struct.pack(P.REP_VALUE_FMT, v))

        # --- Phase 3d batch ---
        elif op == P.GL_OP_GL_DISABLE_VERTEX_ATTRIB:
            (idx,) = struct.unpack(P.P1U_FMT, payload); self.renderer.disable_vertex_attrib_array(idx)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_BLEND_FUNC_SEPARATE:
            sR, dR, sA, dA = struct.unpack(P.BLEND_FUNC_SEPARATE_FMT, payload)
            self.renderer.blend_func_separate(sR, dR, sA, dA)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_BLEND_EQUATION:
            (m,) = struct.unpack(P.P1U_FMT, payload); self.renderer.blend_equation(m)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_BLEND_EQUATION_SEPARATE:
            mR, mA = struct.unpack(P.BLEND_EQ_SEPARATE_FMT, payload)
            self.renderer.blend_equation_separate(mR, mA)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_BLEND_COLOR:
            r, g, b, a = struct.unpack(P.P4F_FMT, payload); self.renderer.blend_color(r, g, b, a)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_STENCIL_FUNC:
            f, r, m = struct.unpack(P.P3U_FMT, payload); self.renderer.stencil_func(f, r, m)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_STENCIL_OP:
            sf, df, dp = struct.unpack(P.P3U_FMT, payload); self.renderer.stencil_op(sf, df, dp)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_STENCIL_MASK:
            (m,) = struct.unpack(P.P1U_FMT, payload); self.renderer.stencil_mask(m)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_DEPTH_RANGEF:
            n, f = struct.unpack(P.P2F_FMT, payload); self.renderer.depth_rangef(n, f)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_FRONT_FACE:
            (m,) = struct.unpack(P.P1U_FMT, payload); self.renderer.front_face(m)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_LINE_WIDTH:
            (w,) = struct.unpack(P.P1F_FMT, payload); self.renderer.line_width(w)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_POLYGON_OFFSET:
            f, u = struct.unpack(P.P2F_FMT, payload); self.renderer.polygon_offset(f, u)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_CLEAR_DEPTHF:
            (d,) = struct.unpack(P.P1F_FMT, payload); self.renderer.clear_depthf(d)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_CLEAR_STENCIL:
            (s,) = struct.unpack(P.P1I_FMT, payload); self.renderer.clear_stencil(s)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_TEX_PARAMETER_F:
            t, pn, v = struct.unpack(P.TEX_PARAM_F_FMT, payload); self.renderer.tex_parameter_f(t, pn, v)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_VERTEX_ATTRIB_DIVISOR:
            i, d = struct.unpack(P.P2U_FMT, payload); self.renderer.vertex_attrib_divisor(i, d)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_BIND_BUFFER_BASE:
            t, idx, b = struct.unpack(P.BIND_BUF_BASE_FMT, payload); self.renderer.bind_buffer_base(t, idx, b)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_BIND_BUFFER_RANGE:
            t, idx, b, off, sz = struct.unpack(P.BIND_BUF_RANGE_FMT, payload)
            self.renderer.bind_buffer_range(t, idx, b, off, sz)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_UNIFORM_BLOCK_BINDING:
            p, i, b = struct.unpack(P.UBLOCK_BINDING_FMT, payload)
            self.renderer.uniform_block_binding(p, i, b)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_BIND_RENDERBUFFER:
            t, rb = struct.unpack(P.BIND_FMT, payload); self.renderer.bind_renderbuffer(t, rb)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_FRAMEBUFFER_RB:
            t, a, rt, rb = struct.unpack(P.FB_RB_FMT, payload)
            self.renderer.framebuffer_renderbuffer(t, a, rt, rb)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_RENDERBUFFER_STORAGE:
            t, ifmt, w, h = struct.unpack(P.RB_STORAGE_FMT, payload)
            self.renderer.renderbuffer_storage(t, ifmt, w, h)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_GENERATE_MIPMAP:
            (t,) = struct.unpack(P.P1U_FMT, payload); self.renderer.generate_mipmap(t)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_DRAW_BUFFERS:
            (n,) = struct.unpack(P.DRAW_BUFFERS_FMT, payload[:4])
            bufs = list(struct.unpack(f"<{n}I", payload[4:4+4*n]))
            self.renderer.draw_buffers(bufs)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_READ_BUFFER:
            (s,) = struct.unpack(P.P1U_FMT, payload); self.renderer.read_buffer(s)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_INVALIDATE_FB:
            t, n = struct.unpack(P.INVALIDATE_FB_FMT, payload[:8])
            atts = list(struct.unpack(f"<{n}I", payload[8:8+4*n]))
            self.renderer.invalidate_framebuffer(t, atts)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_VERTEX_ATTRIB_I_PTR:
            i, sz, ty, st, off = struct.unpack(P.VAP_I_FMT, payload)
            self.renderer.vertex_attrib_i_pointer(i, sz, ty, st, off)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_CLEAR_BUFFER_FV:
            buf, db = struct.unpack(P.CLEAR_BUFFER_FV_FMT, payload[:8])
            vals = list(struct.unpack("<4f", payload[8:24]))
            self.renderer.clear_buffer_fv(buf, db, vals)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_DETACH_SHADER:
            p, s = struct.unpack(P.TWO_ID_FMT, payload); self.renderer.detach_shader(p, s)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_BIND_ATTRIB_LOC:
            p, idx, nlen = struct.unpack(P.BIND_ATTRIB_LOC_FMT, payload[:12])
            name = payload[12:12+nlen]
            self.renderer.bind_attrib_location(p, idx, name)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_VALIDATE_PROGRAM:
            (p,) = struct.unpack(P.P1U_FMT, payload); self.renderer.validate_program(p)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_FLUSH:
            self.renderer.gl_flush()
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)

        elif op == P.GL_OP_GL_BUFFER_SUB_DATA:
            t, off, sz = struct.unpack(P.BUFFER_SUB_DATA_FMT, payload[:16])
            self.renderer.buffer_sub_data(t, off, payload[16:16+sz])
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_TEX_IMAGE_3D:
            hsz = struct.calcsize(P.TEX_IMAGE_3D_FMT)
            t, lvl, ifmt, w, h, d, border, fmt, typ, dsz = struct.unpack(P.TEX_IMAGE_3D_FMT, payload[:hsz])
            self.renderer.tex_image_3d(t, lvl, ifmt, w, h, d, border, fmt, typ, payload[hsz:hsz+dsz] if dsz else b'')
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_TEX_SUB_IMAGE_3D:
            hsz = struct.calcsize(P.TEX_SUB_IMAGE_3D_FMT)
            t, lvl, x, y, z, w, h, d, fmt, typ, dsz = struct.unpack(P.TEX_SUB_IMAGE_3D_FMT, payload[:hsz])
            self.renderer.tex_sub_image_3d(t, lvl, x, y, z, w, h, d, fmt, typ, payload[hsz:hsz+dsz] if dsz else b'')
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_TEX_STORAGE_2D:
            t, lv, ifmt, w, h = struct.unpack(P.TEX_STORAGE_2D_FMT, payload)
            self.renderer.tex_storage_2d(t, lv, ifmt, w, h)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_TEX_STORAGE_3D:
            t, lv, ifmt, w, h, d = struct.unpack(P.TEX_STORAGE_3D_FMT, payload)
            self.renderer.tex_storage_3d(t, lv, ifmt, w, h, d)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_COMPRESSED_TEX_IMAGE_2D:
            hsz = struct.calcsize(P.COMPRESSED_TEX_IMAGE_2D_FMT)
            t, lv, ifmt, w, h, b, isz = struct.unpack(P.COMPRESSED_TEX_IMAGE_2D_FMT, payload[:hsz])
            self.renderer.compressed_tex_image_2d(t, lv, ifmt, w, h, b, payload[hsz:hsz+isz] if isz else b'')
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_COMPRESSED_TEX_SUB_IMAGE_2D:
            hsz = struct.calcsize(P.COMPRESSED_TEX_SUB_IMAGE_2D_FMT)
            t, lv, x, y, w, h, fmt, isz = struct.unpack(P.COMPRESSED_TEX_SUB_IMAGE_2D_FMT, payload[:hsz])
            self.renderer.compressed_tex_sub_image_2d(t, lv, x, y, w, h, fmt, payload[hsz:hsz+isz] if isz else b'')
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)

        elif op == P.GL_OP_GL_GEN_RENDERBUFFERS:
            (n,) = struct.unpack(P.COUNT_FMT, payload)
            ids = self.renderer.gen_renderbuffers(n)
            send_reply(self.sock, P.GL_OP_REP_IDS,
                       struct.pack(P.REP_IDS_FMT, len(ids)) +
                       b''.join(struct.pack("<I", i) for i in ids))
        elif op == P.GL_OP_GL_DELETE_RENDERBUFFERS:
            (n,) = struct.unpack(P.COUNT_FMT, payload[:4])
            ids = list(struct.unpack(f"<{n}I", payload[4:4+4*n]))
            self.renderer.delete_renderbuffers(ids)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)

        elif op == P.GL_OP_GL_BLIT_FRAMEBUFFER:
            sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, fil = struct.unpack(P.BLIT_FB_FMT, payload)
            self.renderer.blit_framebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, fil)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_DRAW_RANGE_ELEMENTS:
            mode, st, en, ct, ty, off = struct.unpack(P.DRAW_RANGE_ELEMENTS_FMT, payload)
            self.renderer.draw_range_elements(mode, st, en, ct, ty, off)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_DRAW_ARRAYS_INSTANCED:
            mode, first, count, inst = struct.unpack(P.DRAW_ARRAYS_INST_FMT, payload)
            self.renderer.draw_arrays_instanced(mode, first, count, inst)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_DRAW_ELEMENTS_INSTANCED:
            mode, count, ty, off, inst = struct.unpack(P.DRAW_ELEMENTS_INST_FMT, payload)
            self.renderer.draw_elements_instanced(mode, count, ty, off, inst)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_GET_UBLOCK_INDEX:
            p, nlen = struct.unpack(P.GET_UBLOCK_INDEX_FMT, payload[:8])
            idx = self.renderer.get_uniform_block_index(p, payload[8:8+nlen])
            send_reply(self.sock, P.GL_OP_REP_VALUE, struct.pack(P.REP_VALUE_FMT, idx & 0xFFFFFFFF))
        elif op == P.GL_OP_GL_COPY_BUFFER_SUB_DATA:
            rt, wt, ro, wo, sz = struct.unpack(P.COPY_BUFFER_SUB_DATA_FMT, payload)
            self.renderer.copy_buffer_sub_data(rt, wt, ro, wo, sz)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)

        elif op == P.GL_OP_GL_UNIFORM_1F:
            l, v0 = struct.unpack(P.UNIFORM_1F_FMT, payload); self.renderer.uniform_1f(l, v0)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_UNIFORM_2F:
            l, v0, v1 = struct.unpack(P.UNIFORM_2F_FMT, payload); self.renderer.uniform_2f(l, v0, v1)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_UNIFORM_3F:
            l, v0, v1, v2 = struct.unpack(P.UNIFORM_3F_FMT, payload); self.renderer.uniform_3f(l, v0, v1, v2)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_UNIFORM_2I:
            l, v0, v1 = struct.unpack(P.UNIFORM_2I_FMT, payload); self.renderer.uniform_2i(l, v0, v1)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_UNIFORM_3I:
            l, v0, v1, v2 = struct.unpack(P.UNIFORM_3I_FMT, payload); self.renderer.uniform_3i(l, v0, v1, v2)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_UNIFORM_4I:
            l, v0, v1, v2, v3 = struct.unpack(P.UNIFORM_4I_FMT, payload); self.renderer.uniform_4i(l, v0, v1, v2, v3)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_UNIFORM_1IV:
            l, ct = struct.unpack(P.UNIFORM_XIV_FMT, payload[:8])
            self.renderer.uniform_1iv(l, ct, payload[8:])
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_UNIFORM_1FV:
            l, ct = struct.unpack(P.UNIFORM_XFV_FMT, payload[:8])
            self.renderer.uniform_xfv(1, self.renderer._GL.glUniform1fv, l, ct, payload[8:])
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_UNIFORM_2FV:
            l, ct = struct.unpack(P.UNIFORM_XFV_FMT, payload[:8])
            self.renderer.uniform_xfv(2, self.renderer._GL.glUniform2fv, l, ct, payload[8:])
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_UNIFORM_3FV:
            l, ct = struct.unpack(P.UNIFORM_XFV_FMT, payload[:8])
            self.renderer.uniform_xfv(3, self.renderer._GL.glUniform3fv, l, ct, payload[8:])
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_UNIFORM_4FV:
            l, ct = struct.unpack(P.UNIFORM_XFV_FMT, payload[:8])
            self.renderer.uniform_xfv(4, self.renderer._GL.glUniform4fv, l, ct, payload[8:])
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_UNIFORM_MATRIX_3FV:
            l, ct, tr = struct.unpack(P.UNIFORM_MAT3_FMT, payload[:12])
            self.renderer.uniform_matrix_3fv(l, ct, tr, payload[12:])
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_UNIFORM_1UI:
            l, v0 = struct.unpack(P.UNIFORM_1UI_FMT, payload)
            self.renderer.uniform_1ui(l, v0)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_UNIFORM_1UIV:
            l, ct = struct.unpack(P.UNIFORM_XIV_FMT, payload[:8])
            self.renderer.uniform_1uiv(l, ct, payload[8:])
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_UNIFORM_2IV:
            l, ct = struct.unpack(P.UNIFORM_XIV_FMT, payload[:8])
            self.renderer.uniform_2iv(l, ct, payload[8:])
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_VERTEX_ATTRIB_4F:
            i, x, y, z, w = struct.unpack(P.VERTEX_ATTRIB_4F_FMT, payload)
            self.renderer.vertex_attrib_4f(i, x, y, z, w)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_VERTEX_ATTRIB_I_4UI:
            i, x, y, z, w = struct.unpack(P.VERTEX_ATTRIB_I4UI_FMT, payload)
            self.renderer.vertex_attrib_i4ui(i, x, y, z, w)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_FB_TEX_2D_MS_EXT:
            t, a, tt, tx, lv, sm = struct.unpack(P.FB_TEX_2D_MS_FMT, payload)
            self.renderer.fb_tex_2d_ms(t, a, tt, tx, lv, sm)
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_COMPRESSED_TEX_IMAGE_3D:
            hsz = struct.calcsize(P.COMPRESSED_TEX_IMAGE_3D_FMT)
            t, lv, ifmt, w, h, d, b, isz = struct.unpack(P.COMPRESSED_TEX_IMAGE_3D_FMT, payload[:hsz])
            self.renderer.compressed_tex_image_3d(t, lv, ifmt, w, h, d, b, payload[hsz:hsz+isz] if isz else b'')
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_COMPRESSED_TEX_SUB_IMAGE_3D:
            hsz = struct.calcsize(P.COMPRESSED_TEX_SUB_IMAGE_3D_FMT)
            t, lv, x, y, z, w, h, d, fmt, isz = struct.unpack(P.COMPRESSED_TEX_SUB_IMAGE_3D_FMT, payload[:hsz])
            self.renderer.compressed_tex_sub_image_3d(t, lv, x, y, z, w, h, d, fmt, payload[hsz:hsz+isz] if isz else b'')
            if sync: send_reply(self.sock, P.GL_OP_REP_OK)
        elif op == P.GL_OP_GL_TEX_STORAGE_3D_MS:
            t, sm, ifmt, w, h, d, fx = struct.unpack(P.TEX_STORAGE_3D_MS_FMT, payload)
            self.renderer.tex_storage_3d_ms(t, sm, ifmt, w, h, d, fx)
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
