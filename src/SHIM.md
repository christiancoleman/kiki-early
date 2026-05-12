# `src/SHIM.md` — emulator shim internals

How the `spi_shim.so` `LD_PRELOAD` shim, the host-side daemons, and the asset
extractor actually work. The verified **platform / game-binary** facts they rely
on — bridge wire protocols, the cipher, `image.bin` and Radium formats, the
game's own functions/structures, the hardware — live in
[`../docs/PROTOCOL.md`](../docs/PROTOCOL.md). Project layout / build / run is in
[`README.md`](README.md). This is the implementation reference.

`*Verified by:*` footnotes here name the method/run/JOURNAL-step the claim was
checked against, same convention as PROTOCOL. When a claim turns out wrong,
delete it — git history is the archive.

---

## Per-device coverage

| Device path | fd type | Purpose | Coverage |
|---|---|---|---|
| `/dev/spidev4.0` | fake fd `1023` | CPU-board 8-byte SPI polling (local switches, not node bus) | `MESSAGE_1` ioctl: default `rx[2] = 0x04` advances the game's state machine. TX byte 7 carries `g_hardware_output_state` byte 7 (the only byte the game's `set_hardware_output_bit` writes); other TX bytes stay zero unless `write_hardware_output_byte` is called |
| `/dev/i2c-10` | fake fd `1022` | EEPROM at slave 0x57 | Read/write-back works; no real state maintained. **Persistence gap: writes vanish, no SIGTERM flush** |
| `/dev/ttyAMA5` | real `socketpair(AF_UNIX, SOCK_STREAM)` | UART to netbridge (game gets sv[0], shim holds sv[1]) | Reads block naturally on the socketpair. Writes are intercepted by `shim_io.c::write` and routed to `synthesize_bridge_response` (see PROTOCOL "CPU ↔ netbridge UART bridge protocol"). Treated as a pure shim endpoint — the request bytes are logged + synthesized, never written into the socketpair |
| `/dev/ttyAMA2` | real `socketpair(AF_UNIX, SOCK_STREAM)` | secondary UART path; no decoded protocol coverage | Pass-through |
| `/dev/dri/card1` | real `socketpair(AF_UNIX, SOCK_STREAM)` | DRM KMS display | DRM ioctls intercepted before they'd reach the socketpair. See "Display pipeline" below |
| `/dev/shim-v4l2-*` | fake fds `FAKE_FD_V4L2_MEDIA=1021` / `FAKE_FD_V4L2_VIDEO=1020` | V4L2 video device probes | All ioctls return `ENOTTY` |
| `/dev/boot_display` | fake fd `FAKE_FD_BOOT_DISPLAY=1019` | boot_display ioctl surface | ioctls return 0; writes swallowed |
| `/dev/snd/*` | (full ALSA stub via libasound symbols) | audio output | See "Audio: shim ALSA stubs" — 18 symbols stubbed; `snd_pcm_writei` paces, captures, optionally tees to bridge |
| GPIO sysfs (`/sys/class/gpio/*/value`, `direction`, etc.) | fake fd pool `1000..1015` | direct GPIO access (e.g. for libgpiod) | Synthetic reads: `"0\n"` value, `"out\n"` direction. One-shot latch reset by `lseek(0)` |
| libgpiod chardev (`/dev/gpiochip*`) | (Stub) | newer GPIO interface | output-only; reads not modeled |

## Game profile roadmap

`src/games/` is the per-game abstraction layer. Today it contains
`pokemon_le_machine.c/.h`, the Pokémon LE machine profile (cabinet inventory:
switch IDs, driver IDs, lamp IDs, channel/bit mappings, manual IDs, names).
Long-term shape:

```text
src/games/
  machine_profile.h
  pokemon_le.{c,h}
  starwars_le.{c,h}
```

The public shim-facing API should expose one selected runtime profile, e.g.
`const machine_profile_t *shim_machine_profile_current(void);`, holding both
stable cabinet inventory data and per-binary runtime anchors: switch-array
pointer VA, switch translator VA, boot-state mask VA, output-state VA,
Start/coin/interlock helpers, and the other game-version-specific addresses
currently hardcoded in the shim. Startup should identify the loaded
binary/version, select the best exact profile by hash/build/version, then
resolve or verify runtime anchors with known byte/code patterns — failing
closed with a clear log if a required anchor can't be resolved, rather than
silently applying a Pokémon LE 0.81 address to another binary. Current status:
pathing + launch tooling are sysroot-generic; runtime anchors are still
Pokémon LE 0.81-specific unless explicitly documented otherwise.

## Forward-decode bridge (host NVDEC)

The game's HEVC and H.264 video decode paths are intercepted in-shim and routed
to a host-side daemon over an abstract AF_UNIX socket, where decoding happens via
NVDEC and the resulting RGBA frames (post-conversion) are shipped back.

| Component | File | Role |
|---|---|---|
| Shim client | `shim_avforward.c` | C library inside `spi_shim.so`. 8-slot session table; each `avcodec_open2(HEVC \| H.264)` from the game allocates a slot and connects to `@spike3_decode`. `avcodec_send_packet` / `avcodec_receive_frame` (in `shim_patches.c`) dispatch into this TU. Every successful `avcodec_receive_frame` registers the decoded RGBA buffer in a sentinel-keyed side-table that `shim_gl::eglCreateImageKHR` later reads via the `EGL_DMA_BUF_PLANE0_FD_EXT` attrib. Builds a single-plane `AVDRMFrameDescriptor` (`DRM_FORMAT_ABGR8888`) into the per-session 528-byte buffer, points `frame->data[0]` at it |
| Host daemon | `decode_daemon.py` | AF_UNIX server. Per-connection `Session` opens `av.codec.CodecContext.create("hevc_cuvid" \| "h264_cuvid", "r")` (codec_id from the OPEN payload). NVDEC outputs are downloaded via `frame.reformat(format="rgba")` + `to_ndarray(format="rgba")` and sent back as a single tightly-packed RGBA8 plane (W·H·4 bytes). Doing the YUV→RGB on the host saves the cross-arch TCG translation cost of running the conversion inside qemu-aarch64 |
| Launcher | `run_game.sh` | Owns forward-decode daemon selection/defaults and launches the daemon before qemu when enabled (`FORWARD_DECODE`) |

**Wire protocol** (length-prefixed on `@spike3_decode`): each message is
`[u32 type][u32 length][payload]` little-endian; length = payload bytes only.

| Direction | Message (opcode) | Payload |
|---|---|---|
| Game → daemon | `REQ_OPEN` (0x01) | `u32 codec_id`, `u32 extradata_len`, extradata bytes |
| | `REQ_PACKET` (0x02) | `i64 pts`, `i64 dts`, `u32 flags`, `u32 size`, packet bytes |
| | `REQ_RECEIVE` (0x03) | (no payload) |
| | `REQ_CLOSE` (0x04) | (no payload) |
| | `REQ_FLUSH` (0x05) | (no payload — issued after `avcodec_flush_buffers`; resets the daemon's PyAV codec context for a post-seek packet stream) |
| Daemon → game | `REP_OPEN_OK` (0x81) | (no payload) |
| | `REP_PACKET_OK` (0x82) | (no payload — ack for `REQ_PACKET`) |
| | `REP_FRAME` (0x83) | `i32 width`, `i32 height`, `i32 fmt` (`AV_PIX_FMT_RGBA` = 26), `i64 pts`, `u32 n_planes` = 1, then plane[0]: `u32 stride` = W·4, `u32 size` = W·H·4, RGBA bytes |
| | `REP_OK` (0x84) | (no payload — ack for `REQ_CLOSE` and `REQ_FLUSH`) |
| | `REP_ERR` (0xF0) | `i32 errno_or_avcode`, `u32 msg_len`, msg bytes |
| | `REP_EAGAIN` (0xF1) | (no payload — packet queue empty, retry later) |
| | `REP_EOF` (0xF2) | (no payload — codec drained, no more frames coming) |

**libavcodec ABI offsets** the shim pokes (defined in `shim_avforward.c`):
- `AVPacket`: `pts=8`, `dts=16`, `data=24`, `size=32`, `flags=40`
- `AVFrame`: `data[8]=0`, `linesize[8]=64`, `width=104`, `height=108`, `format=116`, `pts=136`, `pkt_dts=144`

When the daemon is unreachable, `avcodec_send_packet` / `avcodec_receive_frame`
return `-EIO` and the game raises `SIGABRT` from `SpiVideoStreamDecoderBase` —
there is no SW fallback by design.

*Verified by:* JOURNAL Step 87 — daemon round-trip 49 fps NVDEC against `Lake_01_22_26_h265.mp4` (1940×1100, extradata=2490B); real run `runs/2026-05-06T12.45.39_hw_refactor` shows 449/551 frame/EAGAIN ratio across 1000 `avcodec_receive_frame` calls. RGBA single-plane wire format + host-side `frame.reformat('rgba')` introduced in Step 95; H.264 attract clips at 1360×768 and HEVC in-game clips at 1940×1100 / 1920×1080 confirmed flowing; H.264 v4l2m2m/v4l2request added to the decoder-name remap in Step 94m.

## Display pipeline (DRM / GBM / EGL / GLES)

**Data flow.** The game does its own real GLES rendering — no synthesized frames
are fed to it. The shim intercepts the hardware-specific framebuffer presentation
parts (DRM/KMS ioctls + GBM allocation) and replaces them with stand-ins, but
every `glClear` / `glDraw*` / shader / texture upload is forwarded to a **real GL
backend** that writes real pixels into a real framebuffer. At each fake page-flip
the shim does `glReadPixels` on the active backend's framebuffer and routes the
bytes to one or more sinks (frame bridge over abstract AF_UNIX, periodic on-disk
capture). GBM here is a handle stand-in — its BOs are `calloc(256)` scratch
pointers used as opaque tokens; the only GBM BOs that carry pixel data are the
1024×256 DMD path (`gbm_bo_map` returns `calloc(stride*height)` storage,
`gbm_bo_unmap` captures it). PROTOCOL "Display pipeline" lists the game-binary VA
reference points this path hooks.

Three layers: (1) `/dev/dri/card1` — DRM ioctls intercepted before they'd reach
the real device; (2) GBM library hooks — every `gbm_*` call goes to the shim;
(3) EGL + GLES library hooks — three modes (stub, Mesa-aarch64 overlay,
host-native daemon; launcher-selectable). All three cooperate so the game's
normal pipeline (allocate GBM BO → wrap as EGL image → bind as GL texture →
render → page-flip) succeeds end-to-end without real GPU hardware.

### `/dev/dri/card1` socketpair

`/dev/dri/card1` is opened as a real `socketpair(AF_UNIX, SOCK_STREAM)` (game
gets `sv[0]`, shim holds `sv[1]`). DRM ioctls are intercepted in `shim_ioctl.c`:

| ioctl | Shim response |
|---|---|
| `DRM_IOCTL_VERSION` | Mesa-shaped version string |
| `DRM_IOCTL_GET_CAP` / `SET_CLIENT_CAP` | success |
| `DRM_IOCTL_MODE_GETRESOURCES` | 1 CRTC, 2 connectors (both connected), 1 encoder, 1920×1080@60 |
| `DRM_IOCTL_MODE_GETCONNECTOR` / `GETENCODER` / `GETCRTC` / `SETCRTC` | matching modes |
| `DRM_IOCTL_MODE_CREATE_DUMB` / `ADDFB2` | new FB IDs |
| `DRM_IOCTL_MODE_PAGE_FLIP` (with `DRM_MODE_PAGE_FLIP_EVENT`) | writes a 32-byte `drm_event_vblank`-shaped event to `sv[1]` so the game's `select()` + `drmHandleEvent` advance. **Frame capture trigger** lives here (`shim_frame_capture_at_page_flip` every 2nd flip) |
| `DRM_IOCTL_MODE_MAP_DUMB` | `ENOTSUP` |
| `DRM_IOCTL_MODE_GEM_CLOSE` | success |

*Verified by:* JOURNAL Step 21, 24.

### GBM hooks

`gbm_create_device`, `gbm_surface_create` (+`_with_modifiers`), `gbm_bo_create`
(+`_with_modifiers`), `gbm_bo_destroy`, `gbm_bo_get_{width,height,stride,format,handle,modifier,fd}`,
`gbm_bo_map`, `gbm_bo_unmap`, `gbm_surface_lock_front_buffer`,
`gbm_surface_release_buffer`, `gbm_surface_has_free_buffers`, `gbm_device_destroy`,
`gbm_device_get_{fd,backend_name}`, `gbm_device_is_format_supported` all return
`calloc(256)` scratch-backed pointers so the game can dereference fields without
SEGV. Each BO is registered in a side-table (32 slots, mutex-guarded) with its
`(width, height, format, stride=width*4)` + `is_dmd` flag; `gbm_bo_get_*` consult
it so the DMD BO reports 1024×256 ABGR8888 instead of the screen default.
`gbm_bo_map` returns `calloc(stride*height)` backing on first map; `gbm_bo_unmap`
dumps `is_dmd` BOs to `../dmd/frame_NNNNNN.bgra` (1 MB each) and frees the backing.

### EGL hooks

| Function | Behavior |
|---|---|
| `eglGetDisplay` / `eglGetPlatformDisplay(EXT)` | Drops the game's GBM_KHR query; asks Mesa for `EGL_PLATFORM_SURFACELESS_MESA` |
| `eglInitialize`, `eglTerminate`, `eglBindAPI`, `eglQueryString` | success / Mesa-default strings |
| `eglChooseConfig` / `eglGetConfigs` / `eglGetConfigAttrib` | RGBA8 + `NATIVE_VISUAL_ID=0x34325258` (XR24); patches `EGL_SURFACE_TYPE: WINDOW_BIT → PBUFFER_BIT` (Mesa surfaceless has no window configs) |
| `eglCreateContext` / `eglDestroyContext`, `eglCreateWindowSurface` (→ `eglCreatePbufferSurface` 1920×1080), `eglCreatePbufferSurface`, `eglDestroySurface` | fake handles in stub mode; forwarded to Mesa in real-GL mode |
| `eglMakeCurrent` (with `draw=NULL` → fabricates fallback pbuffer from `shim_last_real_config`), `eglSwapBuffers`, `eglSwapInterval`, `eglQuerySurface`, `eglSurfaceAttrib` | normal |
| `eglGetError` | always `EGL_SUCCESS` |
| `eglGetProcAddress` | routes to shim-owned stubs for `eglGetPlatformDisplayEXT`, `eglCreateImageKHR` / `DestroyImageKHR`, `eglCreateSyncKHR` / `DestroySyncKHR` / `WaitSyncKHR` / `ClientWaitSyncKHR`, `glEGLImageTargetTexture2DOES`. Never forwards |
| `eglCreateImageKHR` (shim-owned) | Never forwards to Mesa. Parses `EGL_DMA_BUF_PLANE0_FD_EXT`; if the fd is one of `shim_avforward`'s sentinels, looks up the live RGBA buffer pointer + stride. Registers `{w, h, fourcc, rgba_ptr, stride}` in a side-table, returns a counter-cast opaque handle (`(void *)(0xdeadbeef0000 + 8N)`) — never `malloc`'d, never freed |
| `eglDestroyImageKHR` (shim-owned) | Drops the side-table entry. Sentinel handles aren't freed (counter-cast pointers, not heap allocations) |
| `glEGLImageTargetTexture2DOES` | Demotes EXTERNAL_OES → TEXTURE_2D. If the entry has an `rgba_ptr`, uploads via real `glTexImage2D` on first call for that bound `tex_id` at this `(w, h)`, then `glTexSubImage2D` after (per a 128-slot per-`tex_id` allocation table preserving storage across frames). Else uploads `NULL` storage for FBO color-attachment use. Tracks via `shim_frametex_record` for capture targeting |

### GLES 3 hooks (~70 entry points)

| Family | Functions | Behavior |
|---|---|---|
| Query | `glGetError`, `glGetString`, `glGetIntegerv`, `glGetShaderiv`, `glGetProgramiv` | Defaults; `compile/link status` always `GL_TRUE` so the Radium pipeline doesn't bail |
| Object create | `glGen*` / `glCreate*` families | Incrementing object IDs |
| Bind | all binding functions | Tracking only |
| Texture upload | `glTexImage2D`, `glTexSubImage2D`, `glCompressedTexImage2D`, `glCompressedTexSubImage2D`, `glTexParameteri`, `glPixelStorei` | Forward to Mesa in real-GL mode; capture data to disk |
| Shader | `glShaderSource`, `glCompileShader`, `glAttachShader`, `glLinkProgram`, `glUseProgram` | Compile/link always succeed |
| Uniforms | uniform family | tracking |
| Vertex | `glVertexAttribPointer`, `glEnableVertexAttribArray` | tracking |
| Framebuffer | family + `glCheckFramebufferStatus → GL_FRAMEBUFFER_COMPLETE` | success |
| State | enable/disable/blend/viewport/clear | tracking |
| Draw | `glDrawArrays`, `glDrawElements` | no-op in stub mode; forward in real-GL |
| Read | `glReadPixels` | the frame-bridge capture path uses this |

### GL execution modes

| Mode | Set via | Behavior |
|---|---|---|
| Stub | launcher-selectable | Every GLES/EGL/GBM call returns plausible defaults. Shaders "compile". Draws are no-op. Produces no real pixels |
| Mesa-aarch64 overlay | launcher-selectable (`GL_DAEMON=0`) | `FWD_RET` / `FWD_VOID` macros tail-call `dlsym(RTLD_NEXT, ...)` into the Mesa overlay at `mesa_sw_overlay/pkg/usr/lib/aarch64-linux-gnu/`. Game's EGL/GLES calls run inside qemu-aarch64 against an in-process Mesa → llvmpipe → a real EGL pbuffer (1920×1080) |
| Host-native daemon | launcher-selectable (`GL_DAEMON=1`, default) | Shim's GL/EGL hooks marshal opcodes over abstract AF_UNIX `@spike3_gl` to `gl_daemon.py` running natively on the host. Daemon binds an `EGL_PLATFORM_SURFACELESS_MESA` ES3 context and replays opcodes via PyOpenGL. EGL setup is synthesized in-shim (FAKE_EGL_* sentinels). See "Host-native GL daemon" below |

#### Cross-backend translation glue

The shim normalizes EGL/GLES traffic before forwarding to whichever backend is
active. The EGL-setup bullets apply only to the Mesa-overlay backend; the daemon
path synthesizes EGL state in-shim. The texture-target demotion and shader
rewrite apply to both real-GL backends.

- `eglGetDisplay` / `PlatformDisplay[EXT]` → drops the game's GBM_KHR query, asks Mesa for `EGL_PLATFORM_SURFACELESS_MESA`
- `eglChooseConfig` → patches `EGL_SURFACE_TYPE: WINDOW_BIT → PBUFFER_BIT`
- `eglCreateWindowSurface` → redirects to `eglCreatePbufferSurface(1920×1080)`
- `eglMakeCurrent(draw=NULL)` → fabricates fallback pbuffer from `shim_last_real_config`
- `eglGetConfigAttrib(EGL_NATIVE_VISUAL_ID)` → forces XR24 fourcc
- All texture-target hooks (`glBindTexture`, `glTexImage2D`, `glTexSubImage2D`, `glCompressedTex{,Sub}Image2D`, `glTexParameter{i,f}`, `glGenerateMipmap`, `glFramebufferTexture2D/Layer`, and the upload inside `glEGLImageTargetTexture2DOES`) demote `GL_TEXTURE_EXTERNAL_OES` (`0x8D65`) to `GL_TEXTURE_2D` (`0x0DE1`). Mesa swrast llvmpipe rejects every state-setting op other than `glEGLImageTargetTexture2DOES` on EXTERNAL_OES with `GL_INVALID_ENUM` (per `GL_OES_EGL_image_external`); demotion lets the texture object live as a 2D RGBA8 through the upload chain
- `glShaderSource` → concatenates the input strings; if the source references `samplerExternalOES`, injects `#define samplerExternalOES sampler2D` after the matching `#extension GL_OES_EGL_image_external` line (else after `#version`, else at top), then forwards a single rewritten string. Required because Mesa swrast llvmpipe samples `samplerExternalOES` from a 2D-bound texture as zero — the OES extension's distinguishing behavior is YUV→RGB on DMA-BUF YUV textures, irrelevant for our RGBA-from-host path. The game's Godot shaders use this `#define` unconditionally; Stern's Radium video shader doesn't, so the shim adds it

#### dlopen/dlsym interception (in `spi_shim.c`)

Tracks `libEGL` / `libGLES` / `libgbm` handles. Substitutes the shim's 3 EGLImage
overrides on `dlsym(handle, "eglCreateImageKHR" / "eglDestroyImageKHR" /
"glEGLImageTargetTexture2DOES")`. `eglGetProcAddress` returns the shim's stubs
(never forwards).

### Host-native GL daemon

In daemon mode (`GL_DAEMON=1`, default) the shim's GL/EGL hooks marshal opcodes
to `gl_daemon.py` over abstract AF_UNIX `@spike3_gl`. The daemon binds an
`EGL_PLATFORM_SURFACELESS_MESA` ES3 context (config `0x31DD`) and replays opcodes
via PyOpenGL; `glReadPixels` round-trips ship RGBA bytes back to the shim for the
frame bridge. Daemon logs land at `../runs/<TS>/gl_daemon.log`.

| Component | File | Role |
|---|---|---|
| Shim marshaller | `shim_gl_marshal.c` | `shim_gl_marshal_active()` (lazy connect with 3× retry + dlsym fallback), `_send_async()`, `_send_sync()` with mutex |
| Wire format | `gl_proto.h` (C) + `gl_proto.py` (Python) | 8-byte LE header `{u16 op, u16 flags, u32 payload_len}` + payload; flags bit 0 = SYNC, bit 1 = HAS_DATA; sync ops carry typed replies (`REP_OK`, `REP_VALUE`, `REP_LOC`, `REP_IDS`, `REP_PIXELS`, `REP_ERR`); opcode set covers EGL setup, GL state, object lifecycle, upload, draw + sync, uniforms, replies, readback |
| Host daemon | `gl_daemon.py` | `Renderer` binds the ES3 context and replays opcodes via PyOpenGL; `Session` is single-client; `Renderer.reset()` runs on every bind after the first (deletes all ids → fresh GL state per connection). Compressed-tex uploads bypass PyOpenGL's `CompressedImageConverter` and call `libGLESv2.so.2` via raw ctypes |
| Launcher | `run_game.sh` | Owns GL-daemon selection/defaults; spawns the daemon before qemu when enabled |

**Shim-side EGL synthesis.** The daemon owns the real EGL state; the shim returns
FAKE_EGL_* sentinel handles for every EGL setup/query/teardown call so the
qemu-side game/probe never sees the real EGL state on host. `glGet*` query hooks
return canned stub values in daemon mode (forwarding to an uninit Mesa overlay
returns garbage).

**Buffer mapping (shadow path).** `glMapBufferRange` / `glUnmapBuffer` can't
round-trip a host pointer — the shim allocates per-target scratch buffers
(`shim_gl_map_table[8]`), returns the scratch pointer to the game, and on
`glUnmapBuffer` with `GL_MAP_WRITE_BIT` marshals the scratch contents via
`OP_GL_BUFFER_SUB_DATA(target, offset, len, scratch)`.

**Zero/zero blend-factor handling.** During GL-daemon runs the renderer gets
repeated `glBlendFunc(GL_ZERO, GL_ZERO)` calls. Applying that literally keeps
`GL_BLEND` enabled but makes draws compute `src*0 + dst*0` → black attract scene;
disabling `GL_BLEND` instead makes it visible but draws RGBA textures opaquely,
exposing alpha-zero texels as black/white rectangles. The verified behavior is to
**ignore zero/zero blend-factor updates and keep the previous valid alpha blend
state** (`GL_SRC_ALPHA / GL_ONE_MINUS_SRC_ALPHA`). `gl_daemon.py` also emits
opt-in throttled logging for `glBindFramebuffer`, `glDrawArrays/Elements`,
`glBlendFunc/Separate`, `glBlitFramebuffer`, `glUniform4f/4fv`,
`glProgramBinary/glGetProgramBinary`, plus an FBO-scan diagnostic. Toggle
names/defaults live in `run_game.sh`.

*Verified by:* JOURNAL Step 96.1 (Phase 1–3c shim→daemon→Mesa pipeline + probes `gles_probe3..6`), Step 96.2 (EGLImage marshal + frame-capture relay + `Renderer.reset`), Step 97 (Phase 3d game-run iteration: EGL/query hook coverage, ES3 context, `glMapBufferRange` shadow buffer, compressed-tex ctypes bypass, full `comm -23` symbol audit closed), Step 98 + `runs/2026-05-10T11.23.55_auto_service_back_zero_blend/gl_daemon.log`.

### Capture pipeline

| Asset | Hook | Output |
|---|---|---|
| **DMD frames** (1024×256 BGRA) | `gbm_bo_map` detects 1024×256 ABGR8888 BO, returns `calloc(stride*height)`; capture in `gbm_bo_unmap` | `../dmd/frame_NNNNNN.bgra` (1 MB each) |
| **Radium shaders** (12 programs / 24 shader objects per init) | `glShaderSource` — concatenate `count × string × length` args; type recorded in `glCreateShader` | `../shaders/shader_NNN_idM_typeXXXX.glsl` |
| **Scene textures** (`StreamingImage` path, `glTexSubImage2D` at game VA `0x8a769c`) | `glTexImage2D` / `glTexSubImage2D` — dump `data` arg sized via the GLES bytes-per-pixel table | `../textures/tex_NNNNNN_WxH_fmtXXXX_typeYYYY.raw` |
| **Compressed textures** | `glCompressedTexImage2D` / `glCompressedTexSubImage2D` | `../textures/tex_compressed_NNNNNN_WxH_fmtXXXX.bin` |

### Frame bridge wire format

When the frame bridge is enabled, every 2nd `MODE_PAGE_FLIP` triggers
`glReadPixels(0, 0, 1920, 1080)` from `shim_last_draw_fbo` (or current
`READ_FRAMEBUFFER`) and writes one frame to `@pokemon_spike3_frame`. Under the
host GL daemon the readback is marshalled over `@spike3_gl` as a sync
`OP_GL_READ_PIXELS`; the daemon executes it and ships RGBA bytes back, which the
shim forwards unchanged. Wire format:

```
+0x00  uint32 LE  magic = 0xF00FBABA
+0x04  uint32 LE  seq number
+0x08  uint16 LE  width  (= 1920)
+0x0a  uint16 LE  height (= 1080)
+0x0c  uint16 LE  fourcc (RGBA8)
+0x0e  uint16 LE  flags
+0x10  W*H*4 bytes  RGBA payload
```

Subscriber: `frame_viewer.py` (Tk + numpy + Pillow). Self-resolves the abstract
socket name. Shows live frames in a 1920×1080 window (`VIEWER_FULLSCREEN=1` for
fullscreen).

### Observed at runtime

| Asset | Counts |
|---|---|
| Shader programs | 12 (24 shader objects: 12 vertex `type=0x8b31` + 12 fragment `type=0x8b30`) |
| RGBA texture uploads | 16 textures at 1024×{64,128,256,512} for the SternLogo splash (1920×1080) + UI surfaces |
| `GL_LUMINANCE_ALPHA` 256×256 atlases | 79 — Nuklear font glyph atlases |
| GBM BO sizes | 1024×256 (DMD), 1920×1080 (display) |

The DMD pipeline (`DMDEmulationScene` ctor at `0x68ca40`): allocates a 1 MB BGRA
buffer at object offset +0x30 → `create_gbm_bo_and_upload @ 0x8ffe80` →
`gbm_bo_map` writes pixel data row-by-row (with stride) → `gbm_bo_unmap` →
`gbm_bo_get_fd` → DMA-BUF fd → `eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT)` →
`glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES)`. Shim's `gbm_bo_map`
returns `calloc(stride*height)` backing; `gbm_bo_unmap` dumps to
`../dmd/frame_NNNNNN.bgra`.

*Verified by:* JOURNAL Step 24, 25.

## Runtime binary patches (`shim_patches.c`)

| Patch | Gate | Target |
|---|---|---|
| memcpy NULL-guard | always on | silently no-op when src/dst NULL — fixes the Godot DisplayRenderer thread crash on un-initialized TLS |
| `avcodec_find_decoder_by_name` remap | launcher-controlled | `"hevc_v4l2request"` / `"hevc_v4l2m2m"` / `"h264_v4l2request"` / `"h264_v4l2m2m"` → `"hevc"` / `"h264"` (SW decoders, so the forward-decode interceptor can engage) |
| `get_format` callback NOP | launcher-controlled | At game VA `0x920cf0`, write `0xd503201f` (NOP) over `0x54ffff81` (`b.ne 0x920ce0`). Patched by `shim_patch_get_format_callback`, called from `shim_inject.c::shim_discovery_init_thread` after a 2 s sleep |

In addition: passthrough hooks for `avcodec_get_hw_config` /
`av_hwdevice_ctx_create` (resolve via `dlvsym`, log, forward); forward-decode
interceptors for `avcodec_send_packet` / `avcodec_receive_frame` (route to the
host daemon via `shim_avforward.c` when launcher-enabled; bridge unreachable →
`-EIO`).

## Other hooks (in `spi_shim.c` + `shim_net.c`)

| Hook | Behavior |
|---|---|
| `system()` | Blocks `openocd` and `/games/spike3/` shell-outs (returns 0 so the game thinks RP2040 flashing succeeded); rewrites `/connectivity/` paths in commands to `$SYSROOT/connectivity/` |
| `pthread_create` | Forces `PTHREAD_INHERIT_SCHED` when the caller passed `EXPLICIT_SCHED` (qemu-user lacks `CAP_SYS_NICE`; otherwise glibc would silently drop the thread → `data_4882108` never set → orchestrator loops forever) |
| termios family | UART fd gets fake-OK because glibc INLINE_SYSCALLs bypass our ioctl hook |
| ALSA | full set of fake-success stubs; `snd_pcm_writei` captures + paces — see "Audio: shim ALSA stubs" |
| GStreamer | passthrough+log on `init_check` / `factory_make` / `parse_launch` / `bin_add` / `element_link` / `set_state` / `deinit`. **Linked but unused in PLE 0.81** — no static callers; the video path is 100% direct libavcodec via Stern's `spi_video_player` (`SpiVideoStreamDecoderBase`) plus Radium's own `VideoStreamDecoder` |
| `connect` / `getaddrinfo` (`shim_net.c`) | log-only passthrough for offline runs |

## Audio: shim ALSA stubs

The game's 18 playback-only ALSA symbols (see PROTOCOL "Audio pipeline") are
stubbed in `shim_inject.c`:
- `snd_pcm_open` returns a `calloc(256)` scratch handle with return value `0`
- `hw_params` setters/getters: no-op success
- `rate_near` writes 44100 to `*val`; `get_rate` / `get_channels` return 44100 / 2
- `snd_pcm_writei` paces with `usleep(size * 1e6 / 44100)` (≈46.4 ms per 2048-frame buffer), appends the raw S16_LE interleaved stereo buffer to `../captures/audio/stream.s16le`, flushes every 64 writes, and tees to the audio bridge if `AUDIO_BRIDGE=1`

To convert the captured stream to WAV: `ffmpeg -f s16le -ar 44100 -ac 2 -i stream.s16le stream.wav`.

*Verified by:* Step 27 run — 11.7 MB / 66.4 s of real-time audio captured in one run, audio thread did not spin, game progressed into scene-loader territory.

## Nodebus synth: per-slot chip-ID table (Pokémon LE)

`shim_nodebus.c::nodebus_chip_id_for_slot` returns the chip_id the shim writes
into `response[4..7]` of every synthesized FE-probe reply for populated slots.
`shim_nodebus.c::nodebus_slot_is_populated` is the shared slot-inventory gate:
cmd `0x25` enumerates the populated nonzero slots, and cmd `0x41` inner cmd `0xFE`
returns a chip/version payload for populated slots or an all-zero
not-populated FE payload otherwise. Each populated-slot chip_id matches a vtable
entry whose `+0x10` (type_idx — the value `sub_840d30` uses as `key2`)
corresponds to a shipped `.hex` firmware family. (The LPC chip vtable itself —
the 28 supported MCU IDs — is platform RE: PROTOCOL "LPC chip vtable".)

| Slot | Role (manual) | chip_id | vtable entry | type_idx | Firmware match |
|---:|---|---|---|---:|---|
| 0  | CPU bridge | `0x00140040` | LPC1124_303 (15) | 4 | host-side bridge LPC on CPU board |
| 1  | Cabinet | `0x042D502B` | LPC1112_101 (2) | 1 | `pinnode-LPC1112_101-1_31_0.hex` |
| 2  | Cabinet-Express | `0x2C40102B` | LPC1313 (13) | 5 | `ws2812node-LPC1313-1_31_0.hex` |
| 4  | QR Reader (Bottom Arch) | `0x00140040` | LPC1124_303 (15) | 4 | `node4-LPC1124_303-1_31_0.hex` |
| 7  | Speaker-Expression | `0x2C40102B` | LPC1313 (13) | 5 | `ws2812node-LPC1313-1_31_0.hex` |
| 8  | Core Driver Lower Playfield | `0x042D502B` | LPC1112_101 (2) | 1 | `pinnode-LPC1112_101-1_31_0.hex` |
| 9  | Core Driver Upper Playfield | `0x042D502B` | LPC1112_101 (2) | 1 | (same record as slot 8) |
| 10 | SPI Servo Node Board | `0x042D502B` | LPC1112_101 (2) | 1 | (same record as slot 8) |
| 12 | Topper-Lamps | `0x2C40102B` | LPC1313 (13) | 5 | `ws2812node-LPC1313-1_31_0.hex` |
| 13 | Topper-Servo | `0x042D502B` | LPC1112_101 (2) | 1 | `pinnode-LPC1112_101-1_31_0.hex` |
| 14 | Universal Topper | `0x2C40102B` | LPC1313 (13) | 5 | `ws2812node-LPC1313-1_31_0.hex` |
| any other | — | all-zero FE response | — | — | not populated |

Result: all 11 populated slots reach `status=2 + flag=0x03` — every row hidden in
Tech Alerts; game advances past Tech Alerts to the Service Menu (PROTOCOL
"Game runtime model" → "Tech Alerts row dispatcher" for the status writer's
success path; bridge cmd `0x25` enumeration drives the flag-bit-1 paths).

LPC1342 entries (vtable 21/22, chip_id `0x3D01402B`) have `type_idx=0` → using
that chip_id causes a `sub_840d30` lookup miss (no shipped firmware record has
`key2=0`) → status 4. Pinnode slots therefore take the simple chip_id path via
LPC1112_101 (type_idx=1), which bypasses the LPC1313 special branch
(`sub_8f5390` cmd `0xF2 × 8`) in `sub_674680`. ws2812node slots take the real
LPC1313 chip_id (`0x2C40102B`, vtable entry 13) since the only ws2812node
firmware shipped is LPC1313. Topology per cabinet manual: `0 → 10 → 9 → 8 → 4`
(playfield daisy chain via CPU CN22), `0 → 1` (cabinet branch via CPU CN11).

Synth requires a wall-clock pause before each boot-time `bridge_send_response`
to preserve the timeout-driven timing real hardware imposes — empirically 200 ms
(`shim_nodebus.c::NODEBUS_SYNTH_DELAY_US`). Immediate replies regress the run from
frame-decode-active to "Godot init never fires". After the first nodebus
switch-poll response, `shim_nodebus.c::nodebus_post_init_fast_replies` switches
later synthetic nodebus replies to a shorter delay for service-menu scans and
steady-state polling.

*Verified by:* JOURNAL Step 92 — runtime vtable dump (`game_base + 0x42317c8`, 28 entries × 0x28) confirms `type_idx` per entry; runtime probe of all 11 populated slots reaching `flag=0x03 status=2` after shim synthesis. Visual confirmation: Tech Alerts panel renders empty + game advances to the Service Menu.

## Driving cmode state from outside

To simulate a switch event from an external source: (1) determine the event_id
for the switch; (2) call the linked list at `data_485ea80 + (event_id * 8)` —
every registered handler fires in priority order; (3) handlers call vtable
methods on the 42 cmodes; cmode state changes; (4) next tick, `sub_94f140`
iterates and each cmode's `+0xd8` updates its display. The shim's `state_bridge`,
when launcher-enabled, polls cmode `+0x308` at 10 Hz and emits JSON deltas on
`@pokemon_spike3_bridge`. (See PROTOCOL "Game runtime model" for the cmode
registry and `fire_event` dispatcher these touch.)

## Asset extraction (spike_extract)

`_extraction/tools/spike_extract/` is a pure-Python standalone extractor for
`image.bin`. **No emulator, no QEMU, no game binary required at extraction
time** — it parses `image.bin` section 7 directly (see PROTOCOL "image.bin file
format") and writes classified output.

```bash
python -m spike_extract --image games/pokemon_le/image.bin --out out/
# with registry-dump validation (optional; confirms the section-7 parse against runtime):
python -m spike_extract --image games/pokemon_le/image.bin \
    --registry games/pokemon_le/_emulation/asset_registry.bin --out out/ --csv out/index.csv
# dry-run — print summary + histogram, write nothing:
python -m spike_extract --image games/pokemon_le/image.bin --dry-run
```

`asset_registry.bin` (optional input) is produced by the shim's
`shim_dump_sound_globals` (walks the runtime `_Hashtable` + `_M_nxt` chain). Used
only to validate that the section-7 parser recovered the same 2,452 entries the
runtime hashmap holds — on a correct parse: 100% intersection, zero mismatches.

Output layout:

```
out/
├── audio_stereo_music/   # 35 WAVs (4-byte/elem stereo, duration ≥ 30 s)
├── audio_stereo_sfx/     # 312 WAVs (4-byte/elem stereo, duration < 30 s)
├── audio_mono/           # 2,105 WAVs (2-byte/elem, audio shape)
└── index.csv             # 2,453 lines (header + 2,452 records)
```

Subdirectories are created lazily — empty categories leave no folder.

`index.csv` columns: `index, file_offset, asset_id_low, flags, encoded_id,
size_elements, element_bytes, byte_size, hash_or_cksum, category` —
`element_bytes` is 4 if `asset_id_low & 0x40` else 2; `byte_size =
size_elements * element_bytes`; `encoded_id = (flags << 32) | asset_id_low`;
`category` ∈ `audio_stereo_music` / `audio_stereo_sfx` / `audio_mono`.

PLE 0.81 extraction summary: 35 music (331 MB) + 312 stereo SFX (180 MB) + 2,105
mono (566 MB) = 2,452 records / 1.1 GB — matches the image.bin section-7 record
count exactly; every section-7 record is audio; nothing encrypted.

Open gaps: `SE_PK_*` name → asset_id mapping is **not** in compiled code (token
resolution happens at runtime via type-0x0b records during scene-bytecode
walking) — static extraction recovers `(asset_id_low, flags, size, file_offset)`
but not the names; only runtime capture / a CM4 memory dump yields the full
name→asset_id table. Non-audio content (scene bytecode in section 5, playfield
grids in section 2) isn't extracted by this tool.

Portability: the section-7 format is stable across Stern's Spike 3 codebase, so
the parser works on any Spike 3 `image.bin`; per-game tuning needed only on the
classification heuristic. For games shipping `image-sc??.bin` sub-files, each is
parsed the same way with its own section-7 directory.

*Verified by:* `_extraction/tools/spike_extract/__main__.py` arg handling; `_extraction/spike_extract_out/index.csv` (2,452 rows + header) + manual audition per category.

