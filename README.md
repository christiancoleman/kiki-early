# `_emulation/` — Spike 3 emulation shim

This directory is the dev workspace for running the Pokémon LE `game` binary
(ARM aarch64, Spike 3 platform) on an x86_64 dev machine under `qemu-user`.
The core artifact is `spi_shim.so` — an `LD_PRELOAD` shim that intercepts the
game's calls to hardware devices, libraries, and files it can't reach on a
workstation, and synthesizes whatever the game expects.

## Get started

1. One-time environment setup: see [`docs/SETUP.md`](docs/SETUP.md).
2. Build the shim: `./build.sh`
3. Run the game: `./run_game.sh` (defaults to a 185 s real-GL run with all
   patches enabled; knobs documented at the top of the script).
4. Optional live frame viewer (second terminal): `python3 frame_viewer.py`
5. Daily log of what each step was trying + what we learned: [`docs/JOURNAL.md`](docs/JOURNAL.md).

## Module guide

The shim started life as a single `spi_shim.c` monolith (~6400 lines). We
split it into subsystem-scoped source files across nine phases — **85 % of
the original code now lives in dedicated subsystem files**, with `spi_shim.c`
acting as the small nucleus that holds the constructor, device-state globals,
`intercept_device`, and the remaining INLINE_SYSCALL-dodging hooks (termios,
ALSA, gstreamer, pthread_create, system).

> **Terminology:** "source file" here means a C compilation unit — one `.c`
> file that the compiler processes into one object, all of which the linker
> bundles into `spi_shim.so`. Externally the result is one library;
> internally the split keeps subsystems legible.

Each file is self-contained in its own concern and depends on the others
only via declarations in `spi_shim.h`.

### `spi_shim.h` — shared header

The only thing that crosses the source-file boundary. Holds:

- `SHIM_LOG(...)` — uniform `[shim]` prefix on every diagnostic line.
- `FWD_RET` / `FWD_VOID` / `FWD_RET_V` / `FWD_VOID_V` — macros used inside
  GL/EGL stub bodies to tail-call the real Mesa implementation when
  `SPIKE3_REAL_GL=1`, or fall through to the stub when not.
- Display constants (`FAKE_MODE_HDISPLAY`, `FAKE_MODE_VDISPLAY`).
- `FAKE_FD_*` constants for SPI / I2C / V4L2 / GPIO fake-fd pools.
- `struct shim_cmode_entry` + externs for the game's 42-mode registry.
- Device-layer state externs (spi_active, uart_game_fd, drm_game_fd,
  transfer counters, etc.) that cross-TU hooks need.
- `real_open` / `real_close` / `real_ioctl` / `real_read` / `real_write`
  function pointer externs — resolved by `resolve_all()` on first open.
- Forward declarations for cross-module functions: bridge lifecycle,
  signal handlers, netbridge response synth, intercept dispatcher, fd
  predicates, GPIO fake-fd pool operations.

### `spi_shim.c` — main shim body (~960 lines, down from 6400)

The small nucleus that remains after nine phases of carving. Owns:

- `__attribute__((constructor))` entry points: directory setup
  (`shim_init_dirs`) and the main `shim_install_crash_handler` that
  registers SIGSEGV/SIGBUS/SIGTERM/SIGINT/SIGUSR1/SIGUSR2 handlers, then
  delegates to `shim_bridge_start()`, `shim_frame_start()`, and
  `shim_inject_start()` for the rest of the bring-up.
- `dlopen` / `dlsym` interceptions for dynamically-loaded libEGL/GLES/GBM
  handles (Step 59): the game `dlopen`s these libs and looks up symbols
  against the returned handles, which normal LD_PRELOAD misses; we route
  specific symbols (`eglCreateImageKHR`, `glEGLImageTargetTexture2DOES`)
  back to our overrides.
- `intercept_device()` — the `/dev/*` → fake-fd dispatcher called by the
  open family in `shim_fs.c`.
- fd predicates (`is_fake_fd` / `is_uart_fd` / `is_drm_fd` / `is_v4l2_fd`)
  plus all the device-layer state globals they read.
- `resolve_all()` — binds the five `real_*` libc function pointers.
- termios wrappers (`tcsetattr` / `tcgetattr` / `tcflush` / etc.) —
  glibc uses INLINE_SYSCALL for these, bypassing our ioctl hook.
- `system` hook that blocks `openocd` / `/games/spike3/` shell-outs.
- `pthread_create` hook forcing `PTHREAD_INHERIT_SCHED`.
- `snd_pcm_*` ALSA stubs + paced writei capture to
  `_emulation/audio/stream.s16le`.
- `perror` diagnostic wrapper.
- GStreamer passthrough + logging.

### `shim_fs.c` — filesystem intercept layer (Phase 5)

Every intercept on the path namespace:

- Sysroot-aware `remap_path` helper (prepends `$SPIKE3_SYSROOT` to
  absolute paths, skipping `/dev/`, `/proc/`, `/sys/`, `/tmp/`).
- Path-open family: `open`, `openat`, **`open64`**, **`openat64`**,
  **`fopen64`**. The 64-bit variants are essential — the game's Godot
  resource loader uses `fopen64` to reach `main.pck`, so the plain
  `open` hook alone misses it.
- Access family: `access`, `faccessat`.
- Stat family: `stat`, `lstat`, `__xstat`, `__lxstat`, `__xstat64`,
  `__lxstat64`, `fstatat`, `__fxstatat`, `__fxstatat64`.
- GPIO sysfs fake-fd pool: allocation / lookup / free helpers + the
  arrays backing the pool. The 16-slot pool plus its per-slot path
  string (`gpio_fake_paths[]`) and read-done latch (`gpio_fake_read_done[]`)
  let the I/O hooks serve sysfs `value`/`direction`/`export` files.
- `should_log_open` — diagnostic filter choosing which opens are worth
  logging (device nodes, sysfs/procfs, Godot resources, etc.).

All open-family hooks call `intercept_device()` first so SPI/I2C/DRM/UART
path matches land on the right fake-fd / socketpair before falling through
to the real libc.

### `shim_io.c` — byte-level I/O hooks (Phase 7)

`close`, `read`, `write`, `lseek` plus the LFS variants `lseek64` /
`pread64` / `pwrite64` (added defensively in Phase 7.post). The first run
with the LFS hooks wired caught **`pread64` actually in use** — a real
blind spot we'd been silently missing. `lseek64` and `pwrite64` have stayed
dormant. The `pread64` hook now emits `fd=N offset=M count=K` for its first
8 calls plus every 500th after that, so future investigation can map where
those reads are actually coming from.

Dispatch order mirrors `intercept_device()`'s contract: GPIO fake fds →
SPI/I2C/V4L2 fake fds → UART socketpair (with bridge-response synth on
write) → DRM socketpair → everything else falls through to real libc.

### `shim_ioctl.c` — ioctl dispatch + DRM page-flip event synthesis (Phase 7)

`ioctl()` for every fake or real fd the shim exposes:

- UART termios ioctls (`TCGETS` / `TCSETS` / `TIOCMGET` / `FIONREAD` / etc.)
- SPI ioctl suite (`WR_MODE` / `WR_MAX_SPEED_HZ` / `MESSAGE_1` — **this is
  where the single-byte fake nodebus response lives**, `rx[2]=0x04`, known
  gap).
- DRM ioctls: `VERSION`, `GET_CAP`, `MODE_GETRESOURCES`, `MODE_GETCONNECTOR`,
  `MODE_GETCRTC`, `MODE_SETCRTC`, `MODE_CREATE_DUMB`, `MODE_ADDFB` /
  `ADDFB2`, `MODE_PAGE_FLIP` (triggers frame capture), `GEM_CLOSE`, …
- V4L2 first-pass probe (log + `ENOTTY`).
- I2C ioctls (`SET_SLAVE`, `RDWR`, `SMBUS`).

Also owns `drm_synthesize_flip_event` which pushes `DRM_EVENT_FLIP_COMPLETE`
records onto the shim side of the DRM socketpair on every `MODE_PAGE_FLIP`,
so the game's `select()` + `drmHandleEvent()` loop makes forward progress.

### `shim_gl.c` — OpenGL ES + EGL + GBM (Phase 6, the big one)

Combines three historically-separate subsystems into one source file
because they share a lot of state (real-GL forwarding globals + fallback-
pbuffer machinery).

- **GBM**: fake `gbm_device` / `gbm_bo` / `gbm_surface` objects. Tiny
  per-BO side table for `gbm_bo_get_width/height/format/stride`.
- **EGL**: `SURFACELESS_MESA` display translation (Step 57+), Mesa config
  capture, `eglChooseConfig` WINDOW→PBUFFER surface-type patching,
  fallback-pbuffer injection at `eglMakeCurrent(draw=NULL)` (Step 59).
  Plus the KHR image extension: `eglCreateImageKHR` fakes DMA-BUF import
  with a regular GL texture.
- **GLES 3.x**: the big pile of stubs. In stub mode (`SPIKE3_REAL_GL=0`)
  each returns a plausible value; in real-GL mode the `FWD_*` macros
  short-circuit to `dlsym(RTLD_NEXT, ...)` forwarding to Mesa.
- `shim_frametex` texture tracker — per-texture `{tex_id, target, w, h}`
  map populated from `glEGLImageTargetTexture2DOES` so the page-flip frame
  capture in `shim_bridges.c` can pick the main render target.
- `shim_last_draw_fbo` (exported) — set by `glBindFramebuffer`, read by
  `shim_frame_capture_at_page_flip` in `shim_bridges.c`.
- `eglGetProcAddress` routing table so the game's extension lookups land
  on our overrides even when it bypasses the dynamic linker.

### `shim_diagnostics.c` — crash dumps + game-state inspection (Phase 3)

Concentrates all *observe-the-game* machinery:

- `shim_safe_read` / `shim_hexdump_stderr` — guest-memory dump helpers.
- `shim_dump_sound_globals` — Pokémon LE sound-registry snapshot.
- `shim_cmode_table[]` — the 42-entry BN-extracted C-mode map (addresses,
  flag bytes, human-readable names).
- `shim_resolve_game_base()` — caching BN VA → runtime-address resolver.
- `shim_dump_cmodes` / `shim_dump_dispatch_tables` — verbose state dumps.
- `shim_sigsegv_handler` — SIGSEGV/SIGBUS handler: dumps, then `longjmp`
  escapes back out of a faulting event-sweep iteration.
- `shim_term_dump_handler` — SIGTERM / SIGINT / SIGUSR1 / SIGUSR2 handler
  (dumps on the first three; runs `shim_event_sweep` on SIGUSR2).

### `shim_inject.c` — active injection + scheduled probing (Phase 9)

Everything the shim pushes *into* the running game, as opposed to code
that merely intercepts calls coming out. Ends up being the biggest of the
late-phase carves:

- Pokémon LE 105-switch manifest (transcribed from manual 500-55AJ-01)
  plus `shim_find_switch` / `shim_fire_switch` — fires a simulated
  switch closure by poking the game's in-memory switch state array
  (`data_4880220 + id*0x30 + 0x28`) and diffs cmode state before/after.
- Event-sweep machinery (Step 52): `shim_fire_event` trampoline to the
  game's in-binary `fire_event` at BN VA `0x9c12a0`, `shim_event_sweep`
  that blind-fires every event ID 0..0xce with `event_data=0`, and per-
  event SIGALRM timeout + SEGV recovery via `sigsetjmp`/`siglongjmp`.
- The three event-sweep globals whose `extern`s live in `spi_shim.h`
  (`shim_event_sweep_jmp`, `shim_event_fire_in_progress`,
  `shim_current_sweep_event_id`) — read by `shim_sigsegv_handler` in
  `shim_diagnostics.c` to longjmp out of a faulting sweep iteration.
- Six env-gated worker threads:
  - `SPIKE3_SWEEP_EVENTS=1` + `SPIKE3_SWEEP_AFTER=<sec>` — delayed sweep.
  - `SPIKE3_FIRE_SWITCH=<id[:id:...]>` — fire one or more switches.
  - `SPIKE3_FORCE_GSTREAMER=<sec>` — trampoline into `gstreamer_init`.
  - `SPIKE3_POKE_CMODE=<name>:<hex>:<delay_s>` — force a cmode field.
  - `SPIKE3_POKE_STATE_FLAG=<mask>:<delay_ms>` — OR bits into
    `system_state_flags`.
  - `SPIKE3_PATCH_SPI_VIDEO=1` — after a 2 s settle, call into
    `shim_patches.c` to install the Step 62 vtable patch and Step 64
    `get_format` NOP.
- The SIGALRM handler used by the sweep to break out of hung event
  handlers (installed by `shim_inject_start`, blocked on the main
  thread so only the sweep-timer thread receives it).
- `shim_inject_start()` — the single public entry point called from
  `shim_install_crash_handler`. Registers SIGALRM, blocks it on the
  main thread, and spawns each env-gated worker thread. All six env
  vars are independent; any subset can be set on a given run.

### `shim_patches.c` — runtime patches to the game (Phase 8)

The "surgery" bucket — hooks and byte-level patches that alter game
behavior at runtime:

- **`memcpy` NULL-guard** (Step 45): the Godot error-logger path on the
  DisplayRenderer thread hits `memcpy(dst=NULL, src=NULL)` because Godot
  TLS singletons aren't registered on that thread. The hook returns
  unchanged for NULL args, forwards to real libc `memcpy` otherwise.
  Counted and logged (first 8, then every 1000th).
- **Step 62 `SpiDecoder::Play` vtable patch**
  (`shim_patch_spi_video_vtable`): replaces vtable slot `+0x28` with a
  stub that sets `update_thread_running=1` and `state=PLAYING` instead
  of polling for 250 ms on a flag that never sets under our shim. Gated
  by `SPIKE3_PATCH_SPI_VIDEO=1`.
- **Step 64 `get_format` callback NOP patch**
  (`shim_patch_get_format_callback`): NOPs the `b.ne 0x920ce0` at BN VA
  0xd20cf0 so the callback accepts the first pix_fmt libavcodec offers
  instead of insisting on DRM_PRIME. Safety check: refuses to patch
  unless the instruction is exactly the expected `0x54ffff81`. Gated by
  `SPIKE3_SW_HEVC=1`.
- **libavcodec HW-accel bypass** (Step 47): hooks
  `avcodec_find_decoder_by_name` (to swap `hevc_v4l2request` → `hevc`
  when `SPIKE3_SW_HEVC=1`), `avcodec_get_hw_config`,
  `av_hwdevice_ctx_create`, `avcodec_send_packet`, and
  `avcodec_receive_frame` — currently passthrough + logging except for
  the name swap.
- **Dummy-frame injection** (Step 47b, `SPIKE3_INJECT_VIDEO=1`): when
  the real decoder returns EAGAIN and a packet is pending,
  `avcodec_receive_frame` returns a shim-managed NV12 1920×1080 AVFrame
  (black Y/neutral UV) with a monotonic PTS so the downstream
  video path doesn't stall. `shim_init_dummy_frame` uses
  `av_frame_alloc` / `av_frame_get_buffer` / `av_frame_ref` resolved
  via `dlvsym(..., "LIBAVUTIL_57")`.

Both Step 62 and Step 64 patches translate BN VA → runtime address via
`shim_resolve_game_base()` (defined in `shim_diagnostics.c`) and
`mprotect` the page to PROT_READ|WRITE|EXEC before writing. They're
called from `shim_vp_patch_thread` in `spi_shim.c`.

## Refactor progress at a glance

| Phase | Carve | Lines moved |
|---|---|---|
| 1 | `spi_shim.h` header extraction (SHIM_LOG + FWD_* macros) | 41 |
| 2 | `shim_bridges.c` (state bridge + frame bridge) | 462 |
| 3 | `shim_diagnostics.c` (crash handlers + cmode table + dumps) | 722 |
| 4 | `shim_nodebus.c` (netbridge protocol response synth) | 384 |
| 5 | `shim_fs.c` (path/open/stat/access + GPIO pool) | 441 |
| 6 | `shim_gl.c` (GBM + EGL + GLES + KHR image + eglGetProcAddress) | 1735 |
| 7 | `shim_io.c` + `shim_ioctl.c` (fd I/O + ioctl dispatch + DRM events) | 654 |
| 8 | `shim_patches.c` (memcpy NULL-guard + Step 62 vtable + Step 64 NOP + libavcodec hooks + dummy-frame injection) | 501 |
| 9 | `shim_inject.c` (switch manifest + event sweep + 6 env-gated worker threads + SIGALRM handler) | 770 |

Each phase was regression-tested against `runs/baseline/REFACTOR_BASELINE.txt`
— the post-Phase-6 run log captured before any carving. Every phase has
matched the baseline within timing-variance noise (`gpio` retry count ±4,
frame count ±30) on all 22 error classes + all 7 hook-fired log lines.
Per-phase deltas are preserved in `runs/baseline/PHASE{1..9}_REPORT.txt`.

**The refactor is complete.** `spi_shim.c` is down to ~960 lines of
device-state + device-intercept dispatcher + INLINE_SYSCALL-dodging
termios/ALSA/gstreamer/pthread/system hooks.

## Defensive-hook pattern: 32/64-bit variants

glibc's Large File Support (LFS) APIs exist as distinct PLT symbols from
their plain counterparts — e.g. `open` vs `open64`, `lseek` vs `lseek64`,
`pread` vs `pread64`, `fopen` vs `fopen64`. On 64-bit aarch64 they're
functionally identical (`off_t` is already 64-bit, so `off64_t` is the same
width), **but the symbol resolution is separate**. A game that's linked
against the 64-bit variant will silently bypass a shim hook on the plain
variant, and vice versa.

**Rule of thumb: when you hook one variant, hook both.** A hook added on the
previously-unhooked variant should carry a `FIRST CALL` banner for at least
one smoke-run so we observe whether it fires at all. The proactive
`lseek64` / `pread64` / `pwrite64` additions in Phase 7.post caught
`pread64` in active use the moment they were wired — a whole category of
file reads we'd been missing across the refactor.

**Current asymmetries worth closing later** (all low effort — ~15 lines
each — but each may or may not surface new behavior):

| Plain | 64-bit | Status |
|---|---|---|
| `pread` | `pread64` | Hook the plain variant; mirror the 64-bit semantics + same `FIRST CALL` banner |
| `pwrite` | `pwrite64` | Same as pread |
| `fopen` | `fopen64` (shim_fs.c) | **Highest priority** — path-taking function. An un-hooked `fopen` can leak un-remapped absolute paths through to libc. |

The all-hooked pairs for reference (don't re-open these):
`open` / `open64`, `openat` / `openat64`, `lseek` / `lseek64`, the whole
stat/xstat/lxstat/fstatat family with `*64` cousins.

## Other documents in this directory

| File | Purpose |
|---|---|
| [`docs/SETUP.md`](docs/SETUP.md) | One-time dev-environment setup |
| [`docs/JOURNAL.md`](docs/JOURNAL.md) | Step-by-step log of every attempt, success, and failure |
| [`docs/PROTOCOL.md`](docs/PROTOCOL.md) | Verified on-wire protocol facts (netbridge, DRM events, etc.) |
| [`docs/MODE_TABLE.md`](docs/MODE_TABLE.md) | The 42 C-modes — names, addresses, flag meanings |
| [`docs/HANDOFF.md`](docs/HANDOFF.md) | Current-state snapshot for picking up the work cold |
| [`docs/ASSETS.md`](docs/ASSETS.md) | Running log of what assets the game loads |
| [`docs/EVENT_SWEEP_STEP52.md`](docs/EVENT_SWEEP_STEP52.md) | Write-up of the Step 52 event-sweep approach |
| [`docs/VIDEO_CLIPS.md`](docs/VIDEO_CLIPS.md) | Catalogue of the 229 extracted video clips |
| `runs/baseline/REFACTOR_BASELINE.txt` | Error-count reference the refactor is regression-tested against |
| `runs/baseline/PHASE*_REPORT.txt` | Per-phase refactor smoke-run diff vs. baseline |
| `runs/baseline/historic_frame_counts.txt` | Per-phase frame-dir sizes preserved when the PNGs themselves were pruned on 2026-04-23 |
| `reference/Binary Ninja Notebook Export (*).json` | Snapshot of the BN analyst's working notebook |

## Restoring legacy capture hooks (shaders / textures / DMD)

Step 56 removed three data-capture pipelines that had been useful early but
were duplicative of `_extraction/` asset extraction:

| Capture | Previous output path | Hook that wrote it |
|---|---|---|
| Shader source (GLSL) | `_emulation/shaders/shader_NNN_idM_typeXXXX.glsl` | `glShaderSource` in `shim_gl.c` |
| Texture pixel data | `_emulation/textures/tex_NNNNNN_WxH_fmtXXXX_typeYYYY.raw` | `glTexImage2D` / `glTexSubImage2D` in `shim_gl.c` |
| Compressed texture | `_emulation/textures/tex_compressed_NNNNNN_fmtXXXX.bin` | `glCompressedTexImage2D` / `glCompressedTexSubImage2D` in `shim_gl.c` |
| DMD frame (BGRA) | `_emulation/dmd/frame_NNNNNN.bgra` (1024×256×4 = 1 MB/frame) | `gbm_bo_unmap` in `shim_gl.c` when the BO was tagged `is_dmd` |

The physical dumps were deleted on 2026-04-23; the hook code was already
gone since Step 56. To restore:

1. Look at the git history for `shim_gl.c` around Step 56 (or the pre-Phase-6
   monolith) — the hooks had roughly this shape: track shader/texture IDs in
   a side table, then dump payload bytes on first write.
2. Re-add the capture calls to the existing `glShaderSource`/`glTexImage2D`/
   `gbm_bo_unmap` stub bodies in `shim_gl.c`.
3. Re-add the output-directory `mkdir`s in `shim_init_dirs()` in `spi_shim.c`
   (`_emulation/captures/{shaders,textures,dmd}`).

`docs/ASSETS.md` has the detailed per-format byte-count tables the hooks
used to size their dumps.

## Environment variables at runtime

Defaults baked into `run_game.sh`; override with `VAR=value ./run_game.sh`.

| Variable | Effect |
|---|---|
| `SPIKE3_SYSROOT` | Absolute path the shim remaps `/data/...` etc. against. Set automatically by the runner. |
| `SPIKE3_REAL_GL=1` | Forward EGL/GLES/GBM to real Mesa (requires the overlay built). Default on. |
| `SPIKE3_FRAME_BRIDGE=1` | Expose `/tmp/spike3_frame.sock`. Default on. |
| `SPIKE3_BRIDGE=1` | Expose cmode-state JSON bridge on `/tmp/spike3_bridge.sock`. Default off. |
| `SPIKE3_PATCH_SPI_VIDEO=1` | Apply the Step 62 `SpiDecoder::Play` vtable patch. Default on. |
| `SPIKE3_SW_HEVC=1` | Apply Step 64 get_format NOP so software HEVC decode proceeds. Default on. |
| `SPIKE3_POKE_CMODE=name:hex:delay_s` | Force a C-mode's `+0x308` field to `hex` after `delay_s` seconds. |
| `SPIKE3_POKE_STATE_FLAG=mask:delay_ms` | OR `mask` into `system_state_flags` after delay. |
| `SPIKE3_FIRE_SWITCH=id[:id:...]` | Fire one or more switch closures after startup. |
| `SPIKE3_SWEEP_EVENTS=1` + `SPIKE3_SWEEP_AFTER=<sec>` | Kick off a blind event-sweep after N seconds. |
| `SPIKE3_FORCE_GSTREAMER=<sec>` | Trampoline into the game's gstreamer_init after N seconds (diagnostic). |
| `SPIKE3_INJECT_VIDEO=1` | Enable dummy-frame injection when `receive_frame` returns EAGAIN. Default off. |

