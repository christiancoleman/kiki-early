# `src/` — the historical Spike 3 emulator

These are historical build and runtime notes. This source archive does not
include the original sysroot, game files, or a prebuilt shim.

This directory holds everything that builds or runs as part of the emulator: the
`LD_PRELOAD` shim (`spi_shim.so`), the build/launcher scripts, the host-side
runtime daemons, the dev/operator tools, and the host-compiled unit tests.

It's the **implementation**. The verified reverse-engineering and Spike-3
**platform** facts live in [`../docs/PROTOCOL.md`](../docs/PROTOCOL.md); the
chronological record of how we got here lives in
[`../docs/JOURNAL.md`](../docs/JOURNAL.md). The shim's internals (translation
units, IPC bridge wire formats, the forward-decode / GL-daemon protocols, the
other shim behavior) are documented in [`SHIM.md`](SHIM.md). This README is
the entry point: what this is, how it's laid out, how to build & run it.

## What this is

Run the **Pokémon LE 0.81.0** game binary — an ARM aarch64 ELF written for
Stern's **Spike 3 platform** — on an x86_64 Linux dev workstation. The approach
is **QEMU user-mode** plus an **`LD_PRELOAD` shim** (`spi_shim.so`) that
intercepts library/device calls and either emulates the missing hardware in
software or forwards translated calls to the host. The same emulator is intended
to boot **any Spike 3 title**; Star Wars LE 0.85.0 is the secondary test target
(PROTOCOL "Comparison binaries").

It emulates the **CPU side only**. The RS-485 node bus is stubbed in software
(the shim's `synthesize_bridge_response` plays the part of the netbridge MCU);
the downstream nodes themselves are not present. Power-distribution responses are
synthesized too.

## Host requirements & one-time setup

WSL2 with Debian 13 (trixie) on Windows 11, or native Linux x86_64. ~5 GB free
disk (game + extraction artifacts + Mesa overlay), ~4 GB RAM free at runtime. No
KVM, no GPU passthrough — render via Mesa llvmpipe (software raster). `sudo` only
for the initial package install + the bind-mount setup.

```bash
sudo apt install -y qemu-user gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu python3 curl dpkg-dev file
# confirm binfmt_misc is registered for aarch64:
cat /proc/sys/fs/binfmt_misc/qemu-aarch64    # first lines: "enabled" / "interpreter /usr/libexec/qemu-binfmt/aarch64-binfmt-P"
# if missing: sudo systemctl restart systemd-binfmt   (or reinstall qemu-user)
```

**Bind-mount the sysroot to native paths (once per WSL session — persists until reboot).**
The game reads absolute paths like `/games/pokemon_le/…`, `/connectivity/…`,
`/data/…`. Rather than intercepting and rewriting every filesystem call, kernel
bind mounts resolve them natively:

```bash
sudo src/scripts/setup_bind_mounts.sh                                # default: pokemon_le
sudo src/scripts/setup_bind_mounts.sh /mnt/c/Workspace/starwars-le-0.85.0   # other Spike-3 game
     src/scripts/setup_bind_mounts.sh --check                        # verify (no sudo)
sudo src/scripts/setup_bind_mounts.sh --umount                       # tear down
sudo src/scripts/setup_bind_mounts.sh --switch <sysroot>             # umount + remount elsewhere
```

After setup, `/games`, `/connectivity`, `/data` are kernel-resolved into the
sysroot. Idempotent; validates the source dirs exist; refuses sysroots outside
`/mnt/c/Workspace/`. Bind mounts on `/mnt/c` (9p) only succeed at host level, not
in an unprivileged user namespace (standard Linux behaviour, not a WSL quirk).
The `run_game.sh` launcher auto-runs `--switch` for you if the active mounts
don't match its sysroot (disable with `AUTO_BIND_MOUNTS=0`, or
`SKIP_BIND_MOUNT_CHECK=1` to skip the check entirely).

**Build the Mesa software-raster overlay (once).** The `GL_DAEMON=0` fallback
path forwards EGL/GLES to a sysroot-private Mesa:

```bash
cd src/mesa_sw_overlay && ./build_overlay.sh
```

Downloads ~63 MB of Debian arm64 `.deb`s, extracts into `pkg/` (~365 MB,
git-ignored), patches the DRI version string so the sysroot's Mesa accepts them.
Re-runnable. (The default backend is the host-native GL daemon — `GL_DAEMON=1` —
which doesn't need this overlay; build it only if you want the in-qemu fallback.)

## Build

```bash
cd _emulation/src
./build.sh                # cross-compiles spi_shim.so (-O2)
DEBUG=1 ./build.sh        # -O0 -g
```

Prereq: `gcc-aarch64-linux-gnu`. Build the shim from source; no prebuilt shim is included. (Pre-existing `-Wnonnull-compare` warnings in `shim_fs.c` are
benign.)

## Test

```bash
cd _emulation/src/tests
./run_tests.sh            # host-compiled unit tests + Python golden model; non-zero exit on failure
```

Host compiler + Python, no game binary, no `/games` bind mount, no qemu. Test
sources mirror `src/`'s module layout (`tests/nodebus/` tests `src/nodebus/`).
See [`tests/README.md`](tests/README.md) for the TDD discipline and coverage.

## Architecture at a glance

```
       host (x86_64 Linux / WSL2)
  ┌──────────────────────────────────────┐
  │  qemu-aarch64 -L $SYSROOT            │
  │  └─ ./game (ARM aarch64 ELF)         │
  │     ├─ statically linked: Godot 4.4, │
  │     │   Radium, Nuklear              │
  │     ├─ dynamically links: libasound, │
  │     │   libavcodec, libdrm, libgbm,  │
  │     │   libEGL, libGLESv2, libcurl…  │
  │     └─ LD_PRELOAD = spi_shim.so      │
  │                                      │
  │  spi_shim.so intercepts:             │
  │    – /dev/ttyAMA5 (netbridge UART)   │
  │    – /dev/spidev4.0  (CPU SPI)       │
  │    – /dev/i2c-10     (EEPROM)        │
  │    – /dev/dri/card1  (DRM/KMS)       │
  │    – /dev/snd/* (ALSA, full stub)    │
  │    – GPIO sysfs, libgpiod chardev    │
  │    – V4L2 video devices              │
  │                                      │
  │  AF_UNIX abstract namespaces:        │
  │    Listeners (subscribers connect):  │
  │      @pokemon_spike3_bridge (state)  │
  │      @pokemon_spike3_frame  (RGBA)   │
  │      @pokemon_spike3_audio  (S16LE)  │
  │      @pokemon_spike3_devbus (switch) │
  │    Client (shim → host daemon):      │
  │      @spike3_decode (HEVC NVDEC)     │
  │      @spike3_gl     (host GLES)      │
  │                                      │
  │  Host processes:                     │
  │    display/decode_daemon.py (NVDEC)  │
  │    display/gl_daemon.py (host GLES)  │
  │    display/frame_viewer.py           │
  │    audio_subscriber.py               │
  │    nodebus/devbus_subscriber.py, …   │
  └──────────────────────────────────────┘
```

Per-translation-unit coverage, the IPC bridge wire formats, the forward-decode
and GL-daemon protocols, the runtime binary patches, and the per-device
emulation details are in [`SHIM.md`](SHIM.md).

## Scope discipline

**In scope** (Stern platform code — applies to every Spike 3 game):
node-bus / UART / SPI / I2C bridge synthesis; `image.bin` format + asset
resolution; Radium scene format and the `IScene` shared abstract base;
DisplayRenderer render loop, SceneCache init, proc dispatch system; Stern's audio
pipeline (ALSA + sound-event registration); hardware shim coverage
(DRM/GBM/EGL/GLES/V4L2/GPIO/ALSA/fontconfig).

**Out of scope** for active investigation (Pokémon-port glue specific to this
title's Godot bridge): `VideoPlayerSpi` `is_inside_tree()` errors + related
vtable assertions; `ManualGodotScene` / `PokemonGodotScene` / `godot_switch_scene`;
`GodotRenderer` pre-warm cycles; `/tmp/godot_logs` content; `assets/godot/main.pck`
internals beyond "this is the Godot PCK".

**Star Wars LE 0.85.0** is the parity test target — zero Godot, same Stern Radium
attract pipeline. A SW-LE-to-attract failure points at shared shim/node-bus code
rather than Pokémon-port glue. See PROTOCOL "Comparison binaries".

