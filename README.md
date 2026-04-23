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

The shim was originally a single `spi_shim.c` file (~6400 lines). We're
splitting it into subsystem-scoped source files. Each file is self-contained
in its own concern and depends on the others only via declarations in
`spi_shim.h`.

> **Terminology:** "source file" here means a C compilation unit — one `.c`
> file that the compiler processes into one object, all of which the linker
> bundles into `spi_shim.so`. Externally the result is one library;
> internally the split keeps subsystems legible.

### `spi_shim.h` — shared header

The only thing that crosses the source-file boundary. Holds:
- The `SHIM_LOG(...)` macro used for all diagnostic output.
- `FWD_RET` / `FWD_VOID` / `FWD_RET_V` / `FWD_VOID_V` — macros used inside
  GL/EGL stub bodies to tail-call the real Mesa implementation when
  `SPIKE3_REAL_GL=1`, or fall through to the stub when not.
- `struct shim_cmode_entry` + the externs for the game's 42-mode registry.
- Forward declarations for cross-module functions (bridge lifecycle,
  signal handlers, netbridge response synthesizer, etc.).
- Display constants (`FAKE_MODE_HDISPLAY`, `FAKE_MODE_VDISPLAY`).

### `spi_shim.c` — main shim body

Still carries most of the code, being split further over subsequent phases.
Currently owns:
- `__attribute__((constructor))` entry points (directory setup, crash-
  handler install, bridge spawn, vp-patch thread).
- The full set of intercepts: `open`/`openat`/`open64`/`openat64`/`fopen64`,
  `access`/`faccessat`, `stat`/`lstat`/`fstatat`, `ioctl`, `read`, `write`,
  `close`, `lseek`, `dlopen`, `dlsym`, `pthread_create`, `system`, `memcpy`.
- SPI/I2C/DRM/UART device intercept dispatcher (the `intercept_device()`
  table that maps `/dev/*` paths to fake fd returns).
- GPIO sysfs fake-fd pool (Step 63).
- DRM ioctl handling (mode-setting, dumb buffer allocation, page-flip events).
- EGL/GLES/GBM hooks (real-GL forwarding with configuration patching;
  the Step 57–59 fallback-pbuffer machinery for default-framebuffer
  completeness).
- Step 62 `SpiDecoder::Play` vtable patch + Step 64 `get_format` NOP patch.
- libavcodec HW-accel bypass hooks (Step 47 + Step 64 software-HEVC redirect).
- Switch manifest + `shim_fire_switch` + event sweep (Step 52 scaffolding).
- `snd_pcm_*` stubs (audio written to `_emulation/audio/stream.s16le`).
- Frame capture state (`shim_last_draw_fbo`) set by the `glBindFramebuffer`
  hook; consumed by `shim_frame_capture_at_page_flip` in `shim_bridges.c`.

### `shim_diagnostics.c` — crash dumps + game-state inspection

Concentrates all *observe-the-game* machinery:
- `shim_safe_read` / `shim_hexdump_stderr` — guest-memory dump helpers.
- `shim_dump_sound_globals` — Pokémon LE sound-registry snapshot.
- `shim_cmode_table[]` — the 42-entry BN-extracted C-mode map (addresses,
  flag bytes, human-readable names).
- `shim_resolve_game_base()` — caching BN VA → runtime-address resolver.
- `shim_dump_cmodes` / `shim_dump_dispatch_tables` — verbose state dumps.
- `shim_sigsegv_handler` — SIGSEGV/SIGBUS handler: dumps, then longjmp-
  escapes back out of a faulting event-sweep iteration.
- `shim_term_dump_handler` — SIGTERM / SIGINT / SIGUSR1 / SIGUSR2 handler
  (dumps on the first three; runs `shim_event_sweep` on SIGUSR2).

## Other documents in this directory

| File | Purpose |
|---|---|
| [`SETUP.md`](SETUP.md) | One-time dev-environment setup |
| [`JOURNAL.md`](JOURNAL.md) | Step-by-step log of every attempt, success, and failure |
| [`PROTOCOL.md`](PROTOCOL.md) | Verified on-wire protocol facts (netbridge, DRM events, etc.) |
| [`MODE_TABLE.md`](MODE_TABLE.md) | The 42 C-modes — names, addresses, flag meanings |
| [`HANDOFF.md`](HANDOFF.md) | Current-state snapshot for picking up the work cold |
| [`ASSETS.md`](ASSETS.md) | Running log of what assets the game loads (fonts, videos, etc.) |
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

