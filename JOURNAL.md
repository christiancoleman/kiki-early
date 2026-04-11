# Spike 3 Emulation Journal

Step-by-step log of the emulation exploration for running the Pokemon Pinball `game` binary on a dev workstation via QEMU user-mode. Each entry documents what was tried, what was learned, and what it leads to next.

**Goal:** Run the real `game` binary under QEMU aarch64 user-mode, stub/emulate hardware dependencies, and eventually bridge I/O to Visual Pinball (VPX).

**Reference architecture** (from Researcher A's working emulator): QEMU user-mode + library hooking (gstreamer/ffmpeg/OpenGL forwarded to host) + file I/O interception (devfs emulation) + node bus emulation.

## Reading the journal

The entries present at this revision are retained, including historical numbering quirks. These are historical working notes:
hypotheses, confident conclusions and address conventions can be corrected by
later entries. Their wording is not a fresh validation of the implementation.
Original subsection numbers are retained even where they differ from the parent
step number.

Researchers A–E and Reference tools/projects are consistent aliases for external
people or resources named in the original notes. Service S and its associated
filenames, symbols and command options are aliases for an excluded service.
Aliases preserve the distinction between my investigation and external input;
they do not transfer authorship or establish permission to reuse external work.

Square-bracketed `[Redacted: ...]` notes describe omitted information at the point
where it occurred. `[REFERENCE_TOOL_A]` and similar paths are placeholders.
Commands, logs and code containing aliases or redactions are edited illustrations,
not runnable instructions. Historical references to files do not imply that those
files are included in this archive.

---

## Step 1: Identify the binary format

```bash
readelf -h games/pokemon_le/game
```

**Result:** ELF64, AArch64, Position-Independent Executable (PIE), GNU ABI. Entry point `0x49cb80`. This is a standard Linux aarch64 dynamic executable — QEMU user-mode is the right tool.

## Step 2: Enumerate shared library dependencies

```bash
readelf -d games/pokemon_le/game
```

**Result — 31 shared libraries required:**

| Category | Libraries |
|----------|-----------|
| **Graphics/Display** | `libEGL.so.1`, `libGLESv2.so.2`, `libdrm.so.2`, `libgbm.so.1`, `libfreetype.so.6` |
| **Media/Audio** | `libgstapp-1.0.so.0`, `libgstvideo-1.0.so.0`, `libgstbase-1.0.so.0`, `libgstallocators-1.0.so.0`, `libgstreamer-1.0.so.0`, `libavcodec.so.59`, `libavfilter.so.8`, `libavformat.so.59`, `libavutil.so.57`, `libasound.so.2` |
| **Hardware I/O** | `libgpiod.so.2`, `libusb-1.0.so.0` |
| **Networking** | `libcurl.so.4`, `libnl-3.so.200`, `libnl-genl-3.so.200` |
| **System/Core** | `libcrypto.so.3`, `libz.so.1`, `libgobject-2.0.so.0`, `libglib-2.0.so.0`, `libreadline.so.8`, `libuuid.so.1`, `libstdc++.so.6`, `libm.so.6`, `libc.so.6`, `ld-linux-aarch64.so.1`, `libgcc_s.so.1` |

**Key observations:**
- `libgpiod.so.2` — GPIO access library. This is likely how the game talks to the Spike 3 node bus (via GPIO lines on the CM4). **This will be one of the first things to stub.**
- `libEGL` + `libGLESv2` — OpenGL ES 2.0 rendering (not desktop GL). Will need host-side translation or stub.
- GStreamer + ffmpeg — media pipeline for video/audio playback. Researcher A forwards these to host.
- `libusb` — possibly for USB peripherals or node communication.
- `libasound` (ALSA) — direct audio output.
- `libdrm` + `libgbm` — DRM/KMS display output (not X11/Wayland). The game renders directly to framebuffer.
- `libreadline` — suggests the game has a debug/developer console!

## Step 3: Check device nodes in the filesystem

```bash
ls dev/
```

**Result:** Only `pts/` and `shm/` — the `/dev/` is nearly empty. On real hardware, `devtmpfs` populates this at boot. The game likely expects device nodes created by the kernel (e.g., `/dev/gpiochip*`, `/dev/spi*`, `/dev/i2c-*`, `/dev/dri/*`, `/dev/fb*`).

**Next step:** Check QEMU availability and try running the binary with strace to see exactly what files/devices it tries to open.

## Step 4: Verify QEMU is installed

```bash
which qemu-aarch64
```

**Result:** `/usr/bin/qemu-aarch64` — available and ready.

**Next step:** Verify the sysroot has the needed aarch64 shared libraries, then attempt first run with strace.

## Step 5: Sysroot decision — working directory vs raw SD card image

Considered mounting the raw SD card image (`/home/c/pokemon_pro-0_81_0.Release.64G.sdcard-secure.raw`, 58G, 6 partitions including LUKS-encrypted game data) as a more authentic sysroot. Checked whether the working directory has the needed libraries:

```bash
find /mnt/c/Workspace/pokemon-le-0.81.0/ -name "ld-linux-aarch64.so.1"  # Found: lib/
find /mnt/c/Workspace/pokemon-le-0.81.0/ -name "libgpiod.so.2"          # Found: usr/lib/
find /mnt/c/Workspace/pokemon-le-0.81.0/ -name "libEGL.so.1"            # Found: usr/lib/ and usr/lib64/
```

**Decision:** The working directory (`/mnt/c/Workspace/pokemon-le-0.81.0/`) is a complete backup of the decrypted filesystem — it has everything. Using it directly as the QEMU sysroot is simpler and equivalent. No need to mount the raw image.

## Step 6: Verify QEMU version

```bash
qemu-aarch64 --version
```

**Result:** `qemu-aarch64 version 10.0.8` — modern version, ready to go.

**Next step:** First run attempt — execute the game binary with strace under QEMU user-mode to see what it reaches for.

## Step 7: First QEMU run with strace

```bash
timeout 10 qemu-aarch64 -L /mnt/c/Workspace/pokemon-le-0.81.0 \
  -strace /mnt/c/Workspace/pokemon-le-0.81.0/games/pokemon_le/game \
  > /tmp/qemu_stdout.log 2> /tmp/qemu_strace.log
```

**Result:** Exit code 124 (killed by timeout after 10 seconds — the binary did NOT crash). Zero stdout output. 9,297 lines of strace output.

**Key findings:**

### 7a: All 31 shared libraries loaded successfully
The dynamic linker found every library via the sysroot. Libraries load from `/usr/lib64/` (falling back from `/lib64/` which doesn't have most of them). No missing dependencies.

### 7b: The node bus uses SPI, not I2C or GPIO
The game opens **`/dev/spidev4.0`** (SPI bus 4, chip select 0) for hardware node communication. The error message is:
```
cpuspi: device open failed: No such file or directory
```

This is the **first and only device** the game tries to open. It doesn't attempt GPIO, I2C, DRM, framebuffer, or any other device files before this point. The SPI device is the very first hardware interaction after library init.

### 7c: Two-thread architecture observed
1. **Main thread (43735)** — completes library loading, does minimal init (GnuTLS config check, getrandom, futex wakes), then spawns a child thread and enters a **1ms sleep loop** (`clock_nanosleep` with `tv_nsec=1000000`).
2. **SPI thread (43746)** — spawned via `clone()`, immediately opens `/dev/spidev4.0`. The open returns fd 0 (suspicious — likely mapped to stdin by QEMU since no real device exists). The game detects the failure and prints `cpuspi: device open failed`. Thread exits cleanly.

### 7d: SPI retry behavior
After the first SPI thread fails and exits, the main thread continues its 1ms sleep loop for ~8,817 iterations (~8.8 seconds), then spawns a **new thread (43747)** that tries `/dev/spidev4.0` again. This second attempt gets a clean `-1 errno=2 (ENOENT)`. The pattern repeats indefinitely — the game is designed to keep retrying SPI connection.

### 7e: No other device access attempted
Between library loading and the SPI open, the only non-library file access is:
- `/etc/gnutls/config` — not found (harmless, TLS library config)
- No `/dev/dri/*`, `/dev/fb*`, `/dev/gpiochip*`, `/dev/i2c-*` attempted before SPI

This suggests the game's init sequence is: **SPI first, everything else after**. The display, audio, and other subsystems likely initialize only after the node bus is up.

### 7f: `libgpiod` loaded but not used (yet)
Despite `libgpiod.so.2` being a linked dependency, no GPIO device files are opened. GPIO may be used later in the init sequence (after SPI succeeds) or may be used by the SPI layer internally on real hardware.

### 7g: Unknown syscall 435
Just before each `clone()` call, there's an `Unknown syscall 435`. This is `clone3()` — a newer Linux syscall that QEMU 10.0.8 apparently doesn't fully support in user mode. The game falls back to regular `clone()` successfully.

**Conclusion:** The single blocker right now is `/dev/spidev4.0`. If we can provide a fake SPI device that responds to the game's SPI protocol, the game should proceed past this point and reveal the next layer of hardware dependencies.

**Next step:** Investigate options for faking `/dev/spidev4.0` — either via an `LD_PRELOAD` shim that intercepts `open()`/`ioctl()`/`read()`/`write()` on that path, or by looking at what SPI ioctls the game would issue to understand the node bus protocol.

## Step 8: Binary Ninja RE of the SPI interface (sub_cfa814)

Manual reverse engineering of the game binary's SPI code in Binary Ninja. This gives us the full protocol specification needed to build the SPI shim.

### 8a: SPI device setup

The primary SPI thread function is at **`sub_cfa814` (0xcfa814)**:

1. Opens `/dev/spidev4.0` with `O_RDWR`
2. Configures via two ioctls:
   - `SPI_IOC_WR_MAX_SPEED_HZ` (0x40046b04) → **100,000 Hz** (100 kHz)
   - `SPI_IOC_WR_MODE` (0x40016b01) → **Mode 3** (CPOL=1, CPHA=1)
3. On failure: `perror("cpuspi: setting SPI_IOC_WR_MAX_SPEED_HZ or SPI_IOC_WR_MODE failed")`, closes fd, returns
4. On success: performs initial 8-byte transfer, then enters polling loop

### 8b: SPI transfer protocol

All transfers are **8-byte full-duplex** via `ioctl(fd, SPI_IOC_MESSAGE, &transfer_struct)` (ioctl code `0x40206b00`).

The `spi_ioc_transfer` structure:
```
offset 0x00: tx_buf pointer (8 bytes to send)
offset 0x08: rx_buf pointer (8 bytes received)
offset 0x10: len = 8
```

- **TX data source:** Global at `data_490da30`, updated via mutex-protected writes (mutex at `0x490d9f8`)
- **RX data destination:** Local 8-byte buffer, then processed

### 8c: GPIO integration into RX buffer

After each SPI transfer, the game calls `sub_d18670()` to read GPIO state (which internally calls `sub_d164a0` with command `0x15`). Three GPIO bits are injected into **byte 2 of the RX buffer**:

```
RX[2] bit 0 ← GPIO bit 0
RX[2] bit 1 ← GPIO bit 1
RX[2] bit 2 ← GPIO bit 2
```

This means the SPI response from the node bus is **augmented with local GPIO state** before being processed. The GPIO likely represents physical inputs on the CPU board itself (not node-bus switches).

### 8d: State machine (sub_cfa760)

The processed 8-byte SPI response (stored at `data_490d9b0`) feeds a state machine:

1. Extracts a state bit: `(response[2] >> 2) & 1`, combined with bits from `data_490d9b8`
2. Looks up action in **16-entry transition table** at `data_3bce960` (4 bytes per entry)
3. Table values: `0`, `1`, `2`, `-1` (0xFFFFFFFF)
4. Updates counter at `data_490d9f0` based on lookup
5. Special case: value `2` doubles `data_4772389` and adds to counter

This state machine likely tracks node bus communication state (sync, ready, error, etc.).

### 8e: Error handling behavior

| Failure | Action |
|---------|--------|
| `open()` fails | `perror("cpuspi: device open failed")`, return |
| ioctl config fails | `perror(...)`, close fd, return |
| Initial transfer fails | `perror("cpuspi: initial transfer failed")`, close fd, return |
| Continuous transfer fails | `perror("cpuspi: transfer failed")`, **continues loop** (no abort) |

The polling loop runs until `data_490da28` flag is cleared (shutdown signal).

### 8f: Key observations for emulation

1. **The protocol is simple** — 8-byte request/response, no complex framing or variable-length messages
2. **100 kHz SPI is slow** — this is a control bus, not a data bus. Confirms node bus is for switch states, coil commands, LED data — not streaming video/audio
3. **GPIO is separate from SPI** — three GPIO bits are read independently and mixed into the response. The shim needs to handle both SPI and GPIO
4. **The state machine is small** (16 entries) — suggests a simple handshake protocol, not a complex multi-phase negotiation
5. **No timing constraints** — no explicit delays between transfers, just continuous polling. The shim can respond instantly
6. **TX data comes from a shared global** — other parts of the game write commands to `data_490da30` (coil fires, LED updates, etc.) that get sent out over SPI

### 8g: Additional references

- `sub_d18670` (0xd18670) — GPIO read function, uses command `0x15` via `sub_d164a0`
- `sub_d15aa8` — Separate UART bridge (not SPI), likely for a different communication channel
- String `iop.spidevx.com` at 0x3a17a38 — no code references, possibly config/metadata

**Next step:** Build an `LD_PRELOAD` shim that intercepts `open("/dev/spidev4.0")`, returns a fake fd, handles the two config ioctls, and responds to SPI_IOC_MESSAGE transfers with valid-looking 8-byte responses. The initial goal is just to get past the SPI init so the game reveals its next set of hardware dependencies. We can also investigate the GPIO function (`sub_d18670` → `sub_d164a0`) to understand what device it opens for GPIO access.

## Step 9: Build and test SPI LD_PRELOAD shim

### 9a: Cross-compiler setup

Installed `gcc-aarch64-linux-gnu` (v14.2.0) to cross-compile the shim as an aarch64 shared library.

### 9b: Shim implementation (`_emulation/spi_shim.c`)

Built an `LD_PRELOAD` shim that intercepts:
- `open()` and `openat()` for `/dev/spidev4.0` → returns fake fd 1023
- `ioctl()` on the fake fd → accepts SPI config and handles `SPI_IOC_MESSAGE` transfers
- `close()` on the fake fd → no-op

For `SPI_IOC_MESSAGE` transfers, the shim logs TX data and returns all-zero RX data. Logs the first 20 transfers and then every 1000th.

```bash
aarch64-linux-gnu-gcc -shared -fPIC -o spi_shim.so spi_shim.c -ldl
```

### 9c: First shim test run

```bash
timeout 10 qemu-aarch64 -L /mnt/c/Workspace/pokemon-le-0.81.0 \
  -E LD_PRELOAD=/.../spi_shim.so \
  /mnt/c/Workspace/pokemon-le-0.81.0/games/pokemon_le/game
```

**Result — the shim works!**

```
[spi_shim] open("/dev/spidev4.0", 0x2) -> returning fake fd 1023
[spi_shim] ioctl SPI_IOC_WR_MAX_SPEED_HZ = 100000 Hz
[spi_shim] ioctl SPI_IOC_WR_MODE = 3
[spi_shim] transfer #1, len=8 TX: 00 00 00 00 00 00 00 00
...
[spi_shim] transfer #16424000, len=8 TX: 00 00 00 00 00 00 00 00
```

- **No "cpuspi" error** — SPI open/config succeeded
- **16.4 million transfers in 10 seconds** — SPI thread polling at full speed with no delay
- **TX is all zeros** — the game is sending empty commands because it's getting all-zero responses back. It's stuck waiting for the node bus to "wake up"
- Game didn't crash, didn't exit — it's alive but not advancing past the SPI polling phase

### 9d: Strace with shim reveals new hardware dependencies

Ran again with QEMU `-strace` to see what else the game opens now that SPI works:

```bash
timeout 5 qemu-aarch64 -L ... -E LD_PRELOAD=... -strace .../game
```

**New files opened (init sequence order):**

| File | Flags | Result | Purpose |
|------|-------|--------|---------|
| `/usr/local/spike/VERSION.txt` | `O_RDONLY` | likely failed (not in sysroot) | Spike platform version check |
| `/dev/i2c-10` | `O_RDWR` | likely failed (no device) | I2C bus — another hardware interface |
| `image.bin` | `O_RDONLY` | **fd 3 (success!)** | Asset loading — the game found it! |
| `/dev/ttyAMA5` | `O_RDWR\|O_NOCTTY` | likely failed (no device) | UART serial port — the UART bridge (`sub_d15aa8`) |

**Key observations:**
- The game **progressed significantly** past the SPI init even with all-zero responses — it doesn't block on SPI handshake to continue other init
- **`image.bin` loaded successfully** — the game is running from the `_emulation/` directory (cwd), and it found the asset file via relative path. We'd need to run from the game directory or symlink it.
- **Three more hardware interfaces identified:** I2C (`/dev/i2c-10`), UART (`/dev/ttyAMA5`), and a version file (`VERSION.txt`)
- The SPI thread runs independently — other init happens in parallel on the main thread

### 9e: Current state assessment

The game's init sequence, in order:
1. Load shared libraries ✅ (sysroot provides them)
2. Open SPI device `/dev/spidev4.0` ✅ (shim intercepts)
3. Configure SPI (100kHz, mode 3) ✅ (shim accepts)
4. Start SPI polling thread ✅ (shim responds with zeros — game doesn't block)
5. Read `/usr/local/spike/VERSION.txt` ❌ (needs stub)
6. Open `/dev/i2c-10` ❌ (needs stub — likely for LCD backlight, NUK display, or power management)
7. Open `image.bin` ✅ (found via relative path)
8. Open `/dev/ttyAMA5` ❌ (needs stub — UART bridge for LCD node or netbridge)

**Next step:** The TX data being all-zeros suggests the game needs a valid SPI response to start sending real commands. Need to investigate what RX bytes the state machine expects — particularly byte 2 bit 2 (`(response[2] >> 2) & 1`). Also need to expand the shim to handle `/dev/i2c-10`, `/dev/ttyAMA5`, and provide a `VERSION.txt`. But first priority: get the SPI response right so the game's TX data becomes non-zero, indicating it's actually talking to the node bus.

## Step 10: State transition table (data_3bce960)

A preliminary dump of this table was attempted but the data needs to be re-verified before drawing conclusions. What we know from the RE: the table exists, contains values 0/1/2/-1, and is indexed by a combination of `(RX[2] >> 2) & 1` and internal state bits. The exact structure needs further investigation if we need to craft precise SPI responses.

For now, the empirical approach (try different RX values, observe behavior changes) is more productive than trying to fully reverse the state machine.

## Step 11: SPI shim with RX[2] = 0x04 (bit 2 set)

Modified shim to return `0x04` in byte 2 of every SPI response (setting the bit the state machine checks).

```bash
cd games/pokemon_le && timeout 10 qemu-aarch64 -L /mnt/c/Workspace/pokemon-le-0.81.0 \
  -E LD_PRELOAD=/.../spi_shim.so ./game
```

**Result — new error messages appeared!**

stderr:
```
failed to initialize bridge: No such file or directory
```

stdout (first game output we've ever seen!):
```
can't verify/program bridge, file(s) missing
failed to init bridge thread
```

**TX data still all zeros** — the state machine transition alone doesn't trigger outgoing node commands. However, the game **advanced further in init** — it's now failing on the UART bridge (`/dev/ttyAMA5`) and looking for firmware files to program/verify the netbridge. This error was not present with all-zero RX responses, confirming the `0x04` byte moved the state machine forward.

The game continues running after these errors — it doesn't exit on bridge failure.

**Updated init sequence:**
1. Load shared libraries ✅
2. Open SPI `/dev/spidev4.0` ✅ (shim)
3. Configure SPI (100kHz, mode 3) ✅ (shim)
4. Start SPI polling thread ✅ (shim responds with `RX[2]=0x04`)
5. Read `/usr/local/spike/VERSION.txt` ❌ (needs stub)
6. Open `/dev/i2c-10` ❌ (needs stub)
7. Open `image.bin` ✅ (relative path, run from game dir)
8. Open `/dev/ttyAMA5` ❌ → **"failed to initialize bridge"**
9. Verify/program bridge firmware ❌ → **"can't verify/program bridge, file(s) missing"**
10. Init bridge thread ❌ → **"failed to init bridge thread"**

**Next step:** Expand the shim to also intercept `/dev/ttyAMA5` (UART) and `/dev/i2c-10` (I2C), returning fake fds. Also create `/usr/local/spike/VERSION.txt` in the sysroot. The goal is to keep peeling back layers of init to see what the game does next.

## Step 12: Expanded shim — UART, I2C, and path remapping

### 12a: Shim expanded to intercept three devices

Rewrote `spi_shim.c` (still named that but now handles all three devices) to intercept:

| Device | Fake FD | Purpose |
|--------|---------|---------|
| `/dev/spidev4.0` | 1023 | SPI node bus |
| `/dev/i2c-10` | 1022 | I2C bus (slave addr 0x57 = EEPROM) |
| `/dev/ttyAMA5` | 1021 | UART bridge (netbridge comms) |

Also hooked `read()` and `write()` for UART data logging, plus specific UART ioctls:
- `FIONREAD` (0x541e) → returns 0 (no data available)
- `TIOCOUTQ` (0x541f) → returns 0 (output queue empty)
- `TCGETS/TCSETS` → accepts termios configuration
- `TIOCMGET` → returns modem lines up (DCD + CTS)

### 12b: Path remapping via SPIKE3_SYSROOT

QEMU user-mode's `-L` flag only affects the dynamic linker — regular file operations (`openat`, `faccessat`, `fstatat`) use host filesystem paths directly. Added path remapping to the shim:

- Set `SPIKE3_SYSROOT=/mnt/c/Workspace/pokemon-le-0.81.0` environment variable
- Shim intercepts `open`, `openat`, `faccessat`, `fstatat` and prepends the sysroot to absolute paths
- Excludes `/dev/`, `/proc/`, `/sys/`, `/tmp/` from remapping

### 12c: Filesystem layout clarification

On the real SD card, `games/game`, `games/service_s_worker`, and `games/spike3/` were **symlinks** pointing into `games/pokemon_le/`. In our backup, the files were copied into both locations (contents identical, confirmed via `diff`). The game binary lives at `/games/pokemon_le/game` and expects sibling directories at `/games/spike3/`.

### 12d: Results with expanded shim

```bash
cd games/pokemon_le && qemu-aarch64 -L /mnt/c/Workspace/pokemon-le-0.81.0 \
  -E LD_PRELOAD=/.../spi_shim.so \
  -E SPIKE3_SYSROOT=/mnt/c/Workspace/pokemon-le-0.81.0 \
  ./game
```

**Init sequence observed:**
1. SPI opens, configures (100kHz, mode 3), starts polling ✅
2. I2C opens, sets slave address **0x57** (EEPROM), does ~10 RDWR transfers ✅
3. UART opens, checks FIONREAD/TIOCOUTQ, **closes immediately** ✅ (intercepted but not enough)
4. Bridge init fails → `"failed to initialize bridge: No such file or directory"`
5. stdout: `"can't verify/program bridge, file(s) missing"` / `"failed to init bridge thread"`
6. Game continues running (SPI polling continues indefinitely)

**Key discoveries:**
- I2C slave address **0x57** is a standard EEPROM address (AT24C256 or similar). The game likely reads serial number, game settings, or calibration data from it.
- The UART bridge thread opens `/dev/ttyAMA5`, checks buffer status, then closes and exits — the "No such file or directory" error may come from a deeper init step, not the UART open itself.
- The `faccessat` for `/games/spike3/netbridge/spike3-swd.cfg` returned 3 instead of 0 — **our faccessat shim has a bug** (likely the `fstatat` overload interfering, or QEMU returning an unexpected value). This needs investigation.
- The bridge failure and UART close happen on **different threads** — the UART thread (45928) closes quickly, then much later a different thread (45921) does the `faccessat` check and reports the failure.

### 12e: PATH REMAPPING STATUS — NOT FULLY VERIFIED

The `SPIKE3_SYSROOT` path remapping was added but we haven't confirmed it works correctly. The `faccessat` returned 3 (should be 0 or -1), suggesting either a bug in the shim or QEMU interference. This needs debugging before we can rely on it.

**What works without remapping (run from `games/pokemon_le/` as cwd):**
- `image.bin` — relative path, works ✅
- `/usr/local/spike/VERSION.txt` — QEMU `-L` handles this (it's in the sysroot) ✅
- `/dev/*` — intercepted by shim ✅

**What needs remapping:**
- `/games/spike3/netbridge/spike3-swd.cfg` — absolute path not under QEMU's `-L` scope ❌

### 12f: Current blocker analysis

The bridge failure is the main blocker preventing further init progress. Two possible causes:
1. **Path issue** — the `faccessat` for `spike3-swd.cfg` isn't resolving correctly
2. **UART protocol issue** — the bridge init needs actual UART communication (firmware version exchange, handshake) that our stub doesn't provide

The UART closing immediately (0 reads, 0 writes) suggests the game detects something wrong before even trying to communicate — possibly the termios setup fails or a follow-up check fails. This needs RE of the bridge init code (`sub_d15aa8` and related functions).

## Current State Summary (end of session)

**What's working:**
- Game binary runs under QEMU user-mode aarch64 (v10.0.8)
- All 31 shared libraries load from sysroot
- SPI node bus emulated (open, config, 8-byte transfers with RX[2]=0x04)
- I2C bus intercepted (slave 0x57, RDWR transfers accepted)
- UART bridge intercepted (open/close/ioctls handled)
- `image.bin` asset container opened successfully
- Game does NOT crash — runs indefinitely

**What's not working:**
- Bridge init fails ("failed to initialize bridge")
- SPI TX data stays all-zeros (game not sending real node commands yet)
- Path remapping (`SPIKE3_SYSROOT`) not verified working
- `faccessat` shim may have a bug (returning 3 instead of 0/-1)

**Run command:**
```bash
cd /mnt/c/Workspace/pokemon-le-0.81.0/games/pokemon_le && \
qemu-aarch64 -L /mnt/c/Workspace/pokemon-le-0.81.0 \
  -E LD_PRELOAD=/mnt/c/Workspace/pokemon-le-0.81.0/games/pokemon_le/_emulation/spi_shim.so \
  -E SPIKE3_SYSROOT=/mnt/c/Workspace/pokemon-le-0.81.0 \
  ./game
```

**Next steps (pick up here):**
1. Debug the `faccessat` shim — it's returning 3 instead of 0/-1. Might be our `fstatat` override causing issues, or QEMU weirdness.
2. RE the bridge init code (`sub_d15aa8` or related) to understand why UART closes immediately and what the "No such file or directory" actually refers to.
3. Once bridge init is resolved, see what the next layer of init looks like (likely DRM/display, audio, GPIO).
4. Get SPI TX data to become non-zero — this would mean the game is actually trying to talk to hardware nodes.
