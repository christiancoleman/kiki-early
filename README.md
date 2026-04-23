# `_emulation/` — Spike 3 emulation shim

This directory is the dev workspace for running the Pokémon LE `game` binary
(ARM aarch64, Spike 3 platform) on an x86_64 dev machine under `qemu-user`.
The core artifact is `spi_shim.so` — an `LD_PRELOAD` shim that intercepts the
game's calls to hardware devices, libraries, and files it can't reach on a
workstation, and synthesizes whatever the game expects.

## Get started

1. One-time environment setup: see [`SETUP.md`](SETUP.md).
2. Build the shim: `./build.sh`
3. Run the game: `./run_game.sh` (defaults to a 185 s real-GL run with all
   patches enabled; knobs documented at the top of the script).
4. Optional live frame viewer (second terminal): `python3 frame_viewer.py`
5. Daily log of what each step was trying + what we learned: [`JOURNAL.md`](JOURNAL.md).

## Module guide

The shim started life as a single `spi_shim.c` monolith (~6400 lines). We've
been splitting it into subsystem-scoped source files — as of Phase 7, **65 %
of the original code now lives in dedicated subsystem files**, with
`spi_shim.c` acting as the small nucleus that holds the constructor, device-
state globals, and the intercepts that haven't landed their own file yet.

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

### `spi_shim.c` — main shim body (~2200 lines, down from 6400)

The small nucleus that remains after seven phases of carving. Owns:

- `__attribute__((constructor))` entry points: directory setup
  (`shim_init_dirs`) and the main `shim_install_crash_handler` that
  registers all signal handlers + spawns the various env-gated threads
  (state bridge, frame bridge, event sweep, switch firing, force-gstreamer,
  cmode poke, state-flag poke, vp-patch).
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
- Switch manifest (Pokémon LE 105-switch table with node addresses) +
  `shim_fire_switch` + `shim_fire_event` trampoline + event sweep
  machinery (Step 52).
- Poke threads: `SPIKE3_POKE_CMODE`, `SPIKE3_POKE_STATE_FLAG`,
  `SPIKE3_FIRE_SWITCH`, `SPIKE3_FORCE_GSTREAMER`.
- Step 62 `SpiDecoder::Play` vtable patch + Step 64 `get_format` NOP
  patch + `shim_vp_patch_thread` that applies them after a 2-sec settle.
- termios wrappers (`tcsetattr` / `tcgetattr` / `tcflush` / etc.) —
  glibc uses INLINE_SYSCALL for these, bypassing our ioctl hook.
- `system` hook that blocks `openocd` / `/games/spike3/` shell-outs.
- `pthread_create` hook forcing `PTHREAD_INHERIT_SCHED`.
- `snd_pcm_*` ALSA stubs + paced writei capture to
  `_emulation/audio/stream.s16le`.
- `perror` diagnostic wrapper.
- `memcpy` NULL-guard (Step 45).
- libavcodec HW-accel bypass (Step 47) + software HEVC redirect (Step 64)
  + dummy-frame injection (Step 47b, gated by `SPIKE3_INJECT_VIDEO=1`).
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
`pread64` / `pwrite64` (added defensively in Phase 7.post — they were
never hooked before, and while we don't believe the game calls them, a
first-call log banner makes it visible immediately if anything does).

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

Each phase was regression-tested against `REFACTOR_BASELINE.txt` — the
post-Phase-6 run log captured before any carving. Every phase has matched
the baseline within timing-variance noise (`gpio` retry count ±4, frame
count ±30) on all 22 error classes + all 7 hook-fired log lines.

Planned remaining phases: **shim_patches.c** (memcpy NULL-guard + Step 62
vtable + Step 64 NOP + libavcodec hooks + dummy-frame injection) and
**shim_inject.c** (switch manifest + event sweep + poke threads).

## Other documents in this directory

| File | Purpose |
|---|---|
| [`SETUP.md`](SETUP.md) | One-time dev-environment setup |
| [`JOURNAL.md`](JOURNAL.md) | Step-by-step log of every attempt, success, and failure |
| [`PROTOCOL.md`](PROTOCOL.md) | Verified on-wire protocol facts (netbridge, DRM events, etc.) |
| [`MODE_TABLE.md`](MODE_TABLE.md) | The 42 C-modes — names, addresses, flag meanings |
| [`HANDOFF.md`](HANDOFF.md) | Current-state snapshot for picking up the work cold |
| [`ASSETS.md`](ASSETS.md) | Running log of what assets the game loads |
| [`EVENT_SWEEP_STEP52.md`](EVENT_SWEEP_STEP52.md) | Write-up of the Step 52 event-sweep approach |
| [`VIDEO_CLIPS.md`](VIDEO_CLIPS.md) | Catalogue of the 229 extracted video clips |
| `REFACTOR_BASELINE.txt` | Error-count reference the refactor is regression-tested against |
| `PHASE*_REPORT.txt` | Per-phase refactor smoke-run diff vs. baseline |

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

