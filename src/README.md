# `_emulation/src/` — the Spike 3 emulator shim

This directory holds everything that builds or runs as part of the emulator: the
`LD_PRELOAD` shim (`spi_shim.so`), the build/launcher scripts, the host-side
runtime daemons, the dev/operator tools, and the host-compiled unit tests.

It's the implementation. **The verified reverse-engineering and Spike-3 platform
facts live in [`../docs/PROTOCOL.md`](../docs/PROTOCOL.md); the chronological
record of how we got here lives in [`../docs/JOURNAL.md`](../docs/JOURNAL.md).**
This README is the entry point for "how is the code laid out / how do I build /
how do I run it."

## Layout

`_emulation/` is the project root. It splits into `src/` (this dir — code) and
the non-code dirs that stay at `_emulation/`:

```
_emulation/
├── src/                       ← everything code/build/run (this dir)
│   ├── nodebus/                ← carved-out module: netbridge UART protocol
│   │   └── shim_nodebus.c
│   ├── games/                  ← per-game machine profiles (switches/drivers/lamps)
│   │   └── pokemon_le_machine.{c,h}
│   ├── tests/                  ← host-compiled unit tests, mirroring src/'s modules
│   │   ├── nodebus/{test_nodebus.c, nodebus_model.py, test_nodebus_model.py}
│   │   └── run_tests.sh
│   ├── mesa_sw_overlay/        ← self-contained sub-build: the GL_DAEMON=0 fallback
│   ├── scripts/                ← setup_bind_mounts.sh, reset_run_state.sh
│   ├── spi_shim.c  spi_shim.h  shim_*.{c,h}  gl_proto.h  spi_shim.ver   ← shim TUs (flat for now)
│   ├── gles_probe{3..6}.c      ← one-off GL probe utilities
│   ├── build.sh  run_game.sh  nuke_emulation.sh                         ← build / launch / cleanup
│   ├── gl_daemon.py  decode_daemon.py  gl_proto.py                      ← host-side runtime daemons
│   └── marker_button.py  switch_command.py  switch_inventory.py  devbus_snapshot.py
│       *_subscriber.py  frame_viewer.py  snap_frame.py  switch_noise_test.py  decode_daemon_test.py
│                                                                        ← dev/operator tools
├── docs/        PROTOCOL.md (verified RE/platform), JOURNAL.md (history)
├── runs/        per-run logs — runs/<TS>/, runs/latest/ symlink (git-ignored)
├── captures/    on-disk frame/audio dumps (git-ignored)
├── reference/   BN notebook exports, manuals, screenshots
└── _ghidra/     Ghidra project (git-ignored)
```

**Module convention.** As subsystems get a TDD pass or a refactor, they're
carved out of the flat `shim_*.c` set into `src/<module>/` with a matching
`tests/<module>/`. `nodebus/` is the first; `gl/`, `io/`, `audio/`, `core/`,
etc. follow. `build.sh` passes `-I"$SRC_DIR"` so a TU in any subdir keeps a bare
`#include "spi_shim.h"`.

## Build

```sh
cd _emulation/src
./build.sh                # cross-compiles spi_shim.so (-O2)
DEBUG=1 ./build.sh        # -O0 -g
```

Prereq: `gcc-aarch64-linux-gnu` (`sudo apt install gcc-aarch64-linux-gnu`). The
shim ships pre-built; only rebuild after editing a TU. (The Mesa overlay used by
`GL_DAEMON=0` is built separately and once — see PROTOCOL §2 and
`mesa_sw_overlay/build_overlay.sh`.)

## Run

```sh
cd _emulation/src
./run_game.sh             # launch the game under qemu-aarch64 with the shim LD_PRELOAD'd
./run_game.sh --debug --note "what i'm testing"
```

`run_game.sh` self-locates and auto-detects the sysroot from its own path. It is
the source of truth for runtime toggles (env vars) — see the comment block at
the top of the script; PROTOCOL deliberately doesn't mirror that list. A fresh
`../runs/<TS>[_<note>]/` is created per run, with `../runs/latest/` symlinked to
it.

**Configurable project path.** `run_game.sh` and `build.sh` self-locate, so they
work from any checkout without configuration. If you need to point them at a
different `_emulation/` tree (unusual), set `SPIKE3_EMULATION_DIR=/path/to/_emulation`
— that's the override knob. Internally the scripts distinguish `SRC_DIR`
(= `_emulation/src/`, where the code + helpers live) from `EMU_ROOT`
(= `_emulation/`, where `runs/`, `docs/`, `captures/` live); `SPIKE3_EMULATION_DIR`
is `EMU_ROOT` and is what gets exported to the shim.

**Canonical exec path.** `run_game.sh` does `cd /games && exec /games/game …`
(a relative symlink to `pokemon_le/game`) to match real-cabinet behaviour. This
is *not* relative to `src/` — `/games/` is the bind-mounted sysroot path, the
same for every checkout. Running from any other cwd / exec path produces
markedly less stable runs — see PROTOCOL §2 and JOURNAL Step 94k.

## Test

```sh
cd _emulation/src/tests
./run_tests.sh            # host-compiled unit tests + Python golden model; non-zero exit on failure
```

These run on the **host** compiler (not the aarch64 cross-compiler) and Python —
no game binary, no `/games` bind mount, no qemu. Test sources mirror `src/`'s
module layout (`tests/nodebus/` tests `src/nodebus/`). See
[`tests/README.md`](tests/README.md) for the TDD discipline and what's covered.

## Where things live

| Concern | Path |
|---|---|
| Shim translation units | `src/*.c`, `src/*.h`, plus carved-out modules in `src/<module>/` |
| Build / launch / cleanup | `src/build.sh`, `src/run_game.sh`, `src/nuke_emulation.sh` |
| Host runtime daemons (NVDEC decode, native GLES) | `src/decode_daemon.py`, `src/gl_daemon.py` (+ `gl_proto.{h,py}`) |
| Dev/operator tools (TIMER panel, switch injection, devbus, viewers) | `src/marker_button.py`, `src/switch_command.py`, `src/switch_inventory.py`, `src/devbus_snapshot.py`, `src/*_subscriber.py`, `src/frame_viewer.py`, `src/snap_frame.py` |
| Per-game machine inventory (switches/drivers/lamps) | `src/games/<game>_machine.{c,h}` |
| Bind-mount + run-state setup | `src/scripts/setup_bind_mounts.sh`, `src/scripts/reset_run_state.sh` |
| GL_DAEMON=0 Mesa overlay | `src/mesa_sw_overlay/` |
| Host-side unit tests | `src/tests/<module>/` |
| Verified RE / platform facts | `../docs/PROTOCOL.md` |
| Chronological investigation log | `../docs/JOURNAL.md` |
| Run logs / captures / Ghidra | `../runs/`, `../captures/`, `../_ghidra/` |
