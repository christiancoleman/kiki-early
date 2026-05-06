# Spike 3 Emulation — Playbook & Verified Reference

**Discipline.** This is the single source of truth for everything we have *proven* about the Spike 3 platform and our emulation of it. **Verified facts only.** Hedging language (`likely`, `probably`, `TODO`, `TBD`, `unverified`) does not belong in this document — those live in `JOURNAL.md`. Every section is **self-contained** — you should not need to chase a journal step to evaluate a claim. Each subsection ends with a `*Verified by:*` footnote naming the method (BN decompile, hexdump @ X, runtime BRK trap on date Y, runtime strace, etc.). When sources conflict, **the binary wins** — re-verify against code or a runtime test, never against file mtime.

`JOURNAL.md` is the exploration/narrative log. Cite it from this document only for "why this workaround exists" historical context, never as proof of a fact.

When a claim here turns out to be wrong, **delete it**. Do not leave a banner. Git history is the archive.

---

## Table of contents

1. [Project overview & architecture](#1-project-overview--architecture)
2. [Quick-start: build & run the emulator](#2-quick-start-build--run-the-emulator)
3. [Boot sequence (host + game)](#3-boot-sequence-host--game)
4. [CPU ↔ netbridge UART bridge protocol](#4-cpu--netbridge-uart-bridge-protocol)
5. [Nodebus (cipher, hash, framing)](#5-nodebus-cipher-hash-framing)
6. [image.bin file format](#6-imagebin-file-format)
7. [Asset extraction (spike_extract)](#7-asset-extraction-spike_extract)
8. [Audio pipeline](#8-audio-pipeline)
9. [Radium scene format](#9-radium-scene-format)
10. [Display pipeline (DRM / GBM / EGL / GLES)](#10-display-pipeline-drm--gbm--egl--gles)
11. [Game runtime model (switches, events, modes)](#11-game-runtime-model-switches-events-modes)
12. [Hardware shim coverage reference](#12-hardware-shim-coverage-reference)
13. [Comparison binaries (SW LE 0.85)](#13-comparison-binaries-sw-le-085)
14. [System-mode debugging (HW watchpoints)](#14-system-mode-debugging-hw-watchpoints)

---

## 1. Project overview & architecture

### What this project does

Run the **Pokémon LE 0.81.0** game binary, an ARM aarch64 ELF originally written for Stern's **Spike 3 platform**, on an x86_64 Linux dev workstation. The approach is **QEMU user-mode** plus an **`LD_PRELOAD` shim** (`spi_shim.so`) that intercepts library and device calls and either emulates the missing hardware in software or forwards translated calls to the host. The same emulator is intended to boot **any Spike 3 title** (Star Wars LE 0.85.0 is the secondary test target — see §13).

### Asset stores

The game has **two independent asset containers**. Knowing which holds what is essential before searching:

| Store | Path | Size | Holds |
|---|---|---|---|
| `image.bin` | `games/pokemon_le/image.bin` | 1.05 GiB | 2452 audio assets (PCM 16-bit LE 44.1 kHz, mono + stereo); 885 scene-bytecode descriptors; animation/coordinate tables; playfield/mode grid records |
| `main.pck` | `games/pokemon_le/assets/godot/main.pck` | 210 MB | Godot resources reachable via `res://`: `.tscn` scenes, GLSL shaders, sprite textures, H.265 battle videos, GDScripts, Godot-side fonts, project icon |

Some categories live elsewhere on the filesystem (Radium scenes in `assets/lcd/`, Nuklear assets in `assets/nuk/`, the boot splash PNG in `spike3/bin/`). See §6 for the image.bin internal layout, §7 for extraction, §9 for Radium scene format.

*Verified by:* image.bin byte size + section header parse (see §6); main.pck file byte size + Godot version banner.

### Scope discipline

**In scope** (Stern platform code, applies to every Spike 3 game):
- Node-bus / UART / SPI / I2C bridge synthesis
- image.bin file format and asset resolution
- Radium scene format and the IScene shared abstract base
- DisplayRenderer render loop, SceneCache initialization, proc dispatch system
- Stern's audio pipeline (ALSA + sound-event registration)
- Hardware shim coverage (DRM/GBM/EGL/GLES/V4L2/GPIO/ALSA/fontconfig)

**Out of scope** for active investigation (Pokémon-port glue specific to this title's Godot bridge, per the 2026-05-02 directional pivot):
- `VideoPlayerSpi` `is_inside_tree()` errors and related vtable assertions
- `ManualGodotScene`, `PokemonGodotScene`, `godot_switch_scene`
- `GodotRenderer` pre-warm cycles
- `/tmp/godot_logs` content
- `./assets/godot/main.pck` internal structure beyond "this is the Godot PCK"

The acid test: this emulator should boot **Star Wars LE 0.85.0** identically to PLE 0.81 because both run the same Stern Radium attract pipeline. SW LE has zero Godot — if our emulator can't get it to attract, the bug is in shared shim/node-bus code. See §13.

---

## 2. Quick-start: build & run the emulator

### Host requirements

WSL2 with Debian 13 (trixie) on Windows 11, or native Linux on x86_64. ~5 GB free disk (game + extraction artifacts + Mesa overlay), ~4 GB RAM free at runtime. No KVM, no GPU passthrough — we render via Mesa llvmpipe (software raster). No `sudo` needed after the initial package install.

```bash
sudo apt install -y \
    qemu-user gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu \
    python3 curl dpkg-dev file
```

Confirm `binfmt_misc` is registered for aarch64:

```bash
cat /proc/sys/fs/binfmt_misc/qemu-aarch64
# expected first two lines:
#   enabled
#   interpreter /usr/libexec/qemu-binfmt/aarch64-binfmt-P
```

If missing: `sudo systemctl restart systemd-binfmt` or reinstall `qemu-user`.

*Verified by:* fresh Debian 13 setup (Step 55 sequence).

### One-time setup: Mesa software-raster overlay

The shim's real-GL mode forwards EGL/GLES calls to a sysroot-private Mesa overlay. Build it once:

```bash
cd "$SYSROOT/games/pokemon_le/_emulation/mesa_sw_overlay"
./build_overlay.sh
```

Downloads ~63 MB of Debian arm64 `.deb`s, extracts them into `pkg/` (~365 MB, gitignored). The script also patches `24.0.8-1 → 24.0.9` in each `*_dri.so` so the sysroot's Mesa 24.0.9 libEGL accepts them. Re-runnable safely.

Validate end-to-end render:

```bash
aarch64-linux-gnu-gcc -o gles_probe2 gles_probe2.c -ldl
OVERLAY="$PWD/pkg/usr/lib/aarch64-linux-gnu"
qemu-aarch64 -L "$SYSROOT" \
    -E LD_LIBRARY_PATH="$OVERLAY:$SYSROOT/usr/lib64:$SYSROOT/usr/lib:$SYSROOT/lib64:$SYSROOT/lib" \
    -E LIBGL_DRIVERS_PATH="$OVERLAY/dri" \
    -E MESA_LOADER_DRIVER_OVERRIDE=swrast \
    -E LIBGL_ALWAYS_SOFTWARE=1 \
    -E EGL_PLATFORM=surfaceless \
    ./gles_probe2
```

Expected last line: `VERDICT: ALL PASS — llvmpipe is really rasterizing`.

### Build the shim (only needed after editing)

```bash
cd "$SYSROOT/games/pokemon_le/_emulation"
./build.sh
# or directly:
aarch64-linux-gnu-gcc -shared -fPIC -O2 \
    -o spi_shim.so spi_shim.c -ldl -lpthread
```

`build.sh` cross-compiles the shim TUs. The shim as checked in is already built — only rebuild after editing `spi_shim.c` or any `shim_*.c`.

### One-time setup: bind-mount the sysroot to native paths

The game is compiled to read absolute paths like `/games/pokemon_le/...`, `/connectivity/...`, `/data/...`. Rather than intercepting every filesystem call in the shim and rewriting these paths, we use kernel-level bind mounts so the paths resolve natively. **One-shot setup, persists until reboot:**

```bash
sudo _emulation/scripts/setup_bind_mounts.sh                  # default: pokemon_le
sudo _emulation/scripts/setup_bind_mounts.sh /mnt/c/Workspace/starwars-le-0.85.0   # other Spike-3 game
_emulation/scripts/setup_bind_mounts.sh --check               # verify (no sudo)
sudo _emulation/scripts/setup_bind_mounts.sh --umount         # tear down
sudo _emulation/scripts/setup_bind_mounts.sh --switch <sysroot>  # umount + remount elsewhere
```

After setup, `/games`, `/connectivity`, `/data` are kernel-resolved into the sysroot — game absolute-path references work without any LD_PRELOAD remapping. Idempotent (skips already-mounted), validates source dirs exist, refuses sysroots outside `/mnt/c/Workspace/`.

**Why this matters:** before the bind-mount approach, the shim had ~50 path-related hooks (`open` / `__openat64_2` / `fopen` / `stat` / `__xstat64` / `opendir` / `readdir` / `getcwd` / `chmod` / `unlink` / `rename` / etc.) and a `remap_path` helper that prefixed every absolute path with `$SYSROOT`. Many of those hooks couldn't catch all libc-internal call paths — strace consistently showed the shim missing real `.radium` opens. With bind mounts, the **kernel** resolves the paths and every libc / direct-syscall / asm path goes through the same bind-mount table simultaneously. Result: more dependable runs, fewer phantom "file not found" issues.

**Caveats:**
- Bind mounts on `/mnt/c` (9p) only succeed at host level, not inside an unprivileged user namespace (verified — standard Linux user-namespace behavior, not a WSL quirk).
- The shim's `remap_path` and most FS hooks have been removed (see §12 — only GPIO sysfs / libgpiod chardev FS-style hooks remain because those are hardware-related, not file-resolution).
- The setup script creates `/games`, `/connectivity`, `/data` at root as zero-content mountpoint directories. They persist after `--umount`; remove with `sudo rmdir` if you want them gone.

*Verified by:* `_emulation/scripts/setup_bind_mounts.sh` source; `findmnt /games /connectivity /data` against a live setup; JOURNAL Step 80 POC validation.

### `/etc/init.d/shared` env propagation

The script sources `$SYSROOT/etc/init.d/shared` and propagates every var via qemu's `-E` flag, exactly as real hardware would inherit them through init. The shim's `remap_path` then translates absolute paths like `/games/pokemon_le/...` to `$SYSROOT/games/pokemon_le/...` on every `open()`. One override applied: `BOOT_DISPLAY` is forced to `$GAMES_PATH/spike3/bin/boot_display` because the shared script's `[ -x ]` check runs on the host filesystem and would otherwise fall back to the wrong path.

*Verified by:* `_emulation/run_game.sh` lines 187-209.

## 3. Boot sequence (host + game)

The Pokémon LE cabinet boots Linux off an SD card, then SysV init brings up Stern's services, then `game_monitor` execs the `game` binary, which runs its own multi-phase initialization. The full chain is split into five phases below.

### Phase 1 — Kernel init (BusyBox)

Kernel finishes initialization and execs `/sbin/init` (BusyBox). Per `/etc/inittab`:

1. Early mounts: `mount -t proc proc /proc`; `mkdir -p /dev/pts /dev/shm`; `mount -a` (mounts `/etc/fstab` entries: devpts, tmpfs on `/dev/shm` + `/tmp` + `/run`)
2. Standard fd symlinks: `/dev/fd → /proc/self/fd`; `/dev/std{in,out,err} → /proc/self/fd/{0,1,2}`
3. `hostname -F /etc/hostname`
4. Execs `/etc/init.d/rcS` (orchestrator)

### Phase 2 — `rcS` runs `/etc/init.d/S??*` in numerical order

Each script handles its own subsystem. Verified against the live `/etc/init.d/` directory:

| Order | Script | Action |
|---|---|---|
| S01 | `S01syslogd` | starts `syslogd` |
| S01 | `S01seedrng` | seeds RNG |
| S02 | `S02klogd` | starts `klogd` |
| S02 | `S02sysctl` | `/sbin/sysctl -p /etc/sysctl.conf` + `/etc/sysctl.d/*.conf` |
| S10 | `S10udev` | `udevd -d`; `udevadm trigger`; `udevadm settle` (populates `/dev`) |
| S12 | `S12lvds_panel` | starts `/games/spike3/bin/boot_display &` (early UI: SternLogo splash on the LCD) |
| S30 | `S30dbus` | `dbus-uuidgen --ensure`; `dbus-daemon --system` |
| S40 | `S40network` | `/sbin/ifup -a` (ethernet/wifi) |
| S40 | `S40bluetoothd` | `bt_clean` (purge non-trusted info); starts `bluetoothd -n` |
| S50 | `S50crond` | `crond -f` |
| S55 | `S55spike3` | `bluetoothctl power on; bluetoothctl agent KeyboardOnly`; starts `bluealsa -p a2dp-source &`; `modprobe i2c-dev exfat fuse` |
| S95 | `S95game` | the "main event" script — see Phase 3 |

*Verified by:* `ls /etc/init.d/`; cat of each script.

### Notable side effects of boot

- **`init_spi_serial_bridge` is a hard gate.** Failure returns exit code `0xe2` and `game_monitor` immediately re-exec's. In emulation we synthesize the SPI/UART bridge responses (see §4 / §12) — the gate would otherwise loop forever.
- **`mcp_init_renderer_and_scene_cache_singletons` (`0x5d7aa0`) is the canonical Stern boot sequence** for graphics + scene cache. Five steps inside (per Stern source-line assertions in the binary): `new DisplayRenderer(0x1b8) → constructor → mutex+deferred-list → wait_on_render_thread_ready → new SceneCache(0x1d0)("./assets") → post-init`. Equivalent in Star Wars LE 0.85: `0x309af0`. Equivalent in `spike_menu`: see §13.
- **`pthread_create` with `SCHED_RR`** — the dispatch thread (`procDispatch`) needs RR scheduling. Under qemu-user without `CAP_SYS_NICE` this fails silently; the shim's `shim_pthread_create` forces `PTHREAD_INHERIT_SCHED` to skip the failing path. See §2 "Common gotchas."

### Localized-message-table primitive (used by step 2's overlay)

When the boot code wants to draw a localized UI string (like "Startup In Progress%s"), it goes through a two-level table lookup:

```
caller — e.g. mcp_draw_startup_in_progress_overlay
   └─ mcp_lookup_localized_msg_by_id(msgid)        // 0x7a6490
        ├─ bounds-check: msgid u< 0xd53            // 3411 messages total
        ├─ idx = mcp_msgid_to_strtable_idx[msgid]  // u16 array @ 0x44cb998
        ├─ bounds-check: idx u< 0xd53
        ├─ row = mcp_localized_string_table[idx]   // pointer table @ 0x4394d80
        └─ return mcp_pick_localized_string_from_row(row)  // 0x7d82d0
                  └─ language_idx = (data_436c0c0 == 5) ? read_config_value_validated(8) : data_436c0c0
                  └─ return row[language_idx]
```

Each row is a 5-pointer array (English / Spanish / French / German / Italian, in that slot order). All five slots can point to the same English string when no translation exists — for "Startup In Progress" they do.

**Verified message IDs** (against this build):

| msgid | String | Caller |
|---:|---|---|
| `0x789` (1929) | `"Startup In Progress%s"` (formatted with one %s arg) | `mcp_draw_startup_in_progress_overlay` (`0x5cddd0`) |

The `%s` argument is built by `sub_7de9c0(3, …)` — likely "stringify the integer 3" producing something like `"...3"` or `"..., step 3"`. Not yet decoded.

The msgid → string-table resolution was **established empirically** (PROTOCOL.md §14 "System-mode debugging" workflow): a HW read watchpoint on `mcp_localized_string_table[0x789]` runtime address `0x55598e89c8` fired with PC = `0x7a64ec` (inside `mcp_lookup_localized_msg_by_id`) and LR = `0x5cdf90` (inside `mcp_draw_startup_in_progress_overlay`). The same chain handles every other localized-string draw.

### Boot-splash tick chain (step 2 + step 3 overlays)

The state-machine path that decides which overlay text to draw on top of the Pokémon-logo / SPIKE3-logo phase:

```
primary_initialization_orchestrator (0x678bc0)
 │  Calls mcp_boot_splash_tick at 3 sites: 0x678de4, 0x678e14, 0x678e24
 │  (i.e. between sub-system inits — three "boot status update" beats)
 │
 └─ mcp_boot_splash_tick                             // 0x5d6ef0
     ├─ sub_733310                                   // get current-time-since-boot-ms
     ├─ sub_647720, sub_8191d0                       // enable-gate predicates
     ├─ sub_7b6db0  → render_sprite_to_framebuffer   // re-render the Pokémon logo sprite
     ├─ mcp_boot_state_dispatch                      // 0x5d0cb0 — vtable dispatch on data_445f800
     │   └─ mcp_boot_state_mask_dispatch             // 0x5d0670 — bit-mask dispatch
     │       │ flag bit (in *(arg1 + 0x38)) selects which overlay to draw:
     │       │   bit 3 (0x008)  → mcp_draw_startup_in_progress_overlay  (this is the one we traced)
     │       │   bit 0 (0x001)  → sub_5cebf0
     │       │   bits 1-2 (0x6) → sub_5cf110
     │       │   bit 4 (0x10)   → sub_5ce0b0
     │       │   bit 5 (0x20)   → sub_5ce670
     │       │   bits 6-9 (0x3c0) / bit 10 (0x400) / bit 13 (0x2000) / bit 14 (0x4000) → other paths
     │       └─ sub_5d6da0(*(arg1 + 8), 3)           // commit/render
     └─ sub_7b6be0 — std::condition_variable::notify_all on DisplayRenderer's cv (wakes render thread)
```

The flag mask at `*(arg1 + 0x38)` (absolute address `0x445f838`, since `arg1 = &data_445f800`) is what ultimately determines which overlay text gets drawn on a given tick. Bit 3 specifically drives the step-2 "Startup In Progress" overlay; the other bits drive other overlays — which specific bit fires which string isn't fully pinned down yet beyond bit 3.

#### Who sets the mask

The bits are written by a family of one-bit-per-function setter helpers, one per dispatch slot. The bit-3 setter (the one driving "Startup In Progress") is:

- **`mcp_set_startup_in_progress_overlay_flag` (`0x5d0930`)** — `arg1 == 1` ORs `0x08` into the mask, `arg1 == 0` ANDs `~0x08`. Only two callers, both inside `primary_initialization_orchestrator`:
  - `0x678dc8` — `sub_5d0930(1)` enables the overlay just before the boot wait loop.
  - `0x678e0c` — `sub_5d0930(0)` clears the bit after the wait loop exits.

The wait loop in between is **counter-bounded (~480 iterations)** and exits early when `mcp_scene_cache_preload_done` (`data_4482108`) becomes non-zero. Pseudocode:

```c
sub_5d0930(1);                          // SET bit 3 → enable overlay
counter = 0x1e0;                        // 480 iterations
do {
    mcp_boot_splash_tick(0);            // bit-3 path renders "Startup In Progress"
    usleep(...);
} while (--counter != 1 && mcp_scene_cache_preload_done == 0);
sub_5d0930(0);                          // CLEAR bit 3 → disable overlay
mcp_boot_splash_tick(0);                // tick once after clear
usleep(0xfa00);
mcp_boot_splash_tick(1);                // final tick
fire_event(0x5f, 0);
```

`mcp_scene_cache_preload_done` is set to 1 at `0x66d490` inside `SceneCache_PreLoadRadiumAssets` — i.e. when the Radium scene preload phase finishes (after enumerating `/lcd/demand_loaded/*.{radium,json}`, `/nuk/images/*.{jpg,bmp,png,dds}`, `/lcd/auto_loaded/*.{radium,json}` and constructing the auto-loaded `RadiumScene` objects). That preload runs on a **background thread** (`RadiumAssetLoader_ThreadEntry`) launched by `mcp_SceneCache_ctor` at `0x66b06c` via `CC_mainDispatchThread(RadiumAssetLoader_ThreadEntry+0xc, 8, "PreLoadSceneCache", arg1)`.

Implication: the wait loop is **time-bounded regardless** — even if the asset preload thread hangs, the orchestrator continues after ~8 wall seconds (in user-mode; longer in system-mode TCG emulation). The overlay is therefore not the proximate cause of any indefinite stall; the actual front-end blocker (per `CURRENT INVESTIGATION` memory, JOURNAL Step 81) is the Diag_1360 menu loop downstream.

*Verified by:* runtime hardware-read-watchpoint observation 2026-05-05 (PC `0x7a64ec`, LR `0x5cdf90`); BN decompile of the listed functions; `get_xrefs_to(0x445f838)` to enumerate writers, `get_xrefs_to(0x5d0930)` to confirm only the two orchestrator call-sites; `get_xrefs_to(0x4482108)` to find the `SceneCache_PreLoadRadiumAssets` writer; chain confirmed by user (system-mode and user-mode both reach the post-overlay state).

---

## 4. CPU ↔ netbridge UART bridge protocol

### Architecture

```
                  /dev/ttyAMA5                  POWERDISTUART
   ┌───────┐  ─────────────────►  ┌──────────┐  ─────────────►  ┌──────────┐
   │ game  │       host UART      │ netbridge│   transparent    │ powerdist│
   │ (CM4) │  ◄─────────────────  │  (RP2040)│   passthrough    │  board   │
   └───────┘   escape framing     └──────────┘  same framing    └──────────┘
                                       │
                                       │ NODE bus (UART_Node*)
                                       ▼
                                  coil / LED / pin /
                                  WS2812 / mag-sensor / TMC* nodes
```

The netbridge has **two UARTs**:
- `HOSTUART` — to the CM4 game (`/dev/ttyAMA5` on the CM4, baud 2000000). Bidirectional.
- `POWERDISTUART` — to the powerdist board. Bridge transparently forwards game cmds in the `0x10-0x1F` range here when `pdstate ∈ {5,6,7}` (Spike 3 powerdist detected via `POWERDIST_IsSpike3 @ netbridge.elf 0x10002098` returning 1).

When `pdstate < 5` the bridge handles a subset of `0x10-0x1F` locally; the rest fall through and the host times out.

In emulation: the shim creates a real `socketpair(AF_UNIX, SOCK_STREAM)` for `/dev/ttyAMA5` (game gets sv[0], shim holds sv[1]). The shim's `synthesize_bridge_response` is the equivalent of the real netbridge's `HOST_Update` for the cmds the game actually sends.

### Address conventions in this document

The game-binary BN database is **rebased to base 0x0**, so BN VA == ELF VA == file offset. Runtime address = `shim_resolve_game_base() + ELF_VA`. All addresses below ending in `g 0xNNNNNNN` (or unprefixed when context is clear) are ELF VAs. The netbridge.elf BN database is a separate database with its own ELF VAs starting at `0x10000000` (RP2040 flash base).

### Game's outbound cmd usage (current run)

From `_emulation/runs/latest/game.log`, the distinct cmd codes the game has been observed sending:

`0x10  0x11  0x15  0x16  0x18  0x1c  0x1d  0x27  0x30`  (9 unique)

Plus **`0x40` and `0x41` sent via runtime-computed cmd byte** in `mcp_nodebus_send_raw @ 0x9166c0`, where the cmd byte is `(arg1[0] >> 7) + 0x40`. The static-immediate scan that previously called these "never sent" missed them.

### Top-level cmd map (canonical truth)

Legend for **Shim status**: ✅ = handled with deliberate engineered shape; ⚠️ = handled but shape may not match the bridge's real reply (see notes); ❓ = uncovered, but the game has been observed sending this cmd; ❌ = uncovered and never observed in any run.

| Cmd | Bridge handler | Reply shape | Shim status |
|---|---|---|---|
| `0x10`-`0x1F` | POWERDIST_SendCmd passthrough (when Spike 3) | async via POWERDISTUART | n/a — see per-cmd entries below |
| `0x10` | local fallback: ADC version triple + 4th byte | `SendMessage(0x10, id, 3 bytes)` | ✅ shim sends 7-byte pwrdist version `01 00 00 00 00 00 00` (response cmd `0x10`) — game's handler at `0x916194` requires exactly 7 bytes |
| `0x11` | POWERDIST passthrough; local fall-through (Spike 2) | undefined locally | ✅ shim sends 0-byte ACK (silent-ACK fallback at netbridge `0x915d28`) |
| `0x12` | `ADC_GetLineFrequency` | `SendMessage(cmd, id, 1 byte)` | ❌ never observed in any run |
| `0x15` | local fallback: `0xff` ack | `SendMessage(0x15, id, 1 byte)` | ✅ shim sends 1-byte `0x00` (pwrdist status — "all clear") |
| `0x16` | POWERDIST passthrough; OR shared handler at `0x915d28` | game polls until `rx[0]>0 && rx[2]==2` (≤1000 × 1ms) | ✅ shim sends `01 00 02 00`. Verified satisfies the game's wrapper `sub_917b60` + polling caller at `0x675c20`; game prints "power dist. boot version / runtime version" after receipt |
| `0x18` | POWERDIST passthrough; firmware handler at `0x91607c` records `min(existing, len)` | the value isn't read | ✅ shim sends 1-byte `0x00` |
| `0x19` | `ADC_GetLineVoltage` | `SendMessage(cmd, id, 2 bytes)` | ❌ never observed |
| `0x1c` | POWERDIST passthrough; shared silent-ACK at `0x915d28` | unblocks the channel | ✅ shim sends 0-byte ACK |
| `0x1d` | POWERDIST passthrough; dedicated handler at `0x916028` | varies by len | ✅ shim sends 0-byte ACK (avoids writing stale data into per-id buffer) |
| `0x20` | bridge version triple (strtol over rodata) + ADC version | `SendMessage(0x20, id, 4 bytes)` | ⚠️ shim returns 4 zero bytes WITH RESPONSE CMD `0x10`. Diverges from the bridge's actual reply (which uses cmd `0x20`). Possibly intentional placeholder; the cmd 0x10 game-handler requires 7 bytes so this would fail if cmd 0x20 ever fires |
| `0x21` | `BBL_Set` / `BBL_Get` (battery backup level) | `SendMessage(cmd, id, 1 byte)` | ❌ never observed |
| `0x22` | `resetFlags` (also sent unsolicited at boot) | `SendMessage(0x22, 0, 1 byte)` | ❌ never observed (bridge sends unsolicited at first connect — see "Async notifications" below) |
| `0x23` | snapshot + clear `bridgeStatus` | `SendMessage(0x23, id, 4 bytes)` | ❌ never observed |
| `0x24` | `ADC_GetVBBmV` (battery voltage) | `SendMessage(cmd, id, 4 bytes)` | ❌ never observed |
| `0x25` | `NODEBUS_Poll` (flow-control probe; binary-search of free TX buffer space) | `SendMessage(cmd, id, 1 byte)` | ❓ never observed in this run, but earlier silent-ACK attempt broke something. Currently lets it time out (per Step 45 "timeout-is-progress") |
| `0x27` | LCD subcommands (sub-cmd at byte 0) | varies — see "Cmd 0x27 LCD subprotocol" below | ✅ shim alternates `0x06` / `0x02` per call to satisfy a paired bit-2-toggle poll loop in the game (see "Cmd 0x27 game-side polling" below) |
| `0x28` | DMD ops (disambiguated by length) | varies | ❌ never observed |
| `0x29` / `0x2A` / `0x2B` / `0x2F` | WS2812 LED chains | varies | ❌ never observed |
| `0x2C` | `I2S_Enable(arg & 1)` | none | ❌ never observed |
| `0x30` | `CRC32(0, &__VECTOR_TABLE, 0x12a40)` over bridge firmware | `SendMessage(0x30, id, 4 bytes)` | ✅ shim returns precomputed `NETBRIDGE_CRC32 = 0x357f5327` (zlib CRC32 chained over `netbridge.elf` `.text → .rodata → .binary_info`). Game prints `netbridge image okay, CRC32=0x357f5327` |
| `0x40` | nodebus-passthrough subprotocol (cleartext path) | varies | ✅ sent at runtime, log only. See "Cmd 0x40 nodebus subprotocol" below |
| `0x41` | node-bus query (encrypted transport) | varies | ✅ sent at runtime, encrypted. Shim logs decoded inner cmd + payload. See §5 for cipher; see "Cmd 0x41 wire format" below |

### Cmd 0x27 LCD subprotocol (bridge handler)

Sub-cmd at `data[0]`:

| Sub | Op |
|---|---|
| 0 | `LCD_GetStatus` → 1-byte status |
| 1 | `LCD_Reset` → 1-byte status |
| 2 | `LCD_WriteImage(data[1] - 1)` → 1-byte status |
| 3 | `LCD_WriteBuffer(data[1], &data[2])` |
| 4 | `LCD_Halt` → `LCDBL_Set(data[1], halt_result)` |
| 5 | `LCDBL_Set(data[1], halt_result)` |

#### Cmd 0x27 game-side polling

Four game-side callers poll the 1-byte response with different conditions:

| Callsite | Enclosing fn | Condition |
|---|---|---|
| `0x916828` | `sub_9167e8` | one-shot, any value acceptable |
| `0x916fe0` | `sub_916f60` first loop | wait until **bit 2 = 1** |
| `0x917020` | `sub_916f60` second loop | wait until **bit 2 = 0** (paired with above) |
| `0x917264` | standalone | wait until **`(resp & 0x03) == 2`**, ≤100 × 100ms |

A static response cannot satisfy the paired loop. The shim alternates:
- odd call → `0x06` (bits 1, 2 set, `& 3 == 2`)
- even call → `0x02` (bit 1 only, `& 3 == 2`)

Both values pass the `0x917264` condition; the alternation lets the paired loop exit both halves. *Verified by:* Step 19.2.

### Cmd 0x28 DMD subprotocol

Disambiguated by length:

| len | Op |
|---|---|
| 0 | `DMD_GetStatus` → 1-byte status |
| 1 | `DMD_TestPins(data[0])` |
| 0x81 | `DMD_WriteData(data[0], &data[1])` |

### Cmd 0x29 / 0x2A / 0x2B / 0x2F WS2812 LED chains

Disambiguated by length + sub-cmd at `data[0]`:

| Cmd | len | Op |
|---|---|---|
| `0x29` | 3 | `WS2812_SetNumLEDs(chain, n, 0)` |
| `0x29` | 9 | `WS2812_SetNumLEDs(chain, 0, &per_segment_lengths)` |
| `0x29` | 1 | `WS2812_Debug(chain)` |
| `0x2A` | varies | `WS2812_SendData(chain, data, len)` |
| `0x2B` | varies | `WS2812_SetLEDPattern(chain, range, pattern, len, count, period)` |
| `0x2F` | varies | `WS2812_NodeSetLED(payload)` |
| `0x2A` (alt) | varies | `WS2812_SetLEDs(chain, range, payload, len, count)` |

### Cmd 0x40 nodebus-passthrough subprotocol

Sub-cmd at `data[0]`. All replies use `SendMessage(0x40, id, ...)`.

| Sub | Op | Reply |
|---|---|---|
| `0x00` | `NODEBUS_Poll` | 1 byte |
| `0x03` | bridge version triple (3 bytes from rodata) | 3 bytes |
| `0x04` | `NODE_SendBreak(data[2] ? 1 : 0)` | none |
| `0x05` | snapshot + clear `bridgeStatus` | 4 bytes |
| `0x06` | configure response timeout: `respTimeus = (data[2..3] LE) * 1000`, `respCnt = 1` | none |
| `0x09` | `BBL_Set(data[2], data[3] << 4, 0)` (when len==2) | none |
| `0x0a` | `BBL_Get` | 2 bytes (`{0x00, bbl}`) |
| `0x0c` | infinite loop (factory test trap / watchdog test) | never returns |
| `0x0d` | **NODEBUS_Hash** (4 secret seeds, 5 sub-sub-cases) — see §5 | 4 bytes |

Senders observed at runtime: `mcp_nodebus_query_bridge_status` (sub-cmd 0x05), `mcp_nodebus_factory_test_trap @ 0x8f5f4c` (sub-cmd 0x0c, sends frame `{0x0c, 0x00, 0x00}`).

### Cmd 0x41 wire format (the real node communication path)

When the bridge sees a host frame whose cmd byte is `0x41`, `HOSTUART_GetMessage @ 0x100029dc` activates `nodeEcho` mode and **transparently forwards payload bytes onto the node bus while parsing the host frame**. Then `HOST_Update`'s cmd `0x41` handler reads the node's reply back.

Wire format (host → bridge):
```
STX | 0x41 | id | node_addr | length | tx_bytes[length] | rx_length | csum | ETX
```

Bridge action as bytes arrive:
1. Byte `0x41`: enter nodeEcho mode (`HOSTUART_GetMessage:0x10002a7c`).
2. `id` byte (rxIdx==1): just stash; don't forward to node bus.
3. `node_addr` byte (rxIdx==2): `NODEUART_WriteAddress(node_addr)` raises the bus "address" line.
4. `length` byte (rxIdx==3): `NODEUART_Write(length)`; set `echoLen = length + 1`.
5. Each subsequent payload byte: `NODEUART_Write(byte)` until `echoLen` exhausted.
6. After `echoLen` exhausted: `NODEUART_SendEnd` to terminate the bus frame.

Then `HOST_Update`'s cmd `0x41` handler runs:
1. Compute expected `rx_length` from `data[length+2]`.
2. `memset` response buffer with `0x55` (placeholder).
3. `UART_NodeGetMessageBlock(buf, rx_length - 1, NULL)` — block-read with bounded timeout (`HOST_GetResponseTime() + nodeRxBitTime` per byte).
4. Append `bridgeStatus` as the last byte.
5. `HOSTUART_SendMessage(0x41, id, buf, rx_length)` — return to host.

Wire format (bridge → host):
```
STX | 0x41 | id | rx_bytes[rx_length-1] | bridgeStatus | csum | ETX
```

The actual node-bus message format (what's inside `tx_bytes` / `rx_bytes`) is determined by the **node firmware** (`pinnode-LPC1124_303-X.Y.Z.hex`, `coil4node-...`, etc.). The bridge is just a transparent UART relay — it doesn't know what "switch 57" means. To decode node-bus events: RE the node firmware, or capture wire traffic on real hardware and pattern-match against switch closures.

The encrypted variant of cmd 0x41 — used for node firmware flashing and 160+ per-(slot, inner_cmd) wrappers — is documented in §5.

### Game-side response dispatch (`sub_d15aa8 @ 0xd15aa8` in PLE 0.81)

A dedicated bridge thread parses the responses byte-by-byte. When a complete frame arrives, it extracts `CMD` and computes `idx = CMD - 0x10`. If `idx u> 0x31` it falls through to "unknown command" error. Otherwise it loads a 16-bit signed entry from a jump table at `.rodata` offset `0x37cf510` (50 entries = 0x32 cmds, covering 0x10 through 0x41) and jumps to `0x915e14 + entry * 4`.

Per-cmd handler addresses (game side):

| Resp CMD | Handler VA | Payload requirement | Action |
|---|---|---|---|
| `0x10` | `0x916194` | **exactly 7 bytes** | Pwrdist version. Packs bytes 0..2 into `data_490e61c`, bytes 3..5 into `data_490e620`, byte 6 → boolean flag at `+0x74` |
| `0x12` | `0x916158` | 7 bytes | TBD |
| `0x14` | `0x9160d8` | 4 bytes | TBD |
| `0x15` | `0x9160a0` | **exactly 1 byte** | Pwrdist status. OR/XORs payload[0] with state at `data_490e5b0 + 128` |
| `0x18` | `0x91607c` | variable | Records `min(existing, payload_len)` into halfword table at `data_490e5b0 + 0x88`, indexed by ID |
| `0x1a` | `0x916064` | TBD | TBD |
| `0x1b` | `0x916038` | TBD | TBD |
| `0x1d` | `0x916028` | **exactly 4 bytes** | If `payload == 4`, branches to channel-copy at `0x915e88` (memcpy payload into per-id buffer). If not, silent ACK |
| `0x20` | `0x915fec` | TBD | HW version |
| `0x22` | `0x915e54` | TBD | TBD |
| `0x2d` | `0x915f08` | TBD | TBD |
| `0x2e` | `0x915ed8` | TBD | TBD |
| `0x41` | `0x915f88` | TBD | TBD |

**Generic fallbacks** (shared handlers reached by cmds without a dedicated entry):
- `0x915e14` — generic "response received with data": signals waiting channel + memcpy payload into channel buffer. Hit by cmds `0x27`, `0x28`, `0x40`.
- `0x915d28` — shared exit / dispatch path:
  - `id == 0` → pure ACK (signal channel, no buffer write)
  - `payload_len > 0` → falls to `0x915e8c`, **same per-id channel-copy as `0x915e14`** (memcpys payload into `data_490e5b0 + 0xa0 + id*8`). This is what lets cmd 0x16's 4-byte response populate the caller's rx_buf even though cmd 0x16 routes here
  - Else `id > 5` → pure ACK
  - Else → state-machine based on byte at `data_490e5b0 + 0xe8 + id`
  - Hit by cmds `0x11/0x13/0x16/0x17/0x19/0x1c/0x21/0x23-0x25/0x30/0x31`
- `0x915d08` — "unknown command" error path. Hit by cmds outside the 0x10-0x41 table.

### Verified handshake values

**Netbridge firmware CRC32** (response for request cmd `0x30`):

```
0x357f5327
```

Computed as zlib CRC32 chained over `netbridge.elf` sections in order `.text → .rodata → .binary_info` (each running CRC seeds the next). *Verified by:* game prints `netbridge image okay, CRC32=0x357f5327` after acceptance.

**Pwrdist "present" response payload** (response for request cmd `0x10`):

```
01 00 00 00 00 00 00
```

Byte 0 = `0x01` produces `data_490e61c = 0x010000`, which is non-zero → game takes the "Spike 3 pwrdist present" branch. Other non-zero values likely work; this is just the one we use. Side effect: bytes 3..5 = `00 00 00` produce `data_490e620 = 0x000000`, which the game prints as `runtime version: 0.0.0` (boot version is printed as `1.0.0`). Cosmetic — the game still takes the "pwrdist present" branch on non-zero boot version.

**Cmd 0x16 response payload**:

```
01 00 02 00
```

`rx[0]=1` (must be > 0), `rx[2]=2` (must equal 2). `rx[1]` and `rx[3]` are stored to caller out-pointers but not checked.

### Async (unsolicited) bridge → host notifications

The bridge emits these on its own — the host doesn't request them.

| Cmd | Trigger | Payload | Source |
|---|---|---|---|
| `0x22` | Sent once at bridge boot, before the main loop starts | 1 byte `resetFlags` (from `watchdog_caused_reboot()`) | `main @ netbridge.elf 0x10001e3c`, line `0x10001ed6` |
| `0x2D` | Sent whenever HOST_XOFF or NODE_XOFF flow-control state changes (checked at top of every `HOST_Update`) | 1 byte: `host_xoff \| (node_xoff << 1)` | `HOST_Update` lines `0x10001214-0x10001242` |

The shim accepts these even though no host cmd was sent — they're how the bridge initially announces itself ("I just rebooted, here's why") and signals ongoing flow-control state. Synthesizing the boot `0x22` once at first connect would mimic the handshake more faithfully; not currently implemented.

### Bridge-command timeouts as progress signal

Some bridge commands **require** the 1000 ms channel timeout to fire — the timeout IS the state-machine transition, not the failure mode. If we synthesize an early ACK, the game interprets our ACK as "response received but no status payload" and keeps polling forever.

Confirmed for cmd **0x41** (handler at `0x915f88`): replacing the default "no synthetic response" branch with a zero-payload ACK sent progress backward — the game stopped reaching Godot init entirely. Reverting to the timeout path restored progress past the Step-31 PopupMenu SEGV and reached four Godot scenes.

Likely also applies to cmds **0x25, 0x12, 0x19** — all of them show up as `bridge message (cmd=XX, id=N) response failed 110: Connection timed out` in the game's stdout, and the game continues after. Marker to look for in a fresh run: `bridge message (cmd=NN, id=M) response failed 110: Connection timed out` is **expected**, not a bug, until proven otherwise.

*Verified by:* Step 45 — `_emulation/run_step45_fix1.stderr` (early-ACK regressed) vs `_emulation/run_step45_fix2.stderr` (timeout-path restored progress).

### Local fallback handlers (bridge-side, when pdstate < 5)

These fire only when the main switch didn't already dispatch (when `POWERDIST_IsSpike3() != 1` so the passthrough doesn't fire, and the cmd is one of these four). For our shim this is the natural model — there's no real powerdist board for the bridge to forward to.

| Cmd | Reply contents | Length |
|---|---|---|
| `0x10` | `{strtol("0",,0), strtol(@0x10011f70,,0), strtol(@0x10011f74,,0)}` (likely a 3-byte version triple) | 3 |
| `0x12` | `min(ADC_GetLineFrequency, 0xff)` clamped | 1 |
| `0x15` | `0xff` (acknowledge stub) | 1 |
| `0x19` | `clamp(ADC_GetLineVoltage, 0..0xffff)` | 2 |

### Specific divergences worth re-checking

- **`0x20` reply uses response cmd `0x10`** (not `0x20`). The game's dispatch computes `idx = CMD - 0x10`, so this routes to slot 0 (cmd 0x10's handler) instead of slot 0x10 (cmd 0x20's handler). Since the cmd 0x10 handler requires exactly 7 bytes and our `0x20` shim returns 4 zero bytes, this would fail the length check if cmd 0x20 ever fires. May be intentional alignment with a different game-side handler, but worth verifying against the bridge's actual reply (which uses CMD `0x20`).
- **`0x25` (NODEBUS_Poll) and `0x41` (NODE block read) silent-ACK revert.** An earlier attempt to silent-ACK these caused breakage; the shim now lets them time out per Step 45. These are high-value to understand because `0x41` in particular is the live encrypted node-bus channel.

---

## 5. Nodebus (cipher, hash, framing)

Two **separate** Stern-custom primitives share the "nodebus" name family but live on different layers and probably came from different design eras. Don't conflate them.

| Primitive | Where it lives | Status in Pokémon LE 0.81 |
|---|---|---|
| **NODEBUS_Cipher** | XOR stream cipher applied to outbound cmd-`0x41` (encrypted node-bus query) traffic | **ACTIVE.** 160+ wrappers around `mcp_nodebus_send_with_retry`. 4 distinct (slot, inner_cmd) pairs observed in boot-time capture |
| **NODEBUS_Hash** | Stern's custom integer-mixer polynomial. Bridge implements it for cmd-`0x40 / sub 0x0d`; some games inline it CPU-side | **ABSENT.** Game does not send cmd `0x40 / 0x0d`, and exhaustive ARM64 lowering scans turned up no inlined polynomial |

Bridge wire format for cmds 0x40 (cleartext) and 0x41 (encrypted) is in §4. This section covers the cipher and hash algorithms themselves.

### 5.1 NODEBUS_Cipher — Stern's per-slot XOR stream cipher

#### Caller chain

```
caller (1 of ~160 templated wrappers around mcp_nodebus_send_with_retry)
   └── mcp_nodebus_send_with_retry @ 0x8ed4a0   (retry / status-byte handling)
         └── mcp_nodebus_send_encrypted @ 0x8ec410   (cipher lives here)
               └── mcp_nodebus_send_raw @ 0x9166c0   (cmd byte = (arg1[0]>>7) + 0x40)
                     └── send_bridge_message_with_response @ 0x9164a0   (UART)
```

`mcp_nodebus_send_encrypted` has three paths gated on `arg1[0]`:

| Condition | Path | Cmd byte sent |
|---|---|---|
| `arg1[0] == 0` | 1-byte zero ping; expects 1-byte reply | 0x40 (cleartext) |
| `arg1[0] & 0x80 == 0` (and non-zero) | cleartext: copy buffer, send, copy reply | 0x40 (cleartext) |
| `arg1[0] & 0x80 != 0` | **encrypted** (this section): cipher payload + checksum, send, decode reply status | 0x41 (`(arg1[0]>>7) + 0x40`) |

The cleartext path is used for bridge-level commands. The encrypted path is used for everything addressed to a specific node (downstream MCU on the RS-485 node bus).

#### Wire frame (encrypted side, before bridge framing)

The caller hands `mcp_nodebus_send_encrypted` an input frame:

```
arg1[0]   = (slot & 0x1F) | 0x80    ; 5-bit slot + encrypt-flag high bit
arg1[1]   = inner_len                ; number of bytes after this header
arg1[2]   = inner_cmd                ; the command byte the destination node sees
arg1[3..] = payload bytes            ; inner_len-1 bytes of inner-cmd payload
arg1[2 + inner_len] = expected_response_len  ; last byte before checksum slot
```

After cipher + checksum, the on-the-wire payload (which becomes the cmd-0x41 frame's payload, then the bridge wraps STX/CMD/ID/CSUM/ETX around it) is:

```
out[0]   = arg1[0]                       ; UNENCRYPTED (slot id)
out[1]   = arg1[1]                       ; UNENCRYPTED (inner_len)
out[2..2+inner_len-1] = encrypted(inner_cmd, payload)
out[2 + inner_len]    = sum_checksum(out[0..2+inner_len-1])
```

The trailing `expected_response_len` byte from the input frame is **overwritten** by the sum-checksum. Total length on the wire = `inner_len + 3` bytes.

#### Cipher algorithm

```
mask = (slot << 4) | ((inner_len + 1) & 0x0F)         # 8-bit derived mask
for i in [0 .. inner_len - 1]:                         # i counts inner_cmd then each payload byte
    k_table = mcp_nodebus_cipher_keys[slot * 16 + (i & 0x0F)]
    k_mask  = (~mask) & 0xFF if (i & 1) else mask      # alternate per parity
    out[2 + i] = orig[2 + i] ^ k_table ^ k_mask
```

Three sources of XOR per byte:
1. **Per-slot key byte** from a 16-byte block at `mcp_nodebus_cipher_keys @ 0x44fcfb8 + slot*16`. Indexed by `i mod 16`, so payloads longer than 16 bytes wrap — there is no per-byte counter or nonce.
2. **Header-derived mask** `(slot << 4) | ((inner_len + 1) & 0x0F)`. Constant for the whole message.
3. **Parity flip** of that mask. Even-indexed bytes XOR with `mask`, odd-indexed bytes XOR with `~mask`.

The cipher is **involutive** (XOR is its own inverse). It is also **outbound only** — `mcp_nodebus_send_encrypted` does not decrypt the response; it `memcpy`s the response payload from the bridge into the caller's `arg2` buffer as-is. The status byte is the response's last byte (after the checksum the bridge strips).

#### Sum checksum

After encryption is in place, the last byte of the frame is overwritten with `(-sum(frame[0..len-1])) & 0xFF` — a one's-bytewise checksum that makes `sum(whole_frame) == 0 (mod 256)`.

#### Encryption is conditional (per-slot status flag)

`mcp_nodebus_send_encrypted` only encrypts if **both**:
1. `inner_cmd` is in `[0x10, 0xEF]` OR equals `0xF2`, AND
2. The per-slot status byte at `mcp_nodebus_per_slot_status + slot*0xC + 3` is non-zero.

With the flag clear (BSS-init state in our shim), the path WRITES 0 at offset `(inner_len+2)`, runs the bridge csum loop, and SKIPS encryption — frame goes out cleartext (despite the slot byte's 0x80 high bit). To verify whether a specific frame on the wire is encrypted: check whether the decoded inner_cmd matches the wire byte raw (cleartext) vs after XOR (encrypted).

#### `inner_len + 2` is checksum, NOT a "hint byte"

Earlier shim work guessed that the wire byte at offset `(inner_len+2)` carried a response-length hint. **Wrong.** The hint (read at `mcp_nodebus_send_encrypted` line `0x8ec4c4` to determine response length) comes from `arg1[inner_len + 2]` — the wrapper's **local buffer**, before csum/encryption. After the cipher path runs, that wire offset is first written to 0, then overwritten by the negated sum. To size shim responses correctly, look up each wrapper's hint via static RE, not by decoding the wire.

#### Per-slot tables (BSS-resident, 32 slots)

All populated at runtime (BSS = zero at load). Layout from `mcp_nodebus_reset_all_slots @ 0x8f61f0`:

| Symbol | Address | Size | Stride | Purpose |
|---|---|---:|---:|---|
| `mcp_nodebus_cipher_keys` | `0x44fcfb8` | 0x200 (512 B) | 16 | Per-slot 16-byte cipher key block |
| `mcp_nodebus_per_slot_status` | `0x44fce38` | 0x180 (384 B) | 12 | Per-slot status struct (offset +11 = the encryption-gating flag) |
| `mcp_nodebus_send_counters` | `0x44fd1c0` | 0x80 (128 B) | 4 | Per-slot send count (uint32) |
| `mcp_nodebus_last_error` | `0x44fce30` | 4 | – | Global last-error code (0 ok, 3 short response, 4 truncated, 1/2 status flags) |

Plus a pair of 4-byte session globals at `0x44fd244` and `0x44fd24c` (zeroed by the same reset). The 32 slots map to physical node addresses on the RS-485 node bus.

#### Senders

**Cleartext (cmd-0x40 path):**

| Function | Sub-cmd | Behavior |
|---|---|---|
| `mcp_nodebus_query_bridge_status @ 0x8ed410` | 0x05 | Snapshot+clear `bridgeStatus`; expects 4-byte reply, ANDs with `~2` and stores to `*arg1` |
| `mcp_nodebus_factory_test_trap @ 0x8f5f4c` | 0x0c | Sends factory-test infinite-loop trap. Never returns on the bridge; host times out |

**Encrypted-path retry shell:** `mcp_nodebus_send_with_retry @ 0x8ed4a0` is the one ~160-caller funnel for encrypted node operations. It gates on `(arg1[0] & 0x80) != 0 && arg1[2] <= 0xEF`, calls `mcp_nodebus_send_encrypted` up to 3 times, reads the returned status byte for retry / re-poll signals (bit 2: trigger `mcp_nodebus_query_bridge_status`; bit 4: retry condition), and indexes `data_4372240[(arg1[0] & 0x3f)] -> +0x1c` for per-slot config.

**Cipher-key fetchers** (the populators of `mcp_nodebus_cipher_keys`):

| Function | Steps |
|---|---|
| `mcp_nodebus_fetch_key_basic @ 0x8f3484` | (0xfd → 12 B preamble) → (0xfc → 16 B key) |
| `mcp_nodebus_fetch_key_with_serial @ 0x8f355c` | Same as basic, then `sprintf("%09d", ...)` formats a node serial-number string into `arg2` |
| `mcp_nodebus_fetch_key_two_probes @ 0x8f36b0` | (0xf9 with payload 0x10000000) → (0xf9 with 0x10001f00) → (0xfc → 16 B key). Defensively zeros the slot's key block on failure |

The `0xfc` 16-byte response is written **directly into** `mcp_nodebus_cipher_keys[slot * 16]`.

**Node firmware flasher:** `mcp_nodebus_flash_node_firmware @ 0x8f5484` — programs a node's MCU over the encrypted channel using inner cmds:
- `0xf603` (with hardware output 0x40 toggle): "power cycling node bus to get into boot mode"
- `0xf311`: bootloader command (expects 4-byte payload)
- `0xf482`: flash-write a 0x80-byte chunk
- `0xfc` / `0xfd` / `0xf9`: aux key/version queries

Lookup table at `data_42317c8` (0x28 stride, ~28 entries) maps known LPC processor IDs to flashing parameters. This is what physically writes `coil4node-LPC1124_303-*.hex` and friends onto the remote node MCUs.

#### Decoded boot-time traffic (185 s capture)

After the cipher decoder was added to `shim_nodebus.c::synthesize_bridge_response` case 0x41, a 185 s headless run produced 29 cmd-0x41 frames covering only **4 distinct (slot, inner_cmd) pairs**:

| slot | inner_cmd | count | payloads observed |
|---:|---|---:|---|
| 0 | `0xf2` | 1 | `[71]` (one-shot, very first cmd-0x41 frame) |
| 0 | `0xf4` | 3 | `[d9 6f]`, `[ea 78]`, `[d9 6f]` |
| 1 | `0xed` | 17 | `[93]` (poll loop) |
| 1 | `0xee` | 8 | `[ea 95]` (paired with the `0xed` poll) |

Slots 2–31 saw zero traffic in the same window. None of the `0xfc / 0xfd / 0xf9` key-fetch family fired — the game must skip provisioning when the bridge timeout pattern is in effect, or sequence those after some other prerequisite.

The first frame the game emits (`slot=0 inner_cmd=0xf2 payload=[71]`) was independently predictable from the cipher recipe before this run and matches byte-for-byte — confirming the cipher recipe end-to-end (encryption HLIL → C decoder → live capture).

#### Python reference implementation

```python
def nodebus_encrypt_frame(slot: int, inner_cmd: int, payload: bytes,
                          response_len: int, key_block: bytes) -> bytes:
    """
    Build and encrypt a cmd-0x41 frame.
      slot:         5-bit slot index (0..31)
      inner_cmd:    arg1[2]
      payload:      bytes after inner_cmd (each byte will be encrypted)
      response_len: caller's expected response length (gets overwritten by checksum)
      key_block:    16 bytes from mcp_nodebus_cipher_keys[slot*16 .. +16]
    Returns the on-the-wire frame (length = inner_len + 3 bytes).
    Cipher is involutive: same function inverts the encryption.
    """
    assert 0 <= slot < 32 and len(key_block) == 16
    inner_len = len(payload) + 1
    frame = bytearray()
    frame.append(slot | 0x80)
    frame.append(inner_len & 0xFF)
    frame.append(inner_cmd & 0xFF)
    frame.extend(payload)
    frame.append(response_len & 0xFF)        # placeholder; checksum overwrites

    mask = (slot << 4) | ((inner_len + 1) & 0x0F)
    for i in range(inner_len):
        k_table = key_block[i & 0x0F]
        k_mask  = (~mask) & 0xFF if (i & 1) else (mask & 0xFF)
        frame[2 + i] = frame[2 + i] ^ k_table ^ k_mask

    csum = (-sum(frame[:inner_len + 2])) & 0xFF
    frame[inner_len + 2] = csum
    return bytes(frame)
```

Verified against BN HLIL byte-for-byte. With zero keys (boot state), `nodebus_encrypt_frame(slot=5, inner_cmd=0xfc, payload=b'', response_len=0x10, key_block=b'\x00'*16)` returns `85 01 ae cc` — the actual first-ever wire frame the game emits for the cmd-`0xfc` key fetch on slot 5.

---

### 5.2 NODEBUS_Hash — Stern's custom integer mixer

#### Status in Pokémon LE 0.81

**Absent.** Game does not send cmd `0x40 / 0x0d`. Exhaustive ARM64 lowering scans of the game binary (12.95M instructions across 52 MB of `.text`) returned zero polynomial-shaped candidates after ruling out audio/libpng false positives. The polynomial is also not present in the netbridge runtime path the game exercises (no `0x40/0x0d` frames sent). **The hash machinery is dormant in this title.**

#### `NODEBUS_Hash` is not a function symbol

Common confusion: `NODEBUS_Hash` is a protocol-documentation label, **not** a callable symbol. There is no entry point. In `netbridge.elf` the polynomial is **inlined** inside `HOST_Update` at VAs `0x10001618..0x10001634` (Thumb-1) — sitting inside the cmd-`0x40` / sub-`0x0d` case of the dispatch switch. `BN search_functions_by_name "NODEBUS"` returns only `NODEBUS_Poll`. In any game binary that inlines a CPU-side copy, expect the same shape: open-coded fragment, not a named function. Hunt by *code shape*.

#### The polynomial

```
y       = ~(x * 0x7FFF)         ≡ (x * 0xFFFF8001) - 1   (mod 2^32)
m1      = (y ^ (y >> 10)) * 9
m2      = (m1 >> 6) ^ m1
z       = ~(m2 * 0x7FF)         ≡ (m2 * 0xFFFFF801) - 1  (mod 2^32)
hash(x) = (z >> 16) ^ z
```

All arithmetic is modulo `2^32`. Inputs and intermediates are `uint32_t`. The two multiplications are negative, hence the `NOT`/`-1` form: the compiler typically emits them as shift-subtract sequences rather than a multiply.

*Verified by:* direct Thumb-1 disassembly of `HOST_Update` in `netbridge.elf` VAs `0x10001618..0x10001634`. The earlier draft's `0x80008001` and `0xfffff801` constants were wrong (Step 65 correction).

#### Operations on top of the bare hash

`NODEBUS_Hash` exposes a 4-slot seed table `hashSeed[4]` (the bridge's secret; written by host, never read back) and dispatches via `(slot_idx, op)`:

| op | Operation | Returns |
|---:|---|---|
| 0 | `hashSeed[slot_idx] = value` (write seed) | hash of mixed result, but the write is the side effect |
| 1 | `hash(value)` | bare hash, no seed |
| 2 | `hash(value) ^ hashSeed[slot_idx]` | post-XOR with seed |
| 3 | `hash(value ^ hashSeed[slot_idx])` | pre-XOR with seed |
| 4 | `hash(value ^ hashSeed[slot_idx]) ^ hashSeed[slot_idx]` | both pre- and post-XOR |

Inputs to the bridge command (cmd `0x40` / sub `0x0d`):
- `slot_idx = (data[2] >> 4) & 3`
- `op       = data[2] & 0xf`
- `value    = data[3..6]` (4 bytes, little-endian uint32)

The bridge is the only side that knows `hashSeed`; the host never reads it back. This makes the structure a **challenge/response authentication primitive**: the host can ask the bridge to hash a known input with an unknown seed, and the bridge proves it knows the seed by returning the result.

#### Reference implementation

```c
#include <stdint.h>

static inline uint32_t nodebus_hash(uint32_t x) {
    uint32_t y  = ~(x * 0x7FFFu);
    uint32_t m1 = (y ^ (y >> 10)) * 9u;
    uint32_t m2 = m1 ^ (m1 >> 6);
    uint32_t z  = ~(m2 * 0x7FFu);
    return z ^ (z >> 16);
}
```

Python equivalent (with seed-table dispatch) is in JOURNAL Step 65 and the deleted `NODEBUS_HASH.md`; the C above is sufficient for verification.

#### ARM64 hunting checklist (for other Spike 3 builds)

The polynomial fits in ~14–18 ARM64 instructions. Strongest "shape" signature is **all four** of these within ±256 bytes:

1. `LSR _, _, #10`
2. `LSR _, _, #6`
3. `LSR _, _, #16`
4. `LSL _, _, #11` (or `SUB _, _, _, LSL #11`)

Plus `LSL _, _, #15` (or `SUB _, _, _, LSL #15`) within ±256 bytes is very strong corroboration.

For the multiplies (`x * -0x7FFF`):

| Form | Pattern | Distinctiveness |
|---|---|---|
| `SUB Wd, Wn, Wn, LSL #15` (self-shift) | `0x4B003C00` mask `0xFFE0FC00`, `Rn==Rm` | Very rare; LSL #15 on its own is uncommon. Strongest single-instruction signature |
| `SUB Wt, WZR, Wm, LSL #15`; `ADD Wd, Wt, Wm` | LSL #15 with `Rn==WZR`, then `ADD` two insns later | Two-insn split form |
| `LSL Wt, Wm, #15`; `SUB Wd, Wp, Wt` | UBFM-alias LSL + later SUB | Compiler-picky split |
| `MOVZ Wn, #0x7FFF` + `MUL Wd, Wm, Wn` | `0x528FFFE0` mask `0xFFFFFFE0` + nearby MUL | Register-loaded (positive multiplicand) |
| `MOVN Wn, #0x7FFE` + `MUL Wd, Wm, Wn` | `0x1280FFC0` mask `0xFFFFFFE0` + nearby MUL | Register-loaded (negative `0xFFFF8001`) |
| `MOVZ Wn, #0x7FFF` + `MNEG Wd, Wm, Wn` | `MNEG` = `0x1B00FC00` mask `0xFFE0FC00` | MNEG fuses negate+multiply |

Same patterns with `#15 → #11` and `#0x7FFF → #0x7FF` for the second multiply.

The `* 9` between the first XOR and `m2` is typically lowered to `ADD Wd, Wm, Wm, LSL #3`.

The constants `0x7FFF` and `0x7FF` are common (audio saturation, byte masks) — don't lead with constants; lead with the distinctive shift sequence.

#### Known false-positive shapes in Pokémon LE 0.81

When scanning `games/pokemon_le/game`, several functions trip individual polynomial-component patterns but are **not** the hash:

| Function | What looks like a hit | Actual purpose |
|---|---|---|
| `sub_11ed6f0`, `sub_11f172c` | Many `MOVZ #0x8001` + `MOVK #0x8000 lsl#16` pairs (loading `0x80008001`) | libpng `png_compose` / channel-loss check. `0x80008001` is the magic-number divider for `x / 0xFFFF`. Identified by `png_error` strings + `png_get_channels` calls |
| `sub_1042214` | `SUB W6, W8, W8, LSL #11` (the only self-shift `SUB-LSL-#11` in the binary) AND `MOVN W14, #0x07FE` (loading `0xFFFFF801`) | Signed Q-format audio-sample saturation. `x - (x << 11) = x * -0x7FF` is being used as a Q-format magnitude scaler; `0xf801` halfword stores are bit-pack endpoints of the +/-0x7FF saturation range |
| `sub_9b7590`, `sub_9b9820`, `sub_9bba80`, `sub_9bd9a8` | `MOVZ #0x7FFF` + `MOVN #0x7FFE` register-paired loads, plus X-reg `LSR #10` + `LSR #16` shift co-locations | Bespoke audio codec rounds. `MOVZ #0x7FFF` / `MOVN #0x7FFE` load the +/-0x7FFF saturation clamps for 15-bit signed PCM samples. The `0x4000` rounding bias and `(1 << (width-1)) - 1` overflow check give it away as a sample bit-width normalizer. The `0x1b` constants nearby are GF(2^8) reduction (AES MixColumns coefficient) |

If a future scan flags any of these functions, refer here before acting.

## 6. image.bin file format

`image.bin` is Stern's proprietary asset container. **1.05 GiB** in Pokémon LE 0.81 (`0x42f323ee` bytes). Memory-mapped at runtime via `mmap64` in `load_image_bin_file`. The file holds two **independent lookup systems** (audio + scenes) plus a handful of header/manifest blocks.

The runtime hashmap `g_asset_registry @ 0x48ca4d0` (a libstdc++ `std::unordered_map`) is a runtime cache of section 7's records — verified 2452/2452 match against the section-7 directory by walking `_M_nxt` chains via the shim's `shim_dump_sound_globals` and parsing the dump in `_extraction/tools/spike_extract/registry_dump.py`. Static extraction from image.bin alone is sufficient.

### File header (176 bytes)

```
+0x00  uint8       header size (= 0xb0 = 176)
+0x01  uint8[7]    pad / reserved (zero)
+0x08  uint64 LE   section[0] file offset
+0x10  uint64 LE   section[1]
+0x18  uint64 LE   section[2]
+0x20  uint64 LE   section[3]
+0x28  uint64 LE   section[4]
+0x30  uint64 LE   section[5]
+0x38  uint64 LE   section[6]
+0x40  uint64 LE   section[7]
+0x48  uint64 LE   section[8]
+0x50  uint64 LE   section[9]    ← packed (count_hi:32 | type_lo:32), NOT an offset
+0x58…+0xaf       16 + 64 bytes of additional fields (counts + magic/hash bytes)
```

### Section table (Pokémon LE 0.81 values)

| Hdr idx | File offset | Size hint | Verified content | Status |
|--:|--:|--:|---|---|
| 0 | `0x0001b800` | — | EULA + version manifest. First 8 bytes: `40 00 00 00` (length 0x40?) + `8e 70 f3 42` (uint32 — possibly checksum). Then ASCII: `"STERN PINBALL\nEND USER LICENSE AGREEMENT\n\nThis limite…"` | partial — header bytes uncharacterized |
| 1 | `0x0001b7e0` | — | Audio/asset config header. uint32 LE @ +0 = `44100` (audio sample rate). **Byte +7 = `0x05` is the per-asset variant stride** used by `get_asset_ptr_by_packed_id` to index section 5 | partial — beyond byte +7, fields uncharacterized |
| 2 | `0x000189b8` | 0x2E28 (11,816 B) | Playfield/mode grid records. 1,477 uint64 pointers into section 0 (and adjacent regions). Most records are 408-byte `{record_id:u32, flag:u32, w:u16=17, h:u16=23, pad[5], cells[391]}` grids; later records (delta != 408) likely have different dimensions | verified — see "Section 2: playfield grid records" below |
| 3 | `0x00018658` | — | Aliases section 8 (same offset). Likely a parallel/duplicate header pointer — both registers resolve to the same data | aliased to §8 |
| 4 | `0x000000f8` | — | Aliases section 5 (same offset). Likely parallel/duplicate header pointer | aliased to §5 |
| 5 | `0x000000f8` | 12,568 × 8 B = 100,544 B | **Per-asset bytecode descriptor table.** uint64 file offsets to bytecode in the bulk region, grouped in 5-tuples (one per language variant). 2,492 unique offsets in `[0x42EEEB3C, 0x42F23E06)` | verified — see "Section 5: per-asset descriptor table" below |
| 6 | `0x42f323ea` | 4 B | File trailer. Last 4 bytes of the file (`96 19 d0 f5` here) — likely a CRC32 trailer | partial — bytes confirmed; semantic unverified |
| 7 | `0x42f23e06` | 2,452 × 24 B = 58,848 B | **Asset directory** (audio in PLE 0.81). 2,452 fixed-size records per `spike_extract`. Verified 100% intersection with runtime `g_asset_registry` hashmap | verified — see "Section 7: asset directory" below |
| 8 | `0x00018658` | — | **Scene metadata table** consumed by `load_scene_assets`. Records are 24-byte stride per the decompile's `i = &i[0x18]` and `mulu.dp.d(x0, 0x18)`. Fields per record (offsets within record): `+0 = uint64` (some pointer/value), `+8 = encoded packed_id field` (masked with `0xfffc0003ffffffff`), `+0x10 = uint32 size`. Per-record count from `get_image_meta_field(arg1, 4)`. The same offset as section 3 — they alias | partial — record format observed but not all fields decoded |
| 9 | `0x000005c50000001b` | — | **Not a file offset.** Packed value: `count_hi:32 = 0x000005c5 = 1477`, `type_lo:32 = 0x0000001b = 27`. Likely a count + type tag for one of the other sections (1477 matches section 2's pointer count exactly) | partial — interpretation inferred but consistent |

The file body (`~0x1c000` to `0x42f23e06`) is the bulk asset data — section 5's offsets and section 7's `file_offset` fields point into it. Section 7 (audio directory) lives at the very end of the file just before the 4-byte trailer.

*Verified by:* direct `hexdump` of image.bin header at offset 0x00 + parsing the 10 uint64 LE section pointers; runtime `mmap64` confirmed in `load_image_bin_file @ 0x847204`.

### Asset resolution chain

The game has TWO independent lookup paths into image.bin:

#### Audio path (used by ALSA writer + sound-event registration)

```
sound_event_name (e.g. "SE_PK_FX_ATTACK_TYPE_FIRE")
   ↓ sub_b09660 over std::unordered_map @ data_48c56d0
uint16_t token (e.g. 0xc0)
   ↓ runtime-populated mapping by load_scene_assets walking type-0x0b records (NOT static)
uint64 encoded_id = (flags << 32) | asset_id_low
   ↓ asset_registry_find over g_asset_registry @ 0x48ca4d0
{file_offset, size, type}
   ↓ get_asset_ptr_by_packed_id → image_mapped_base_plus_offset
absolute pointer inside mmap'd image.bin → PCM samples
```

Token → asset_id is **not in compiled code**. It's populated at runtime when `load_scene_assets` walks each scene's bytecode and inserts type-0x0b records. Per-scene variants are possible. Static extraction can recover (asset_id_low, flags, size, file_offset) from section 7 directly; the name → asset_id mapping requires runtime capture.

*Verified by:* JOURNAL Step 41 (stages 1-2), Step 42 (stages 3-5), Step 43 (g_asset_registry / section-7 perfect intersection).

#### Scene path (used by select_and_load_scene)

```
scene_id (e.g. 10)
   ↓ scene_config_table[scene_id * 4] @ 0x43fcf10
asset_id (1:1 for low scene_ids — scene 10 → asset_id 10)
   ↓ get_asset_ptr_by_packed_id (asset_id, ...) @ 0x72c470
section_5_base + (x21 + asset_id_low * stride) * 8
   ↓ uint64 LE deref (section[5] = 0xf8; stride = section[1] byte +7 = 5)
file_offset into image.bin body
   ↓ image_mapped_base_plus_offset
absolute pointer → scene bytecode
   ↓ interpreted by select_and_load_scene's bytecode walker
loaded scene state
```

`x21` (the variant index 0..4) comes from `sub_7d8200`, which returns the current language/region selector. Default value 0 — all 5 variants for scenes 0-12 currently point to the same offset (scenes are language-agnostic in this build).

*Verified by:* `hexdump_address 0x43fcf00` + 0xf8 in image.bin; live trace of scene 10 → asset_id 10 → file offset 0x42eeebad via BRK trap (`shim_discovery.c`, runs/2026-05-04T16:30:01).

### Section 5: per-asset bytecode descriptor table

12,568 uint64 slots (100,544 bytes) starting at file offset `0xf8`, grouped in 5-tuples (one per language variant). 2,492 unique bytecode offsets in the bulk region `[0x42EEEB3C, 0x42F23E06)`.

For asset_id N, variant V: `file_offset = section_5_base[N * 5 + V]`. All 5 variants for any given asset_id 0..N point to the same offset in PLE 0.81 (no per-language scene swaps observed in low-id range).

#### Bytecode tag frequency (across all 2,492 bulk bytecodes)

Each bytecode is a stream of variable-length tagged records terminated either by tag 0x00 OR by reaching the next unique section-5 offset (whichever comes first). Decoder must bound each walk by the next unique offset, not by tag 0x00 alone — bytecodes for adjacent scene_ids share trailing data (scene 11's bytecode IS scene 10's bytecode shifted 25 bytes; scene 12's IS scene 10's bytecode shifted 50 bytes).

| Tag | Size | Count across all bulk bytecodes | Typical role |
|---:|---:|---:|---|
| `0x05` | 6 | 2,496 | Header — byte 2 is 256-distinct (matches section-7 flag count) |
| `0x02` | 2 | 2,577 | Short reference |
| `0x0b` | 10 | 2,414 | **Asset reference** — carries asset_id_low at payload offset +3 |
| `0x09` | 15 | 4,483 | Animation/transform data |
| `0x04` | 2 | 108 | Short reference (secondary variant) |

#### Type-0x0b asset reference payload (10 bytes)

When the bytecode walker encounters byte `0x0b`, it reads the next 10 bytes as the asset reference payload:

| Byte | Distinct values | Meaning |
|---:|---:|---|
| 0-2 | always `0x00` | padding |
| 3 | 16 | `asset_id_low` bits 0-7 |
| 4 | `{0x00, 0x04}` | `asset_id_low` bits 8-15 |
| 5 | `{0x00, 0x02}` | `asset_id_low` bits 16-23 (bit 17 is the stereo-audio marker) |
| 6 | `{0x00, 0x20}` | `asset_id_low` bits 24-31 (bit 29) |
| 7 | `{0x00, 0x02}` | partial flag bit (word bit 1) |
| 8 | always `0x00` | padding |
| 9 | 8 values | partial flag bits (word bits 20-22 after `<< 16`) |

`u32 LE` read at payload offset +3 is a valid `asset_id_low` for every one of the 2,414 records (100% match against the 64 distinct id_lows in section 7).

The 32-bit `flags` word of the section-7 composite key (`(flags << 32) | id_low`) is **partially recoverable** from this payload alone. Bytes 7 & 9 of the 0x0b record together encode at most 16 combinations, but section 7 has 256 distinct flag values. The likely missing source is byte 2 of the preceding type-`0x05` header (which has 256 distinct values across all 2,496 `0x05` records — matching the section-7 flag cardinality exactly). Best static brute-force formula is `flags = (H[0] << 24) | P[7] | (P[9] << 16)` and yields 660/2414 matches; full reconstruction needs more decoding work or runtime capture.

For partial extraction (asset_id_low only), the type-0x0b payload at byte +3 is sufficient.

*Verified by:* JOURNAL Step 44; runtime BRK on `select_and_load_scene` confirms scene 10 bytecode at file offset 0x42eeebad starts with `05 04 01 f5 09 00 00 02 00 ...` matching the expected tag layout.

### Section 7: asset directory (audio in PLE 0.81)

Fixed-size records, 24 bytes each. 2,452 records total (= the runtime `g_asset_registry._M_element_count`).

| Bytes | Type | Field |
|---|---|---|
| 0-7 | `uint64 LE` | `file_offset` into image.bin where the asset data lives |
| 8-15 | `uint64 LE` | `(flags << 32) \| asset_id_low` — composite encoded ID |
| 16-23 | `uint64 LE` | `(hash_or_cksum << 32) \| size` — `size` is the low 32 bits (element count, NOT byte count) |

Asset byte size = `size * element_size`, where `element_size` is **2** when bit 6 of `asset_id_low` is clear and **4** when set.

The composite `encoded_id` is the same `(flags << 32) | asset_id_low` u64 the runtime hashmap uses as key. Walking the hashmap and dumping it produces a 100% intersection with the section 7 directory (zero `reg-only`, zero `img-only`).

#### Classification (audio counts in PLE 0.81, per `spike_extract`)

| Category | Rule | Count |
|---|---|---:|
| `audio_stereo_music` | 4-byte/elem (bit 6 of aid_low) + stereo marker (bit 17 of aid_low) + duration ≥ 30 s | 35 |
| `audio_stereo_sfx` | 4-byte/elem + stereo marker + duration < 30 s | 312 |
| `audio_mono` | 2-byte/elem, audio shape (bit 7 set, bits 8/9 clear) | 2,105 |

User-validated: all 2,452 confirmed audio (no encryption applied to the bulk data per JOURNAL Step 43+65 + manual auditioning). The audio_mono bucket may include some animation/coordinate int16 arrays as false positives — flags byte further disambiguates if needed.

*Verified by:* `_extraction/tools/spike_extract` produces 2,452 records matching runtime hashmap dump; user-validated audio playability.

### Section 2: playfield grid records

Section 2 @ `0x189b8`, length `0x2E28` (11,816 bytes). 1,477 uint64 LE pointers into section 0 (and adjacent regions). Pointer deltas vary: 408 most common, then 40, 64, 48, 56, 88.

First 12 records (and most `delta == 408` records) have identical shape:

```
+0x00  uint32 LE   record_id (sequential 1, 2, 3, …)
+0x04  uint32 LE   flag/variant (alternating 0 / 2)
+0x08  uint16 LE   width  (17)
+0x0a  uint16 LE   height (23)
+0x0c  uint8[5]    padding
+0x11  uint8[391]  grid cells (17 × 23, values are small enums: 0x00, 0x03, 0x0f, …)
+0x188 total size 408
```

Later records (delta != 408) likely have different grid dimensions.

**This is NOT a scene → asset mapping.** Zero of the section-2 uint32 values match any `asset_id_low` from section 7, and zero uint64 values match any section-5 offset. The records hold playfield lamp/target/insert state tables used by gameplay logic per mode or per animation frame.

*Verified by:* JOURNAL Step 44.1.

### Multi-image sub-files (image-sc%02d.bin)

`load_image_bin_file @ 0x847204` selects the on-disk filename based on `image_id`:

```c
if (image_id != 0)
    sprintf(filename, "image-sc%02d.bin", image_id);
else
    strcpy(filename, "image.bin");
```

So image_id 0 → `image.bin`; image_id N → `image-sc01.bin`, `image-sc02.bin`, etc. Per-image-id mappings are stored in `image_tree_root @ 0x44df960` (an rb-tree keyed by image_id). `get_image_filename(image_id)` walks the tree; if not found, returns `"image.bin"`.

Pokémon LE 0.81 ships **only `image.bin`** — no `image-sc??.bin` files. The 16-bit `image_id` portion of every `asset_id` (the upper 16 bits, `asset_id >> 16`) is `0` for every asset extracted to date. Other Spike 3 games (e.g. Spike 2 cabinets per the user's reference list of 29 sub-files) split assets across `image-sc01.bin` through `image-sc29.bin`.

*Verified by:* directory listing of `games/pokemon_le/` (no `image-sc*` files); BN decompile of `load_image_bin_file` and `get_image_filename @ 0x846cd8`.

### Sub-section: cmode "scene_config_table" misnaming

The 1,254-line `scene_config_table.c` export in `_extraction/_outdated/` covers VA 0x48071d0 onward, which holds the Spike 3 **service/test/adjustments menu tree** (Active Switch Test, Game Adjustments, Pikachu Tests and Calibration, Exit the test menu, plus a handful of audio-test items: "MUSIC: MAINPLAY", "SOUND: SUPER SKILL SHOT", "SPEECH: 'SQUIRTLE'"). Each entry is a `{function_ptr, label_string, u16_index, u16_parent_id}` quadruple. This is the menu structure Nuklear walks when the operator enters test mode.

The **real `scene_config_table`** is at VA `0x47fcf10` (Pokémon LE 0.81), 41,664 bytes earlier. It is either BSS (populated at runtime by an unidentified setup function) or static data not yet exported from the `.bndb`. Closing the 885 scene → asset_id mapping requires either re-exporting that region or capturing it at runtime.

If the export file appears under a different name in a future session, it should be `service_menu_table.c` for clarity.

---

## 7. Asset extraction (spike_extract)

`_extraction/tools/spike_extract/` is a pure-Python standalone extractor for `image.bin`. **No emulator, no QEMU, no game binary required at extraction time** — parses `image.bin` section 7 directly and writes classified output. See §6 for the file format the tool consumes.

### Usage

```bash
# Extract every asset from image.bin into out/
python -m spike_extract --image games/pokemon_le/image.bin --out out/

# With registry-dump validation (optional; confirms section-7 parse against runtime)
python -m spike_extract \
    --image games/pokemon_le/image.bin \
    --registry games/pokemon_le/_emulation/asset_registry.bin \
    --out out/ --csv out/index.csv

# Dry-run — print summary + histogram, write nothing
python -m spike_extract --image games/pokemon_le/image.bin --dry-run
```

`asset_registry.bin` (optional input) is produced by the emulation shim's `shim_dump_sound_globals` — it walks the runtime `_Hashtable` struct + `_M_nxt` chain and dumps a binary file the registry parser can consume. Used only as validation that the section-7 parser recovered the same 2,452 entries the runtime hashmap holds. On a correct parse: 100% intersection, zero mismatches.

*Verified by:* `_extraction/tools/spike_extract/__main__.py` argument handling; verified-working extraction in `_extraction/spike_extract_out/`.

### Output layout

```
out/
├── audio_stereo_music/         # 35 WAVs (4-byte/elem stereo, duration ≥ 30 s)
├── audio_stereo_sfx/           # 312 WAVs (4-byte/elem stereo, duration < 30 s)
├── audio_mono/                 # 2,105 WAVs (2-byte/elem, audio shape)
└── index.csv                   # 2,453 lines (header + 2,452 records)
```

Subdirectories are created lazily — empty categories leave no folder.

### `index.csv` schema

```
index, file_offset, asset_id_low, flags, encoded_id, size_elements, element_bytes, byte_size, hash_or_cksum, category
```

| Column | Type | Source field |
|---|---|---|
| `index` | int (0-based) | position in section 7 |
| `file_offset` | hex `0x…` | record bytes 0-7 |
| `asset_id_low` | hex `0x…` (low 32) | record bytes 8-11 |
| `flags` | hex `0x…` (high 32 of bytes 8-15) | record bytes 12-15 |
| `encoded_id` | hex `0x…` (full 64-bit composite) | `(flags << 32) \| asset_id_low` |
| `size_elements` | int | record bytes 16-19 (low 32) — **element count, NOT bytes** |
| `element_bytes` | int | derived: 4 if `asset_id_low & 0x40` else 2 |
| `byte_size` | int | `size_elements * element_bytes` |
| `hash_or_cksum` | hex `0x…` | record bytes 20-23 (high 32 of bytes 16-23) |
| `category` | string | classification: `audio_stereo_music` / `audio_stereo_sfx` / `audio_mono` (see §6) |

### Pokémon LE 0.81 extraction summary

| Category | Count | Total size | Notes |
|---|---:|---:|---|
| `audio_stereo_music` | 35 | 331 MB | All 3 originally-confirmed music tracks included |
| `audio_stereo_sfx` | 312 | 180 MB | Stereo SFX pool. Classification rule = `id_low & 0x20040` bit pattern |
| `audio_mono` | 2,105 | 566 MB | **All confirmed audio** — user-validated 2026-04-27, no animation false-positives |
| **Total** | **2,452** | **1.1 GB** | Matches image.bin section-7 record count exactly |

Every section-7 record is audio in this build. Nothing is encrypted (validated by manual auditioning + JOURNAL Steps 41, 43, 65).

*Verified by:* `_extraction/spike_extract_out/index.csv` (2,452 rows + header) + manual audition of representative samples per category.

### Open gaps

- **SE_PK_* name → asset_id mapping is NOT in compiled code.** Token resolution happens at runtime via type-0x0b records during scene-bytecode walking (see §8 audio pipeline + §6 image.bin format). Static extraction recovers (asset_id_low, flags, size, file_offset) but not the SE_PK_* names. Only runtime capture (past the Step 31 SEGV) or a CM4 hardware memory dump would yield the full name → asset_id table.
- **Non-audio content in image.bin** (scene bytecode in section 5, playfield grids in section 2) is not extracted by this tool. The tool's scope is section 7. Other section parsers would be additional tooling.

### Portability to other Spike 3 games

The section-7 format is stable across Stern's Spike 3 codebase, so the parser works on any `image.bin` from any Spike 3 game. Per-game tuning needed only on the classification heuristic — different games may use different `flags` encodings. For games shipping `image-sc??.bin` sub-files (see §6 multi-image), each is parsed the same way with its own section-7 directory.

---

## 8. Audio pipeline

### Game-side ALSA imports

The game links against `libasound.so.2` and imports **18 playback-only symbols** — no capture, no sequencer, no mixer. From `nm -D --undefined-only`:

```
snd_pcm_open, snd_pcm_close, snd_pcm_prepare, snd_pcm_drain, snd_pcm_nonblock,
snd_pcm_writei,
snd_pcm_hw_params, snd_pcm_hw_params_any, snd_pcm_hw_params_sizeof,
snd_pcm_hw_params_set_{access,format,channels,rate,rate_near,buffer_size,period_size},
snd_pcm_hw_params_get_{channels,rate}
```

The audio thread feeds `snd_pcm_writei` with **S16_LE stereo interleaved buffers at 44100 Hz** (2048 frames = 8192 bytes per call, ~46 ms cadence).

Game-side functions:
- `alsa_pcm_init @ 0xce61e0` — checks `snd_pcm_open`'s return at `0xce6270` (`tbnz` bit 31 → error-7 path)
- `alsa_audio_thread @ 0xce5f68` — tight `writei` loop with no intrinsic `usleep`

#### Failure mode under un-shimmed QEMU

Game calls `snd_pcm_open` which returns nonzero error (no `/dev/snd/*` in qemu-user-mode), ignores the return code, then calls `snd_pcm_prepare` with the uninitialized PCM pointer. Glibc's `snd_pcm_prepare` asserts `pcm != NULL` at `pcm.c:1239` and aborts. The shim's full ALSA stub (below) prevents this.

*Verified by:* direct observation of un-shimmed run (Step 25); BN decompile of `alsa_pcm_init` and `alsa_audio_thread`.

### Shim ALSA stubs

All 18 symbols are stubbed in `_emulation/shim_inject.c`:
- `snd_pcm_open` returns a `calloc(256)` scratch handle with return value `0`
- `hw_params` setters/getters: no-op success
- `rate_near` writes 44100 to `*val`
- `get_rate` / `get_channels` return 44100 / 2
- `snd_pcm_writei` paces with `usleep(size * 1e6 / 44100)` (46.4 ms per 2048-frame buffer), appends the raw S16_LE interleaved stereo buffer to `_emulation/captures/audio/stream.s16le`, flushes every 64 writes, and tees to the audio bridge if `AUDIO_BRIDGE=1`

To convert the captured stream to WAV: `ffmpeg -f s16le -ar 44100 -ac 2 -i stream.s16le stream.wav`.

*Verified by:* Step 27 run — 11.7 MB / 66.4 s of real-time audio captured in a single run, audio thread did NOT spin (CPU usage stayed bounded), game progressed into scene-loader territory.

### Sound event registration (game-side)

`sub_8d3460 @ 0x8d3460` (called during C++ static init before GDScript runs) populates a `std::unordered_map<std::string, uint16_t>` at `data_48c56d0` (BN VA `0x48c56d0`) with **25 sound-event name → token entries**.

Each registration is:

```c
sub_b094c0(&data_48c56d0, &"SE_PK_FX_…", token, 0xffffffff)
```

Insertion path: `sub_b094c0` (insert helper) → mutex-guarded via `sub_b09370` → hash via `sub_b09850` using `std::_Hash_bytes` with seed `0xc70f6907` (= `std::hash<std::string>`).

Bridge signature (Itanium-mangled):
```
GodotSpikeBridge::register_sound(std::string const& name, uint16_t token, int flags=0xffffffff)
```

The `t` in the mangled name confirms `uint16_t` (NOT `uint32_t`).

The Godot bridge exposes this method to GDScript via a `std::function<void(Array const&)>` adapter lambda (typeinfo at BN VA `0x46bb1f8`, libstdc++ manager at `sub_b086b4`). The lambda unpacks an `Array` from GDScript and calls into `register_sound` with typed arguments.

#### Token namespace (verified table)

| Token range | Count | Name prefix |
|---|---:|---|
| `0x6b–0x71` | 7 | `SE_PK_FX_EOB_POKEDEX_*` |
| `0xba–0xcb` | 18 | `SE_PK_FX_ATTACK_TYPE_*` |

#### Full 25-entry table

| Token | Name |
|---:|---|
| `0x6b` | `SE_PK_FX_EOB_POKEDEX_PAGE_TURN` |
| `0x6c` | `SE_PK_FX_EOB_POKEDEX_POKEMON_CAUGHT_LARGE_SCREEN` |
| `0x6d` | `SE_PK_FX_EOB_POKEDEX_POKEMON_CAUGHT_SMALL_SCREEN` |
| `0x6e` | `SE_PK_FX_EOB_POKEDEX_RESULTS` |
| `0x6f` | `SE_PK_FX_EOB_POKEDEX_SCREEN_DOWN` |
| `0x70` | `SE_PK_FX_EOB_POKEDEX_SCREEN_UP` |
| `0x71` | `SE_PK_FX_EOB_POKEDEX_BACKGROUND` |
| `0xba` | `SE_PK_FX_ATTACK_TYPE_BUG` |
| `0xbb` | `SE_PK_FX_ATTACK_TYPE_DARK` |
| `0xbc` | `SE_PK_FX_ATTACK_TYPE_DRAGON` |
| `0xbd` | `SE_PK_FX_ATTACK_TYPE_ELECTRIC` |
| `0xbe` | `SE_PK_FX_ATTACK_TYPE_FAIRY` |
| `0xbf` | `SE_PK_FX_ATTACK_TYPE_FIGHTING` |
| `0xc0` | `SE_PK_FX_ATTACK_TYPE_FIRE` |
| `0xc1` | `SE_PK_FX_ATTACK_TYPE_FLYING` |
| `0xc2` | `SE_PK_FX_ATTACK_TYPE_GHOST` |
| `0xc3` | `SE_PK_FX_ATTACK_TYPE_GRASS` |
| `0xc4` | `SE_PK_FX_ATTACK_TYPE_GROUND` |
| `0xc5` | `SE_PK_FX_ATTACK_TYPE_ICE` |
| `0xc6` | `SE_PK_FX_ATTACK_TYPE_NORMAL` |
| `0xc7` | `SE_PK_FX_ATTACK_TYPE_POISON` |
| `0xc8` | `SE_PK_FX_ATTACK_TYPE_PSYCHIC` |
| `0xc9` | `SE_PK_FX_ATTACK_TYPE_ROCK` |
| `0xca` | `SE_PK_FX_ATTACK_TYPE_STEEL` |
| `0xcb` | `SE_PK_FX_ATTACK_TYPE_WATER` |

GDScript call pattern: `Spike.play_sound(SpikeSoundIds.SE_PK_FX_<NAME>)` passes the string. The C++ bridge looks it up in the hashmap to resolve to a token.

*Verified by:* JOURNAL Step 41; BN decompile of `sub_8d3460` and the helper chain; Itanium demangling of the register_sound symbol.

#### NOT in the hashmap

~725 other `SE_PK_*` strings from the previs `spike_game_sound_ids.gd` autoload (VO, TUNE, MX, BONUS, THEME, and several FX families beyond attack-type / EOB-pokedex) are **not** in this hashmap. Their dispatch path is unverified; presumably dispatched through a different bridge method we haven't located.

### Full 5-stage name → audio-bytes resolution pipeline

| Stage | Function | Effect |
|---|---|---|
| 1 | `sub_8d3460` (static init) | Name string → uint16_t token, into hashmap at `data_48c56d0` (25 entries only) |
| 2 | `sub_b09660` | Name lookup in the hashmap; returns hash node with token at offset `+0x28` |
| 3 | **NONE — runtime only** | Token → asset_id: NOT in compiled code. Populated from scene bytecodes (type-0x0b records, see §6) by `load_scene_assets` at `0xb321d0` when each scene loads. Per-scene variants possible |
| 4 | `asset_registry_find` over `g_asset_registry` at `data_48ca4d0` | Encoded asset_id → `{file_offset, size, type}` |
| 5 | `get_asset_ptr_by_packed_id` → `image_mapped_base_plus_offset` | File offset → absolute pointer inside mmap'd `image.bin` |

#### Why stage 3 is runtime only

Per Sidekick (Step 42): "This separation allows content updates without binary recompilation — a regulatory compliance requirement for gaming machines." No `uint32_t token_to_asset[]` array exists in the binary; token constants never appear in asset-ID construction paths. The same `SE_PK_*` can map to different `asset_id`s across scenes.

To obtain the full name → asset_id mapping: either (a) runtime-capture under emulation by hooking `asset_registry_find` / `sub_b09660` after `load_scene_assets` has run for every scene, or (b) dump `g_asset_registry` from a running game on real hardware (CM4).

*Verified by:* JOURNAL Step 42 (architectural reasoning + static-search negative result).

### Encryption status

Spike 2 era: audio was encrypted via `NODEBUS_Hash` round-trips to the netbridge (per Pinball Browser team RE). **Pokémon LE 0.81: NOT encrypted.** All 2,452 section-7 records extract clean (manual audition + duration analysis confirms playable PCM). The hash polynomial is also absent in this build — see §5.2.

---

# then write a local scene_catalog.py with that game's SHA-1 catalog
```

(Adjust the `../` count to match nesting depth.)

When SW LE 0.85 is brought into the same toolchain: same drill. Symlink the three shared bits + write `<sw_le>/assets/lcd/scene_catalog.py`. Most of SW LE's bc0792d8 entries should carry over from spike_menu's catalog (shared Stern code).

#### Refactor candidate: shared Stern catalog

If/when SW LE's catalog also reuses the bc0792d8 descriptions verbatim (very likely), promote the shared block from `scene_catalog.py` files into `assets/lcd/_stern_shared_catalog.py` and have each game's per-game catalog `from _stern_shared_catalog import SHARED_BC0792D8` and merge it into its own `SCENE_INFO`. Skipped today (only two consumers, manageable copy-paste cost; refactor when third consumer arrives).

### Bridge to game code (out-of-scope reference)

The Radium scene tree integrates with Godot via `ManualGodotScene` (vtable at game VA `0x4630230`). Its constructor `sub_b25fc8 @ 0xb25fc8` is called from `load_scene_assets @ 0xb321d0` at `0xb32370` and `0xb32388`. **Per `feedback_godot_is_illegal.md` (2026-05-02), the ManualGodotScene internals — its extended ftable at `0x4630240..0x4630340`, the VideoPlayerSpi class layout, and the play() vtable — are out of scope for active investigation.** They are documented in JOURNAL Steps 36, 71-72 if needed for archeology, but should not be promoted into PROTOCOL.

---

## 10. Display pipeline (DRM / GBM / EGL / GLES)

**Data flow at a glance.** The game does its own real GLES rendering — no synthesized frames are fed to it. We intercept the hardware-specific framebuffer presentation parts (DRM/KMS ioctls + GBM allocation calls) and replace them with stand-ins, but every `glClear` / `glDraw*` / shader / texture upload the game issues is forwarded through to **real Mesa with the llvmpipe CPU rasterizer**, which writes real pixels into a real off-screen EGL pbuffer (returned by Mesa's `eglCreatePbufferSurface(1920×1080)` after we redirect the game's `eglCreateWindowSurface` call). At each fake page-flip the shim does `glReadPixels` on that pbuffer, getting bit-exact what the game just rendered, and routes the bytes to one or more sinks (frame bridge over abstract AF_UNIX, `/dev/fb0` blit for the qemu GTK window, periodic on-disk capture). GBM in this flow is a handle stand-in — its BOs are `calloc(256)` scratch pointers used as opaque tokens passed between layers; the only GBM BOs that actually carry pixel data are the special 1024×256 DMD path (`gbm_bo_map` returns `calloc(stride*height)` storage and `gbm_bo_unmap` captures it). The result: anything the game draws — including any rendering bug — shows up in the pbuffer and downstream, just executed slower by software raster than by a real GPU.

The game presents frames via Linux's KMS/DRM stack with EGL on top of GBM. Three layers in the shim:

1. **`/dev/dri/card1`** — DRM ioctls intercepted before they'd reach the real device
2. **GBM library hooks** — every `gbm_*` call goes to the shim
3. **EGL + GLES library hooks** — two modes (stub / real-Mesa-overlay) controlled by `SPIKE3_REAL_GL`

All three layers cooperate so the game's normal display pipeline (allocate GBM BO → wrap as EGL image → bind as GL texture → render → page-flip) succeeds end-to-end without real GPU hardware.

### `/dev/dri/card1` socketpair

`/dev/dri/card1` is opened as a real `socketpair(AF_UNIX, SOCK_STREAM)` (game gets `sv[0]`, shim holds `sv[1]`). DRM ioctls are intercepted in `shim_ioctl.c` before they'd reach the socketpair (which would reject them):

| ioctl | Shim response |
|---|---|
| `DRM_IOCTL_VERSION` | Mesa-shaped version string |
| `DRM_IOCTL_GET_CAP` / `SET_CLIENT_CAP` | success |
| `DRM_IOCTL_MODE_GETRESOURCES` | 1 CRTC, 2 connectors (both reporting connected), 1 encoder, 1920×1080@60 |
| `DRM_IOCTL_MODE_GETCONNECTOR` / `GETENCODER` / `GETCRTC` / `SETCRTC` | matching modes |
| `DRM_IOCTL_MODE_CREATE_DUMB` / `ADDFB2` | new FB IDs |
| `DRM_IOCTL_MODE_PAGE_FLIP` (with `DRM_MODE_PAGE_FLIP_EVENT` flag 1) | writes a 32-byte `drm_event_vblank`-shaped event to `sv[1]` so the game's `select()` and `drmHandleEvent` advance. **Frame capture trigger** lives here (calls `shim_frame_capture_at_page_flip` every 2nd flip) |
| `DRM_IOCTL_MODE_MAP_DUMB` | `ENOTSUP` |
| `DRM_IOCTL_MODE_GEM_CLOSE` | success |

*Verified by:* Step 21/24 — game completes full DRM resource enumeration → `gbm_bo_map`/`gbm_bo_unmap` path executes → DMD frame produced.

### GBM hooks

| Function | Behavior |
|---|---|
| `gbm_create_device`, `gbm_surface_create` (+`_with_modifiers`), `gbm_bo_create` (+`_with_modifiers`), `gbm_bo_destroy`, `gbm_bo_get_{width,height,stride,format,handle,modifier,fd}`, `gbm_bo_map`, `gbm_bo_unmap`, `gbm_surface_lock_front_buffer`, `gbm_surface_release_buffer`, `gbm_surface_has_free_buffers`, `gbm_device_destroy`, `gbm_device_get_{fd,backend_name}`, `gbm_device_is_format_supported` | Returns `calloc(256)` scratch-backed pointers so the game can dereference fields without SEGV |

Each BO is registered in a side-table (32 slots, mutex-guarded) holding its `(width, height, format, stride=width*4)` and an `is_dmd` flag. `gbm_bo_get_*` consult the table so the DMD BO correctly reports 1024×256 ABGR8888 instead of the screen default. `gbm_bo_map` returns `calloc(stride*height)` backing on first map; `gbm_bo_unmap` dumps `is_dmd` BOs to `_emulation/dmd/frame_NNNNNN.bgra` (1024×256×4 = 1 MB per frame) and frees the backing.

### EGL hooks

| Function | Behavior |
|---|---|
| `eglGetDisplay` / `eglGetPlatformDisplay(EXT)` | Drops the game's GBM_KHR query; asks Mesa for `EGL_PLATFORM_SURFACELESS_MESA` |
| `eglInitialize`, `eglTerminate`, `eglBindAPI`, `eglQueryString` | success / Mesa-default strings |
| `eglChooseConfig` / `eglGetConfigs` / `eglGetConfigAttrib` | RGBA8 + `NATIVE_VISUAL_ID=0x34325258` (XR24); patches `EGL_SURFACE_TYPE: WINDOW_BIT → PBUFFER_BIT` (Mesa surfaceless has no window configs) |
| `eglCreateContext` / `eglDestroyContext`, `eglCreateWindowSurface` (redirected to `eglCreatePbufferSurface` 1920×1080), `eglCreatePbufferSurface`, `eglDestroySurface` | fake handles in stub mode; forwarded to Mesa in real-GL mode |
| `eglMakeCurrent` (with `draw=NULL` → fabricates fallback pbuffer from `shim_last_real_config`), `eglSwapBuffers`, `eglSwapInterval`, `eglQuerySurface`, `eglSurfaceAttrib` | normal |
| `eglGetError` | always `EGL_SUCCESS` |
| `eglGetProcAddress` | routes to shim-owned stubs for `eglGetPlatformDisplayEXT`, `eglCreateImageKHR` / `DestroyImageKHR`, `eglCreateSyncKHR` / `DestroySyncKHR` / `WaitSyncKHR` / `ClientWaitSyncKHR`, `glEGLImageTargetTexture2DOES`. Never forwards — would hand fake GBM to real Mesa |
| `eglCreateImageKHR` (shim-owned) | Never forwards to Mesa (would deref fake GBM); registers `{w, h, fourcc}` in side-table, returns sentinel |
| `glEGLImageTargetTexture2DOES` | Looks up the side-table + does real `glTexImage2D(0, RGBA8, w, h, ..., NULL)` to give the bound texture storage; tracks in `shim_frametex` for capture targeting |

### GLES 3 hooks (~70 entry points)

| Family | Functions | Behavior |
|---|---|---|
| Query | `glGetError`, `glGetString`, `glGetIntegerv`, `glGetShaderiv`, `glGetProgramiv` | Defaults; `compile/link status` always `GL_TRUE` so Radium pipeline doesn't bail |
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

### `SPIKE3_REAL_GL` modes

| Mode | Set via | Behavior |
|---|---|---|
| Stub (default off) | `REAL_GL=0 ./run_game.sh` | Every GLES/EGL/GBM call returns plausible defaults. Shaders "compile". Draws are no-op. Sufficient for protocol-level tests; produces no real pixels |
| Real-GL (default on, `REAL_GL=1`) | `./run_game.sh` (default) | `FWD_RET` / `FWD_VOID` macros tail-call `dlsym(RTLD_NEXT, ...)` into the Mesa overlay at `mesa_sw_overlay/pkg/usr/lib/aarch64-linux-gnu/`. Game's EGL/GLES calls now go to real Mesa → llvmpipe → a real framebuffer |

#### Real-GL translation glue

- `eglGetDisplay` / `PlatformDisplay[EXT]` → drops game's GBM_KHR query, asks Mesa for `EGL_PLATFORM_SURFACELESS_MESA`
- `eglChooseConfig` → patches `EGL_SURFACE_TYPE: WINDOW_BIT → PBUFFER_BIT`
- `eglCreateWindowSurface` → redirects to `eglCreatePbufferSurface(1920×1080)`
- `eglMakeCurrent(draw=NULL)` → fabricates fallback pbuffer from `shim_last_real_config`
- `eglGetConfigAttrib(EGL_NATIVE_VISUAL_ID)` → forces XR24 fourcc
- `glFramebufferTexture2D/Layer` → swaps `GL_TEXTURE_EXTERNAL_OES → GL_TEXTURE_2D`

#### dlopen/dlsym interception (in `spi_shim.c`)

Tracks `libEGL` / `libGLES` / `libgbm` handles. Substitutes the shim's 3 EGLImage overrides on `dlsym(handle, "eglCreateImageKHR" / "eglDestroyImageKHR" / "glEGLImageTargetTexture2DOES")`. `eglGetProcAddress` returns the shim's stubs (never forwards — would hand fake GBM to real Mesa).

### Capture pipeline

| Asset | Hook | Output |
|---|---|---|
| **DMD frames** (1024×256 BGRA) | `gbm_bo_map` detects 1024×256 ABGR8888 BO, returns `calloc(stride*height)`; capture in `gbm_bo_unmap` | `_emulation/dmd/frame_NNNNNN.bgra` (1 MB each) |
| **Radium shaders** (12 programs / 24 shader objects per init) | `glShaderSource` — concatenate `count × string × length` args; type recorded in `glCreateShader` | `_emulation/shaders/shader_NNN_idM_typeXXXX.glsl` |
| **Scene textures** (`StreamingImage` path, `glTexSubImage2D` at game VA `0xca769c`) | `glTexImage2D` / `glTexSubImage2D` — dump `data` arg sized via GLES bytes-per-pixel table | `_emulation/textures/tex_NNNNNN_WxH_fmtXXXX_typeYYYY.raw` |
| **Compressed textures** (DXT1/DXT5 — none observed in PLE 0.81) | `glCompressedTexImage2D` / `glCompressedTexSubImage2D` | `_emulation/textures/tex_compressed_NNNNNN_WxH_fmtXXXX.bin` |

### Frame bridge wire format

When `FRAME_BRIDGE=1`, every 2nd `MODE_PAGE_FLIP` triggers `glReadPixels(0, 0, 1920, 1080)` from `shim_last_draw_fbo` (or current `READ_FRAMEBUFFER`) and writes one frame to `@pokemon_spike3_frame` (AF_UNIX abstract namespace):

```
+0x00  uint32 LE  magic = 0xF00FBABA
+0x04  uint32 LE  seq number
+0x08  uint16 LE  width  (= 1920)
+0x0a  uint16 LE  height (= 1080)
+0x0c  uint16 LE  fourcc (RGBA8)
+0x0e  uint16 LE  flags
+0x10  W*H*4 bytes  RGBA payload
```

Subscriber: `frame_viewer.py` (Tk + numpy + Pillow). Self-resolves the abstract socket name. Shows live frames in a 1920×1080 window (or fullscreen with `VIEWER_FULLSCREEN=1`).

### Observed at runtime (Step 24/27 captures)

| Asset | Counts |
|---|---|
| Shader programs | 12 (24 shader objects: 12 vertex `type=0x8b31` + 12 fragment `type=0x8b30`). ~7.5 KB total source. First is a standard `#version 300 es` sprite vertex shader |
| RGBA texture uploads | 16 textures at 1024×{64,128,256,512} for SternLogo splash (1920×1080) + various UI surfaces |
| `GL_LUMINANCE_ALPHA` 256×256 atlases | 79 — Nuklear font glyph atlases |
| GBM BO sizes seen | 1024×256 (DMD), 1920×1080 (display) |
| Compressed textures | 0 (no `glCompressedTex*` calls observed) |

The DMD pipeline (`DMDEmulationScene`, ctor at BN VA `0xa8ca40` → `0x68ca40` runtime): allocates 1 MB BGRA buffer at object offset +0x30 → `create_gbm_bo_and_upload @ 0xcffe80` → `gbm_bo_map` writes pixel data row-by-row (with stride) → `gbm_bo_unmap` → `gbm_bo_get_fd` → DMA-BUF fd → `eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT)` → `glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES)`.

Shim's `gbm_bo_map` returns `calloc(stride*height)` backing; `gbm_bo_unmap` dumps to `_emulation/dmd/frame_NNNNNN.bgra` (1 MB raw BGRA). First capture confirmed: `frame_000000.bgra` is exactly 1,048,576 bytes (= 1024 × 256 × 4).

*Verified by:* Step 24 capture inventory; Step 25 DMDEmulationScene class identification.

### Game-binary VA reference points (current rebased BN values)

| Symbol | VA |
|---|---|
| `sub_d01880` page flip — frame commit | `0x901880` |
| `sub_bb76e0` DisplayRenderer render loop — main render thread, callback-driven | `0x7b76e0` |
| `sub_a8ca40` DMDEmulationScene constructor | `0x68ca40` |
| `sub_cffe80` create_gbm_bo_and_upload — DMA-BUF DMD upload | `0x8ffe80` |
| `sub_d00030` init_tex_rgba — EGL image creation from DMA-BUF fd | `0x900030` |
| `sub_ca7210` StreamingImage_Bind — non-DMD scene texture bind | `0x8a7210` |
| `sub_d001c0` Texture2dBase constructor | `0x9001c0` |

---

## 11. Game runtime model (switches, events, modes)

The game runtime is built around three coupled subsystems:
1. **Switch state array** — the in-memory mirror of every physical switch
2. **`fire_event` dispatcher** — fans out events to handler chains
3. **Cmode (mode/rule) registry** — 42 statically-registered game-mode objects with vtable-driven tick + event handlers

Switches feed into events feed into cmodes; cmodes drive scene/display updates back out via the proc system. The pieces below cover each.

### Switch state array

The game maintains an in-memory mirror of each physical switch's state. The array is populated by the UART cmd `0x27` receive path (netbridge → CPU, 1-byte status payload per poll cycle — see §4 Cmd 0x27); the specific decoder function that writes into this array is `sub_651610` (the "translator").

#### Corrected understanding (Step 72f, 2026-04-30)

`g_switch_state_array @ 0x4480220` is **an 8-byte POINTER**, not the array itself. The actual per-switch state structs live on the **heap** at `*(uint64_t*)0x4480220 + sw_id * 0x30`.

`sub_651610`'s disassembly at `0x65164c` is `ldr x14, [x20, #0x220]` — a LOAD, not an LEA. So writes to `0x4480220 + sw_id*0x30 + 0x28` (as in earlier shim attempts) hit the **BSS alias** adjacent to the pointer field, NOT the live data. To mutate live switch state correctly:

```c
uintptr_t arr = 0;
memcpy(&arr, (void*)(game_base + 0x4480220), 8);
uintptr_t sw_struct = arr + sw_id * 0x30;     // live heap struct
*(uint8_t*)(sw_struct + 0x28) = 1;            // close switch
```

The shim's `SHIM_SWITCH_STATE_BASE 0x4480220` macro is misleading and should be treated as the address of the pointer field, not the array base. Old `shim_fire_switch` writes that didn't deref the pointer DID NOT propagate.

#### Per-switch struct layout (0x30 bytes)

| Offset | Field | Notes |
|---|---|---|
| `+0x00..+0x07` | linked-list `next` (or similar pointer) | |
| `+0x08..+0x0f` | linked-list `prev` (or similar pointer) | |
| `+0x10` | `config_ptr` | pointer to a config struct whose `+0x30` byte encodes flags |
| `+0x10 → +0x30` | config flags | bit 2 (`0x04`) = invert, bits 5+6 (`0x60`) = broken/stuck masks |
| `+0x26` | u16 `edge_counter` | incremented per closure/release; `sub_651610` pushes only when old counter == 0 |
| `+0x28` | u8 `state` | current state byte — 1 closed, 0 open. Inverted before return if invert flag set |
| `+0x2a` | u8 `disable_flag` | bit 0 = "ignore this switch" |

#### Companion arrays (also pointers, parallel to the main one)

- `data_4880228` — pointer to switch-disable mask base. `*(*(uint64_t*)0x4480228 + sw_id) & 1` tells whether switch is disabled.
- `data_4880230[sw_num]` — `sw_stuck_active` flag (NVRAM-backed)
- `data_4880238[sw_num * 2]` — `sw_alert_count` (16-bit, NVRAM-backed)

#### Readers

| Function | Behavior |
|---|---|
| `sub_a50850(sw_num) @ 0xa50850` | Returns the state byte with the invert flag applied. Short-circuits to 0 on `sw_num == 0` or `sw_num >= data_48cbaa0` |
| `sub_a50350(sw_num) @ 0xa50350` | Returns 1 if the switch is "OK" (not broken, not stuck, alert count clear); 0 otherwise. Gates event firing |

`data_48cbaa0` holds the runtime-determined live switch count (PLE has ~105 manual-listed switch IDs but only installed+valid switches count).

*Verified by:* JOURNAL Step 52 + Step 72f correction.

### `fire_event` dispatcher

`sub_9c12a0 @ 0x9c12a0` (file offset `0x5c12a0` since BN base is rebased to 0).

Signature: `uint64_t fire_event(int32_t event_id, int64_t event_data)`

```c
uint64_t fire_event(int32_t event_id, int64_t event_data) {
    if (event_id > 0xce) {
        CC_criticalErrorHandler(0x6a);
        return 1;
    }
    for (int64_t* i = data_485ea80[event_id]; i; i = i[2]) {
        if (i[0]) {
            int32_t r = ((int32_t (*)(int64_t))i[0])(event_data);
            if (!(r & 1)) return r & 0xff;          // short-circuit on even return
        }
    }
    return 1;
}
```

#### Linked-list node layout at `data_485ea80 + event_id * 8`

| Offset | Field | Notes |
|---|---|---|
| `+0x00` | `callback_fn` | `int32_t (*)(int64_t event_data)` — handler function pointer |
| `+0x08` | `priority` | byte; insertion is descending-priority via `sub_9c11d0` |
| `+0x10` | `next` | `node*` — loop variable `i[2]` (LP64: stride 8, so `i[2]` = byte offset +0x10) |

Bounds: `event_id` 0..0xce (206 event types max). Exceeding triggers critical error code `0x6a`.

Return semantic: dispatcher returns `1` on full-chain completion. If any handler returns `!(r & 1)` (i.e. even value), dispatch short-circuits and returns `r & 0xff`. Handlers that want the chain to continue return an odd value (conventionally `1`).

#### Registered handlers (Step 51 runtime observation)

- 102 / 207 event slots populated, 236 total handlers
- Event `0xc5` has 2 handlers — one is `sub_950d60`, which iterates the 42-cmode registry at `data_4767b30` and calls vtable slot `+0xf0` on each (see "Cmode registry" below)

#### Trampoline pattern for the shim

```c
typedef uint64_t (*fire_event_t)(int32_t event_id, int64_t event_data);
uintptr_t game_base = shim_resolve_game_base();
fire_event_t fire_event = (fire_event_t)(game_base + 0x5c12a0);
uint64_t r = fire_event(event_id, event_data);
```

`event_data` is opaque to the dispatcher and handler-specific. Event `0xc5` ignores it (handler invokes cmode vtable with no args). Other events may require a valid pointer; calling with `event_data=0` for unknown events can SEGV in the handler.

*Verified by:* JOURNAL Step 52 dispatch decode + Step 51 runtime registry walk.

### Switch → event dispatch is two-stage

The actual design **decouples hardware input from event dispatch**:

#### Stage 1 — `sub_b24f70 @ 0xb24f70` — switch-state reader / display updater

Reads switch state via `sub_a50850(sw_num)` at call site `0xb25438`. Iterates over switch-id arrays at `data_47643a8` (offset tables that group switches by function). **Does not call `fire_event`**. Its output is "updated game/display state" — writes to cmode fields, lamp state, display elements.

#### Stage 2 — game logic observes state changes and fires events

Separate functions observe the state changes Stage 1 produces and call `fire_event` with specific event_ids + payloads. Each direct caller does one narrow job:

| Caller | VA | event_id | event_data | Purpose |
|---|---|---:|---|---|
| `sub_b129f0` | `0xb129f0` | `0xc2` | `0` | Mode state transitions |
| `sub_bd39f0` | `0xbd39f0` | `0x12` | `&uint16` | Event validator — checks against lookup tables at `0x4787fa8` and `0x4780724` before firing |
| `sub_b16f64` | `0xb16f64` | `0xc3` | `1..4` | Lamp timer events; sources data from `data_4807430` |
| `sub_b82990` | `0xb82990` | `0x06` | `&struct` | Award / score events; reads `data_47790f8`, range-checks `0 < arg1 < 0xf0` |
| `sub_b12760` | `0xb12760` | `0xc1` | `0` | Mode init completion — fires once at end of setup |

None of them reads the switch state array directly.

#### Implications for the shim

- **Direct write to the deref'd switch state byte is the correct lowest-level input** — same byte the UART cmd 0x27 receive path writes. Stage 1 (`sub_b24f70`) picks it up on its next poll iteration.
- **Calling `fire_event(id, 0)` blindly bypasses Stage 1 entirely** — sidesteps all the game logic that decides *which* event to fire based on state. That's why the Step 52 blind sweep had ~1% signal.
- **To test a specific game behaviour**, close the relevant switch via the (corrected-deref'd) state array and let the game's own Stage 2 decide what events to fire.

#### Known event_id semantics

| event_id | Meaning | Evidence |
|---:|---|---|
| `0x06` | Awards / score events | `sub_b82990` payload is a struct from `data_47790f8`; range check `0 < id < 0xf0` |
| `0x10` | Secondary init start | fired by `secondary_initialization` (boot phase 5) |
| `0x11` | Secondary init complete | fired by `secondary_initialization` |
| `0x12` | Validated events (filtered through lookup tables before firing) | `sub_bd39f0` validates against tables at `0x4787fa8` + `0x4780724` |
| `0x3e` | Battle-environment activation (empirical) | activated `cmode_battle_coast` during Step 52e sweep — `+0x308: 0 → 0x100000000` |
| `0x5c/0x5d/0x5e/0x5f` | Hardware-tier readiness events | fired by `primary_initialization_orchestrator` (boot phase 5) |
| `0xc1` | Mode init complete | `sub_b12760` fires once at end of setup |
| `0xc2` | Mode state transition | `sub_b129f0` fires unconditionally |
| `0xc3` | Lamp timer event (payload 1..4 selects lamp subgroup) | `sub_b16f64` cycles values 1-4 |
| `0xc5` | "Broadcast to all 42 cmodes" — calls vtable slot `+0xf0` on each | Step 51: `sub_950d60` is event-0xc5 handler |

All other event_ids remain semantically unknown.

### Cmode registry — 42 modes, statically registered

`_INIT_74 @ 0x8733c0` runs at C++ static init and constructs all 42 `crule::*` objects in `.bss`. Each constructor signature is:

```
sub_XXXXXX(bss_addr, mode_id, type_flags, scene_id, arg5 [, arg6])
```

#### Cmode struct layout (verified at runtime Step 50)

```c
struct crule_cmode_base {
    /* +0x00 */ vtable_ptr_t *vtable;
    /* +0x08 */ uint32_t mode_id;             // VERIFIED
    /* +0x0c */ uint32_t type_flags;          // VERIFIED — category bitmask
    /* +0x10 */ uint32_t arg5;                // VERIFIED — behaviour parameter, meaning TBD
    /* +0x14 */ uint32_t _pad_14;
    /* +0x18..0x57 */ zeros (after static init)
    /* +0x58 */ scene_data_t *scene_data_ptr; // VERIFIED — points to scene_data struct
    /* +0xb0 */ void *timer_obj;              // from arg6, meaning TBD
    /* +0x308 */ int32_t init_or_state;       // varies across cmodes post-init
};
```

Total instance size ≥ 0x308 bytes (776 bytes). Additional fields uniformly set by base: `+0x18 = 0x700`, `+0x19 = 0xa00000064`, `+0xb8 = 0x15300000152`, `+0x308 = 0`.

All 42/42 cmode .bss slots match the predicted (mode_id, type_flags, scene_id) values from `_INIT_74` static analysis.

#### `type_flags` decode (across all 42 modes)

| Bits set | Hex | Count | Modes | Interpretation |
|---|---|---:|---|---|
| 0 | `0x01` | 20 | mode_id 0x00-0x12, 0x1e, 0x1f | Simple/base mode (attract, base play, service, etc.) |
| 2 | `0x04` | 3 | mode_id 0x09, 0x26, 0x27 | Multiball-related |
| 3 | `0x08` | 3 | mode_id 0x24, 0x25, 0x1c | Utility mode with arg6 |
| 2+5 | `0x24` | 2 | 0x17, 0x18 | **Pikachu modes** |
| 3+5 | `0x28` | 6 | 0x13, 0x14, 0x15, 0x16, 0x19, 0x1a | **Pokémon story modes** (Bulba/Charm/Squirtle × 2) |
| 3+6 | `0x48` | 4 | 0x20, 0x21, 0x22, 0x23 | **Battle environment modes** (Mountain/Forest/Desert/Coast) |
| 2+7 | `0x84` | 4 | 0x28, 0x1b, 0x1d, 0x29 | **Multiball / wizard modes** (Team Rocket M1-3 + wizard) |

Bit 5 = "Pokémon" flag, bit 6 = "Battle environment" flag, bit 7 = "Wizard/multiball" flag. Bits 0-3 distinguish sub-variants.

#### Story modes table

| mode_id | scene_id | bss addr | inferred class |
|---:|---:|---|---|
| `0x13` | `0x18` | `0x4858480` | `cmode_story_bulbasaur_1` |
| `0x14` | `0x19` | `0x48585e0` | `cmode_story_bulbasaur_2` |
| **`0x15`** | **`0x1a`** | **`0x48586b8`** | **`cmode_story_charmander_1`** ✓ confirmed |
| `0x16` | `0x1b` | `0x48587f8` | `cmode_story_charmander_2` |
| `0x19` | `0x1c` | `0x4858900` | `cmode_story_squirtle_1` |
| `0x1a` | `0x1d` | `0x4858a50` | `cmode_story_squirtle_2` |

`cmode_story_charmander_1` is the verified anchor (constructor `sub_90d840` decompile confirmed all field offsets). The other 5 story modes use the same cmode pattern with different vtable pointers.

#### Battle environment modes (`type_flags = 0x48`)

| mode_id | scene_id | bss addr | inferred class |
|---:|---:|---|---|
| `0x20` | `0x14` | `0x4857aa0` | `cmode_battle_mountain` |
| `0x21` | `0x15` | `0x4857cd0` | `cmode_battle_forest` |
| `0x22` | `0x16` | `0x4857f00` | `cmode_battle_desert` |
| `0x23` | `0x17` | `0x4858130` | `cmode_battle_coast` |

#### Multiball / wizard modes (`type_flags = 0x84`)

| mode_id | scene_id | bss addr | inferred class |
|---:|---:|---|---|
| `0x1b` | `0x24` | `0x4859160` | `cmode_team_rocket_multiball_2` (maybe) |
| `0x1d` | `0x26` | `0x4859368` | `cmode_team_rocket_multiball_3` |
| `0x28` | `0x23` | `0x48590a0` | `cmode_team_rocket_multiball_1` |
| `0x29` | `0x27` | `0x4859438` | `cmode_team_rocket_wizard` |

#### Multi-variant per-player mode (`mode_id = 0x2b`, 5 variants)

Five separate BSS buffers for the same mode_id with different `type_flags` (`0x01/0x00/0x04/0x08/0x10`) — most plausible interpretation: **per-player instances** (up to 5 players, each with their own state). The bitmask matches "player 1/2/3/4/5" convention.

Per-player BSS addresses: `0x4856730`, `0x4856798`, `0x48569d8`, `0x4856848`, `0x4856908`.

#### Scenes shared across modes

Modes that share `scene_id 0x29` (the universal attract / fallback scene): `0x00, 0x03, 0x0c, 0x0d, 0x25` plus the 5 `mode_id=0x2b` variants.

The full 42-row table (including the 20 simple base modes, the 3 multiball-family modes, the 3 utility modes with arg6, and the 1 special multi-arg mode) was extracted from `_INIT_74` Step 50. To regenerate: BN-decompile `_INIT_74 @ 0x8733c0` and walk the constructor calls.

### Cmode registry array — `data_4767b30`

42 pointers to cmode objects (336 bytes / 0x150). The game's central cmode directory; every game-loop tick iterates it.

#### Two confirmed dispatchers

**`sub_950d60` — event handler for event 0xc5:**

```c
int64_t sub_950d60(int64_t event_data) {
    sub_950cd0();
    if (*(data_4856a90 + 0x260) != sub_94e940)
        return (*(data_4856a90 + 0x260))(&data_4856a90, event_data);
    for (cmode **i = &data_4767b30; i != &data_4767c80; i++) {
        cmode *m = *i;
        (*(m->vtable + 0xf0))();   // call cmode's virtual method at slot +0xf0
    }
    return 1;
}
```

Registered via `sub_9c11d0(0x48594f0, 0xc5, sub_950d60, 0x80)` — event 0xc5, priority 0x80.

**`sub_94f140` — update callback for update_type 1:**

```c
int64_t *sub_94f140(void) {
    cmode **i = &data_4767b30;
    while (i != &data_4767c80) {
        cmode *m = *i; i++;
        int64_t (*tick)() = (int64_t(*)())*(m->vtable + 0xd8);
        if (tick != sub_8e90e0) tick();   // skip null-impl cmodes
    }
}
```

Registered via `sub_adfcc0(0x48594f8, 1, sub_94f140, 0x3ff, 0, 0x80)` — update_type 1, max 1023 iterations, priority 0x80.

#### Cmode vtable method slots (partial map)

| vtable offset | BN slot index | Purpose |
|---|---:|---|
| `+0x00` | 0 | destructor 1 |
| `+0x08` | 1 | destructor 2 |
| `+0x10` | 2 | uncategorized (sub_8afe70-range in Charmander1Start) |
| `+0xd8` | 27 | **tick / per-frame update** (called by `sub_94f140`) |
| `+0xf0` | 30 | **event handler** (called by `sub_950d60` for event 0xc5) |

Other slots TBD. The remaining ~35 slots in the BDLMode vtable are likely mode-lifecycle events (enter, exit, ball-drain, shot-detect, etc.).

### Event/update dispatch registries

- **Event registry** at `data_485ea80` — 206 event slots (indexed 0..0xce per `sub_9c11d0`'s bounds check). Each slot is the head of a priority-sorted linked list of `{callback_fn, priority, next_ptr}` triples. `sub_9c11d0` inserts by descending priority.
- **Update registry** at `data_48c52c0` — 5 update-type slots (indexed 0-4). Each slot is a linked list of `{UpdateObject*, callback_fn, max_count, priority}`.

## 12. Hardware shim coverage reference

The shim (`spi_shim.so`) intercepts every device the game expects and emulates it in software. The bulk of FS-level path remapping has been moved out of the shim entirely (see PROTOCOL §2 "One-time setup: bind-mount the sysroot") — the kernel resolves `/games`, `/connectivity`, `/data` via the mount table, and the shim now focuses on hardware emulation.

### Per-device coverage

| Device path | fd type | Purpose | Coverage |
|---|---|---|---|
| `/dev/spidev4.0` | fake fd `1023` | CPU-board 8-byte SPI polling (local switches, not node bus) | `MESSAGE_1` ioctl: default `rx[2] = 0x04` advances state machine. Per-cmd override via `SPIKE3_SPI_RX_<HEX>=hexbytes` env. TX currently all-zeros (gated on bridge) |
| `/dev/i2c-10` | fake fd `1022` | EEPROM at slave 0x57 | Read/write-back works; no real state maintained. **Persistence gap: writes vanish, no SIGTERM flush** |
| `/dev/ttyAMA5` | real `socketpair(AF_UNIX, SOCK_STREAM)` | UART to netbridge (game gets sv[0], shim holds sv[1]) | Reads block naturally on the socketpair. Writes are intercepted by `shim_io.c::write` and routed to `synthesize_bridge_response` (see §4) |
| `/dev/ttyAMA2` | real `socketpair(AF_UNIX, SOCK_STREAM)` | secondary UART (purpose unverified) | Pass-through |
| `/dev/dri/card1` | real `socketpair(AF_UNIX, SOCK_STREAM)` | DRM KMS display | DRM ioctls intercepted before they'd reach the socketpair. See §10 for the full DRM/GBM/EGL/GLES coverage |
| `/dev/shim-v4l2-*` | fake fds `FAKE_FD_V4L2_MEDIA=1021` / `FAKE_FD_V4L2_VIDEO=1020` | V4L2 video device probes (Step 46) | All ioctls return `ENOTTY` |
| `/dev/boot_display` | fake fd `FAKE_FD_BOOT_DISPLAY=1019` | boot_display ioctl surface | ioctls return 0; writes swallowed |
| `/dev/snd/*` | (full ALSA stub via libasound symbols) | audio output | See §8 — 18 symbols stubbed; `snd_pcm_writei` paces, captures, optionally tees to bridge |
| GPIO sysfs (`/sys/class/gpio/*/value`, `direction`, etc.) | fake fd pool `1000..1015` | direct GPIO access (e.g. for libgpiod) | Synthetic reads: `"0\n"` value, `"out\n"` direction. One-shot latch reset by `lseek(0)` |
| libgpiod chardev (`/dev/gpiochip*`) | (Stub) | newer GPIO interface | output-only; reads not modeled |

### Runtime binary patches (`shim_patches.c`)

| Patch | Gate | Target |
|---|---|---|
| memcpy NULL-guard | always on | silently no-op when src/dst NULL — fixes Godot DisplayRenderer thread crash on un-initialized TLS |
| `avcodec_find_decoder_by_name` remap | `SPIKE3_SW_HEVC=1` | `"hevc_v4l2request"` / `"hevc_v4l2m2m"` → `"hevc"` so libavcodec's software HEVC decoder is selected when the game asks for the V4L2 hwaccel variant |
| Dummy NV12 frame injection | `SPIKE3_INJECT_VIDEO=1` | when `avcodec_receive_frame` returns EAGAIN with pending packets, ref a 1920×1080 black frame with monotonic PTS so the game's render path always gets *something* |

In addition to the gated patches above, `shim_patches.c` also installs unconditional **passthrough hooks** for `avcodec_get_hw_config`, `av_hwdevice_ctx_create`, `avcodec_send_packet`, `avcodec_receive_frame` — these resolve the real symbols via `dlvsym`, log call activity, and forward to real libavcodec.

### Other hooks (in `spi_shim.c` + `shim_net.c`)

| Hook | Behavior |
|---|---|
| `system()` | Blocks `openocd` and `/games/spike3/` shell-outs (returns 0 so the game thinks RP2040 flashing succeeded); rewrites `/connectivity/` paths in commands to `$SYSROOT/connectivity/` |
| `pthread_create` | Forces `PTHREAD_INHERIT_SCHED` when caller passed `EXPLICIT_SCHED` (qemu-user lacks `CAP_SYS_NICE`; otherwise glibc would silently drop the thread → `data_4882108` never set → orchestrator loops forever). See §2 "Common gotchas" |
| termios family | UART fd gets fake-OK because glibc INLINE_SYSCALLs bypass our ioctl hook |
| ALSA | Full set of fake-success stubs; `snd_pcm_writei` captures + paces. See §8 |
| GStreamer | passthrough+log on `init_check`/`factory_make`/`parse_launch`/`bin_add`/`element_link`/`set_state`/`deinit`. **Linked but unused in PLE 0.81** — no static callers in the game binary; gstreamer init never fires. The actual video path is 100% direct libavcodec via Stern's `spi_video_player` (`SpiVideoStreamDecoderBase` class) plus Radium's own `VideoStreamDecoder` |
| `connect` / `getaddrinfo` (`shim_net.c`) | log-only passthrough gated by `SPIKE3_OFFLINE=1` |

### Hardware reference: Stern Spike 3 nodes

Per Stern's "SPIKE System and Node Guide" (Fall of the Empire manual §2). Platform-generic; Pokémon LE uses the same node architecture.

#### Five node types

| Type | Role |
|---|---|
| CPU node (addr 0) | Primary controller, runs game software, provides node bus power to other nodes. **The board this emulator emulates** |
| Cabinet 48V node (addr 1) | Coin door, tilt, bottom-cabinet I/O |
| 48V playfield node | Coils, flashers, high/low-power outputs + switch inputs; powered by 48V rail |
| Light/switch node | 32 switch inputs + 32 LED outputs per board; bus-powered |
| Node extensions | Sub-nodes that add power I/O to a parent node via simple serial sub-bus |

#### Address map (4-bit, 0-15)

- Addresses 0-7 reserved for fixed-function nodes (no DIPs, not configurable)
- Addresses 8-15 configurable via 3- or 4-position DIP switches per node
- Addr 0 = CPU, addr 1 = cabinet — hard-assigned

#### Bus electrical

- RJ45 8-pin modular jacks, off-the-shelf Cat5e UTP or better
- **NOT electrically Ethernet** — no TCP/IP, proprietary serial framing
- Plugging a SPIKE node into a real Ethernet port may damage hardware
- 48V DC main supply bus; 48V nodes drive high-power outputs; standard I/O nodes pull power from the node bus, supplied by CPU

#### Node firmware files (this build)

Hex files in `games/pokemon_le/*.hex` follow `{function}node-{MCU_variant}-{version}.hex` (general-purpose nodes drop the `node` infix). All shipped at version `1_31_0`.

| Function prefix | MCU variants shipped |
|---|---|
| `coil4` | LPC1112_101, LPC1112_201, LPC1313 |
| `coil4_led` | LPC1313 |
| `pin` | LPC1112_101, LPC1112_201, LPC1313 |
| `node4` (general-purpose) | LPC812, LPC1124_303 |
| `ws2812` | LPC1313 |
| `ws2812pin` | LPC1313 |
| `hdmi` | LPC1313 |
| `hdmi_ws2812` | LPC1313 |
| `lcd` | LPC1113_302 |
| `magsensor` | LPC812 |
| `tmc2590` | LPC1313 |
| `tmc5041` | LPC1313 |

Power-distribution firmware is separate at `spike3/powerdist/powerdist_ext.hex` (programmed via the powerdist node, not the main RS-485 bus). These hex files are flashed onto remote node MCUs by `mcp_nodebus_flash_node_firmware @ 0x8f5484` over the encrypted node-bus channel (see §5.1) during commissioning. Not emulated by the shim.

*Verified by:* `ls games/pokemon_le/*.hex` — 17 files matching the table above.

### CPU board (520-1007-42)

Raspberry Pi CM4 carrier on a 509-1007-42 board. Address: Node 0, fixed. Lives in the backbox. Display outputs the CM4 exposes:

| Output | Connector | Role | Notes |
|---|---|---|---|
| HDMI 0 | micro-HDMI on CM4 | AUX display | optional second-screen |
| HDMI 1 | micro-HDMI on CM4 | MIRROR display | primary backbox display |
| LCD via CN6 | 34-pin 2.00 mm header | Playfield LCD | jumper-configured resolution |
| CN14 | — | LCD power | dedicated power rail |

CN6 LCD resolution jumpers (JP1):

| Jumpers | Resolution | Panel |
|---|---|---|
| None | 1920 × 1080 | Spike 3 18" LCD |
| JMP0 | 1360 × 768 | Spike 2 15.6" LCD |
| JMP1 | 1024 × 600 | Spike 3 10" Home Edition |
| JMP0 + JMP1 | 800 × 480 | Spike 2 7.6" Home Edition |

This **directly matches** the Radium FrameSize distribution (§9): 1920×1080 (12 scenes, HDMI 1), 1360×768 (22 scenes, CN6 default for PLE), 800×480 (1 scene, boot-splash), 454×256 (1 scene, speaker panel — separate from CN6).

The shim's current `/dev/dri/card1` reports 1 CRTC + 2 connectors both connected at 1920×1080. That covers the primary backbox; HDMI 0 (AUX), CN6 (playfield LCD), and DMD (1024×256 / 454×256) are handled via separate paths (GBM/EGL for DMD, GL pipeline for everything else).

### Power Distribution Board (520-1092-00)

Smart power-distribution node with its own MCU. Has boot mode / runtime mode like other Spike nodes — flashable via the node bus.

What pwrdist reports back to the game (via node bus):
- Voltage rail status: main 48V, 48V PF (playfield), 12V (topper), 3.3V (logic), VS (bulk caps charged)
- Door interlock switch state — safety cutoff: 48V PF shuts off when door open
- Start button state (CN9-1)
- Action button state (CN9-2)

Status LED (D2) blink codes (real-hardware debug only):
- Boot mode: 4 Hz fast blink, 75% duty
- Runtime mode: 1 Hz slow blink, 75% duty
- Error: pattern of blinks with long pauses

**Emulation:** the shim returns synthetic pwrdist responses for cmds 0x10/0x15/0x16/0x18/0x1c/0x1d (see §4). Sending all-zeros for status bytes has worked through init. If the game starts gating later features on specific rail/door state, we may need to send "all rails OK, door closed" bits.

## 13. Comparison binaries (SW LE 0.85)

The emulator's correctness target is "boot ANY Spike 3 game to attract." Every claim about Stern-shared code (proc system, IScene base class, DisplayRenderer, SceneCache, RadiumScene, node-bus, SPI, audio) should reproduce in **Star Wars LE 0.85.0** because both games run the same Stern stack. SW LE has **zero Godot** in its binary (vs Pokémon LE's 457 "godot" string hits) — making it the cleanest A/B target for "what is Pokémon-port glue vs. shared Stern?"

### Cross-mapped functions (PLE ↔ SW LE)

Confirmed shared Stern code with byte-shape-identical structure:

| Concept | PLE 0.81 VA | SW LE 0.85 VA / name | Notes |
|---|---|---|---|
| Init renderer + scene cache | `0x5d7aa0` | `0x309af0` (`mcp_init_renderer_and_scene_cache_singletons`) | Canonical 5-step boot sequence: `new DisplayRenderer(0x1b8) → ctor → mutex+deferred-list → wait_on_render_thread_ready → new SceneCache(0x1d0)("./assets") → post-init` |
| DisplayRenderer render loop | `0x7b7ac0` | `mcp_DisplayRenderer_render_thread_body` | Byte-shape-identical at the OpenGL call level. Slot-9 (activate) dispatch + slot-8 (tick) dispatch over IScene linked lists |
| RadiumScene base ctor | TBD | `mcp_RadiumScene_base_ctor (sub_41e0c0)` | BN-confirmed via auto-named vtable `&_vtable_for_RadiumScene{for 'IScene'}` |
| RadiumScene typeinfo | TBD | `0x749d98` (name string `11RadiumScene` at `0x647bf0`) | base typeinfo at `0x735d08` = IScene typeinfo (= `_typeinfo_for_IScene`) |
| RadiumScene vtable | TBD | `0x733ea0` (slot 8 tick `0x41d100`, slot 9 activate `0x420070`, slots 4-6 at `0x35f670/0x35f680/0x35f690`) | flag-accessor defaults match PLE's IScene flag-accessor defaults at consecutive 0x10 stride — high evidence of shared Stern IScene base |
| SceneCache::initialize | TBD | `mcp_SceneCache_initialize (sub_4c2140)` | scans `/lcd/demand_loaded`, `/nuk/images`, `/lcd/auto_loaded`, builds RadiumScene per `.radium`/`.json`/image file, registers in hashmaps at `arg1[0xa]` and `arg1[0x11]`, spawns thread named `"LoadSceneCache"` |
| SceneCache load thread body | TBD | `mcp_SceneCache_load_thread_body (sub_4c2ea4)` | the `LoadSceneCache` thread; drains queue at SceneCache+0x1b0, dynamic_casts each entry to RadiumScene, calls slot 12 |
| SceneCache add scene to display | TBD | `mcp_SceneCache_add_scene_to_display_unsure (sub_4c17e0)` | type-dispatches by IScene subclass (RadiumScene → arg1+0x50 hashmap; NuklearScene → arg1+0x110) |
| Process dispatch loop | `0x7292c4` | `mcp_process_dispatch_loop (sub_5faadc)` (in spike_menu) | orchestrator thread body |
| Per-tick orchestration hook | `0x720cf0` (`mcp_proc_pre_dispatch_tick`) | TBD | calls `process_deferred_scene_queue`, `drain_pending_switch_events`, plus 12 other unstudied subsystem ticks |
| `select_and_load_scene` | `0x730fb0` | TBD | fires events `0xAB` / `0xAD` / `0xAF` on scene transitions |

### Subclasses of IScene (shared abstract base)

| Subclass | Where | Notes |
|---|---|---|
| `RadiumScene` | both PLE + SW LE | The shared Stern UI subclass — handles all attract/HUD/menu Radium scenes |
| `NuklearScene` | both | Nuklear immediate-mode UI for system/test menus |
| `DMDEmulationScene` | both | The 1024×256 DMD path (or per-CPU-board variant) |
| `GodotRenderer` | **PLE only** | Pokémon-port-specific. Out of scope per `feedback_godot_is_illegal.md`. PLE's IScene vtable at `0x42a4d10` has 11 slots; GodotRenderer fills the 4 pure virtuals + the 4 default-overridden flag accessors |

PLE has 4 IScene subclasses (typeinfo data refs). SW LE has 4 (typeinfo data refs at `0x748240/0x748250/0x749d98/0x74cec8`) — same count.

### Why this matters

When a hypothesis or fix involves something on the off-limits Godot list (VideoPlayerSpi `is_inside_tree`, ManualGodotScene, `/tmp/godot_logs`, `./assets/godot/main.pck`, etc.), pivot to SW LE 0.85 as the validation target. The acid test for any shared-Stern fix is: can it boot SW LE attract?

If a shim change improves PLE behavior but breaks SW LE, the change is touching Pokémon-port-specific code path and should be reconsidered. If both improve together, the change is in shared Stern territory and is safe.

The DisplayRenderer / SceneCache / RadiumScene / proc-system / node-bus / SPI / audio code is all shared. Anything Godot-specific (PCK loading, ClassDB registration, `_process(delta)` ticks, scene-tree management, libgodot internals) is PLE-only and out of active investigation scope.

*Verified by:* JOURNAL Step 73 (2026-05-01) onwards — IScene vtable decompile per slot, SW LE function shape matches.

---

## 14. System-mode debugging (HW watchpoints)

### Visible display via /dev/fb0

qemu raspi4b's GTK window mirrors the kernel's `/dev/fb0` framebuffer (`bcm2708_fb` driver). The shim's frame-capture path hooks into the `MODE_PAGE_FLIP` ioctl handler — every second flip, it does `glReadPixels` on the current draw FBO and (when `SPIKE3_FB0_OUTPUT=1`) blits the RGBA bytes directly into mmap'd `/dev/fb0`. Result: the qemu GTK window shows whatever the game just rendered, with no host-side subscriber, no cross-VM transport, no networking-stack workarounds. Same architecture as the user-mode `frame_viewer.py` flow but with `/dev/fb0` instead of an AF_UNIX abstract socket.

Implementation lives in `shim_bridges.c::shim_fb0_blit` (lazy `open(/dev/fb0)` + `FBIOGET_VSCREENINFO` on first frame, mmap, then per-frame format-convert + nearest-neighbor scale into the mapped framebuffer). It runs alongside the existing AF_UNIX/TCP frame-bridge path — both are no-ops if their respective consumers aren't enabled.

**Force fb0 to game resolution.** qemu raspi4b's default `bcm2708_fb` size is 800×480, smaller than the game's 1920×1080. The kernel's `bcm2708_fb` driver honors the `bcm2708_fb.fbwidth=` / `bcm2708_fb.fbheight=` cmdline params, so `RUN_QEMU.sh`'s `-append` carries `bcm2708_fb.fbwidth=1920 bcm2708_fb.fbheight=1080`. With those, fb0 registers at native game resolution and no scaling is needed in the blit fast path.

**Avoid the WSLg fullscreen crash.** GTK on WSLg's Wayland backend crashes qemu with `Gdk-Message: Error 71 (Protocol error) dispatching to Wayland display.` on certain operations (notably fullscreen toggles). `RUN_QEMU.sh` exports `GDK_BACKEND=x11` before launching qemu — WSLg also exposes an X11 socket, GTK is happy on X11, fullscreen works.

### Networking is not available in this environment

For completeness: TCP-based bridges are not available in the system-mode rig. qemu raspi4b's machine code disables `brcm,bcm2711-pcie`, `brcm,bcm2711-genet-v5`, and `brcm,bcm2711-thermal` in the DTB before the kernel parses it (verified via the `bcm2711 dtc: %s has been disabled!` warnings + the `compat_disable[]` array in the qemu binary). `-device usb-net` attaches to qemu's emulated DWC2 controller correctly (verified via `info qtree` from the monitor) but no kernel driver enumerates the resulting USB device — the rootfs's built-in `dwc_otg` driver expects BCM2835 register layout, mainline `dwc2.ko` won't auto-bind to the DTB's `brcm,bcm2708-usb` compat string, and patching the DTB to `brcm,bcm2835-usb` (which dwc2 would match) still doesn't get past probe in this combo. The `SPIKE3_FB0_OUTPUT` path sidesteps the entire problem — system-mode renders the game's frames straight to qemu's GTK window.

# 1. Boot. --ephemeral discards SD writes; --debug backgrounds qemu; --gdb [PORT] enables gdbstub.
# DO NOT pass --headless: per feedback_system_mode_parity.md the GTK window must be up so
# /dev/fb0 visible-display + general parity with user-mode are preserved.
./RUN_QEMU.sh --debug --ephemeral --gdb 1234

# 2. Watch the boot until the game launches. The wrap_game.sh prints
#    "GAME_PIE_MAP: <base>-<end> r-xp ... /games/game" lines once exec /games/game completes.
tail -f last_run.log
# (Ctrl+C tail when you see GAME_PIE_MAP for /games/game)

# 3. Compute runtime addresses. Default PIE base with ASLR off in this kernel
#    is 0x5555550000 (verified 2026-05-05). For any BN VA, runtime = 0x5555550000 + BN_VA.
python3 -c 'B=0x5555550000; print(hex(B + 0x43989c8))'   # example: a table-entry address

# 4. Attach gdb and set the read watchpoint.
gdb-multiarch -ex 'set arch aarch64' \
              -ex 'target remote :1234' \
              -ex 'rwatch *(unsigned long long*)0x55598e89c8' \
              -ex 'commands
                 silent
                 printf "\n=== HIT pc=%p (BN 0x%llx)  lr=%p (BN 0x%llx) ===\n", \
                   $pc, (unsigned long long)$pc - 0x5555550000, \
                   $lr, (unsigned long long)$lr - 0x5555550000
                 printf "  x0=%p x1=%p x19=%p x20=%p\n", $x0, $x1, $x19, $x20
                 bt 30
                 c
                 end' \
              -ex 'continue'

# 5. Each hit prints PC and LR translated to BN VAs. Look up those addresses in BN
#    to identify the function reading the location.
```

### PIE base discovery — first time on a new build

The PIE base for the game.elf changes only when the kernel's mmap-base policy or ASLR setting changes. With `randomize_va_space=0` (set by initramfs/init), in this Stern kernel build, the base has been **0x5555550000** consistently. To re-verify on a fresh build:

```bash
# After boot, tail last_run.log for the GAME_PIE_MAP line printed by wrap_game.sh:
grep -E 'GAME_PIE_MAP.*r-xp 00000000.*/games/game' last_run.log | head -1
# Expected format:
# GAME_PIE_MAP: 5555550000-55559af000 r-xp 00000000 fe:02 1835258  /games/pokemon_le/game
# The first hex value (start of the r-xp 00000000 line for the game binary) IS the PIE base.
```

**Subshell-PID gotcha:** `wrap_game.sh` uses a backgrounded subshell to `cat /proc/$$/maps`. `$$` in POSIX `(…)` subshells refers to the parent shell's PID, which after `exec /games/game` becomes the game's PID. The subshell sleeps 100 ms first to let `exec` complete. If the sleep is too short, the subshell sees the pre-`exec` busybox/wrapper maps instead of the game's. If you ever see `GAME_PIE_MAP` showing `/bin/busybox` or `/bin/sh`, increase the sleep.

### BN VA ↔ runtime translation

The game's BN database is rebased to base 0x0, so `BN VA == ELF VA == file offset`. With ASLR off, `runtime_addr = 0x5555550000 + BN_VA` and `BN_VA = runtime_addr - 0x5555550000`. Always run the math through python (per `feedback_compute_via_python.md`):

```bash
python3 -c 'B=0x5555550000; va=0x43989c8; print(f"BN 0x{va:x} -> runtime 0x{B+va:x}")'
python3 -c 'B=0x5555550000; rt=0x55598e89c8; print(f"runtime 0x{rt:x} -> BN 0x{rt-B:x}")'
```

