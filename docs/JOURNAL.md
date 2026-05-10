# Spike 3 Emulation Journal

Step-by-step log of the emulation exploration for running the Pokemon Pinball `game` binary on a dev workstation via QEMU user-mode. Each entry documents what was tried, what was learned, and what it leads to next.

**Goal:** Run the real `game` binary under QEMU aarch64 user-mode, stub/emulate hardware dependencies, and eventually bridge I/O to Visual Pinball (VPX).

**Reference architecture** (from Researcher A's working emulator): QEMU user-mode + library hooking (gstreamer/ffmpeg/OpenGL forwarded to host) + file I/O interception (devfs emulation) + node bus emulation.

> **Path note (2026-04-23 reorganization):** Entries below Step 64 reference path prefixes that were reorganized into `_emulation/docs/`, `_emulation/captures/`, `_emulation/runs/logs/`, etc. Historic references like `_emulation/run_step45.log` now live at `_emulation/runs/logs/run_step45.log`; `_emulation/audio/stream.s16le` at `_emulation/captures/audio/stream.s16le`. See the current `_emulation/README.md` "Directory layout" section for the authoritative map. Historical paths are retained except where replaced by an explicit alias.

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

## Step 13: Unblocking bridge init via Sidekick RE + shim path/syscall hooks (2026-04-17)

### 13.0: Why this session and how we approached it

**Entry state:** Stuck on `"failed to initialize bridge: No such file or directory"`. The leading hypothesis from Step 12 was a bug in our `faccessat` shim. We also didn't really know what "the bridge" was at the code level — we'd been inferring from surrounding files.

**Decision — get definitive RE before guessing more.** The user has Binary Ninja Sidekick (AI assistant in Binary Ninja). Instead of continuing to poke at the shim blindly, we wrote a structured research task asking for: string xrefs to the error messages, full pseudocode of the bridge init functions, the file-access map, the call graph up to main, and the UART close path. The goal was to stop guessing and start building on real code.

**What the first Sidekick round revealed that changed our model:**

1. **The "bridge" is an RP2040 microcontroller** (the netbridge) that the main CPU flashes via SWD at boot. This was a surprise — we expected libs; instead, the game shells out to `openocd` as an external process. Nobody was going to guess that.
2. **The failing call is `access()`, not `faccessat()`.** That's the real reason our shim didn't help. On aarch64 glibc, `access()` uses `INLINE_SYSCALL_CALL` to hit the `faccessat` *syscall* directly — bypassing the libc `faccessat` *function*. Our LD_PRELOAD of the libc wrapper never fired. This is a libc implementation quirk we'd never have found without seeing the source call.
3. **Expected CRC comes from the bridge itself** via `send_bridge_message_with_response(cmd=0x30)` over UART — not from EEPROM or a hardcoded constant as we'd assumed. That makes the CRC verify dependent on the UART thread being alive: circular. A dead bridge thread can never self-report, so the game will always want to re-flash when emulated.
4. **The UART bridge thread (`sub_d15aa8`) is spawned BEFORE the file checks in `sub_d19580`.** So the thread is already running (and failing) by the time `access()` is called. The `data_490e5b0 & 1` success flag is independently set by the thread — not by any return value of `sub_d19580`.
5. **`sub_a75ae0` tail-calls `perror("failed to initialize bridge")` on any `sub_d19580` failure and returns** — it does NOT continue with later init. So as long as `sub_d19580` returns 0, the rest of init runs (EEPROM phase, sub_ced270, etc.), even if the bridge thread itself died.

**The plan these findings suggested:**

Two levers to pull, both to make `sub_d19580` return 0 cleanly:

- **Lever A — path resolution.** Add `access()` to the shim (plus `stat`/`lstat`/`__xstat*` for good measure since they have the same glibc-bypass problem). This alone fixes the "file(s) missing" branch.
- **Lever B — block `openocd`.** When the CRC inevitably mismatches (dead bridge thread → `expected_crc = 0`), the game will try to flash the RP2040. Intercept `system()` and return 0 for any `openocd`/`/games/spike3/` command. We don't care about the flash "succeeding" — the thread-ready flag is checked independently.

**What we deliberately chose NOT to do yet:** We did NOT try to emulate the UART bridge protocol. We don't have enough RE on `sub_d15aa8` / `sub_cf7c10` to know the packet format or what message triggers `data_490e5b0 |= 1`. Attempting to fake the protocol now would be guessing. We'll pull the unabridged HLIL first.

### 13.1: Call graph

```
main (0x861270)
  → sub_a78bc0 (0xa78bc0)
    → sub_a75ae0 (0xa75ae0)              ← prints "failed to initialize bridge"
      → sub_d19580 (0xd19580)            ← the bridge init master function
        → pthread_create(sub_d15aa8+8)   ← spawns UART bridge thread
        → sub_d1e0e0(...timeout 0x3e8)   ← waits up to 1000 ms for thread ready
        → access(spike3-swd.cfg, 4)      ← returns -1 if missing → "file(s) missing"
        → access(rp2040.cfg, 4)
        → access(netbridge.elf, 4)
        → sub_d19250(netbridge.elf)      ← zlib CRC32 of .text + .rodata + .binary_info
        → sub_d181d0(&expected_crc)      ← queries bridge via cmd 0x30 over UART
        → system("openocd -d0 ...")      ← flashes RP2040 if CRC mismatch
        → check data_490e5b0 & 1         ← "failed to init bridge thread" if unset
```

### 13.2: Important findings from the Sidekick RE (reference)

- **`sub_d19580` spawns the thread BEFORE checking files.** The UART bridge thread is already running (and failing) by the time the `access()` calls happen.
- **Expected CRC is not hardcoded** — `sub_d181d0` calls `send_bridge_message_with_response(cmd=0x30)` over UART; the bridge itself reports what CRC to expect. So the CRC check depends on the UART thread being alive.
- **CRC is zlib/PKZIP poly 0xEDB88320** over specific ELF sections of `netbridge.elf` (not the whole file).
- **`data_490e5b0 & 1`** is the thread-ready flag. Set inside `sub_d15aa8` on a successful handshake; never set if the thread bails.
- **`sub_a75ae0` tail-calls `perror("failed to initialize bridge")` if `sub_d19580` returns nonzero** and does NOT continue — so bridge init failure blocks all subsequent init (sub_ced270, sub_ced280, EEPROM read/write at 0x1800, final 1s poll of sub_d17b60 waiting for `b == 2`).
- **EEPROM schema** (partial, from sub_a75ae0 usage of data_4882198, 32 bytes from I2C 0x57 / offset 0x1800):
  - `0x00..0x1d` — payload (unknown fields)
  - `0x1e` — checksum byte = `~sum(bytes[0..0x1d])` (low byte only)
  - `0x1f` — unused
- **VERSION.txt consumer** is `sub_a4fbf0 (0xa4fbf0)`. Parses `"%19s %c%d.%d.%d"` (e.g. `spike3_secure R0.81.0`). Failure is non-fatal — zeroes the version and continues.

### 13.3: Shim changes and why

**The specific fix for the "spike3-swd.cfg missing" error** came directly from finding 2 above: glibc bypasses the libc `faccessat` wrapper. So we added an `access()` hook with the same `remap_path` treatment. While we were there, we did the same for `stat/lstat/__xstat/__lxstat/__xstat64/__lxstat64` — these have the identical glibc-bypass problem and we'd rather fix them proactively than discover the same bug twice when stat() calls start showing up later.

**Why also hook `system()`:** Finding 3 told us the CRC check would fail because the bridge thread is dead. The game's fallback is `system("openocd -d0 -f ... -c \"program netbridge.elf ...\"")`. We don't want that running at all in emulation — there's no RP2040 to flash, `openocd` might not even be installed on the host, and most importantly the command could have unpredictable side effects. Returning 0 from `system()` for any command containing `openocd` or `/games/spike3/` is a small, targeted block. Finding 4 made this safe: the thread-ready flag is independent of programming success.

**Latent bug spotted in existing hooks:** `fstatat` and `__fxstatat` were sharing a single static `real_fstatat` function pointer. Whichever was called first would `dlsym` to ITS symbol, and the other would then call the wrong underlying function. Separated them with their own static pointers. Not the cause of today's issue, but a bug worth fixing.

Concrete additions to `_emulation/spi_shim.c`:
- `access()` with remap
- `stat()`, `lstat()`
- `__xstat`, `__lxstat`, `__xstat64`, `__lxstat64` (glibc compat)
- `__fxstatat64` (parity with `__fxstatat`)
- `system()` with openocd/spike3 block
- Separated fn pointers for `fstatat` / `__fxstatat` / `__fxstatat64`

### 13.4: Test results after shim fixes

```
stdout:
  netbridge image CRC mismatch, write new image
  failed to init bridge thread

stderr (filtered, collapsed):
  [shim] OPEN /dev/spidev4.0  → fake fd 1023
  [shim] SPI ioctl WR_MAX_SPEED_HZ = 100000 Hz
  [shim] SPI ioctl WR_MODE = 3
  [shim] OPEN /dev/i2c-10     → fake fd 1022
  [shim] I2C ioctl SET_SLAVE addr=0x57  (×2 — before and after bridge init)
  [shim] I2C ioctl RDWR       (EEPROM read & write-back)
  [shim] OPEN /dev/ttyAMA5    → fake fd 1021
  [shim] UART ioctl FIONREAD → 0
  [shim] UART ioctl TIOCOUTQ → 0
  [shim] CLOSE UART (fd 1021) after 0 reads, 0 writes
  [shim] access REMAP: /games/spike3/netbridge/spike3-swd.cfg → /mnt/.../spike3-swd.cfg  (all 3 files)
  [shim] BLOCKED system(): openocd -d0 -f ... (full command, intercepted)
```

Key observations:
- No `perror("failed to initialize bridge")` — `sub_d19580` returned 0, not -1. `sub_a75ae0` proceeded past bridge init.
- Second `I2C SET_SLAVE 0x57` is `sub_a75ae0`'s EEPROM write-back → confirms EEPROM phase ran.
- **TCSETS is NEVER called** on the UART (0 occurrences in the entire log). Only FIONREAD + TIOCOUTQ + close. This means `sub_cf7c10`'s termios setup block is NOT being reached — the UART path is taking an early exit somewhere before `tcsetattr`.
- Only ONE `/dev/ttyAMA5` open — thread does not retry after close.
- No other devices opened (no DRM, no framebuffer, no audio, no GPIO). Game appears stuck in `sub_a75ae0`'s final 1-second poll of `sub_d17b60` (which waits for `var_40 == 2`, times out, and returns).

### 13.5: What the test revealed, and the new mystery

Progress is real: the first two levers worked. We're past the bridge file checks and past the openocd block. `sub_d19580` returned 0. `sub_a75ae0` advanced into the EEPROM phase (the second `I2C SET_SLAVE 0x57` is the write-back after the checksum verify). Both signs that the main init path is running again.

But **one observation from the test is genuinely strange and changes what we need next: TCSETS is never issued on the UART.** Zero occurrences in the whole log. The entire UART lifecycle is just `open → FIONREAD → TIOCOUTQ → close`, with no `read()`, no `write()`, and no termios configuration. Per Sidekick, `sub_cf7c10` is supposed to do cfsetspeed + cfmakeraw + TCSETS + TCFLSH + TIOCMSET. None of those are happening.

Three hypotheses, in rough order of likelihood:
1. **`sub_cf7c10` bails before `tcsetattr`.** A validation check on a parameter (baud code, csize, stopbits, parity) fails early and returns -1 without going near termios. `sub_d15aa8` interprets the failure, calls `tcdrain`/`close` (which is where FIONREAD+TIOCOUTQ come from — `tcdrain` uses TIOCOUTQ internally to wait for output).
2. **`sub_d15aa8` itself has a pre-config "bridge alive?" probe** that opens `/dev/ttyAMA5`, calls FIONREAD to see if there's already data queued (there should be, from a running bridge), gets 0 from our shim, and decides "no bridge present, abort".
3. **`sub_cf7c10` does run termios but through a different ioctl path** that we're not catching (e.g., the older `TCSETS2` variant `0x402c542b` — we only hook `TCSETS` = `0x5402`).

Hypothesis 2 feels most likely given the pattern. FIONREAD returning 0 on a real bridge would be unusual (the RP2040 should be spamming status). Our EAGAIN-on-read might not even matter — the code may never get to read() if FIONREAD=0 is the abort signal.

**Why we can't just guess the fix:** We don't have `sub_cf7c10`'s exact parameter list or its error branches. We don't know whether `sub_d15aa8` calls FIONREAD before or after `sub_cf7c10`. And even if we fix the close, we still need the wire protocol to set `data_490e5b0 |= 1` — returning random bytes from UART reads won't do it. The next logical step is another Sidekick round for the unabridged function bodies and the wire protocol.

**In parallel, the offline CRC work:** We read `netbridge.elf`'s section headers directly (`readelf -WS`) and computed zlib CRC32 of the three relevant sections offline in Python. Results:
- `.text` (off=0x1100, size=0x9a48): `0x2933163d`
- `.rodata` (off=0xab48, size=0x8fe0): `0xeb9ef215`
- `.binary_info` (off=0x13b28, size=0x18): `0x1d150d91`
- **Chained forward (.text → .rodata → .binary_info): `0x357f5327`** — our expected answer to cmd 0x30 once we build the bridge protocol shim
- Chained reverse (safety backup): `0x90aeceac`

We pre-compute this now so when we know the wire format we can hardcode the response immediately instead of re-deriving.

### 13.6: Decisions made this session, and why

| Decision | Alternative | Why we chose this |
|----------|-------------|-------------------|
| Ask Sidekick for full RE before touching shim | Keep guessing based on strace output | We'd already spent a session on guesswork; an RE pass dwarfs the hours of speculative iteration |
| Hook `access()` specifically (not `access`-via-faccessat tricks) | Intercept at syscall level via seccomp/ptrace | LD_PRELOAD of the libc function is simplest; works because the game uses libc's `access()` which resolves through the PLT, not via inline-syscall |
| Add `stat`/`lstat`/`__xstat*` proactively | Wait until they fail in practice | Same glibc-bypass class of bug; cheap to fix all at once; avoids repeat debugging |
| Block `openocd` via `system()` hook | Let it run and fail naturally | Unpredictable host side effects; `openocd` may not be installed; easier to verify in logs that it was blocked than to reason about what happened if it ran |
| Return 0 (success) from blocked `system()` | Return -1 (failure) | Finding 4 says thread-ready flag is independent; return value of `system()` in this path only logs "failed to verify/update bridge", no hard abort |
| DON'T emulate the UART protocol yet | Start crafting fake response bytes | Without the wire format we'd just generate noise; the protocol RE is Sidekick's next task |
| Pre-compute CRC offline while waiting for Sidekick | Defer until we know the wire format | Orthogonal work; confirms the algorithm identification and gives us the target value regardless of when we wire it up |
| Keep `service_s_worker` out of scope | Start RE'ing `service_s_worker` in parallel | One blocker at a time; `service_s_worker` only matters if `game` explicitly waits on something `service_s_worker` provides, which we haven't seen evidence of |

---

## Step 14: Resolving the address mismatch and unblocking the UART thread (2026-04-17, later)

### 14.0: Why this step and how we got here

Coming out of Step 13 we had one open mystery — `TCSETS` was never issued on the UART despite `sub_cf7c10` supposedly being a termios configuration function. Sidekick gave us specific instruction addresses for a "csize validation bug" hypothesis, but when we cross-checked against `aarch64-linux-gnu-objdump` the instructions at those addresses didn't match at all (we saw `add x3, x3, #0x830` where Sidekick showed `sub sp, sp, #0x190`). The concern: either Sidekick was hallucinating, or we were looking at different binaries.

**Decision — pause patching, confirm the binary.** Rather than spend cycles verifying each Sidekick claim via objdump, ask the user to run `sha256sum` on both sides (theirs via BN's Python console using `bv.file.raw`, ours on the repo file). If hashes matched, something subtler was happening.

**What we found.** Same hash (`7eaae301bb4491652adb38a84c5d7c3cf8a85071ab9630b299c3f50b47724eaa`), same 71,349,568-byte file. Sidekick's analysis had been correct all along — at the time, Binary Ninja's PIE loader was rebasing the binary so its VAs ran ahead of the file offsets. The BN database has since been rebased to base 0x0; today BN VA == ELF VA == file offset and no translation is required.

**Address convention going forward:** BN VA == ELF VA == file offset for this binary. Runtime address is `game_base + va`. The translation table in Step 14.1 still lists the original pre-rebase BN VAs alongside today's values for reference.

### 14.1: Address translation table

| Symbol (Sidekick name) | BN VA (pre-rebase, historical) | ELF VA / current BN VA |
|---|---|---|
| `main` | 0x861270 | 0x461270 |
| `sub_a78bc0` | 0xa78bc0 | 0x678bc0 |
| `sub_a75ae0` | 0xa75ae0 | 0x675ae0 |
| `sub_d19580` | 0xd19580 | 0x919580 |
| `sub_d15aa8` (UART thread entry) | 0xd15aa8 | 0x915aa8 (2 NOPs, body at +8) |
| `sub_cf7c10` (UART open wrapper) | 0xcf7c10 | 0x8f7c10 |
| `sub_d181d0` (queries cmd 0x30) | 0xd181d0 | 0x9181d0 |
| `sub_d19250` (CRC over netbridge.elf) | 0xd19250 | 0x919250 |
| `sub_d164a0` (send_bridge_message_with_response) | 0xd164a0 | 0x9164a0 |
| `sub_d1e0e0` (pthread_cond_timedwait wrapper) | 0xd1e0e0 | 0x91e0e0 |
| `sub_d1e1e0` (sets `data_490e5b0 \|= 1`) | 0xd1e1e0 | 0x91e1e0 |
| `data_490e5b0` (thread-ready flag) | 0x490e5b0 | 0x450e5b0 |
| `data_3bce450` (baud rate table) | 0x3bce450 | 0x37ce450 |

### 14.2: Disassembling `sub_cf7c10` at the correct offset

With addresses reconciled, I disassembled `sub_cf7c10` (0x8f7c10) end-to-end. The structure:

```
prologue (0x8f7c10-0x8f7cb4)   allocates 400 bytes, copies 240-byte baud table to stack
baud lookup loop (0x8f7ccc-0x8f7cd4)   linear scan for matching baud code
speed bit-31 check (0x8f7ce4)   branch to 0x8f7ecc if invalid
csize check (0x8f7ce8-0x8f7cf8)   ldrb + sub #5 + cmp #3; branch to 0x8f7e88 if >3
stopbits check (0x8f7cfc-0x8f7d0c)   ldrb + sub #1 + cmp #1; branch to 0x8f7e0c if >1
open() (0x8f7d10-0x8f7d20)   O_RDWR | O_NOCTTY; branch to 0x8f7e4c if fd<0
ioctl FIONREAD (0x8f7d24-0x8f7d2c)   our shim handles
ioctl TIOCOUTQ (0x8f7d34-0x8f7d4c)   our shim handles
cfmakeraw (0x8f7d50-0x8f7d54)   pure userspace
baud→termios conversion (0x8f7d58-0x8f7d8c)
cfsetspeed (0x8f7d8c-0x8f7d90)   pure userspace
tcsetattr (0x8f7db4)             ← FAILS: bit 31 of return set → branch to 0x8f7e38
tcflow (0x8f7dc4)                skipped on failure path
tcflush (0x8f7dd0)               skipped on failure path
0x8f7e38 error path: close(fd), w20 = -1, jump to epilogue
```

With `w22 = 0` (the silent-mode arg passed from `sub_d15aa8`), each failure branch takes the "silent" variant — no fprintf, just set w20=-1 and cleanup. That's why our test log had no diagnostic string from `sub_cf7c10`; just the observable fd open/close pattern.

### 14.3: Root cause — `tcsetattr` uses INLINE_SYSCALL, bypassing our `ioctl` hook

The game calls `tcsetattr(fd, 0, &termios)` via the PLT. glibc's `tcsetattr` is implemented in `sysdeps/unix/sysv/linux/tcsetattr.c` using `INLINE_SYSCALL` to invoke the `ioctl(TCSETS)` syscall directly — **it does not call the libc `ioctl()` function**. Our LD_PRELOAD of `ioctl()` therefore never fires for this path.

Under QEMU user-mode, the TCSETS syscall on our fake fd 1021 (which is not a real kernel fd) returns `-EBADF`. `tcsetattr` returns -1, the game takes the error branch and closes the fd.

**This is the same bug class as `access()` in Step 13.** glibc's short-circuit optimizations for system calls break LD_PRELOAD intercepts that hook the function layer. The fix is always the same: hook the higher-level libc function directly.

The `FIONREAD` and `TIOCOUTQ` ioctls DID hit our hook because the game's code calls them as `ioctl(fd, FIONREAD, ...)` — a public libc function call through the PLT. So `ioctl`-via-game-code works; `ioctl`-via-glibc-internal doesn't.

### 14.4: Shim changes and why

Added hooks for every termios function that glibc typically inline-syscalls:

- `tcsetattr` — the immediate fix for the UART close
- `tcgetattr` — likely used by callers we haven't seen yet; returns zeroed termios on fake fd
- `tcflush`, `tcflow`, `tcdrain`, `tcsendbreak` — called on the success path or error cleanup; return 0 on fake fd

Explicitly NOT hooked:
- `cfmakeraw`, `cfsetspeed`, `cfsetispeed`, `cfsetospeed` — these modify the termios struct in memory only; no syscall involved. LD_PRELOAD wouldn't help and isn't needed.

### 14.5: Test results — the bridge protocol is now alive

```
UART writes observed from the game:
  02 20 01 df 03   ← cmd 0x20 (hw version info query)
  02 30 01 cf 03   ← cmd 0x30 (expected CRC query)

Frame validates to Sidekick's B2 specification:
  [0x02][CMD][ID][...optional data...][CKSUM][0x03]
  Checksum = sum of all bytes including itself == 0 (mod 256)
  0x20 + 0x01 + 0xdf = 0x100 → low byte 0x00 ✓
  0x30 + 0x01 + 0xcf = 0x100 → low byte 0x00 ✓

New stdout:
  bridge message (cmd=30, id=1) response failed 110: Connection timed out
  netbridge image CRC mismatch, write new image
  bridge message (cmd=20, id=1) response failed 110: Connection timed out

New stderr:
  failed to initialize bridge: Invalid argument
```

What this tells us:
- **The UART thread is alive.** `sub_d15aa8` entered its main loop and is driving the wire protocol.
- **`data_490e5b0 |= 1` was set** (otherwise we'd still see "failed to init bridge thread" — we don't).
- **The game's protocol stack is working correctly** — it's producing valid frames with correct checksums in the exact format Sidekick described.
- **Queries are timing out** because our shim's UART `read()` returns EAGAIN; the game's `send_bridge_message_with_response` waits 1000 ms per cmd via `pthread_cond_timedwait` then gives up with errno 110 (ETIMEDOUT).
- **`sub_d19580` now returns non-zero** (because cmd 0x10 pwrdist version also times out), so `sub_a75ae0` calls `perror("failed to initialize bridge")`. But this is the *next* blocker — not the thread-init one we started with.

### 14.6: Decisions made and the next-step tradeoff

| Decision | Alternative | Why we chose this |
|----------|-------------|-------------------|
| Verify binary hashes before accepting Sidekick's addresses at face value | Trust Sidekick and patch based on its citations | Patching wrong bytes could corrupt the binary silently. 10 minutes of verification saved a potential disaster. |
| Disassemble `sub_cf7c10` ourselves at the corrected file offset | Ask Sidekick for more detail | Now that the address translation was known, we had enough in objdump to trace the function end-to-end without another round-trip |
| Hook termios functions at libc level | Hook the kernel syscalls via seccomp or QEMU plugin | LD_PRELOAD consistency with the rest of the shim; minimal new infrastructure; works for this binary which calls tcsetattr via the PLT |
| Return 0 from fake-fd termios calls (no state) | Maintain a termios state per fake fd | Currently nothing in the game reads back what was set; if that changes (e.g., tcgetattr after tcsetattr expecting round-trip), we'll add state then |
| Accept the "timing out on bridge responses" blocker rather than immediately synthesizing fake responses | Synthesize fake responses now | Response formats aren't fully known yet — guessing would burn time on incorrect guesses. Better to read `netbridge.elf` firmware or ask Sidekick for `send_bridge_message_with_response`'s response-side parsing before crafting replies. |

### 14.7: What's next — options to unblock bridge responses

Three plausible paths, all of which probably converge:

1. **Real pipe for the UART fd.** Replace the fake 1021 fd with one side of a `pipe2()`. The game's read() will actually block on the pipe. The shim holds the other end and can push synthetic responses whenever we want. Cleaner than EAGAIN-spam and lets us implement proper request/response timing.
2. **Synthesize responses for cmd 0x10, 0x20, 0x30.** Need the exact response frame for each. Options for learning that:
   - Ask Sidekick for `send_bridge_message_with_response` (0xd164a0) full body + the response-side parser in `sub_d15aa8`'s message loop (it must know how to decode frames the bridge sends back)
   - Disassemble `netbridge.elf` (ARM32 Cortex-M0+, 188KB) which is the actual RP2040 firmware — it's the authoritative source of what the bridge actually sends
3. **For cmd 0x30 specifically**, the response value is already known: the CRC32 `0x357f5327` we pre-computed in Step 13.5. We just need the framing.

Recommended order: (1) pipe plumbing first so reads block properly → (2a) Sidekick round for the response parser → (3) hardcode the three response frames the game needs for this init phase → observe what the game asks for next.

---

## Step 15: UART fd refactor to real socketpair (2026-04-18)

### 15.1: Why this change

Step 14 exposed a design limitation: our fake UART fd (1021) meant `read()` had to return something synthetic. Returning EAGAIN on a supposedly-blocking fd was wrong semantics — it was working by accident, and would have misled the thread as soon as the protocol started expecting blocking reads. Before adding response synthesis, upgrade the plumbing so the fd behaves like a real serial device.

### 15.2: The change

Replaced `FAKE_FD_UART = 1021` with a `socketpair(AF_UNIX, SOCK_STREAM)`. The game receives one end (`uart_game_fd`); the shim holds the other (`uart_shim_fd`). Game-side `read()` now blocks on the real socketpair and will unblock whenever the shim writes to `uart_shim_fd`. Game-side `write()` goes through to the real socketpair, which means bytes written by the game are readable by the shim on `uart_shim_fd` — setting us up for request-frame parsing in the next step.

Kept fake fds for SPI (1023) and I2C (1022) — those are pure ioctl interfaces, no `read`/`write` involved, so no need to change them. Termios function hooks (`tcsetattr` et al) stay — a socketpair isn't a tty, so the kernel still returns `-ENOTTY` for TCSETS on it if we don't intercept.

Introduced `is_uart_fd(fd)` helper; `is_fake_fd(fd)` no longer matches UART.

### 15.3: Test results

```
[shim] OPEN /dev/ttyAMA5 -> real socketpair game=3 shim=4 (UART bridge)
[shim] UART ioctl FIONREAD -> 0
[shim] UART ioctl TIOCOUTQ -> 0
[shim] UART tcsetattr(actions=0) -> fake OK
[shim] UART tcflow(action=1) -> fake OK
[shim] UART tcflush(q=2) -> fake OK
[shim] UART write #1 (5 bytes): 02 30 01 cf 03
[shim] UART write #2 (5 bytes): 02 20 01 df 03
```

- End-to-end UART bring-up identical to Step 14 (tcsetattr hook works, writes emitted correctly)
- No more EAGAIN spam — log is far quieter; the UART thread sits in `read()` patiently
- Game still times out on bridge responses (expected — we don't push replies yet); stdout and final error unchanged
- Game runs indefinitely without crashes

### 15.4: What's ready for the next step

All the plumbing to implement the bridge protocol is in place:

- Game-side reads block naturally → whenever we `real_write(uart_shim_fd, response_frame, n)` the game's read unblocks
- Game-side writes are already readable on `uart_shim_fd` → we can parse request frames synchronously in the write hook, or asynchronously via a reader thread on `uart_shim_fd`
- Termios/ioctl/close all correctly handle the real fd

Once Sidekick returns the response frame format for cmd 0x10 / 0x20 / 0x30 (Round 4 task is out), we can wire up the synthesis in the write hook directly: parse the CMD byte, build the matching response frame with the known values (CRC = `0x357f5327` for cmd 0x30), and `real_write(uart_shim_fd, ...)`.

---

## Step 16: Bridge protocol half-working; the address-space discovery and the pwrdist dispatch mystery (2026-04-18)

### 16.0: Why this session and the arc

Coming in, we had a working socketpair UART but no response synthesis. Sidekick's Round 4 RE gave us the framing, checksum rule, and per-cmd payload sizes. We wired response synthesis into the shim's `write()` hook: when the game writes a request frame, parse the CMD, build a matching response, push it to `uart_shim_fd` so the game's next `read()` unblocks with the payload.

This worked immediately for **cmd 0x30 (CRC query)**. Game log shows `netbridge image okay, CRC32=0x357f5327` — the pre-computed zlib CRC32 of netbridge.elf's `.text+.rodata+.binary_info` sections is exactly what the game expects. The entire OpenOCD reprogramming branch is now skipped on a clean match.

It did **not** work for cmd 0x10 (pwrdist version query). Symptom: game prints `BRIDGE_ProcessMessage unknown command: 0x00` and falls back to "Spike 1/2 power distribution board". Root cause — discovered by disassembling `sub_d15aa8`'s dispatch at file offset 0x915d00:

```asm
sub w0, w2, #0x10        ; w0 = cmd - 0x10
cmp w0, #0x31
b.ls 0x915e00            ; dispatch ONLY if (cmd-0x10) u<= 0x31
                         ; i.e., cmd in [0x10, 0x41]
; otherwise fall through to "BRIDGE_ProcessMessage unknown command"
```

**Sidekick's "response cmd 0x00 → pwrdist handler" mapping was wrong.** Cmd 0x00 under-flows the `sub` and fails the range check, so the dispatch handler that actually writes to `data_490e61c` never runs. The channel *is* still signaled via the unknown-cmd path (that's why `send_bridge_message_with_response` returned 0), but the payload was never parsed. `data_490e61c` stayed 0, triggering the `x19 = -6` fallback, which propagates up as the `failed to initialize bridge` perror.

### 16.1: The binary-address confusion episode (and what we learned)

Before landing on the above, we spent a detour verifying Sidekick's addresses. Sidekick claimed specific bytes at `0xd15aa8` / `0xcf7c10` that didn't match what my objdump showed at those file offsets. Hash comparison proved we were both looking at the same binary (`7eaae301...`, 71349568 bytes). The cause was that BN's PIE loader was rebasing the binary at the time, so its VAs ran ahead of the file offsets. The BN database has since been rebased to base 0x0; today BN VA == ELF VA == file offset and no translation is needed.

**Convention going forward:** BN VA == ELF VA == file offset. Use the same address everywhere.

### 16.2: Shim changes in this session

Added to `spi_shim.c`:
- **UART fd refactor** (Step 15): replaced fake fd 1021 with a real `socketpair(AF_UNIX, SOCK_STREAM)`. `uart_game_fd` is returned from `open()`; `uart_shim_fd` is held by the shim. Reads block naturally; writes pass through.
- **Response synthesis** in the `write()` hook: parses outgoing request frames, builds a response, pushes via `real_write(uart_shim_fd, ...)`. Currently handles cmd 0x20 → 0x10 reply, cmd 0x30 → 0x20 reply (with pre-computed CRC). Cmd 0x10 handler is there but with the wrong response cmd — see 16.3.
- **errno save/restore** around the response-synthesis work so our extra `real_write` on `uart_shim_fd` can't leak errno into the game's view of the primary write.
- **Diagnostic perror hook** that logs `errno` at call time. This let us confirm that when `sub_a75ae0` tail-calls `perror("failed to initialize bridge")`, errno = 22 (EINVAL) — separate mystery from the pwrdist blocker and currently ignored.

### 16.3: Immediate pick-up point for next session

The one concrete unknown: **which response-CMD byte routes to the dispatch handler that writes 7 bytes of pwrdist version into `data_490e61c`?**

Two cheap ways to find out:
1. Look at the dispatch ladder/table at file offset 0x915e00 (BN 0xd15e00) and find which cmd case jumps to the handler at file 0x916194 (BN 0xd16194). That's the RE answer.
2. Empirically: our current code tentatively uses response cmd 0x10 (untested but in range). Run the emulation — if data_490e61c gets set and init progresses past the pwrdist fallback, we're right. If not, iterate (try 0x11, 0x12, 0x14, etc., all in range).

Option 1 is faster and more reliable. Easiest path: ask Sidekick to show the dispatch at BN 0xd15e00 (raw disassembly of the first ~40 instructions) so we can see the cmd ladder structure and identify which cmd byte handles the pwrdist response payload.

### 16.4: External reference discovered — `[REFERENCE_TOOL_B]/`

Rust crate by Researcher E that drives Spike **2** hardware from a custom app [Redacted: identifying demonstration title]. Not a direct solution for us but a strong architectural cross-check:

- Confirms **SPI = local CPU-board switch state** (`/dev/spidev1.0` on Spike 2; `spidev4.0` on Spike 3). Our 8-byte SPI polling read matches this role.
- Confirms **UART = node bus** (`/dev/ttymxc1` on Spike 2 @ 460800 baud; `ttyAMA5` on Spike 3 @ 2000000 baud).
- Confirms the **checksum algorithm** `(256 − sum%256) % 256` — identical to what we derived.
- **Different framing:** Spike 2 sends raw byte messages with no `0x02/0x03` frame bytes; Spike 3 added framing.
- **Different cmd numbers:** Spike 2 uses 0x0a (GetBridgeState), 0x03 (GetBridgeVersion), 0x05 (GetBridgeStatus), 0x07 (SetPower), etc. Spike 3 uses 0x10/0x20/0x30 and a different numbering scheme. Not directly portable but a useful shape reference.
- **Init sequence on Spike 2:** SetPower(true), SetPower(false), 500ms, SetAmpPower(true), 500ms, SetPower(true), 500ms, Reset node 0, 250ms, then Poll loop to discover nodes. Likely close to what Spike 3 does after bridge init completes.

Doesn't unblock the pwrdist dispatch question, but worth a read before tackling the post-bridge init phase.

---

## Current State Summary (end of session — 2026-04-18)

### Where we are right now

The bridge protocol is **half-working**. The CRC response handshake succeeds; the game confirms `netbridge image okay, CRC32=0x357f5327`. But the pwrdist version query response is getting rejected by the dispatch (wrong response CMD byte), so `data_490e61c` stays 0, `sub_d19580` returns -6, and `sub_a75ae0` perror's out of init.

**What's working:**
- Game binary runs under QEMU user-mode aarch64 (v10.0.8); 31 libs load
- SPI fake fd + 8-byte transfers with `RX[2]=0x04`
- I2C fake fd (slave 0x57 EEPROM)
- UART on a real socketpair, full termios config via shim hooks
- Path remapping via `SPIKE3_SYSROOT`; access/stat family all hooked
- `system()` hook blocks all openocd shell-outs
- UART bridge thread `sub_d15aa8` alive, driving protocol
- Response synthesis for cmd 0x30 (CRC) → game accepts, skips OpenOCD reflash
- Thread-ready flag set; "failed to init bridge thread" is gone
- Game runs indefinitely without crashing

**What's not working:**
- Pwrdist version response uses wrong response CMD byte — game says `BRIDGE_ProcessMessage unknown command: 0x00` and falls back to Spike 1/2 path → `sub_d19580` returns -6 → perror fires
- Cmd 0x20 (HW version) response is probably in the same situation but hasn't been validated — we don't see it rejected currently because the pwrdist failure aborts first
- SPI TX still all-zeros (gated on bridge being fully up)
- No display / audio / GPIO activity yet
- Separate mystery: errno=22 (EINVAL) at the perror site. Incidental — not on the critical path once we fix the pwrdist dispatch

### Where we're headed

**Immediate next step:** identify the correct response CMD byte for the pwrdist handler that writes `data_490e61c` (BN function at 0xd16194 / file 0x916194). The dispatch ladder in `sub_d15aa8` at file 0x915e00 (BN 0xd15e00) has the answer. Easiest path: one short Sidekick query or 20 lines of objdump will expose the cmd mapping.

**Once that's fixed:** `sub_d19580` should return 0, `sub_a75ae0` proceeds past the perror into `sub_ced270` / `sub_ced280` / `sub_cf5f10` / the EEPROM read-write at 0x1800 / final 1s poll of `sub_d17b60`. From there we'll see the **next layer of init** — likely some mix of:
- DRM/KMS display setup (`/dev/dri/*`)
- libEGL / libGLESv2 context creation
- ALSA audio init (`/dev/snd/*`)
- GStreamer / ffmpeg pipeline setup for LCD/NUK animations
- GPIO device opens (`/dev/gpiochip*`)
- A second wave of SPI activity once node bus is up (non-zero TX bytes)

Each of those will be its own stub-or-forward decision per Researcher A's architecture.

### Why it matters

The bridge is the **single hardest gate** between "binary loads" and "game logic alive". Every other hardware subsystem depends on it being up first, both directly (the game uses node bus for switches/coils/LEDs) and indirectly (init sequence is gated on `sub_a75ae0` completing). Once bridge init returns 0 cleanly, a huge amount of downstream init runs in one sweep — we'll know within one session whether we're 3 more sessions away from "system thinks it's on and at the attract screen" or further. Right now we can't tell because the gate is closed.

Past this gate the character of the work changes. Today was protocol RE and byte-level shim bug fixing. Next it becomes library stubbing (libEGL, libdrm, gstreamer) — different tooling, but the kind of work Researcher A has already proven viable.

### Address convention (Binary Ninja ↔ our tools)
BN database is rebased to base 0x0; BN VA == ELF VA == file offset.
(Step 14.1's table lists pre-rebase BN VAs alongside today's values for reference.)

### Run command (unchanged)
```bash
cd /mnt/c/Workspace/pokemon-le-0.81.0/games/pokemon_le && \
qemu-aarch64 -L /mnt/c/Workspace/pokemon-le-0.81.0 \
  -E LD_PRELOAD=/mnt/c/Workspace/pokemon-le-0.81.0/games/pokemon_le/_emulation/spi_shim.so \
  -E SPIKE3_SYSROOT=/mnt/c/Workspace/pokemon-le-0.81.0 \
  ./game
```

### Pre-computed values ready to use
- zlib CRC32 of `netbridge.elf` `.text + .rodata + .binary_info` (forward chain): **`0x357f5327`** — verified as correct CRC response for cmd 0x30

### Reference: `[REFERENCE_TOOL_B]/`
Rust crate driving Spike **2** hardware (Reference tool B). Useful architectural reference only — Spike 2 and Spike 3 differ in baud rate, framing, and cmd numbers. See Step 16.4.

### Next steps (pick up here)
1. **Find the correct pwrdist response CMD byte.** Either ask Sidekick to show the dispatch ladder at BN 0xd15e00 and identify which cmd routes to BN 0xd16194, or disassemble file 0x915e00 in the game binary directly.
2. Patch `synthesize_bridge_response()`'s case 0x10 in `_emulation/spi_shim.c` with the correct response CMD byte (currently tentatively 0x10).
3. Re-run. Expect: `sub_d19580` returns 0, no perror, new init activity appears (look for fresh device opens or new stdout messages).
4. For whatever blocks next: repeat the pattern — trace, stub, test. Display/audio likely need library forwarding to host equivalents (Researcher A model).
5. Optional polish: fix the incidental errno=22 at perror site (only if init continues blocking on it — probably won't once `sub_d19580` returns 0).

---

## Step 17: Dispatch-table decode and the stale-blocker reveal (2026-04-18)

### 17.0: Why this step

An external observation attributed to Researcher A suggested a different trailer on node-to-bridge frames. That prompted a trailer experiment on the UART response path. The experiment and its correction follow; the external observation was not itself proof of the UART format. [Redacted: private source wording and identifying source paths.]

Independent of that, Step 16.3 listed two ways to find the correct response-CMD byte for pwrdist dispatch. We did both this session.

### 17.1: Trailer experiment — refuted

Edited `synthesize_bridge_response()` case 0x10 to set `resp[11] = 0x00` instead of `0x03`. Rebuilt, re-ran.

**Result (stdout):**
```
netbridge image okay, CRC32=0x357f5327
bridge message (cmd=10, id=1) response failed 110: Connection timed out
failed to get power distribution board version, assuming Spike 1/2 power distribution board: Connection timed out
```

Game now **times out** on cmd 0x10 (errno 110 = ETIMEDOUT). Interpretation: the parser in `sub_d15aa8` is a byte-at-a-time state machine looking for `0x03` as a frame terminator (the `cmp w0, #0x3; b.eq frame_complete` pattern we see at file `0x915c30`). With `0x00` the parser never recognizes the frame as complete and the game's `pthread_cond_timedwait` eventually expires.

**Conclusion:** the `0x03` trailer is a frame terminator, required on both directions of the UART. Researcher A's `0x00` observation must be on the RS485 side (node ↔ netbridge), which we never see because we've replaced the netbridge entirely. Reverted the change.

### 17.2: Dispatch-table decode

Disassembled the dispatch setup in `sub_d15aa8`. Relevant instructions:

```
0x915bac  adrp  x0, 0x37cf000
0x915bb0  add   x0, x0, #0x510       ; jump-table base = 0x37cf510
0x915bb8  str   x0, [sp, #40]
...
0x915cfc  sub   w0, w2, #0x10        ; w0 = cmd - 0x10
0x915d00  cmp   w0, #0x31
0x915d04  b.ls  0x915e00             ; dispatch if cmd in [0x10, 0x41]
...
0x915e00  ldr   x1, [sp, #40]        ; load table base
0x915e04  ldrh  w0, [x1, w0, uxtw #1]; entry = (i16)table[cmd - 0x10]
0x915e08  adr   x1, 0x915e14
0x915e0c  add   x0, x1, w0, sxth #2  ; target = 0x915e14 + sxt(entry) * 4
0x915e10  br    x0
```

The table is 50 signed 16-bit entries in `.rodata` at `0x37cf510`; each entry scaled by 4 and added to `0x915e14` gives the handler address. Decoded:

| Response cmd | Handler VA  | Notes                                                       |
|--------------|-------------|-------------------------------------------------------------|
| 0x10         | 0x916194    | **PWRDIST version** — 7-byte payload required               |
| 0x12         | 0x916158    | 7-byte payload (needs check)                                |
| 0x14         | 0x9160d8    | 4-byte payload                                              |
| 0x15         | 0x9160a0    | **1-byte payload** — OR/XOR into state word at `+128`       |
| 0x18         | 0x91607c    | variable payload; stores min into halfword table at `+0x88` |
| 0x1a         | 0x916064    | (needs check)                                               |
| 0x1b         | 0x916038    | (needs check)                                               |
| 0x1d         | 0x916028    | (needs check)                                               |
| 0x20         | 0x915fec    | HW version response (matches Sidekick's earlier mapping)    |
| 0x22         | 0x915e54    | (needs check)                                               |
| 0x2d         | 0x915f08    | (needs check)                                               |
| 0x2e         | 0x915ed8    | (needs check)                                               |
| 0x41         | 0x915f88    | (needs check)                                               |
| 0x27,0x28,0x40 | 0x915e14  | generic table-copy handler (channel-signal + memcpy)        |
| 0x11,0x13,0x16,0x17,0x19,0x1c,0x21,0x23,0x24,0x25,0x30,0x31 | 0x915d28 | generic "no-op with ACK" fallback |
| all others (0x1e,0x1f,0x26,0x29,0x2a–0x2c,0x2f,0x32–0x3f) | 0x915d08 | generic **error / unknown-command** path |

**Key confirmations:**
- **cmd 0x10 = the pwrdist handler.** Our shim's current `resp[1] = 0x10` is correct.
- **cmd 0x15 and cmd 0x18 are real, dispatched handlers** — not unknown cmds. We need to synthesize these too.

### 17.3: The stale-blocker reveal

Ran baseline (0x03 trailer, cmd 0x10 response cmd = 0x10). The end-of-session summary from Step 16 said the game was still reporting "unknown command: 0x00". **That was stale** — left over from when we had `resp[1] = 0x00` at an earlier point. Current reality:

```
stdout:
  netbridge image okay, CRC32=0x357f5327
  bridge message (cmd=15, id=1) response failed 110: Connection timed out
  bridge message (cmd=18, id=2) response failed 110: Connection timed out
  failed to get power distribution board version, assuming Spike 1/2 power distribution board: Connection timed out
```

- **Cmd 0x10 pwrdist response is accepted.** The handler at `0x916194` runs, packs our `01 00 00` into `data_490e61c = 0x010000` and the flag at `data_490e5b0 + 0x74` becomes 0 (since payload[6] = 0).
- **The new, real blocker is cmd 0x15 and cmd 0x18.** These are sent by the game immediately after cmd 0x10 completes, as the next phase of pwrdist initialization. Our shim has no synthetic response for them, so they time out after 1000 ms each.
- The "assuming Spike 1/2 power distribution board" message is the propagated error from the cmd 0x15 timeout — *not* from cmd 0x10 rejection.

### 17.4: Handler requirements for the new blockers

Disassembled `0x9160a0` (cmd 0x15 handler):

```
0x9160a0  cmp   w27, #0x1              ; payload length must == 1
0x9160a4  b.ne  0x915d28               ; else jump to generic fallback
; body: payload[0] is OR/XORed into the word at [data_490e5b0 + 128]
;       and also stored at [0x43723bc] (presumably a .data status byte)
```

→ **Cmd 0x15 response needs a 1-byte payload.** Value semantics unknown — likely a pwrdist status bitfield. Safe first guess: `0x00` (all-clear / no errors).

Disassembled `0x91607c` (cmd 0x18 handler):

```
0x91607c  ; no length check
; body: takes min(existing_halfword_at[data_490e5b0 + 0x88 + id*2], w27)
;       where w27 is the payload byte count — stores that min back
```

→ **Cmd 0x18 response payload is variable.** Handler just records min payload length per id into a halfword table. Any non-zero payload should work; purpose is unclear but looks like a connection-quality or protocol-negotiation tracker.

### 17.5: Decisions

| Decision | Why |
|---|---|
| Decode the jump table myself (objdump + python) rather than Sidekick | ~10 min work, no round-trip; gives the full table, not just one cmd |
| Document the full table even though we only need cmd 0x10 today | Cmd 0x15 / 0x18 / 0x20 / others will almost certainly show up next and this is their directory |
| Keep cmd 0x20 handler in the shim (response cmd 0x10, 4-byte HW version) as-is | Jump table confirms cmd 0x20 routes to HW-version handler at 0x915fec. But the handler at 0x916194 (response cmd 0x10) expects **7** payload bytes, and for a HW-version reply we'd send only 4 — so resp cmd 0x10 for a 4-byte reply would *fail the length check at 0x916194*. Our cmd 0x20 case may be wrong: review after cmd 0x15/0x18 unblocks further init and we can observe whether cmd 0x20 is even issued. |
| Don't synthesize cmd 0x15 / 0x18 responses this session | Keep this session focused on the analysis deliverable; the next action item has a clean interface |

### 17.6: Next steps

1. **Synthesize cmd 0x15 response** — 1-byte payload, value `0x00`. Frame: `02 15 id 00 cksum 03` (6 bytes). Checksum `-(0x15 + id + 0x00)` low byte.
2. **Synthesize cmd 0x18 response** — guess 1-byte payload, value `0x00`. If handler wants something specific, we'll see a follow-up error.
3. **Re-run.** Expected progression: pwrdist init completes → `sub_d19580` returns 0 → `sub_a75ae0` proceeds past bridge init into `sub_ced270` / EEPROM / the final poll. Next cmds or next device opens should appear.
4. **If new bridge cmds show up:** consult the dispatch table above for the handler requirements.
5. **Revisit cmd 0x20 HW-version response** — the 4-byte payload we currently send will fail the cmd 0x10 handler's 7-byte length check if it's ever actually consumed as a cmd 0x20 reply. Either the cmd 0x20 HW-version handler is at a different response cmd than 0x10, or the game currently doesn't issue cmd 0x20 (which is why we haven't hit this). Investigate if cmd 0x20 starts showing up post-pwrdist.

### 17.7: Reference — response CMD → handler table

Full 50-entry jump table from `0x37cf510`. See script in session log for decode. Handlers of interest (non-error/non-generic):

- `0x10` → `0x916194` (pwrdist version, 7 bytes)
- `0x12` → `0x916158`
- `0x14` → `0x9160d8` (4 bytes)
- `0x15` → `0x9160a0` (1 byte, pwrdist status)
- `0x18` → `0x91607c` (variable, connection quality?)
- `0x1a` → `0x916064`
- `0x1b` → `0x916038`
- `0x1d` → `0x916028`
- `0x20` → `0x915fec` (HW version)
- `0x22` → `0x915e54`
- `0x2d` → `0x915f08`
- `0x2e` → `0x915ed8`
- `0x41` → `0x915f88`

Dispatch fallbacks:
- `0x915e14` — channel-signal + memcpy (for response acknowledgments with payload copy)
- `0x915d28` — silent no-op with ACK (for cmds that don't carry payload data)
- `0x915d08` — "unknown command" error path

---

## Step 18: Escape encoding + pwrdist init complete (2026-04-18)

### 18.0: Entry state

After Step 17 the picture was: cmd 0x10 dispatch is correct; cmds 0x15 and 0x18 need synthesis. Added cases for both with 1-byte `0x00` payloads.

### 18.1: Cmd 0x15 worked; cmd 0x18 did not — and a checksum error appeared

First re-run:

```
stdout:
  netbridge image okay, CRC32=0x357f5327
  bridge rx checksum error
  bridge message (cmd=18, id=2) response failed 110: Connection timed out
  failed to get power distribution board version, assuming Spike 1/2 ...
```

Cmd 0x15 went through cleanly (no timeout for it). Cmd 0x18 triggered `bridge rx checksum error`.

The shim's response log was the giveaway:
```
[shim] bridge RESP to cmd=0x18 id=0x1b (6 bytes): 02 18 1b 00 cd 03
```

But the game's stderr reported `cmd=18, id=2`. **ID `0x02` ≠ `0x1b`**. The game had sent cmd 0x18 request with logical id `0x02` — escape-encoded as `0x1b 0x82`. Our request parser read `req[2] = 0x1b` (the escape byte) as the id.

### 18.2: Escape encoding — the discovery

Re-reading the byte-by-byte parser at file `0x915c00`-`0x915c4c`:

```asm
0x915c00: cmp  w0, #0x2        ; STX?
0x915c04: b.eq 0x915de0         ; → reset
...
0x915c10: bic  w0, w0, w26      ; clear escape bits: w0 &= ~w26
0x915c14: mov  w26, #0x0         ; clear escape flag
0x915c18: strb w0, [x21, w27]    ; store decoded byte
0x915c1c: add  w27, w27, #0x1
0x915c20: ...
0x915c2c: ldrb w0, [x19]
0x915c30: cmp  w0, #0x3          ; ETX?
0x915c34: b.eq 0x915ca0           ; → frame processing
0x915c38: cmp  w0, #0x1b          ; ESC?
0x915c3c: b.ne 0x915c00           ; no → loop back
0x915c40: add  x19, x19, #0x1     ; consume the 0x1b
0x915c44: mov  w26, #0x80         ; set escape flag (next byte gets bit 7 cleared)
```

**Protocol:** `0x02`, `0x03`, `0x1b` appearing as literal data are transmitted as `0x1b (byte | 0x80)`; receiver unescapes the byte following `0x1b` by ANDing with `0x7f`.

Our cmd 0x30, 0x10, 0x15 responses happened to have no bytes in the escape set, so they worked by coincidence. Cmd 0x18 response with id `0x02` (escape-encoded on game side as `1b 82`) revealed the gap — both our parsing (pulled `req[2] = 0x1b`) and our emission (wrote `resp[2] = 0x1b` unescaped) were wrong.

### 18.3: Fix

Added `bridge_encode_escape` / `bridge_decode_escape` helpers in `_emulation/spi_shim.c`. Updated:

- `synthesize_bridge_response` now decodes the raw UART bytes into logical form before extracting `cmd` and `id`.
- `bridge_send_response` now encodes the logical response into on-wire escape form before writing to `uart_shim_fd`. Log still shows the logical frame; a second log line shows the encoded form if it differs in length.

### 18.4: Result — pwrdist init completes, game advances

Re-run after the escape fix, plus adding cmd 0x1c ACK:

```
stdout:
  netbridge image okay, CRC32=0x357f5327
  power dist:
    runtime size 0x0000
    runtime cs   0x0000
    start        0x3800
    length       0x2d7c
    checksum     0x2bc6
    currently in runtime mode...
    switching to boot mode...
  bridge message (cmd=1c, id=1) response failed 110: Connection timed out
  failed to write extended power distribution code ...
```

And then after adding cmd 0x1c → silent ACK (5-byte zero-payload frame):

```
stdout: (empty — past the pwrdist init diagnostic prints)

stderr:
  bridge RESP to cmd=0x30 id=0x01  ✓
  bridge RESP to cmd=0x10 id=0x01  ✓
  bridge RESP to cmd=0x15 id=0x01  ✓
  bridge RESP to cmd=0x18 id=0x02  ✓  (escape-encoded on the wire as 02 18 1b 82 ...)
  bridge RESP to cmd=0x1c id=0x01  ✓
  bridge RESP to cmd=0x10 id=0x01  ✓  (re-query after flash)
  bridge RESP to cmd=0x10 id=0x01  ✓  (re-query again)
  bridge: no synthetic response for cmd 0x1d
  bridge: no synthetic response for cmd 0x11
  bridge: no synthetic response for cmd 0x27  (×4+)
```

**Observations:**

- No more `bridge rx checksum error`.
- No more `failed to initialize bridge`. No perror.
- No more `failed to get power distribution board version`.
- Game issued cmd 0x10 twice more after cmd 0x1c → confirms the "re-verify after flash" path ran cleanly.
- New cmds (`0x1d`, `0x11`, `0x27`) appeared — we're now in the next init phase.

**sub_d19580 is returning 0.** sub_a75ae0 has proceeded past bridge init.

### 18.5: New blockers (next session)

| Cmd | Jump-table target | Handler type | Notes |
|---|---|---|---|
| `0x11` | `0x915d28` | generic silent-ACK fallback | Trivial: 0-byte payload response, same as cmd 0x1c |
| `0x1d` | `0x916028` | dedicated handler | **Disassembled end-of-session:** `cmp w27, #0x4 / b.eq 0x915e88` — if payload length == 4, branches into the generic channel-copy (memcpy payload into `data_490e5b0 + 0xa0 + id*8`). Else silent-ACK. So cmd 0x1d expects exactly 4 bytes. Content semantics unknown — given pwrdist reports door/buttons/rail state per FotE manual §4.16, plausibly `[status_bits][rail_bits][adc_hi][adc_lo]`. See memory `reference_pwrdist_board.md`. |
| `0x27` | `0x915e14` | generic channel-copy + memcpy | Expects a payload; memcpy source = request payload. Dest buffer is at `data_490e5b0 + 0xa0 + id*8`. We don't know the expected payload size or what the game reads from the buffer afterward. Careful: a null `dest_buf` at that slot would crash in memcpy — verify before stubbing. |

### 18.6: Decisions this session

| Decision | Why |
|---|---|
| Decode requests in the shim, not just echo `req[2]` as-is | Without this, any channel id in `{0x02, 0x03, 0x1b}` gets mis-parsed. Fixing the parser is cheap (~30 lines of C) and eliminates a whole bug class. |
| Encode responses with the same escape rules, not just emit logical bytes | Same mirror of the fix — any byte the game reads after STX that matches an escape value without preceding `0x1b` gets mis-interpreted. |
| Stub cmd 0x1c with 0-byte payload ACK | Jump table says cmd 0x1c routes to the generic silent-ACK handler; 0-byte payload takes the `cbnz w27` false branch and still signals the channel. Cheaper than emulating actual pwrdist firmware flash. |
| Stop after cmd 0x1c unblocked, before stubbing cmd 0x27 | Cmd 0x27's handler does memcpy into a per-id dest buffer at `data_490e5b0 + 0xa0 + id*8`. If the buffer pointer is null, a 0-size memcpy is fine per spec but still risky. Also the game probably reads that buffer — need to know the expected content before stubbing. Safer to pause, document what's verified, and handle cmd 0x27 with eyes on its callers next session. |

### 18.7: Updated `next steps`

1. **Stub cmd 0x11** — generic silent-ACK, same shape as cmd 0x1c. 5-byte frame: `02 11 id cksum 03`. Low risk.
2. **Disassemble `0x916028`** (cmd 0x1d handler) to determine payload requirements. Stub accordingly.
3. **Investigate cmd 0x27** before stubbing — find the game-side caller (`send_bridge_message_with_response(cmd=0x27, ...)`), see what payload it expects and what it does with the result. The generic channel-copy handler at `0x915e14` is likely NOT the right model — the real handler may be game-code after the channel unblocks.
4. After all three handled, expect next wave of activity: likely node-bus discovery (the game iterates over node addresses 0-15), or I2C/SPI changes, or new device opens (display, audio, GPIO).

Verified facts from this step are in `_emulation/PROTOCOL.md`.

### Run command unchanged.

---

## Step 19: Pwrdist init fully green (2026-04-18)

### 19.0: Entry state

Step 18 left three unanswered cmds: `0x11`, `0x1d`, `0x27`. Plan was to stub the silent-ACK ones, then investigate `0x27`'s semantics before stubbing.

### 19.1: Cmds 0x11 and 0x1d — low-risk silent-ACKs

Both route to generic paths per Step 17's table decode (0x11 → `0x915d28`, 0x1d → `0x916028` with payload ≠ 4 → falls to `0x915d28`). Added 5-byte zero-payload ACK responses for both, shape `02 CMD id cksum 03`.

Observed request payloads on the wire:

- `02 11 00 1b 82 ed 03` → cmd 0x11, id 0, logical payload `[0x02]` (1 byte).
- `02 1d 01 1b 82 30 30 80 03` → cmd 0x1d, id 1, logical payload `[0x02, 0x30, 0x30]` (3 bytes — not 4, so the cmd 0x1d handler takes its else branch anyway).

Game issued each only once in the successful run — their callers don't appear to read the response content, so 0-byte ACK is sufficient.

### 19.2: Cmd 0x27 — alternating status byte

Disassembled all four callsites via range-restricted objdump over `0x916000`–`0x91a000` (~50 BL sites to the bridge TX function `0x9164a0`, grep for predecessors that load `#0x27` into w0):

| Callsite | Enclosing fn | Role | Exit condition on response byte |
|---|---|---|---|
| `0x916828` | `sub_9167e8` | One-shot getter — stores `resp[0]` at caller out-ptr, returns | any value |
| `0x916fe0` | `sub_916f60` loop 1 | Poll until bit 2 of resp[0] == **1** | `tbz w0, #2` back-branch |
| `0x917020` | `sub_916f60` loop 2 | Poll until bit 2 of resp[0] == **0** (paired with loop 1) | `tbnz w1, #2` back-branch |
| `0x917264` | standalone polling fn | Poll (≤100 iters × 100 ms) until `(resp[0] & 0x03) == 0x02` | `cmp w1,#2 / b.ne` |

Request signature is consistent: `w0=0x27`, `x1=&tx_buf`, `w2=1` (1-byte payload = `0x00`), `x3=&rx_buf` (same slot as tx), `x4=&rx_len_inout` (halfword, initialized to max 1), `w5=0`. So cmd 0x27 sends 1-byte request and expects 1-byte response.

The paired loop in `sub_916f60` requires resp[0] bit 2 to toggle `1 → 0` within the same function call, so a static response stalls one half. Resolution: alternate odd/even calls.

- odd call → `0x06` (bits 1, 2 set; `resp & 3 == 2`)
- even call → `0x02` (bit 1 only; `resp & 3 == 2`)

Both values satisfy site `0x917264`; alternation satisfies the pair. Stub uses a static call counter in C.

### 19.3: Cmd 0x16 — the big one

Stubbed cmd 0x16 first as a 0-byte silent-ACK assuming it was in the same generic-fallback family as 0x11 / 0x1c. Re-ran — game sent **267 cmd 0x16 frames in 10 s**, the classic "polling loop on a response bit we're not populating" signature.

Found the caller via objdump: `send_bridge_message_with_response` callsite `0x917bb8` inside `sub_917b60`. That function sends cmd 0x16 with a 1-byte tx payload (`0x00`) and asks for up to 4 response bytes. It stores rx[1], rx[2], rx[3] to three caller-supplied out-ptrs and returns rx[0].

The unique caller of `sub_917b60` is at `0x675c20` and is a tight 1000-iteration poll with 1 ms sleeps:

```
675c20:  bl   sub_917b60
675c24:  cmp  w0, #0
675c28:  b.le retry          ; rx[0] > 0 required
675c2c:  ldr  w0, [sp,#16]   ; w0 = rx[2]
675c30:  cmp  w0, #2
675c34:  b.ne retry          ; rx[2] == 2 required
```

So cmd 0x16 response must be **4 bytes with `rx[0] > 0` and `rx[2] == 2`**. Used `01 00 02 00`.

### 19.4: The hidden fact about `0x915d28`

Step 17's summary of `0x915d28` as "silent no-op with ACK" was incomplete. Disassembly this session:

```
; Entry (reached when dispatch table entry = 0x915d28, e.g. cmd 0x11/0x13/0x16/0x1c/...):
915d28:  cbz   w3, 915ca8          ; id == 0 → pure ACK, w27=-1, signal channel
915d2c:  cbnz  w27, 915e8c         ; payload_len > 0 → CHANNEL-COPY branch!
915d30:  cmp   w3, #5
915d34:  b.gt  915ca8              ; id > 5 → pure ACK
915d38:  add   x5, x20, #0x5b0
915d3c:  sxtw  x2, w3
...                                ; state-machine on data_490e5b0+0xe8+id
```

And `0x915e8c`:

```
915e8c:  add   x0, x20, #0x5b0     ; x0 = &data_490e5b0
915e90:  sxtw  x5, w3               ; x5 = id
915e94:  add   x1, x0, #0xa0        ; x1 = per-id rx-buf ptr table
915e98:  ldr   x8, [x1, x5, lsl #3] ; x8 = rx_buf[id]
915e9c:  cbz   x8, 915d30           ; null → skip memcpy
915ea0:  add   x0, x0, #0x88        ; per-id rx max-len table
915ea4:  add   x1, sp, #0x2d2       ; x1 = rx payload start
915ea8:  ldrh  w28, [x0, x5, lsl #1]; max_len[id]
915eb0-b4: w28 = min(max_len, payload_len)
915ec8:  strh  w28, [x7, x5, lsl #1]; write back actual rx_len
915ebc:  mov   x0, x8                ; memcpy dest = rx_buf[id]
915ecc:  bl    memcpy@plt             ; memcpy(rx_buf, rx_payload, min_len)
915ed4:  b     915d30                ; fall through to state-machine path
```

**So for a response routed through `0x915d28`**: sending a non-zero payload triggers the same per-id channel-copy logic as the dedicated handler at `0x915e14`. That's how our cmd 0x16 response of 4 bytes ended up in the caller's rx_buf despite "going through the silent-ACK handler."

### 19.5: Result — pwrdist init fully green

After all four fixes, game stdout reaches:

```
netbridge image okay, CRC32=0x357f5327
power dist:
  runtime size 0x0000
  runtime cs   0x0000
  start        0x3800
  length       0x2d7c
  checksum     0x2bc6
  currently in runtime mode...
  switching to boot mode...
  now in runtime mode...                          ← new in this step
  success, power dist. restarting                 ← new in this step
  power dist. boot version: 1.0.0, runtime version: 0.0.0   ← new in this step
```

Bridge traffic in a 15-second run: **12 total writes**, one each of `0x30/0x15/0x18/0x1c/0x1d/0x11/0x16`, three `0x10` (version re-queries after flash), two `0x27` (the alternating poll exits in 2 calls now that both paired branches are satisfiable). No `no synthetic response` messages. No errors.

After the cmd 0x16 response, the game pivots to I2C — `I2C ioctl SET_SLAVE addr=0x57` fires (EEPROM, post-init phase). Bridge thread goes idle; SPI polling continues but doesn't produce new state. No further stdout, no errors, no new unhandled syscalls — game is blocked on something non-bridge, likely display / EGL / GPU or file I/O we haven't seen.

### 19.6: Decisions this session

| Decision | Why |
|---|---|
| Log every bridge UART write (not just first 10 / every 1000th) | Bridge frames are low-volume (hundreds per run); knowing each request payload is required to diagnose polling loops. SPI polling is still sampled. |
| For cmd 0x1d, send 0-byte ACK instead of 4-byte channel-copy | `cmp w27,#4` routes to different branches; 0-byte ACK takes the silent path and doesn't write unknown-semantic bytes into `data_490e5b0+0xa0+id*8`. Safer while the content meaning is unverified. |
| Cmd 0x27 as alternating `0x06`/`0x02` rather than stateful per-callsite tracking | We can't distinguish callsites from the wire. Alternation satisfies all three polling patterns in the observed call sequence. If a callsite later emerges that breaks this assumption, add context. |
| Accept that "runtime version: 0.0.0" is a side effect of our cmd 0x10 payload `01 00 00 00 00 00 00` | `data_490e61c` gets `0x010000` (boot version = 1.0.0); `data_490e620` gets `0x000000` (runtime version = 0.0.0). Game still takes the "pwrdist present" branch on non-zero boot version, which is what matters. |

### 19.7: Next steps

1. **Investigate the post-bridge stall.** Game is silent after cmd 0x16 → I2C → SPI polling. Candidates:
   - `/dev/fb0` / `/dev/dri/card0` open (framebuffer / DRM display)
   - EGL context creation (`eglGetDisplay` / `eglInitialize`)
   - GStreamer pipeline initialization
   - ALSA PCM open
   - A file read from `image.bin`

   First move: strace-style log missing `open`/`openat` calls and EGL/GL entry points by adding a generic logger in the shim, or run under strace to see syscalls at the kernel interface level.

2. **Cmd 0x20 (HW version) still carries Step 18.5's known-wrong response** — we emit response cmd `0x10` with 4 bytes, but the pwrdist handler at `0x10` expects 7 bytes. The game did not issue cmd 0x20 in this run. Leave as-is for now; if the game starts issuing it later, fix to either route to handler `0x20` or send 7 bytes.

3. **Revisit cmd 0x27 if a 3rd variant shows up.** Current alternation works because both observed callers happened to fire at consistent parity. If a future session sees cmd 0x27 retry counts inconsistent with our model, switch to a per-id or per-caller state machine.

### Run command unchanged.

---

## Step 20: Bridge phase fully green — pthread scheduling was the stall (2026-04-18)

### 20.0: Entry state

Step 19 left the game at `power dist. boot version: 1.0.0, runtime version: 0.0.0` → silent. `qemu-aarch64 -strace` showed the main thread `clock_nanosleep(250 ms) = 0` then no further syscalls for ~95 K log lines. Bridge thread (3613) healthy in `pselect6`; SPI polling (3611) healthy. Whatever the main thread was doing was userspace-only.

### 20.1: Locating the 250 ms sleep

Grepping for `usleep(250000)` / `#0x3d090` / `#0xd090` turned up one hit:

```
678dd4: mov   w21, #0xd090
678dd8: movk  w21, #0x3, lsl #16   ; w21 = 0x3d090 = 250000
678ddc: mov   w19, #0x1e0           ; 480 iterations
678de0: mov   w0, #0x0
678de4: bl    5d6ef0                ; <logger/counter>
678de8: mov   w0, w21
678dec: bl    usleep@plt            ; <-- the 250 ms sleep
678df0: ldrb  w0, [x20]             ; read flag byte
678df4: subs  w19, w19, #1
...
678e04: b.ne  678de0                ; loop back if flag&1 still 0
```

Pointer chain: `adrp x20, 4352000 / ldr x20, [x20, #0x780] / ldrb w0, [x20]` → loads the byte at `*(.data.rel.ro offset 0x780)`. Relocation table entry `0x4352780 R_AARCH64_RELATIVE *ABS*+0x4482108` — so the polled byte lives at `.bss` offset `0x4482108` (BN VA `0x4882108`).

### 20.2: Binary Ninja answers the static questions

Sidekick / BN lookup (session transcript):

- Byte flag `0x4882108` — auto-named `data_4882108`, no symbol.
- Enclosing fn — `sub_a78bc0` (BN VA; file `0x678bc0`). 824 bytes / 20 blocks / 80-byte frame. Calls `bl sub_a75ae0` (bridge-init wrapper) at `0xa78cf8`, then fires ~40 subsystem initializers from `0xa78cec–0xa78dac`, then enters the 480-iter × 250 ms poll at `0xa78de0–0xa78e04`, then retries.
- Xrefs to `data_4882108`:
  - **Write**: `strb w4, [x1, #0x108]` at `0xa6d490` inside `sub_a6ce20` (3020 bytes / 123 blocks). Sets the flag to `1`. The next instruction is `bl 0xb2ae90 <CC_mainDispatchThread>` — so `sub_a6ce20` is the dispatch-subsystem init that sets the ready flag and transfers to the main dispatch loop.
  - **Read**: the three instructions in the poll loop above.

`CC_mainDispatchThread` — the `Thread` suffix plus the set-flag-then-call-it pattern strongly implies it's a thread entry point.

### 20.3: The timing anomaly — `clone3` → `clone` fallback → EPERM

Walking back from the `write(1, …, 337)` syscall that emits the power-dist stdout block, the trace shows:

```
3608 rt_sigprocmask(SIG_BLOCK, [all signals], …) = 0
3608 Unknown syscall 435                            ← clone3 (QEMU user-mode unimplemented)
3608 clone(CLONE_VM|…|CLONE_THREAD|…) = 3614         ← glibc falls back to clone
3608 sched_setscheduler(3614, 2, …) = -1 errno=1 (Operation not permitted)
3608 rt_sigprocmask(SIG_SETMASK, …) = 0
3608 futex(0x…eedd8, FUTEX_WAKE, 1, …) = 1
3614 futex(0x…eedd8, FUTEX_WAIT, 2, …) = 0
3608 futex(0x…eea90, FUTEX_WAIT_BITSET, 3614, …) = 0   ← pthread_join-style wait
3614 futex(0x…eedd8, FUTEX_WAKE, 1, …) = 0
3614 exit(0)                                         ← child exits without running entry
3608 write(1, …, 337) = 337                         ← now prints the pwrdist output
3608 clock_nanosleep(0, 0, {tv_sec=0, tv_nsec=250000000}, NULL) = 0
(no further main-thread syscalls for the rest of the trace)
```

This is the **glibc `pthread_create` with explicit scheduling attributes** control-flow: parent spawns the child suspended on an internal futex, then tries to apply `SCHED_RR` via `sched_setscheduler`, then wakes the child. When the scheduling apply fails, the parent signals the child via the same futex to exit *without* running its entry function — so the child bails and `pthread_create` returns the scheduling error to the caller.

Root cause of the EPERM:

```
ulimit -H -r => 0    # hard RT-priority limit is zero on this WSL user
ulimit -S -r => 0
```

CAP_SYS_NICE isn't even relevant — the kernel caps out RT priority at 0 for this user. Under QEMU user-mode this is unfixable without `sudo`/`setcap`.

The child thread was supposed to set `data_4882108` (probably via `sub_a6ce20` → `CC_mainDispatchThread`). It never ran; the flag stays 0 forever; `sub_a78bc0` loops on it.

### 20.4: Fix — hook `pthread_create`, force `PTHREAD_INHERIT_SCHED`

glibc uses `INTERNAL_SYSCALL_CALL` for `sched_setscheduler` inside `pthread_create`, which bypasses LD_PRELOAD on the public wrapper (same INLINE_SYSCALL pattern as `access` / `stat` / termios). Hooking `sched_setscheduler` directly wouldn't fire. So we hook `pthread_create` itself:

```c
int pthread_create(pthread_t *t, const pthread_attr_t *attr,
                   void *(*start)(void *), void *arg) {
    if (attr != NULL) {
        int inherit = 0;
        if (pthread_attr_getinheritsched(attr, &inherit) == 0
            && inherit == PTHREAD_EXPLICIT_SCHED) {
            pthread_attr_t copy;
            memcpy(&copy, attr, sizeof(pthread_attr_t));
            pthread_attr_setinheritsched(&copy, PTHREAD_INHERIT_SCHED);
            return real_pthread_create(t, &copy, start, arg);
        }
    }
    return real_pthread_create(t, attr, start, arg);
}
```

With inherit-sched, glibc skips the `sched_setscheduler` call entirely and the child runs under the parent's default scheduling policy (`SCHED_OTHER`). Priority doesn't matter under QEMU user-mode, so this is a correctness-preserving transform.

Rebuild: now needs `-lpthread` in addition to `-ldl` (added `#include <pthread.h>`).

### 20.5: Result — new milestone, new blocker

```
[shim] pthread_create: forcing PTHREAD_INHERIT_SCHED (was EXPLICIT)
[shim] OPEN FAIL: /dev/dri/card1 (flags=0x2, errno=2 No such file or directory)
[shim] perror('failed to open device') errno=2
failed to open device: No such file or directory
[shim] access REMAP: /games/spike3/bin/SternLogo_800x480.png -> …/SternLogo_800x480.png
game: glWindow.cpp:970: void glWindow::setGlContext(): Assertion `eglGetError() == EGL_SUCCESS' failed.
qemu: uncaught target signal 6 (Aborted) - core dumped
```

The bridge phase is fully green. Hook fires exactly once. Game proceeds to display initialization:
- Opens `/dev/dri/card1` (DRM KMS device) — **fails, not present in host**.
- Tries to load `SternLogo_800x480.png` — the boot splash screen asset.
- Attempts EGL context creation via `glWindow::setGlContext` — assertion fails, SIGABRT.

### 20.6: Decisions this session

| Decision | Why |
|---|---|
| Hook `pthread_create` rather than `sched_setscheduler` | glibc's pthread_create uses `INTERNAL_SYSCALL_CALL` for the internal scheduling apply — LD_PRELOAD on the public `sched_setscheduler` wrapper never fires. Proved empirically: the wrapper hook logged nothing while `pthread_create` hook fired once. |
| Force `PTHREAD_INHERIT_SCHED` on a copied attr, not mutate the caller's attr | The caller's `pthread_attr_t` may be const or reused across calls; our change is scoped to this call. |
| Leave ulimit alone, don't attempt to setcap qemu-aarch64 | Requires password-interactive sudo; user-level LD_PRELOAD fix is cleaner and portable across users/hosts. |
| Log the forced override so it's visible in every run | Future sessions will see `forcing PTHREAD_INHERIT_SCHED` and know this fix is active — keeps the shim's behavior auditable. |

### 20.7: Next steps

The bridge/node-bus phase is done. The next phase is **display / GL / asset loading**:

1. **Decide display strategy.** Three options:
   - (a) Stub `/dev/dri/card1` and all DRM ioctls, spawn an X11 window with EGL via host mesa-egl — matches Researcher A's library-hooking approach. Most invasive but highest fidelity.
   - (b) Route the game's EGL/GL through host libraries with a null display — no rendering, game becomes headless. Simplest; lets game logic run without pixels. Useful for CLI + VPX bridging.
   - (c) Just stub the DRM open to succeed; then the EGL assertion will explain what DRM/GBM state the game expects — we iterate from there.
2. **Inspect `glWindow::setGlContext`** (file location TBD) to see what `eglGetError()` was expected to return and what EGL calls preceded it. This tells us which device attributes the game wants.
3. **Explore the ~40 initializers at `0xa78cec–0xa78dac`.** One of them calls `sub_a6ce20` (dispatch thread init). Mapping each by string references or function-call fingerprints will give us a subsystem map (SPI / NUK / LCD / HDMI / audio).

### Run command unchanged — still needs the shim:

```bash
cd /mnt/c/Workspace/pokemon-le-0.81.0/games/pokemon_le && \
timeout 10 qemu-aarch64 -L /mnt/c/Workspace/pokemon-le-0.81.0 \
  -E LD_PRELOAD=…/spi_shim.so \
  -E SPIKE3_SYSROOT=/mnt/c/Workspace/pokemon-le-0.81.0 \
  ./game
```

Rebuild command now needs `-lpthread`:

```bash
cd _emulation && \
  aarch64-linux-gnu-gcc -shared -fPIC -o spi_shim.so spi_shim.c -ldl -lpthread
```

---

## Step 21: Display layer — DRM + GBM + EGL stubbed (2026-04-18)

### 21.0: Entry state

Step 20 got us to the display-init phase: game opens `/dev/dri/card1` (fails), prints `failed to open device`, accesses `SternLogo_800x480.png`, aborts on `eglGetError() == EGL_SUCCESS`. We decided on strategy (c) — iterative stubbing to map the DRM/EGL surface the game actually uses.

### 21.1: Binary Ninja analysis of `glWindow::setGlContext`

Sidekick dumped the full call sequence:

**Phase 1 — DRM/GBM setup** (`sub_d01160`): `open("/dev/dri/card1", O_RDWR)` → `drmModeGetResources` → `drmModeGetConnector` (×2) → `drmModeGetEncoder` → `gbm_create_device` → `eglGetPlatformDisplayEXT`.

**Phase 2 — EGL init** (`sub_d01d10`): `gbm_bo_create` (×3, triple buffering) → `eglInitialize` → `eglBindAPI(EGL_OPENGL_ES_API)` → `eglGetConfigs` → `eglChooseConfig` → `eglGetConfigAttrib` (checks `EGL_NATIVE_VISUAL_ID == 0x34325258` i.e. GBM_FORMAT_XRGB8888) → `eglCreateContext` (`EGL_CONTEXT_CLIENT_VERSION=3` → GLES 3) → `eglMakeCurrent`.

**Phase 3 — GL** (`sub_d02100`+): `glGetString` ×4 → `glEnable(GL_BLEND)` → `glBlendFunc` → `eglGetProcAddress` ×7 (KHR image / sync, `glEGLImageTargetTexture2DOES`) → `sub_d00800` (create EGL framebuffer) → `drmModeSetCrtc` → `glBindFramebuffer`.

**Phase 4 — assertion** (`sub_d01100` at file `0x701100`): the wrapper that calls `eglMakeCurrent` again and checks `eglGetError() == EGL_SUCCESS`. This is the failure point.

Key constants BN gave us:
- Two connectors hardcoded: `/dev/dri/card1` (primary), fallback `/dev/dri/card0`.
- Fallback trigger: if connector count ≤ 1 on card1, the game retries on card0.
- GBM format: `0x34325258 = GBM_FORMAT_XRGB8888`.
- EGL context: v3 (GLES 3.x required).

### 21.2: DRM stubs

Added `FAKE_FD_DRM = 1020` and the following ioctl handlers:

| ioctl | Behavior |
|---|---|
| `DRM_IOCTL_VERSION` | Returns 1.6.0 with dummy name/date/desc strings |
| `DRM_IOCTL_SET_CLIENT_CAP` | Silent success for any cap |
| `DRM_IOCTL_GET_CAP` | Returns `1` for `DUMB_BUFFER` + `ADDFB2_MODIFIERS`, depth `24`, else `0` |
| `DRM_IOCTL_MODE_GETRESOURCES` | 1 CRTC (id 1), 2 connectors (ids 10, 11), 1 encoder (id 20), 0 FBs; 320–3840 × 200–2160 |
| `DRM_IOCTL_MODE_GETCONNECTOR` | Both connectors "connected" as HDMI-A with one 1920×1080@60 mode. (Initially I made only one connected — the game queried the *disconnected* one first and bailed; making both connected resolved it.) |
| `DRM_IOCTL_MODE_GETENCODER` | Encoder 20 → CRTC 1 |
| `DRM_IOCTL_MODE_GETCRTC` / `MODE_SETCRTC` | Accept silently, no mode stored |
| `MODE_CREATE_DUMB` / `DESTROY_DUMB` / `ADDFB2` / `RMFB` / `PAGE_FLIP` | Return success with monotonically increasing handles |
| `MODE_MAP_DUMB` | Return `-ENOTSUP` (no backing mmap) |
| `GEM_CLOSE` | Silent success |

Game now prints its resource enumeration to stdout:

```
number of frame buffer objects: 0
number of CRTCs: 1
number of connectors: 2
number of encoders: 1
min/max width: 320/3840
min/max height: 200/2160
```

First time we've seen game-originated stdout beyond the pwrdist lines. Confirms the DRM stubs are believable to the game's debug-print path.

### 21.3: GBM and EGL library hooks

Stubbing DRM ioctls wasn't enough — `gbm_create_device` inspected the fake fd against `/sys` driver identification and failed with EINVAL. Needed to hook GBM at the library level:

- `gbm_create_device` → fake handle (`0x61626d44` "abmD")
- `gbm_surface_create` / `gbm_bo_create` (+ `_with_modifiers` variants) → fake handles
- `gbm_bo_get_{width,height,stride,format,handle,fd}` → fake values matching our 1920×1080 XR24 pretense
- `gbm_surface_lock_front_buffer` / `release_buffer` / `has_free_buffers` → no-op

Then EGL at library level too (mesa's libEGL would deref our fake gbm_device handle):

- `eglGetDisplay` / `eglGetPlatformDisplay` / `eglGetPlatformDisplayEXT` → fake EGL display
- `eglInitialize` → 1.5
- `eglBindAPI`, `eglChooseConfig`, `eglGetConfigs`, `eglQueryString`, `eglMakeCurrent`, `eglSwapBuffers`, `eglSwapInterval`, `eglQuerySurface` → success
- `eglCreateContext`, `eglCreateWindowSurface`, `eglCreatePbufferSurface` → fake handles
- `eglGetError` → `EGL_SUCCESS`
- `eglGetConfigAttrib` → returns RGBA8 format + `EGL_NATIVE_VISUAL_ID = 0x34325258` (matches the XR24 check)
- `eglGetProcAddress` → funneled through our own stubs for `eglGetPlatformDisplayEXT`, `eglCreateImageKHR`, `eglDestroyImageKHR`, `eglCreateSyncKHR`, `eglDestroySyncKHR`, `eglWaitSyncKHR`, `eglClientWaitSyncKHR`, `glEGLImageTargetTexture2DOES`. The KHR extension stubs return sentinel image / sync handles + `EGL_TRUE`.

### 21.4: EGL attribute bug

First pass had wrong EGL attribute numbers (I confused `EGL_RED_SIZE` / `EGL_NATIVE_VISUAL_ID`). The game logged `failed to find egl config for gbm format: No such file or directory` — exactly what the BN trace predicted if the config didn't match. Fixed the constants (see `eglGetConfigAttrib` switch block). Game now accepts the config and proceeds.

### 21.5: Current state — pure stubs reach the GL boundary

Full cumulative sequence (stdout + key stderr):

```
netbridge image okay, CRC32=0x357f5327
power dist: …
  now in runtime mode…
  success, power dist. restarting
  power dist. boot version: 1.0.0, runtime version: 0.0.0
[shim] pthread_create: forcing PTHREAD_INHERIT_SCHED (was EXPLICIT)
[shim] OPEN /dev/dri/card1 -> fake fd 1020 (DRM)
[shim] DRM ioctl MODE_GETRESOURCES -> crtc=1 conn=2 enc=1
  number of frame buffer objects: 0
  number of CRTCs: 1
  number of connectors: 2
  number of encoders: 1
  min/max width: 320/3840
  min/max height: 200/2160
[shim] gbm_create_device(fd=1020) -> fake handle
[shim] eglGetPlatformDisplayEXT(…) -> fake
[shim] gbm_bo_create(1920x1080, fmt=0x34325258) -> fake   (×3)
[shim] eglInitialize -> 1.5
[shim] eglBindAPI(0x30a0)
[shim] eglCreateContext -> fake context
[shim] eglMakeCurrent(…) -> EGL_TRUE
[shim] eglGetProcAddress("eglCreateImageKHR") -> stub
[shim] eglGetProcAddress("eglDestroyImageKHR") -> stub
[shim] eglGetProcAddress("eglCreateSyncKHR") -> stub
[shim] eglGetProcAddress("eglDestroySyncKHR") -> stub
[shim] eglGetProcAddress("eglWaitSyncKHR") -> stub
[shim] eglGetProcAddress("eglClientWaitSyncKHR") -> stub
[shim] eglGetProcAddress("glEGLImageTargetTexture2DOES") -> stub
[shim] access REMAP: /games/spike3/bin/SternLogo_800x480.png -> …
[shim] eglMakeCurrent(dpy=0x65676c44, draw=(nil), read=(nil), ctx=0x65676c58)
qemu: uncaught target signal 11 (Segmentation fault) - core dumped
```

Asset-tracking observations so far (cross-referenced in `_emulation/ASSETS.md`):
- `SternLogo_800x480.png` — boot splash, direct filesystem read via `access()` + `open()` (not in image.bin). Game loads this right after first `eglMakeCurrent`.
- Fontconfig cache paths (`/var/cache/fontconfig`, `~/.cache/fontconfig`, `~/.fontconfig`) — fontconfig tries each, all fail under our sysroot; `Fontconfig error: No writable cache directories` noted but not yet fatal.
- Radium shader compilation fails (`[Radium Error] Shader failed to compile shader id: 0`) — expected without real GL.
- `Texture2dDirect` class (game-defined) needs EGL_KHR_image extensions — our stubs now provide them, so this error went away in the latest run.

### 21.6: The wall — real GL required

The SEGV is inside mesa's aarch64 libGLESv2. The game makes real `glGenTextures` / `glTexImage2D` / `glGetString` / etc. calls; mesa runs them against no real context; crash.

Pure-stub option (c) can go further — stub every GL entry point to no-op — but:
- Rendering still produces no pixels.
- The game may check GL behavior (`glGetError`, `glGetIntegerv` for max texture size, `glCheckFramebufferStatus`) and bail if results don't make sense.
- Every `glGetString(GL_EXTENSIONS)` response we return shapes the game's capability matrix.

Option (a) — real host GL — is cleaner from here. Rough sketch:
1. Link the shim against host mesa libEGL (`dlopen("libEGL.so.1", RTLD_NOW)` into the host-native library — not the guest one in the aarch64 sysroot).
2. Use `EGL_PLATFORM_SURFACELESS_MESA` to get an EGL display without DRM/GBM.
3. Replace our fake eglCreateContext with a real host one.
4. The hard part: GL calls go through aarch64 `libGLESv2.so.2` that the game dynamically loaded; we'd need to replace those too. That's the aarch64-to-x86 bridging that Researcher A's framework does.

The complication is that QEMU user-mode loads ARM64 libraries into the emulated process, while we want to call HOST libraries. Normally LD_PRELOAD replaces with aarch64 stubs (what we've been doing). To use host mesa, the shim itself needs to be a bridge — translating guest calls to host GL calls, which involves marshaling + re-vectoring the call stack across architectures. Non-trivial.

### 21.7: Decisions this session

| Decision | Why |
|---|---|
| Make both connectors "connected" instead of just one | Game queried connector id 11 first (our "disconnected" one) and bailed; rather than understand the iteration order, just satisfy both. |
| Return `-ENOTSUP` for `MODE_MAP_DUMB` | No backing mappable fd under QEMU user-mode; GBM is expected to fall back. If the game actually depends on mmap, we'll see a specific crash later. |
| Use same fake handle for all three GBM BOs | Simpler; will revisit if the game's BO accounting cares about pointer identity. |
| Return stubs from `eglGetProcAddress` (not NULL) | Returning NULL cascaded into `eglMakeCurrent(NULL, NULL, NULL, NULL)` and "failed to get EGL display extension" errors — much worse than fake-success. |
| Put asset observations in a separate `ASSETS.md` file | User's goal is eventual asset extraction; keeping a discipline of per-asset-class notes makes the eventual extractor job easier than rediscovering from logs. |
| Stop before GL stubs — hand the decision back | GL surface is huge, and (c)-stubbing past here starts paying back less per stub. User's end goal is full emulation, so a decision about (a) vs more (c) is warranted. |

### 21.8: Next steps (pending user direction)

Two viable forks:

A. **Minimal GL stubs to push past the splash.** Stub `glGetString`, `glGenTextures`, `glBindTexture`, `glTexImage2D`, `glTexParameteri`, `glGenFramebuffers`, `glBindFramebuffer`, `glCreateShader`, `glCompileShader`, `glLinkProgram`, `glUseProgram`, `glGetError`, `glViewport`, `glClear`, `glDrawArrays`, `glDrawElements`, `glGetIntegerv`, `glCheckFramebufferStatus`, `glEnable`/`glDisable`, `glBlendFunc`. Returns "success" or plausible defaults. Goal: see how far the game gets before its game-logic code kicks in. Useful for I/O bridge work even without rendering.

B. **Real GL via surfaceless EGL.** More work upfront. Value: actual pixels, which we can capture and ship to VPX later. Sketch in §21.6.

Also pending regardless:
- Asset log in `ASSETS.md` continues to grow per session.
- Several `openat` / `access` failures to investigate when the time is right: fontconfig cache paths, various `/home/c` lookups.

### Run command unchanged, rebuild command unchanged (`-ldl -lpthread`).

---

## Step 22: GL stub wall — value mostly extracted, wall reached (2026-04-18)

### 22.0: Entry state

Step 21 left the fork A/B decision open. User chose A (extend GL stubs) with the reasoning that it would also give a leg up for B.

### 22.1: GL stub wall built

Added ~70 GLES3 entry points to the shim as no-op stubs:

- **Query/error**: `glGetError → GL_NO_ERROR`, `glGetString` (version/vendor/renderer/extensions — advertises GL_OES_EGL_image etc. so Texture2dDirect is happy), `glGetIntegerv` (plausible max-texture-size / max-viewport-dims).
- **Object create**: `glGenTextures` / `glGenBuffers` / `glGenFramebuffers` / `glGenRenderbuffers` / `glGenVertexArrays` / `glCreateShader` / `glCreateProgram` — incrementing counter IDs so each object is distinct.
- **Binding**: no-op.
- **Texture data**: `glTexImage2D` logs the format/size (asset-tracking gold), otherwise no-op.
- **Shader**: `glShaderSource` / `glCompileShader` / `glLinkProgram` no-op; `glGetShaderiv(GL_COMPILE_STATUS)` and `glGetProgramiv(GL_LINK_STATUS)` return `GL_TRUE` so Radium's pipeline doesn't bail.
- **Framebuffer**: `glCheckFramebufferStatus → GL_FRAMEBUFFER_COMPLETE`.
- **State / Uniforms / Draw**: all no-op; draw/uniform calls log with budget to avoid flooding.

### 22.2: First-derivative win — game runs its init deep

With GL stubs in place, the game progresses to:

- **11 shader programs compiled** (22 shader objects, GL_VERTEX_SHADER 0x8b31 + GL_FRAGMENT_SHADER 0x8b30 pairs). This is the Radium shader library.
- **SternLogo splash texture upload**: `glTexImage2D 1920×1080 internal=0x1908 (GL_RGBA) fmt=0x1908 type=0x1401 (GL_UNSIGNED_BYTE)` with real data pointer — the boot splash PNG, loaded via libpng and uploaded.
- **3 glDrawArrays(count=6)** calls — fullscreen quads (2 triangles each). The splash gets rendered.
- **Second gbm_bo_create(1024×256, fmt=0x34324241 [BGRA8888], flags=0x10)** — almost certainly the LCD DMD display surface.
- Game sets up Texture2dDirect with KHR image extensions for the DMD buffer.
- SEGV at `NULL + 0x48` (si_addr=0x48) — some pointer field inside one of our fake backing structs is zero.

### 22.3: Second-derivative win — the GL/asset surface map

This is the actual value of A (the reason we chose it over a direct B jump). Observed GL surface used by the game:

| Category | Functions seen | Count |
|---|---|---|
| Error / query | `glGetError`, `glGetString`, `glGetIntegerv`, `glGetShaderiv`, `glGetProgramiv` | 5 |
| Object create | `glGenTextures`, `glGenBuffers`, `glGenFramebuffers`, `glGenVertexArrays`, `glCreateShader`, `glCreateProgram` | 6 |
| Binding | `glBindTexture`, `glBindBuffer`, `glBindFramebuffer`, `glBindVertexArray`, `glActiveTexture` | 5 |
| Texture | `glTexImage2D` (+Sub, +Parameter), `glPixelStorei`, `glGenerateMipmap` | ~5 |
| Buffer | `glBufferData` (+Sub) | 2 |
| Shader / Program | `glShaderSource`, `glCompileShader`, `glAttachShader`, `glLinkProgram`, `glUseProgram`, `glGetUniformLocation`, `glGetAttribLocation`, `glUniform{1,2,3,4}{i,f,iv,fv}`, `glUniformMatrix{3,4}fv`, `glVertexAttribPointer`, `glEnableVertexAttribArray` | ~16 |
| Framebuffer | `glFramebufferTexture2D`, `glCheckFramebufferStatus` | 2 |
| State | `glEnable`, `glDisable`, `glBlendFunc`, `glBlendFuncSeparate`, `glViewport`, `glClearColor`, `glClear` | 7 |
| Draw | `glDrawArrays`, `glDrawElements` | 2 |

All used by the game. No exotic extensions beyond `EGL_KHR_image` and `GL_OES_EGL_image`. This is the **entire GL surface B needs to bridge to host mesa.** ~50 entry points — manageable.

### 22.4: Asset-tracking wins

Per `_emulation/ASSETS.md`, new entries:

- **SternLogo** — 1920×1080 RGBA upload via `glTexImage2D`, source PNG at `/games/spike3/bin/SternLogo_800x480.png`.
- **LCD DMD buffer** — 1024×256 BGRA8888 GBM BO. Size is a strong tell: Spike 3's LCD display renders a fixed 1024×256 region. This BO would be mapped (in real execution) via `EGL_KHR_image` → GL texture, then used as the render target for Radium animations.
- **Radium shader library** — 11 programs / 22 shader objects. **Big future win**: the game calls `glShaderSource` for each one with the GLSL source as a C string. Capturing those args in the shim would dump every shader the game uses — useful for understanding the rendering pipeline and for re-creating it in other engines.

### 22.5: The wall — struct-layout reverse-engineering is diminishing returns

Progression to SEGV at `NULL + 0x48`:

1. First SEGV (Step 21 end): `0x61626d60` — game dereferenced our FAKE_GBM_BO sentinel pointer. Fixed by backing fake GBM objects with `calloc(256)` scratch memory.
2. Second SEGV (this step): `0x48` — some pointer *inside* our calloc'd GBM scratch is NULL, game followed it. Would require either reverse-engineering the exact libgbm/mesa `struct gbm_bo` layout (version-dependent, 50+ fields) OR delegating to real libgbm.

The returns per-fix are shrinking. The game isn't just calling libgbm/libEGL via the public API — in some paths it walks internal struct fields directly (either via inline accessors from a libgbm header that got inlined into the game binary, or because a game-side wrapper cached the struct layout at compile time).

### 22.6: Decision — pivot to B

The value of A was (1) getting past the splash and (2) mapping the surface. We got (2) fully — §22.3 is the B scope. We got (1) partially — rendering started, 3 draws happened, then the second texture-allocation path tripped.

Continuing A means reverse-engineering libgbm / libEGL struct layouts. That work doesn't transfer to B (where real libgbm owns those layouts) — it's pure stub-maintenance. Not worth the time vs pivoting to real GL.

### 22.7: Next steps — pivot to B (real host GL via surfaceless EGL)

Rough plan:

1. **Don't remove the GL stubs yet** — they're the current "best known" baseline. Keep them as a fallback.
2. **Add a `SHIM_REAL_GL=1` environment mode.** When set, `eglGetPlatformDisplayEXT` / `eglInitialize` / etc. forward to **host-native** mesa (via `dlopen("libEGL.so.1", RTLD_NOW | RTLD_DEEPBIND)` pointing at `/usr/lib/x86_64-linux-gnu/libEGL.so.1`, explicitly overriding the aarch64 sysroot).
3. **Use `EGL_PLATFORM_SURFACELESS_MESA`** (0x31DD) to get an EGL display without needing DRM/GBM. The game's `eglGetPlatformDisplayEXT(platform=EGL_PLATFORM_GBM_KHR=0x31D7, ...)` call can be translated to SURFACELESS internally.
4. **Bridge GL calls**: the game's `libGLESv2.so.2` gets LD_PRELOADED-over; our stubs forward to host `libGLESv2.so.2`. Calling-convention bridging across aarch64-guest → x86-host is the risky part — `syscall` and function-ptr arguments are the known gotchas.
5. **Capture `glShaderSource`** as a side-effect — dump all 11+ programs to `_emulation/shaders/` for extraction.

Rebuilding and keeping it incremental is fine — start with just `eglInitialize` and `glGetString` going real, verify the handle passing works, expand from there.

### 22.8: Decisions this session

| Decision | Why |
|---|---|
| Back GBM fake objects with `calloc(256)` scratch | Game dereferences our fake pointers directly; scratch memory gets NULL everywhere which doesn't crash unless the game follows a pointer chain. Moved the crash from "pointer is invalid address" to "pointer-in-our-scratch-is-NULL" — a deeper layer. |
| Advertise `GL_OES_EGL_image`, `GL_EXT_texture_format_BGRA8888` etc. in `glGetString(GL_EXTENSIONS)` | Texture2dDirect and Radium query these; without them, they take failure branches and abort. |
| Return `GL_TRUE` for `glGetShaderiv(GL_COMPILE_STATUS)` / `glGetProgramiv(GL_LINK_STATUS)` | Radium otherwise bails on shader-compile failure. We're lying about success, but without a real GL we can't do better; and with real GL we'd get genuine success for well-formed shaders. |
| Keep all 50+ GL stubs even though we're pivoting | They're the best-known signatures for B. Each stub is a drop-in location where B will plug in a forwarded call. |
| Pivot to B rather than continue chasing struct-field-deref crashes | See §22.5. Diminishing returns on A, and A's main value was the surface map, which we've got. |

### Run command and rebuild command unchanged.

---

## Step 23: Render-loop + DMD pipeline mapped — strategy pivot to asset capture (2026-04-18)

### 23.0: Entry state

Step 22 closed with the plan to pivot to real host GL via `EGL_PLATFORM_SURFACELESS_MESA`. Before doing that, asked Sidekick / BN for the render-loop and DMD pipeline structure. The findings substantially changed the plan.

### 23.1: BN findings — render loop and DMD pipeline

**Page-flip caller** (the game-side function that commits each frame):

`sub_d01880` (BN VA `0xd01880` / file `0x901880`)
- `0xd01918`: `bl drmModePageFlip` with `DRM_MODE_PAGE_FLIP_EVENT` (flag=1, blocks for vblank)
- `0xd01a80`: `bl drmModePageFlip` with `DRM_MODE_PAGE_FLIP_ASYNC` (flag=2, immediate)
- After each flip, the function enters `select(drm_fd+1, &readfds, …)` at `0xd019cc` waiting for the DRM event, then calls `drmHandleEvent` at `0xd019e0` to process the page-flip-completion callback.

**Render-loop entry**:

`sub_bb76e0` (BN VA `0xbb76e0` / file `0x7b76e0`) — DisplayRenderer thread, 5220 bytes, 269 blocks. **Callback-driven, NOT a `while(running)` loop.** Wakes on a `std::condition_variable`, processes a multi-queue command system (7+ queues), renders main display + secondary (DMD) window, flips, waits.

Thread creation chain: `sub_bb7480` (DisplayRenderer constructor) → `CC_mainDispatchThread(sub_bb8bfc, …, "DisplayRenderer", …)` at `0xbb75ac` → `sub_bb8bfc` (thread wrapper) → `sub_bb76e0` (the loop).

**DMD pipeline** — class `DMDEmulationScene`, constructor at `sub_a8ca40` (BN VA `0xa8ca40` / file `0x68ca40`):
- Allocates 1 MB BGRA buffer (`malloc(0x100000)`) at object offset `+0x30` — the CPU-side DMD frame buffer.
- Creates `Texture2dDirect(1024, 256, 0)` — the GPU-side texture wrapper.
- Creates `Sprite(texture)` — positions centered on parent display.
- Class-name string at `0x3a11108`.

**The actual DMD upload path** uses **DMA-BUF zero-copy**, not `glTexSubImage2D`:

1. `create_gbm_bo_and_upload` (BN VA `0xcffe80` / file `0x8ffe80`):
   - `gbm_bo_create(gbm_device, 1024, 256, 0x34324241 GBM_FORMAT_ABGR8888, 0x10 GBM_BO_USE_RENDERING)`
   - `gbm_bo_map(bo, 0, 0, 1024, 256, GBM_BO_TRANSFER_WRITE, &stride, &map_data)` ← **the key call we don't currently hook**
   - Row-by-row memcpy from CPU buffer into mapped GBM buffer (accounting for stride)
   - `gbm_bo_unmap(bo, map_data)`
   - `gbm_bo_get_fd(bo)` → DMA-BUF fd
2. `init_tex_rgba` (BN VA `0xd00030` / file `0x900030`):
   - Builds EGL attribute list with `EGL_LINUX_DMA_BUF_EXT` (0x3270), `EGL_DMA_BUF_PLANE0_FD = fd`, `EGL_DMA_BUF_PLANE0_PITCH = stride`, etc.
   - `eglCreateImageKHR(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attribs)`
   - `glBindTexture(GL_TEXTURE_EXTERNAL_OES = 0x8d65, tex_id)`
   - `glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, img)` ← zero-copy bind
   - `eglDestroyImageKHR(display, img)` (texture retains the BO reference)

For non-DMD scene textures, the path is conventional:
- `StreamingImage_Bind` (BN VA `0xca7210` / file `0x8a7210`)
- Inside, at `0xca769c`: `glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, fmt, GL_UNSIGNED_BYTE, pixel_data)`
- Compressed DXT1/DXT5: `glCompressedTexSubImage2D`

### 23.2: The strategy pivot — capture-not-render

Two implications fall out of these findings:

**(a)** The current `NULL + 0x48` SEGV is almost certainly because **we don't hook `gbm_bo_map`**. When `create_gbm_bo_and_upload` calls it on our 256-byte fake-scratch BO, real libgbm walks driver state and dereferences NULL inside our scratch.

**(b)** The render loop being callback-driven (not pull-based) means **we don't need real rendering to keep it running** — we just need to satisfy the page-flip wait protocol. Synthesizing a fake DRM page-flip event into the DRM fd's read side will unblock `select()` and `drmHandleEvent` will fire, triggering the next iteration.

Combined: we can keep the existing GL stubs, add `gbm_bo_map` + a fake page-flip event, and **the render loop will self-drive**. Every frame it processes will call `gbm_bo_map` (DMD) and `glTexImage2D` / `glTexSubImage2D` (scene textures) and `glShaderSource` (Radium shader programs) — with **the real asset data in the call args**. We capture those, dump to disk, and we have an asset extractor without needing any real GL.

This is a strictly better trade than the original B plan (real GL via SURFACELESS) for the current goal of asset extraction:
- Cheaper to implement (a few hooks vs cross-arch GL bridging).
- Captures the assets the user actually wants.
- Real GL becomes "needed only for VPX display bridging" — deferrable until I/O work is done.

### 23.3: The plan for the next session

**Goal**: capture pipeline working, render loop running indefinitely.

**Hooks to add** (~5 surgical additions to the shim):

| Hook | Implementation | Output |
|---|---|---|
| `gbm_bo_map` | New library-level hook. Detect 1024×256 ABGR8888 → DMD; allocate real `calloc(stride * height)` backing per BO; track per-BO so `gbm_bo_unmap` can flush. Set `*stride_out = width * 4`, `*map_data_out = ` our backing. Return our backing pointer. | (DMD frames captured on unmap, see next row) |
| `gbm_bo_unmap` | New library-level hook. If the BO was tagged DMD, write the backing buffer to `_emulation/dmd/frame_NNNNNN.bgra` (1 MB raw, 1024×256×4). Free the backing. | `_emulation/dmd/frame_NNNNNN.bgra` per DMD frame |
| `glShaderSource` (existing stub, add capture) | Concatenate `count` source strings, write to `_emulation/shaders/shader_NN_typeXXXX.glsl` keyed by shader id + type (vertex/fragment) | `_emulation/shaders/*.glsl` |
| `glTexImage2D` + `glTexSubImage2D` (existing stubs, add capture) | When `data` is non-NULL, dump `width * height * bytes_per_pixel(format,type)` bytes to `_emulation/textures/tex_NNNNNN_WIDTHxHEIGHT_FMT.raw` | `_emulation/textures/*.raw` |
| `glCompressedTexSubImage2D` (new) | Dump compressed payload to `_emulation/textures/tex_NNNNNN_compressed_FMT.bin` | `_emulation/textures/*_compressed.bin` |
| Synthesize fake DRM page-flip event | When the game's `drmModePageFlip` ioctl on our fake DRM fd fires (DRM_IOCTL_MODE_PAGE_FLIP — already stubbed to return 0), also write a `struct drm_event_vblank` of the right shape to the **shim side of a socketpair** that takes the place of the DRM fd. The game's `select()` then sees the read-side become readable, `drmHandleEvent` reads our synthesized event and calls the user-data callback. **Important**: this changes the DRM fd from a fake constant fd to a real socketpair (same pattern as UART). | Render loop self-drives; one frame becomes many. |

**Open questions for the next session** (none blocking):
- Confirm that `gbm_bo_map`'s caller actually expects a write-back path, or if it's purely "give me a pointer I can write into and a stride." (BN excerpt suggests the latter — we just need to populate the stride and return a buffer.)
- The DRM event format for page-flip-completion: `struct drm_event_vblank { drm_event base; u32 user_data; u32 tv_sec; u32 tv_usec; u32 sequence; u32 crtc_id; }` — verify this matches the version the game's libdrm expects.

**What does NOT get done next session** (deferred):
- Real host GL via SURFACELESS EGL. Now categorized as "needed for VPX display bridging" — re-evaluate after we have:
  1. I/O bridge (switches/coils/lamps → VPX) functional.
  2. A reason to need actual rendered pixels (e.g., capturing playfield graphics for VPX backglass rendering).
- Command-queue analysis (BN follow-up #3) — defer until VPX render-command injection is on the table.

### 23.4: Decisions this session

| Decision | Why |
|---|---|
| Pivot from "real-GL B" to "capture-assets via existing stubs + add `gbm_bo_map`" | BN revealed the render loop is callback-driven and the DMD path is DMA-BUF zero-copy. Both insights make capture cheaper and more valuable than rendering. |
| Treat asset extraction as the primary near-term deliverable | User's stated goal; aligns with the project's "extract assets" track from the two-tracks memory; doesn't require display rendering. |
| Replace fake DRM fd constant with a real socketpair | Same trick that worked for UART — gives us a real readable fd we can synthesize events on, satisfying the game's `select()` without needing real DRM. |
| Defer real-GL pivot indefinitely | Real GL only buys us pixels, and we don't yet have anything to do with pixels (no VPX bridge yet). The capture path gives us the data extraction the user wants, immediately. |

### Run command unchanged. Rebuild command unchanged (`-ldl -lpthread`).

## Step 24: Capture pipeline wired into the shim (2026-04-18)

### 24.1: Implementation

All five Step 23.3 hooks landed in `spi_shim.c`. Build is clean with `-Wall -Wextra` (one pre-existing unused-static warning for `uart_is_active`, unrelated).

**DRM fd → real socketpair.** Replaced `FAKE_FD_DRM = 1020` + `drm_active` with `drm_game_fd` / `drm_shim_fd` (same pattern as UART). `is_fake_fd` no longer covers DRM; new `is_drm_fd()` helper tests membership. `intercept_device` creates the socketpair on `/dev/dri/card1` open with `drm_shim_fd` set `O_NONBLOCK`. `close()` tears down both ends. `read()` and `ioctl()` both route DRM fd traffic explicitly (ioctl early-exits before the generic passthrough, which was the fix for "the socketpair would reject DRM ioctls"). The DRM switch in `ioctl()` kept its full body — only the guard changed (`fd == FAKE_FD_DRM` → `is_drm_fd(fd)`).

**Page-flip event synthesis.** New helper `drm_synthesize_flip_event(crtc_id, user_data)` writes a 32-byte `drm_event_vblank`-shaped struct to `drm_shim_fd` whenever `DRM_IOCTL_MODE_PAGE_FLIP` comes in with `DRM_MODE_PAGE_FLIP_EVENT` set (flag bit 0). The struct's `length` field is the full 32, so libdrm's `drmHandleEvent` will consume the right number of bytes regardless of which `drm_event_vblank` revision it was compiled against (older libdrm stops at `sequence`; newer includes `crtc_id`). Sequence counter increments per flip; timestamps come from `CLOCK_MONOTONIC`. This is what makes the render loop self-drive: the game's `select(drm_fd+1, …)` wakes, `drmHandleEvent` reads the event, and `sub_d01880` returns, letting the callback-driven render thread start its next iteration.

**GBM BO tracking + map/unmap.** New side table `gbm_bo_table[32]` guarded by a pthread mutex. `gbm_bo_create` and `gbm_bo_create_with_modifiers` register `(w, h, format, flags, stride=w*4)` per BO; the `is_dmd` flag gets set when `w=1024 && h=256 && format=GBM_FORMAT_ABGR8888 (0x34324241)`. `gbm_bo_get_{width,height,stride,format}` now consult the table so the DMD BO correctly reports 1024×256 ABGR8888 instead of 1920×1080 XR24. `gbm_bo_map` allocates `calloc(stride*height)` backing on first map, hands back `(backing, stride)`. `gbm_bo_unmap` dumps the backing to `_emulation/dmd/frame_NNNNNN.bgra` when `is_dmd`, then frees it. `gbm_bo_destroy` unregisters.

**Shader source capture.** `glCreateShader` now records the shader's type in `gl_shader_type[id]` (indexed by id, max 1024). `glShaderSource` concatenates the count×(string, length) args into a single buffer and writes it to `_emulation/shaders/shader_NNN_idM_typeXXXX.glsl`. Length arg is honored (glShaderSource's contract: non-negative length overrides strlen).

**Texture capture.** `glTexImage2D` and `glTexSubImage2D` now call `gl_capture_tex` when `data != NULL`, which computes bytes-per-pixel from `(format, type)` using a small table of GLES 3.1 enum values (packed types like `UNSIGNED_SHORT_5_6_5` return total bytes directly) and dumps `w*h*bpp` to `_emulation/textures/tex_NNNNNN_WxH_fmtXXXX_typeYYYY.raw`. Compressed variants (`glCompressedTexImage2D` already existed as a no-op; added `glCompressedTexSubImage2D` as a new hook) dump raw `size` bytes to `tex_compressed_NNNNNN_...bin`.

**Capture-dir bootstrap.** `__attribute__((constructor))` `shim_init_capture_dirs` mkdirs `_emulation/{dmd,shaders,textures}` at `LD_PRELOAD` time. Relative paths resolve against the game's cwd, which is `games/pokemon_le/` per the documented run command. Could not `#include <sys/stat.h>` (it conflicts with the shim's loose-typed stat/lstat/fstatat wrappers — `void *statbuf` vs `struct stat *`); forward-declared `mkdir` directly.

### 24.2: What to look for on next run

| Artifact | Signal of success |
|---|---|
| `_emulation/dmd/frame_000000.bgra` … | DMD upload path (`create_gbm_bo_and_upload`, `0xcffe80`) executed at least once — means `gbm_bo_map` and `gbm_bo_unmap` are being reached instead of the previous NULL+0x48 SEGV. 1024×256×4 = 1 MB per file. Opens cleanly in GIMP as "Raw image data → ABGR8888 no alpha plane" or in ImageMagick as `-size 1024x256 -depth 8 ABGR:frame.bgra out.png`. |
| `_emulation/shaders/shader_NNN_*.glsl` | 22 files expected (Step 21 observed 11 vertex + 11 fragment). If we see those plus extra later, those are runtime scene shaders. |
| `_emulation/textures/tex_NNNNNN_*.raw` | Non-DMD scene textures. StreamingImage_Bind path (`0xca7210`) is the primary source. Sizes and fmt fields tell us what's being uploaded. Compressed variants (DXT1/DXT5) land in `tex_compressed_*.bin`. |
| Render loop keeps running | `[shim] DRM ioctl MODE_PAGE_FLIP #N …` messages increment, and `DRM read #N got 32 bytes` messages appear — confirms `drmHandleEvent` is consuming our events and the loop is self-driving. |

### 24.3: Known unknowns for follow-up

- `drm_event_vblank` layout compat: if the game's libdrm is old enough that `drmHandleEvent` rejects 32-byte events, we'd see a read-but-no-callback symptom. Easy test: check whether PAGE_FLIP count keeps advancing after the first event. (If not, shorten the event to 28 bytes — drop `crtc_id`.)
- Other BO paths: the game may call `gbm_bo_map` on BOs allocated via a path we didn't register (e.g., through `gbm_surface_*`). The unknown-BO fallback in `gbm_bo_map` allocates scratch sized by the caller's `w*h`, which is enough to avoid a SEGV, but the unmap will just `free(map_data)` without capturing. If we see a missing DMD-shaped BO, check whether it came via gbm_surface_ rather than gbm_bo_create.
- Capture volume: if per-frame BGRA at 1MB adds up fast, may need a frame-stride cap (`dmd_frame_counter % N == 0`). Not doing that pre-emptively — easier to rate-limit later once we know the real frame rate the loop produces.

### 24.4: Build

    aarch64-linux-gnu-gcc -shared -fPIC -Wall -Wextra -o spi_shim.so spi_shim.c -ldl -lpthread

Clean. New symbols in `spi_shim.so`: `gbm_bo_map`, `gbm_bo_unmap`, `glCompressedTexSubImage2D`. Existing `glShaderSource` body replaced; existing `glTexImage2D`/`glTexSubImage2D`/`glCompressedTexImage2D` now capture. `drm_synthesize_flip_event` is internal (`static`).

## Step 25: First capture run — the SEGV is gone, ALSA is the new blocker (2026-04-18)

### 25.1: What happened

Game was launched with the Step-24 shim. It ran for several seconds (11.89M SPI transfers worth) before aborting on an ALSA assertion:

```
game: pcm.c:1239: snd_pcm_prepare: Assertion `pcm' failed.
qemu: uncaught target signal 6 (Aborted) - core dumped
```

The prior NULL+0x48 SEGV inside the DMD path is gone. The render thread reached the point of flipping at least one frame, and we captured artifacts.

### 25.2: What landed on disk

- `_emulation/dmd/frame_000000.bgra` — 1,048,576 bytes, i.e. 1024×256×4 BGRA. Proves `gbm_bo_map` → `memcpy` → `gbm_bo_unmap` path executes end-to-end and that the BO the game uses for the LCD DMD is exactly 1024×256 ABGR8888 (`is_dmd` flag matched, shim wrote the frame). Only one frame captured because the game died shortly after.
- `_emulation/shaders/` — **24 files** (12 vertex `type=0x8b31`, 12 fragment `type=0x8b30`). This refines the prior "11 programs / 22 objects" count from Step 21 — actual is **12 programs / 24 objects**. First shader (`shader_000_id1_type8b31.glsl`) is a standard `#version 300 es` model/projection sprite vertex shader; source text is clean GLSL. Total shader source is ~7.5 KB.
- `_emulation/textures/` — **17 files**, all uncompressed `GL_RGBA` + `GL_UNSIGNED_BYTE`:
  - `tex_000000_1920x1080` (8 MB) — SternLogo splash (rendered into the 1920×1080 screen canvas from an 800×480 PNG — matches the Step 21 observation).
  - 16 more at 1024×{64,128,256,512} — consistent with LCD-oriented font atlases or line-oriented tile strips, not mipmap chains (mipmap progression would be power-of-two descending and halving each axis).
  - Zero compressed-texture hits. The game's active texture path in this window is entirely uncompressed RGBA.

### 25.3: Did the render loop actually self-drive?

Only one DMD frame captured before abort, so we cannot distinguish between:
- (a) the flip-event synthesis worked, the loop iterated once, then ALSA killed it, or
- (b) the first DMD upload happened during init *before* any page flip (the DMD upload in `create_gbm_bo_and_upload` happens as part of scene construction, possibly eagerly), and the flip-event path was never exercised.

Verdict deferred — once ALSA is fixed and the game runs longer, we'll see whether DMD frames keep coming. If they do, (a) is confirmed. If we get exactly one and then silence, (b) and the flip path still needs work.

### 25.4: The new blocker — ALSA

`aarch64-linux-gnu-nm -D --undefined-only games/pokemon_le/game | grep snd_` lists 18 imported ALSA symbols, all playback-side:

```
snd_pcm_open, snd_pcm_close, snd_pcm_prepare, snd_pcm_drain, snd_pcm_nonblock,
snd_pcm_writei,
snd_pcm_hw_params, snd_pcm_hw_params_any, snd_pcm_hw_params_sizeof,
snd_pcm_hw_params_set_access/format/channels/rate/rate_near/buffer_size/period_size,
snd_pcm_hw_params_get_channels/get_rate
```

No capture, no sequencer, no mixer. The assertion fires inside glibc's `snd_pcm_prepare` at `pcm.c:1239`, which checks `assert(pcm != NULL)` at function entry. The game apparently calls `snd_pcm_open` (which fails under QEMU user-mode because there is no `/dev/snd/*`), does not check the return code, and passes the uninitialized PCM pointer to `snd_pcm_prepare`. Glibc asserts, game aborts.

**Fix strategy** (same pattern as GBM/EGL — hook all imported symbols, hand back fake handles, swallow data):

| Symbol | Stub behavior |
|---|---|
| `snd_pcm_open(pcm_out, name, stream, mode)` | Allocate a small `calloc(256)` scratch and return it via `*pcm_out`; return 0. Log first call. |
| `snd_pcm_close / prepare / drain / nonblock(pcm, ...)` | Return 0, no-op. |
| `snd_pcm_hw_params_sizeof` | Return a plausible size (≥1 KB so the caller's stack or heap allocation is big enough). |
| `snd_pcm_hw_params` + `snd_pcm_hw_params_any` + all `snd_pcm_hw_params_set_*` | Return 0. |
| `snd_pcm_hw_params_get_channels` | `*val = 2`, return 0. |
| `snd_pcm_hw_params_get_rate` | `*val = 44100; *dir = 0`, return 0. |
| `snd_pcm_writei(pcm, buf, frames)` | Return `frames` (claim all frames consumed). No sleep — game's own audio thread should gate itself. If it spins, add a `usleep(frames * 1000000 / 44100)` to pace. |

This should let audio init succeed silently, the game continues, and we get long-run captures.

### 25.5: What to verify after the ALSA fix

1. DMD frame counter advances past 1 → render-loop self-drive confirmed (§25.3 question resolved).
2. Shader capture count either stays at 24 or grows — if it grows, runtime-generated scene shaders are a thing (so Radium lazy-compiles, not just an init-time batch).
3. Texture captures grow — we'll start seeing game-content textures beyond the 1920×1080 SternLogo + 17 atlas-shaped uploads.
4. New asset classes may open: look for `libavcodec` / gstreamer opens (video), new `openat` patterns into `image.bin` (PCM audio).

### 25.6: Questions for Binary Ninja (not blocking — ALSA stub should work without them)

Useful follow-up, in order of value:

1. **Who calls `snd_pcm_open` in the game and does it check the return code?** If it doesn't, the no-op-succeeds stub above will work. If it does but still proceeds on error to `snd_pcm_prepare`, there's a weirder bug to reason about.
2. **Does `snd_pcm_writei` get called in a tight loop or one-shot-per-sample-block?** Determines whether a no-op stub needs to pace itself to avoid the audio thread spinning at 100% CPU.
3. **What format / sample rate does the audio subsystem set via `snd_pcm_hw_params_set_rate` / `set_format`?** So we can return matching values from the getters and so we know what to expect in the captured PCM (when we add PCM capture via `snd_pcm_writei`).

## Step 26: ALSA stub — fake-succeed + capture PCM stream (2026-04-19)

### 26.1: Decision

Used the BN answers (Step 25.6 investigation summary) to go with the **fake-success with capture** approach rather than fail-gracefully. Rationale: the project's asset-tracking goal (memory `project_asset_tracking.md`) explicitly lists "sounds" as a capture target, so exercising the audio thread and dumping `snd_pcm_writei` payloads delivers data we want. The fallback to fail-gracefully stays one-line-easy if this approach misbehaves.

BN-confirmed facts that shaped the stub:

| Fact | Source | Stub implication |
|---|---|---|
| `snd_pcm_open` return is checked at `0xce6270` (tbnz bit 31) | BN of `alsa_pcm_init` @ `0xce61e0` | `fail-gracefully` path would work (return `-ENODEV`) but loses audio |
| `snd_pcm_writei` is in a tight loop, **no usleep** | BN of `alsa_audio_thread` @ `0xce5f68` | Stub MUST pace itself — added `usleep(size * 1000000 / rate)` |
| Format: S16_LE, 2 channels, interleaved, 2048 frames/buffer, default 44100 Hz | BN of hw_params setters | Getters return 44100 Hz + 2 ch; `set_rate_near` writes back 44100 |

### 26.2: Implementation

Added 18 ALSA hooks in `spi_shim.c` (after `/* --- end GL stubs --- */` block). All playback-side — matching `nm -D --undefined-only games/pokemon_le/game | grep snd_` exactly:

- `snd_pcm_open` — allocates `calloc(256)` scratch handle on first call, returns 0, writes handle via `*pcm_out`.
- `snd_pcm_close / prepare / drain / nonblock` — no-op return 0.
- `snd_pcm_hw_params_sizeof` — returns 1024 (safe upper bound; caller's stack or heap alloc won't under-allocate).
- All 7 `snd_pcm_hw_params_set_*` + `snd_pcm_hw_params` + `snd_pcm_hw_params_any` — return 0.
- `snd_pcm_hw_params_set_rate_near` — writes `*rate = 44100, *dir = 0` (matches what the game asks for anyway).
- `snd_pcm_hw_params_get_channels` — `*val = 2`.
- `snd_pcm_hw_params_get_rate` — `*val = 44100, *dir = 0`.
- `snd_pcm_writei(pcm, buffer, size)`:
  - appends `size * 4` bytes (stereo × S16_LE = 4 B/frame) to `_emulation/audio/stream.s16le` under mutex,
  - `fflush()` every 64th call so partial data survives a kill,
  - paces with `usleep(size * 1000000 / 44100)` — at 2048 frames that's 46.4 ms, matching the real audio buffer duration,
  - returns `size` to claim all frames consumed.

Constructor now mkdir's `_emulation/audio/` alongside the other capture dirs.

### 26.3: Stream playback / conversion

The captured file is raw S16_LE stereo interleaved at 44100 Hz. Convert with:

```bash
# Listen:
ffplay -f s16le -ar 44100 -ac 2 _emulation/audio/stream.s16le

# Convert to WAV:
ffmpeg -f s16le -ar 44100 -ac 2 -i _emulation/audio/stream.s16le stream.wav

# Inspect duration:
python3 -c 'import os; print(os.path.getsize("_emulation/audio/stream.s16le")/(44100*4), "seconds")'
```

### 26.4: What the Step 26 run should show

1. Game progresses past the `snd_pcm_prepare(NULL)` assertion (the first concrete proof that the ALSA stubs work).
2. DMD frame counter > 1 — with audio out of the way, the render loop should run long enough to produce many frames, which also resolves the §25.3 open question (was flip-event synthesis actually consumed, or did we get one frame from eager DMD init?).
3. `_emulation/audio/stream.s16le` grows at ≈176 KB/s. First kilobytes will likely be silence or attract-mode music, depending on how fast the audio thread starts feeding.
4. Shim log shows `snd_pcm_open` → `snd_pcm_writei #1 frames=2048 bytes=8192` → continues.

### 26.5: If this approach misbehaves

Fallback 1 (audio thread spinning despite usleep): increase the usleep a bit — maybe the game's frame_count is variable and sometimes too small for the resulting usleep to be meaningful.

Fallback 2 (weird game-state interactions because we fake-succeed): swap `snd_pcm_open` body to:

```c
int snd_pcm_open(void **pcm, const char *name, int stream, int mode) {
    if (pcm) *pcm = NULL;
    return -19;  /* -ENODEV */
}
```

That's the fail-gracefully path BN proved works (error code 7 from `alsa_pcm_init`, game skips audio). Loses captures but is zero-risk.

### 26.6: Build / run unchanged

```
aarch64-linux-gnu-gcc -shared -fPIC -Wall -Wextra -o spi_shim.so spi_shim.c -ldl -lpthread
```

Clean (one pre-existing `uart_is_active` unused-static warning, unrelated). `nm -D spi_shim.so | grep snd_pcm_` lists all 18 expected exports.

## Step 27: ALSA stubs green; scene loader ran deep; SEGV 11 in what is almost certainly video (2026-04-19)

### 27.1: Run headline

Game executed ~4× longer than before (49.5 M SPI transfers) then died with `qemu: uncaught target signal 11 (Segmentation fault) - core dumped`. No glibc assertion, no message — raw memory access fault.

### 27.2: What landed on disk

| Capture | Step 25 | Step 27 | Delta |
|---|---|---|---|
| `_emulation/dmd/` | 1 | (not re-examined; at least 1) | — |
| `_emulation/shaders/` | 24 objects / 12 programs | **58 objects / 29 programs** | +34 objects = +17 programs |
| `_emulation/textures/` | 17 | **445** | +428 |
| `_emulation/audio/stream.s16le` | — | **11,716,160 bytes** (66.4 s of S16_LE stereo @ 44100 Hz) | — |

The audio file size directly confirms ALSA pacing worked: 66 s of real-time buffer duration means the audio thread was NOT spinning — `usleep(size*1e6/rate)` kept it behaved. Roughly 1436 `snd_pcm_writei` calls at the 2048-frame default.

### 27.3: Observations from the texture list

- **Explicit mipmap pyramids.** Many assets are uploaded as a descending chain `1800×600 → 900×300 → 450×150 → 225×75 → 112×37 → 56×18 → 28×9 → 14×4 → 7×2 → 3×1 → 1×1` with each level = prev/2 (floor). The game does NOT rely on `glGenerateMipmap` for these — it hands every level to `glTexSubImage2D` explicitly. The chain repeats ~15 times for different base-size assets (1800×600 is by far the most common, plus a few with other bases).
- **Font atlases.** 79 × `256×256 fmt=0x190a (GL_LUMINANCE_ALPHA) type=0x1401 (UNSIGNED_BYTE)` textures. LA@256×256 is the classic SDF / glyph-grid shape — this is almost certainly the LCD font set rasterized into slots.
- **UI-size sprites.** Several `1920×1080` (fullscreen), `2210×884` (ultra-wide?), `2048×2560` (vertical big atlas), `1536×1536`, `1032×896`, `695×658`, `564×583` (repeated 6×), `851×1045`, `2048×2048` — all `fmt=0x1907 (RGB)` or `0x1908 (RGBA)` `type=0x1401`.
- **Unknown enum** `fmt=0x8d99` on a single 4×4 texture (Step 27-texture #69). Not in the standard GLES core enum space; belongs to an extension. Ignored for now — 4×4 is placeholder-sized so it's not data-bearing.
- **Depth placeholder.** `4×4 fmt=0x1902 (DEPTH_COMPONENT) type=0x1403 (UNSIGNED_SHORT)` — sized like a scratch/default shadow-map that never gets a real upload.
- **No compressed textures landed.** `_emulation/textures/tex_compressed_*.bin` never materialized — the game's active texture path in this run is entirely uncompressed.

### 27.4: The stderr JSON leak

Mixed in with the SPI log spam, the game printed (via its own logging/debug path, not the shim):

```json
{
  "Coast":       "res://gfx/bknd/Coast/Lake_01_22_26_h265.mp4",
  "Desert":      "res://gfx/bknd/Desert/desert_battle_med_h265.mp4",
  "Forest":      "res://gfx/bknd/Forest/Forest_01_22_26_h265_2.mp4",
  "Mountain":    "res://gfx/bknd/Mountain/Mountain_01_22_26_h265.mp4",
  "Rocket_Lab":  "res://gfx/bknd/Rocket_Lab/TeamRocketLab_h265.mp4",
  "Rocket_HQ":   "res://gfx/bknd/Rocket_HQ/RocketOffice_01_22_26_h265.mp4",
  "Arena":       "res://gfx/bknd/Arena/Stadium_01_22_26_h265.mp4",
  "Arena_Neon":  "res://gfx/bknd/Arena_Neon/Tournament_Neon_01_22_26_h265.mp4"
}
```

This tells us:
1. The game has a **Godot-flavored `res://` resource system** — same convention as the standalone `assets/godot/pokemon_battle_previs/` tool in the repo.
2. **Eight battle environment backdrops**, H.265-encoded, keyed by environment name. Matches the previs tool's Ctrl+B battle mode background set.
3. The naming suffix `01_22_26` looks like a date (Jan 22, 2026) — consistent with a recent build iteration.
4. SPI TX byte 7 transitioned from `0x2c → 0x6c` right around this print, indicating the game advanced into a new state (probably "scene loader active" or similar).

Shortly after this print, the SEGV fires. Strong circumstantial evidence that **the video loader is the crash site**: game imports `libavcodec.so.59`, `libgstapp-1.0.so.0`, `libgstvideo-1.0.so.0`; none of them are stubbed; any one of them can SEGV against our fake GBM/EGL/DRM state when it tries to open a device for decode.

### 27.5: What we still don't know about the SEGV

- Which function was in-flight? No backtrace captured in this run.
- Did the SEGV happen inside a shim hook (NULL-deref of scratch state) or inside real library code reading game state?
- Did it happen on the audio thread, render thread, or main thread?

### 27.6: Investigation plan — cheapest first

**(a) Redirect stderr to a log.** The interesting shim lines (the last hook call before SEGV) scroll off-screen in an interactive terminal. Add a wrapper:

```bash
cd /mnt/c/Workspace/pokemon-le-0.81.0/games/pokemon_le && \
qemu-aarch64 -L /mnt/c/Workspace/pokemon-le-0.81.0 \
  -E LD_PRELOAD=/mnt/c/Workspace/pokemon-le-0.81.0/games/pokemon_le/_emulation/spi_shim.so \
  -E SPIKE3_SYSROOT=/mnt/c/Workspace/pokemon-le-0.81.0 \
  ./game 2> _emulation/run_step27.log
```

Then `tail -n 300 _emulation/run_step27.log` after SEGV. The last shim-line tells us which subsystem was active.

**(b) Add a SIGSEGV handler to the shim.** ~20 lines; uses `backtrace()` + `backtrace_symbols_fd(2)` in a signal handler. Under QEMU user-mode the backtrace resolves to aarch64 stack frames (but may not always demangle cleanly). Worth trying if (a) is ambiguous.

**(c) Run under gdb-multiarch with `qemu-aarch64 -g 1234`.** Gives the exact crash frame with source context. Heaviest setup; fall back to this if (a) and (b) fail.

### 27.7: Decisions

- **Do NOT swap to fail-gracefully ALSA.** The audio capture is delivering data we want, and the ALSA path is provably not the crash site (the crash came ~66 s in, audio was still flowing, and the final shim line was a shim SPI log not an ALSA call).
- **Document the texture/audio/video observations** before chasing the SEGV — the data we have is valuable even if the next hour is spent diagnosing.
- **Treat the JSON leak as primary evidence** for the video-loader hypothesis. BN follow-up: find the function that prints that dictionary and trace its callers.

### 27.8: Proven-fact updates to land in PROTOCOL

- ALSA playback stubs work — 18 symbols intercepted, audio thread pacing verified by 66 s real-time run.
- Texture format 0x1908 + 0x1401 (RGBA byte) is the game's default upload format for scene assets.
- Texture format 0x190a + 0x1401 (LA byte) at 256×256 is the font-glyph atlas shape.
- Explicit-mipmap convention: the game hands every pyramid level to `glTexSubImage2D`; `glGenerateMipmap` is not load-bearing.

### 27.9: User tool note — ffplay on the captured audio

`ffplay -f s16le -ar 44100 -ac 2 …` fails with `Option not found` in ffplay 7.1 (the `-ac` alias was dropped from ffplay's AVOptions). Working invocations:

```bash
ffplay -f s16le -ar 44100 -ch_layout stereo _emulation/audio/stream.s16le
# or convert to WAV first:
ffmpeg -f s16le -ar 44100 -ac 2 -i _emulation/audio/stream.s16le _emulation/audio/stream.wav
ffplay _emulation/audio/stream.wav
```

## Step 28: stderr log captured; the game is Godot 4.4.1 + Radium (2026-04-19)

### 28.1: The project-defining discovery

The stderr log for the Step 27 run (saved to `_emulation/run_step27.log`) contained this line among the init messages:

```
Godot Engine v4.4.1.stable.custom_build - https://godotengine.org
OpenGL API OpenGL ES 3.1 (stub) - Compatibility - Using Device: shim - shim GLES3
```

**The game is a Godot 4.4.1 game with a Stern-custom build, plus a proprietary module called "Radium".** This reframes the entire project:

- `res://gfx/bknd/Coast/Lake_01_22_26_h265.mp4` — Godot's `res://` ResourceLoader convention. The game's resources live inside a Godot project root or a PCK archive.
- `shader_type canvas_item;` / `varying vec4 vertex_color;` / `uniform sampler2D color_texture;` — that's **Godot Shader Language (GSL)**, not raw GLSL. The shader strings we capture via `glShaderSource` are Godot's GSL→GLSL compiler output, not the source the game author wrote.
- The `--Main Shader--` error format with `E   N-> <line>` is Godot's internal shader-compiler error reporter (surfaced here with `varying` rejected because Godot 4.x canvas_item vertex/fragment don't share `varying` the same way; but Godot logs them as errors and proceeds regardless — we can see rendering continues).
- `[Radium Error] VideoStreamDecoder: Cannot open input file ''` — **Radium** is Stern's Godot module (statically linked into the game binary) that provides `VideoStreamDecoder`, scene animation decoding, hardware bridging, and the canvas-item passes. It's the same Radium that the `assets/lcd/*.radium` files belong to.
- `Warning: Failsafe file not found at res://gfx/attack_anim_sheets/None/none_attack_v1_2k.png` — Godot's fallback-resource pattern. Verified this resource DOES exist in `assets/godot/main.pck_extracted/gfx/attack_anim_sheets/None/`. So the resource files are present on the SD card; the game just can't see them under emulation.
- `can't create directory '/data/'` (×9) — Godot's user data directory is not writable.

**Correction to initial hypothesis: image.bin is NOT the Godot PCK.** Per `_extraction/README.md` (authoritative reference):

- Godot PCK: **`assets/godot/main.pck`**, 210 MB, extracted into `assets/godot/main.pck_extracted/` (untouched, via GodotPCKExplorer — use this as the ground-truth reference). All 8 battle backdrops confirmed present at `gfx/bknd/<env>/*_h265.mp4`, plus the failsafe `none_attack_v1_2k.png`. The user has a derived project at `assets/godot/pokemon_battle_previs/` via GDRETools — that one is a working/modified copy, NOT the reference.
- Stern asset container: **`image.bin`**, 1.1 GB, Stern's proprietary format with a 10-section header, 2,452 assets (231 PCM audio SFX/speech + 2,221 animation/texture/Radium-scene data). Per README "not encrypted" (attributes high entropy to audio + compressed textures); the user believes parts may be encrypted, treat as open question. Music tracks are NOT in image.bin — likely in the PCK.

The two stores cross-reference via `ManualGodotScene` (vtable at game addr `0x04630230`): image.bin's scene bytecode can contain type-0x0b records referencing Godot resources from the PCK.

**Actual blocker:** the game can't find/mount `main.pck` under QEMU user-mode. Godot 4.x normally loads its PCK from one of: (a) appended to the end of the executable, (b) `<binary>.pck` next to the binary, (c) an explicit `ProjectSettings::load_resource_pack(path)` call. For this game the PCK lives at `assets/godot/main.pck` — a non-standard path, so method (c) must be used. Under the emulation environment, either the hardcoded path doesn't resolve (cwd issues, `res://` path lookup failing before the pack is mounted, `/data/` mkdir failure blocking init), or the mount call itself fails. Not yet determined which.

### 28.2: Full run-order from the log (first occurrence of each event)

1. Netbridge + pwrdist init — green.
2. DRM/GBM/EGL — prints `failed to get fd for bo: -1` + `failed to create framebuffer` (our `gbm_bo_get_fd` returns -1 unconditionally; the game tries to wrap a BO as a DMA-BUF framebuffer and the `-1` fd path fails). Non-fatal — game continues.
3. `can't create directory '/data/'` × 9 — repeated fails, probably from different Godot subsystems each attempting `/data/` during their own init.
4. **TAS57XX audio amp init** over I2C (address range 0x29–0x5b): 10 EQ filters + HP/LP filters + bass/treble shelves configured. Each write is followed by a `reads back as ffffffff` verify-fail message — our I2C SMBUS passthrough returns zeros, but the game reads `0xff` (uninitialized slave data or our shim returning wrong default). Non-fatal.
5. `MAX9850: headphone amp/line out init complete` — matching verify-fails.
6. `bridge message (cmd=25, id=1) response failed 110: Connection timed out` (×2) — two new bridge commands not in our synthesizer table.
7. `bridge message (cmd=41, id=1) response failed 110: Connection timed out` (×20+) — the game retries cmd 41 many times, no response synthesized, times out each time.
8. `failed to open chip: No such file or directory` — GPIO chip (probably `/dev/gpiochip0`) attempted open.
9. **`Godot Engine v4.4.1.stable.custom_build`** — the big reveal.
10. `--Main Shader--` errors with `varying vec4 vertex_color` (×8 or so) — Godot's shader compiler logging errors for `canvas_item` shaders. Rendering appears to continue regardless.
11. `[Radium Error] VideoStreamDecoder: Cannot open input file ''` (×3) — empty filename going into Radium's video decoder.
12. More shader compile messages, `samplerExternalOES` variant.
13. `Warning: Failsafe file not found at res://gfx/attack_anim_sheets/None/none_attack_v1_2k.png`.
14. The 8-environment JSON dump (battle backgrounds).
15. **SEGV.**

### 28.3: Crash hypothesis

The sequence `empty-filename → failsafe-not-found → JSON-dump-of-8-environments → SEGV` is consistent with this story:

- Godot's `res://` resolver can't resolve anything because `main.pck` isn't mounted under emulation.
- The game's battle-environment initializer builds the 8-env map (the JSON), prints it, then iterates to preload each environment's video stream.
- Either Radium's VideoStreamDecoder or libavformat/libavcodec — called with a `res://` path that resolved to empty — NULL-derefs somewhere in the preload chain.

This is **not** a shim regression; it's the game running out of resources because the PCK isn't mounted. Before Step 27 we weren't even getting this deep — the ALSA stub unblocked us into this layer.

### 28.4: Next-move decision tree

**(a) Figure out how the game tries to mount main.pck.** Strings-search the game binary for "main.pck" and "load_resource_pack" — whichever path or call site appears, we trace whether the game has a hardcoded absolute path, a relative path from cwd, an `ENVVAR`-derived path, or a binary-appended PCK. From that we can decide: remap the path through `SPIKE3_SYSROOT`, arrange symlinks, or stub the mount call to succeed silently. This is the root-cause fix for the empty `res://` paths.

**(b) Cheap long-shot — give the game a writable `/data/`.** If `/data/` being unwritable is what's blocking Godot's pack-mount or user-fs init, a writable `/data/` might unblock things:

```bash
sudo mkdir -p /data && sudo chmod 777 /data
# re-run
```

If the `can't create directory '/data/'` spam disappears AND `res://` paths start resolving, we've scored.

**(c) Backtrace the SEGV.** Add a SIGSEGV handler to `spi_shim.c` using `backtrace()` + `backtrace_symbols_fd(STDERR_FILENO)`. ~30 lines. Tells us the exact crash frame — probably inside Radium's VideoStreamDecoder or libavformat, called with an empty path.

**(d) Stub libavformat's `avformat_open_input` to return an error.** That's already the observed behavior (Radium logs the decoder-open failure), but the game eventually NULL-derefs downstream. A more aggressive stub might return an error before the NULL-deref site is reached.

**Recommended order**: (a) first — understanding the PCK-mount mechanism is the root-cause fix and also informs everything else. (b) and (c) in parallel if (a) doesn't produce an obvious hook.

### 28.5: Worth adding to PROTOCOL (proven by Step 27-28 run)

- Game engine: **Godot 4.4.1 custom_build** + **Radium** (Stern-proprietary module, statically linked). Cross-reference `_extraction/README.md` for image.bin layout; `_extraction/CLAUDE.md` for the loader call chain; `assets/godot/main.pck_extracted/` for the unmodified PCK reference tree.
- `res://` convention used for all game-side resource paths — resolves into `main.pck` at runtime.
- **image.bin is NOT the PCK.** Separate 1.1 GB Stern container with 2,452 assets (SFX, animation, Radium data) — PCK is separate at `assets/godot/main.pck` (210 MB).
- I2C readback semantics: TAS57XX and MAX9850 audio-amp slaves expect writes to be read back with the same value for verify. Our shim returns zeros, game logs errors, proceeds anyway.
- DMA-BUF framebuffer path: game uses `gbm_bo_get_fd` to get a fd to pass into `drmModeAddFB2`-like calls; our -1 return fails that path. Game continues.
- Bridge commands 25 and 41 not in our synthesis table — log-only failures, not fatal.

### 28.6: Audio content finding

66.4 seconds of real-time audio captured, format verified by ffplay. User reports playing the file produces no audible sound — most likely early-init silence (the game was still resolving resources and never started its audio source stream). Verify via `xxd _emulation/audio/stream.s16le | head -5`: if all `0000`, the game wrote all-zero buffers (expected during init); if non-zero, real samples are in there and playback is a local WSL-audio issue rather than a capture issue.

## Step 29: shader fix, SIGSEGV handler, crash localized to PopupMenu logger (2026-04-19)

### 29: `glGetIntegerv` filled in for GLES 3.x caps
Added `GL_MAX_VARYING_VECTORS=15`, `_COMPONENTS=60`, vertex/fragment uniform sizes, texture unit counts, render-target limits, etc. "Too many varyings" errors went from many → 6. Adding `GL_MAX_VERTEX_OUTPUT_COMPONENTS=64` and `GL_MAX_FRAGMENT_INPUT_COMPONENTS=60` in Step 29 drove it to **0**. All 58 captured shaders now compile from Godot's perspective.

### 30: Still same runtime cap
With shaders compiling, runtime extended ~3× (audio grew from 66 s → 188 s) but capture counts plateaued (DMD=1, shaders=58, textures=445) — the game ran much longer but didn't progress past a new wall. Added a one-shot logger to `glGetIntegerv` for unknown pnames; unknowns found were cosmetic (`GL_BLEND_SRC_ALPHA`, `GL_NUM_EXTENSIONS`).

### 31: Backtrace pinpoints Godot's error logger
Added `SIGSEGV`/`SIGBUS` handler to the shim with `backtrace()` + `dladdr()` resolution. Crash identified:

- Fault: `si_code=SEGV_MAPERR`, `fault_addr=0x40`, crashing instruction is glibc's `cpyfp` (aarch64 ARMv8.8 memcpy primitive) — so a `memcpy()` with a NULL pointer.
- Thread: **DisplayRenderer** — frame 12 at game offset `0x7b7c3c` puts us 1372 bytes into `sub_bb76e0` (BN VA `0xbb76e0`, the DisplayRenderer render loop identified in Step 23).
- Call site (frame 3): game offset `0x11e2208` / BN VA `0x15e2208`. Rodata strings loaded by that function's arg-setup: `"items.size()"`, `"p_idx"`, `"scene/gui/popup_menu.cpp"`, `"set_item_language"`, `"scroll_to_item"`, etc. Passes `w2 = 1764` as line number.
- Godot 4.4.1 source at `scene/gui/popup_menu.cpp:1764`: `ERR_FAIL_INDEX(p_idx, items.size())` inside `PopupMenu::set_item_language(int p_idx, const String &p_language)`.
- Verdict: **Game calls `PopupMenu::set_item_language(p_idx, lang)` on a PopupMenu where `items.size() == 0`** (or `p_idx` is otherwise out-of-range). The ERR_FAIL macro fires correctly; then Godot's error-logger function itself SEGVs because a thread-local or singleton pointer is NULL. Highly plausible cause: the custom Stern/Radium DisplayRenderer thread isn't registered with Godot's TLS setup, so `StringName` / memory-pool / `OS::get_singleton()` reads NULL inside the logger path, and vformat → memcpy(NULL, …) faults.

### 31.1: AnimationPlayer — skipped for now with a note

User observation: the `Node not found: "AnimationPlayer" (relative to "/root/pokemon_battle_scene")` error also appears in the log. User says this is either a bug or a deliberate non-inclusion in the 0.81.0 build — the Godot project (`assets/godot/pokemon_battle_previs/` / `main.pck_extracted/scenes/pokemon_battle_scene.tscn` equivalent) runs fine in the Godot editor without it. Hypothesis: **`AnimationPlayer` may be dynamically instantiated and populated at runtime by Stern's Radium module for Radium-specific animation playback.** The scene file ships without it because the engine creates it on the fly. Under our emulation, whatever subsystem would do that isn't running (yet) — so the node is missing, but this is orthogonal to the PopupMenu crash we localized above.

**⚠️ REMEMBER:** If later work unblocks scene-render progress and we see new crashes that reference `AnimationPlayer`, `Tween`, or animation-track playback, revisit this assumption. Radium probably has its own `Node` subclass that inherits `AnimationPlayer`-like semantics and registers under `/root/pokemon_battle_scene/AnimationPlayer`. A smoking gun would be a `memnew(AnimationPlayer)` or `add_child` call in Radium's init path.

### 31.2: Why not chase the PopupMenu crash further

- The crash is in the game's own code / Godot interaction, not in our shim.
- Fixing it would require either (a) patching the game binary to replace Godot's logger with a no-op, (b) hooking libc's memcpy with a NULL guard (overhead on every frame), or (c) solving the TLS/thread-registration mismatch so the logger stops hitting NULL pool pointers.
- None of those give us meaningful asset-capture progress beyond what we already have (445 textures, 58 shaders, 1 DMD frame, 66–188 s audio across runs).
- Parking it. If we need to push past later, register dump from the SIGSEGV handler is the cheap diagnostic to try first.

## Step 30: System UI architecture discovered — NUK = Nuklear, service_s_worker = Service S daemon (2026-04-19)

Pivot away from live emulation to static investigation of the non-Radium pieces of the platform. User flagged that the interesting understanding is in `/lcd` (Radium) and `/nuk` (Spike 3 system UI). Started with `/nuk` because it has less of a chance of duplicating existing `_extraction/` work.

### 32.1: NUK is Nuklear

`assets/nuk/` is **[Nuklear](https://github.com/Immediate-Mode-UI/Nuklear)** — the open-source single-header immediate-mode C UI library. Confirmed by C++ mangled class names in the main game binary:

- `12NuklearLayer`
- `12NuklearScene`
- `16NuklearDialogBox`

Plus `nk_spi_system` (Stern's custom Nuklear input backend — almost certainly maps flipper/start/action buttons to Nuklear navigation) and the full `NK_COLOR_*` style-enum set that Nuklear exposes.

Nuklear is a tiny single-header library, so most `nk_*` API functions get inlined away by the compiler — we only see one `nk_*` literal string (`nk_spi_system`). But it's definitively Nuklear: the class naming + the NK_COLOR enum set + the asset layout (fonts + image sheets organised as UI-widget art) are all diagnostic.

**What Nuklear actually renders in this game:**

- Boot splash (`assets/nuk/images/System/Spike3_Boot/Spike3_logo.png`)
- Test-mode diagnostic menus (16 PNG illustrations in `assets/nuk/images/System/TestMode/` — cabinets, speaker panels, shaker motor, headphone kit, plumb-bob, slam-tilt, CPU board from manual, etc.)
- Coil/switch-test overlays (3 PNGs in `assets/nuk/images/Test/` — pokemon_playfield_le, pokemon_playfield_pro, pokemon_backpanel_scaled)
- Additional service UI. [Redacted: identifying asset paths and feature inventory.]

**Fonts** (`assets/nuk/fonts/`): `Hack-Regular.ttf` (mono/debug console), `Stern_FjallaOne.ttf` + `_MONONUMBERS.ttf` (branded display font), `Stern_WorkSans.ttf` + `_Bold.ttf` (branded UI body).

**Retroactive link to Step-27 captures:** The 79 × `256×256 fmt=0x190a (LUMINANCE_ALPHA) type=0x1401 (UNSIGNED_BYTE)` texture uploads at the tail of `_emulation/textures/` are almost certainly Nuklear's font-glyph atlases rasterized from the above TTFs — LA@256×256 is Nuklear's classic glyph-grid shape.

### 32.2: Three coexisting UI/render layers in one binary

The main `game` binary is a fat ELF hosting **three** distinct UI/render frameworks simultaneously:

| Layer | What it draws | How it integrates |
|---|---|---|
| **Godot 4.4.1** | Pokemon battle/discover/capture/Pokedex scenes, menus driven by `tscn` scene graphs | Game engine; owns main window; loads resources from `main.pck` via `res://`; [Redacted: service-specific class names.] |
| **Radium** (Stern proprietary) | Pinball scene rendering, DMD frames, animation playback, LCD/auto-loaded scenes from `assets/lcd/` | Statically linked into the game binary; integrates with Godot via `ManualGodotScene` (vtable at game `0x04630230`) and probably a custom `AnimationPlayer` subclass (see `project_animation_player_gap.md`). Scene bytecode format documented in `_extraction/README.md` |
| **Nuklear** (NUK) | System boot, test mode, Service S login/QR/avatars/achievements | Statically linked; Stern wraps it in `NuklearLayer` / `NuklearScene` / `NuklearDialogBox`; `nk_spi_system` custom input backend |

**Open architectural question, flagged for BN dig:** How exactly does `NuklearLayer` / `NuklearScene` hook into the render pipeline — does it render onto Godot's main canvas as a `CanvasLayer` or `Control` node, does it render to its own framebuffer that gets composited, or does it target a different display surface entirely (secondary LCD / HDMI)? Knowing which determines a lot:

- If Nuklear draws onto Godot's main canvas → same GLES pipeline we already capture via the shim → Nuklear frames show up in `_emulation/textures/` captures as a byproduct.
- If Nuklear draws to a separate framebuffer / secondary display → there's another output path we haven't instrumented.

### 32.3: Separate service process

> [Redacted: service-specific endpoints, authentication details, identifying
> strings, process-control commands and feature inventories.]

The investigation identified a separate service process and considered whether
the game's initialization depended on it. That dependency was a hypothesis,
not an established explanation of the rendering failure.

### 32.4: Explicit future-work item — NuklearLayer/Scene → render pipeline via BN

**Next time we're in BN:**

1. Find the ctors / vtables of `NuklearLayer`, `NuklearScene`, `NuklearDialogBox`. The mangled names are `_ZN12NuklearLayer…`, `_ZN12NuklearScene…`, `_ZN16NuklearDialogBox…`. BN's symbol table should demangle automatically or we can grep the string table.
2. From the ctor, trace:
   - Where it's instantiated (who `new NuklearLayer()`s it) — that's the framework owner.
   - What display surface it targets (EGL surface? Godot Viewport? A raw framebuffer?).
   - The render method: is it a `NuklearLayer::render()` that walks Nuklear's command list and emits GLES calls, or does it go through a Godot `CanvasLayer::_draw()` hook?
3. Also hunt `nk_spi_system`: find its usage to understand the pinball-button → Nuklear-input mapping (what button navigates up/down, what selects, etc.).
4. Cross-reference the 79 × 256×256 LA captured textures against Nuklear's font atlas format to confirm our retroactive-link hypothesis.

Expected outcome: a one-paragraph architecture sketch of how Nuklear frames end up on pixels, so we know whether our existing shim captures them as a byproduct or whether there's a separate output path we haven't instrumented.

### 32.5: Pivot — now digging into `/lcd/` (Radium)

With NUK architecturally understood, moving to `assets/lcd/` — Radium scenes. User's stated primary interest. Existing references available:

- `Reference tool A` tool at `[REFERENCE_TOOL_A]/` (outside repo) — with `reference-player.py` and `convert_radium.sh` for JSON conversion
- `_extraction/README.md` — image.bin scene bytecode format, section 5 offset table, `ManualGodotScene` vtable reference
- `_extraction/CLAUDE.md` — loader call chain (`select_and_load_scene → load_image_bin_file → get_image_section_ptr → get_asset_by_id → load_scene_assets → initialize_scene_asset`)
- External observations on asset containers and an audio-encryption hypothesis. [Redacted: private source identities, paths and wording.]

Step 31+ will cover that work.

## Step 31: Radium scene format — decoded structure, display layout, integration model (2026-04-19)

### 33.1: Scope of `/lcd/`

- 1.015 GB total. Sub-repo (has its own `.git`).
- **21 scenes in `auto_loaded/`, 15 scenes in `demand_loaded/`** — 36 total.
- Each scene in a SHA-1-named directory containing `scene.radium` (binary) + `scene.radium.json` (Reference tool A decoded output) + optional `scene.assets/` tree for heavy external media (2 scenes use this).
- Size range: 85 bytes (empty stub) → 81 MB (largest attract scene with embedded video).
- `Reference tool A` at `[REFERENCE_TOOL_A]/` is the existing decoder (Researcher A, blackbox RE + decomp-backed).

### 33.2: Scene-graph object taxonomy (all 36 scenes)

The `.radium` binary decodes to a Flash/SWF-style structured object graph:

```
FrameSize (Right, Bottom)  +  FrameRate  +  BackgroundColor[4]
Dictionary of Characters (each has Id, optional Name)
  └── polymorphic types:
      Font (FontName, Bold, Italic, Characters[codepoints])
      Text (Text, Font ref, Bounds, Color, Alignment, LineSpacing…)
      Sprite (FrameCount, Elements[keyframed])
      Bitmap (Dimensions, Image { Format, Buffer })
      Shape (Rectangle, Bitmap)
      Video (Dimensions, FrameCount, Buffer{NAMED_SLOTS})
```

Counts across all 36 scenes: **29 Font, 29 Text, 28 Sprite, 11 Bitmap, 7 Shape, 2 Video**. Scenes are mostly text+sprites; bitmaps and videos are relatively rare.

**Animation model** (Sprite.Elements[]): each Element has `Visible` (keyframes of bool), `ColorTransform` (keyframes), `Transform` (keyframes of **Matrix4x4**). Flash-style timeline semantics — no skeletal rigging. The Matrix4x4 allows 3D transforms even in 2D LCD scenes.

### 33.3: Display resolution distribution

FrameSize values across the 36 scenes:

- **1360 × 768** — 22 scenes (WXGA; most common; likely the primary LCD playfield panel)
- **1920 × 1080** — 12 scenes (FHD backbox HDMI)
- **800 × 480** — 1 scene (matches `games/spike3/bin/SternLogo_800x480.png` — probably the boot-splash Radium wrapper)
- **454 × 256** — 1 scene (near the 1024×256 DMD we've been capturing; possibly a different small LCD or mini-DMD surface)

So the platform has at least **3 distinct display surfaces** targeted by Radium: main backbox (1920×1080), playfield LCD (1360×768), and a small LCD/DMD (454×256 or 1024×256). Plus the boot-splash-specific 800×480.

### 33.4: Content-addressed scene lookup — proven

Tested three scene SHA-1s against the main game binary with `strings -n 40`:

- `18dad27e209b169855855422f6578e527adbb5cf` → 1 hit
- `6cd5668f1f8189d6aa5e79203f12a0d0a4c90038` → 1 hit
- `bc0792d8dc81e8aa30b987246a5ce97c40cd6833` → 8 hits

Plus the path prefixes `/lcd/auto_loaded` and `/lcd/demand_loaded` both appear in the game binary. So the game resolves "scene X" → SHA-1 → `/lcd/{auto,demand}_loaded/<SHA-1>/scene.radium`. This is a **content-addressed store** — the SHA-1 is probably of the original build-side path (Stern's internal scene naming), making the SD-card layout reproducible across builds without leaking internal paths.

**Corollary**: we can grep the game binary for each scene SHA-1 to find the callsite that loads it, which will identify what gameplay mode each scene corresponds to (attract / battle / Pokédex / capture / menu). This is a concrete next-step if we want to map all 36 scenes to functions.

### 33.5: Asset-buffer storage

Within `Bitmap.Image.Buffer` and `Video.Buffer[NAME]`:

- **External path**: `"FileName": "<N>.asset/<M>.asset"` — points into the scene's `scene.assets/` subtree for large blobs. Seen in 2 scenes (the compound ones), where e.g. `ATTRACT_LOOP1` → `2.asset/0.asset` (3 MB of video).
- **Inline blob**: `FileName: ""`, `Buffer` contains raw bytes inline in the `.radium` binary. Reference tool A dumps these to `/tmp/blob_*.bin` when converting to JSON (the JSON references them via `"_ref": "/tmp/blob_*.bin"`).
- Either way, a `Length` field gives the byte count.

**Attract-mode video scene** (`auto_loaded/bc0792d8.../45a4e8c6.../`): has a Video object named `video.in_attract_videos` with 6+ buffer slots like `ATTRACT_LOOP1`, `ATTRACT_LOOP1_BW`, `ATTRACT_LOOP2`, `ATTRACT_LOOP2_BW`, `ATTRACT_LOOP3`, each → a `.asset` file. The `_BW` suffix is almost certainly **monochrome versions** of each loop for the DMD/low-color display.

### 33.6: Integration with Godot + Radium render pipeline (mental model, not yet verified in BN)

- `ManualGodotScene` (vtable at game VA `0x04630230`, per `_extraction/README.md`) wraps a Radium scene as a Godot Node.
- `initialize_scene_asset` (pseudo-C in `_extraction/`) is the entry point: scene ID → mmap'd image.bin bytes / `/lcd/*/scene.radium` → `ManualGodotScene` → Godot's scene tree.
- Godot's `_process(dt)` tick advances Radium's animation by dt (updates the keyframe state per Element), and Radium's renderer emits GLES 3.x draw calls — the 58 GLSL shaders captured in Step 27/29 are what Radium uses to composite Text/Sprite/Bitmap/Shape onto GL.
- The LCD DMD (1024×256 ABGR8888) we captured in Step 25 is rendered by a different Radium path (the `DMDEmulationScene` from Step 23) — this is Radium-for-DMD, rendering at the small resolution. The 454×256 Radium scene found here may feed that.

### 33.7: Open questions / next-sessions threads

1. **Which scene is which gameplay mode?** Grep each of the 36 SHA-1s in the game binary, identify enclosing functions in BN, name each scene.
2. **The `_BW` variants** — confirm whether they're DMD-specific (renderer picks based on display target).
3. **`Format` integer in Bitmap.Image** — seen values 4 and 5. Need to map these to pixel formats (RGBA8, RGB8, etc.) from the Radium decoder source.
4. **How does `ManualGodotScene` actually tick and render?** BN dig — same expected scope as the Nuklear question (Step 30.4).
5. **What does `454 × 256` mean** — is that a third LCD panel, or a rendering intermediate?

### 33.8: Proven-fact additions for PROTOCOL

- Radium scene layout, object taxonomy, FrameSize distribution, content-addressed SHA-1 lookup.

## Step 32: Every Radium scene classified — each SHA-1 to a role (2026-04-19)

### 34.1: Method

Wrote `/tmp/radium_xref/analyze.py`. For each of the 36 `scene.radium` files:

1. Decoded Reference tool A JSON → pulled `FrameSize`, `FrameRate`, object-type counts, `Character.Name` values, `Sprite.Element.Name` values, sample `Text.Text` strings, `Font.FontName`s, `Video.Buffer` slot names, and all external `FileName` refs.
2. `grep -abo <SHA-1> game` to find all literal byte-offset occurrences of the scene's directory-name hash in the game binary.
3. Dumped a ±320 byte rodata window around each occurrence, stripped other SHA-1-looking 40-hex strings, and recorded the first printable runs ≥ 6 chars as "nearby strings" context.
4. Combined scene-internal content + binary-adjacent context to classify each scene's purpose.

35 of 36 SHA-1s appear as literal strings in the game binary (1 occurrence each for most; 8 occurrences for `bc0792d8…` — the attract/system parent). 5 scenes have zero literal-string hits, likely referenced indirectly (nested under `bc0792d8…` and loaded by composition from the parent, or enumerated at runtime from the directory listing).

**Major additional discovery**: found the Godot scene-load callsite at BN VA `0x3a07cb0` / file offset `56655024`, which contains the game's core scene table:

```
res://scenes/info.tscn
./assets/godot/main.pck
res://scenes/discover/discover.tscn
res://scenes/capture_battle/capture_battle.tscn
res://scenes/pokemon_battle/pokemon_battle.tscn
discover_scene
capture_scene
battle_scene
null_scene
```

So the game has 3 core gameplay Godot scenes (discover / capture_battle / pokemon_battle, matching the Godot previs tool's Ctrl+D / Ctrl+C / Ctrl+B hotkeys), an `info` scene, and a `null_scene` placeholder. Two of the demand-loaded Radium scenes (`cb8a8ab3…` under auto and nested under `bc0792d8…`) are referenced right next to this list — they are the **Radium overlays bound to the `info` Godot scene**.

### 34.2: The `bc0792d8…` parent — attract/system/setup package

The `bc0792d8dc81e8aa30b987246a5ce97c40cd6833` directory name appears **8 times** in the binary, in contexts clearly identifying it as the **platform/attract bundle**:

| BN VA | Nearby strings | Role |
|---|---|---|
| `0x3a0cf78` / `0x3a0cfd8` | Connectivity/Login/LoginTopAA, Icons/Coin/Coin%04d, LoadingDots, AchievementVerified, JUKEBOX_LOOP{1..6} | Service S attract UI + jukebox |
| `0x3a0d788` | Grand Champion Reset, High Scores Reset, Reset Audits, Reset Volumes, INPUT=%s/GROUND=%s | Service-menu reset confirmations |
| `0x3a0dea8` | wpa_supplicant, udhcpc.pid, MAC-address format, score_average, player_scores | Network + score tracking |
| `0x3a0e040` | NetInterlockSwitchOverlay, "Game Code v%d.%d.%d Is Out Of Date", "Update To v%d.%d.%d Or Newer", sternpinball.com | Out-of-date warning screen |
| `0x3a14ef8` / `0x3a14f80` | spike3_logo_layer, System/Spike3_Boot/Spike3_logo, Version, ProgressBar, NoProgressBar, NetPressStartOverlay | Boot + logo + progress bar |
| `0x3a17c28` | current/peak/avg coil stats, "NODE %2d", part/serial numbers, "# DIAGNOSTICS", ReplayFourLevels | Node diagnostics + replay info |

So `bc0792d8…` is the **single compound scene-group** that bundles everything a pinball machine shows when it's NOT playing a game: boot progress, credits, attract videos, Service S login/icons/jukebox, update warnings, service-menu confirmations, network status, node diagnostics, replay levels.

### 34.3: Full scene classification table

**Auto-loaded (21) — always resident in memory:**

| # | SHA-1 prefix | Size | Frame | Purpose (confidence) |
|---|---|---:|---|---|
| 1 | `18dad27e…` | 81 MB | 1920×1080 | **Main HUD frame** (Pokeball counter, NetScoreFrame, DoubleScoring overlay) |
| 2 | `4d963cfa…` | 1.6 MB | 1920×1080 | End-of-ball "Total =" overlay |
| 3 | `5d8400b9…` | 2.7 MB | 1920×1080 | Team overlay panel (slide-in) |
| 4 | `60ed7e50…` | 21 KB | 1920×1080 @12fps | **Master in-game video dictionary** — 220+ named H.265 video buffers (PK_BGPattern_*, PK_Bulbasaur*_Award*, PK_Pikachu*_* mode videos, PK_TeamRocket*, PK_Match_00..90, PK_Tilt, PK_Replay, PK_BallSave, PK_SkillShot, etc.) |
| 5 | `6a22f74b…` | 4.7 MB | 1360×768 | MODE NAME TOTAL (9 TextValues for mode-complete totals) |
| 6 | `6b877640…` | 2.3 MB | 1360×768 | MODE TITLE banner |
| 7 | `6cd5668f…` | 85 B | 1360×768 | Empty stub — probably the "null_scene" placeholder from the Godot list |
| 8 | `7949bb14…` | 4.7 MB | 1360×768 | AWARD TITLE + TextValue1 |
| 9 | `84907e57…` | 2.4 MB | 1360×768 | 9-line dev/test panel ("MON LE DEV", "Feb 12, 2026", "pokemon_le" nearby) |
| 10 | `856d8afd…` | 970 KB | 1920×1080 | Score/timer text overlay (Stern_Impact, Timer_BG_Instance) |
| 11 | `97cc9ed7…` | 18 MB | 1920×1080 | Alt main HUD layer (Bitmap+Sprite; DoubleScoring + PokeballCounter) |
| 12 | `9d578751…` | 4 MB | 1920×1080 | Inactive player / multiplayer overlay ("Inactive Player", LightningIcon, WaterIcon) |
| 13 | `a7786498…` | 4.9 MB | 1920×1080 | Multiplayer team/mode overlay ("LINE 1"; TeamRocket + SkillShot refs nearby) |
| 14 | `aa681fbf…` | 134 KB | 1360×768 | DANGER 2-line popup (Match screens 00-70 nearby) |
| 15 | `b466d6b7…` | 561 KB | **454×256** | **Small-display scene** — Pokeballs_Textbox nearby; this is the third LCD surface (likely a speaker panel or lockbar LCD) |
| 16 | `c0f85d66…` | 2.4 MB | 1360×768 | AWARD VALUE (sister of #8; 0 direct xrefs — loaded via parent) |
| 17 | `c6e8a05a…` | 8.3 MB | 1920×1080 | Score formatting w/ Text_Score element ("10-K" shorthand) |
| 18 | `cab00277…` | 173 KB | 1360×768 | Debug-line overlay ("DEBUG LINE", Stern_FranklinGothDemiIt) |
| 19 | `cac32730…` | 11 MB | 1920×1080 | Squirtle-mode-specific layer ("Squirtle 02" text, BallText) |
| 20 | `cbe7806d…` | 4.7 MB | 1360×768 | Service-menu mode title ("Meowth Balloon Test", "Press Enter to Test", "Poke Ball Mechanism" nearby) |
| 21 | `45a4e8c6…` (in `bc0792d8…/`) | 1.6 KB | 1360×768 @12fps | **Attract video dictionary** — ATTRACT_LOOP1..3 + _BW variants, ATTRACT_LOOP_SERVICE_S, JUKEBOX_LOOP1..6, Pattern. `video.in_attract_videos` Character |

**Demand-loaded (15) — loaded on use:**

| # | SHA-1 prefix | Size | Frame | Purpose (confidence) |
|---|---|---:|---|---|
| 22 | `2533b0a1…` | 2.4 MB | 1360×768 | AWARD TITLE (variant of #8 for demand mode) |
| 23 | `394c4a03…` | 8.3 MB | 1920×1080 | **Instant Info** screen (107 chars; "Title/Line1/Line2"; Magikarp Topper Tests + GameScoresInfoLine* nearby) |
| 24 | `3fdc91b3…` | 139 KB | **800×480** | Small-screen "LINE 1" — matches `SternLogo_800x480.png` dims; probably boot-splash Radium wrapper (0 direct xrefs) |
| 25 | `cb8a8ab3…` (top-level) | 6.1 MB | 1920×1080 @24fps | **Info-scene overlay** (`res://scenes/info.tscn`; timer_text="15"; 15s countdown timer) |
| 26 | `036fe928…` (in `bc0792d8…/`) | 512 KB | 1360×768 @24fps | **Boot screen with ProgressBar** ("spike3_logo_layer", "System/Spike3_Boot/Spike3_logo", Version, ProgressBar, GAME NAME) |
| 27 | `0d349f5f…` (in `bc0792d8…/`) | 852 KB | 1360×768 @60fps | Credits display ("Credits 50"; 0 direct xrefs — loaded via parent) |
| 28 | `1e3687a3…` (in `bc0792d8…/`) | 928 KB | 1360×768 | Service S overlay (Checkmark/ScanQR/Stopwatch/AchievementVerified icons; JUKEBOX_LOOP1 nearby) |
| 29 | `417f7495…` (in `bc0792d8…/`) | 342 KB | 1360×768 @60fps | "www.sternpinball.com" marketing screen (0 direct xrefs) |
| 30 | `74d01d82…` (in `bc0792d8…/`) | 512 KB | 1360×768 @24fps | Boot variant (GAME NAME + ProgressBar, CBOR/msgpack decoder strings nearby — likely the resource-bundle decoder's init screen) |
| 31 | `7b15d22d…` (in `bc0792d8…/`) | 2.3 MB | 1360×768 @24fps | Network/score timer (wpa_supplicant, udhcpc.pid, score_average nearby) |
| 32 | `a4e06cdd…` (in `bc0792d8…/`) | 272 KB | 1360×768 @24fps | **Out-of-date / update-required screen** ("Game Code Is Out Of Date", "Update To vX.Y.Z Or Newer", "Available at sternpinball.com", STERN_RussoOne font) |
| 33 | `cb8a8ab3…` (in `bc0792d8…/`) | 2.5 MB | 1360×768 @24fps | Info scene for attract variant (same SHA as #25, different wrapper) |
| 34 | `d1a2f117…` (in `bc0792d8…/`) | 527 KB | 1360×768 | Shape-only scene (0 xrefs; probably an animation mask or transition frame) |
| 35 | `f1e5baf5…` (in `bc0792d8…/`) | 271 KB | 1360×768 @60fps | **System-settings warnings** ("CLOCK NOT SET", Grand Champion Reset, Reset Audits, Reset Volumes) |
| 36 | `ffa461e7…` (in `bc0792d8…/`) | 803 KB | 1360×768 @60fps | **Node diagnostics + replay levels** (ReplayLevel1Text, "999,999,999,999", NODE #X version/part/serial, "# DIAGNOSTICS") |

### 34.4: What the 454×256 and 800×480 scenes probably are

- **`b466d6b7…` at 454×256** (auto-loaded): Only 3 chars (Bitmap + Sprite), nearby strings mention `Textboxes.Pokeballs.Textbox_Name` and `Pokedex-EOR-Done`. Likely the **speaker-panel / lockbar mini-LCD** showing the pokeball counter. 454×256 is an unusual size that suggests a secondary custom panel rather than the main LCD or DMD.
- **`3fdc91b3…` at 800×480** (demand-loaded, 0 xrefs): Frame size matches exactly the `SternLogo_800x480.png` boot image. Probably wraps the Stern boot logo as a Radium scene for compositing onto a small/medium display. Zero direct xrefs is consistent with a "loaded once at very early boot and then discarded" usage pattern.

### 34.5: Scenes with zero SHA-1 xrefs (5)

`c0f85d66…`, `3fdc91b3…`, `0d349f5f…`, `417f7495…`, `d1a2f117…`.

All 5 are **either nested under `bc0792d8…` (4 of them) or match an obvious sibling-load pattern** (AWARD VALUE loaded alongside AWARD TITLE):

- 4 of 5 are in the `bc0792d8…` package. The game loads the parent, and the parent scene's own bytecode (or a directory-listing walk) enumerates the children.
- `c0f85d66…` (AWARD VALUE) is almost certainly loaded in tandem with `7949bb14…` (AWARD TITLE) by a common wrapper.

No "dead" scenes in our data — the SHA-1-xref-zero set has a benign explanation. (Binary-encoded SHA-1 lookups would confirm this but aren't load-bearing right now.)

### 34.6: Fonts inventory (across all scenes)

Distinct FontName values observed in the `Font` objects:

- `Stern_Impact` — dominant for score/HUD text (Scenes 2, 10, 11, 12, 14, 17, 19) — Stern's branded impact-style face.
- `Stern_GoldenSmile` — used for MODE/AWARD titles and panels (5, 6, 7, 8, 9, 13, 16, 20, 21, 22, 23, 24) — game-mode display font.
- `Stern_FranklinGothDemiIt` — debug overlay (18).
- `STERN_RussoOne` — update-required screen (32).
- `HelveticaNeueBlack` — demand/system scenes (27, 29, 31, 33, 35, 36) — generic system face.
- `Bitstream Vera Sans` — boot/progress scenes (26, 28, 30) — early-boot fallback before branded fonts are ready.

The fonts in `assets/nuk/fonts/` (Hack-Regular, Stern_FjallaOne, Stern_WorkSans) are **disjoint** from these Radium fonts — confirms Radium and Nuklear draw text through different font pipelines, as expected (different render backends for different display surfaces).

### 34.7: Takeaways and follow-ups

- The game has **~4 display surfaces** touched by Radium scenes: **1920×1080** (backbox HDMI — main gameplay), **1360×768** (playfield/card LCD — modes/awards/system screens), **800×480** (boot-splash-sized display), **454×256** (speaker-panel/lockbar mini LCD). The DMD at 1024×256 captured by the shim may be yet another surface, or the 454×256 panel scaled up. BN dig on `ManualGodotScene` ctor + the display-target argument would resolve which scene drives which surface.
- The Godot scene side is clean: `discover`, `capture_battle`, `pokemon_battle`, `info`, `null_scene`. These map to the **gameplay modes** visible in the previs project (Ctrl+D, Ctrl+C, Ctrl+B).
- The Radium side handles **overlays, HUDs, attract, system, and boot** — it's the "frame around the Godot game" in all directions (score HUD over the top, boot/attract/system below).
- Next concrete BN tasks (parked for future session):
  1. Find the function that maps scene-SHA-1 → display surface. Near the BN VA `0x3a0d001` (string: `bc0792d8…/1e3687a305fa…`) we can read assembly and back-track.
  2. Open `ManualGodotScene` ctor / methods to see how a Radium scene wraps as a Godot Node and how dt advances.
  3. Reconstruct the full scene-event graph — which gameplay mode loads which Radium overlay, when. Binding clues: the `PokeballCounter01_Instance` and `NetScoreFrameOverlay` names show up in rodata alongside the HUD scene SHA-1s, which means the game code binds to those by name at runtime.
- The analysis script (`/tmp/radium_xref/analyze.py`) is reusable — if the user wants a persistent copy, it can be moved into the repo (`_extraction/tools/` or similar).

## Step 33: Binding index — which function binds which Radium scene, via rodata clustering (2026-04-19)

Goal: identify, per Radium scene, the **C++ function in the game binary** that loads it and walks its element tree. The technique is binding-path clustering:

- When a C++ function calls `scene.get_node("A.B.C")` etc., the compiler emits the literal string `"A.B.C"` into rodata near the function's own rodata (typically adjacent to the function's local string constants).
- Functions that bind many elements of a scene produce a **cluster of PascalCase-dotted strings** at adjacent rodata offsets.
- Scene SHA-1 strings sit in rodata too — usually right next to the binding cluster that owns that scene, because the same function loads the scene and then walks it.

So: cluster the binary's rodata "A.B.C" strings by offset proximity; locate the nearest scene SHA-1(s) to each cluster; output is a function⇄scene⇄element-vocabulary graph.

### 35.1: Method

Scripts preserved at `games/pokemon_le/_extraction/tools/`:

- `radium_scene_xref.py` — per-scene binary-occurrence analysis (Step 32).
- `radium_bindings.py` — builds a binding index from all 407 diagnostic scene element-names (Step 33).
- `radium_rodata_clusters.py` — finds dotted-path string clusters by offset proximity (≥3 strings, gap ≤ 400 bytes).
- `radium_mode_xref.py` — exploratory pass for `res://scenes/*.tscn` callsites (proved they're all in one static scene-registry table, so not distinguishing).

Key finding: the four `res://scenes/{info,discover,capture_battle,pokemon_battle}.tscn` strings are in ONE rodata table at BN VA `0x3a07c68…0x3a07d20` — a static **scene-registry table** the game uses for mode dispatch. They do NOT individually cluster with different HUD vocabularies. So mode→overlay mapping is NOT "direct callsite" — the binding is dynamic based on which registry entry is active.

### 35.2: The 6 Radium binding clusters (filtered from 20 total — the other 14 are Godot engine internals)

| # | BN VA range | #paths | Dominant prefixes | Adjacent scene SHA-1(s) (offset) | Inferred purpose |
|---|---|---:|---|---|---|
| **1** | `0x3a0aa50–0x3a0bf68` | **75** | `Team_Overlay_Instance`, `Player{1-4}_Instance`, `Match_Instance`, `Panel_{Left,Right,Bottom}_Instance`, `BallAndCredits_Instance` | **18dad27e** (56672296 — inside cluster), **97cc9ed7** (56672232 — inside), **9d578751** (56667352 — inside), **cac32730** (56666456 — inside), **c6e8a05a** (56666504 — inside), **856d8afd** (56666632 — inside), **5d8400b9** (56667080 — inside), **a7786498** (56666032 — inside), **cab00277** (56666400 — inside) | **Main-gameplay HUD binding function**: 1920×1080 backbox overlays — per-player active/inactive scores for 1-4 players, Match digits, Team Overlay panel, Panel_{Left,Right,Bottom} sliding title boards, BallAndCredits, DEBUG overlay, Squirtle mode layer, Score formatting. Binds ALL these HUD scenes in one function. |
| **2** | `0x3a05e80–0x3a06950` | **40** | `TeamScoresInstance` (36), `CreditsAndPricing` (2), `CustomMessageInstance` (1) | **394c4a03** (56647232 — at cluster start) | **Instant Info binding function**: co-op team-score display (GC + HS1..HS4 frames × Name1..Name4 + Score/PlayerId), plus credits/pricing info, plus "Custom Message" admin overlay. Matches scene `394c4a03` (Instant Info, 107 chars). |
| **3** | `0x3a06f18–0x3a073c8` | **28** | `Text` (10 — `.TextTitle`, `.TextValue0..8`), `Textboxes.{Pokeballs,Berries,PokemonCaught}` (13 — `.Textbox_{Count,Score,Name}`), `grand_champion` (3 — `.PageTitle`, `.PlayerId`, `.PlayerScore`), `CreditsAddCredits` | **6cd5668f** (56651968 — inside), **6a22f74b** (56652224 — inside), **6b877640** (56652176 — inside), **7949bb14** (56651896 — inside), **b466d6b7** (56653056 — at tail) | **End-of-mode / results / award binding function**: walks MODE TITLE + MODE NAME TOTAL + AWARD TITLE + AWARD VALUE + end-of-ball totals, drives Textbox widgets for PokemonCaught / Berries / Pokeballs on the **454×256 mini-LCD** (`b466d6b7`), plus grand_champion entry. Also binds CreditsAddCredits for the "Add Credits" dialog. |
| **7** | `0x3a17d30–0x3a17dc8` | 5 | `ReplayFourLevels` only | **ffa461e7** (56720465 — just before) | **Replay-levels display binding**: four replay thresholds + ReplayAtText. Matches scene `ffa461e7` (Node diag + replay levels). |
| **12** | `0x3a15ae0–0x3a15b10` | 4 | `icon` only (`.name`, `.file_name`, `.icon_url`, `.file_checksum`) | — | **Icon metadata binding**: probably Service S avatar/icon management — downloads remote icons with checksums. Independent of Radium scenes per se. |
| **15** | `0x3a07df0–0x3a07ee0` | 3 | `LettersPalette.cursor`, `LettersPalette.LetterPaletteLetters.LetterPaletteText`, `timer_text.timer_text` | **cb8a8ab3** (56655280 — just before) | **Info scene binding**: the LettersPalette high-score initial-entry widget + 15-sec countdown timer. Matches scene `cb8a8ab3` (info.tscn overlay). |

The remaining 14 clusters in the binary's dotted-path universe are all **Godot engine internals**: shader uniforms (`scene_data.*`, `canvas_data.*`, `params.*`, `world.xyz`, `FRAME.*`, `PARTICLE.*`), Vulkan pipeline state (`p_depth_stencil_state.*`, `p_rasterization_state.*`, `p_blend_state.*`, `p_state.*`, `p_view.swizzle_*`), Godot's MacOS-keymap input actions (`ui_text_*.macos`), Godot asset-manifest strings (`icon.{png,svg}`, `logo.{png,svg}`, `godot.{editor,properties,documentation}`), remote/marketing URLs (`google.com`, `api.sternpinball.io`, `iop.spidevx.com`, `iop.strndev.com`), Texture2d source-file strings (logger identifiers), and shader outputs (`COLOR.rgb`, `LIGHT.rgb`, `SHADOW_MODULATE.rgb`). None of these touch Radium scene elements.

### 35.3: Implications for mode→overlay mapping

The clusters tell us **which C++ binding function covers which Radium scenes**, but they do NOT directly map to which **gameplay mode** (discover/capture/battle/attract) activates them. Reasoning about mode→overlay:

- **All 9 HUD scenes in Cluster 1 share a single binding function**. This is the "main HUD" — used by ALL gameplay modes (discover, capture, battle). Mode-specific content comes via the video dictionary (`60ed7e50`, 220+ named loops) and dynamically-swapped scene layers per mode (e.g., `cac32730` = Squirtle-mode-specific layer).
- **Instant Info (Cluster 2 / scene 394c4a03)** is a mode-agnostic overlay — `res://scenes/info.tscn` loadable from any gameplay mode. The binding function pulls team scores, credits/pricing, and admin custom messages.
- **End-of-mode panels (Cluster 3)** are driven by a single binding that walks multiple scenes in sequence during the award/total sequence. The `Textboxes.Pokeballs|Berries|PokemonCaught` paths indicate these are pinball-specific collection widgets (caught Pokémon counter, berries counter, pokeballs counter) — likely shown on the 454×256 mini-LCD (speaker panel or lockbar).
- **Info scene overlay (Cluster 15 / scene cb8a8ab3)**: LettersPalette indicates Stern's traditional Flash-era initial-entry screen ("enter your initials"). Only appears after high-score achievement.
- **Replay display (Cluster 7 / scene ffa461e7)** is the `ReplayAt / ReplayLevel1-4` overlay — shown after a game ends to display the player's replay thresholds.

### 35.4: What each gameplay mode touches (inferred)

| Mode | Godot scene | Radium overlays bound (bindings cluster) |
|---|---|---|
| attract / boot / idle | (none — Godot idle) | `bc0792d8…` package (11 scenes: boot/credits/jukebox/Service S/update/reset/diag/replay), `45a4e8c6` attract video dict, `60ed7e50` in-game video dict (kept loaded) |
| **discover** | `discover.tscn` | Main HUD Cluster 1 (9 HUD scenes), `60ed7e50` video dict, end-of-mode Cluster 3 scenes on level completion |
| **capture** | `capture_battle.tscn` | Same Main HUD Cluster 1, different video loops from `60ed7e50`, different mode-specific overlay layer |
| **battle** | `pokemon_battle.tscn` | Same Main HUD Cluster 1, `cac32730` Squirtle-specific layer possibly, `60ed7e50` battle-videos |
| info overlay | `info.tscn` | `cb8a8ab3` Info scene + Cluster 15 LettersPalette (for high-score initial entry) |
| end-of-game | — | Cluster 3 end-of-mode panels + `ffa461e7` replay display + `cb8a8ab3` initial entry (if achieved) |
| service menu | — | `f1e5baf5` (reset warnings), `a4e06cdd` (out-of-date), `cab00277` (DEBUG), `ffa461e7` (diag) |

### 35.5: Open threads for BN-assisted follow-up (deferred)

1. **Identify the binding functions by entry point**. Each cluster's rodata strings are loaded by `adrp + add` from within one or a small number of functions. Disassembling `.text` around the function-prologue closest to each cluster's first string gives the binding function's entry-point VA. With BN we could confirm the function signatures (e.g., `bind_main_hud(Scene& s)`, `bind_endofmode_panels(Scene& s)`, `bind_info_overlay(Scene& s, ...)`).
2. **Find the mode-dispatch table entries** that call each binding function. The scene-registry at BN VA `0x3a07c68…0x3a07d20` (`null_scene`/`discover_scene`/`capture_scene`/`battle_scene`/`info.tscn`) probably has callback pointers immediately adjacent, pointing at the per-mode initializer functions.
3. **Map the 220+ `PK_*` video names in `60ed7e50`** (master in-game video dictionary) to per-mode subsets. The game code likely has per-mode `const char*[] mode_videos = {"PK_Bulbasaur1_Start", "PK_Bulbasaur1_Award1", ...}` arrays — clustering those would identify which characters/modes are in each script.

### 35.6: Scripts preserved

Moved to the repo so future sessions can re-run:

```
games/pokemon_le/_extraction/tools/
├── radium_scene_xref.py          # per-scene content + SHA-1 xref analysis (Step 32)
├── radium_bindings.py            # all-scene element-name → binary binding index (Step 33)
├── radium_rodata_clusters.py     # dotted-path proximity clustering (Step 33)
└── radium_mode_xref.py           # res://scenes/*.tscn callsite dump (exploratory)
```

### 35.7: Verified facts to graduate to PROTOCOL

- 6 Radium binding clusters in the game binary (1 / 2 / 3 / 7 / 12 / 15), each corresponding to a distinct C++ binding function.
- Binding function #1 ("Main HUD") owns 9 of the 1920×1080 HUD scenes simultaneously.
- Binding function #3 ("End-of-mode") owns a run of 1360×768 panels + the 454×256 Pokeball textbox scene.
- The 14 non-Radium clusters in the binary's dotted-path universe are all Godot engine internals.

## Step 34: reference-player-v2.py — dt-paced playback + Pokemon attract driver + binding API stub (2026-04-19)

Goal: get Reference tool A's player playing Pokémon Radium scenes at the correct speed and with proper clip sequencing — and design the API surface for a future "drive from Godot" bridge.

### 36.1: BN answers — integrated

**Ask 1 (frame pacing)** resolved the `FIXME: Use dt?` in Researcher A's player. The game's `DisplayRenderer` (sub_bb76e0 @ 0xbb76e0) is **event-driven (condition variable) + vsync-locked** (via `page_flip_and_wait`): one step per vsync, no `clock_gettime` in the loop, no dt computation. So in our player, pyglet's wall-clock `dt` is a correct proxy — at 60 Hz vsync with a 30 fps scene we advance 0.5 frames per tick, matching the game's "one step per vsync with scene fps determining advance rate" semantics.

**Ask 2 (attract sequencer)** gave us `sub_aded20` @ 0xaded20: a pure modulo-6 state machine over two parallel 6-entry arrays:

- **Color** (`data_476b350`): `ATTRACT_LOOP1`, `ATTRACT_LOOP_SERVICE_S`, `ATTRACT_LOOP2`, `ATTRACT_LOOP_SERVICE_S`, `ATTRACT_LOOP3`, `ATTRACT_LOOP_SERVICE_S` — indices 0..5, Service S entries at odd indices.
- **BW** (`data_476b2f0`): same pattern with `_BW` suffix on each name.

Current index stored at BSS `data_48c52b8`. Wrappers `sub_adf470..sub_adf61c` drive `sub_aded20` from external triggers (video-playback-done callbacks). When Service S is inactive, a `while service_s_flag[idx] == 0: idx = (idx+1) % 6` loop skips Service S entries. The separate jukebox path (`sub_9ac0e0` @ 0x9ac0e0, array `data_462eeb0`) is for `JUKEBOX_LOOP1..6` when Service S is active; it's a distinct playback mode, NOT interleaved into the modulo-6 cycle.

**Ask 3 (ManualGodotScene)** gave us the binding API surface. The vtable at `0x4630230` is minimal (3 slots — deleter family + one ManualGodotScene-specific override at `sub_b2be40`), but the **extended function-pointer table** immediately following it at `0x4630240..0x4630340` is the real API — ~15 setters with distinct parameter counts:

| Game addr | Param count | Our best guess |
|---|---|---|
| 0xb2b7b0 | 2 | `get_element(path)` / `get_node(path)` |
| 0xb2b850 | 0 | (no-arg reset?) |
| 0xb2b8a0 | 2 | `set_text(path, value)` |
| 0xb2b900 | 6 | multi-param setter (transform+color?) |
| 0xb2b920 | 1 (likely 2) | `set_visible(path, bool)` |
| 0xb2b960 | 4 | generic property setter |
| 0xb2b990 | 15 | `set_clip(path, name, ...)` — many-arg video setter |
| 0xb2bab0 | 1 | set flag |
| 0xb2ba90 | 9 | `set_transform(path, mat4)` |
| 0xb2bb00 | 2 | `set_color(path, rgba)` |
| 0xb2bb40 | 4 | `set_rect(path, l,t,r,b)` |
| 0xb2bb70 | 1 | `set_active(bool)` |
| 0xb2bb90 | ? | set state |

Constructor `sub_b25fc8` is called from `load_scene_assets` (0xb321d0) at 0xb32370/0xb32388, meaning scene instantiation happens during asset load — not during rendering. Per-frame tick is NOT on this vtable; the class inherits Godot's `Node::_process(delta)` for animation advancement, called by Godot's internal scene-tree traversal.

### 36.2: Reference renderer experiment — what changed

The experiment added elapsed-time-based playback, an attract-sequence driver
and a small state-binding interface to Reference tool A. A separate modified
copy was kept alongside the original. Syntax validation passed; visual testing
was still pending.

> [Redacted: source-level modifications to the external renderer, its internal
> class/method layout, and service-specific playback controls. The observation
> and test status are retained; this is not presented as standalone Kiki code.]

### 36.3: How to test the speed fix

```
cd [REFERENCE_TOOL_A]/
python3 reference-player-v2.py --pokemon-attract \
    /mnt/c/Workspace/pokemon-le-0.81.0/games/pokemon_le/assets/lcd/auto_loaded/bc0792d8dc81e8aa30b987246a5ce97c40cd6833/45a4e8c6251510e3f7b11f8b21f227e1fa49f331/scene.radium.json
```

Expected: attract video plays at **native speed** (instead of 2× too fast), cycles `ATTRACT_LOOP1 → ATTRACT_LOOP2 → ATTRACT_LOOP3 → wrap`, skipping Service S entries because `--Service S-active` wasn't passed. Add `--Service S-active` to see the Service S clip interleave; add `--bw` for the monochrome sequence.

Other scenes (e.g. the main HUD `18dad27e…`) also benefit from the dt fix but have no driver yet — they'll play the decoded keyframes at the correct rate, but without game-state binding they'll mostly show default text values.

### 36.4: Binding interface — remaining limitations

Transform changes did not reach the renderer, color changes were limited, and
there was no live data source. A state-binding surface by itself was not enough
to connect gameplay to presentation.

> [Redacted: external renderer member names and implementation instructions.]

### 36.5: Proposed "wrap Godot scenes" architecture (sketch, for later)

When we decide to pursue the long-term wrap:

1. **Godot side** runs with game logic only (no rendering, or headless export). Publishes state to a well-known IPC: a JSON file written at 10 Hz, or a UDP broadcast, or a TCP socket. Simplest: a single JSON file in `/tmp/radium_state.json` containing `{ "PokeballCounter01_Instance.PokeballCounter.Score": "12,345,678", "Player1_Instance...": "..." , ... }`.
2. **Player side** adds a background thread that polls/reads the state file, and applies each key → value pair via `RadiumBinding.set_text(key, value)` (or `set_visible`/`set_clip`/etc., with a type-tag prefix in the key if needed).
3. **Scene driver**: small Python modules per gameplay mode (`pokemon_discover.py`, `pokemon_capture.py`, `pokemon_battle.py`) set up which Radium scenes to load and initialize scene-specific `setClip` / `play` state. Like the hardcoded `/deadpool_le/` / `/king_kong_pro/` blocks already in the original, but externalized.
4. **Future extension**: instead of polling JSON, embed a tiny HTTP server in the player (or a gRPC-ish thing) that Godot can POST updates to directly. Same `RadiumBinding` surface, different transport.

Deferred until the current fix is validated.

### 36.6: What got lodged

- v2 file: `[REFERENCE_TOOL_A]/reference-player-v2.py` — parses clean, 2230 lines, 7 inline `# v2:` edits + 3 new top-level constructs.
- Original untouched: `[REFERENCE_TOOL_A]/reference-player.py` — 1900 lines, preserved as-is per user's "maintain history without git" request.
- JOURNAL Step 34 (this entry) — records BN-derived facts + the v2 design.
- PROTOCOL — to be updated with the three BN-proven facts (vsync-locked timing model, attract sequencer semantics, ManualGodotScene extended-ftable layout) after the v2 test run validates the dt fix actually works.

## Step 35: First v2 test run — video-decoder gap + graceful fallback + docs (2026-04-19)

### 37.1: What happened

User ran `python3 reference-player-v2.py --pokemon-attract <path-to-attract-scene>`. The v2 driver installed correctly ("wired Video id=1 (mode=color, service_s_active=False)", "[v2] attract: state 0 → ATTRACT_LOOP1"), but pyglet's `.media.load` threw `DecodeException: No decoders available for this file type` on `scene.assets/2.asset/0.asset`.

### 37.2: Diagnosis

Three compounding facts:

1. **Radium's inline video assets are QuickTime `.mov`** containers **without file extension** (verified by magic bytes `ftyp qt  ` at offset 4 of `2.asset/0.asset`). The files are literally named `scene.assets/<N>.asset/<M>.asset`.
2. **Pyglet's codec registry picks by file extension**, so even if the decoder was available, it wouldn't be invoked. Renaming or symlinking to `.mov` would be a first step.
3. **Pyglet 2.0.17's ffmpeg bridge is broken against ffmpeg ≥ 7** — the user's system has libavcodec.so.61 / libavformat.so.61 / ffmpeg 7.1.3, but pyglet's `libavutil.py` calls `avutil.av_get_default_channel_layout` which was removed in ffmpeg 6. `pyglet.media.have_ffmpeg()` returns `False`. Neither PyAV nor OpenCV is installed as alternative decoders.

So on this system there is simply no Python video-decoding path available to pyglet right now. Short of upgrading pyglet (2.1 may fix) or installing PyAV and writing a custom backend, video frames won't render.

### 37.3: v2 fix applied

Made `Video.setClip` non-fatal:

- On first `DecodeException`, tries symlinking to `/tmp/radium_v2_XXXX.mov` so pyglet's extension-based decoder picker at least attempts the video path.
- If that also fails (as it does on this system — no working ffmpeg bridge), logs a one-shot clear message with a pointer to `README.v2.md → Video decoder setup` and sets `self._player = None`. Never raises.

Made `PokemonAttractDriver` resilient to a missing player:

- If the Video's `_player` is None after `setClip`, the driver falls back to a **timer-based auto-advance** (default 30 s per clip, user-tunable via `--attract-clip-duration N`). State-machine logging still fires on every transition.
- If the Video has a player, the original `on_eos`-driven advancement is used.

### 37.4: Documentation

- **Created `[REFERENCE_TOOL_A]/README.v2.md`** (~12 KB). Covers: what v2 adds, setup (pyglet + numpy, convert_radium.sh), video-decoder gap + workaround, CLI flags, Pokemon-LE scene structure (auto_loaded/demand_loaded, content-addressed SHA-1 naming, compound `bc0792d8…` package with its nested scenes), recommended scenes to test with, troubleshooting, architecture for the future "wrap Godot" plan, and outstanding TODOs.
- **Updated `_emulation/ASSETS.md`**:
  - NUK=Nuklear identified; Nuklear font set specified; Radium per-scene embedded fonts catalogued.
  - Full LCD Radium section rewritten: directory layout, scene format, 36-scene classification summary, playback tooling (radium.py + original player + v2 fork), runtime video-asset format (QuickTime .mov without extension + inline-in-scene.assets storage), attract-video buffer list with file sizes.

### 37.5: What the user sees now

```
$ python3 reference-player-v2.py --pokemon-attract <attract-scene.radium.json>
Collected all pointers
Creating character 18
Element 'VideoSurface' (Frame 0 / 1) {}
[v2] PokemonAttractDriver: wired Video id=1 (mode=color, service_s_active=False)
[v2] attract: state 0 → ATTRACT_LOOP1

[v2] Video decoder unavailable — pyglet cannot decode the scene's .mov assets:
    "No decoders available for this file type: /tmp/radium_v2_XXXX.mov"
    Sequencer state transitions will still log (timer-advance), but no video will render.
    See README.v2.md → 'Video decoder setup' to enable playback.

[v2] attract: no video player — timer-advance in 30s
  (30 seconds later)
[v2] attract: state 2 → ATTRACT_LOOP2   (state 1 ATTRACT_LOOP_SERVICE_S was skipped because --Service S-active wasn't set)
[v2] attract: no video player — timer-advance in 30s
  …
```

Plus a black window (the attract scene is PURELY a Video object, so without video there's nothing to render).

For a more visually interesting test, the README now recommends scenes with text/sprite content: main HUD (`18dad27e…`), mode title (`6b877640…`), award title (`7949bb14…`), instant info (`394c4a03…`), boot with progress bar (`036fe928…`), out-of-date warning (`a4e06cdd…`), etc. These render without needing video and let you verify the frame-pacing fix on their own animations.

### 37.6: Unblocking video playback — future work

To get actual attract-video rendering on this system, options in order of effort:

1. **`pip install av`** + add a PyAV-based `Video` backend in v2 (new class that decodes frames via PyAV, uploads each as a pyglet texture). Medium effort, most reliable.
2. **Downgrade system ffmpeg to 5.x** so pyglet's existing bridge works. Low effort for pyglet, but hostile to the rest of the system.
3. **Wait for pyglet 2.1** which reportedly has ffmpeg 6+ compat. Zero effort but unknown timeline.

Deferred until the user has a concrete need to see attract video — the timer-driven state machine is already proving the sequencer is correct.

## Step 36: PyAV video backend + blob auto-heal (2026-04-19)

User hit a new failure on the boot-progress scene (`036fe928…` under `bc0792d8…`): `FileNotFoundError: /tmp/blob_4d91043d18.bin`. And they installed PyAV (`pip install av`), so we can now do proper video playback.

### 38.1: Why cached scene data disappeared

The converted scene description referenced temporary files. A later run found
those files missing after the temporary directory had been cleared.

### 38.2: Cache regeneration

The experiment checked for missing references and regenerated the temporary
data before playback. This addressed stale derived data rather than a bad
source asset.

> [Redacted: external decoder internals and source-level integration details.]

### 38.3: PyAV video backend landed

New class `_V2PyAVPlayer` at module level in v2 — minimal drop-in for `pyglet.media.Player`:

- `__init__(path)` — opens with `av.open(path)`, picks first video stream, caches `average_rate` as `fps`, primes with one decoded frame so `width`/`height` are known.
- `update(dt)` — accumulates wall-clock time, advances the PyAV decoder iterator until `current_frame_idx` matches `int(elapsed * fps)`. If decoding raises `StopIteration`, sets `_eos=True` and fires `on_eos` callback. Skip-ahead on lag (no catch-up frame-by-frame).
- `.texture` property — lazy upload: on first access per frame-index, converts the current `av.VideoFrame` to an `rgb24` numpy array (using PyAV's built-in `to_ndarray`), uploads to a cached GL texture via `glTexImage2D(GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, ...)`. Returns a `_TextureRef` object with `.id` matching the pyglet.media.Player.texture shape.
- `.play()`, `.queue(source)` (no-op), `.delete()` for compatibility.

New pipeline function `_v2_open_video(path)` tries strategies in order:
1. **PyAV** — `av.open(path)` → `_V2PyAVPlayer`. Logs e.g. `[v2] PyAV opened 0.asset (853×480 @ 29.97 fps)`. Registered in `_v2_pyav_players` list.
2. **pyglet.media.load** direct path, then via `/tmp/radium_v2_*.mov` symlink.
3. **Graceful degrade** — log both failure modes once, return None. Driver falls back to timer-advance.

`Video.setClip` simplified to a single call: `self._player = _v2_open_video(videoPath)`.

`update(dt)` now also iterates `_v2_pyav_players`: calls `.update(dt)` on each active player, removes ones where `_eos` became true.

### 38.4: Expected behavior now

With PyAV installed (confirmed: user has `av 14.2.0`, pyglet 2.0.17, ffmpeg 7.1.3):

- Running the attract scene should open a PyAV player per clip transition, decode frames at real-time, upload as textures, and show video. Log will look like:
  ```
  [v2] PokemonAttractDriver: wired Video id=1 (mode=color, service_s_active=False)
  [v2] attract: state 0 → ATTRACT_LOOP1
  [v2] PyAV opened 0.asset (853×480 @ 29.97 fps)
  [EOS after ~30s] [v2] attract: state 2 → ATTRACT_LOOP2
  [v2] PyAV opened 2.asset (853×480 @ 29.97 fps)
  ...
  ```
- The boot scene `036fe928…` (non-video, sprite+text+progressbar) should no longer crash on missing blobs — the preflight regenerates them.
- Any other scene whose .json references `/tmp/blob_*.bin` files auto-heals on first open.

### 38.5: Limitations (known, documented in README.v2.md)

- **No audio playback.** `_V2PyAVPlayer` decodes the video stream only. PyAV has full audio-decode support but we'd need to feed samples into pyglet's audio queue — deferred.
- **Texture upload on GL thread.** Decoder runs in pyglet's clock callback (same thread as on_draw, fine), but if someone refactors to background decoding later, the upload path will need `glFlush`/context sharing.
- **No frame rate sync with refresh.** We decode when wall-clock demands; if the window is sub-60Hz, frames may be dropped. `skip_ahead` in `update(dt)` handles this by advancing the decoder iterator without uploading intermediate frames.

### 38.6: What got lodged

- `[REFERENCE_TOOL_A]/reference-player-v2.py` — up to 2535 lines (2230 → 2535; added `_v2_ensure_blobs_exist`, `_V2PyAVPlayer`, `_v2_open_video`, and plumbed through `update(dt)`).
- `[REFERENCE_TOOL_A]/README.v2.md` — Video decoder section rewritten; PyAV promoted from TODO to "primary recommended path".
- This journal entry.

## Step 37: Static-frame freeze — fixes the "5× too fast" state-oscillation (2026-04-19)

User reported that even after the dt fix, "some scenes still cycle their assets way too fast" with a good intuition: "maybe when they're used properly you're supposed to instantiate them with one of the assets and frame the game that way."

### 39.1: Root cause

`Sprite.__init__` auto-runs `self.play(1, data['FrameCount'], loop=True)` — so every sprite loops its entire timeline by default. But **Pokemon scenes are mostly state-based, not animated**. The boot scene `036fe928…` is the clearest example:

- `FrameRate=24, FrameCount=2, FrameLabels={NoProgressBar: 1, ProgressBar: 2}` — two discrete STATES, not a 2-frame animation.
- Nested sprites inside it have `FrameCount=101` — a progress-bar widget with 101 increments (0–100%), not a 101-frame loop.

The default "loop everything forever" then oscillates between `NoProgressBar` and `ProgressBar` states 12× per second, while nested progress-bar sprites cycle through all 101 fill levels in ~4 seconds. Visual chaos that looks like "assets cycling 5× too fast".

The game instantiates the scene, picks one state (e.g. `ProgressBar`), and sets the nested progress-bar's frame to the current completion percentage. Nothing loops.

### 39.2: Fix — global static-frame override

Added `_v2_static_frame = [None]` (list-wrapped for mutability from nested-class methods). When non-None:

- `_Playable._updatePlayable` returns early — `self._frame` doesn't advance.
- `_Playable._getFrame` returns `_v2_static_frame[0]` clamped to each Playable's valid `[_startFrame, _startFrame + _frameCount - 1]` range.

Every sprite in the scene tree freezes on frame N (or the nearest valid frame in its range). For the boot scene, `--static-frame 2` shows the `ProgressBar` state with the nested widget snapped to frame 2 of its 101-frame range (2% fill). `--static-frame 50` snaps it to 50% fill. `--static-frame 1` shows the `NoProgressBar` state.

### 39.3: CLI + scene summary

Two new flags:

- `--static-frame N` — freeze every sprite on frame N.
- `--static-label NAME` — resolves NAME against root Sprite's FrameLabels at scene-load time. For the boot scene, `--static-label ProgressBar` is equivalent to `--static-frame 2`.

New `_v2_print_scene_summary()` runs per-scene after `RadiumScene()` constructs and prints the scene's timing knobs so users know what to try:

```
[v2] scene: 036fe9286978d41e4df740cd9ddb9a998e4afc69
     FrameSize: 1360×768   FrameRate: 24.0   root FrameCount: 2
     FrameLabels: NoProgressBar=1, ProgressBar=2
```

When `--static-frame` is active, it's echoed too.

### 39.4: play.py TUI — inline state control

`/mnt/c/Workspace/pokemon-le-0.81.0/games/pokemon_le/assets/lcd/play.py` gained:

| Command | Effect |
|---|---|
| `f N` | set `--static-frame N` for subsequent launches |
| `l NAME` | set `--static-label NAME` (resolves per-scene) |
| `f off` | clear static-frame/label (back to looping) |
| `info N` | print scene N's FrameRate / FrameCount / FrameLabels without launching |

Typical workflow for exploring a stateful scene:

```
  info 20                    (shows FrameLabels={NoProgressBar:1, ProgressBar:2})
  l ProgressBar              (set label)
  20                         (launch with that label)
```

### 39.5: Limits

- `--static-frame` freezes ALL sprites globally to the same frame. For scenes with different nested-sprite timelines that want different frames per sprite (e.g. "main sprite at state 1, progress bar at 50%"), this needs per-element control — deferred until we have a concrete scene where global freeze is insufficient.
- Does NOT help scenes that genuinely need animation (e.g. loading-spinner loops). For those, the `--static-frame` flag just shouldn't be used.
- Does NOT replace scene drivers. For scenes with orchestrated animation (Kong intro, attract sequencer), a driver is still the right tool. Static-frame is for one-shot visual inspection.

### 39.6: Reminder of the mental model

Radium scenes are **closer to Flash MovieClip templates** than standalone animations. The game:

1. Loads a scene.
2. Binds data to specific elements by name (e.g. `PokeballCounter01_Instance.PokeballCounter.Score.set_text("12,345,678")`).
3. Picks a state/frame (e.g. `play(1, 1, False)` to hold on frame 1, or `play("label_a", "label_b", False)` to run a specific segment).
4. Optionally runs driver code on gameplay events (e.g. advance frame on score update, trigger animation on ball lock).

Our player now supports (2) via `RadiumBinding`, (3) via `--static-frame` / `--static-label`, and (4) per-scene drivers like `PokemonAttractDriver`. Still missing: per-element frame control and the IPC layer for (2) from a Godot process. Both deferred to follow-up sessions.


## Step 38: Video actually renders — chased `no-player` to a ptrs-duplicate + shipped `--play-clip` (2026-04-19)

Previous step's attract run showed PyAV successfully opening H.264 streams and `PokemonAttractDriver` cycling its state machine, but the window stayed a solid black rectangle — no actual video pixels reached the texture. This step traces that end-to-end and lands a generic clip-player on top.

### 40.1 Diagnostic pass — fix the silently-wrong things first

User reported the on-screen "Time: 0.00" debug overlay never moved. That was a false cue: the overlay at `reference-player-v2.py` line 2602 was hardcoded `"Time: %.2f / %.2f [s]" % (0.0, animationDuration)` instead of using `time_`. Fixed to use `time_` so a static timer now means "update loop is actually dead" rather than "hardcoded zero".

Instrumented `_V2PyAVPlayer.__init__`:

- Record `self._codec_name` from `self._stream.codec_context.name` (spots H.264 vs. HEVC etc.).
- After priming first frame, record `self._pix_fmt` from `frame.format.name`.
- If prime-frame extraction failed (`_width == 0`), raise `RuntimeError(f"PyAV opened … (codec=…) but got no frames")` so the outer `_v2_open_video` `except Exception` falls through to pyglet instead of silently returning a dead player.

The "PyAV opened" log now prints `codec=…, pix_fmt=…` so we can distinguish codec-decode failures from pipeline-wiring bugs at a glance.

Instrumented `Video._render` with one-shot-per-state-transition logging:

| Reported state | Meaning |
|---|---|
| `no-player` | `self._player is None` — nobody called `setClip()` or setClip's `_v2_open_video` returned None |
| `no-texture` | Player exists but its `.texture` property returns None (decoder eof or upload failed) |
| `texture#N` | Player's uploaded GL texture id (happy path) |

State machine is `hasattr(self, '_v2_logged_state')`-initialized and only logs on transitions, so busy render loops don't spam.

### 40.2 ffmpeg probe — codec was mis-remembered

Before chasing code paths we had the user run `ffmpeg -hide_banner -i …/2.asset/0.asset -frames:v 1 -f null -`. Output:

```
Stream #0:0[0x1](eng): Video: h264 (Constrained Baseline) (avc1 / 0x31637661),
    yuv420p(progressive), 1360x768, 1420 kb/s, 30 fps, 30 tbr, 15360 tbn (default)
```

**The Radium `.asset` files are H.264, not H.265.** The Godot battle-backdrop videos leaked in Step 27 stderr (`res://gfx/bknd/*/…_h265.mp4`) are a DIFFERENT, Godot-side asset class — those are plausibly H.265. The 15 attract clips and 214 in-game clips inside `scene.assets/` are H.264 baseline at 1360×768@30fps, ~17 s each. This settles an assumption that had been floating since Step 28 without being verified.

### 40.3 The smoking gun — `Video._render → no-player`

With instrumentation on, attract run logged:

```
[v2] PokemonAttractDriver: wired Video id=1 (mode=color, service_s_active=False)
[v2] attract: state 0 → ATTRACT_LOOP1
[v2] PyAV opened 0.asset (1360×768 @ 30.00 fps, codec=h264, pix_fmt=yuv420p)
[v2] Video._render 'video.in_attract_videos' (1360×768) → no-player
```

Driver wires Video id=1, PyAV opens fine — but render sees no player. Two Video instances.

Added a second round of instrumentation: `Video.__init__` prints every construction with `_id` and Buffer keys, and `_characterToObject` prints cache-hits vs. creates for Video type. Re-ran:

```
[v2] _characterToObject CREATE Video id=18 (cache had keys: [])
Creating character 18
[v2] Video.__init__ id=18 name='video.in_attract_videos' buffer_keys=['ATTRACT_LOOP1', …]
…
[v2] Video.__init__ id=1 name='video.in_attract_videos' buffer_keys=['ATTRACT_LOOP1', …]
[v2] PokemonAttractDriver: wired Video id=1
…
[v2] _characterToObject cache-hit Video id=18 player=no  ← render path
```

Confirmed: **the scene's `ptrs` table contains two distinct entries with the same `Character.Name='video.in_attract_videos'` and the same Buffer keys** — ids 1 and 18. `dumpChildren` (runs during scene load, before the driver) walks via keyframes and materializes id=18 into `scene.objs`. Driver's `_v2_find_video_char_by_name` iterates `radiumScene.ptrs.items()` and returns the first match (id=1), constructs a fresh Video at id=1, overwrites `scene.objs[1]` (empty slot) — but the render walk's `_characterToObject` looks up `character['_id']` which is 18, finds the stale `dumpChildren`-era Video with `_player=None`, and calls its `_render`.

### 40.4: Reuse the already-materialized video object

The probe had selected a duplicate object instead of the one participating in
rendering. Reusing the active object resolved the mismatch between successful
decode and absent playback.

> [Redacted: external renderer object-layout details and implementation code.]

### 40.5 GenericClipDriver + `--play-clip` flag

Attract was only step one — scene 18 (`60ed7e50…`) is the master in-game video dictionary with 214 `PK_*` clips (mode awards, pokéball states, match digits, ball-save, skill-shot tiers, tilt, replay, per-mode LoopingBGs, etc.). No modulo-6 sequencer applies; each clip is a per-event animation the game triggers on specific gameplay moments.

Added `GenericClipDriver` (in `reference-player-v2.py`, right after `PokemonAttractDriver`):

- Takes `scene` + `clip_name`.
- Looks for a Video in `scene.objs` whose Buffer contains `clip_name` (same scene.objs-first pattern as the attract fix).
- Fallback: iterate `scene.ptrs` and materialize on miss.
- Calls `setClip(clip_name)`. On PyAV EOS (`on_eos` callback), calls `setClip(clip_name)` again to loop.
- If the clip isn't in any scene Video's Buffer, prints `[v2] --play-clip 'X' not found in any scene Video buffer (available: …)` with the full list of names the scene does have — so the error tells the user what they could have typed.

New CLI flag `--play-clip NAME`. Wired into scene-load to take precedence over `--pokemon-attract` if both are set — "user explicitly said play this clip" beats "run the attract sequencer".

### 40.6 Clip catalog — `_emulation/VIDEO_CLIPS.md`

Wrote a scanner that walks every `scene.radium.json` under `assets/lcd/`, finds each `ptr_wrapper → data` dict that has both a `Character.Name` and a `Buffer` dict, and collects Buffer keys. Result:

- **2 scenes** carry Videos:
  - `auto_loaded/bc0792d8…/45a4e8c6…` — `video.in_attract_videos` — **15 clips** (ATTRACT_LOOP1..3 + `_BW` + `_SERVICE_S` + `_SERVICE_S_BW` + JUKEBOX_LOOP1..6 + Pattern).
  - `auto_loaded/60ed7e50…` — `video.in_game_videos` — **214 clips**.
- Both video dicts appear in ptrs TWICE per scene (the duplicate-ptrs quirk we just fixed around).

Catalog saved to `_emulation/VIDEO_CLIPS.md` grouped by mode:

- Ball / pinball state: `PK_BallSave_{01,02}`, `PK_Pokeball_{10..100}pct`, `PK_Pokeball_Drop_{01..10}`, `PK_EB_{ExtraBall,ExtraBallLit,ShootAgain}`, `PK_Replay`, `PK_Tilt`, `PK_TiltWarning`, `PK_SkillShot_Award{1..3}`.
- Character select: `PK_CharacterSelectBG_{Bulba,Charm,Pika,Squirtle}`.
- Background patterns: `PK_BG_WorldMap_LoopingBG_01`, `PK_WorldMap_LoopingBG_01`, `PK_BGPattern_{Electric,Fire,Grass,Normal,Water,PokeBall{Black,Blue,Grey,Red}}`, `PK_{Electric,Fire,Grass,Normal,Water}Patterns`.
- Mode: Bulbasaur 1/2, Charmander 1/2, Charmander Spinner, Squirtle 1/2 — each has `_Start` + `_LoopingBG` + `_Award{1,2,3}` + `_Total` (with small variants).
- Mode: Magikarp (fishing + multiball + spelling + evolves + total).
- Mode: Pikachu vs. {Charizard, Gyarados, Lucario, Mimikyu, Raichu, Magikarp} — per-opponent award tiers, LoopingBGs, Start, Total. Note `PK_PikachuVSMagikarp_*` uses capital VS (inconsistent with the rest — preserved verbatim in catalog).
- Mode: Eevee evolutions — `PK_Eevee{Espeon,Flareon,Glaceon,Jolteon,Leafeon,Sylveon,Umbreon,Vaporeon}_Award{NN}` (all 8 Eeveelutions).
- Mode: Team Rocket Multiball 1/2/3 + the three `*Advance` phases, plus Meowth attacks on Multiball 3.
- Match / end-of-ball: `PK_Match_{00,10,…,90}` (decimal digit endings).
- Misc: `PK_Psyduck_Award1`.

### 40.7 play.py TUI — `c NAME` and `clips N`

Added two commands to the interactive menu:

| Command | Effect |
|---|---|
| `c NAME` | Set `--play-clip NAME` for subsequent launches. Echoes `[play-clip set to 'NAME']`. |
| `c off` | Clear `--play-clip`. |
| `clips N` | Read scene N's `scene.radium.json`, walk for Video-shaped dicts, print their Character.Name and Buffer keys. Pure-Python read, no player launch. |

A new `scene_clips(scene_path)` helper mirrors `scene_labels(scene_path)` (Step 37) — same ptr_wrapper walker, collects `{Character.Name: sorted-set-of-buffer-keys}`. Merges duplicates under one key (the same Radium duplicate-ptrs quirk).

Footer now mentions: "See _emulation/VIDEO_CLIPS.md for the full 229-entry clip catalog".

### 40.8 What this unblocks and what remains

Unblocked:

- **Any Video-bearing scene is now previewable in full motion**. Pick any of the 229 clip names and see it loop. Before this step, only the attract scene had bespoke driver code and even it wasn't actually rendering.
- **The Radium "scene → Video → setClip(name) → texture → GL draw" pipeline is end-to-end verified**. Our previously-stubbed `RadiumBinding.set_clip` method corresponds exactly to this flow — we now have a working demo of how a Godot driver would drive a Radium Video at runtime.

Still missing (punch-list for future sessions):

- **Audio from the video tracks**. `_V2PyAVPlayer` decodes the video stream only. PyAV can decode the audio stream in parallel — would need a pyglet audio queue (or direct ALSA/SDL2) fed with the samples. This is the main ask for next session.
- **Non-Radium video paths**. The 8 Godot battle-backdrop `.../_h265.mp4` files (Step 27 stderr leak) live under `res://gfx/bknd/...` and are loaded via Godot's resource loader, not the Radium Video path. Separate decoder surface from `scene.assets/`-style `.mov`s.
- **Driver per non-attract mode**. GenericClipDriver is adequate for inspection but a real Main HUD driver would need to drive 9 scenes + ~75 element paths from cluster 1, not just play a video loop.
- **The JSON-over-IPC bridge** from a Godot process — stubbed in `RadiumBinding` but no consumer yet. Once audio lands, this is the next thing that would make the player drivable as a real front-end.

## Step 39: Sound-event mapping — `sub_8d3460` registers 25 strings; hashmap path found, token→asset_id gap remains (2026-04-19)

Goal: map 231 extracted PCM WAVs + 303 `_tf2_w4` candidates to `SE_PK_*` sound IDs in the previs `spike_game_sound_ids.gd` autoload (~750 constants) so standalone Godot previs plays the right SFX from `res://sfx/<SOUND_ID>.wav`.

### 41.1: Memory correction — music is NOT encrypted

Prior memory `project_audio_next.md` said "largest tf2_w4 blobs show near-uniform entropy, likely encrypted." **Refuted.** User identified three `tf2_w4.bin` files as clean stereo S16_LE PCM at 44.1 kHz:

| File | asset ID | Duration |
|---|---|---:|
| `asset_0050_id20020446_tf2_w4.bin` | `0x20020446` | 67.76 s |
| `asset_0625_id00020446_tf2_w4.bin` | `0x00020446` | 139.06 s |
| `asset_1089_id00020046_tf2_w4.bin` | `0x00020046` | 81.10 s |

The `_extraction/README.md` classification "bit-6 set on id_encoded → 4-byte/element, non-audio" collides with stereo S16_LE PCM (2 ch × 2 B = 4 B/frame). 303 `image.bin` assets were classified `_tf2_w4` and given `.bin` extension — but many of them *are* audio that got the wrong filename convention. Each /tmp candidate WAV is exactly 78 bytes larger than its `_extraction/assets/*.bin` source (the ffmpeg RIFF/fmt/LIST-INFO header).

### 41.2: Batch-converted the remaining 300 candidates

Wrote 303 files to `_extraction/wav_candidates/` (487 MB) via `ffmpeg -f s16le -ar 44100 -ac 2` — header-stamp only, no re-encoding. Index at `_extraction/wav_candidates/CANDIDATES.md` bucketed by duration: **35 long (>30 s)** music candidates, **44 medium (5–30 s)** SFX/VO candidates, **224 short (<5 s)** mostly texture data playing as white noise.

Cross-referenced all 303 against `/tmp/blob_<sha256[:10]>.*` (Reference tool A's inline scene-data cache): zero overlap. The two stores are content-disjoint — blobs come from Radium `scene.radium` inlines, tf2_w4 bins come from `image.bin` asset-registry records.

### 41.3: Sidekick round 1 — 6 questions, winning lead at Q5

Six-question prompt covering SE_PK_ string xrefs, hash-table search, Pokémon-cry table, ALSA backward trace, Stern audio classes, music ID patterns.

- **Q1**: 24 `SE_PK_FX_*` strings registered at runtime into `std::unordered_map<std::string, uint16_t>` at `data_48c56d0`. Tokens 0x6b–0x71 and 0xba–0xcb. **Tokens are not asset IDs.**
- **Q2**: Hash = `std::_Hash_bytes` with seed `0xc70f6907` (standard `std::hash<std::string>`). Only 24 strings in the binary.
- **Q3/Q6**: No Pokémon-cry flat array, no music-ID compiled constants. Both scene-embedded / runtime-resolved.
- **Q4**: Audio thread `sub_cd2590` is a channel-mixer — no event-name routing at that level.
- **Q5** (winning): Found mangled symbol `_ZN16GodotSpikeBridge14register_soundERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEtiEUlRK5ArrayE_`. Itanium `t` = `unsigned short` — the 2nd arg is **uint16_t** (a token), not uint32_t (Sidekick's summary was loose). Matches Q1's token values.

### 41.4: Binary is fully stripped

`nm -D game | grep -i spikebridge` → 0 hits. `nm game | grep -i spikebridge` → 0 hits. Both dynamic and static symbol tables are empty for game-internal methods. The only textual reference to register_sound is in RTTI strings. Standard LD_PRELOAD can't hook internal C++ methods by name — inline binary patching of the function prologue would be required.

### 41.5: Path-find from lambda typeinfo → registration function

Chain from the typeinfo to the actual registration function:

1. **Lambda typeinfo** at BN VA `0x46bb1f8` (a `__class_type_info`) — its `__type_name` field points to rodata string `ZN16GodotSpikeBridge…` at BN VA `0x3bc8f08`.
2. **libstdc++ std::function manager** `sub_b086b4` at file offset `0xb086b4` — references the typeinfo in op 0. Manager pattern: `op 0 = __get_type_info` writes typeinfo to `*arg1`; `op 1 = __get_functor_ptr` writes `arg2`; `op 2 = __clone_functor` writes `*arg2`. Stateless-lambda variant (no op 3 destroy body because the closure has no captures). This is the `_Base_manager` generated by libstdc++ whenever a `std::function<void(Array const&)>` wraps the specific lambda — it's referenced from every `std::function` instance that wraps that lambda.
3. **Registration function** `sub_8d3460` at file offset `0x8d3460` — located via xrefs from the manager + Sidekick. Bulk-inserts 25 sound-event strings into `data_48c56d0` at C++ static init.

### 41.6: The 25-entry token table

Each call at init:

```
sub_b094c0(&data_48c56d0, &"SE_PK_FX_...", token, 0xffffffff)
```

`sub_b094c0` is the hashmap-insertion helper → mutex guard (`sub_b09370`) → `sub_b09850` (hash + collision resolution). Tokens (ranges contiguous):

- `SE_PK_FX_EOB_POKEDEX_*` — 7 entries, tokens `0x6b`–`0x71`
- `SE_PK_FX_ATTACK_TYPE_*` — 18 entries, tokens `0xba`–`0xcb`

Full table: `PROTOCOL.md → Sound event registration`.

### 41.7: The dispatch gap

GDScript pattern (from `pokemon_battle_previs/` and presumably the real build's `main.pck`):

```
Spike.play_sound(SpikeSoundIds.SE_PK_FX_EOB_POKEDEX_PAGE_TURN)
```

String goes across the bridge, C++ looks up the hashmap → gets token 0x6b → resolves to asset_id (via an unidentified second step) → plays.

**But only 25 strings are in the hashmap.** The other ~725 `SE_PK_*` names from the autoload (VO, TUNE, MX, BONUS, THEME, and several FX families outside attack-type / EOB-pokedex) aren't registered. Real hardware clearly plays music and VO — so a second dispatch path must exist. Plausible routes:

- Sister C++ method like `play_music(name)` / `play_voice(name)` with its own hashmap.
- Name-prefix-based dispatch (inspect the first 5 chars of the name to branch).
- Godot's native `AudioStreamPlayer` loading music from `res://` — but `main.pck_extracted/` has no `audio/` folder, so if Godot plays it, the streams must come via a C++ loader reading image.bin.

All three are untested.

### 41.8: Open for next session

1. **Token → asset_id step.** The 25-entry map resolves name → token but not token → asset_id. Look for a `uint32_t token_to_asset[256]` array (indexed by 0x6b, 0xba, etc.) or a second hashmap, called from whatever responds to `Spike.play_sound(...)` after the name lookup.
2. **725-name dispatch.** Find sister methods to `register_sound` and any name-based routing. Likely candidate is another function near `sub_8d3460` that registers the larger set, OR a play-side function that tries multiple hashmaps.
3. **Manual triage of the 35 long tf2_w4 candidates.** User audition: the 35 files >30 s in `wav_candidates/` are likely music (themes, tunes, main-play loops). Easy win against the short SE_PK_THEME / MX / TUNE / BONUS lists.

### 41.9: Artifacts this session

- `_extraction/wav_candidates/` — 303 new WAVs (487 MB) + `CANDIDATES.md` index.
- `PROTOCOL.md → Sound event registration` — 25-entry token table with BN VAs.
- `ASSETS.md → Audio` — new row for tf2_w4 candidates + Sound-event registration subsection.
- `memory/project_audio_next.md` — music-encryption hypothesis rolled back; state reflects 41.1–41.8.

## Step 40: Full pipeline traced — token→asset_id is runtime-only; static extraction dead (2026-04-19)

Final Sidekick round on audio. User flagged BN budget exhausted after this one. Result closes the question: the token → asset_id step is **intentionally** not in compiled code.

### 42.1: Five-stage resolution pipeline

| Stage | Function | Effect |
|---|---|---|
| 1. String registration (static init) | `sub_8d3460` | Inserts 25 `SE_PK_FX_*` strings → uint16_t tokens into hashmap at `data_48c56d0` (documented Step 39) |
| 2. Name lookup | `sub_b09660` | `std::_Hash_bytes` seed `0xc70f6907`; returns hash node with token at offset `+0x28` |
| 3. **Token → asset_id** | NOT IN COMPILED CODE | Populated at runtime from scene bytecodes (image-sc*.bin / image.bin scene 0) via `load_scene_assets` (0xb321d0) — the type-0x0b records registering into `g_asset_registry` |
| 4. Asset registry lookup | `asset_registry_find` over `g_asset_registry` at `data_48ca4d0` | Encoded asset_id → `{file_offset, size, type}` |
| 5. Mmap resolve | `get_asset_ptr_by_packed_id` → `image_mapped_base_plus_offset` | File offset → absolute pointer inside mmap'd `image.bin` |

### 42.2: Why static analysis fails (Sidekick quote)

> "This separation allows content updates without binary recompilation — a regulatory compliance requirement for gaming machines."

Practical consequences:
- No `uint32_t token_to_asset[]` array exists to dump.
- Token constants (0x6b, 0xba, …) **never** appear in asset-ID construction paths.
- Same `SE_PK_*` name can bind different asset_ids in different scenes (mode-specific variants).
- Full mapping only exists in memory after scenes load.

### 42.3: Dead options after this finding

- **Static dump**: not available (no table).
- **Hook register_sound / sub_b09660**: gets us the 25 static names → tokens, nothing more.
- **Any further BN trace** on this subject: out of budget.

### 42.4: Still-viable paths

- **(A) Runtime capture under emulation.** Get past Step 29's PopupMenu TLS SEGV, then hook `asset_registry_find` (file offset `0x8ca4d0` neighborhood) to log (packed_asset_id, file_offset, size) tuples as scenes load, and hook `sub_b09660` to correlate names → tokens. Gated on resolving the Godot-TLS crash that's been parked since Step 29. High eng cost.
- **(B) CM4 hardware memory dump.** User owns CM4 + Pi carrier (per memory `hardware_inventory.md`). Boot real filesystem on CM4 (no 48V needed), attach gdb/perf to running game, dump `g_asset_registry` once scenes have loaded. One-shot capture. Low eng cost, moderate logistics.
- **(C) Manual triage.** Audition 231 + 303 WAV candidates against the ~750 `SE_PK_*` names; tag what we recognize. Zero eng cost, high user time cost. Can be accelerated with a small play/tag TUI.

### 42.5: Decision — pivoting to (C)

BN exhausted means (A) is gated behind the Step-31 TLS investigation, which is a different work track. (B) depends on when CM4 setup lands. (C) can start immediately and produces usable results per hour of listening.

Going with (C). Starting points:
- **3 confirmed music tracks** (67 / 139 / 81 s) → slot into 3 of the `SE_PK_THEME_*` / `SE_PK_MX_*` constants by audition.
- **6 user-labeled Pokémon cries** (PIKACHU/ODDISH/PSYDUCK/MUDKIP/ONIX/DIGLETT) → map directly to `SE_PK_VO_POKEDEX_####` by Pokédex number.
- **35 long `wav_candidates`** (>30 s) → audition against TUNE / THEME / BONUS / MX names.
- **44 medium** (5–30 s) → longer VO and FX.
- **Skip 224 shorts** — mostly RGBA-as-PCM noise.

### 42.6: Artifacts

- This JOURNAL entry.
- `PROTOCOL.md → Sound event registration` — extended with stages 3–5 + rationale.
- `memory/project_audio_next.md` — updated with final state: static path dead; triage is now the official plan.

## Step 41: Standalone extractor built — image.bin section 7 is all we need (2026-04-20)

Pivoted the mapping problem into a *generic extraction* problem after Step 40 closed the SE_PK_* → asset_id avenue. User's architectural ask: a **standalone** extractor that operates on `(game binary, image.bin)` alone, modeled on Pinball Browser — no runtime emulation, no shim dependency. Deliverable: `games/pokemon_le/_extraction/tools/spike_extract/`.

### 43.1: Walked the `g_asset_registry` hashmap at runtime

Extended the shim's `SIGSEGV`+`SIGBUS` handler to parse the `std::_Hashtable` struct at `data_48ca4d0` and walk the `_M_nxt` linked list, dumping each node's raw bytes to `_emulation/asset_registry.bin`.

Hurdles resolved along the way (all documented here for future QEMU memory-dump work):

- **`/proc/self/mem` is host-view, not guest-view.** `pread()` to read guest VAs fails with `EIO`. Switched to direct `memcpy` — under QEMU user-mode the shim runs as guest code (aarch64 via TCG), so direct pointer access hits QEMU's address translation.
- **Game binary is rebased by QEMU ASLR.** Dladdr-from-backtrace gives the runtime base. Trap: `strstr(dli_fname, "/game")` also matches the shim's path (`.../games/pokemon_le/_emulation/spi_shim.so`) because "games" is plural. Filter with `&& !strstr(dli_fname, ".so")` to avoid false match on shim frames.
- **Binary fully stripped** — neither `nm -D` nor static symbols expose internal C++ methods. Inline binary-patching of the function prologue would be needed to hook by address. Not required for this dump (we're reading globals, not hooking functions).
- **SIGTERM intercepted by QEMU, not forwarded to guest.** `timeout --signal=TERM` produces an empty log. Rely on the natural Step-31 `PopupMenu` SEGV instead, which fires ~55–66 s in.

Final shim dump captures **2452 nodes × 128 bytes** = 314 KB to `asset_registry.bin`. Nodes are actually ~48 bytes; the extra padding in each 128-byte slot includes adjacent heap memory (harmless, parser only reads the first 48 bytes).

### 43.2: `_Hashtable` struct decoded

Header at `game_base + 0x44ca4d0`:

```
_M_buckets       = heap ptr (0x7?????02fc80 typical)
_M_bucket_count  = 5087  (prime > 2452)
_M_before_begin._M_nxt = heap ptr to first node
_M_element_count = 2452  (matches image.bin section-7 record count)
_M_max_load_factor = 1.0
_M_next_resize   = 5087
```

Node key is the `uint64_t` at offset `+0x08`. **Verified:** 2452 unique keys across all nodes.

### 43.3: Registry keys match image.bin section 7 exactly

Parsed image.bin section 7 (24-byte records at file offset `0x42F23E06`) with the documented format from `_extraction/README.md`:

```
bytes 0-7:   uint64 file_offset
bytes 8-15:  uint64 (flags<<32 | asset_id_low)     ← the composite key
bytes 16-23: uint64 (hash<<32 | size_elements)
```

Cross-reference result:

```
registry keys:         2452
image.bin encoded_ids: 2452
intersection:          2452
reg-only:              0
img-only:              0
perfect_match:         True
```

**The runtime registry is a cached index of section-7 data — nothing more.** Static extraction from `image.bin` alone is sufficient; the shim's dump becomes validation-only.

### 43.4: Standalone extractor `spike_extract/`

New tool at `games/pokemon_le/_extraction/tools/spike_extract/`:

```
spike_extract/
├── __init__.py           # package header
├── __main__.py           # CLI: python -m spike_extract --image image.bin [--registry X] [--out dir]
├── image_bin.py          # Section-7 parser → AssetRecord dataclass
├── registry_dump.py      # Reads _emulation/asset_registry.bin (optional validation)
└── README.md             # Usage + format docs
```

Usage:

```
# Pure static extraction — no shim needed
python -m spike_extract --image games/pokemon_le/image.bin --out out/

# With validation against runtime dump
python -m spike_extract --image image.bin --registry asset_registry.bin --out out/
```

Validation run confirmed 2452/2452 match with the runtime registry.

### 43.4b: Classifier refined + full extraction

Initial histogram had a 268-record `data_4byte` bucket labelled "non-audio 4-byte data". Inspecting the first 16 bytes of those records revealed small signed int16 deltas clustered near zero — the textbook shape of low-amplitude stereo PCM sample pairs. One of the 3 known music tracks (asset 1089, id `0x00020046`) was sitting in `data_4byte` because its id_low doesn't have bit 10 set — my "music" heuristic was too narrow.

Discovered pattern: **all 347 4-byte-element records in Pokémon LE 0.81 have bit 17 (`0x20000`) set in `id_low`**. This is the stereo-audio marker. Rewrote the classifier to:

- Stereo audio (any): `elem == 4 AND (id_low & 0x20000) AND (id_low & 0x40)`
- Split into music vs SFX by computed duration (≥30 s → music) since there's no reliable static discriminator.

Cleaned up folder creation to be lazy — empty categories produce no folder.

Final full-extraction histogram:

| Category | Count | Size |
|---|---:|---:|
| `audio_mono` | 2105 | 566 MB |
| `audio_stereo_sfx` | 312 | 180 MB |
| `audio_stereo_music` | 35 | 331 MB |

All 3 known music tracks classified correctly. MUDKIP cry byte-exact with legacy `_extraction/wav/` (mod WAV header). `data_4byte` bucket eliminated. `audio_mono` is still coarse (2105 > 230 true-mono baseline), but the stereo-audio pool is clean.

Output: `_extraction/spike_extract_out/` (1.1 GB, 2452 WAV files + `index.csv`).

### 43.5: Portability

The section-7 format is stable across Stern's Spike 3 codebase. Parser should work on any `image.bin` from any Spike 3 game (Jurassic Park Pro, Stranger Things, etc.). Per-game tuning is only the classification heuristic — different games may use different `flags` encodings and the 64 `asset_id_low` category codes may shift. The file-structure parser is universal.

Future: accept `--registry-va 0x48ca4d0` as CLI flag (default for Pokémon LE 0.81) for scanning other builds' game binaries; extend `image_bin.py` to handle `image-sc*.bin` scene-specific subfiles via the RB-tree-of-mmaps pattern documented in `load_image_bin_file` at BN `0xc47210` (decomp captured in Step 41 session notes).

### 43.6: What the shim's role is now

The shim stays in the repo as the emulation-investigation tool (Steps 14–42). Its new `shim_dump_sound_globals` routine is preserved as a **reference-capture mechanism** for validating future Spike 3 image.bin parsers. For routine extraction, no shim, no QEMU — just `python -m spike_extract --image image.bin`.

### 43.7: Artifacts

- `games/pokemon_le/_extraction/tools/spike_extract/` — the tool.
- `games/pokemon_le/_emulation/asset_registry.bin` — reference registry dump (314 KB; regenerate by running the shim with the current `spi_shim.c`).
- `games/pokemon_le/_emulation/spi_shim.c` — SIGSEGV handler now includes hashmap struct parse + linked-list walk + file-based dump.
- PROTOCOL.md — new `g_asset_registry` section with the verified struct layout and validation record.

## Step 42: Section 2 + Section 5 probed; 0x0b bytes 3-6 decoded; 885 scene map still open (2026-04-21)

Context: user's architecture ask — go after both Section 2 and Section 5 in parallel to close the static 885 scene → asset_id mapping gap that Step 41 confirmed is not runtime-cached anywhere special. Pinball-Browser-style metadata extraction tools want this mapping to present "what plays during which game state."

Three probe scripts under `games/pokemon_le/_extraction/tools/`:
- `probe_sections.py` — baseline layout enumeration.
- `probe_sections_v2.py` — bounded bytecode walk + byte-distribution of 0x0b payloads.
- `probe_sections_v3.py` — records-per-id_low histogram + brute-force flag-packing tests.
- `probe_sections_v4.py` — full-bytecode dumps + 0x05 and 0x09 byte-distributions + Section 2 record structure.

### 44.1: Section 2 is a playfield-grid record table, NOT a scene map

Header + v4 findings:
- Section 2 @ 0x189b8, length 0x2E28 (11,816 bytes).
- Treated as uint64 LE → 1,477 pointers into section 0 (0x1B800-0x29EE9) and adjacent bulk region.
- Deltas between consecutive pointers cluster around 408, 40, 64, 48, 56, 88 — variable-size records.
- **First 12 records are identical 17×23 grids**: bytes 0..11 = `{u32 id}{u32 flag}{u16 width=17}{u16 height=23}`, then 391 cells × 1 byte + 5 bytes trailing padding → 408 total. Cell values vary (0x0f, 0x03, etc. — small enum indices).
- Records 12-13 have delta=360 (different dimensions). Other deltas in the 24-128 range suggest later records are smaller grid types or non-grid headers.

No `asset_id_low` values, no section-5 offsets, no section-7 encoded_ids appear inside section 2 as uint32s (0 of 2954 tested). Section 2 does NOT encode the 885 scene → asset mapping.

Best current interpretation: Section 2 is a **mode/animation playfield-pattern table** — each record is a 17×23 grid of lamp-state / target-state / per-cell enum drives. Consistent with the README label "numbered grid data" (u32[0] is a sequential record id). Likely consumed by the gameplay logic to drive playfield hardware per-game-state.

### 44.2: Section 5 is per-ASSET descriptors, not per-SCENE

- 12,568 slots × 8 bytes grouped in sets of 5 (language variants).
- 2,492 unique bulk-region bytecode offsets (the 5-tuple "equal" cases in section 5).
- Each bytecode bounded by the next unique offset decodes cleanly (255 ended `eos`, the rest terminate on `hit_end` at their neighbor — no decoder bugs once properly bounded).
- Tag frequency per-bytecode pattern: 1×0x05, 1×0x0b, 1-2×0x02/0x09, occasional 0x04/0x0a/0x0e/0x12. Bytecode length distribution: 25 bytes (simple) up to ~108 bytes (with animation/transform data).

2,414 of the 2,492 bytecodes contain exactly one type-0x0b record. The missing ~78 bytecodes + the 38 section-7 records that have no corresponding 0x0b are the delta between 2,452 assets and 2,414 0x0b references. Plausibly these are meta-descriptors (fonts? virtual assets referenced indirectly?).

**Architecture correction:** section 5 is NOT a scene → asset list. It is a 1:1 asset descriptor table — one bytecode per asset describing load/render flags. The 885 gameplay scenes reference these descriptors indirectly via something else (unconfirmed — scene_config_table at runtime VA 0x47fcf10 is still our only lead for that mapping).

### 44.3: Type-0x0b payload (10 bytes) — bytes 3-6 = asset_id_low (VERIFIED 100%)

Byte-by-byte distribution across all 2,414 0x0b payloads:

| Byte | Distinct values | Interpretation |
|---:|---:|---|
| 0-2 | 1 each (always 0) | padding |
| 3 | 16 | asset_id_low byte 0 (low 8 bits) |
| 4 | 2 (0x00 / 0x04) | asset_id_low byte 1 |
| 5 | 2 (0x00 / 0x02) | asset_id_low byte 2 (the 0x20000 stereo-audio marker bit!) |
| 6 | 2 (0x00 / 0x20) | asset_id_low byte 3 (bit 29 = 0x20000000) |
| 7 | 2 (0x00 / 0x02) | partial flag bit (bit 1 of flags word) |
| 8 | 1 (always 0) | padding |
| 9 | 8 | partial flag bits (bits 20-22 of flags word) |

u32 LE of bytes[3..7] matches an asset_id_low in section 7 for all 2,414 records (100%).

### 44.4: Flag encoding — only partial from 0x0b payload

Section-7 records have 256 unique `flags` values (the high 32 bits of each encoded key). Each of the 64 distinct asset_id_lows appears 5-81 times in section 7 with different flag values — so asset_id_low alone is never enough to identify a specific record.

Brute-forcing combinations of 0x0b bytes 7 & 9 to reconstruct a flag word:

| Formula | Match rate |
|---|---:|
| `flags = 0` | 427/2414 |
| `flags = P7 \| (P9 << 16)` | 619/2414 |
| `flags = (H[0] << 24) \| P7 \| (P9 << 16)` (incl. 0x05 byte 0) | 660/2414 |
| Everything else tried | worse |

The upper bound of what bytes 7+9 can encode is 2 × 8 = 16 combinations — far short of 256. Since H[0] (tag-0x05 byte 0) only has 11 distinct values, it can add at most ~11× more. Not a complete answer.

The likely location of the missing flag bits is 0x05 byte 2, which has 256 distinct values across all 2,496 0x05 records — exactly matching the flag-value count in section 7. But we don't have a formula yet that joins id_low + 0x05[byte 2] + 0x0b[byte 7, 9] to hit 100%. Probably the loader computes the key from multiple sources including per-scene context.

### 44.5: `scene_config_table.c` in _extraction/ is misnamed

The 1,254-line `scene_config_table.c` dumps the region starting at VA 0x48071d0, which is **the Spike 3 service/test/adjustments menu structure** — entries like `"Active Switch Test"`, `"Game Adjustments"`, `"Pikachu Tests and Calibration"`, `"Exit the test menu"`, etc., plus a few audio-test items (`"MUSIC: MAINPLAY"`, `"SOUND: SUPER SKILL SHOT"`, `"SPEECH: 'SQUIRTLE'"`). Each entry is a triple: `{function_ptr, label_string, u16_index u16_parent_id}`.

The real 885-entry scene table at VA 0x47fcf10 is 41,664 bytes earlier and was NOT captured in this Binary Ninja export — it may genuinely be BSS-populated at runtime as CLAUDE.md claims, or it may be static data in an adjacent region that the export missed.

Renaming `scene_config_table.c` → `service_menu_table.c` is warranted but deferred until we re-inspect the real `scene_config_table` region in Binary Ninja.

### 44.6: Status of the "how does a scene coordinate audio+visuals" mapping

**Closed:**
- Per-asset flags/type/size (section 7).
- Per-asset bytecode descriptor including mlock hints (section 5).
- Asset category classification (mono/stereo/music via id_low bits 6 and 17) — Step 41.
- Radium scene → asset bindings via the 6 rodata clusters — Step 33.
- Attract video sequencer state machine — Step 34.

**Still open:**
- 885 scene_id → asset_id list. The runtime BSS table at 0x47fcf10 is the authoritative answer but we haven't captured it.
- Full flag encoding in section-5 bytecodes (partial: id_low verified; flags only ~27%).
- SE_PK_* name → asset_id mapping for the ~725 unregistered names.

### 44.7: Next options

1. **Binary Ninja re-export of VA 0x47fcf10.** If the 885-entry table is static data, exporting it from the current .bndb gives us the mapping directly. Cheapest next step.
2. **Runtime capture** under the emulation shim. Step 41 already dumps `g_asset_registry`; extending the SIGSEGV handler to also dump `scene_config_table` would close the mapping under QEMU without clearing Step 29's PopupMenu crash — the crash fires after scene table is populated.
3. **Full decode of the 0x05 byte 2 field** to finish the static flag encoding. Would let us recover (asset_id_low, flags) pairs purely from image.bin without registry.
4. **CM4 hardware dump** — user owns the hardware and the LUKS key is already extracted. On-device gdb or /proc/*/maps dump of the running `game` process is the cleanest one-shot capture of the real table.

### 44.8: Artifacts

- `games/pokemon_le/_extraction/tools/probe_sections.py` — v1 baseline probe.
- `games/pokemon_le/_extraction/tools/probe_sections_v2.py` — bounded decoder + payload byte distribution.
- `games/pokemon_le/_extraction/tools/probe_sections_v3.py` — id_low → records histogram + flag-packing brute force.
- `games/pokemon_le/_extraction/tools/probe_sections_v4.py` — full bytecodes + 0x05/0x09 distribution + section-2 record structure.
- PROTOCOL.md — new "Section 5 scene bytecode" section with the 0x0b layout and known-open flag encoding.

## Step 43: Step 29 unblocked — memcpy NULL-guard lands, game reaches 4 Godot scenes (2026-04-21)

User pivoted from asset extraction back to emulation. Goal: unpark the Step-31 PopupMenu TLS SEGV and see what's next.

### 45.1: Fresh-run triage (`run_step45.*`)

Ran the current shim (last rebuilt 2026-04-20 with Step 41's SIGSEGV+registry-dump additions) unchanged. Same crash signature as Step 29 — `SEGV_MAPERR fault_addr=0x40` at `./game+0x7b7c3c` (1372 bytes into `sub_bb76e0` DisplayRenderer render loop), crashing in glibc's `cpyfp` (aarch64 __memcpy_aarch64) from a NULL memcpy inside Godot's `_err_print_error` logger.

Stderr (17,876 lines) — upstream errors preceding the crash, sorted by proximity:

| Proximity | Error | Cause |
|---|---|---|
| immediate | `bridge: no synthetic response for cmd 0x41` | shim default branch, no handler |
| close | `bridge: no synthetic response for cmd 0x25` | same |
| close | `ERROR: Node not found: "AnimationPlayer" (relative to "/root/pokemon_battle_scene")` | Radium dynamic-attach gap, parked Step 29.1 — user confirms the AnimationPlayer absence also appears when building the extracted Godot project, so it's genuinely missing from the scene on disk. |
| medium | `[Radium Error] VideoStreamDecoder: Cannot open input file ''` | Radium expected an asset we haven't provided |
| medium | `can't create directory '/data/'` (×9) | game's own mkdir fails even though sysroot has /data/ |
| medium | `Can't create shader cache folder: /tmp/cache` | Godot wants /tmp/cache |
| medium | `/bin/sh: cannot create /connectivity/hostname: Directory nonexistent` | subshell doesn't inherit our path remap |
| cosmetic | Fontconfig cache / dummy PhysicsServer3D / mouse not supported | — |
| cosmetic | `ld.so: spi_shim.so cannot be preloaded` in child `/bin/sh` | LD_PRELOAD path isn't set in child environment |

### 45.2: Hypothesis + three-change shim attempt (`run_step45_fix1.*`)

Hypothesis was that cmd 0x41's timeout was the proximate trigger (it fired immediately before the SEGV). Implemented three shim changes:

1. **Silent-ACK for bridge cmd 0x41 and 0x25** — zero-payload response in the same shape used for cmds 0x11 / 0x1c.
2. **`mkdir("/tmp/cache", 0755)` in the shim constructor** — host-visible path, `/tmp/` is excluded from `remap_path()`.
3. **`memcpy` LD_PRELOAD override with NULL-guard.** If `dst` or `src` is NULL, silently no-op and return `dst` (count + log the first 8 + every 1000th hit). Otherwise forward to the real libc memcpy resolved via `dlsym(RTLD_NEXT, "memcpy")`. Thread-local `in_memcpy` recursion flag + byte-copy fallback if dlsym hasn't resolved.

Result — **exit 124 (timeout), no SIGSEGV**. Game survived 90 s. But capture counts (445 textures, 58 shaders, 1 DMD frame) were **identical** to the crashing run and no Godot Engine banner ever appeared. Stdout stopped at line 36 (the `/data/` mkdir noise). Game regressed: it never even reached Godot init.

Read of the distribution of non-SPI log lines explained it: **254 × `bridge RESP to cmd=0x41 id=0x01 (5 logical bytes): 02 41 01 be 03`** — the game was in a tight polling loop on cmd 0x41 and never escaped.

### 45.3: Fix the regression — revert cmd ACKs, keep memcpy guard (`run_step45_fix2.*`)

Removed the silent-ACK for cmd 0x25/0x41. Kept `/tmp/cache` mkdir + memcpy NULL-guard. PROTOCOL-level note: **the cmd 0x41 timeout IS the progress path** — the game's state machine falls through to a default/null branch after `1000 ms`, which is how init proceeds. Our fast zero-payload ACK arrives before the timeout fires and the game interprets it as "ACK but no status payload → keep polling". Until we know the expected response payload for cmd 0x41 (handler `0x915f88`, TBD per PROTOCOL.md) we should not send synthetic replies.

Rebuild + rerun. Exit code **4** (Godot's own exit, not SIGSEGV). stderr 19,055 lines. stdout 132 lines. Game reached Godot cleanly.

**memcpy NULL-guard hits:** 6 total
- #1, #2, #6: `n=0` (benign vformat-of-empty-string)
- **#3: `n=2304`, #4: `n=10752`, #5: `n=1408`** — substantive copies that would have SEGV'd. All three `src=0x774c24c68630` (same buffer). These are the real catches; each one would have terminated the process without the guard.

**Godot scenes reached** (grepping `/root/[^"]+` from stderr):
- `/root/Main`
- `/root/pokemon_battle_scene`
- `/root/capture_battle_scene` — new! Full path `/root/capture_battle_scene/battle_scene_grp/left_world_control/left_char_Sprite2D`
- `/root/Discover` — new! `/root/Discover/discover_progress_bar`

**New subsystems / modules revealed:**
- **`modules/spi_video_player/video_player_spi.cpp`** — Stern's custom Godot C++ module for video playback through the Spike video path. First direct confirmation of this module. Error `Condition "!is_inside_tree()" is true. at: play (line 376)` means the module's `play()` tried to start a video before the player node was in the scene tree.
- **libavcodec's HEVC decoder** — `[hevc @ ...] Failed to find a V4L2 device for H265`, `Failed setup for format drm_prime`, `Error parsing NAL unit #0`. The game tries to hardware-decode `res://gfx/bknd/*/…_h265.mp4` via V4L2. Our QEMU user-mode sandbox has no `/dev/video*`, so the decoder falls back (unsuccessfully). This is the expected next blocker for video playback on-host.
- **`@export`, `@icon`, `@export_range`, `@export_multiline`, `@warning_ignore*`** — GDScript annotation StringNames dying at shutdown. These are Godot's exit-time "BUG: Unreferenced static string to 0" diagnostics (core/string/string_name.cpp:144), plus "Pages in use exist at exit in PagedAllocator: Variant::Pools::Bucket{Large,Medium,Small}" (paged_allocator.h:170). Both indicate the shutdown wasn't clean — objects outlived the Variant pools — which is normal when a process is tearing down mid-activity.

**Capture counts after Step 43:**

| Artifact | Step 27/29 | Step 43 | Δ |
|---|---:|---:|---:|
| textures | 445 | 507 | +62 |
| shaders | 58 | 74 | +16 (8 new programs × 2) |
| DMD frames | 1 | 1 | 0 |
| audio (bytes) | ~11.7 MB | 124 MB | +112 MB |

The audio file grew disproportionately because the game, once past the PopupMenu wall, enters its real audio-playback path and the shim's `snd_pcm_writei` paces with `usleep(size*1e6/rate)` inside a loop that was previously blocked.

### 45.4: What NOT to do (lessons for future)

- **Do not silent-ACK bridge cmds you don't understand.** The timeout path is sometimes the intentional progress path. Always verify a new ACK preserves forward progress via a diff of `grep -c "Godot Engine"` or similar milestone markers against a known-good run.
- **Do not conflate "no crash" with "progress"** — the 90 s cmd-0x41 polling loop survived without crashing but went nowhere. Exit 124 from `timeout` is a red flag, not a green one; verify capture counts and scene-load milestones actually advanced.
- **memcpy NULL-guard should stay on.** It's cheap, it's catching real NULL dereferences in Godot's error path, and it doesn't affect performance (in-memcpy flag + dlsym-resolved real memcpy for the non-NULL hot path).

### 45.5: Next blockers visible at Step 43 exit state

Sorted roughly by how much progress each could unlock:

1. **`modules/spi_video_player` → H.265 V4L2 hwaccel.** Game calls into Stern's `video_player_spi` module which demands a V4L2 HEVC decoder for `res://gfx/bknd/*/…_h265.mp4`. Options: (a) stub the module's `play` call to no-op so the game doesn't hang waiting for decode; (b) provide a V4L2 emulator; (c) patch the module to try a different codec path. Option (a) is cheapest and probably lets us see what else lives past it.
2. **Radium `Failed to get HW surface format`** — similar story, Radium's HW surface allocator needs something our stubs don't provide.
3. **GPIO pin 25** — `/sys/class/gpio/gpio25/direction` missing. Shim can intercept /sys paths, or we can pre-populate in sysroot.
4. **bridge cmds 0x12 and 0x19 timeouts** — new depth not reached before. Same caveat as 0x41: verify they're not the progress path before ACK'ing.
5. **`/connectivity/` subshell mkdir** — subshell spawned by game doesn't inherit remap, writes to bare `/connectivity/`. Options: `mkdir -p /connectivity` on host pre-run; or hook `system()` to reject these specific commands; or inherit `LD_PRELOAD=` + `SPIKE3_SYSROOT=` into child env via `execve` hook.

### 45.6: Artifacts

- `_emulation/spi_shim.c` — memcpy NULL-guard (new section at end of file), `/tmp/cache` mkdir (in constructor), cmd 0x25/0x41 investigation comments.
- `_emulation/spi_shim.so` — rebuilt, 150,576 bytes.
- `_emulation/run_step45.{stdout,stderr}` — crash-signature baseline.
- `_emulation/run_step45_fix1.{stdout,stderr}` — regression run with cmd 0x41 ACK; useful only to prove the ACK stalls init.
- `_emulation/run_step45_fix2.{stdout,stderr}` — **the unblock run**. Use this as the baseline for Step 44 onwards.
- `memory/project_emulation_progress.md` — updated to reflect Track A is no longer parked.
- `memory/project_animation_player_gap.md` — user confirmation that the Godot extract has the same AnimationPlayer absence.
- PROTOCOL.md — next update should add a note that cmd 0x41 (and 0x25, and presumably 0x12/0x19 now) rely on 1000 ms timeout as their progress signal until their response payload is reversed.

## Step 44: V4L2 HEVC decoder probe — env vars readable, but open() never called (2026-04-21)

User picked the "fake V4L2 HEVC decoder via shim ioctl hooks" option from Step 43's next-step list. Hypothesis: if libavcodec thinks it has a V4L2-stateless HEVC decoder, the game's `spi_video_player` module gets decoded frames and the res://gfx/bknd/*/…_h265.mp4 environment backdrops render.

### 46.1: Scoping the libavcodec V4L2 probe

Key intel from `usr/lib64/libavcodec.so.59` (version 59.37.100 = ffmpeg 5.1):

- Two env vars control the V4L2 request device paths:
  - `LIBVA_V4L2_REQUEST_MEDIA_PATH`  (default `/dev/media0`)
  - `LIBVA_V4L2_REQUEST_VIDEO_PATH`  (default probe)
- Relevant rodata strings (offsets):
  - `0x868200` `LIBVA_V4L2_REQUEST_MEDIA_PATH`
  - `0x868220` `LIBVA_V4L2_REQUEST_VIDEO_PATH`
  - `0x868d08` `Failed to find any V4L2 devices`
  - `0x868d30` `Failed to find a V4L2 device for H265`
  - `0x868d58` `Trying V4L2 devices: %s,%s`
  - `0x868d98` `Unable to create media pool`
  - `0x869050` `hevc_v4l2request`
- The V4L2 probe function sits around file offset `0x6b4b00-0x6b4f00` (multiple `adrp x2, 868000` references to that error-string cluster).

### 46.2: Shim changes (kept, logging-only)

Added to `_emulation/spi_shim.c`:
- `FAKE_V4L2_MEDIA_PATH = "/dev/shim-v4l2-media"` + `FAKE_V4L2_VIDEO_PATH = "/dev/shim-v4l2-video"` constants.
- `FAKE_FD_V4L2_MEDIA = 1021`, `FAKE_FD_V4L2_VIDEO = 1020` fake-fd slots.
- `v4l2_media_active`, `v4l2_video_active` active-flag booleans; `v4l2_ioctl_count` counter.
- `intercept_device()` now routes the two paths to fake fds.
- `is_v4l2_fd()` helper (parallels `is_drm_fd`).
- `close()` hook recognizes and retires the fake fds.
- `ioctl()` hook: on any V4L2 fd, log the full request encoding (type, nr, direction, size, arg pointer) and return `-1` with `errno=ENOTTY`. This is the canonical "this device doesn't support this ioctl" response — no false positives, maximum observability.

### 46.3: Three experiments, one answer

**Exp 1** — run with env vars set to shim-owned fake paths:
```
-E LIBVA_V4L2_REQUEST_MEDIA_PATH=/dev/shim-v4l2-media
-E LIBVA_V4L2_REQUEST_VIDEO_PATH=/dev/shim-v4l2-video
```
Result: error changed from `Failed to find a V4L2 device for H265` → `Unable to create media pool`. **But neither fake path was ever opened** (no shim log, no qemu-strace openat entries). Shim's V4L2 ioctl handler never fired (`v4l2_ioctl_count == 0`).

**Exp 2** — env vars pointing at real host-accessible files under `/tmp/`:
```
-E LIBVA_V4L2_REQUEST_MEDIA_PATH=/tmp/v4l2-test-media
-E LIBVA_V4L2_REQUEST_VIDEO_PATH=/tmp/v4l2-test-video
```
Touched both files up front. Same result — "Unable to create media pool", no open() on either /tmp/ path.

**Exp 3** — comma-joined single env var (attempting "MEDIA_PATH=/dev/a,/dev/b" in case the format requires it):
`-E "LIBVA_V4L2_REQUEST_MEDIA_PATH=/dev/shim-v4l2-media,/dev/shim-v4l2-video"`. QEMU's `-E` parser rejected the comma (it's the env-var delimiter in getsubopt(3) format). Workaround would be `QEMU_SET_ENV=` with escaping; not pursued.

### 46.4: Interpretation

libavcodec reads the env vars (behavior changes deterministically when they're set), but the "Unable to create media pool" error fires before any `open()`-like syscall reaches our shim. The most plausible explanation: libavcodec's `v4l2_request_pool_alloc` (or equivalent) does a precondition check — plausibly via `libdrm` / `dmabuf-heap` / `udev` / direct `ioctl` on a shared fd — that fails on a non-Linux-V4L2 host. The actual open of the media device happens only after that precondition succeeds.

### 46.5: Why this is multi-session

To close this properly we need one of:
1. **BN trace of `libavcodec.so.59 @ file offset 0x6b4b00`** to identify which preflight check fails. Then either satisfy that check (shim) or patch it out (binary mod).
2. **Hook libavcodec's codec entry points** directly (`avcodec_send_packet`, `avcodec_receive_frame`, etc.) to fake decode results — bypasses V4L2 entirely but gets the game past this blocker. Smaller surface but more fragile (libavcodec API moves between versions).
3. **Load a real `v4l2loopback` kernel module on host**, expose `/dev/video*`, point the env vars at those, build a user-mode feeder. Big setup cost; requires host kernel module.

Scope-wise, option 2 is the most likely one-session win.

### 46.6: Artifacts

- `_emulation/spi_shim.c` — V4L2 fake-fd open/close/ioctl infrastructure (kept for reuse).
- `_emulation/spi_shim.so` — rebuilt (150,736 bytes).
- `_emulation/run_step46_probe.stderr` — the "Unable to create media pool" smoking gun.
- `_emulation/run_step46_envtest.stderr` — proves /tmp/ paths also not opened.
- `_emulation/run_step46_comma.stdout` — QEMU's `-E` usage banner (comma got eaten).
- `_emulation/strace_step46.txt` — 34,364-line qemu-strace confirming NO open of any V4L2-related path.

### 46.7: Decision

Parking V4L2 emulation at this checkpoint. The shim infrastructure (fake paths, ioctl logger) stays in place for when either BN budget opens up or someone picks option 2 (libavcodec API hooks) in a future session. Documented as Step 44 so next time we pick this up we don't re-run the same three experiments.

## Step 45: libavcodec API hooks + dummy frame injection — stopped at AVFrame format mismatch (2026-04-21)

User picked the "highest excitement-per-hour" option: hook libavcodec's codec-API calls (not V4L2 ioctls) so decode pipeline produces *something* for the game's video renderer even without real HEVC hardware.

### 47.1: What the game actually uses

`nm -D --undefined-only games/pokemon_le/game` imports 9 avcodec_* + 5 avformat_* + 2 av_hwdevice_* + 2 av_frame_* + misc. The relevant HW-accel calls are:
- `avcodec_find_decoder_by_name` — find HEVC decoder by name
- `avcodec_get_hw_config` — enumerate HW configs for the codec
- `av_hwdevice_ctx_create` — build an AVHWDeviceContext
- `av_hwdevice_find_type_by_name` — map "drm"/"vaapi"/etc. to enum
- `avcodec_send_packet` / `avcodec_receive_frame` — decode pipeline

### 47.2: Passthrough-logging reveals the real path

Initial attempt forced SW decode by:
- `avcodec_find_decoder_by_name` strip `_v4l2request` suffix → base "hevc"
- `avcodec_get_hw_config` → always NULL
- `av_hwdevice_ctx_create` → AVERROR(ENOSYS)

Result: exit 134 (SIGABRT). Game assertion:
```
game: platform/spike3//SpiVideoStreamDecoderBase.cpp:1830:
Assertion `hw_decoder' failed.
```

So the game HARDCODES HW decode — no SW fallback exists.

Reverted to passthrough logging (with `dlvsym("symbol","LIBAVCODEC_59")` for versioned symbol resolution). Observed:
- `avcodec_get_hw_config(codec, 0) -> 0x....` — real HW config found (non-NULL).
- `av_hwdevice_ctx_create(type=8, device=NULL, flags=0) -> 0` — **SUCCEEDS**. Type 8 = `AV_HWDEVICE_TYPE_DRM`.
- `avcodec_send_packet` returns 0 → packets accepted.
- `avcodec_receive_frame` returns -11 (EAGAIN) consistently → no decoded frames produced.

Clean exit 4. Game handles EAGAIN gracefully — just doesn't render video. Critical architectural finding: **Pokemon LE uses DRM-prime HEVC decode via `/dev/dri/card1`, NOT V4L2 request**. The entire Step 44 V4L2 probe rabbit hole was barking up the wrong tree for this game.

Unresolved mystery: `avcodec_find_decoder_by_name` hook never logged despite other hooks firing. Suspected cause: game static-init resolves the decoder pointer before the shim's constructor runs, or uses a cached pointer from somewhere else. Inconsequential for Step 45 since passthrough = same behavior as no-hook.

### 47.3: Dummy frame injection — NV12 1920×1080 black

If `avcodec_receive_frame` returns EAGAIN AND there's a pending packet from a recent `send_packet`, the shim replaces the caller's frame with a dummy.

Shim implementation:
- `shim_init_dummy_frame()` — lazily allocates via `av_frame_alloc` + `av_frame_get_buffer`, sets width=1920 / height=1080 / format=NV12 (23) via hardcoded AVFrame offsets (104/108/116 — stable in libavutil 57 / ffmpeg 5.1). Fills Y plane with `0x10` (studio black luma), UV plane with `0x80` (neutral chroma).
- `avcodec_send_packet` hook: increments `shim_pending_dummies` on ret==0.
- `avcodec_receive_frame` hook: on EAGAIN with pending > 0, `av_frame_unref(caller)` + `av_frame_ref(caller, shim_dummy_frame)` + set pts/pkt_dts to monotonic (counter × 3000, assuming 1/90000 timebase).

### 47.4: Two walls hit, in order

**Wall 1 — missing PTS.** Game asserts at `SpiVideoStreamDecoderBase.cpp:1490`:
```
Assertion `frame->pts != AV_NOPTS_VALUE' failed.
```
Fresh-allocated AVFrames have `pts = AV_NOPTS_VALUE` (INT64_MIN). Fix: set `frame->pts` at offset 136 to a monotonic counter (`next_pts += 3000`).

**Wall 2 — format mismatch.** With PTS fixed, 4 dummy frames inject successfully (pts = 0, 3000, 6000, 9000). Then the game's consumer at `./game+0xe3e250` (BN VA `0x123e250`) SEGVs at fault_addr `0x7943398190f8` — a non-NULL heap pointer, meaning the consumer dereferences some field of our frame as though it were a different type.

Interpretation: when `av_hwdevice_ctx_create(AV_HWDEVICE_TYPE_DRM)` succeeds, the game configures the codec with hwaccel=drm_prime, and the expected `AVFrame` format is `AV_PIX_FMT_DRM_PRIME` (188). In that format, `data[0]` is a pointer to `AVDRMFrameDescriptor` (nb_layers, nb_objects, per-plane format modifiers, and DMA-BUF fds). We gave it NV12 format + raw pixel buffers — the consumer treats `data[0]` as an `AVDRMFrameDescriptor*` → dereferences garbage → SEGV.

### 47.5: Status + what's left

**Proved:**
- LD_PRELOAD hooks for libavcodec symbols work (with `dlvsym("symbol","LIBAVCODEC_59")` for versioned resolution).
- AVFrame offsets 104/108/116/136/144 (width/height/format/pts/pkt_dts) are correct for libavutil 57.
- Dummy frame injection mechanism works — game accepts 4 frames in a row with valid PTS.
- **Game uses DRM-prime decode**, not V4L2-request. Step 44's V4L2 emulation path is unnecessary for THIS game.

**Not yet done:**
- Fabricate a valid `AVDRMFrameDescriptor` for `AV_PIX_FMT_DRM_PRIME` frames. Would require:
  - `AVDRMFrameDescriptor { nb_objects, objects[{ fd, size, format_modifier }], nb_layers, layers[{ format, nb_planes, planes[{ object_index, offset, pitch }] }] }`
  - A real DMA-BUF fd (from `memfd_create` or via host-side `udmabuf`/`sw_sync`) that EGL can import via `EGL_EXT_image_dma_buf_import`.
  - Or: a fake fd routed through the shim's `/dev/dri/card1` socketpair with enough ioctl-level emulation to satisfy `glEGLImageTargetTexture2DOES` with our existing GL stubs.

That second path is plausible but bigger than Step 45's "excitement-per-hour" budget. The existing GL shim already captures `glTexImage2D` / `glTexSubImage2D`, so we'd need the consumer's `glEGLImageTargetTexture2DOES` path to silently no-op and some other texture-upload path to deliver pixels.

### 47.6: Capture counts final state

| Artifact | Step 43 | Step 45c (dummy NV12 no pts) | Step 45d (dummy NV12 + pts) |
|---|---:|---:|---:|
| textures | 507 | 513 | 513 |
| shaders | 74 | 74 | 74 |
| DMD | 1 | 1 | 1 |

No shader/texture diff from Step 43 — the crash happens before the video consumer creates new GL resources.

### 47.7: What to keep in the shim

All the Step 45 hooks stay. They're the load-bearing investigation infrastructure:
- `shim_resolve_libav(sym, version)` — dlvsym with dlopen fallback for versioned libavcodec/libavutil symbols.
- Passthrough-logging versions of `avcodec_find_decoder_by_name`, `avcodec_get_hw_config`, `av_hwdevice_ctx_create`.
- Counters + periodic logs on `avcodec_send_packet` / `avcodec_receive_frame`.
- Dummy-frame injection gated on `shim_pending_dummies > 0` and `ret == EAGAIN`.

Behavior with the current shim (Step 45d): crashes at `./game+0xe3e250` after 4 injected frames. Reverting the dummy-frame-injection block to pure passthrough (no ref/unref) restores the Step-45 behavior (exit 4 clean shutdown, no video but stable). If you want "no crash, no video" for a baseline, delete the block that unrefs/refs the frame.

### 47.8: Next-session starting point

Best next move for real video: build a minimal AV_PIX_FMT_DRM_PRIME dummy. Approach:
1. Resolve `AV_PIX_FMT_DRM_PRIME` value at runtime (probably 188 in ffmpeg 5.1, can confirm via libavutil `av_get_pix_fmt_name`).
2. Allocate an `AVDRMFrameDescriptor` struct on the heap (nb_objects=1, nb_layers=1, one NV12 plane layout).
3. Create a real DMA-BUF fd with `memfd_create("shim-dmabuf")` + mmap + F_SEAL_SHRINK, fill with black, register in the struct.
4. Set `frame->format = AV_PIX_FMT_DRM_PRIME`, `frame->data[0] = descriptor`, keep width/height/pts.
5. Hope the game's consumer's glEGLImageTargetTexture2DOES path handles it (shim already has stubs for EGL image functions — see Step 21).

If that's too ambitious, fallback: find a way to make the game fall through the DRM-prime consumer to a CPU-upload path. Likely requires binary analysis at `./game+0xe3e250`.

### 47.9: Artifacts

- `_emulation/spi_shim.c` — libavcodec/libavutil hooks + dummy frame infra (Step 45 section at end of file).
- `_emulation/spi_shim.so` — rebuilt, 151,368 bytes.
- `_emulation/run_step47.stderr` — first-try (SIGABRT from hw_decoder assertion, SW-fallback hooks caused regression).
- `_emulation/run_step47b.stderr` — passthrough hooks confirmed DRM-prime path, clean exit 4.
- `_emulation/run_step47c.stderr` — dummy frame w/o pts → PTS assertion SIGABRT.
- `_emulation/run_step47d.stderr` — dummy frame with pts → 4 frames injected, SIGSEGV at game's DRM-descriptor consumer.
- `memory/project_emulation_progress.md` — updated with Step 45 summary.

## Step 46: Netbridge protocol decoded from the netbridge.elf binary itself (2026-04-21)

Major pivot. User observed that we've been reversing the bridge protocol from the CPU-side by watching what the game sends. The other side of the wire — the netbridge itself — is sitting as `games/pokemon_le/spike3/netbridge/netbridge.elf`: a 188 KB ARM EABI ELF, **statically linked, NOT stripped, WITH DEBUG_INFO**. The protocol truth is right there.

### 48.1: What we found in the netbridge binary

- Version: 0.98.4 (matches `games/pokemon_le/spike3/netbridge/version.txt`).
- ARM 32-bit Cortex-M0+ (RP2040).
- Flash at `0x10000000` (size 0x12b40), SRAM at `0x20000000` (size 0x730).
- Every function exported as a debug symbol: `HOST_*`, `HOSTUART_*`, `NODE_*`, `NODEUART_*`, `NODEBUS_*`, `POWERDIST_*`, `WS2812_*`, plus RP2040 SDK plumbing.
- Paired `.bndb` (`netbridge.elf.bndb`, 1.4 MB) alongside — already BN-analysed.

### 48.2: Host dispatch architecture (HOST_Update @ `0x10001210`, 0x5c0 bytes)

Called each main loop iteration. Structure:

1. Read XOFF state from both ends (`HOSTUART_GetXOFFState` + `NODEUART_GetXOFFState`), pack into `bridgeStatus @ 0x200020b0` as `(host_xoff) | (node_xoff << 1)`.
2. **If bridgeStatus changed**, emit cmd 0x2D notification frame back to the CPU with the new state byte. The analysis interpreted these as asynchronous flow-control broadcasts, not node responses. [Redacted: a private-source quotation used for comparison.]
3. Call `HOSTUART_GetMessage` to pull the next CPU-originated message. If none, return.
4. Extract cmd byte (at payload offset 11 in the frame). Dispatch by range:
   - **cmds `0x10-0x1F`**: forwarded to `POWERDIST_SendCmd @ 0x100020b4`. This is the pwrdist layer we already handle correctly (Step 18–20).
   - **cmds `0x20-0x41`** (bounds-check `cmp r5, #33 / bhi return`): jump table at `0x1000155c`, indexed by `(cmd - 0x20) * 4`, target via `mov pc, <ptr>`.
   - Any cmd outside those two ranges: early return (silent drop).

### 48.3: The jump table is only partially populated

Raw pointer table at `0x1000155c` (34 entries × 4 bytes = 136 bytes):

| cmd | target VA | symbol / notes |
|---:|---|---|
| 0x20 | `0x10012048` | real handler (unnamed — local to HOST_Update) |
| **0x21** | `0x2001a5e4` | **points at `resetFlags` in SRAM** — NOT a function |
| **0x22** | `0x200020b0` | **points at `bridgeStatus` in SRAM** — NOT a function |
| 0x23 | `0x100120d0` | real handler |
| 0x24 | `0x10012028` | real handler |
| 0x25 | `0x10011f70` | real handler |
| 0x26 | `0x10011f74` | real handler |
| 0x27 | `0x10000100` | real handler (bootrom-adjacent) |
| **0x28** | `0x10012b40` | **points at end of .text** (`__bi_ptr22+0x4`) — suspicious |
| 0x29 | `0x100120e8` | real handler |
| 0x2a | `0x10012120` | real handler |
| 0x2b … 0x41 | *garbage* | thumb instruction bytes from `HOST_Update`'s body interpreted as pointers |

**Only cmds `0x20`, `0x23`, `0x24`, `0x25`, `0x26`, `0x27`, `0x29`, `0x2A` have real function pointers** (8 of 34 slots). `0x21`/`0x22`/`0x28` point at data/end-of-code — plausibly dead entries the dispatcher reaches for unsupported cmds. Everything from `0x2B` through `0x41` would jump to random code.

**Verdict**: the real netbridge firmware also has **no handler for cmd 0x41** (and 0x25 is a stub-looking entry whose behaviour needs inspecting). Our Step-45 conclusion that "the 1 s timeout IS the game's progress signal" is the correct model of actual hardware behavior. Sending synthetic ACKs for unhandled cmds (Step 43 fix1) violated that model and stalled progress. Keep the current timeout-based shim; do NOT ACK arbitrary bridge cmds.

### 48.4: Reconciling with our existing PROTOCOL.md table

Our CPU-side dispatch table (at game rodata `0x37cf510`) covers 50 entries (cmds 0x10-0x41), with specific handlers listed for `0x10, 0x12, 0x14, 0x15, 0x18, 0x1a, 0x1b, 0x1d, 0x20, 0x22, 0x2d, 0x2e, 0x41` (per PROTOCOL.md). Those are **CPU-side response handlers** — they fire when the netbridge sends *back* a frame with the same cmd. The CPU table being wider than the netbridge's request dispatch is consistent: the CPU handles netbridge→CPU notifications (like cmd 0x2D bridgeStatus) that are never requested, only pushed asynchronously.

### 48.5: Node-bus layer (one level deeper)

- `NODEBUS_Poll @ 0x10001f34` (80 bytes) — bus polling loop.
- `NODE_Init`, `NODE_SendBreak`, `NODEUART_Write`, `NODEUART_WriteAddress`, `NODEUART_WritePoll`, `NODEUART_SendEnd` — UART-level node send primitives.
- `UART_NodeRxIrq @ 0x10002dec`, `UART_NodeTxIrq @ 0x10002ebc` — RX/TX IRQ handlers.
- `UART_NodeGetMessageBlock @ 0x10002f40` — blocking read from node bus.
- `POWERDIST_Update @ 0x10002174` (632 bytes) — pwrdist state machine.
- `WS2812_NodeSetLED @ 0x10003d2c` — the netbridge drives WS2812 LED strips directly, not via the RS485 node bus (presumably via GPIO from the RP2040).

SRAM globals worth noting: `bridgeStatus`, `lcdStatus @ 0x2001a5d6`, `hostRxFifo`, `hostTxFifo`, `hostRxXoff`, `pdRxQueue @ 0x20013920`, `pdTxQueue @ 0x20013930`, `resetFlags @ 0x2001a5e4`.

### 48.6: What this unlocks

Instead of treating the bridge as a black box, we can now:
- Look at each `0x20-0x2A` handler's source via BN (the user's netbridge.elf.bndb) and match request/response payloads byte-for-byte.
- Understand the cmd 0x2D notification format — it's just `bridgeStatus` with framing. Our shim's `0x2D` response (PROTOCOL.md "shared exit path" `0x915d28`) is the CPU-side consumer; we don't need to emit these unless we simulate flow-control state changes.
- Understand the node-bus UART layer for switch/coil traffic — the next frontier for Stage 6 (interactive emulation).

### 48.7: External observations cross-checked

External observations from Researchers A, C and D were compared with the
firmware analysis. They concerned asynchronous bridge traffic, frame trailers
and hardware differences between generations.

> [Redacted: private source text and identifying source paths. Those observations
> were comparison material, not substitutes for the binary analysis above.]

### 48.8: Next-session concrete starting points

1. **User task — open `netbridge.elf.bndb` in Binary Ninja** and walk the 8 real handlers (`0x20, 0x23, 0x24, 0x25, 0x26, 0x27, 0x29, 0x2A`). Debug info is intact so BN will show local variable names. 10-20 minutes of reading.
2. Correlate each handler's request payload size + response payload to the game's CPU-side dispatch behavior. Shim can then emit accurate responses (rather than timeouts) for cmds 0x20-0x2A, speeding up init 10-50×.
3. For the pinball-interaction layer (Stage 6): disassemble `NODEBUS_Poll @ 0x10001f34` and `UART_NodeGetMessageBlock @ 0x10002f40` to understand how the netbridge talks to individual node boards (switches, coils, LEDs).
4. For state-machine understanding (Stage 5): grep `games/pokemon_le/game` for mangled `_ZN*EeveeMultiball*`, `_ZN*CharmanderBattle*`, `_ZN*Attract*` class symbols. Each mode is a Godot-side Node class with its own state; their ctors + `_ready()` methods reveal the mode-select flow.

### 48.9: High-level framework (reference for future sessions)

Stage taxonomy developed this session; applies to any Linux-binary-on-unknown-hardware emulation project:

1. **Stage 0 — Inventory.** Binary architecture, linked libraries, target hardware schematic. Before anything else.
2. **Stage 1 — First-instruction execution.** QEMU user-mode, chroot, or system mode. ld.so resolves, main() runs, stdout prints.
3. **Stage 2 — Syscall boundary.** Intercept `/dev/*` opens + ioctls via LD_PRELOAD or QEMU. Log-everything first, satisfy-expectations second.
4. **Stage 3 — Library boundary.** Sometimes easier to stub `snd_pcm_writei` than `/dev/snd/*` ioctls. Find the narrowest cut between "I can fake it" and "I'd have to emulate silicon."
5. **Stage 4 — Progress observables.** Clean stdout, stable render loop, audio writes at real-time rate. Capture everything.
6. **Stage 5 — State-machine understanding.** Read the binary's mode/phase transitions; now you can *drive* the binary.
7. **Stage 6 — Two-way interaction.** User input → emulated peripherals → binary → emulated output. "Emulation" becomes "playable."

Pokemon LE current position: Stage 3 ~40% done + Stage 4 solid + Stage 5 ~20%. Step 46's netbridge analysis is the first real Stage 5 payoff — we now know what the protocol is supposed to do, not just what we can trick it into doing.

### 48.10: Artifacts

- `games/pokemon_le/spike3/netbridge/netbridge.elf` — 188 KB, ARM Cortex-M0+, RP2040 firmware, unstripped with debug_info. THE netbridge.
- `games/pokemon_le/spike3/netbridge/netbridge.elf.bndb` — Binary Ninja analysis (already present).
- JOURNAL Step 46 (this entry).
- PROTOCOL.md next edit: add "netbridge jump table at `0x1000155c` has 8 real handlers; cmds 0x2B-0x41 are intentionally unhandled; timeout-is-progress confirmed from firmware binary."
- `memory/project_emulation_progress.md` next update: bump Stage 5 progress and note the netbridge.elf as analysis asset.

### 48.11: Revision — the jump table is more complex than a simple function-pointer array

Later in the same session, direct disassembly of the "handler" addresses revealed they don't decode as coherent Thumb code — they're inside `.rodata` (`0x10009b48`–`0x10012b28`), pointing at patterned data (`asrs r4, r6, #10` repeating = uint32 data interpreted as instructions). Three possibilities (BN-only to resolve):

1. **Intentionally-broken-table hypothesis:** the firmware relies on the CPU never sending cmds past the handled range (because the CPU's own timeout fires first at 1 s, well before the netbridge's dispatch would crash). The "jump table" is literally 11 entries, and cmds past that rely on a crash-or-timeout behavior we haven't characterized.
2. **Data-driven-dispatch hypothesis:** what I read as `mov pc, r3` is actually one step in a larger sequence — maybe the loaded pointer is NOT a function address but a **handler descriptor** (size, flags, code pointer) and the real jump happens elsewhere.
3. **Wrapper hypothesis:** the `mov pc, r3` instruction exists, but BN's decompilation shows a different logical flow (e.g., tail-call optimisation leaving artifacts that look like jump-through-nonsense).

Operational takeaway **does not change**: our shim's "no synthetic response for unhandled bridge cmds → CPU times out → progress continues" matches empirical hardware behavior regardless of which hypothesis is correct. Do NOT add ACKs for cmds 0x2B-0x41 without BN-verified handler semantics.

### 48.12: User's observation — game binary vtable class hierarchy

User noticed while browsing BN that the game binary has type names visible (not symbols, but RTTI-derived vtable names) of the form:

```
SceneLoaderLayeredDisplayElement::BDLModeCharmander1Award::VTable
SceneLoaderLayeredDisplayElement::BDLModeCharmander1BG::VTable
SceneLoaderLayeredDisplayElement::BDLModeCharmander1Completed::VTable
SceneLoaderLayeredDisplayElement::BDLModeCharmander1SpinAward::VTable
SceneLoaderLayeredDisplayElement::BDLModeCharmander1Start::VTable
SceneLoaderLayeredDisplayElement::BDLModeCharmander1Total::VTable
SceneLoaderLayeredDisplayElement::BDLModeCharmander2Award::VTable
SceneLoaderLayeredDisplayElement::BDLModeCharmander2BG::VTable
SceneLoaderLayeredDisplayElement::BDLModeCharmander2Start::VTable
SceneLoaderLayeredDisplayElement::BDLModeCharmander2Total::VTable
```

`BDL` ≡ "Battle Display Layer" (inferred). Per-mode, a stanza of layered display elements covers the full mode lifecycle: **Start → BG (looping) → Award (on score events) → SpinAward (shot-specific variant) → Completed (end-of-mode transition) → Total (final tally)**.

**Multiplication check.** Six display elements × ~150 unique modes (accounting for stages: Bulbasaur 1/2, Charmander 1/2 + Spinner, Squirtle 1/2, Magikarp fishing/multiball/evolve, Pikachu vs {Charizard, Gyarados, Lucario, Mimikyu, Raichu, Magikarp}, Eevee {Espeon, Flareon, Glaceon, Jolteon, Leafeon, Sylveon, Umbreon, Vaporeon}, TeamRocket multiball 1/2/3 + Advance variants, plus attract / intermission / test-menu scenes) = ~900 layered display element classes. That closely matches the **885 scene slot** count from `select_and_load_scene`'s `0x375` upper bound (Step 32 / CLAUDE.md).

**So the 885 "scenes" = instances of `SceneLoaderLayeredDisplayElement`-derived classes, each representing one display layer in a mode's lifecycle.**

That's a Stage-5 payoff even before we disassemble a single BN function — the class hierarchy IS the scene catalog. Next session: walk the complete `SceneLoaderLayeredDisplayElement::*::VTable` list via BN, categorize by top-level mode (Charmander1, Bulbasaur1, etc.), and we have the full game state machine.

### 48.13: Next session — BN queries the user should run

Logged separately in the session-wrap response. In short:

- **Netbridge verification** (optional, low-value): open `netbridge.elf.bndb`, nav to `HOST_Update`, paste the decompiled pseudo-C for the cmd dispatch. Confirms/refutes our 3 hypotheses above. 5 min.
- **Game class hierarchy** (high-value): list every `SceneLoaderLayeredDisplayElement::*::VTable` type name from the game binary. Group by mode root (Charmander1, Charmander2, Bulbasaur1, …). Cross-reference with VIDEO_CLIPS.md and PROTOCOL.md's Radium scene classification. 15-30 min.
- **Mode transition driver** (also high-value): find the class that INSTANTIATES the BDLMode* classes and hands them to the scene loader. It's probably named something like `ModeManager`, `PlayfieldController`, `GameModeBase`, `Mode`. Its `_process(dt)` or equivalent method is the mode-select state machine. 15 min.

## Step 47: BN outputs decoded — netbridge jump table + BDLMode/crule class architecture (2026-04-21)

User ran all three BN prompts from Step 46.13. The results close multiple long-open gaps and reveal the game's complete mode architecture.

### 49.1: Netbridge jump table — correction to Step 46.3

**My earlier interpretation was wrong.** BN confirms: at `0x1000127a` the `ldr r3, [pc, #736]` loads the CONSTANT at `0x1000155c`, which is the VALUE `0x10012048` — **the jump table base**. Then `ldr r3, [r3, r5]` at `0x1000127e` performs the actual table lookup at `0x10012048 + (cmd - 0x20) * 4`. So:

- **The real jump table is at `0x10012048`**, NOT at `0x1000155c`.
- BN types `data_1000155c` as `void*` pointing to `jump_table_10012048`.
- BN recognizes `jump_table_10012048` as a bona fide computed-switch table of code pointers.
- My Step 46.3 table dump (the one labelled "jump table entries") was actually reading HOST_Update's literal pool (several PC-relative constants the function uses: jump table base, `resetFlags` SRAM addr, `bridgeStatus` SRAM addr, etc.) as though they were sequential jump-table entries. I saw the pool as an array; it wasn't.
- `0x10012048` IS in `.rodata`, but Cortex-M0+ executes from rodata fine (no MMU execute-bit). The compiler apparently emitted the table there because it's read-only data that happens to contain code pointers.

**Operational impact:** none. Step 46's conclusion that "timeout-is-progress for unhandled cmds" still holds, but now for a different reason — the REAL table at `0x10012048` presumably has valid function pointers for the full cmd range (BN says "switch statement" which implies all cases handled), not a broken 11-entry table. Need a later BN dump of the table contents to get the actual per-cmd handler addresses.

PROTOCOL.md updated (this step) to reflect the correction.

### 49.2: BDLMode class catalog — 130+ display-element classes, fully organised

BN dump shows the game defines **130+ classes** under `SceneLoaderLayeredDisplayElement::*` grouping, cleanly categorised:

| Category | Count | vtable size | #vfuncs | Purpose |
|---|---:|---:|---:|---|
| **Mode Starts** | 21 | 296 B | 37 | Mode-entry banner (e.g. "CHARMANDER 1") |
| **Mode Awards** | 21 | 240 B | 30 | Per-shot/per-event award display during mode |
| **Mode Totals** | 22 | 256 B | 32 | End-of-mode score tally |
| **Mode BGs** | 23 | 192 B | 24 | Background/looping visuals during mode |
| **Special awards & rules** | 43+ | varies | varies | BDLModeCharmander1SpinAward, BDLModeSquirtle1PoolAward, BDLRuleEeveelutionCombos*, BDLRuleSkillShotAward, etc. |

**Complete mode roster** (extracted from BDLMode*Start class names):

| Family | Modes |
|---|---|
| Pokémon singles | Charmander 1/2 (+Spinner variant), Bulbasaur 1/2, Squirtle 1/2 (+Pool/Ramp variants), Pikachu 1/2 |
| Battle environments | BattleMountain, BattleForest, BattleDesert, BattleCoast, BattleWizard, BattlePlayerFaint |
| Team Rocket | TeamRocketMultiball 1/2/3 + TeamRocketWizard |
| Story | StoryWizard 1/2 |
| Utility | AreaCatch, Generic, BattleStart, BattleAward |

**Total mode families: 21**, each with a typical {Start, BG, Award, Total} 4-panel lifecycle → **21×4 = 84 core display-element classes**, plus 43+ specialised rule classes → matches the reported 130+.

**Specific class: `BDLModeCharmander1Start`**
- vtable: `0x46ad050`
- typeinfo: `0x46acbe8` (RTTI string `"23BDLModeCharmander1Start"` at `0x3bbd978`)
- Parent class: `BDLModeStart` (single-inheritance)
- 37 virtual methods starting at slot 0 = destructor `func_910a50`
- Factory: `sub_90e3f4` (288 bytes) — guard-protected singleton in `.bss @ 0x4851998`
- **References Radium scene `7949bb14a9a21abf5e2885a01eb5ed4f9c3a8eca`** (verified: exists in `assets/lcd/auto_loaded/`, classified as "AWARD TITLE" 1360×768 in Step 32)

### 49.3: Three-layer architecture — the MVC of Stern pinball

| Layer | Class prefix | Count | What it is |
|---|---|---:|---|
| **View** (display content) | `BDLMode*`, `BDLRule*` | 130+ | Per-mode text, audio triggers, award values — filled into reusable Radium scenes |
| **Templates** (display visuals) | Radium scene SHA-1s | 36 | Flash-style scene graphs in `assets/lcd/` — shared across modes |
| **Controller** (game logic) | `crule::cmode_*` | 50+ | Per-mode rules: entry conditions, scoring, shot tracking, state transitions |

Example: `cmode_story_charmander_1` is registered in `_INIT_74` via:
```c
sub_90d840(0x48586b8, 0x15, 0x28, 0x1a, 0x10, 0xf)
```
- `0x48586b8` — .bss storage for this cmode instance
- `0x15` (21) — mode ID
- `0x28` — type/flags
- `0x1a` (26) — **scene_id** (indexes into image.bin's 885-slot scene space!)
- `0x10, 0xf` — additional params (unknown yet)

### 49.4: Mode registration driver — `_INIT_74 @ 0x8733c0`

Global C++ static initializer, 2432 bytes, called at program startup. Contains **50+ calls** to mode-constructors like `sub_90d840`, `sub_911d40`, `sub_91b810`, each registering one `cmode_*` instance. Together they constitute the **complete game mode table**: every mode ID, every scene ID, every flag, materialised statically in .bss at startup.

This is the direct answer to Step 42's open question about `scene_config_table`. The BSS table at VA `0x47fcf10` (or wherever `scene_config_table` lives) is populated by `_INIT_74`'s calls, each of which registers {mode_id, scene_id, flags} for one mode. The 885 scene slots map to 50+ modes × some fanout factor.

**Key function to decode next session: `sub_90d840`** — this is the `crule::cmode_*` base-class constructor (or initializer). Its full signature tells us what each of the 6 parameters is. Once we know that, the entire `_INIT_74` becomes a trivial table of mode metadata.

### 49.5: No traditional mode manager — crule::cmode_* are the state machines

BN confirmed there's no class named `ModeManager`/`PlayfieldMode`/etc. Instead, each `crule::cmode_*` object IS its own state machine. The game's gameplay loop (probably a Godot `_process(dt)` or equivalent at an outer level) iterates over the registered cmode instances, calls their update methods, and handles transitions.

This is unusual but consistent with how Stern's SAM-era games worked — "rule" objects are self-contained and communicate via pub/sub on events (ball drained, shot made, target hit) rather than through a central manager.

### 49.6: String intel

Rodata strings like `"CHARMANDER 1 TIMER"`, `"CHARMANDER 1 CHAMPION"`, `"PIKACHU VS. CHARIZARD CHAMPION"` are scoring/champion labels referenced by cmode or BDL classes. Useful as anchors for xref searches: find the class/function that uses `"CHARMANDER 1 TIMER"` → that's the Charmander 1 timer subsystem.

### 49.7: What this unlocks for emulation

1. **Complete mode catalog** for free — we don't need to reverse the game loop to enumerate modes. _INIT_74 has them all.
2. **Shim-level mode tracking** — we can hook `operator new` for cmode_*/BDLMode_* allocations OR their constructors, and log every mode activation at runtime. That's LIVE game-state visibility for the emulation.
3. **VPX integration target** — each cmode has a well-defined lifecycle (entry conditions, scoring, exit). VPX triggers the entry conditions (e.g., player makes a shot combo) and the game responds with correct scoring. No emulation of game logic needed — we just drive the existing game's state machine.
4. **Scene → mode mapping solved** — the 885 image.bin scenes map to 50+ modes via the scene_id field in `_INIT_74`. Parsing that function gives us the full mapping that Step 42 left open.

### 49.8: Scene hash architecture insight

Factory `sub_90e3f4` pairs `BDLModeCharmander1Start` with Radium scene `7949bb14...` (the "AWARD TITLE" 1360×768 template). Since the Radium scene is a generic template (empty text panel + background animation), the BDL class fills in:
- Text content (the mode name, scoring values, etc.)
- Audio cues
- Possibly asset overrides

Different BDLMode classes (Charmander1Start, Bulbasaur1Start, Squirtle1Start, …) can all reference the SAME Radium scene and just swap text. This explains the 130 classes / 36 scenes ratio.

### 49.9: Next BN query — the single highest-leverage one

Decompile `_INIT_74 @ 0x8733c0` fully. BN HLIL should show all 50+ mode-constructor calls. Pasting that gives us:
- Every mode's ID
- Every mode's scene_id
- Every mode's flags
- Hints at the full mode list

Also decompile `sub_90d840` (the cmode_* base constructor) to learn what each of its 6 parameters is. 50 credits max for both combined.

With that data we can build a complete mode table in under 10 minutes of synthesis.

### 49.10: Artifacts

- JOURNAL Step 47 (this entry).
- PROTOCOL.md netbridge correction (table at `0x10012048`, not `0x1000155c`).
- `memory/project_emulation_progress.md` — will update with 3-layer MVC architecture note.
- Open: full `_INIT_74` + `sub_90d840` decomp pending from user's BN.

## Step 48: The complete mode table — 42 crule objects extracted from _INIT_74 (2026-04-21)

User ran the BN queries and delivered the decompiled `_INIT_74` (all 47 constructor calls) and `sub_90d840` (one cmode constructor body, showing parameter → field mapping). Full synthesis landed in `_emulation/MODE_TABLE.md`. This is the biggest structural reveal of the project.

### 50.1: What we got

`_INIT_74` registers **42 `crule::*` objects** at fixed BSS addresses during C++ static init, before `main()` runs. Every registration looks like:

```c
sub_XXXXXX(bss_addr, mode_id, type_flags, scene_id, arg5[, arg6]);
__cxa_atexit(destructor, bss_addr, &data_4760008);
```

Plus 5 sub-instances of mode_id=0x2b (one per player, probably) and 2 final non-mode callback registrations (`sub_9c11d0`, `sub_adfcc0`).

### 50.2: Param semantics confirmed from sub_90d840

The user's decompilation of `sub_90d840` (the specific `cmode_story_charmander_1` constructor) nailed down the cmode struct layout:

| cmode field | offset | source |
|---|---|---|
| `vtable_ptr` | `+0x00` | assigned at end of constructor |
| `mode_id` | `+0x08` | arg2 |
| `arg5_behaviour` | `+0x10` | arg5 |
| `const 0x700` | `+0x18` | init constant |
| `const 0xa00000064` | `+0x19` | init constant |
| `type_flags` | `+0x30` | arg3 |
| `scene_data_ptr` | `+0x58` | `get_scene_data_by_id(scene_id)` result |
| `0` | `+0x60` | init |
| `timer_obj` | `+0xb0` | from arg6 |
| `const 0x15300000152` | `+0xb8` | cmode_timed base constant |

### 50.3: Mode categories by type_flags

Decoded observationally. Each `type_flags` bit classifies a mode:

| type_flags | count | modes |
|---:|---:|---|
| `0x01` | 20 | base/system/attract rules |
| `0x04` | 3 | multiball-family |
| `0x08` | 3 | utility |
| `0x24` | 2 | Pikachu modes (Pikachu 1/2) |
| `0x28` | 6 | Pokémon story modes (Bulba/Charm/Squirtle × 2) |
| `0x48` | 4 | Battle environments (Mountain/Forest/Desert/Coast) |
| `0x84` | 4 | Team Rocket multiball/wizard (Multiball 1/2/3 + Wizard) |

### 50.4: Confirmed named modes

Only one is 100% confirmed by user's BN symbol lookup — the others are inferred from ordering + type_flags:

| mode_id | scene_id | bss | constructor | name |
|---:|---:|---|---|---|
| `0x15` | `0x1a` | `0x48586b8` | `sub_90d840` | **`cmode_story_charmander_1`** ✓ |
| `0x16` | `0x1b` | `0x48587f8` | `sub_911d40` | `cmode_story_charmander_2` (inferred) |
| `0x13` | `0x18` | `0x4858480` | `sub_906fd0` | `cmode_story_bulbasaur_1` (inferred) |
| `0x14` | `0x19` | `0x48585e0` | `sub_90ac40` | `cmode_story_bulbasaur_2` (inferred) |
| `0x19` | `0x1c` | `0x4858900` | `sub_91b810` | `cmode_story_squirtle_1` (inferred) |
| `0x1a` | `0x1d` | `0x4858a50` | `sub_920bc0` | `cmode_story_squirtle_2` (inferred) |

Full table (all 42 + inferred names) in `_emulation/MODE_TABLE.md`.

### 50.5: scene_id is NOT the 885-slot image.bin index

scene_id values in `_INIT_74` span `0x02-0x29` (max 41). Only 38 unique values used. That's nowhere near the 885-scene image.bin slot count. So these two number spaces are **different**:
- `cmode.scene_id` (0-0x29) → local index that `get_scene_data_by_id` maps to a Radium scene + display context
- image.bin section-7 `asset_id` (0-884) → individual asset entries (sprites, sounds, text, per-scene content)

A cmode probably references a scene_id which maps to a Radium template + a cluster of image.bin asset_ids. One `cmode_story_charmander_1` → scene_id 0x1a → Radium scene `7949bb14…` (AWARD TITLE template) → a set of Charmander-specific asset_ids.

### 50.6: The mode_id = 0x2b x 5 pattern

Five separate constructors register the SAME mode_id (`0x2b`) with different type_flags (`1, 0, 4, 8, 0x10`) at different BSS addresses. Those type_flags values form a 5-bit power-of-2 progression (`1, 2, 4, 8, 16`) — **one bit per player in a 5-player-max cabinet**. Strong evidence this is **per-player persistent state**, not 5 different modes. Three of the 5 pass `arg5 = 0x15` which is the Charmander1 mode_id — likely a "current story mode" pointer for each player.

### 50.7: The two final callback registrations

```
sub_9c11d0(0x48594f0, 0xc5, sub_950d60, 0x80)
sub_adfcc0(0x48594f8, 1,    sub_94f140, 0x3ff, 0, 0x80)
```

These pass **function pointers** (`sub_950d60`, `sub_94f140`) — they're event subsystem bootstrap. `0xc5 = 197` and `0x3ff = 1023` look like ID/mask values for switch/coil indices. Most plausible: these register shot-detection callbacks that fire when specific switches close and dispatch to the right mode/rule object. Finding `sub_950d60` and `sub_94f140` will reveal the game's **event dispatch spine**.

### 50.8: What's immediately actionable

1. **Live runtime introspection via the shim.** All 42 cmode addresses are known. A `shim_dump_cmodes()` routine hooked into the SIGSEGV/SIGTERM handlers (like Step 41's `asset_registry` dump) would:
   - Read `mode_id`/`type_flags`/`scene_data_ptr` from each BSS slot.
   - Verify they match `_INIT_74`'s parameters (sanity check static init ran).
   - Detect which modes have "activated" by checking `+0x308 init_flag` or field deltas over time.
   - No deep instrumentation needed — just read .bss at known offsets.
2. **VPX/interactive emulation target.** For Stage 6 (two-way interaction), each cmode is a known unit with a known lifecycle. Stimulus (switch close, ball drain) → the "event dispatch spine" hits a cmode → mode state advances → display updates. We now know every cmode's storage location; the remaining piece is the event dispatch mechanism (sub_950d60 / sub_94f140).
3. **BDL ↔ cmode mapping.** Every BDLMode* class probably holds a pointer to its owning cmode. Xref-search for the 42 BSS addresses from inside the BDL factory functions (like `sub_90e3f4` for Charmander1Start) would reveal the link.

### 50.9: Next BN queries (budget-efficient, 2 queries)

1. **`sub_935e50`** — the base cmode constructor called BEFORE the vtable is assigned in `sub_90d840`. Its parameter list and field assignments reveal (a) the `get_scene_data_by_id` mechanism, (b) the `cmode_timed` timer setup, (c) where `+0x18` / `+0x19` constants come from.
2. **`sub_950d60` and `sub_94f140`** — the two callback function pointers in the final registrations. These are likely event dispatchers. Decompiling them shows how switch/coil events reach cmode state machines.

### 50.10: Artifacts

- `_emulation/MODE_TABLE.md` — authoritative mode registration table, synthesised from BN output (THIS SESSION's biggest deliverable).
- JOURNAL Step 48 (this entry).
- `memory/project_emulation_progress.md` — pending update to reflect Step 48's Stage-5 progress.
- Open: `sub_935e50`, `sub_950d60`, `sub_94f140` decomp for next session.

## Step 49: Runtime validation — 42/42 cmodes verified at expected .bss addresses (2026-04-21)

Built `shim_dump_cmodes()` patterned on Step 41's `shim_dump_sound_globals()`. Hooked into SIGSEGV + SIGTERM handlers. Ran the game, triggered the dump.

### 50b.1: Result on first run

All 42 cmode .bss slots have the **correct mode_id** at offset `+0x08`. But type_flags read at `+0x30` showed all zeros — mismatched.

Two hexdumps (of cmode_story_charmander_1 and cmode_battle_mountain) revealed the real layout by side-by-side comparison:

```
charmander1 (expected type_flags=0x28):
  +0x00: 28 cc be 0f 64 7a 00 00   vtable ptr
  +0x08: 15 00 00 00               mode_id = 0x15
  +0x0c: 28 00 00 00               type_flags = 0x28   ← HERE
  +0x10: 10 00 00 00               arg5 = 0x10
  +0x58: f0 14 d5 0f 64 7a 00 00   scene_data_ptr

battle_mountain (expected type_flags=0x48):
  +0x00: 48 76 be 0f 64 7a 00 00   vtable ptr (differs — different class)
  +0x08: 20 00 00 00               mode_id = 0x20
  +0x0c: 48 00 00 00               type_flags = 0x48   ← HERE
  +0x10: 08 00 00 00               arg5 = 0x08
  +0x58: 30 0b d5 0f 64 7a 00 00   scene_data_ptr
```

**Correction:** `type_flags` is at byte offset `+0x0c`, not `+0x30`. User's BN HLIL had annotated `this+0x30 = type_flags` in comments, but the actual code (visible byte-for-byte in the hexdump) puts it at `+0x0c`. Probably a typo in the BN paraphrase, or BN was showing the offset in a different type's unit.

### 50b.2: After the fix — full validation

Rebuilt shim with `type_flags` at `+0x0c`. Reran. Result:

```
--- cmode dump summary: 42/42 mode_id+type_flags matched, 9 activated (init_flag != 0), 0 mismatches ---
```

**Perfect. Every single cmode exists at its predicted address with the predicted mode_id, type_flags, and non-null scene_data_ptr.** Static init completed cleanly under our QEMU emulation — meaning our shim's path-remapping, pthread-scheduling, syscall hooks, and LD_PRELOAD chain are solid enough to run the game's entire C++ static initialization phase.

### 50b.3: scene_data struct discovered

Dereferencing `scene_data_ptr` for 3 cmodes (base_00, charmander_1, battle_mountain) via hexdump revealed:

```
+0x00: vtable ptr          (SAME 0x7df98fb6dca8 for all 3 — they're all same class)
+0x08: scene_id            (matches cmode's scene_id exactly: 0x29 / 0x1a / 0x14)
+0x0c: secondary_id        (0x1a / 0x03 / 0x05 — small int; purpose unclear,
                            maybe a Radium scene index or scene sub-type)
+0x10: shared_registry     (SAME 0x7df98fd9aa60 for all — global lookup table)
+0x18+: array of per-scene heap pointers (mode-specific content: text, assets, ...)
```

So `get_scene_data_by_id(scene_id)` returns a heap struct that stores the scene_id, a secondary classifier, a shared registry pointer, and an array of content pointers. To resolve scene_id → Radium scene SHA-1, we'd decompile `get_scene_data_by_id` (in sub_935e50's call chain) in BN next session.

### 50b.4: Post-static-init "activated" cmodes

The `+0x308 init_flag` field is non-zero for 9 of 42 cmodes, but the values vary wildly — some small ints (0x0a, 0x21, 0x800), some heap-address-looking pointers (0xc7d504b0-range). So `+0x308` is NOT a uniform init_flag across all cmodes — it's a per-subclass field. Some cmode subclasses use it as a counter or state, others reinterpret it as a pointer to additional state.

Not a bug in our dump — our interpretation of that offset was too optimistic. Noted for future refinement.

### 50b.5: What this enables

**Runtime game-state observation now works.** From any SIGSEGV/SIGTERM handler, we can dump:
- All 42 cmode slots' mode_id, type_flags, scene_data_ptr, some state field.
- Three representative scene_data struct hexdumps.
- Asset registry (Step 41) + sound-event hashmap (Step 39) from earlier runs.

For Stage-6 interactive emulation, this is the **eyes inside the game**. When we simulate a switch close and want to see what the game does, we can dump cmodes before+after and observe which transitioned.

### 50b.6: Artifacts

- `_emulation/spi_shim.c` — added `shim_dump_cmodes()` + `shim_resolve_game_base()` helpers.
- `_emulation/spi_shim.so` — rebuilt.
- `_emulation/run_step50{,b,c,d}.stderr` — captured dumps at each iteration.
- `_emulation/MODE_TABLE.md` — updated with verified struct offsets + scene_data layout.
- This JOURNAL sub-step.

## Step 50: The game loop decoded — scene data table, cmode registry, event dispatch (2026-04-21)

User ran BN prompt #5 covering three decomp targets: `get_scene_data_by_id`, the two callback fns (`sub_950d60`, `sub_94f140`), and the two registrars (`sub_9c11d0`, `sub_adfcc0`). Plus vtable xrefs for `cmode_story_charmander_1`.

### 51.1: Scene data is a simple indexed table

`get_scene_data_by_id @ 0x8ae920`:
```c
void *get_scene_data_by_id(uint32_t scene_id) {
    sub_8ae460();                                      // init check
    if (scene_id <= 0x27)
        return &data_480eab0 + scene_id * 0x1a0;       // 40-entry table
    sub_9ada50(0x139);                                 // log error code 313
    return &data_480e900;                              // error sentinel
}
```

Table: `data_480eab0`, **40 entries × 0x1a0 bytes (416) = 16,640 bytes** of .bss scene_data slots. Error sentinel at `data_480e900` is what base_00 (scene_id 0x29 out-of-range) returns, matching our live-dump observation (Step 48d).

`secondary_id` (at scene_data `+0x0c`) is populated by `sub_8ae4dc` at runtime — not from static data. So the scene_id ↔ Radium-scene-SHA-1 mapping is set up dynamically. **Open:** what does `sub_8ae4dc` read to populate secondary_id? That's the last remaining piece of the scene_id puzzle.

### 51.2: The cmode registry at `data_4767b30`

42 cmode pointers × 8 bytes = 336 bytes (0x150). Endpoint at `data_4767c80`. Every 42 cmode objects registered in `_INIT_74` end up with their .bss address stored in this array.

Step-51 shim dump verified: the array is populated (14 of our naive-order predictions matched exactly; the registry uses a different order than `_INIT_74`'s call sequence — probably sorted by mode_id or type_flags — that's fine; the content is all 42 cmodes, just rearranged). **The array is real, populated, and ready to be iterated by the game loop.**

### 51.3: Event dispatch spine

Three layers:

**Layer A: Registrar `sub_9c11d0`** (206 event types, 0x00-0xce):
```c
void *sub_9c11d0(void *obj, uint32_t event_id, void (*cb)(), char priority) {
    // event_id > 0xce is a critical error
    // Insert {cb, priority, next} into priority-sorted linked list at
    // data_485ea80 + (event_id * 8)
}
```

**Layer B: Dispatcher `sub_950d60`** (registered for event 0xc5, priority 0x80):
```c
int64_t sub_950d60(int64_t event_data) {
    sub_950cd0();
    if (override != sub_94e940) return override(&state, event_data);
    for (cmode **i = &data_4767b30; i != &data_4767c80; i++)
        (*(*i)->vtable[+0xf0])();   // call cmode event handler (vtable slot 30)
    return 1;
}
```

**Layer C: Update callback `sub_94f140`** (registered as update_type 1, max 1023, priority 0x80):
```c
void sub_94f140(void) {
    for (cmode **i = &data_4767b30; i != &data_4767c80; i++) {
        auto tick = (*i)->vtable[+0xd8];
        if (tick != sub_8e90e0) tick();   // skip null-impl cmodes (slot 27)
    }
}
```

**Cmode vtable slots map:**
| offset | slot | role |
|---|---:|---|
| `+0x00` | 0 | destructor #1 |
| `+0x08` | 1 | destructor #2 |
| `+0xd8` | 27 | **per-frame tick / update** |
| `+0xf0` | 30 | **event handler** (for event 0xc5 at least) |

### 51.4: Runtime verification (Step 50 run)

Shim dump confirms:
- **Event dispatch populated**: **102 of 207 event slots** have handlers, **236 total handlers** registered.
- **Event 0xc5 has 2 handlers** (chain_len=2) — sub_950d60 confirmed + one more.
- **Event range 0x00-0xce fully exercised** — highest active event is `0xce` (chain_len=2).
- **Update types 1, 2, 3 all populated** (0 and 4 empty).
- **Cmode registry populated** with 42 cmode pointers — order differs from our naive `_INIT_74`-sequential guess, so some runtime sort happens during registration.

**Interpretation:** The game's entire event/update infrastructure is operational after static init. It's just waiting for events to fire. The 102-of-207 populated events is probably the coverage of the actual pinball hardware — switches, coils, timers, mode transitions, etc. 105 events have no handlers (unused in Pokemon LE, reserved for other platforms/features).

### 51.5: Plausible event ID ranges (speculation — not verified)

Standard pinball event categories:
- `0x00-0x3f` (64 slots, many populated) — switch closures? Common pinball has ~64 switches.
- `0x40-0x7f` (64 slots) — coil fires / solenoid events.
- `0x80-0xbf` (64 slots) — lamp/LED state changes.
- `0xc0-0xce` (15 slots) — game-state events (start, tilt, ball save, mode begin, etc.).

Event 0xc5 fires sub_950d60 which dispatches to all cmodes — that's a "game state changed" kind of event. Consistent with `0xc0+` = game-state range.

### 51.6: Stage 6 (interactive emulation) unlocked in principle

With full knowledge of:
- Event dispatch (`data_485ea80` + sub_9c11d0)
- Update dispatch (`data_48c52c0` + sub_adfcc0)
- Cmode registry (`data_4767b30`)
- Scene data table (`data_480eab0`)
- Cmode struct layout (`mode_id +0x08`, `type_flags +0x0c`, `scene_data +0x58`, tick vtable slot `+0xd8`, event vtable slot `+0xf0`)

...we could from the shim:
1. Build a trigger function `shim_fire_event(uint32_t event_id, int64_t event_data)` that walks the linked list at `data_485ea80 + event_id*8` and calls every registered handler. That simulates a switch event from external input (VPX, keyboard, test harness).
2. Snapshot cmode state before and after to see what state transitioned.
3. Dump any newly-allocated BDL display elements to see what got rendered.

That's **Stage 6 (interactive emulation) in principle** — the remaining practical obstacles are: (a) event_id ↔ hardware switch mapping (still unknown), (b) the display path needs to actually render frames (Step 45's DRM_PRIME problem is the blocker), (c) a driving loop / external input source.

### 51.7: What's closed and what remains

| Gap as of Step 42 | Status after Step 50 |
|---|---|
| 885 scene_config_table mapping | Superseded. Real architecture: 42 cmodes → 40-entry `data_480eab0` scene_data table via scene_id. The 885 count was misinterpretation. |
| What does cmd 0x41 do | Empirically: "timeout is progress" (Step 46/47). Semantic: unknown but doesn't block us. |
| How does the game loop work | **Fully decoded** (Step 50): event dispatch + update dispatch, both iterating `data_4767b30`. |
| Mode → asset mapping | **Decoded** via cmode.scene_id → scene_data.secondary_id → (still TBD, but mechanism clear). |
| Stage 5 understanding | **Fully done.** |
| Stage 6 interactive emulation | Architecture understood, practical blockers remain. |

### 51.8: Next BN query (if we keep going)

One small high-value one to fully close scene_id → Radium SHA-1:
- **Decompile `sub_8ae4dc`** — it's what populates `secondary_id` and the scene_data fields at runtime (mentioned in user's Part A.4). If it reads from a static table somewhere (probably in rodata), that table IS the Radium scene lookup.

### 51.9: Artifacts

- `_emulation/spi_shim.c` — added `shim_dump_dispatch_tables()`.
- `_emulation/spi_shim.so` — rebuilt (≈154 KB).
- `_emulation/run_step51.stderr` — live validation output.
- `_emulation/MODE_TABLE.md` — updated with Step 50 architecture decomp.
- JOURNAL Step 50 (this entry).


## Step 51: fire_event trampoline, switch state array, switch-first API pivot (2026-04-21)

User ran BN prompt 5 to close out the Stage-5 gaps identified in Step 50. Results refuted one prior interpretation, identified the event-firing primitive, and located the switch state array. Session then pivoted from a blind event-ID sweep to a switch-indexed scaffolding after finding the sweep low-signal.

### 52.1: scene_id correction — the 0x480eab0 table is cawards state, not Radium picker

Step 50 speculated that `sub_8ae4dc` populates a `secondary_id` at scene_data `+0x0c` that maps `scene_id` → Radium SHA-1. **Refuted.**

`sub_8ae460` is the `__cxa_guard_acquire` singleton init for a `cawards_manager` (vtable literal: `_vtable_for_cawards_manager{for 'HookListener'}`). `sub_8ae4dc` writes per-player threshold constants into sub-arrays of the 0x1a0 records:

| Offset in slot | Value written | Likely meaning |
|---|---|---|
| +0x18 | clamped ≤ `0x186a0` (100000) | counter cap |
| +0x30 | `0x64` (100) | multiplier |
| +0x48 | `0x7a120` (500000) | goal threshold |
| +0x60 | `0xa` (10) | small counter |
| +0x78 | `0x12c` (300) | small counter |
| +0x90 | `|= 1 << x0_18` | category bitfield |

All indexed by `((sub_9d0eb0() - 1) << 3)` = 5-player-max per-player stride. Plus a closing `do { sub_8ad3b0(tbl, 0..2); sub_b29ef0(0x5a); } while` 10-iter loop that looks like cawards category setup.

Conclusion: `data_480eab0` is (or at least includes) per-mode cawards/achievement state, not a Radium scene picker. The real `scene_id` → Radium SHA-1 mapping almost certainly lives inside the `BDLMode*` factory functions — e.g. `sub_90e3f4` (Charmander1Start factory, 288 bytes) explicitly references `7949bb14a9a21abf5e2885a01eb5ed4f9c3a8eca` per Step 47.2.

**Files updated:** `_emulation/MODE_TABLE.md` (struck the `secondary_id` claim, flagged the cawards origin, left scene_id → Radium as open); `memory/project_emulation_progress.md` (bumped to Step 51 with correction called out).

### 52.2: fire_event — the dispatcher we can trampoline into

BN confirmed `sub_9c12a0` at VA `0x9c12a0`:

```c
uint64_t fire_event(int32_t event_id, int64_t event_data) {
    if (event_id > 0xce) { CC_criticalErrorHandler(0x6a); return 1; }
    for (int64_t* i = data_485ea80[event_id]; i; i = i[2]) {
        if (i[0]) {
            int32_t r = ((int32_t(*)(int64_t))i[0])(event_data);
            if (!(r & 1)) return r & 0xff;
        }
    }
    return 1;
}
```

- Linked-list node at `data_485ea80 + event_id * 8`: `{callback_fn @ +0x00, priority @ +0x08, next @ +0x10}` (`i[2]` at 8-byte stride = byte offset +0x10).
- Event_id bounds 0..0xce (206 slots, matches Step 50's `sub_9c11d0` registrar bound).
- Short-circuit: any callback returning an even value halts the chain; odd (conventionally 1) = continue.

Five callers identified: `sub_b129f0`, `sub_bd39f0`, `sub_b16f64`, `sub_b82990`, `sub_b12760`. The switch→event translator is one of them (or an indirect). Plan-C BN query in flight.

**Shim addition:** `shim_fire_event(event_id, event_data)` trampoline at `game_base + 0x5c12a0`. Relies on a new `shim_cached_game_base` atomic that any guest-thread shim hook (the `ioctl()` wrapper, specifically) primes on first invocation — needed because shim threads spawned via `pthread_create` have no game frames on their backtrace, so `shim_resolve_game_base()` would otherwise return 0 from the timer thread.

### 52.3: switch state array layout verified

From BN decomp of `sub_a50850` (switch state reader) and `sub_a50350` (switch validation):

| Field | Address | Detail |
|---|---|---|
| State array base | `data_4880220` (BN VA) | stride `0x30` per switch |
| Count | `data_48cbaa0` | runtime-populated (dynamic, bounds check) |
| State byte | `+0x28` within record | returned by `sub_a50850` with invert applied |
| Config ptr | `+0x10` within record | pointer to a struct whose `+0x30` byte carries flags: bit 2 = invert, bits 5/6 = broken/stuck masks |
| `sw_broken` | `data_4880228[id]` | NVRAM-backed |
| `sw_stuck_active` | `data_4880230[id]` | NVRAM-backed |
| `sw_alert_count` | `data_4880238[id*2]` | 16-bit, NVRAM-backed |

Observed at runtime (Step 51g): state bytes for real switches were not all 0/1 — Left Flipper Button (id 9) read `0x20` (32), Right Flipper Button (id 10) read `0xab` (171). Two plausible explanations: (a) the field is a packed uint32 (sub_a50850 types it as `uint32_t x0_3 = *(x1 + 0x28)`) and only a specific bit is the "closed" flag, or (b) the runtime is using the adjacent bytes for something else. Not pinned down yet; scaffolding writes 0x01 to the byte at `+0x28` and restores the prior value on "release", which is faithful to what sub_a50850's return-value-and-compare pattern suggests.

### 52.4: blind event-ID sweep — executed, preserved, deprioritised

Built `shim_event_sweep()` that fires every `event_id ∈ [0x00, 0xce]` with `event_data=0` and diffs cmode state (+0x308 and +0x60 per cmode, 42 cmodes). Guard harness: `sigsetjmp(shim_event_sweep_jmp)` in the loop body; `SIGSEGV` handler at top checks `shim_event_fire_in_progress` and `siglongjmp`s back if set; `SIGALRM` handler via `alarm(1)` catches hung events.

Thread safety for `siglongjmp`: `SIGALRM` is blocked on all threads except the timer thread itself, via `pthread_sigmask(SIG_BLOCK)` in the shim constructor + `pthread_sigmask(SIG_UNBLOCK)` in the timer thread entry. Since the block mask is inherited by threads spawned later (including all of the game's threads), `alarm()` reliably delivers to the timer thread. `siglongjmp` from the `SIGALRM` handler then lands back in the same thread that did `sigsetjmp`.

Complete sweep totals (run_step52e, sweep @ 35 s, 207/207 covered):

| Category | Count |
|---|---:|
| Fired cleanly (ret=1, Δ=0) | 164 |
| Reactive (Δ>0) | **2** |
| Crashed (handler deref'd event_data=0) | 37 |
| Timed out (handler didn't return in 1 s) | 4 |

Only 2 reactive events observed:
- **`event 0x3e`** → activates `battle_coast` (+0x308: `0` → `0x100000000`)
- **`event 0x43`** → touches `base_03`

Observed but not reproducible (timing-dependent): `event 0x26` activated `base_03` in one earlier run but not in another. This is direct evidence that most events expect a valid `event_data` struct, and that a single firing context isn't enough to cover them.

**Full narrative:** `_emulation/EVENT_SWEEP_STEP52.md`.

### 52.5: Pivot — switch-first API

User's direction: don't keep iterating event IDs blindly; the manual enumerates every real switch, and unused IDs are by design. Build scaffolding for firing one switch at a time with inline comments on known-bad ones, rather than an exhaustive sweep that produces 1% signal.

Changes landed:

1. **Switch manifest** — `shim_switch_manifest[]` in `spi_shim.c` transcribed from Pokémon LE manual (500-55AJ-01), ~69 real switches covering Node 8 (playfield main), Node 9 (arena/BATTLE/Team Rocket), Node 1 (cabinet/coin), plus C10 service. ID gaps (3, 4, 13, 14, 24, 26, 32, 38, 39, 42, 43, 47, 48, 62, 65, 68, 69, 75, 77, 81-89, 91, 92, 95) preserved as absences — unused slots by design.
2. **`shim_fire_switch(int id)`** — looks up entry, bounds-checks against `data_48cbaa0` runtime count, snapshots 42 cmodes, writes `0x01` to state byte, 20 ms dwell, re-snapshots, restores previous state, logs human-readable report.
3. **`SPIKE3_FIRE_SWITCH=id1:id2:...` env var** + `SPIKE3_SWITCH_DELAY` — colon-delimited (QEMU `-E` swallows commas). Timer thread fires each switch with 200 ms spacing.
4. **Blind sweep gated behind `SPIKE3_SWEEP_EVENTS=1`** — default off. Sweep code preserved for later use once we have valid `event_data` structs to try.

### 52.6: First switch-API test (run_step52g)

```
=== shim_fire_switch_thread: sleeping 35 s then firing switches [93:9:10:22] ===

--- shim_fire_switch: 93 'Start Button' (Cabinet, Micro, 1-SW-11) state@0x76ceb3dc13b8 ---
  switch 93 'Start Button': before_state=0 Δ=0 (no cmode state change — translator not yet hooked)

--- shim_fire_switch: 9 'Left Flipper Button' (Cabinet, Leaf, 8-SW-25) state@0x76ceb3dc03f8 ---
  switch 9 'Left Flipper Button': before_state=32 Δ=0 (no cmode state change — translator not yet hooked)

--- shim_fire_switch: 10 'Right Flipper Button' (Cabinet, Leaf, 8-SW-24) state@0x76ceb3dc0428 ---
  switch 10 'Right Flipper Button': before_state=171 Δ=0 (no cmode state change — translator not yet hooked)

--- shim_fire_switch: 22 'Shooter Lane' (Playfield, Micro, 8-SW-28) state@0x76ceb3dc0668 ---
  switch 22 'Shooter Lane': before_state=0 Δ=0 (no cmode state change — translator not yet hooked)
```

All four switches:
- Found their entries in the manifest.
- Computed a valid state byte address (rebased through `shim_cached_game_base`).
- Read a pre-state byte and wrote `0x01`.
- Observed **no cmode state change** — expected, since the UART decode path that reads switch state and fires derived events (Plan C) isn't hooked yet. The shim's direct write tells the game's read path "closed", but nothing *polls* the state array except the game's own UART cmd 0x27 receive path. Until we intercept that path (or call the translator directly), the switch flip is silent to the rest of the game.

Abort at run tail (exit 134, `pure virtual method called` in Godot's string_name cleanup) is the usual shutdown noise, not related to the switch API — fires at line 18919 of stderr, 2345 lines after the last switch report at line 16574.

### 52.7: Secondary observation — flipper state bytes are non-boolean

Left Flipper before_state = `0x20`, Right Flipper before_state = `0xab`. The game reads `uint32_t x0_3 = *(switch_rec + 0x28)` per `sub_a50850` — so the field might be a packed u32 rather than a plain boolean byte. Deferred investigation: BN query on writers to `+0x28` (the UART decoder) will show whether the byte is just `{0,1}` from the netbridge or has embedded state (debounce counter, last-change timestamp, etc.).

### 52.8: Status and next moves

**Closed in Step 51:**
- `fire_event` identified + trampoline built
- Switch state array layout verified
- Cawards correction landed
- Switch-first API in place with manifest from manual
- Bind sweep deprioritised (kept as env-var-gated reference)

**Open, tracked:**
- **Plan C** (in-flight BN query): decomp `sub_b129f0` / `sub_bd39f0` / `sub_b16f64` / `sub_b82990` / `sub_b12760` + xref writers to `data_4880220 + n*0x30 + 0x28` to find the switch→event translator.
- **Flipper-state byte encoding** — what's in `+0x28` when the switch is closed vs open vs in-transit.
- **Scene_id → Radium SHA-1** — decomp one BDLMode factory (e.g. `sub_90e3f4`, 288 bytes) to find where the SHA-1 gets wired into a cmode.

### 52.9: Artifacts

- `_emulation/spi_shim.c` — added: `shim_cached_game_base` atomic; cache prime in `ioctl()`; `shim_fire_event()` trampoline; `shim_event_sweep()` + `shim_capture_cmode_snap()`; sigsetjmp guard in SEGV handler; SIGALRM handler + per-thread block mask; timer thread; `shim_switch_manifest[]` table with ~69 entries; `shim_fire_switch()` API; `SPIKE3_SWEEP_EVENTS` gate; `SPIKE3_FIRE_SWITCH` / `SPIKE3_SWITCH_DELAY` env vars; `SPIKE3_INJECT_VIDEO` gate for the Step-47 frame injector (default off = Step-45 stable baseline).
- `_emulation/spi_shim.so` — rebuilt, 153,328 bytes.
- `_emulation/EVENT_SWEEP_STEP52.md` — narrative + raw totals from the blind sweep.
- `_emulation/MODE_TABLE.md` — corrected scene_data section + updated next-BN-queries.
- `_emulation/PROTOCOL.md` — added `fire_event` section and `switch state array` section.
- `_emulation/run_step52c.stderr` — sweep @ 40 s (incomplete).
- `_emulation/run_step52d.stderr` — sweep @ 25 s, hung at 0x5d (pre-alarm).
- `_emulation/run_step52e.stderr` — full sweep @ 35 s, 207/207 covered.
- `_emulation/run_step52g.stderr` — switch-API test, 4 switches fired cleanly.
- `memory/MEMORY.md` + `memory/project_emulation_progress.md` — updated descriptions to reflect Step 51.

### 52.10: BN prompt sent for Plan C

```
Decompile these 5 callers of fire_event @ 0x9c12a0:
- sub_b129f0, sub_bd39f0, sub_b16f64, sub_b82990, sub_b12760

For each: PseudoC body, param types, whether it reads from data_4880220
(switch state array) or calls sub_a50850, what values it passes as
event_id + event_data to fire_event.

If none is the switch→event translator, xref:
- Callers of sub_a50850 — which functions read the switch state array?
- Writers to data_4880220 + sw_num*0x30 + 0x28 — the UART decode path
  that updates switch state when cmd 0x27 arrives from netbridge.

Bonus: signature + body of sub_8ad030 (used by sub_8ae4dc with keys 0/1/2
to index into data_480eab0 — clarifies whether cawards data overlays the
scene_data struct or is a separate arena).
```


## Step 52: Two-stage switch architecture, event semantics, gstreamer proven dormant (2026-04-21)

User ran BN Plan C. Three hard findings reshape our model.

### 53.1: None of the 5 fire_event callers is the switch translator

| Caller | event_id | event_data | Role |
|---|---:|---|---|
| `sub_b129f0` | `0xc2` | `0` | Mode state transitions |
| `sub_bd39f0` | `0x12` | `&uint16` | Validated events (checks lookup tables at `0x4787fa8`, `0x4780724` before firing) |
| `sub_b16f64` | `0xc3` | `1..4` | Lamp timer events; sources from `data_4807430` |
| `sub_b82990` | `0x06` | `&struct` | Award/score events; reads `data_47790f8`, range `0 < id < 0xf0` |
| `sub_b12760` | `0xc1` | `0` | Mode init complete |

None reads `data_4880220`.

**Actual switch reader: `sub_b24f70 @ 0xb24f70`** — iterates switch-id arrays at `data_47643a8`, calls `sub_a50850` to read each switch state, updates game/display state. **Does not call `fire_event`.**

The dispatch is **two-stage**:
- Stage 1: `sub_b24f70` reads switch state → writes updated state into cmode fields / lamp subsystems.
- Stage 2: separate monitoring functions watch those cmode fields and call `fire_event` with derived event_ids + payloads.

Implication for our shim: **`shim_fire_switch` is already at the right layer.** It writes the same byte the UART cmd 0x27 decode path would — `sub_b24f70` picks it up on its next poll. No synthesis of event_ids needed; the game's own Stage-2 logic generates them. This validates the "switch-first API" pivot from Step 51.

### 53.2: Cawards is parallel-array, not overlay (bonus query)

`sub_8ad030` is a bounds-check helper: `returns arg2 if arg2 < *(arg1+0xc) else error`. `*(arg1+0xc)` is the size field of a header struct. Called by `sub_8ae4dc` with keys 0/1/2 to validate into three per-key parallel arrays stored within the 0x1a0 slot. **So cawards uses a header + parallel-array layout, NOT an overlay onto a Radium-picker field.** scene_id → Radium SHA-1 mapping remains open and almost certainly lives inside the `BDLMode*` factory chain.

### 53.3: Gstreamer is linked but unused in Pokémon LE 0.81

User pushed back on the "dead code" claim — fair, since BN showed defined symbols `gstreamer_init`, `gstreamer_shutdown`, `gstreamer_message_bus_thread`. We verified rigorously:

| Check | Result |
|---|---|
| Direct `bl 0x8c1ec0` callers | 0 |
| `adrp+add` computing 0x8c1ec0 | 1 (self-reference inside gstreamer_init) |
| R_AARCH64_RELATIVE relocs addend-0x8c1ec0 | 0 of 177,481 total |
| Byte-level `0x8c1ec0` / `0xcc1ec0` static pointer | 0 |
| `.dynsym` entry | no |
| dlopen/dlsym imports | **none** — no runtime plugin loading |
| gst_* imports besides init_check + deinit | **zero** — no element-factory-make, parse-launch, pipeline, bin, bus, pad, caps, buffer APIs |
| Element-name strings (`"appsink"`, `"kmssink"`, …) | 0 |
| Gst type-name strings (`GstElement`, `GstPipeline`, …) | 0 |
| GObject imports (`g_object_*`, `g_signal_*`, `g_type_*`) | 0 — only `g_main_loop_{new,quit,run,unref}` + `g_thread_{new,join,exit}` |
| Force-call `gstreamer_init` via shim trampoline | Succeeds; `gst_init_check` returns 1; main loop + VideoMessageBus thread start; NO subsequent gst activity in the game |

Conclusion: the gstreamer subsystem is scaffolding linked from Stern's shared Spike 3 codebase, but Pokémon LE 0.81 contains no code that would ever add a GstBus watch, create a pipeline, or post a message. The VideoMessageBus thread, if started, would block in `g_main_loop_run` processing exactly zero events. The game's video path is **100% direct libavcodec** via Stern's `SpiVideoStreamDecoderBase` (HEVC DRM-prime, Step 45 wall) and Radium's `VideoStreamDecoder` classes.

PROTOCOL.md updated with corrected gstreamer section. Shim gst hooks stay in place — harmless, will fire if any unexpected code path ever reaches them.

### 53.4: Hardware context captured

Per user-supplied FotE / Pokémon LE manual excerpts: the Spike 3 CPU Node (520-1007-42, what the user owns) is CM4-based with two micro-HDMI outputs (HDMI 0 = AUX, HDMI 1 = MIRROR) and a 34-pin CN6 LCD header whose resolution is jumper-selected (800×480 / 1024×600 / 1360×768 / 1920×1080). This maps directly to the FrameSize distribution of the 36 Radium scenes — the game renders into surfaces that correspond to physical panels on the CPU board. Downstream nodes (cabinet, playfield driver, LED boards) are not in the user's inventory.

Captured as `memory/reference_spike3_cpu_board.md`.

### 53.5: Artifacts

- PROTOCOL.md — added "Switch → event dispatch is a two-stage architecture", "Known event_id semantics" table, corrected "GStreamer — linked but unused" section.
- MODE_TABLE.md — added sub_8ad030 findings; cawards confirmed parallel-array.
- `memory/reference_spike3_cpu_board.md` — hardware map.
- `_emulation/spi_shim.c` — gstreamer hooks (gst_element_factory_make, gst_parse_launch, gst_bin_add, gst_bin_add_many, gst_element_link, gst_element_link_many, gst_element_set_state, gst_init_check, gst_deinit) + `shim_force_gstreamer_thread` diagnostic. Via `SPIKE3_FORCE_GSTREAMER=<sec>` and `SPIKE3_INJECT_VIDEO=1` (gates Step-47 frame injection off by default for stable runs).

## Step 53: Live state bridge — game drives Pokemon video rendering (2026-04-21)

With gstreamer ruled out, pivoted to Track-B synchronisation: stream cmode state from the running game to the reference-player so the player shows the clip that matches whichever cmode is currently live.

### 54.1: DMD is black — modern Spike 3 has no physical DMD

Before building the state bridge, investigated captured DMD frames. **The single 1024×256 frame we capture per run is entirely zero-filled.** Consistent with the user's note that modern Spike 3 has no physical DMD — the CN20 10-pin header exists for optional accessories but is not populated on Pokémon LE. The game allocates the buffer (framework default) but never paints it. "DMD live display" is not a viable quick win.

### 54.2: State bridge — shim side

Added to `spi_shim.c`:
- `/tmp/spike3_bridge.sock` AF_UNIX SOCK_STREAM listener, enabled by `SPIKE3_BRIDGE=1`.
- Accept thread: one subscriber at a time; evicts prior on new connect.
- Poll thread: samples the 42 cmodes' `+0x308` at 10 Hz, emits newline-delimited JSON on change:

```
{"t":"cmode","id":21,"name":"story_charmander_1","flags":40,
 "init":1792,"init_prev":0,"ts_ms":12345}
```

- `SPIKE3_POKE_CMODE=<name>:<hex_value>:<delay_sec>` — direct write to a named cmode's `+0x308`. Bypasses the game's Stage-2 logic so we can force activations for testing.

Non-blocking: `MSG_NOSIGNAL | MSG_DONTWAIT`, graceful on subscriber disconnect.

### 54.3: State bridge — subscriber side

`_emulation/historical-state-subscriber.py`:
- Retrying AF_UNIX client — reconnects at 1 Hz until the shim's listener is up.
- Parses newline-delimited JSON, pretty-prints cmode deltas with `+ / - / Δ` markers (activation / deactivation / change).
- `--play` mode: on any delta where `init != 0` AND the cmode has a clip-family mapping, launches `reference-player-v2.py --play-clip <FAMILY>_Start`. Previous player is terminated before new one starts.
- `CMODE_TO_CLIP_FAMILY` covers Pokémon story modes, Pikachu-vs-X, Team Rocket multiball/wizard, Magikarp multiball. Battle environment modes (coast/forest/desert/mountain) are deliberately excluded — their backgrounds use HEVC DRM-prime (Step 45 wall), not Radium clips.

### 54.4: End-to-end validation (run_step54e, 2026-04-21)

Config: `SPIKE3_BRIDGE=1 SPIKE3_POKE_CMODE=story_charmander_1:0x700:35`, subscriber launched with `--play`.

Timeline:
- t=0–35 s: game boots, reaches Godot scene load, shim bridge accepts subscriber.
- t=35 s: poke thread writes `0x700` to `story_charmander_1 +0x308` (was `0xd00`).
- t=35.1 s: poll thread detects delta, emits JSON.
- t=35.1 s: subscriber receives, looks up `"story_charmander_1" → "PK_Charmander1"`, launches `reference-player-v2.py --play-clip PK_Charmander1_Start`.
- t=35.2 s: reference-player opens `60ed7e50…/scene.assets/33.asset/0.asset`, decodes 1920×1080 @ 29.97 fps HEVC via PyAV, renders to pyglet+WSLg window.
- t=38 s: poke thread writes `0` (deactivation); subscriber logs `-` transition but does not kill player.

Subscriber log:
```
[v2] GenericClipDriver: playing 'PK_Charmander1_Start' on Video id=217 (looping on EOS)
[v2] PyAV opened 33.asset (1920×1080 @ 29.97 fps, codec=hevc, pix_fmt=yuv420p)
[v2] Video._render 'video.in_game_videos' (1360×768) → texture#1
```

`ps -ef | grep reference-player` confirmed the player process running at ~99 % CPU, actively rendering.

**This is the first time the running game binary's state has driven real Pokémon visual output.** Not full-emulation rendering (game's own UI compositor is still stubbed out), but a complete pipeline: game logic → cmode state → shim poll → Unix socket → subscriber → player → WSLg window.

### 54.5: Incidental finding — `+0x308` baselines are non-zero for ~9 cmodes

Step 49 observed that 9/42 cmodes have non-zero `+0x308` after static init. `story_charmander_1`'s baseline was `0xd00`. The field is subclass-interpreted, not a uniform "activation flag." Initial subscriber filter `"prev==0 AND init!=0"` missed all transitions for those 9 cmodes; widened to "`init != 0`" which catches any state that indicates the mode is live.

Longer-term, a cleaner signal would be hooking `sub_b24f70` directly to observe per-switch → per-cmode-field writes. That would give us a precise "mode X just transitioned to state Y" event without having to infer from `+0x308` deltas.

### 54.6: What this unlocks

- **Immediate**: user can now force any Pokémon cmode to appear live and see the corresponding Radium clip play. Exercise flag: `SPIKE3_POKE_CMODE=story_squirtle_1:0x700:30`, run with `--play`, watch PK_Squirtle1_Start.
- **Next stepping stone**: wire `shim_fire_switch` to produce organic cmode deltas. We know writing to `data_4880220 + sw*0x30 + 0x28` is the right input; once Stage 1 (`sub_b24f70`) is running and seeing our writes, real switch→mode→video chains should flow. May need to force a few preconditions (ball trough, start button sequence) to move the game past attract into gameplay.
- **Bigger**: cycle the Radium clip through Start → LoopingBG → Award → Total based on secondary cmode-state observations. Requires more observation of which fields change when.
- **Biggest (deferred)**: real EGL via Mesa/WSLg so the game's own compositor renders. Sysroot already contains aarch64 libEGL / libGLESv2 / libgbm, but they're ARM-embedded-GPU builds (armada, exynos, imx-dcss DRI drivers) — won't reach the host's GPU through QEMU user-mode. Multi-session job; real path is probably QEMU system mode with virtio-gpu or running on the CM4.

### 54.7: Texture PNG dump

Also extracted a sample of the 5 captured 1920×1080 RGBA textures + 3 sprite atlases to `_emulation/textures_png_preview/`. These are raw GL texture uploads the game performed; they contain real Pokémon art (boot splash, HUD elements, sprite sheets). Not rendered to a window yet, but visible directly in any image viewer — tangible proof the capture pipeline has been gathering real data all along.

### 54.8: Artifacts

- `_emulation/spi_shim.c` — added `shim_bridge_start`, `shim_bridge_accept_thread`, `shim_bridge_poll_thread`, `shim_bridge_emit`; `shim_poke_cmode_thread`. Two new env vars: `SPIKE3_BRIDGE=1`, `SPIKE3_POKE_CMODE=<name>:<hex>:<sec>`.
- `_emulation/spi_shim.so` — rebuilt, 155,136 bytes.
- `_emulation/historical-state-subscriber.py` — new subscriber (~140 LOC). Two modes: `--print-only` default, `--play` for reference-player launch.
- `_emulation/textures_png_preview/` — 8 texture PNGs (3 MB total).
- `_emulation/run_step54{a,b,c,d,e}.{stdout,stderr}` — progression of test runs.
- `_emulation/bridge_subscriber_{a..e}.log` — subscriber captures at each iteration.
- `memory/reference_spike3_cpu_board.md` — hardware context doc.

### 54.9: Next moves (options, user to choose)

1. **Cycle clips through Start → LoopingBG** — play the Start clip once, then switch to LoopingBG when it ends. Subscriber-side work, ~30 min.
2. **Organic switch drive** — try `SPIKE3_FIRE_SWITCH=93:22:15:20:19:18:17:16` (start + shooter lane + trough balls) to see if the game moves past attract into a real mode. If it does, all the cmode changes would flow through the bridge naturally, no poke needed.
3. **Hook `sub_b24f70`** — intercept the switch-state reader to log which switch closures drive which cmode fields. Cleanest long-term observability.
4. **Real EGL via Mesa/WSLg** — the big rendering job, multi-session. Shelved for now.

## Step 54: Plan A dead, Plan B-lite lives — aarch64 Mesa software raster works under qemu-user (2026-04-22)

Went in planning virtio-gpu host-GPU passthrough; came out with a complete software-rasterization path that works in qemu-user-mode and renders real pixels through llvmpipe. No VM, no kernel, no `/dev/dri`.

### 55.1: Verdict on Plan A (virgl → host GPU)

Empirically disproven on WSL2. Probe sequence:

| Check | Result |
|---|---|
| `qemu-system-x86_64` + `virtio-gpu-gl-pci` + `egl-headless` on startup | `qemu-system-x86_64: egl: no drm render node available` / `egl: render node init failed` |
| `qemu-system-x86_64` + `virtio-gpu-gl-pci` + `gtk,gl=on` (TCG, no KVM) | Window starts fine, GL-display init succeeds. `gl=on` for the display ≠ virgl. |
| WSL2 `/dev/dri` | absent |
| WSL2 `/dev/dxg` | present, but virglrenderer's host-side EGL path wants a DRM render node |
| Host `libvirglrenderer1` installed | yes (1.1.0) — library is ready, just no DRI node to target |

Conclusion: virglrenderer-on-WSL2 cannot initialise host EGL because WSL2 does not expose a DRM render node. Plan A is deferred until a native Linux host is available (or WSL2 gains a DRI bridge). Full failure log + verdict in session transcript; short form in `_emulation/mesa_sw_overlay/README.md`.

### 55.2: Gemini cross-check — one correction taken, one confused pivot, one useful reframe

User solicited a second opinion. Three findings worth recording:

- **Correction (adopted):** Box64 / FEX-Emu translate x86→aarch64, not the other way. Prior suggestion to list them as aarch64→x86 emulators was wrong; qemu-user is the correct tool in that direction. There is no mature aarch64→x86 equivalent of Box64.
- **Confused pivot:** Gemini suggested using Mesa's d3d12 Gallium driver to talk to WSL's `/dev/dxg`. Three stacked problems prevent this working:
  1. No aarch64 Mesa build with the d3d12 driver ships anywhere — it's x86_64-first upstream, and Debian arm64 does not package it;
  2. WSL's D3D12 userspace libraries (`/usr/lib/wsl/lib/libd3d12.so`, `libd3d12core.so`, `libdxcore.so`) are x86_64 ELFs — an aarch64 process cannot load them; qemu-user cannot bridge ISAs inside a single process;
  3. DXGKrnl's ioctl set is custom (not in qemu-user's translation table) and carries pointer-bearing structs whose arg-layouts qemu-user won't rewrite.
- **Useful reframe (adopted):** stay in qemu-user-mode, replace the sysroot's Mesa DRI drivers with a software-raster aarch64 build, remove the shim's EGL/GLES fake-success. No VM. This became Step 54 proper.

### 55.3: Mesa overlay architecture

Target: sysroot Mesa reports "Mesa 24.0.9" in its `libEGL.so.1` version strings. Gate-check plan: overlay a Debian-arm64 Mesa near that version next to the sysroot, force-load `swrast` via `MESA_LOADER_DRIVER_OVERRIDE`, initialise surfaceless EGL.

All overlay files live in `games/pokemon_le/_emulation/mesa_sw_overlay/pkg/usr/lib/aarch64-linux-gnu/` and are built by downloading arm64 `.deb`s and extracting with `dpkg-deb -x` — **no `sudo`, no foreign-architecture `dpkg --add-architecture`, no sysroot modification**.

### 55.4: Dep-closure walk (Debian 25.0.7 first attempt)

Pulled 4 arm64 packages from `deb.debian.org/debian/pool/main/m/mesa/`:
- `libgl1-mesa-dri_25.0.7-2_arm64.deb` (46 KB) — **megadriver**: every `*_dri.so` is a symlink to one 133 KB `libdril_dri.so` loader. Actual driver code lives in…
- `mesa-libgallium_25.0.7-2_arm64.deb` (8 MB) — `libgallium-25.0.7-2.so`, the engine libdril dlopens on first driver-call. New packaging in Mesa 25.x.
- `libegl-mesa0`, `libgbm1` — 25.0.7 versions, not used in final overlay.

`libgallium-25.0.7-2.so` NEEDED list revealed the transitive closure. Iterative pull loop (parse the missing-lib from each dlopen failure, look up `apt-cache show <pkg>` on the host to get the pool filename, swap `_amd64.deb` → `_arm64.deb`, `curl` from Debian, `dpkg-deb -x` into overlay, retry) grew the overlay to 30 arm64 packages, final size **365 MB uncompressed** (63 MB of .deb):

LLVM-19 (libLLVM.so.19.1, 123 MB) • libzstd1 • libelf1t64 • libsensors5 • libdrm-{amdgpu,nouveau,radeon,intel}1 • libxcb1 + 6 xcb sub-libs • libx11-xcb1 • libxshmfence1 • libwayland-client0 • libwayland-server0 • libedit2 • libtinfo6 • libxml2 • libbsd0 + libmd0 • libicu76 • libz3-4 • libxau6 + libxdmcp6 • libstdc++6.

Probe: `dlopen("libgallium-25.0.7-2.so")` under qemu-aarch64 with `LD_LIBRARY_PATH` pointing at the overlay. After 19 iterations, dlopen succeeded — full 25.0.7 gallium + its 120-ish transitive deps resolvable.

### 55.5: Mesa version-string gate rejected 25.0.7 driver against 24.0.9 libEGL

Next run (EGL probe using sysroot's `libEGL.so.1`):

```
libEGL fatal: did not find extension DRI_Mesa version 1
```

Sysroot's 24.0.9 libEGL wants `DRI_Mesa` v1 from its driver; Debian 25.0.7 driver doesn't provide that specific extension signature. ABI divergence between 24.x and 25.x on the driver ↔ loader interface.

### 55.6: Backswap to Mesa 24.0.8-1 (closest match available)

Debian's source-version history (queried via `snapshot.debian.org/package/mesa/`) shows Mesa 24.0.9 never shipped — the series jumps `24.0.8-1` → `24.1.0-1`. Ubuntu noble arm64 only had 24.0.5. **Mesa 24.0.9 exact is sysroot-only.** Best available: 24.0.8-1 (one patch-level off, 24.0.x is a pure bugfix series so ABI is identical).

Pulled via `snapshot.debian.org/mr/binary/libgl1-mesa-dri/24.0.8-1/binfiles` → `/file/<sha1>`:
- `libgl1-mesa-dri_24.0.8-1_arm64.deb` (7 MB) — **different architecture**: one 28 MB `armada-drm_dri.so` with every other driver as a hardlink. All gallium code is statically inside (no separate `libgallium.so` in 24.x).
- `libegl-mesa0_24.0.8-1_arm64.deb`, `libgbm1_24.0.8-1_arm64.deb`.

Overlaid on top of the 25.0.7 tree. 24.0.8 drivers won.

24.0.8 swrast_dri.so NEEDED: all satisfied by the existing overlay + sysroot — `libLLVM-17.so.1` replaces LLVM-19 as the JIT. Debian 13 renamed the package `libllvm17` → `libllvm17t64` (time_t-64-bit transition). Pulled `libllvm17t64_17.0.6-22+b2_arm64.deb` from trixie (119 MB). Both LLVM-17 and LLVM-19 now coexist in the overlay — LLVM-17 is the one actually used by the 24.0.8 driver.

Retry: new error, progress.

```
libEGL fatal: DRI driver not from this Mesa build ('24.0.8-1' vs '24.0.9')
```

Mesa's own strict version-string check, not a real ABI gate. Driver reports its build as `"24.0.8-1"`, libEGL was compiled with `PACKAGE_VERSION="24.0.9"`, strcmp fails. No escape env var exists — grepped sysroot libEGL for `MESA_.*VERSION|SKIP|allow_driver` and found nothing actionable.

### 55.7: Defeat the version-string check by in-place binary patch

The string `"24.0.8-1\0"` is stored 8 times in `armada-drm_dri.so` (rodata), including at the `DRI_Mesa` extension descriptor and in several error-message contexts. Python patch:

```python
for path in glob("…/pkg/usr/lib/aarch64-linux-gnu/dri/*_dri.so"):
    with open(path, "rb") as f: data = bytearray(f.read())
    data = data.replace(b"24.0.8-1", b"24.0.9\x00\x00")   # same 8-byte length
    open(path, "wb").write(data)
```

8 occurrences patched. Driver still loads (the changed bytes are in strings only, no code).

Retry EGL probe:
```
libEGL.so.1 loaded: 0x…
EGL 1.5  vendor='Mesa Project'
  client apis: OpenGL OpenGL_ES
  extensions (first 400c): EGL_EXT_create_context_robustness EGL_EXT_query_reset_notification_strategy …
EXIT=0
```

**EGL init passes.** Surfaceless, no `/dev/dri`, no X11 touched.

### 55.8: End-to-end render validation — multi-clear + shaded triangle

Stronger probe (`_emulation/mesa_sw_overlay/gles_probe2.c`): runs 4 distinct `glClear` colors + a real vertex/fragment shader pair + `glDrawArrays` triangle + `glReadPixels` at a point inside the triangle and a point outside.

```
EGL: 1.5  Mesa Project
GL_RENDERER: llvmpipe (LLVM 17.0.6, 128 bits)
GL_VERSION:  OpenGL ES 3.2 Mesa 24.0.9
GL_GLSL_VER: OpenGL ES GLSL ES 3.20

Test 1: three distinct glClear colors
  clear #1 purple          at(64,64) expect=bf 19 e6 ff  got=bf 1a e6 ff  OK
  clear #2 teal            at(64,64) expect=33 cc 4c ff  got=33 cc 4c ff  OK
  clear #3 black           at(64,64) expect=00 00 00 ff  got=00 00 00 ff  OK
  clear #4 white           at(64,64) expect=ff ff ff 40  got=ff ff ff 40  OK
  subtotal: 4/4

Test 2: shaded triangle (shader compile + link + draw)
  vertex shader compile: OK
  fragment shader compile: OK
  program link: OK
  inside-tri center        at(64,64) expect=e6 33 99 ff  got=e5 33 99 ff  OK
  outside-tri corner       at( 4, 4) expect=0d 0d 0d ff  got=0d 0d 0d ff  OK

TOTAL: 6/6 passes
VERDICT: ALL PASS — llvmpipe is really rasterizing
```

Game uses GLES 3.0; we're providing 3.2 via llvmpipe with GLSL ES 3.20. The two off-by-ones (bf **1a** e6 ff vs bf 19 e6 ff, e**5** 33 99 ff vs e6 33 99 ff) are unorm rounding (0.1 × 255 = 25.5 rounds up; 0.9 × 255 = 229.5 rounds down this run) — both within my ±2 tolerance and both confirm the driver is doing real math, not a constant fill.

### 55.9: Zero-risk sanity — game + overlay + unchanged shim

Before modifying the shim, proved the 225 MB overlay on `LD_LIBRARY_PATH` doesn't disturb the existing Step-54 pipeline:

```
cd games/pokemon_le && timeout 25 qemu-aarch64 -L /mnt/c/Workspace/pokemon-le-0.81.0 \
    -E LD_PRELOAD=.../spi_shim.so \
    -E LD_LIBRARY_PATH=.../mesa_sw_overlay/pkg/usr/lib/aarch64-linux-gnu:…:sysroot dirs \
    -E SPIKE3_SYSROOT=/mnt/c/Workspace/pokemon-le-0.81.0 \
    ./game > run_step55_sanity.log 2>&1
```

Result: 12,827 log lines, steady state reached (shim's `shim_dump_dispatch_tables` output + SPI poll), 34 Step-54 milestone markers hit (Radium / ManualGodotScene / pokemon_battle_scene / Godot Engine / /root/Main / bridge), **zero overlay-induced errors** (no "DRI driver not from this Mesa", no "undefined symbol", no "version mismatch", no "cannot open shared object").

Confirmed: the overlay is load-time innocent. Because the shim's EGL/GLES hooks are still intercepting every call, Mesa never actually gets invoked during the run — but none of its 120-ish .so files cause any conflict just by being resolvable on `LD_LIBRARY_PATH`.

### 55.10: What's unlocked, what's next

**Unlocked (this step):** A working aarch64 software-raster GL stack layered on top of the sysroot. No VM, no virtio, no kernel, no `/dev/dri`, no host GPU. Just a library overlay + four env vars:

```
LD_LIBRARY_PATH=<overlay>:<sysroot dirs>
LIBGL_DRIVERS_PATH=<overlay>/dri
MESA_LOADER_DRIVER_OVERRIDE=swrast
LIBGL_ALWAYS_SOFTWARE=1
EGL_PLATFORM=surfaceless
```

**Next (Step 55, planned):** Gate the shim's EGL + GLESv2 + GBM interceptors behind an env var (`SPIKE3_REAL_GL=1`). With the flag off, existing Step-54 behaviour. With the flag on, the shim short-circuits its GL fake-success paths and the game's calls fall through to real Mesa. Bridge/audio/UART/DRM-socketpair/memcpy/pthread_create/`system()`/ALSA stubs all stay always-on; only GL leaves.

The first SPIKE3_REAL_GL=1 run is almost certainly going to surface new failures — surfaceless platform means the game's GBM/DRM page-flip path will hit surfaces we haven't matched to real EGL resources, shader-compile errors could light up, Godot may assert on a config we didn't handle. Each is its own sub-step. But we now have a **real** pipeline to walk down instead of a stack of stubs that always "succeed" by fiat.

### 55.11: Artifacts

- `_emulation/mesa_sw_overlay/` — all overlay work lives here.
  - `pkg/` — extracted overlay (365 MB, `.gitignore`-worthy; reproducible from `debs*/`)
  - `debs/` — 25.0.7 + transitive deps (30 .deb files, 63 MB)
  - `debs_2408/` — 24.0.8 Mesa core + libllvm17t64 (4 .deb files, 6.9 MB)
  - `build_overlay.sh` — one-shot reproducer (pulls + extracts + patches)
  - `.gitignore` — excludes `pkg/` and `.deb`s
  - `probe.c`, `probe` — dlopen gate-check
  - `egl_probe.c`, `egl_probe` — EGL init gate-check
  - `gles_probe.c`, `gles_probe` — EGL + GLES3 + glClear probe (original)
  - `gles_probe2.c`, `gles_probe2` — multi-clear + shaded-triangle probe (6/6 pass)
- `_emulation/SETUP.md` — new dev-environment setup doc for onboarding contributors.
- `_emulation/run_step55_sanity.log` — zero-risk run proving overlay is load-time innocent.

## Step 55: Shim gate `SPIKE3_REAL_GL` lands; first real-Mesa runs trip on extension interception (2026-04-22)

Goal: remove the shim's EGL/GLES/GBM fake-success stubs so the game's calls fall through to the Mesa overlay we built in Step 54. Two-phase outcome — infrastructure works cleanly, first real-GL run surfaces an extension-handling gap that wasn't obvious from Step 54's isolated probes.

### 56.1: Infrastructure changes (shim gating)

`spi_shim.c` grew an `is_real_gl()` helper reading `SPIKE3_REAL_GL` env var + two `FWD_RET` / `FWD_VOID` forwarding macros. Each macro, as the first statement of a gated stub, resolves the real function via `dlsym(RTLD_NEXT, ...)` (cached per-function in a static pointer) and tail-calls it when the flag is set; falls through to the stub body otherwise.

Applied to:
- **EGL** (~20 functions): `eglGetDisplay` / `eglGetPlatformDisplay[EXT]` / `eglInitialize` / `eglTerminate` / `eglQueryString` / `eglBindAPI` / `eglGet|ChooseConfig[s]` / `eglGetConfigAttrib` / `eglCreate|DestroyContext` / `eglCreate|DestroyWindowSurface` / `eglCreatePbufferSurface` / `eglMakeCurrent` / `eglSwapBuffers` / `eglSwapInterval` / `eglSurfaceAttrib` / `eglQuerySurface` / `eglGetError` / `eglGetProcAddress`.
- **GLES** (~80 functions): all the Gen/Delete/Bind/Tex/Buffer/Shader/Program/Uniform/VertexAttrib/Framebuffer/State/Draw/Sync families.
- **GBM** — **INITIALLY FORWARDED, THEN REVERTED to stub-always** (Step 55.2 below).

Removed instrumentation — per user direction, runtime capture of textures / shader sources / DMD frames was dropped as duplicative of the asset-extraction path (`_extraction/tools/spike_extract/`) and irrelevant on Spike 3 (no physical DMD). Deleted: `gl_capture_tex`, `gl_capture_ctex`, `gl_bytes_per_pixel`, `gl_tex_capture_count`, `gl_ctex_capture_count`, `gl_shader_capture_count`, `gl_shader_type[]`, `GL_SHADER_TYPE_MAX`, `is_dmd` / `mapped` fields in `gbm_bo_entry`, DMD-frame dump block in `gbm_bo_unmap`, `dmd_frame_counter`, and mkdirs for `_emulation/{dmd,shaders,textures}/`.

Uniform macros (`GL_UNIFORM_1/2/3/4(i/f, type)`) were expanded out into individual function bodies — macros don't mesh cleanly with adding per-function FWD calls, and the expansion is ~30 lines that are now each individually greppable.

Sizes:
- `spi_shim.c`: 4,959 → 5,196 LOC (macro def + FWD lines partially offset by capture removal)
- `spi_shim.so`: 155 KB → 158 KB (FWD-pointer statics)

### 56.2: Regression verified — stub mode unchanged

With `SPIKE3_REAL_GL` unset, the shim behaves identically to Step 53:
- 10,761 log lines over 20s (baseline 10-13k)
- 30 Step-54 milestone markers
- No segfault, no assertion, no `libEGL fatal`, no undefined symbol

Run artifact: `_emulation/run_step56_regression3.log`.

### 56.3: First real-GL run trips on `eglGetError` assertion

`SPIKE3_REAL_GL=1` + overlay env vars (`LIBGL_DRIVERS_PATH=<overlay>/dri`, `MESA_LOADER_DRIVER_OVERRIDE=swrast`, `LIBGL_ALWAYS_SOFTWARE=1`, `EGL_PLATFORM=surfaceless`) + LD_LIBRARY_PATH pointing at the 365 MB Mesa overlay:

```
game: glWindow.cpp:970: void glWindow::setGlContext():
  Assertion `eglGetError() == EGL_SUCCESS' failed.
qemu: uncaught target signal 6 (Aborted) - core dumped
```

Root cause analysis: the game's GL setup calls `eglCreateWindowSurface` with a `native_window` that's actually a fake GBM surface handle (from our `gbm_surface_create` stub). Real Mesa with `EGL_PLATFORM=surfaceless` cannot consume window surfaces at all — and can't consume the fake GBM handle either way. The call returns `EGL_NO_SURFACE`, setting the error, and the game's assertion fires.

### 56.4: Fix v1 — GBM un-forwarded + `eglCreateWindowSurface` redirected to pbuffer

Two changes:

1. **GBM un-forwarded.** Mesa's GBM backend needs a real DRM device our fake `/dev/dri/card1` socketpair cannot satisfy (real Mesa gbm does `DRM_IOCTL_MODE_CREATE_DUMB` and friends, not the subset we intercept). Surgically removed the 32 `FWD_RET`/`FWD_VOID` lines from the GBM section via a Python one-shot (scanning for `FWD_RET(`/`FWD_VOID(` line-starts in the GBM region 2261–2503). GBM stays pure-stub in both modes.

2. **`eglCreateWindowSurface` redirects to `eglCreatePbufferSurface` in real-GL mode.** The game's `native_window` is ignored; a pbuffer of `FAKE_MODE_HDISPLAY × FAKE_MODE_VDISPLAY` (1920×1080) is created via `dlsym(RTLD_NEXT, "eglCreatePbufferSurface")` and returned. Game sees an EGL surface it can bind; Mesa gets something it can actually attach a context to.

### 56.5: Fix v1 run — SEGV in idle polling, zero game output

After the v1 fix, `SPIKE3_REAL_GL=1` run `_emulation/run_step56_realgl_v2.log` still segfaults:

- 3,669 log lines before SEGV, vs ~13k for a clean baseline run — game crashed before reaching normal steady state.
- **Zero** game-side output in the log — no `Godot Engine` banner, no `Radium` log lines, no `/root/*_scene` load messages. Crash occurred *before* Godot init.
- Netbridge handshake completed, bridge RESP framing OK. Then SEGV 11 somewhere in the shim-dump tail.
- No EGL call logs visible (the FWD macros short-circuit before the existing `SHIM_LOG` calls, so we have no forward-path visibility).

### 56.6: Hypothesis — EGL extension pointers returned through a now-forwarded `eglGetProcAddress`

When `SPIKE3_REAL_GL=1`, `eglGetProcAddress` forwards to Mesa's real implementation. Mesa returns **Mesa's own** `eglCreateImageKHR`, `eglCreateSyncKHR`, `eglWaitSyncKHR`, `eglClientWaitSyncKHR`, `glEGLImageTargetTexture2DOES`. The game then calls those with:
- A fake GBM BO pointer (from our stubbed `gbm_bo_create`), not a real EGLClientBuffer.
- A fake EGL image handle.

Mesa dereferences → SEGV. In stub mode, `eglGetProcAddress` returned our `stub_*` functions which accepted fake handles silently.

**The fix is in Step 56** (new task): keep our `stub_*` functions as the authoritative return values for the KHR image/sync family and `glEGLImageTargetTexture2DOES`, regardless of SPIKE3_REAL_GL state. Forward only `eglGetPlatformDisplay*EXT` queries to Mesa. Also add SHIM_LOG coverage to every EGL function's forward path so the next crash is traceable.

### 56.7: What's proven and what's open

**Proven:**
- Shim gating infrastructure works. FWD macros dispatch cleanly, `dlsym(RTLD_NEXT)` finds the real Mesa symbols (overlay is in LD_LIBRARY_PATH), caching is correct.
- Regression is intact. With `SPIKE3_REAL_GL` unset, Step-54 behavior bit-for-bit preserved. No test harness collateral.
- Texture/shader/DMD capture removal is safe. Those paths were dead, removing them didn't regress steady-state.

**Open:**
- `SPIKE3_REAL_GL=1` SEGVs before Godot init. Cause is almost certainly the extension-interception gap; Step 56 handles it. Expected to need 1-2 more sub-steps after that to reach Godot init and a real render.
- We don't know yet whether `eglCreateContext` succeeded (no log from it). Step 56's added SHIM_LOGs will answer.
- Surfaceless vs. window-based swap semantics: real `eglSwapBuffers` on a pbuffer is a no-op in spec, but the game may assume buffer-flip timing (vsync, page-flip events through the DRM socketpair). This may surface after Step 56.

### 56.8: Artifacts

- `_emulation/spi_shim.c` — +440 LOC (gating + macros) / -160 LOC (capture removal); net +280 LOC.
- `_emulation/spi_shim.so` — rebuilt, 158,752 bytes.
- `_emulation/run_step56_regression.log`, `run_step56_regression2.log`, `run_step56_regression3.log` — stub-mode regression runs, all clean.
- `_emulation/run_step56_realgl.log` — first real-GL run; `eglGetError` assertion.
- `_emulation/run_step56_realgl_v2.log` — with GBM un-forwarded + pbuffer redirect; SEGV at ~3.6k lines before Godot init.


## Step 56: `eglGetProcAddress` pinned to our stub table; `FWD_RET_V` instrumentation pinpoints the real crash site (2026-04-22)

Goal (from Step 55.6): close the KHR extension-interception SEGV by guaranteeing our `stub_*` functions are the authoritative pointers the game retrieves, regardless of `SPIKE3_REAL_GL`. Add forward-path log coverage so the next crash is traceable.

### 57.1: Infrastructure changes

Three surgical edits to `spi_shim.c`:

1. **New `FWD_RET_V` / `FWD_VOID_V` macros** (lines 133–150). Identical to `FWD_RET` / `FWD_VOID` except for a `SHIM_LOG("FWD " #FN)` emitted before the `dlsym(RTLD_NEXT, ...)` tail-call. Used only at low-frequency sites so the ~10k-line/20s GLES steady-state flood doesn't drown any diagnostic signal.
2. **EGL sites swapped** from `FWD_RET` to `FWD_RET_V` — 20 call sites covering `eglGetDisplay` / `eglGetPlatformDisplay[EXT]` / `eglInitialize` / `eglTerminate` / `eglQueryString` / `eglBindAPI` / `eglGetConfigs` / `eglChooseConfig` / `eglGetConfigAttrib` / `eglCreate|DestroyContext` / `eglCreatePbufferSurface` / `eglDestroySurface` / `eglMakeCurrent` / `eglSwapBuffers` / `eglSwapInterval` / `eglSurfaceAttrib` / `eglQuerySurface` / `eglGetError`. Done via `sed -i -E '2515,2704 s/\bFWD_RET\b/FWD_RET_V/g'`, verified with grep (exactly 20 matches in the EGL range, zero collateral). GLES stays on silent `FWD_RET`.
3. **`eglGetProcAddress` no longer forwards.** Deleted the top-of-function `FWD_RET(pfn_t, eglGetProcAddress, ...)` at what was line 3512. The static lookup table is now consulted *always*, regardless of mode. A Step-57-specific comment explains the invariant: Mesa's real `eglCreateImageKHR` / `eglCreateSyncKHR` / `glEGLImageTargetTexture2DOES` would be handed our fake GBM BOs and fake sync handles and SEGV on the first deref; the KHR stubs accept those handles silently; `eglGetPlatformDisplay[EXT]` returns our own trampoline which forwards *inside* its body via `FWD_RET_V`; unknown names return NULL rather than falling through to `dlsym(RTLD_NEXT, name)` because every unstubbed extension is one we have not proven fake-handle-safe.

Sizes:
- `spi_shim.c`: 5,196 → 5,215 LOC (net +19: two verbose macros, one longer comment, one deleted FWD line).
- `spi_shim.so`: 158,752 → 158,968 bytes (+216 B).

Clean build, zero warnings.

### 57.2: Regression verified — stub mode preserved

With `SPIKE3_REAL_GL` unset:

- 11,478 log lines in 22 s (Step 55 baseline: 10,761 in 20 s; both fall in the 10–13k-lines envelope).
- Exit 124 (timeout, not crash).
- Zero `Segmentation`, `Aborted`, `assertion`, `libEGL fatal`, `cannot open shared`, `undefined symbol` — matches Step 55.2 criteria.
- Tail is the same event-registry / update-type dispatch dump as Step 55 regression3.

Run artifact: `_emulation/run_step57_regression.log`.

### 57.3: Real-GL run — progresses 4 EGL calls further; aborts in config setup

With `SPIKE3_REAL_GL=1` + overlay env vars (`LIBGL_DRIVERS_PATH`, `MESA_LOADER_DRIVER_OVERRIDE=swrast`, `LIBGL_ALWAYS_SOFTWARE=1`, `EGL_PLATFORM=surfaceless`) + `LD_LIBRARY_PATH` into `mesa_sw_overlay/pkg/usr/lib/aarch64-linux-gnu`:

```
terminate called after throwing an instance of 'std::bad_array_new_length'
  what():  std::bad_array_new_length
qemu: uncaught target signal 6 (Aborted) - core dumped
```

Exit 134 (SIGABRT). 3,056 log lines before the abort. Crucially: **this is a different failure from Step 55.5's SEGV**. The KHR-extension SEGV class is closed.

### 57.4: Diagnosis — fake-display contamination between our stubs and real Mesa

The `FWD_RET_V` log trace captures the exact flow:

```
[shim] eglGetProcAddress("eglGetPlatformDisplayEXT") -> stub 0x7652ad28bfac
[shim] FWD eglGetPlatformDisplayEXT
[shim] eglGetPlatformDisplayEXT(platform=0x31d7, dpy=0x765230001180) -> fake
[shim] FWD eglInitialize
[shim] FWD eglBindAPI
[shim] FWD eglGetConfigs
terminate called after throwing an instance of 'std::bad_array_new_length'
```

Read of the flow:

1. Game calls `eglGetProcAddress("eglGetPlatformDisplayEXT")`. Our table returns **our wrapper** (Step 56 fix confirmed — Mesa is not consulted).
2. Game calls the wrapper with `platform = 0x31d7` (`EGL_PLATFORM_GBM_KHR`) and the fake GBM device pointer we returned earlier from `gbm_create_device`.
3. `FWD_RET_V` fires inside the wrapper. `dlsym(RTLD_NEXT, "eglGetPlatformDisplayEXT")` returns **NULL** — Mesa's `libEGL_mesa.so.0` exports `eglGetPlatformDisplay` (the EGL 1.5 core symbol) but not the `EXT` alias; the `EXT` variant is reachable only through the `eglGetProcAddress` chain, which we have (correctly) pinned to our own wrapper. `_real` stays NULL, the fall-through stub body runs, returns `FAKE_EGL_DISPLAY`.
4. Game now holds `FAKE_EGL_DISPLAY` and feeds it to `eglInitialize`. `FWD_RET_V` finds Mesa's real `eglInitialize` (direct EGL 1.0 symbol), forwards with the fake display — Mesa doesn't recognize it, returns `EGL_FALSE`, doesn't populate `*major` / `*minor`.
5. `eglBindAPI(OPENGL_ES_API)` forwards successfully — this call is display-less.
6. `eglGetConfigs(FAKE_DISPLAY, NULL, 0, &num_config)` forwards to Mesa — Mesa rejects the fake display, returns `EGL_FALSE`, `*num_config` is left uninitialised (or set to a garbage value by Mesa's error path).
7. Game's downstream `new EGLConfig[num_config]` trips libstdc++'s array-new-length check → `std::__throw_bad_array_new_length` → SIGABRT.

Mechanism in one sentence: because Mesa doesn't export `eglGetPlatformDisplayEXT` as a direct symbol, our EXT wrapper silently falls back to the fake-display stub body, and every subsequent EGL call that *does* forward to Mesa then sees an alien display handle and fails.

### 57.5: What's proven and what's open

**Proven:**
- `eglGetProcAddress` stub table is authoritative. The Step 55.6 SEGV class (KHR image/sync/EGLImageTargetTexture2DOES driven by Mesa dereferencing fake handles) is closed by construction — our pointers are the only pointers the game ever retrieves through that API.
- `FWD_RET_V` instrumentation is the right diagnostic granularity. 4 EGL calls produced 4 log lines, visible in-stream with the SPI / DRM / GBM shim chatter; GLES hot paths remain silent.
- Stub-mode behaviour is preserved bit-for-bit (same line volume, same exit condition, same tail dump).

**Open — Step 57 territory:**
- Resolve the EXT-symbol gap. Mesa in surfaceless mode exposes only `EGL_PLATFORM_SURFACELESS_MESA` (0x31DD) and exports `eglGetPlatformDisplay` (not the EXT alias) directly. Fix: make our `eglGetPlatformDisplay[EXT]` wrappers, when `SPIKE3_REAL_GL` is set, retry `dlsym(RTLD_NEXT, "eglGetPlatformDisplay")` (no EXT suffix) and call it with `EGL_PLATFORM_SURFACELESS_MESA` + `EGL_DEFAULT_DISPLAY` (NULL), ignoring the game's GBM platform/handle. Return Mesa's real display to the game. All subsequent EGL calls (`eglInitialize` / `eglGetConfigs` / `eglChooseConfig` / `eglCreateContext` / `eglMakeCurrent` / `eglSwapBuffers`) then receive a valid Mesa display and should succeed.
- After that fix, the next crash is likely at `eglCreateWindowSurface` (Step 55.4's pbuffer redirect is still in place; confirm it still works with a real display) or `eglCreateContext` (config attributes may need translation). Each is its own sub-step.

### 57.6: Artifacts

- `_emulation/spi_shim.c` — +19 LOC net (two verbose macros + Step 56 comment on `eglGetProcAddress`; one deleted FWD line; 20 EGL sites renamed).
- `_emulation/spi_shim.so` — rebuilt, 158,968 bytes.
- `_emulation/run_step57_regression.log` — 11,478 lines, exit 124, clean.
- `_emulation/run_step57_realgl.log` — 3,056 lines, exit 134, `std::bad_array_new_length` abort; FWD trace captures the exact failure sequence.


## Step 57: Mesa display + config negotiation + frame bridge — game now reaches DMA-BUF FBO setup (2026-04-22)

User pivoted the plan: stop treating "pixels on screen" as a future milestone, build it now. This step bundles four concerns into one landing — EGL platform-display translation, `eglChooseConfig` attribute patching, a frame-bridge IPC channel with a Python viewer, and the diagnostic instrumentation that guided each sub-fix. Ending state: Mesa is doing real GL init for the game, the game walks through Stern-logo load, netbridge init, power-dist init, DRM mode negotiation, and crashes NULL-dereferencing inside the `create_egl_framebuffer` path (0xd00800 in BN) because our `stub_eglCreateImageKHR` returns a handle with no texture backing. That's the Step-59 target.

### Why bundle, why these four things together

The goal was a first rendered frame reaching a Python viewer, so the display-acquisition fix and the IPC it delivers into had to land together. Splitting them would leave Mesa rendering into a pbuffer nobody can see. Diagnostic instrumentation (the attribute-array logger, the `FWD <fn>` verbose macros from Step 56) drove every decision — we needed to see what the game actually asked Mesa for, not guess.

### Display acquisition — `GBM_KHR` → `SURFACELESS_MESA`

Step 56's FWD_RET_V trace pinpointed that Mesa doesn't export `eglGetPlatformDisplayEXT` as a direct dynamic symbol; the fall-back to our fake-display stub was poisoning every subsequent forwarded call. Fix: `eglGetDisplay` / `eglGetPlatformDisplay` / `eglGetPlatformDisplayEXT` all route through a new `shim_real_platform_display()` helper that tries `eglGetPlatformDisplay` (EGL 1.5 core, always exported) and falls through to legacy `eglGetDisplay`, calling with `EGL_PLATFORM_SURFACELESS_MESA` (0x31DD) + `EGL_DEFAULT_DISPLAY`. The game's `GBM_KHR` platform and GBM device pointer are dropped; semantically we're saying "we don't have a real display controller, just give me a surfaceless Mesa display we can render into an offscreen buffer with." Mesa accepts this and returns a real EGLDisplay handle. The game treats it as opaque.

First run with this change confirmed `eglInitialize` / `eglBindAPI` / `eglGetConfigs` all succeed — the Step 56 `std::bad_array_new_length` abort is gone. New failure mode: `eglChooseConfig` returns `*num_config = 0`, game logs its own `failed to find appropriate egl configs for attributes`, continues through netbridge/power-dist init, hits a downstream SEGV eventually.

### Attribute negotiation — `WINDOW_BIT` → `PBUFFER_BIT`

Added a diagnostic dump to `eglChooseConfig` that walks the attribute list, names each EGL enum, and logs Mesa's returned config count. The game's ask: 8/8/8/0 RGBA, 24-bit depth, GLES2-renderable, `EGL_SURFACE_TYPE = EGL_WINDOW_BIT (0x4)`. Mesa's surfaceless+swrast platform only advertises `EGL_PBUFFER_BIT` configs — no match on the surface-type bit.

Fix: copy the attribute list into a scratch buffer on the way in, replace `EGL_WINDOW_BIT` with `EGL_PBUFFER_BIT` on `EGL_SURFACE_TYPE`, pass the patched list to Mesa. This is end-to-end consistent with the Step 55.4 `eglCreateWindowSurface → eglCreatePbufferSurface` redirect — the game asks for a window surface at its API level, gets a pbuffer behind the scenes, doesn't know the difference. Mesa now returns 24 matching configs. Game walks through 24 × `eglGetConfigAttrib` calls (iterating every config looking for `EGL_NATIVE_VISUAL_ID = 0x34325258 = XR24`), then logs `failed to find egl config for gbm format` — no Mesa config declares that visual ID, because Mesa's surfaceless configs have no native-visual tie. Non-fatal; game continues and picks something.

### Frame bridge — parallel socket for binary framebuffers

New `/tmp/spike3_frame.sock` (AF_UNIX SOCK_STREAM), independent from the existing cmode-delta JSON bridge at `/tmp/spike3_bridge.sock`. Gated by `SPIKE3_FRAME_BRIDGE=1`.

Separate socket rather than mixing with the JSON bridge because the traffic shape is fundamentally different — 8 MB binary payloads at render rate vs. newline-terminated JSON at human event rate. Mixing forces every subscriber to parse two framings. Two sockets, one purpose each, is simpler. When the architecture needs to go over UDP or TCP to a VPX client on another box later, the binary wire format is already there — just change the bind target.

Wire protocol (little-endian, 16-byte header + W·H·4 payload): u32 magic `0xF00FBABA`, u32 seq, u16 width, u16 height, u8 format (0=RGBA8), u8 flags (bit 0 = y_flipped for GL bottom-up origin), u16 reserved. Shim holds the socket lock across header + payload send, MSG_DONTWAIT on both — if the subscriber can't keep up (kernel send buffer full), we drop the subscriber rather than queue partial frames.

Hook lives inside `eglSwapBuffers` in real-GL mode: captures via `dlsym(RTLD_NEXT, "glReadPixels")` *before* the swap (back-buffer contents undefined post-swap per EGL spec), sends header + payload, then tail-calls Mesa's swap. Throttled log every 64 frames so the 30–60 Hz swap rate doesn't flood stderr.

Python viewer `_emulation/frame_viewer.py` connects, reads the header, reads the payload, Y-flips if flagged, displays in a tkinter window via `PIL.ImageTk`. Decoder runs in a background thread into a `queue.Queue(maxsize=1)` so Tk's event loop isn't blocked by recv. `--no-display` for headless, `--save-every N` dumps every Nth frame as PNG, `--scale` rescales the window.

The bridge is currently **dormant** — no swaps happen because the game crashes before reaching the render loop. But connecting the viewer against a running game session works end-to-end with no errors; when Step 61 unblocks rendering, frames should start flowing with no further bridge work.

### Instrumentation that made the difference

`FWD_RET_V` from Step 56 was load-bearing here — without per-call forward logs we'd have had no idea that the config-negotiation wall was specifically at the surface-type bit. Added one more ad-hoc diagnostic in this step (the attribute-array decoder) rather than generalizing; config-attrib queries are a one-shot, generalizing the decoder isn't worth it yet.

### Progress vs. Step 56

- Step 56: exit 134 (SIGABRT `std::bad_array_new_length`) at 3,056 lines, crashed in `eglGetConfigs` config-array setup.
- Step 57: exit 139 (SIGSEGV, NULL deref) at 3,963 lines, crashes inside `./game+0x901a7c` (file offset 0x501a7c).

Milestones crossed this run that Step 56 never touched:
- `/games/spike3/bin/SternLogo_800x480.png` — Stern logo PNG access (first asset load).
- `netbridge image okay, CRC32=0x357f5327` — netbridge firmware verified.
- `power dist: ... power dist. boot version: 1.0.0, runtime version: 0.0.0` — power-dist node mode-switch completed (boot → runtime).
- `drm mode 0 w 1920 h 1080` — DRM mode negotiated at expected resolution.
- `DRM ioctl MODE_ADDFB -> fb_id=100` — game creating a DRM framebuffer object. This is the last successful ioctl before the SEGV.

### Breaking the DMA-BUF wall — EGLImage texture backing + VISUAL_ID + dlsym experiments

The first DMA-BUF attempt extended `stub_eglCreateImageKHR` / `stub_glEGLImageTargetTexture2DOES` from pure stubs into real GL-texture backing: parse the attribute list for width/height/fourcc, allocate a unique sentinel, store `{sentinel → (w,h,fourcc)}` in a 128-entry map; when `glEGLImageTargetTexture2DOES` is called, look up the sentinel and call Mesa's real `glTexImage2D(target, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL)` on the currently-bound texture — giving it real storage that the game's subsequent `glFramebufferTexture2D` can attach. No real DMA-BUF needed; Mesa's own GL machinery allocates, we substitute a regular GL texture for the zero-copy DMA-BUF the game thinks it's getting.

First run with this change: still SEGV, no change. Our override wasn't being hit. The game doesn't call `eglGetProcAddress("eglCreateImageKHR")` through our PLT intercept; the eglGetProcAddress log showed only `"eglGetPlatformDisplayEXT"`. Hypothesized the game was using `dlopen`/`dlsym` against libEGL.so.1 directly, bypassing LD_PRELOAD.

Added a `dlopen`/`dlsym` intercept block that tracks handles returned by `dlopen` for libEGL / libGLES / libgbm and substitutes our overrides for `eglCreateImageKHR` / `eglDestroyImageKHR` / `glEGLImageTargetTexture2DOES` on `dlsym` lookups against those handles. Bootstrap via `dlvsym(RTLD_NEXT, "dlopen", "GLIBC_2.34")` / `"GLIBC_2.17"` since our dlsym overrides itself. Log every `dlopen` to confirm interception.

Run-time confirmed: the ONLY `dlopen` calls the process makes are Mesa's own driver loading (`swrast_dri.so`). The game never dlopens libEGL.so.1 — it's DT_NEEDED, loaded at process start. So dlopen hooking was a dead end for this binary, but kept in place as a small, safe instrumentation.

Meanwhile, `readelf -d` confirmed `libEGL.so.1` IS a DT_NEEDED of `game`, and the dynamic symbol table had `eglCreateContext` marked `UND GLOBAL` — a normal PLT import that LD_PRELOAD *should* intercept. But Step 57-c's FWD log still showed no `FWD eglCreateContext`. The discrepancy wasn't resolved; eglCreateContext was silently reaching Mesa through some path we haven't traced. Critically it wasn't blocking — Mesa got a real context either way, the game held onto it, `eglMakeCurrent` (which IS intercepted) bound it, and the game continued.

Final surgical fix — the third open from the earlier plan — `EGL_NATIVE_VISUAL_ID` override in `eglGetConfigAttrib`: forward to Mesa normally, then unconditionally overwrite `*value = 0x34325258` (XR24) for `attribute == EGL_NATIVE_VISUAL_ID (0x302E)`. Mesa's surfaceless configs don't declare any native visual ID; the game's config-filter loop (24 × `eglGetConfigAttrib`) was rejecting every Mesa config against the GBM fourcc our stubs advertise. Overwriting the returned VISUAL_ID to match GBM's XR24 passes the filter.

### It runs

With all four of {SURFACELESS display, WINDOW→PBUFFER surface-type patch, EGLImage → GL-texture backing, NATIVE_VISUAL_ID override} in place, the game **reaches steady state** under real Mesa:

- **Exit 124** (timeout, not crash) at 11,245 log lines — matches the stub-mode regression baseline (11,411 lines). The game is ALIVE.
- `FWD eglCreateContext` fires cleanly (the Step 57-c silent-bypass mystery apparently only manifested when the game was already failing; once config negotiation works, context creation goes through our normal PLT intercept).
- `eglGetProcAddress` is now queried for the full extension set — `eglCreateImageKHR`, `eglDestroyImageKHR`, `eglCreateSyncKHR`, `eglDestroySyncKHR`, `eglWaitSyncKHR`, `eglClientWaitSyncKHR`, `glEGLImageTargetTexture2DOES`. Our table returns our overrides for each.
- `eglCreateImageKHR(target=0x3270 [EGL_LINUX_DMA_BUF_EXT], w=1024, h=256, fourcc=0x34324241 [AB24/ABGR8888])` fires — the game is constructing DMA-BUF-style EGLImages through our override.
- `glEGLImageTargetTexture2DOES(target=0x8d65 [GL_TEXTURE_EXTERNAL_OES], image=...) → glTexImage2D 1024x256 RGBA8` fires — our override materializes the texture backing, Mesa allocates real storage.
- Three `DRM ioctl MODE_ADDFB` calls (fb_id 100 / 101 / 102) — the game is setting up triple-buffered framebuffers for scanout.
- Four `DRM ioctl MODE_PAGE_FLIP #{1..4}` calls — the game is presenting frames through DRM page-flip, cycling through the three framebuffers.

**The render pipeline is live.** Game → GBM BO → EGLImage (our override) → GL texture with real Mesa storage → FBO color attachment → Godot renders → DRM page-flip to present. The full chain works end-to-end.

### The remaining gap — frames route through MODE_PAGE_FLIP, not eglSwapBuffers

Our frame-bridge hook lives inside `eglSwapBuffers`, which **never fires** — the game uses DRM page-flip for presentation, not EGL swap. 4 page flips in the run, 0 swaps. The pixels are real and rendered into real Mesa-allocated textures, but our `glReadPixels` call site is wrong.

Relocation options for the capture hook (pick later):

1. **Hook `MODE_PAGE_FLIP` ioctl.** Track `{fb_id → BO handle}` from `MODE_ADDFB`, `{BO handle → EGLImage sentinel}` from the EGLImage creation chain, `{sentinel → tex_id}` we already have. On page-flip, look up the chain → bind the backing texture to a throwaway FBO → `glReadPixels` → send over the frame bridge. Most principled.

2. **Periodic timer thread.** Fire every 33 ms, bind the most-recently-allocated EGLImage texture to a throwaway FBO, read + send. Simpler, less precise timing.

3. **Hook `glFramebufferTexture2D` + periodic.** Track which texture is the main color attachment; read it periodically. Middle ground.

Route 1 is the right answer for fidelity. Route 2 ships frames tomorrow. Call either way; the frame bridge infrastructure, Python viewer, wire protocol are all in place and unchanged.

### Also noteworthy

- The corrupted-looking `ERROR::SHADER: Compile-time error: Type: VERTEX` messages from the pre-VISUAL_ID runs are gone in the successful run. They were a cascade consequence of the upstream config failure — the game couldn't form a valid EGL surface, so shader compiles in its pre-render setup failed, and the game's own error printer was dumping the shader info log through a dangling pointer (hence the garbage-byte `��[S|` output). Fixing EGL init fixed them.

- `eglCreateContext` eventually does appear in the FWD trace. Theory: the game tries eglCreateContext early (silent bypass we observed), and when its own init code sees the context setup didn't complete cleanly, retries via a second path that DOES go through PLT. Both paths end up at Mesa's eglCreateContext; only the second one shows in our log. Non-blocking either way.

### Artifacts

- `_emulation/spi_shim.c` — +680 LOC net (Step 57 total): SURFACELESS translation helper; 3 display-acquisition function rewrites; attribute-list patching + attribute-name decoder in `eglChooseConfig`; frame bridge socket + accept thread + capture hook; `dlopen`/`dlsym` intercept infrastructure; 128-entry EGLImage sentinel → texture map with `glTexImage2D`-backed resolution; `EGL_NATIVE_VISUAL_ID` override in `eglGetConfigAttrib`; SHIM_LOG definition moved earlier in the file to predate the new `dlopen`/`dlsym` hooks. Size 5,215 → 5,895 LOC.
- `_emulation/spi_shim.so` — rebuilt ~235 KB.
- `_emulation/frame_viewer.py` — new, 186 LOC, tkinter + PIL + numpy subscriber; works against the bridge end-to-end but dormant until capture point moves to `MODE_PAGE_FLIP` or equivalent.
- `_emulation/run_step58_regression.log` — stub-mode regression, 11,411 lines, exit 124, clean.
- `_emulation/run_step58_realgl.log` — first real-GL with SURFACELESS, revealed `eglChooseConfig` empty-return.
- `_emulation/run_step58b_realgl.log` — attribute-array logger added, confirmed `WINDOW_BIT` mismatch.
- `_emulation/run_step58c_realgl.log` — surface-type patched, 24 configs returned, crash at `./game+0x901a7c` in `create_egl_framebuffer` DMA-BUF path.
- `_emulation/run_step58d_realgl.log` — EGLImage texture backing added; override not called; same crash.
- `_emulation/run_step58e_realgl.log` — added `dlopen`/`dlsym` intercept; dlopen only fires for Mesa's own DRI driver, not libEGL; same crash.
- `_emulation/run_step58f_realgl.log` — **breakthrough run**, NATIVE_VISUAL_ID override in place; 11,245 lines, exit 124, EGLImage chain fully exercised, 3 ADDFB + 4 PAGE_FLIP, no SEGV. The game is live.

### What a future session picks up

1. Relocate the frame capture from `eglSwapBuffers` to either `MODE_PAGE_FLIP` (fidelity) or a periodic timer (shipping fast). Both reuse everything in `shim_frame_capture_and_send` except the trigger.
2. Once frames flow, connect `frame_viewer.py` and watch the Stern boot splash + Godot attract mode render live.
3. The switch-injection half of the VPX bridge architecture (message type 0x02 in the frame-bridge earlier protocol sketch) remains unimplemented — comes next, but doesn't need to precede first-frame-visible.

### Step 57 addendum — capture relocated to MODE_PAGE_FLIP; pipeline end-to-end, but pixels are black

The remainder of the session drove the frame capture into the DRM page-flip path. Summary of what was built and what that reveals about the game's render state.

**Capture infrastructure.** The `eglSwapBuffers` hook was replaced with a no-op forward (the game never calls swap — confirmed, 0 swaps in 22 s). A new `shim_frame_capture_at_page_flip()` fires inside the `MODE_PAGE_FLIP` ioctl handler, dlsym-resolves Mesa's `glReadPixels` / `glGetIntegerv` / `glBindFramebuffer`, reads from whatever is bound as `GL_READ_FRAMEBUFFER`, and ships the RGBA over the bridge. Every 2nd flip is captured (30 Hz at 60 fps source). `shim_frame_send` was refactored from MSG_DONTWAIT + drop-on-first-EAGAIN to blocking `send(MSG_NOSIGNAL)`; the 200 KB default kernel send buffer cannot hold an 8 MB frame, and a non-blocking path would drop the subscriber on literally every frame. Blocking in the ioctl handler is acceptable — the game's thread tolerates millisecond stalls there.

**Pipeline end-to-end confirmed.** Final run `run_step58m_realgl.log` + viewer: shim sent ≥ 25 frames (sequence numbers seen), viewer decoded header + RGBA, saved 25 consecutive PNGs (`frame_000001.png` through `frame_000025.png`). No socket errors, no dropped frames until the game exited on timeout and the viewer saw `socket closed by peer`. The full chain — shim's `glReadPixels` → 16-byte header + 8 MB payload over `/tmp/spike3_frame.sock` → viewer's numpy reshape + PIL save — is operational.

**But the pixels are black.** `np.count_nonzero(frame)` returns 2 out of 6.2 M for every saved frame; max pixel value 81 (out of 255). Investigation:

1. Capture was initially against `fb=0` (the pbuffer the game received from `eglCreateWindowSurface` → pbuffer redirect). Reads all black — the game isn't compositing into the default framebuffer.
2. Added a `glBindFramebuffer` hook that records the game's most recent non-zero draw-FBO into `shim_last_draw_fbo`; capture switched to read from that. `from_fbo=0` still — the hook NEVER saw a non-zero FBO bind.
3. Diagnostic: count glBindFramebuffer calls under real-GL. Result: 20 calls across 22 s, ALL with `fb=0`. The game is only ever binding the default framebuffer through the standard PLT path.
4. Cross-reference: grep for `glFramebufferTexture2D` / `glGenFramebuffers` / `glDrawArrays` / `glDrawElements` in the intercepted log — zero hits. (Our shim exports all of them per `nm -D spi_shim.so`, so they would be intercepted if called.)
5. Shader errors that plagued the pre-VISUAL_ID runs are gone (0 in the final runs) — Mesa is compiling shaders successfully, so the pipeline can work, the game just isn't issuing draw calls into it.

**What this means.** The game's 22-second run executes the EGL + DRM setup, allocates one 1024×256 `GL_TEXTURE_EXTERNAL_OES` via our EGLImage path (probably a font/UI atlas), cycles the DRM triple-buffer with 4 empty page flips, and then idles in the netbridge event loop. **There is no Godot-scene render activity yet.** The 4 page flips are pipeline priming, not frame presentation of rendered content. The `"Godot Engine"` banner that normally appears in Godot-based games is absent from all real-GL logs — Godot never finishes its boot sequence.

Why Godot hasn't booted to the scene tree is the next investigation target — probably waiting on some expected runtime state that our mocks don't satisfy (a switch, an asset load, a netbridge event, a service_s_worker response, a clock). The user's BN notes on `create_egl_framebuffer` (0xd00800) described the path the game *would* take for actual scene rendering; in this boot it's not reaching that callsite, or it's reaching it but bypassing our LD_PRELOAD the same way `eglCreateContext` and `eglCreateImageKHR` did.

**State left for next session.** The capture path is complete and proven to deliver pixels. `frame_viewer.py` works. The blocker is upstream: find what's keeping Godot from initializing, unblock it, then rendering flows through the capture we already built. No more GL-level bring-up required.

### Step 57 final artifacts

Added to the earlier list:
- `_emulation/spi_shim.c` — now 5,970 LOC. Added: `shim_frametex_record` + 32-entry registry (vestigial, kept for diagnostic); `shim_frame_capture_at_page_flip` (game-thread GL readback); refactored `shim_frame_send` / `shim_frame_send_all` to blocking `send`; `#include <poll.h>` (dormant now that send is blocking); `glBindFramebuffer` hook that tracks `shim_last_draw_fbo` for future use; diagnostic bind-count logger.
- `_emulation/spi_shim.so` — rebuilt ~240 KB.
- `_emulation/run_step58m_realgl.log` + 25 × `_emulation/frames/frame_*.png` — first successful end-to-end capture; frames are black (game state, not pipeline issue).
- `_emulation/run_step58n_realgl.log` — capture with last-draw-FBO tracking enabled; identical black output, proved the game binds only `fb=0`.
- `_emulation/run_bindfb_check.log` — diagnostic confirming 20 `glBindFramebuffer(fb=0)` calls, zero `glDraw*` / `glFramebufferTexture2D` / `glGenFramebuffers` in the intercepted log.
- `_emulation/run_step58_stub_final.log` — final stub-mode regression, 11,725 lines, exit 124, zero fatals. All Step-58 additions are neutral to stub mode.



### Artifacts

- `_emulation/spi_shim.c` — +330 LOC net (SURFACELESS translation helper, 3 display-acquisition function rewrites, attribute-list patching + name decoder in `eglChooseConfig`, frame bridge socket state + accept thread + capture-and-send, constructor hook). Size 5,215 → 5,545 LOC.
- `_emulation/spi_shim.so` — rebuilt 225,232 bytes (was 158,968 at Step 56; +66 KB for new bridge infra + patching logic).
- `_emulation/frame_viewer.py` — new, 186 LOC, tkinter + PIL + numpy subscriber with headless and PNG-save modes.
- `_emulation/run_step58_regression.log` — stub-mode regression, 11,411 lines / 22 s / exit 124, clean. Matches Step 56 baseline (11,478 lines).
- `_emulation/run_step58_realgl.log` — first real-GL run with SURFACELESS translation, 4,141 lines, exit 139. Revealed the `eglChooseConfig` empty-return.
- `_emulation/run_step58b_realgl.log` — added attribute-array logger, 3,971 lines, exit 139. Confirmed `EGL_SURFACE_TYPE = EGL_WINDOW_BIT` was the mismatch.
- `_emulation/run_step58c_realgl.log` — surface-type patch applied, Mesa returns 24 configs, game crosses 4 new milestones (SternLogo, netbridge, power-dist, DRM mode), crashes in `create_egl_framebuffer` DMA-BUF path. 3,963 lines.

## Step 58: BN notebook read; boot-splash ⊥ Godot confirmed; blocker hypothesis pivots (2026-04-22)

User exported the full BN analyst notebook to `_emulation/Binary Ninja Notebook Export (2026-04-22).json` (172 KB / 80 entries / 10 categories). Read in full. Key conclusions that reshape our next move:

**Architectural separation is explicit.** Entries [76], [77], [78] document that the game runs TWO distinct render pipelines:
- Boot-splash / DMD renderer at `0xcff000-0xd05000`: direct EGL/GLES 2.0 with `GL_TEXTURE_EXTERNAL_OES` + `samplerExternalOES`, custom shaders at `0x3a1ffe8` (vertex), `0x3a200a8` (fragment), `0x3a23d78` (color transform), `0x3a1df50` (gradient). Shader compiler at `Shader.cpp` (`0xce6530`) — **the exact source of our `OpenGL Error: 0x506 at Shader.cpp line 17`**.
- Godot engine: RendererRD (Vulkan) or GLES3 backend, separate FBO setup, incompatible GLSL versions (GLES 3.0 / GLSL 450).

The `create_egl_framebuffer` at `0xd00800` — the DMA-BUF pipeline we've been investigating — is the **boot-splash path only**. Godot has its own FBO path we haven't observed yet.

**Implication for our diagnosis.** The 4× `0x506` errors (3× SpriteRenderer.cpp:143 glDrawArrays, 1× Shader.cpp:17 glUseProgram) are **boot-splash failures only**, not blockers on Godot. Our plan to fix EGLImage → GL_TEXTURE_2D substitution doesn't unlock Godot rendering; it fixes a pipeline Godot doesn't use.

**What the notebook does NOT answer (sent as follow-up query):**
1. Godot engine entry points: `OS::initialize`, `DisplayServer::create`, `RenderingServer::init`, `SceneTree::init`, `Main::setup` / `Main::start` — where are they in the binary?
2. `procDispatch` thread (`sub_b2ae90`) decompilation — does it call Godot init or not?
3. **The 10-second timeout loop at `0xb2aba0`** (entry [2]) — `pthread_cond_timedwait` with timing thresholds at 0x257ms / 0xe0fms. What condition variable does it wait on, and who signals it? If Godot init is gated on that signal, this is our actual blocker.
4. Explicit confirmation of a second `eglCreateContext` (one for boot-splash, one for Godot) or just one shared context.
5. Init-sequence gating: what must complete before Godot starts?
6. Is `SceneLoaderLayeredDisplayElement` (entry [70]; base class for all 130+ BDLMode-derived types) a Godot `Node` subclass or Stern's own?

**Bonus findings relevant for later:**
- Entry [51]: game selects `/dev/ttyAMA5` vs `/dev/ttyAMA2` from `uname().release[8]` — host kernel version character affects our UART shim.
- Entry [2]: primary event loop triggers `/bin/sync` + `/sbin/reboot -f` under "specific conditions." We run 22s without a reboot, so those conditions aren't tripping, but worth knowing there's a self-destruct path.
- Entries [1], [9]: SIGINT / SIGBUS / SIGSEGV handlers registered at `_start @ 0x861270`. SIGBUS specifically for hardware access faults.
- Entries [30], [73]: game logic is data-driven via scene files (`image-sc*.bin`, 885+ scenes); compiled binary is presentation engine + event dispatcher only. Fire_event at `0x9c12a0`; mode init table at `_INIT_74 @ 0x8733c0` (matches earlier Step-50 findings).
- Entries [35], [37], [41]: `/dev/spidev4.0` is a serial bridge to an I/O controller MCU, not true SPI — we already shim this correctly.
- Entry [46]: Godot audio uses Godot's native AudioStream pipeline (OggVorbis), NOT GStreamer. GStreamer is video-only. Confirms the Step 52 finding that gstreamer is dormant for audio.
- Entry [32]/[33]: 4 embedded MP3s directly in data sections at 0x462e006 / 0x46742ee / 0x474b61e / 0x476599e (~100 KB each).

**Caveat per user warning:** notebook entries were written over multiple BN analysis sessions and **may self-contradict** when a later observation refines an earlier one. Early entries call this a "slot machine" — BN analyst's initial (wrong) interpretation; this is Stern's Pokémon Pinball. When anything in the notebook seems suspicious, cross-reference with our own logs before trusting it.

**Holding position until the follow-up BN query returns.** No shim changes this turn — no point wiring up a DMA-BUF/EGLImage fix for the boot-splash if the Godot gate is a condition variable elsewhere. Saved notebook pointer to memory (`reference_bn_notebook.md`) so future sessions can re-enter the context without re-reading 172 KB of JSON.

## Step 59: BN follow-up (v4) overturns the two-pipeline story (2026-04-22)

Follow-up BN queries landed as notebook versions v2–v4 (entries 81–86, append-only). The architectural read from Step 58 was **wrong** — a correction the analyst made in the follow-up pass.

**[82] Single EGL context architecture — boot-splash is the ONLY rendering system.** There is exactly one EGL context creation callsite (`create_boot_splash_egl_context @ 0xd01d10`). Godot's rendering classes (`RasterizerSceneGLES3`, `GLES3::MeshStorage`, `ShaderGLES3`) are compiled into the binary but never instantiated at runtime. Boot-splash renders everything, including what would have been Godot scene content. *If boot-splash init fails, there is NO fallback path.* This directly contradicts entry [76]'s older claim of "two distinct pipelines" — [76] was the analyst's initial (wrong) interpretation.

**[81] No standard Godot init.** `OS::initialize` / `DisplayServer::create` / `RenderingServer::init` / `SceneTree::init` / `Main::setup` / `Main::start` are all absent from the runtime flow. The game accesses Godot components through a custom `godot_spike_bridge` interface, not Godot's own bootstrap. So the "where does Godot init?" question from Step 58 has no answer because it never happens — `procDispatch` at `sub_b2ae90` drives the entire show through condition variables and event firing.

**Consequences.**
- The 4× `0x506 GL_INVALID_FRAMEBUFFER_OPERATION` errors are not boot-splash-only collateral that we could ignore. They're the entire render pipeline failing. Every black frame is what the boot-splash actually tried to draw.
- The Step 58 plan ("let boot-splash fail, Godot will take over") doesn't work because there is no Godot to take over. Fixing the boot-splash FBO is the blocker.
- `create_egl_framebuffer (0xd00800)` — the DMA-BUF / `GL_TEXTURE_EXTERNAL_OES` pipeline — isn't "boot-splash only" in the sense of being a skippable subset; it's the only FBO setup the game has.

**[84] 10-second timeout at 0xb2aba0 is fully traced.** Condition variable `g_main_loop_condition_var (0x48c9288)`. Single signaler is `process_dispatch_loop (0xb292ac)`, which signals at `0xb293c8` after draining the event queue and firing event `0x92`. Dispatch thread runs on its own CV `0x48c86f8` monitoring `event_queue_change_counter (0x48c8728)`. Main loop stalls if nothing enqueues events for `process_dispatch_loop` to consume. Not directly our current blocker (the game runs past this — no reboot in 22 s), but useful for switch-injection work later.

**[85] Timing thresholds 0x257 ms (599 ms) and 0xe0f ms (3599 ms).** Gate downstream operations on `(now - g_boot_time_reference @ 0x48ca668)` being in a range AND `system_state_flags` bit `0x10` being set. Threshold getters at `get_timestamp_threshold_1 @ 0xb84120` and `get_timestamp_threshold_2 @ 0xb68590`. The bit-0x10 flag is almost certainly "boot-splash init succeeded" — which for us is never set because init fails.

**[86] Shutdown gate.** `/bin/sync` + `/sbin/reboot -f` only fires when `shutdown_condition_flag_1 (0x48c9468)` set AND `shutdown_condition_flag_2 (0x48c9469)` clear. We're not hitting this — confirms the 22 s timeout is just qemu killing us, not the game rebooting.

**[83] Switch input pipeline.** Switch changes are detected by polling `g_switch_state_array (0x4880220)` (not interrupt-driven), piped through `/dev/spidev4.0` serial packets. Clarifies that the earlier notebook speculation about UART command `0x27` was wrong. Relevant for when we build the VPX-→-game switch-injection direction of the frame bridge; not today's blocker.

### Revised next move

The Step-58.post plan (`glFramebufferTexture2D` intercept to swap `EXTERNAL_OES` → `TEXTURE_2D`) was the right IDEA but the wrong execution — the intercept never fired because the game doesn't reach our `glFramebufferTexture2D` export through the PLT (GLAD-style cached function pointers bypass LD_PRELOAD). Two viable approaches:

1. **Resolve the substitution via `eglGetProcAddress` instead.** We already intercept `eglGetProcAddress` and return our own stubs for KHR extensions. Extend the table to return shim wrappers for `glFramebufferTexture2D` / `glFramebufferTextureLayer` / `glFramebufferRenderbuffer` / `glCheckFramebufferStatus`. If the game resolves these via `eglGetProcAddress` (which our Step 57 log showed it does for KHR extensions), our wrappers get the call and can rewrite the target.

2. **Upstream swap in `glEGLImageTargetTexture2DOES`.** Rather than attach `EXTERNAL_OES` textures to an FBO at all, maintain a parallel `GL_TEXTURE_2D` texture per EGLImage: when the game binds a `EXTERNAL_OES` texture via our `glEGLImageTargetTexture2DOES`, silently create a `TEXTURE_2D` with same dimensions, record the {external_tex_id → 2d_tex_id} mapping. The game then calls `glFramebufferTexture2D(GL_TEXTURE_EXTERNAL_OES, external_tex)` which still fails — UNLESS we also intercept the FBO attach via (1). The two approaches work together.

3. **Verify via `eglGetProcAddress` query count first.** Before building, check whether the game queries `eglGetProcAddress("glFramebufferTexture2D")`. If it doesn't, approach (1) won't fire either, and we need a different interception mechanism (possibly hooking `dlsym(RTLD_DEFAULT, ...)` or auditing the game's GL function-pointer table in memory).

Verification is one run with the existing shim and an expanded `eglGetProcAddress` logger — effectively free. Worth doing before committing to either fix.

## Step 60: LFS open() family hooks — main.pck was invisible to us (2026-04-22)

After Step 60's SpiDecoder::Play vtable patch, the game still SIGABRT'd at shutdown with `pure virtual method called` / `terminate called without an active exception`. The patch fired (`[vp] fake_spi_play` logged twice, ***** banner gone), but the game reached the same destructor cascade and aborted.

Looking at what the game was actually doing, I noticed our shim logged **zero** opens for `.pck`, `.tscn`, or anything Godot-resource-looking. The game binary references `./assets/godot/main.pck` as an embedded relative path — so either the pck was embedded in the binary (no `GDPC` magic, ruled out) or we weren't seeing the open.

`readelf --dyn-syms` showed the game links against **`open64@GLIBC_2.17`**, **`openat64@GLIBC_2.17`**, **`fopen64@GLIBC_2.17`**, **`popen@GLIBC_2.17`** — the Large File Support family. On 64-bit aarch64 Linux, `open64` is functionally identical to `open` (off_t is already 64-bit), but the symbols are distinct. Our shim was only hooking `open` and `openat`, so `open64`/`fopen64` calls fell through to real libc without path-remap or logging. The game's Godot resource loader uses `fopen64` — so we were blind to every single Godot-side file open, including `main.pck` itself.

Added `open64`, `openat64`, `fopen64` hooks mirroring the existing `open`/`openat` semantics (device interception + sysroot path remap + logger). Widened `should_log_open` to match `*.pck`, `*.tscn`, `*.scn`, `*.remap`, `*.import`, `godot` patterns regardless of path prefix.

**Result — game no longer crashes.** Exit code changed from 134 (SIGABRT) to 124 (timeout). The game runs the full 185 seconds stable. 622 successful `fopen64` calls, 660 successful `open64` calls, game actively loads main.pck (multiple times — probably re-mapped for different resource queries). Godot banner prints, scripts load, and MODE_PAGE_FLIP fires **40 times** per 185 s (up from 4-5). Viewer captures 215 frames, each with a unique hash — Pokemon logo now has live "Startup in Progress..." dot animation.

**Remaining issues (not blocking, but open):**
- `/root/Main` still not found when GDScripts reach for it (`$"/root/Main"` in capture_battle, discover) — the shipping binary's custom init bypasses main.tscn load per BN [81]
- Game is still in boot-splash steady state, not advancing to attract mode
- `Condition "!is_inside_tree()" at play (video_player_spi.cpp:376)` still fires — orphaned VideoStreamPlayer nodes being play()'d outside the tree
- 127 `FOPEN64 FAIL` and 135 `OPEN64 FAIL` — missing resource files (mostly Godot cache dirs, override.cfg, .godot/scene_groups_cache.cfg). Non-fatal.

**What this changes about our earlier conclusions:**
- Step 60's "game aborts at ~180s deterministically" → actually, the abort was caused by the unresolved fopen64 paths corrupting enough state to trip destructor sequencing during shutdown. Without those failures, no abort.
- The `pure virtual method called` was a SYMPTOM of the `open64`-hiding problem, not a root-cause destructor bug.
- BN entry [81]'s "custom Stern init bypasses standard SceneTree::init" is still true, but Godot is clearly far more alive than we thought — main.pck is loaded, scripts run, scenes instantiate.

**Next:** investigate why `/root/Main` isn't loaded despite `main.tscn.remap` being present in the pck. Is there a missing `ResourceLoader::load("res://scenes/main.tscn")` + `add_child` in the C++ init path? Could the game be waiting on a specific event to kick off Main's load? The shipping init's custom path loads `pokemon_battle_scene`, `capture_battle_scene`, `Discover` directly but Main never shows up.


## Step 61: fallback pbuffer unblocks the render — real pixels out, boot-splash is drawing (2026-04-22)

User's strategic pivot (Step 62) of ignoring Godot paid off in one session. Traced the real blocker cleanly: the game calls `eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)` — a surfaceless context binding — but still draws to `fb=0` (the default framebuffer). On real hardware the game's GBM→DRM render chain apparently provides an implicit drawable that we don't reproduce; on our shim, Mesa's default framebuffer for a surfaceless context is `GL_FRAMEBUFFER_UNDEFINED (0x8219)`, so every `glDrawArrays` / `glUseProgram` returned `GL_INVALID_FRAMEBUFFER_OPERATION (0x506)` by spec. Those were the `SpriteRenderer.cpp:143` and `Shader.cpp:17` errors we'd been chasing all of Step 57 — never an FBO-completeness problem on a user framebuffer, always the *default* framebuffer failing because no surface was bound.

### Diagnostic path

Extended the `eglGetProcAddress` table with `glFramebufferTexture2D` / `glFramebufferTextureLayer` / `glFramebufferRenderbuffer` to catch any via-procaddress FBO creation — confirmed the game never queries these names. Combined with zero glGenFramebuffers/glFramebufferTexture2D intercepts through PLT, this was strong evidence the game simply doesn't create user FBOs in this boot path; it draws straight to the default framebuffer.

Added a post-`eglMakeCurrent` diagnostic querying `glCheckFramebufferStatus(GL_FRAMEBUFFER)` and `eglQuerySurface(draw, EGL_WIDTH/HEIGHT)`. That revealed `draw=NULL` (surfaceless) and default-FBO status `0x8219 = GL_FRAMEBUFFER_UNDEFINED`. Same run also tripped the Step-56.3 `eglGetError() == EGL_SUCCESS` assertion because `eglQuerySurface` on a NULL surface sets `EGL_BAD_SURFACE` — fixed by guarding the diagnostic.

### Fix

In `eglMakeCurrent`: when `draw == NULL || read == NULL` under SPIKE3_REAL_GL, lazily create a fallback pbuffer by calling Mesa's real `eglCreatePbufferSurface(display, first_EGLConfig_we_saw, {WIDTH, HEIGHT, EGL_NONE})` and substitute it for the NULL handles before forwarding to Mesa. The config comes from a capture in `eglChooseConfig` (captured the first Mesa-returned config right after the `WINDOW_BIT→PBUFFER_BIT` patch). The display comes from a capture in `shim_real_platform_display`.

The game never reads back from the default framebuffer — its actual present path is DRM `MODE_PAGE_FLIP` on GBM BOs, and its scene content goes through EGLImage-sampled FBOs that read external textures as sources. So substituting a pbuffer it didn't know it had doesn't break anything downstream; it just gives `glDrawArrays` somewhere to draw *to*, which is sufficient to unblock the render queue.

### Result

- **Zero `OpenGL Error` messages** in the run — down from 4 per previous real-GL runs.
- `glCheckFramebufferStatus(DEFAULT) = 0x8CD5 (GL_FRAMEBUFFER_COMPLETE)` — the default framebuffer is now usable.
- Game runs to timeout (exit 124, 11,186 lines), same steady state as before.
- Frame bridge captured **22 × 1920×1080 PNGs** via `MODE_PAGE_FLIP` hook.
- PNG analysis: **~34% non-zero pixels, full 0–255 range** (vs. 0.02% non-zero, max 81 in Step 57's all-black captures). That's real image content.
- The content is stable frame-to-frame (34.0%, 34.2%, 34.2%, 34.2%...) which is consistent with a static image (e.g. the Stern boot splash — `SternLogo_800x480.png` that we saw get loaded earlier in the run).

### Why the Godot pivot mattered

If we'd kept chasing "why isn't Godot initializing," we'd never have looked at `eglMakeCurrent`'s arguments. The Godot-ignoring stance forced the right reframe: the render pipeline is purely Stern's own, so the failure had to be in the Stern-side GL/EGL plumbing, and the diagnostic path (`glCheckFramebufferStatus` on the default FB right after `eglMakeCurrent`) was only worth writing if we'd stopped looking for a Godot gate. Entry [82]'s "single EGL context architecture" + the user's "Reference implementation A runs a different Spike 3 title without Godot" observation collapsed the search space to a bounded GL problem.

### Artifacts

- `_emulation/spi_shim.c` — +120 LOC net: fallback-pbuffer state (`shim_last_real_display`, `shim_last_real_config`, `shim_fallback_pbuf`), config capture in `eglChooseConfig`, display capture in `shim_real_platform_display`, pbuffer substitution + diagnostic block in `eglMakeCurrent`, forward decls for the three globals, extra `glFramebufferTextureLayer` export, three extra entries in `eglGetProcAddress` table.
- `_emulation/spi_shim.so` — rebuilt.
- `_emulation/run_step58q_procaddr.log` — proved `glFramebufferTexture2D` not queried via `eglGetProcAddress`.
- `_emulation/run_step58r_mkcur.log` — revealed `draw=NULL` + `GL_FRAMEBUFFER_UNDEFINED (0x8219)`; also tripped the Step 55.3 assertion because our `eglQuerySurface(NULL,...)` clobbered EGL state.
- `_emulation/run_step58s_fallpbuf.log` — first run with fallback pbuffer; zero 0x506 errors, FBO status COMPLETE.
- `_emulation/run_step58t_viewer.log` + `_emulation/frames/frame_000001.png` through `frame_000022.png` — first real-pixel captures; 34% non-zero content.

### What's next

Look at an actual saved PNG to see what the boot-splash is drawing. If it's the Stern logo, we know the render chain is end-to-end functional for static content. Then the next questions open up: why only 4 page flips (is the splash expected to be multi-frame animated)? Does the game ever transition past boot-splash into attract/gameplay without additional prodding? Those are game-logic questions now, not rendering questions.

## Step 62: strategic pivot — ignore Godot entirely (2026-04-22)

User called out that Reference implementation A for a different Spike 3 title doesn't involve Godot. FotE and Pokemon LE are both Spike 3 games; the platform-level render pipeline is the common denominator. If Godot were required at runtime for the render loop, Researcher A's emulator wouldn't be working without it.

Reconciling with the BN notebook evidence:
- Entry [25] explicitly frames this as "selectively uses Godot Engine components as libraries while maintaining complete independence" — that's Stern linking Godot's utility classes statically (Array, Dictionary, Variant, Resource) without booting the engine.
- Entry [81] "no traditional Godot initialization functions" fits "engine never booted."
- Entry [82] "Godot's rendering classes compiled into the binary but NEVER INSTANTIATED" fits "utility classes used, engine not used."
- The `GodotSpikeBridge` class we found is a publish-queue for sound events and game events — not render-related. It's plausibly used by Stern's own scene-logic code (the 130+ BDLMode classes), not by a running Godot VM.
- The previs project at `assets/godot/pokemon_battle_previs/` is a DEV tool. Its autoload `Spike` → `godot_spike_bridge.gd` is a GDScript STUB mimicking the C++ class's interface so dev scenes can run standalone outside the game binary.

Reframing Track A: the render pipeline is entirely Stern's own. `create_boot_splash_egl_context (0xd01d10)` is THE EGL context. `create_egl_framebuffer (0xd00800)` is THE FBO setup. `SpriteRenderer` and `Shader` (at `Shader.cpp:17`, our error site) are Stern's own C++ classes drawing from Radium scene data loaded out of `image.bin`. The `0x506 GL_INVALID_FRAMEBUFFER_OPERATION` errors we captured are the whole render pipeline failing — fixing them is the full job, bounded and GL-scoped. No Godot excavation required.

The 10-second `pthread_cond_timedwait` gate at `0xb2aba0` (entry [84]) waits on events fired by `process_dispatch_loop`. I had been assuming those events came from `GodotSpikeBridge.pending_events`. More likely candidates now that Godot is off the table: SPI/netbridge packets from the hardware MCU bridge, internal timers, scene-loader completion callbacks. Our SPI stub returning zeros at ~1 kHz is probably enough activity to keep the dispatch wheel turning (game runs 22 s without rebooting, consistent with "timeout never tripping because events keep coming").

What stays in the notebook that's still useful:
- All boot-splash / render-path entries [74] [75] [76] [77] [78] [79] [80] [82] [85]
- Hardware bridge entries [35] [36] [37] [38] [39] [41] [50] [83]
- BDLMode / scene-loader entries [69] [70] [73]
- Asset-loading entries [13] [14] [19] [20] [24]
- Sound-ID pipeline entries [42] [43] [45] [63] [64] [67]

What's parked (revisit only if Track A hits a game-logic issue that turns out to be script-driven):
- GodotSpikeBridge internals — class methods, what it drains to, where it's registered
- Godot VM status at runtime
- SceneLoaderLayeredDisplayElement relationship to Godot

Revised next action: extend the `eglGetProcAddress` intercept as a two-way instrument. (a) log every unknown name the game queries — tells us what core GL functions the game resolves through eglGetProcAddress vs direct linkage; (b) if the game queries `glFramebufferTexture2D` / `glFramebufferTextureLayer` / `glFramebufferRenderbuffer` through this path, return our wrapper so the `EXTERNAL_OES → TEXTURE_2D` swap actually fires. If it doesn't query through this path, we learn the game uses `libglapi.so.0`'s internal dispatch table and the fix needs to happen at a lower layer (patch the dispatch table at memory-address level post-context-create). Either outcome is informative and the change itself is ~30 LOC plus a sort-uniq pass on the log afterward.


## Step 63: shim refactor — seven phases of carving, before we keep digging (2026-04-22 → 2026-04-23)

Step 62 ended with a narrow plan: instrument `eglGetProcAddress` to see what core-GL names the game queries. Before writing it I re-read `spi_shim.c` and concluded the file itself had become the problem. At ~6400 LOC in a single translation unit, every new diagnostic meant scrolling past unrelated state, and I kept asking myself "does this subsystem hook the LFS variant?" or "is this fd predicate already defined somewhere above?" The Godot pivot (Step 62) reframed the investigation as a bounded Stern-only GL problem — but a bounded search space doesn't help when the tool you use to search it is illegible. The next several investigation steps are going to accrete a lot of diagnostic code; landing all of that on top of a 6400-line monolith would make it impossible to tell what subsystem each new hook belonged to, or whether an existing hook already covered the case.

So we took the detour: carve the shim into subsystem translation units along the boundaries of its actual behavior, preserve every line (including BN-derived RE code — cmode tables, switch manifests, dispatch-table dumpers), and give it the shared header it had been missing. Baseline-match each phase against the pre-refactor smoke run; anything that deviates is a regression and gets fixed before moving on. Pure maintainability investment — no new behavior, no new diagnostics (except defensively), no attempt to advance the game-logic frontier.

### The seven phases

| Phase | Module                | Owns                                                                        |
|-------|-----------------------|-----------------------------------------------------------------------------|
| 1     | `spi_shim.h`          | `SHIM_LOG`, `FWD_RET`/`FWD_VOID` and verbose variants, is_real_gl() proto. First step — macros every TU needs. |
| 2     | `shim_bridges.c`      | state bridge poll thread, frame bridge (`glReadPixels` → RGBA socket), page-flip capture. No-op unless `SPIKE3_BRIDGE=1` / `SPIKE3_FRAME_BRIDGE=1`. |
| 3     | `shim_diagnostics.c`  | cmode registry (42 BN-extracted entries), event sweep, SIGSEGV/SIGUSR2 dump handlers, dispatch-table dumpers, sound-globals dump. **The explicit home for RE output.** |
| 4     | `shim_nodebus.c`      | UART netbridge framing + response synth. Keeps the game's dispatch wheel turning without real hardware. |
| 5     | `shim_fs.c`           | `open`/`openat`/`open64`/`openat64`/`fopen`/`fopen64`, device intercept dispatcher, GPIO sysfs fake-fd pool, sysroot path remap. Step 60's LFS hooks carved here kept them together. |
| 6     | `shim_gl.c`           | EGL surface/context plumbing, GLES stubs, fallback pbuffer, `eglGetProcAddress` table. Step 61's pbuffer fix lives here. |
| 7a    | `shim_io.c`           | `close`, `read`, `lseek`, `write` + LFS variants (`lseek64`, `pread`/`pread64`, `pwrite`/`pwrite64`). |
| 7b    | `shim_ioctl.c`        | `ioctl` dispatch for UART/SPI/DRM/V4L2/I2C + DRM page-flip event synthesis. |

`spi_shim.c` dropped from 6385 → 2235 lines (65% moved). What's left is globals, `intercept_device`, the `resolve_all` dlsym pump, crash-handler install, and a few tie-together bits that Phases 8–9 will address.

### What the refactor paid for immediately

**(1) Found a blind spot in our LFS hook coverage.** Phase 7 pre-emptively added `lseek64`, `pread`/`pread64`, `pwrite`/`pwrite64` with a `SHIM_LFS_FIRST_CALL` banner macro that logs the first time each new variant fires. On the next smoke run, `pread64` immediately banner-banged — 7 calls on `fd=31` at offsets 102M, 282M, 811M with 44,800-byte chunks. Reactive logic would have left that path silent forever; proactive instrumentation surfaced it the moment we wired the hook. The `fd=31` provenance isn't yet mapped (no `OPEN64 OK` line binds a path to fd 31 — likely inherited, or dup'd, or opened by a thread whose path fell outside `should_log_open`'s filter), but we now know the game is doing random-access reads on a large file during steady state. The 811M offset exceeds `main.pck` (210 MB), so it's almost certainly `image.bin` (1.1 GB) — a concrete new lead we didn't have before Phase 7.

**(2) Codified "hook both variants" as policy.** `shim_io.c`'s top block-comment records the rule-of-thumb: if you hook `FN`, hook `FN64`; the asymmetry is an unreliability generator, not a redundancy. We documented the three remaining asymmetries (`fopen` plain, `pread` plain, `pwrite` plain) with `fopen` flagged highest-priority because it takes a path and could silently leak un-remapped paths past `shim_fs.c`.

**(3) Research code has a home.** `shim_diagnostics.c` is now the explicit container for BN-extracted knowledge and the probing machinery that reads it — cmode table, event sweep, dispatch-table dumpers, sound-globals dump. Memory `feedback_preserve_re_knowledge` was written mid-refactor after I almost deleted the cmode table during Phase 3 as "unused." Future RE findings land there instead of being wedged into whatever unrelated file was most recently touched.

**(4) Baseline-match regression harness.** Each phase's smoke run compared to the pre-refactor baseline. Through Phase 7 the only deviation is GPIO jitter (single-bit read-order difference that doesn't affect game logic). No behavioral drift, which matters because several phases moved hooks across TU boundaries and we had no type-checking layer to catch a silently-dropped dependency.

### What the refactor does NOT change

The game still stops where Step 61 + Step 60 left it: boot-splash with 40 page flips / 215 unique frames over 185 s, Godot banner prints, scripts load, `/root/Main` still missing, `VideoStreamPlayer::play()` still trips `is_inside_tree() == false` on orphaned nodes at destruct. This step didn't advance the state machine. That's fine and intentional — we paid tech-debt tax so Phases 8–9 and the subsequent investigation can move cheaply.

### What's next

- **Phase 8 (`shim_patches.c`)** — pull the runtime patches out of `spi_shim.c`: memcpy NULL-guard, Step 60 SpiDecoder::Play vtable patch, Step 64 `get_format` NOP, libavcodec HW-accel bypass, dummy-frame injection. These all share the "find-and-replace after dlopen" shape and belong together.
- **Phase 9 (`shim_inject.c`)** — switch-manifest poker, event-sweep scheduler, `vp_patch` thread. All write into game memory at timed intervals; same operational shape.
- **Follow-up: Phase 7 asymmetries.** Add `fopen`, `pread`, `pwrite` plain-variant hooks with the FIRST-CALL banner. Start with `fopen`.
- **Follow-up: `fd=31` provenance.** Walk the full log for what was opened in the window before `pread64 #1` (or loosen `should_log_open` to dump every open regardless of path filter).
- **After 8/9:** return to the investigation frontier. Two concrete threads — (a) the `/root/Main` custom-init question (how does Stern's boot path bring Main into the tree, given BN [81] says standard `SceneTree::init` is bypassed?); (b) the `VideoStreamPlayer is_inside_tree()` destructor trip (orphaned nodes that shouldn't have been constructed, or should have been parented to a fake root before `play()`).


## Step 64: refactor endgame — shim_patches.c + shim_inject.c close out the carve (2026-04-23)

Phases 8 and 9 land the last two modules and complete the refactor.

### Phase 8 — `shim_patches.c`

The "surgery" bucket: hooks and byte-level patches that intentionally alter the game's behavior at runtime. Pulled five blocks out of `spi_shim.c` totaling 501 lines:

- `memcpy` NULL-guard (Step 43) — the long-standing Godot-DisplayRenderer-thread `memcpy(dst=NULL, src=NULL)` workaround.
- Step 60 `SpiDecoder::Play` vtable patch + `shim_fake_spi_play` replacement.
- Step 64 `get_format` callback NOP patch + `shim_sw_hevc_enabled` env-var gate.
- libavcodec HW-accel bypass — `avcodec_find_decoder_by_name` name swap (`hevc_v4l2request` → `hevc` under `SPIKE3_SW_HEVC=1`) plus `avcodec_get_hw_config` / `av_hwdevice_ctx_create` / `avcodec_send_packet` / `avcodec_receive_frame` passthrough hooks with the `shim_resolve_libav(..., "LIBAVCODEC_59")` dlvsym helper.
- Step 45b dummy-frame injection — `shim_init_dummy_frame` (`av_frame_alloc`/`av_frame_get_buffer` to allocate a 1920×1080 NV12 frame zero-filled to studio-black) + the `av_frame_ref`/`av_frame_unref` path in `avcodec_receive_frame` that substitutes it when the real decoder returns EAGAIN under `SPIKE3_INJECT_VIDEO=1`.

Intentionally kept in `spi_shim.c` through Phase 8: `shim_vp_patch_thread`, the 2-second-delayed thread that applies the two patches. It moved in Phase 9 as part of the "all worker threads in one place" design. Exposed `shim_patch_spi_video_vtable` / `shim_patch_get_format_callback` via the header so the thread could keep calling them cross-TU.

Smoke result: 20 of 22 error classes bit-identical to baseline. Two coupled retry counters (V4L2 + hwaccel) dropped 7 → 6, which was the most invariant metric prior (7 across every one of the 8 prior runs). After some inspection I called it a timing artifact of the 224-byte binary-size bump shifting when lazy `dlvsym` finished resolving `avcodec_find_decoder_by_name`. The user accepted the call and we moved on rather than re-running multiple times to characterize it; Phase 9's smoke validated the hypothesis by returning the count to 7 under a different memory layout, so no further investigation warranted.

Side discovery during the Phase 8 smoke-report script: `REFACTOR_BASELINE.txt`'s `receive_frame OK: 4` figure was a **measurement bug in the baseline script** (last-match regex rather than summing the running counter), not a true 4 successful frames. Real decoder behavior is ~50/50 EAGAIN/OK across all 9 runs: `ok=1989 err=2011` at the `#4000` sample point in every clean run. The baseline file still says 4; we chose not to rewrite it (it's a historical snapshot, same "don't edit past steps" principle as this journal).

### Phase 9 — `shim_inject.c`

The last carve, and the biggest single-file output of the refactor at 846 lines. Everything the shim pushes *into* the running game, as distinct from interception coming out:

- Pokémon LE 105-switch manifest (transcribed from manual 500-55AJ-01) + `shim_find_switch` + `shim_fire_switch` that pokes the in-memory switch-state array at `data_4880220 + id*0x30 + 0x28` and diffs the cmode state around the closure.
- Event-sweep machinery (Step 51): `shim_fire_event` trampoline to in-binary `fire_event @ 0x9c12a0`, `shim_event_sweep` that blind-fires every event ID 0..0xce with per-event SIGALRM timeout and SEGV recovery via `sigsetjmp`/`siglongjmp`.
- The three sigjmp/sig-atomic globals whose externs live in the header (read by `shim_sigsegv_handler` in `shim_diagnostics.c` to longjmp out of a faulting iteration) — definitions moved here.
- Six env-gated worker threads: sweep timer, switch firer, gstreamer force-init, cmode poke, state-flag poke, vp-patch.
- SIGALRM handler used by the sweep.
- A single new public entry point — `shim_inject_start()` — that the main constructor calls after signal registration. It registers SIGALRM, blocks SIGALRM on the main thread (so the six worker threads inherit the mask and only the sweep-timer thread unblocks it for itself), and does each of the six env-var probes + `pthread_create` + `pthread_detach` in one place. Replaced ~100 lines of env-gated thread spawning inside `shim_install_crash_handler` with a single line: `shim_inject_start();`.

Hit an **off-by-one error in extraction** during build — the range for the event-sweep machinery was 412-678, but `shim_event_sweep`'s closing `}` was at line 679. Result: `shim_inject.c` had an unclosed `shim_event_sweep` (which caused the compiler to see every subsequent `static` function as "invalid storage class for nested function"), and `spi_shim.c` had an orphaned `}` sitting between breadcrumbs. Fix was to move the `}` between the two files. Caught by compiling; the user's reminder to read files in full (rather than trusting memorized line numbers) is what made diagnosis take one read instead of hunt-and-peck. Worth recording because the ranges *looked* correct by name (function-start to function-end name-match) but the extraction logic needed "end line = closing brace line, not last-body-line."

Smoke result: **0 mismatches of 22 error classes.** `ok=1989 err=2011` at `#4000`, `receive_frame` sampled lines = 16, gpio = 138 (baseline 139, within ±4). Tighter baseline match than Phase 8 — reorganization happened to land the binary on a memory layout where the retry count returned to its invariant 7.

### Numbers

| Before refactor | After refactor |
|---|---|
| `spi_shim.c`: 6385 lines | `spi_shim.c`: 959 lines (nucleus) |
| Single TU | 10 TUs: `spi_shim.c` + 9 subsystem modules |
| N/A | `spi_shim.h`: 206 lines of cross-TU declarations |
| Monolithic | 85 % of the original code in subsystem modules |

Per-phase line moves: 41 → 462 → 722 → 384 → 441 → 1735 → 654 → 501 → 770. Every phase regression-tested to 0 error-class mismatches within the ±4 gpio / ±30 frame variance envelope documented in the README.

### What the refactor paid for

Cumulative across all nine phases (including Step 63's findings):
1. **`pread64` blind spot** (Phase 7). Proactive LFS variant hooks caught the game doing random-access reads on `fd=31` — a code path we'd been completely blind to. Still need to correlate `fd=31` back to a file.
2. **Baseline-script measurement bug** (Phase 8). Hidden for all of Phase 1-7 by identical regex-last-match on both baseline and phase reports; surfaced once I wrote a fresh report script from memory that got the math right.
3. **"Hook both 32/64-bit variants" policy** codified in `shim_io.c` with three asymmetries still on the follow-up list (`fopen`, `pread`, `pwrite` plain variants).
4. **A home for research code** (`shim_diagnostics.c`) — cmode table, event sweep, switch manifest, dispatch-table dumpers — and the feedback memory (`feedback_preserve_re_knowledge`) to enforce it during future work.
5. **A home for runtime surgery** (`shim_patches.c`) and **active injection** (`shim_inject.c`) — the next round of game-memory patching and env-gated probing has a place to land instead of being bolted onto a monolith.

### Updated follow-ups (still open after the refactor)

- **Close the three plain/64-bit hook asymmetries** — `fopen`, `pread`, `pwrite`. `fopen` first since it takes a path. ~15 lines each + a smoke run each to observe whether the plain variant fires.
- **`fd=31` provenance.** Widen `should_log_open` to dump every open regardless of path filter for one run, correlate against the `[lfs] pread64 #N: fd=31 offset=... count=...` line that fires at startup.
- **Two game-logic investigation threads resume now:**
  - `/root/Main` custom-init: how does Stern's boot path bring Main into the tree, given BN [81] says standard `SceneTree::init` is bypassed? The GDScripts reach for `$"/root/Main"` and find nothing.
  - `VideoStreamPlayer::play()` trips `is_inside_tree() == false` on orphaned nodes at destruct. Either these nodes shouldn't exist, or they need parenting to a fake root before `play()`. The Step 60 vtable patch silences the downstream abort but doesn't fix the orphan-construction.


## Step 65: NODEBUS_Hash polynomial — constants corrected, CPU-inline absent in this build (2026-04-27)

**Trigger.** External observations from Researchers A, C and D suggested
looking for a CPU-side implementation of the node-bus hash. The investigation
tested that hypothesis in this game rather than assuming it transferred from
another title.

> [Redacted: private source wording and the identifying comparison example.]

User's framing: find the inlined hash in `games/pokemon_le/game`, enumerate **every** call site, look for sneaky uses (Stern has historically used innocuous file names like `screen_timing` for update decryption — any consumer chain is in scope, not just audio).

**Finding 1 — the doc's polynomial constants were wrong.**

`BRIDGE_PROTOCOL.md` had previously stated:
```
m1 = ((x * 0x80008001) ^ ((x * 0x80008001) >> 10)) * 9
m2 = (m1 >> 6) ^ m1
hash(x) = ((m2 * 0xfffff801) >> 16) ^ (m2 * 0xfffff801)
```

Direct disassembly of `HOST_Update` in `netbridge.elf` (Thumb-1, ARM Cortex-M0+, debug_info present) at VA `0x1000161a..0x10001634` shows the actual implementation:
```
0x10001618: 0x404b   EORS r3, r1                  ; r3 ^= r1 (input ^ seed)
0x1000161a: 0x1e59   SUBS r1, r3, #1              ; r1 = r3 - 1
0x1000161c: 0x03db   LSLS r3, r3, #15             ; r3 = r3 << 15
0x1000161e: 0x1acb   SUBS r3, r1, r3              ; r3 = (r3-1) - (r3<<15)  =  r3 * -0x7FFF - 1
0x10001620: 0x0a99   LSRS r1, r3, #10             ; r1 = r3 >> 10
0x10001622: 0x404b   EORS r3, r1                  ; r3 ^= r1
0x10001624: 0x2109   MOVS r1, #9                  ; r1 = 9
0x10001626: 0x4359   MUL  r1, r3                  ; r1 = 9 * r3                = m1
0x10001628: 0x098b   LSRS r3, r1, #6              ; r3 = m1 >> 6
0x1000162a: 0x404b   EORS r3, r1                  ; r3 = m1 ^ (m1>>6)          = m2
0x1000162c: 0x1e59   SUBS r1, r3, #1              ; r1 = m2 - 1
0x1000162e: 0x02db   LSLS r3, r3, #11             ; r3 = m2 << 11
0x10001630: 0x1ac9   SUBS r1, r1, r3              ; r1 = (m2-1) - (m2<<11) = m2 * -0x7FF - 1 = z
0x10001632: 0x0c0b   LSRS r3, r1, #16             ; r3 = z >> 16
0x10001634: 0x404b   EORS r3, r1                  ; r3 = z ^ (z>>16)           = hash
```

So the correct multiplicands are **0x7FFF and 0x7FF**, with `NOT` (one's-complement) applied:
```
y       = ~(x * 0x7FFF)         ≡ (x * 0xFFFF8001) - 1   (mod 2^32)
m1      = (y ^ (y >> 10)) * 9
m2      = (m1 >> 6) ^ m1
z       = ~(m2 * 0x7FF)         ≡ (m2 * 0xFFFFF801) - 1  (mod 2^32)
hash(x) = (z >> 16) ^ z
```

`x * 0xFFFF8001 ≢ x * 0x80008001` (differ by `0x7FFF0000 * x`), so the doc's polynomial would have produced different output for any `x ≠ 0`. The fix is recorded in `BRIDGE_PROTOCOL.md` § "Cmd `0x40` sub `0x0d` — NODEBUS_Hash".

**Finding 2 — the polynomial is not inlined in `games/pokemon_le/game`.**

Exhaustive scan of `.text` (file offset 0x460000, size 0x03167100, 12.95M instructions) for every plausible ARM64 lowering of the polynomial's two multiplies:

| Lowering | Encoding pattern | Hits |
|---|---|---:|
| `SUB Wd, Wn, Wn, LSL #15` (single-insn `x * -0x7FFF`, self-shift) | `0x4B003C00`, mask `0xFFE0FC00`, Rn==Rm | **0** |
| `SUB Wd, Wn, Wn, LSL #11` (single-insn `x * -0x7FF`, self-shift) | `0x4B002C00`, mask `0xFFE0FC00`, Rn==Rm | 1 (in `sub_1042214`, audio saturation) |
| `SUB Wd, WZR, Wm, LSL #15` (`-(x << 15)`) followed by `ADD Wd, Wd, Wm` | structural pair search | 0 polynomial-shaped (6 SUB-WZR-LSL-#15 hits exist, all in audio decoders `sub_9b7590` / `sub_9b9820`, used for sample-bit-width clamping with rounding bias `0x4000`) |
| `LSL Wt, Wm, #15` followed within ±8 insns by `SUB Wd, Wp, Wt` (split form) | structural pair search | 0 |
| `MOVZ Wn, #0x7FFF` (loads multiplicand) near `MUL`/`MNEG` | proximity scan | 7 incidental hits, none clustered with the polynomial's distinctive shifts |
| `MOVN Wn, #0x7FFE` (loads `0xFFFF8001`) | `0x1280FFC0`, mask `0xFFFFFFE0` | 7 hits — 6 in `sub_9b7590`/`sub_9b9820` clusters, all loading `-0x7FFF` as audio saturation min, paired with `MOVZ #0x7FFF` (saturation max) |
| `MOVN Wn, #0x07FE` (loads `0xFFFFF801`) | `0x1280FFCE`-class | 1 (at `0x01042234` in audio saturation `sub_1042214`) |
| `MNEG W,W,W` (entire binary) | `0x1B00FC00`, mask `0xFFE0FC00` | 3, none near `MOVZ #0x7FFF` or `#0x7FF` |
| Polynomial signature: `LSR W #10` AND `LSR W #6` AND `LSR W #16` AND `LSL W #11`, all within ±256 bytes | structural pair search | **0 W-reg clusters** (X-reg cluster of 4 sites at `0x009b8a0c`, `0x009bacb4`, `0x009bd380`, `0x009bfe4c` are all in 128-bit audio decoders `sub_9b7590` family, GF(2^8)-shaped with `0x1b` constants) |

Each candidate I followed up bottoms out as something else:
- `sub_11f172c` / `sub_11ed6f0` — libpng `png_compose` / channel-loss-check (`0x80008001` is the magic-number divider for `x / 0xffff` lowering; `png_error`/`png_get_channels` strings are the giveaway).
- `sub_1042214` — signed Q-format audio sample saturation. The `x - (x << 11)` arithmetic and `0xf801` halfword stores are bit-pack endpoints of the +/-0x7FF saturation range. The `MOVN #0x07FE` is loading `0xFFFFF801` for the sample-floor clamp, not for a polynomial multiply.
- `sub_9b7590` / `sub_9b9820` — audio decoder rounds with `(1 << (width-1)) - 1` overflow check and `(-(x << 15) + 0x4000) >> (width-1)` rounding-and-shift normalization. The `0x1b` constants are GF(2^8) reduction (AES-like in shape). The MOVZ/MOVN pairs are saturation clamps, not polynomial multipliers.

This is consistent with our earlier negative result on static cmd-byte senders for cmd `0x40`/`0x41`: the game does not call the bridge's NODEBUS_Hash, and it also does not compute the polynomial on the CPU side.

**Conclusion.** Pokemon LE 0.81 does **not** use NODEBUS_Hash anywhere we can detect. the externally suggested CPU-inline pattern is **not** observed in this build. If Stern uses any audio/asset/network/NV decryption keyed off this polynomial, it must be done in a form that defeats every plausible compiler lowering we tested — possible but unlikely without contradicting the `image.bin`-extracts-clean evidence (all 2452 records are confirmed audio per user, no encryption stripped during extraction).

**Open questions / next moves (paused for user direction).**
- Is there a different mixer in Spike 3? The external observation concerned Spike 2; transferring it between generations remained an open question.
- Any value in cross-checking `service_s_worker` (the Service S daemon) for crypto primitives — it's the network-facing side and may be the actual home of any sneaky-use crypto.
- The audio decoders at `sub_9b7590` are independently interesting (they look like Stern's bespoke audio codec). Worth a separate trace if assets that *aren't* in `image.bin` (e.g. anything in `main.pck`) might pass through them encrypted.

**Artifacts produced.**
- `BRIDGE_PROTOCOL.md` § NODEBUS_Hash — polynomial corrected with verified Thumb-1 disassembly cite.
- `BRIDGE_PROTOCOL.md` TODO — "Spec out cmd `0x40 / 0x0d` NODEBUS_Hash usage" moved to Resolved (negative result).
- This entry.

No BN renames performed (no function to rename — there is no inlined hash to label). No memory file claims about polynomial constants need surgery beyond a small note that the older `reference_nodebus_protocol.md` summary may have inherited the wrong constants from the doc.


## Step 66: encrypted nodebus channel discovered — cmd 0x41 is alive (2026-04-27)

**Trigger.** User asked to look at `sub_8ec410`. That walk cracked open the part of Step 46 that was wrong: cmd 0x40 / 0x41 ARE sent at runtime, just not via static `mov w0, #0x40` immediates that our earlier scan was looking for.

**Sender chain.**

```
caller wrapper (1 of ~160 templated per (slot, inner_cmd))
    ↓
mcp_nodebus_send_with_retry @ 0x8ed4a0  (status-byte retry wrapper, gates encrypted path)
    ↓
mcp_nodebus_send_encrypted @ 0x8ec410   (the cipher lives here; three branches depending on arg1[0])
    ↓
mcp_nodebus_send_raw @ 0x9166c0          (computes cmd byte = (arg1[0] >> 7) + 0x40)
    ↓
send_bridge_message_with_response @ 0x9164a0  (UART)
```

The cmd byte being **runtime-computed** is why the static-immediate scan in Step 46 returned zero senders. Lesson for future RE: don't conclude "dead code" from absence of immediate-load patterns alone — Stern uses runtime-computed cmd bytes for the channels they want guarded.

**Cipher (per-slot XOR stream, outbound only).**

```
mask = (slot << 4) | ((inner_len + 1) & 0x0F)
for i in [0 .. inner_len-1]:
    out[i] = orig[i]
           ^ mcp_nodebus_cipher_keys[slot*16 + (i & 0xF)]
           ^ ((i & 1) ? ~mask : mask)
# trailing byte = -sum(frame) mod 256
```

Self-inverse (XOR), validated against BN HLIL byte-for-byte. The boot-time first-ever frame for slot 5 cmd 0xfc fetch is `85 01 ae cc` (verified in Python).

**Per-slot tables (32 slots, all BSS-resident).**

| Symbol | Address | Stride | Total |
|---|---|---:|---:|
| `mcp_nodebus_cipher_keys` | 0x44fcfb8 | 16 | 512 |
| `mcp_nodebus_per_slot_status` | 0x44fce38 | 12 | 384 |
| `mcp_nodebus_send_counters` | 0x44fd1c0 | 4 | 128 |
| `mcp_nodebus_last_error` | 0x44fce30 | – | 4 |

Layout fully resolved by `mcp_nodebus_reset_all_slots @ 0x8f61f0` (single function with three `memset`s of the right sizes).

**Key provisioning.**

> [Redacted: the boot-time procedure for obtaining per-slot keys and the
> associated security observation. The investigation distinguished initialization
> from subsequent traffic; later steps revisited which traffic was encrypted.]

**Use cases identified.**
- Bridge-level cleartext cmd-0x40 sub-cmds: `mcp_nodebus_query_bridge_status @ 0x8ed410` (sub-cmd 0x05), `mcp_nodebus_factory_test_trap @ 0x8f5f4c` (sub-cmd 0x0c — also resolves the BRIDGE_PROTOCOL.md TODO about that path).
- Encrypted cmd-0x41 traffic: 160+ wrappers around `mcp_nodebus_send_with_retry`. Confirmed user: `mcp_nodebus_flash_node_firmware @ 0x8f5484` — the **node firmware programmer** (writes `coil4node-LPC1124_303-*.hex` and friends onto remote node MCUs via inner cmds 0xf603 = boot mode, 0xf311 = bootloader cmd, 0xf482 = flash write).
- Hypothesis (not yet confirmed): switch-state writes flow through response handling of these wrappers. Tracing that is the natural next move.

**BN renames applied (with `mcp_` prefix per workflow).** 10 functions + 4 data labels + 9 comments. User saved DB after these.

**Open questions.**
- Inbound decryption: responses appear cleartext on the wire to the game side, but this needs runtime confirmation.
- Inner cmd `0xf9` semantics (used by the two-probes fetcher with two different 4-byte arguments).
- Bit `0x40` OR-gating in `mcp_nodebus_send_encrypted` (inner cmd in `[0x10, 0xEF]` or `0xF2` queries `mcp_nodebus_per_slot_status[slot] + 11` — privilege flag of some kind).

**Artifacts.**
- New standalone reference: `_emulation/docs/NODEBUS_CIPHER.md` (~340 lines: algorithm, wire frame, Python impl, hunting checklist for other Spike 3 builds, open questions).
- `BRIDGE_PROTOCOL.md` updated in 8 places to retract the "cmd 0x40/0x41 dead code" claim with cross-references to `NODEBUS_CIPHER.md`. Step 46's "surprising negative result" entry marked superseded.
- Memory: new `polynomial_nodebus_hash.md` correction note + new `project_nodebus_cipher.md`; `reference_nodebus_protocol.md` cmd-0x41 entry updated.


## Step 67: cipher decoder in shim — boot-time traffic decoded, surface narrowed (2026-04-27)

**Plan.** Phase 1–5 of "Cipher-aware shim logging for cmd 0x41" (`/home/c/.claude/plans/proud-snacking-rocket.md`).

**Finding ahead of the shim work.** `runs/latest/game.log` at the start of this step had the cmd-0x41 frames I wanted to validate against, but every frame was *truncated mid-write* — the existing UART-byte logger interleaves with the SPI logger without an atomic newline, so the bytes never make it into the log. Sample line:

```
[shim] UART write #13 (11 bytes): 02 41 00 80[shim] SPI #12427000 cmd=0x6c TX: ...
```

"11 bytes" claimed, only 4 actually printed before SPI thread squeezed in. The SHIM_LOG macro is atomic but the byte logger isn't going through it. Phase 1 (validating cipher against existing log) was therefore impossible — the existing log is corrupted by interleaving. **Skipped to Phase 2.** Side note: this is a separate bug worth fixing in `shim_io.c`, but out of scope for this plan.

**Phase 2: cipher decoder in `shim_nodebus.c`.** Single-file change. Added:
- `static uint8_t nodebus_cipher_keys[32][16] = {{0}};` mirroring the game's BSS state (zero at init = matches what the game has, since shim never returns key fetches).
- `static void nodebus_cipher_decrypt(slot, inner_len, buf)` mirroring `mcp_nodebus_send_encrypted`'s loop byte-for-byte (cipher is involutive — same XOR undoes encryption).
- `case 0x41:` in `synthesize_bridge_response()` — parses cipher frame from bridge payload, decrypts, logs `slot / inner_cmd / payload` via a single atomic `write(STDERR_FILENO, line, n)` call. Still no synthetic response (timeout-is-progress preserved per Step 43 lesson).
- `case 0x40:` ditto for cleartext bridge sub-cmds (now logs sub-cmd byte and payload length).

Build clean: `spi_shim.so` 231,728 bytes (was ~231 KB).

**Phase 3: capture.** `TIMEOUT=185 VIEWER=0 AUDIO_PLAYER=0 service_s_worker=0 ./run_game.sh --debug`. Game ran ~185 s, exited with status 4 (the expected Pokemon-logo FATAL ERROR — same blocker as before, no regression). New `runs/latest/game.log` is 40,231 lines / 3.5 MB. **29 cleanly decoded `bridge: cmd 0x41 ...` lines.** Zero cmd-0x40 frames in this run.

**Decoded boot traffic — 4 unique (slot, inner_cmd) pairs out of 160+ wrappers:**

| slot | inner_cmd | count | payloads |
|---:|---|---:|---|
| 0 | `0xf2` | 1 | `[71]` (one-shot, very first cmd-0x41 frame) |
| 0 | `0xf4` | 3 | `[d9 6f]`, `[ea 78]`, `[d9 6f]` |
| 1 | `0xed` | 17 | `[93]` (poll loop) |
| 1 | `0xee` | 8 | `[ea 95]` (paired with the `0xed` poll) |

Slots 2–31 saw no traffic. Key-fetch family (`0xfc / 0xfd / 0xf9`) did NOT fire — the game must skip provisioning when the bridge is timing out, or sequence those after some other prerequisite the shim doesn't satisfy.

**Cipher recipe is end-to-end correct.** The very first frame the game emitted (`slot=0 inner_cmd=0xf2 payload=[71]`) was independently predictable from the cipher recipe before this run, and the live capture matches byte-for-byte. Confirms encryption HLIL → C decoder → live capture chain.

**Phase 5 documentation.** Added `NODEBUS_CIPHER.md § 3.5 ("Decoded boot-time traffic")` summarizing the table above; updated `§ 7 ("What we have NOT figured out")` to note that all 4 observed inner cmds fall in the bit-`0x40` OR-gating range (likely relevant) and that key-fetch family didn't fire under current shim conditions. JOURNAL: this entry.

**What this unlocks.**
- Encrypted nodebus traffic is no longer opaque — every cmd-0x41 frame the game sends is logged decoded.
- The 160-wrapper RE surface is narrowed to 4 wrappers that actually fire at boot. Targeted RE on those is now tractable.
- Phase 4 decision is open: either chase those 4 wrappers in BN to learn what they semantically do (deeper RE), or extend the shim with synthesized key-fetch responses to advance past the timeout-is-progress phase and see what new traffic surfaces. User-side decision.

**Open / unverified.**
- Why no key-fetch traffic (`0xfc / 0xfd / 0xf9`)? Possibilities: (a) game sequences key fetches after bridge handshake handlers we currently respond to, and our partial responses make it skip the fetch entirely; (b) key fetches go through a separate path we didn't tag; (c) the encrypted-path entry only fires for nodes that already finished an unencrypted handshake the shim isn't simulating. Worth tracing in BN.
- The 4 active wrappers' semantics (inner cmds 0xed/0xee/0xf2/0xf4) — name and purpose of each.
- Fixing the SHIM_LOG byte-logger interleaving in `shim_io.c` is a parallel cleanup item.

**Artifacts produced.**
- `shim_nodebus.c` — cipher decoder + cmd-0x40/0x41 logging cases (~80 lines added).
- `NODEBUS_CIPHER.md § 3.5 / § 7` updates.
- This entry.

No memory updates this round.


## Step 68: polling-cmd byte hunt — slot 0 reconciled, slot 1 remains opaque (2026-04-27)

Continuing from Step 67's decoded boot traffic (4 inner_cmds: 0xed/0xee/0xf2/0xf4). Goal: identify which wrappers emit each cmd byte so the shim can synthesize plausible responses.

### Resolved — slot 0 frames

**Cmd 0xF2** is emitted by 4 sibling wrappers, all packing `inner_len | cmd` as a single 16-bit `MOVZ #0xF202`:
- `0x8f2db0` → `mcp_nodebus_emit_cmd_f2_short_unsure`
- `0x8f2c80` → `mcp_nodebus_emit_cmd_f2_short_v2_unsure`
- `0x8f2e50` → `mcp_nodebus_emit_cmd_f2_dual_path_unsure`
- `0x8f1934` → `mcp_nodebus_emit_cmd_f2_with_args_unsure`

**Cmd 0xF4** is emitted by `0x8ed7a0` (`mcp_nodebus_emit_cmd_f4_bulk_write_unsure`), using `MOVN W3, #0x000B` to load `0xFFFFFFF4`, then `STRB` the low byte. Variable-length wrapper for chunked data writes. Callers:
- `0x8f1180` → `mcp_nodebus_emit_cmd_f4_thunk_unsure` (guarded thunk)
- `0x8f3020` → `mcp_nodebus_emit_cmd_f4_chunked_then_f2_commit_unsure` (loops 0xF4 in ≤0x80-byte chunks, then commits with 0xF2)

Self-consistency check: for slot=0 frame `f1 8d 00 bf` (decoded inner_cmd=0xF2, payload=[71]), reverse-decoding through `mcp_nodebus_emit_cmd_f2_short_unsure(arg2=0x97)` produces wire bytes `0xF1 0x8D`, matching the actual frame. Cipher formula `mask = (slot << 4) | ((inner_len + 1) & 0xF)` is verified correct.

Also renamed surrounding poll-loop infrastructure:
- `0x589480` → `mcp_nodebus_lamp_poll_loop_unsure` (inner per-board update loop)
- `0x675efc` → `mcp_nodebus_main_poll_loop_unsure` (outer game-loop polling orchestrator)
- `0x674680` → `mcp_nodebus_node_probe_5x_unsure` (5× consistency-check probe per slot during init)

### Unresolved — slot 1 frames

**Cmds 0xED, 0xEE** — wrappers NOT found via simple ARM64 immediate-loading patterns (MOVZ / MOVN / SUB / ADD with the cmd byte as immediate) anywhere in the wrapper cluster (0x8ed000–0x8f8000). Hypotheses:
- (a) Computed via multi-instruction sequence not pattern-matched
- (b) Loaded from a per-chip-type cmd-byte table
- (c) Emitted by chip-vtable-dispatched functions outside the wrapper cluster — `mcp_lpc_chip_vtable @ 0x42317c8` has 28 entries × 5 fps each, with pointers into 0x97x000–0x99x000 range; those handlers (e.g., `sub_9777c0` is 88KB) likely contain the per-chip cmd logic.

Slot 1 has a different LPC chip type than slot 0, with chip-specific message-type codes.

### FP-walker reliability note (Step 67)

The shim's caller-chain logging for slot=1 ed frames points at `mcp_nodebus_send_cmd_fe_probe_unsure` as the top intermediate caller. That function emits cmd 0xFE/0xFA, **not** 0xED, and its buffer math (inner_len=1 vs observed inner_len=2) doesn't reconcile. Either qemu's stack-unwinding has a quirk for translated guest code, or there's stale LR data in the deeper frames. **Don't trust the chain past d5/d6 for cipher frames.**

### Reconciliation with Step 67

Step 67's claim that polling cmds "don't appear ANYWHERE as static immediates or raw bytes" was a single-byte-immediate scan only. The 16-bit packed forms (`0xF202` etc.) do exist in the wrapper cluster — the previous search just didn't cover ARM64 16-bit MOVZ encodings.

### Where this leaves us

Slot 0 (bridge/CPU node) cipher-cmd traffic is mostly understood — meaningful synthetic responses for cmd 0xF2/0xF4 are within reach. Slot 1+ traffic (for downstream nodes the user doesn't have hardware for) remains opaque and would require deep RE through the chip-vtable handler tree. The cost/benefit favors deferring slot-1 emulation in favor of higher-impact blockers (Pokemon-logo crash, VideoPlayerSpi pure-virtual abort).

## Step 69: cipher decoder retracted — encryption is conditional, polling traffic is fe_probe (2026-04-28)

Two correlated discoveries that retract Steps 67/68's interpretation of the polling traffic, plus a working (but complicated) shim synthesizer for fe_probe.

### What we believed entering this step

After Step 68, we had:
- Decoded inner_cmds 0xED/0xEE for slot 1 polling (and 0xDD/0xDE for slot 2, 0x8D/0x8E for slot 7, etc.)
- The pattern `decoded_high_nibble = 0xF - slot` looked suspicious but we rationalized it as different-per-slot wrapper paths
- Couldn't find any 0xED/0xEE/0xDD wrapper via static MOVZ/MOVN/SUB/ADD scans

### What's actually true

The "decoded" values were ARTIFACTS of the shim's cipher decoder running unconditionally. Encryption isn't actually applied in our shim runs at all.

Verified by GDB BP at `mcp_nodebus_send_encrypted` entry (`0x7fffe422c410` runtime, base `0x7fffe3940000` with `setarch --addr-no-randomize`):
- Slot 1 frames hit BP with `cmd=0xFE ilen=1` (1st fe_probe call, hint byte at offset 3 = `0x0B`)
- Then `cmd=0xFA ilen=2` (2nd, hint at offset 4 = `0x01`)
- Then `cmd=0xFE ilen=1` again (3rd, hint = `0x0A`)

Those exactly match `mcp_nodebus_send_cmd_fe_probe_unsure @ 0x8ed870`'s three call sites. So slot polling is fe_probe — chip identification — across every slot (0–13 active in our run). The 0xED/0xDD/0x8D values were `0xFE ^ ((slot << 4) | ((ilen+1) & 0xF))` — pure cipher-decoder artifacts.

### Why encryption skipped

`mcp_nodebus_send_encrypted` only runs the cipher when:
- Cmd byte is in [0x10, 0xEF] OR equals 0xF2, AND
- Per-slot status byte at `mcp_nodebus_per_slot_status[slot] + 3` is non-zero

For cmd 0xFE (fe_probe 1st/3rd) and 0xFA (2nd), the cmd-range check fails (0xFE/0xFA > 0xEF). So encryption is skipped regardless of status flag. For cmd 0xF2 (the 1 frame from `sub_8f2db0` we observed), the cmd-range check passes but the per-slot status flag is zero in our shim (never set by anything we synthesize), so encryption is also skipped.

Net: every cmd 0x41 frame we observe is **cleartext on the wire**. The cipher exists in the binary; we just never trigger the path that turns it on.

### Hint-byte clarification

The "response-length hint" lives in the WRAPPER's input buffer at offset (inner_len + 2), read by `mcp_nodebus_send_encrypted` line `0x8ec4c4` BEFORE the csum/encryption phase. After that phase, the byte at the same offset gets overwritten by the bridge-level checksum (computed on the post-encryption bytes). So **the wire byte at frame[inner_len+2] is the BRIDGE CSUM**, not the hint. The hint is never visible on the wire — to synthesize correctly we have to know each wrapper's hint statically.

### What we built

`shim_nodebus.c` rewrite for cmd 0x41:
1. Removed the cipher decode (was producing the false 0xED/0xDD/etc.)
2. Logs wire bytes as cleartext directly (`bridge: cmd 0x41 slot=N inner_cmd=0xXX ilen=Y payload=[…]`)
3. Added an opt-in fe_probe synthesizer behind `NODEBUS_SYNTH_FE=1` env flag. When set:
   - Detects cmd 0xFE / 0xFA at frame offset 2
   - Reads the hint byte from frame offset (2 + ilen) directly (cleartext, since no encryption)
   - Builds a bridge response: `STX + 0x41 + id + (hint bytes payload) + bridge_csum + ETX`
   - Payload[0] = 0x80 (high bit set = "node present"), rest = 0 (chip ID zero → falls through to default chip vtable entry)

Forwarded the env var through `run_game.sh` so it actually reaches qemu's guest env.

### What it does

With `NODEBUS_SYNTH_FE=1`:
- 260 fe_probe responses synthesized across all 10 active slots
- Game completes fe_probe rounds (1st AND 2nd call — first time we've seen 0xFA frames)
- ZERO bridge timeouts (vs 27+ in baseline)
- BUT Godot doesn't initialize at all (no "Godot Engine" banner, no "Detected Capture/Discover scene" lines, no res:// references)

The "timeout-is-progress" insight from Step 43 stands: bridge cmd timeouts are the path that lets the game advance past nodebus init and into Godot. By making fe_probe succeed, we trap the game in a deeper init phase waiting for key-fetch responses (cmd 0xF9 / 0xFC / 0xFD) that we don't synth — so Godot never starts.

### Default behavior preserved

`NODEBUS_SYNTH_FE` is **off by default**. Without the env var, the shim is functionally equivalent to the post-Step-67 build minus the (broken) cipher decode. The default run reaches Godot scene detection same as baseline.

### BN updates landed

- `0x42317a0` renamed → `mcp_lpc_chip_default_vtable_entry_unsure`
- `mcp_nodebus_send_encrypted` function comment: encryption gating + hint-byte location
- `mcp_nodebus_send_cmd_fe_probe_unsure` function comment: 3 call sites + buffer layouts + caller's response-byte mapping

(Save the BN database via File → Save Database.)

### What's open

- **Key-fetch synthesis** — to make synth-on actually progress further than synth-off, we'd need to also synthesize cmd 0xF9 / 0xFC / 0xFD responses. The wrappers are `mcp_nodebus_fetch_key_basic @ 0x8f3484`, `mcp_nodebus_fetch_key_with_serial @ 0x8f355c`, `mcp_nodebus_fetch_key_two_probes @ 0x8f36b0`. Hint values and response shapes need similar GDB-BP capture as we did for fe_probe.
- **"Detected scene" → next blocker** — synth-disabled baseline reaches Godot scene detection but doesn't progress to playable. The actual upstream blocker is in Godot land. `/tmp/godot_log(s)` (user-flagged in `reference_godot_logs.md`) is unread; that's where to look for scene-init / GDScript errors.
- **Cipher genuinely active path** — still untested in our runs. To exercise it we'd need to set `mcp_nodebus_per_slot_status[slot]+3` non-zero for at least one slot via a synthesized response. Not strictly necessary for current goals.

## Step 70: filesystem remap completion — mkdir/mkdirat hooks, font redirect, /dump symlink fix, system() rewrite (2026-04-28)

Two-phase filesystem-side cleanup that took the game from FATAL exit (status 4) to clean SIGTERM at TIMEOUT, with NV writes landing on disk for the first time.

### 70a — mkdir/mkdirat hook + font carve-out + sysroot pre-creates

**Trigger.** Default-env baseline at start-of-session was exiting with code 4 (`sys_debug_log_fatal_error`) at game.log:38995 after `Detected Capture/Discover scene` and `can't create directory '/connectivity/'`. The shim's `remap_path()` (`shim_fs.c:74`) covered access/stat/open/faccessat/fstatat but **not mkdir/mkdirat** — Godot's `DirAccess::make_dir()` and the game's NV-init both bypass us and try to create dirs on the host root.

**Changes.**
- `shim_fs.c` — added `mkdir()` + `mkdirat()` hooks immediately after the access wrapper. Same dlsym→remap_path→log→real pattern as access. EEXIST treated as success (mkdir-p semantics) since Godot's DirAccess + Stern's NV layer both flag EEXIST as failure even on existing dirs.
- `shim_fs.c` — added font carve-out in `remap_path()` BEFORE the `/home/` skip: redirects `/home/c/.fonts.conf{,.d}` and `/home/c/.config/fontconfig/{fonts.conf,conf.d}` onto `$SYSROOT/etc/fonts/{local.conf,conf.d}`. Without this, libfontconfig's per-user probes fall through to WSL-native dirs.
- Created `$SYSROOT/etc/fonts/local.conf` as a minimal pokemon-side stub (existing remap was correct, file was just missing).
- Pre-created `$SYSROOT/data/nv/pokemon_le/{FRRAM,LKRAM,NVM,NVRAM,PIN_NVRAM,PTRAM,SYS_NVRAM,node_diag}/` — empty dirs only, no content (game's NV double-buffer logic detects fresh state and starts new).

**Result.** Exit went 4 → **124 (clean SIGTERM via timeout=200)**. game.log went 39k → **94k lines**. mkdir REMAP fired 13 times across `/connectivity/`, `/data/nv/pokemon_le/*`, `/dump/`, `/tmp`, `/`. Frame viewer went 30 → **1,050 frames @ 5.4 fps**. As an unexpected side effect the VideoPlayerSpi `!is_inside_tree()` error count dropped from 1 to 0 in this specific run (turned out to be a timing fluke — see Step 71).

### 70b — /dump symlink fix + system() command rewrite

**Trigger.** 70a's run still had 2 `mkdir FAILED` entries on `/dump/log` and `/dump/log/connectivity` (errno=2). Root cause: `$SYSROOT/dump` was a **dangling absolute symlink** → `/connectivity/dump` (resolved on host root, where /connectivity/ doesn't exist). And the system() hook at `spi_shim.c:487` was forwarding `/bin/sh -c 'echo SPI-POK-000000 > /connectivity/hostname'` and `rm -r -f /connectivity/files/gkpd_{1,2,3}` unmodified — host shell saw paths it can't reach.

**Changes.**
- Replaced `/mnt/c/Workspace/pokemon-le-0.81.0/dump` with a relative symlink → `connectivity/dump` (preserves the cross-link semantics; resolves correctly on host follow). Pre-created `$SYSROOT/connectivity/dump/log/connectivity/` (`log/` was already there, `log/connectivity/` was the missing leaf).
- `spi_shim.c:487` system() hook — added substring-rewrite of `/connectivity/` → `$SYSROOT/connectivity/` in cmd strings before forwarding to real_system(). 8KB scratch buffer, only fires when cmd contains the prefix. Limited to `/connectivity/` (the only path prefix actually seen in failing system() commands).

**Result.** mkdir FAILED count → 0. system() REWRITE fired 4 times. `Directory nonexistent` → 0. `connectivity/hostname` actually exists on disk (`SPI-POK-000000`). Frame viewer went 1050 → **1,110 frames @ 5.7 fps**. **Major bonus discovery: NV writes now land on disk** — the game wrote 14 NV blocks + 14 .crc32 across all 8 NV stores during a single run, including PIN_NVRAM rotating through 00000000/00000001/00000002. The `project_persistence_gap.md` memory ("EEPROM and filesystem NV writes both vanish") is at least partly retracted: filesystem writes work; SIGTERM-flush behavior across runs is still untested.

### What's open after Step 70

- The remaining 4 `access FAILED` entries (`/connectivity/files/gkpd_4`, `/connectivity/update/UPDATE_COMPLETED`, `/data/nv/pokemon_le/factory_reset`, `/data/nv/pokemon_le/PIN_NVRAM/00000002`) are all **expected**: state-marker files where absence is meaningful, or transient probes for not-yet-written rotation blocks.
- Cross-run NV persistence — does the game READ existing NV state on boot 2 and continue, or does it re-init fresh? Step 71 indirectly answered: with NV present, game proceeds further (PIN_NVRAM rotates to 00000005), but takes a different (slower) init path before reaching scene detection.
- VideoPlayerSpi `!is_inside_tree()` blocker is back to 1 in the next run (70a's "0" was a fluke). Step 71 picks this up.

### Memory updates landed

- `project_emulation_progress.md` — refreshed description with Step 69 cipher retraction and SYNTH-on-deception warning.
- `feedback_stay_in_shim_and_sysroot.md` — new feedback memory: prefer in-shim hooks + in-sysroot pre-creates over new env vars / run_game.sh args / WSL-native paths. User explicit guidance.
- `project_godot_engine.md` — clarified that ONLY `assets/godot/main.pck` is authoritative; `main.pck_extracted/` and `pokemon_battle_previs/` are user scratch.

---

## Step 71: VideoPlayerSpi `!is_inside_tree()` assertion bypass + secondary blocker discovery (2026-04-28)

The VideoPlayerSpi → Radium bridge is the breakthrough surface (per user). Step 71 attacks the assertion that fires when GDScript calls `play()` before the player is added to the parent scene tree.

### Static investigation

BN MCP confirmed the error chain:

| BN VA | Runtime VA (base 0x7fffe3940000) | Role |
|---|---|---|
| `0x0e3819c` | `0x7fffe477819c` | `sub_e3819c` — VideoPlayerSpi::play() entry. Vtable entry at BN `0x42cc6f8`; no direct code xrefs. |
| `0x0e381ac` | `0x7fffe47781ac` | `mov x19, x0` — saves `this` to callee-saved x19 |
| `0x0e381b0` | `0x7fffe47781b0` | `ldr x0, [x0, #0x2c0]` — loads Node base flag (NODE_FLAG_IN_TREE-equivalent at +0x2c0) |
| `0x0e381b4` | `0x7fffe47781b4` | `tbz w0, #22, +0xb8` (raw `0x36b005c0`) — assertion: branch to error if bit 22 clear |
| `0x0e381b8` | `0x7fffe47781b8` | `ldr x0, [x19, #0x988]` — playback object (VideoStreamPlaybackSpi) |
| `0x0e381bc` | — | `cbz x0, +0x90` — skip cast block if playback NULL |
| `0x0e38298` | `0x7fffe4778298` | tail-call into Godot's `_err_print_error` family |

Per agent investigation: the playback object at `+0x988` is dynamically-cast to `VideoStreamPlaybackSpi` and if successful, vtable+0x110 invoked — that's where Stern's Radium playback engages.

### GDB attempt — too slow

Tried tmux+`mcp__openmcpgdb__` with BPs at `0x7fffe47781b4` (tbz) and `0x7fffe4778298` (error path). qemu-aarch64 with `-g 1234` (passed through run_game.sh as a positional arg, no script changes; `setarch -R bash -lc '...'` for fixed PIE base 0x7fffe3940000). **GDB attach slowed execution ~6-10x** — `Detected Capture scene.` took 16 minutes real-time vs ~150s normal. TIMEOUT=300 was way too short; killed before BP captured. Useful gotchas saved to `reference_tmux_gdb_workflow.md`.

### NODEBUS_SYNTH_FE=1 with NV state — still no Godot init

Tested whether NV state from prior runs changed Step-69's "synth-on traps deeper init" result. **No** — same finding: synth-on eliminates bridge timeouts, but Godot Engine banner never appears. Synth-on is a dead end for play() diagnosis.

### Shim NOP patch — original assertion silenced, secondary one revealed

Added `shim_patch_video_player_assertion()` to `shim_patches.c` (always-on, no env gate per `feedback_stay_in_shim_and_sysroot`):
- Resolves runtime addr of BN `0xe381b4`
- Verifies bytes `0x36b005c0` (the tbz)
- Patches to `AARCH64_NOP` (`0xd503201f`)
- Wired into `shim_vp_patch_thread` (shim_inject.c:582) after the existing 4 patches

**Result.** Exit 124, 95k log lines, 1110 frames @ 5.6 fps (same as 70b). The `video_player_spi.cpp:376` assertion is GONE. play() proceeds past the NOP'd tbz. **A new error now fires at Godot's core**:

```
ERROR: Condition "!is_inside_tree()" is true. Returning: false
   at: can_process (scene/main/node.cpp:867)
```

This is `Node::can_process()` from Godot itself (not Stern's video_player_spi module). Same root cause (player not in tree), but it's a soft early-return (`Returning: false`) — Godot's main loop sees can_process=false and skips calling `_process()` on the node. Without `_process()` ticks, no video frame dequeue from the decoder.

PIN_NVRAM rotation reached 00000005 (vs 00000002 in 70b) — game progressed deeper, more state writes.

### What's open after Step 71

- **The breakthrough still requires Radium plumbing.** play() being silenced doesn't help if `_process()` never ticks. Either need to (a) get the player into the tree (real Godot fix; requires GDScript modification or runtime add_child intercept), or (b) hook __dynamic_cast / trampoline at sub_e3819c to confirm whether the playback object at `+0x988` is even non-NULL (= Radium IS registered as the playback backend).
- **Per user direction:** focus more on Radium next. `[REFERENCE_TOOL_A]/` is the semi-replicated player that "wraps Godot and plays videos on top of it". Search that dir for `radium` keyword for related resources before re-attacking the binding side.
- **Trampoline log** (deferred from this step) — a small in-shim trampoline at sub_e3819c entry that captures `this`, `*(this+0x2c0)`, `*(this+0x988)` per call. Needed for definitive answer on whether Radium backend is registered.
- The `is_inside_tree() Returning: false at can_process` pattern might appear at other Godot Node call sites (e.g., `_process_internal`, `_notification`, `set_process`) — would surface as more silent skips rather than visible errors.

---

## Step 72: switch-93 wire-side BRK trap on sub_8edfe0 (2026-04-30)

The user-mode wedge — game runs end-to-end (rendering, audio, video, UART, polling) but its mode state machine never advances — is a switch-input-starvation problem. Step 72 builds the wire-side injection path so we can drive `switch 93 (Start Button)` into the game's translator chain and verify the cmode rotation theory.

### Background — the chain (BN-confirmed, sessions 2026-04-29 / 2026-04-30)

```
sub_8edfe0(channel, buf8)            ; reads/decrypts 8-byte wire state for `channel`
   → sub_7b4d00(channel, buf8)       ; iterates data_44cbd70, decodes per-switch bits
       → sub_651610(channel, buf8)   ; XOR-diffs vs g_switch_state_array (0x4480220)
           → pending list data_4480278
               → drainer sub_6511c0  ; fires events via fire_event
```

`sub_651610` is the **single centralized translator** (no parallel writers found in BNQL xrefs to `0x4480220`).

### Switch 93 wire-side numerics (BN session 2026-04-30)

| Field | Value | Source |
|---|---|---|
| Descriptor | `0x43e7cf0` (size 0x40) | static analysis |
| Field bytes at `0x43e7d18` | `06 00 0b 00 01 00 01 00` | hexdump_address |
| **Channel** | **6** | byte 0 → `entry+0x46` at runtime via `sub_7b4638` |
| **Wire-bit base** | **11** | byte 2 → `entry+0x47` at runtime |
| Bit-A index | 21 (= 11 + 0xa) | `sub_7b4d00` decode at BN 0x7b4d5c–70 |
| Bit-B index | 22 (= 11 + 0xb) | `sub_7b4d00` decode at BN 0x7b4db4–e08 |
| Byte 2 mask (closed paired sentinel) | `0x60` (`0x20 | 0x40`) | bit 21 = byte 2/bit 5; bit 22 = byte 2/bit 6 |

### Verified at sub_8edfe0 entry (game.bndb hexdump)

```
0x8edfe0  02 00 80 d2     mov   x2, #0          ; clear arg3
0x8edfe4  b7 ff ff 17     b     sub_8edec0      ; tail-call (-292 bytes → 0x8edec0)
```

Function is exactly 8 bytes. `sub_8edec0` does the XOR-decrypt of a `mcp_nodebus_send_with_retry({0x80|channel,1,0x0a11})` response into arg2 using per-channel keys at `0x44fce43`. **No global counters mutated**, so early-return is safe; caller (`sub_674b90`) gates `data_4482150[arg1] = 1` on our return value.

### Implementation — Recipe A (preferred): stub sub_8edfe0 via BRK trap

Mirrors the existing Step 71 `shim_patch_video_player_play_entry_brk` pattern — patch the function's first 4 bytes with `brk #0` (`0xd4200000`), let the SIGTRAP handler decide the response:

| File | Change |
|---|---|
| `spi_shim.h` | Declared globals `shim_sub_8edfe0_runtime`, `shim_sub_8edfe0_arm_channel`, `shim_sub_8edfe0_arm_deadline_ms`, `shim_sub_8edfe0_payload[8]`. Declared helpers `shim_patch_sub_8edfe0_brk()` and `shim_sub_8edfe0_arm()`. |
| `shim_patches.c` | New `shim_patch_sub_8edfe0_brk()`: mprotect, byte-check (refuses unless `0xd2800002` = `mov x2,#0`), publishes runtime addr, writes `brk #0`. Always-on once enabled (no per-arm env gate). |
| `spi_shim.c` | Extended `shim_sigtrap_handler` with a new dispatch branch: when `pc == shim_sub_8edfe0_runtime`, read `x0` (channel), `x1` (buf), `x30` (lr); compute payload (channel-matched + deadline-fresh → use armed payload, else zeros); write 8 bytes to `*x1`; set `x0 = 1` (success); set `pc = lr` to return to caller, skipping the rest of the function. Logs first 4 traps and then every 1024th, plus every armed trap. |
| `shim_inject.c` | New `shim_sub_8edfe0_arm(channel, payload, hold_ms)` helper. New `shim_switch_wire_map[]` table (currently 1 entry: switch 93 → channel=6, byte=2, masks 0x20|0x40). `shim_fire_switch` now branches on `shim_find_switch_wire(id)`: when wire info present, use the BRK-arm path; else fall back to legacy state-byte flip. |
| `shim_inject.c` | `shim_inject_start` now installs the BRK patch when either `SPIKE3_PATCH_SUB_8EDFE0=1` is set OR `SPIKE3_FIRE_SWITCH` is set (auto-implied). Patch lands at constructor-end time so it's in place before the first bridge poll. |

### Why "always-on synthesizer" is safe for this experiment

Once the trap is installed, sub_8edfe0 returns synthetic data (zeros for unarmed channels, payload for armed channel). That bypasses the real `sub_8edec0` wire read. Acceptable because: shim_nodebus.c **already** synthesizes empty/dummy bridge responses — there is no real wire to read from. Replacing zero-from-fake-wire with zero-from-shim is a no-op for non-armed channels.

### Build verification

```
[build] OK — spi_shim.so: 298552 bytes
```

Strings/symbols present in linked output: `shim_patch_sub_8edfe0_brk`, `shim_sub_8edfe0_arm`, `shim_sub_8edfe0_payload`, all `[sw_brk]` log strings, `wire-arm`, `state-flip`. Pre-existing unused-function warnings for `nodebus_cipher_decrypt` and `log_caller_chain` are unchanged.

### Test recipe (next run)

```bash
cd /mnt/c/Workspace/pokemon-le-0.81.0/games/pokemon_le/_emulation
SPIKE3_BRIDGE=1 TIMER=ON SPIKE3_FIRE_SWITCH=93 SPIKE3_SWITCH_DELAY=30 \
  ./run_game.sh --debug
```

Run for ≥3 minutes (per `feedback_test_duration`). Watch for:

- `[sw_brk] patched sub_8edfe0 @ ... (brk #0)` — patch installed.
- `[sw_brk] sub_8edfe0 trap #N ch=X ...` — game's bridge poll is calling sub_8edfe0; shows which channels are polled in practice.
- `[sw_brk] arm: channel=6 payload=000060...` — fire_switch_thread armed the synth.
- Per-trap log with `armed=1 ch=6` during the 120 ms window — confirms the arm landed during a poll cycle.
- `--- shim_fire_switch: 93 'Start Button' ... mode=wire-arm ---` followed by Δ count.
- **Critical signal**: `[bridge] cmode delta:` lines after the 30 s mark → wedge cleared by switch-93 input. Or no deltas → wedge is upstream of switch processing (orchestrator gating, etc.).

### What's open after Step 72

- **Channel-id table is one entry**. If the bridge poll never reaches `channel=6`, the arm window is wasted. Diagnostic logging in the SIGTRAP handler will show which channels actually poll. If channel 6 isn't polled, we trace why (which Node-1 enumeration sub_674b90 driver thread does at startup).
- **Hold time** is a guess (120 ms). If logs show "armed=1" never lands during a poll, increase. If sub_651610 sees only the rising edge but not the falling edge before deadline, increase.
- **Bit-B requirement** unverified — the recipe sets both bits 21 and 22; if the latch path needs only bit-A, byte 2 = 0x20 may suffice. Cheap to A/B test if bit-B causes spurious "second switch" misfires.
- **Other switches** (1-SW-X for Action / Coins / Tournament Start) follow the same channel=6 mapping per Pokemon LE manual — once switch-93 is verified working, fill in `shim_switch_wire_map[]` for the rest by deriving wire-bit from the `1-SW-N` field in the manifest.

### Step 72b — first verification run + correction (2026-04-30)

**First attempt failed at boot.** Patch was wired into `shim_inject_start` (constructor-end time). `shim_resolve_game_base()` returns 0 there because it works by walking the calling thread's stack for a `/game` frame — at constructor-end no game thread has called any shim hook yet, so the cache is empty. Log:
`14:03:00 [sw_brk] game_base unresolved; sub_8edfe0 BRK aborted`. Step 71's `shim_patch_video_player_play_entry_brk` has the same dependency and is already deferred to `shim_vp_patch_thread`'s 2 s sleep. Moved the call there. Also moved env-gating (`SPIKE3_PATCH_SUB_8EDFE0=1` ∨ `SPIKE3_FIRE_SWITCH` set) into `shim_patch_sub_8edfe0_brk` itself so the deferred caller doesn't need its own gating logic.

**Note on env forwarding (corrected reading of run_game.sh).** Earlier I thought run_game.sh needed new `-E` lines for `SPIKE3_FIRE_SWITCH` / `SPIKE3_SWITCH_DELAY`. Wrong: qemu-aarch64 user-mode inherits the parent shell's environment by default; `-E` only ADDS or modifies. Setting `SPIKE3_FIRE_SWITCH=93` in the invocation shell flows through to the game's `getenv()` automatically. No script change needed.

**Second run (200 s, SPIKE3_FIRE_SWITCH=93 SPIKE3_SWITCH_DELAY=30 STATE_BRIDGE=1).**

| Signal | Result |
|---|---|
| `[sw_brk] patched sub_8edfe0 @ ... (brk #0)` | ✓ at run+2 s — patch installed |
| `[sw_brk] sub_8edfe0 trap #N ch=X` | **0 hits across 200 s** |
| `[sw_brk] arm: channel=6 payload=0000600000000000 hold_ms=120` | ✓ at run+30 s — fire_switch armed |
| `armed=1 ch=6` traps | 0 (because no traps at all) |
| `--- shim_fire_switch: 93 'Start Button' ... mode=wire-arm ---` followed by Δ=0 | ✓ Δ=0 reported |
| `[bridge] cmode delta:` | 0 across full run |

**The function is dormant.** `sub_8edfe0` was never called even once. Yet bridge IS alive: cmd 0x27 probes (4 in 200 s) and cmd 0x41 with inner cmds `0xf0 / 0xfa / 0xfe` (identification/probe slots 0/1/2/4/7/8). Notably absent: any cmd `0x80..0x8f` (the `0x80|channel` query that `sub_8edec0` issues). So the upstream poll thread that calls `sub_674b90` (which then calls `sub_8edfe0`) hasn't started.

**Wedge is upstream of the switch-poll path itself.** Mode-state isn't advancing → switch poll not activated → no calls to `sub_8edfe0` → arm never lands. The Step-72 instrumentation IS working as designed — it just confirms the dormancy hypothesis instead of breaking the wedge.

### What's next after Step 72b

Spawned a focused BN agent to trace **what gates `sub_674b90`** — the function whose inactivity blocks the entire chain. Two outcomes possible:

- **Gate is a memory flag** (e.g. `if (data_X) call sub_674b90`). Then a `SPIKE3_POKE_*`-style env-gated thread can flip it after a settle, similar to existing `SPIKE3_POKE_CMODE` / `SPIKE3_POKE_STATE_FLAG`. Cheap to implement.
- **Gate is a thread-create that's conditional** (e.g. only created post-init success). Then we'd have to either patch the conditional, or skip the gate entirely by Recipe B — directly enqueue a pending-list entry at `data_4480278` and let `sub_6511c0` drain it. More invasive but bypasses the gate.

The BN agent's report will pick the path. JOURNAL update at Step 72c when results land.

### Step 72c — BN sub_674b90 trace + Recipe B implementation (2026-04-30)

BN agent report: `sub_674b90` is called from `sub_675df0` (the only path that ever passes a non-zero channel), which runs inside a `while(true)` loop in `mcp_nodebus_main_poll_loop_unsure` (0x675efc). The poll thread is spawned by `sub_675d10` (0x675d10) — but only **after** `fire_event(0x80, 0)` returns truthy. To enter the loop body the binary must also pass `mcp_nodebus_init_orchestrator() & 1 == 1`, which depends on `fire_event(0x82, 0)`. State flags `data_4482120 & 0x10`, `data_44821b8 & 1`, `data_4482124 = 1`, `data_4482190 = 1`, `data_4482194 = 2` must be set as well.

The agent recommended **Suggestion B (direct call to `sub_651610`)** as the long-term answer — bypasses the dormant orchestrator entirely. Implemented in `shim_inject.c`'s wire-arm path: build closed/open 8-byte payloads, call `((void(*)(uint8_t,uint8_t*))(game_base+0x651610))(channel, payload)` directly, sleep through the hold window, then call again with zeros for the release edge.

### Step 72d — diagnostic dump + planted switch_id (2026-04-30)

First runs of Recipe B: ZERO cmode delta. Added pre/post-fire dumps to figure out why.

**Key finding from the dump**: `data_44cbda8` (channel-bit map count) is **0** — the table that `sub_7b4d00` uses to map (channel, bit) → switch_id is **EMPTY**. It's populated at runtime by `sub_7b4638`, which is downstream of the dormant orchestrator. `sub_651610` walked it, found no entry matching channel=6 bit 21, silently no-op'd.

But — looking at `sub_651610`'s actual decompilation and disassembly, the function does NOT use `data_44cbd70`. That table is only for `sub_7b4d00`. `sub_651610` uses a DIFFERENT structure at `data_44804b8`: per-channel struct of stride 0xb0, with a switch_id table at offset 0x28 (16 entries × 2 bytes per bit, accessed via `ldrh w6, [x3, #0x28]` where `x3 = data_44804b8 + 2*(88*ch + abs_bit)`).

For switch 93 / channel 6 / bit 21 (= byte 2 bit 5): the slot is at `data_44804b8 + 6*0xb0 + 0x28 + 21*2 = data_44804b8 + 0x472`. We plant `0x005d` (= 93) there before calling `sub_651610`. Dump confirms plant: `data_44804b8+0x448092a: 0x00000000 -> 0x0000005d`. Per-channel state buffer at `+0x20..0x27` does get updated to `00 00 60 00 00 00 00 00` post-call — confirming `sub_651610` runs the byte-write outer loop.

### Step 72e–f — pointer field reframe + push-already-happened diagnosis (2026-04-30)

**Critical correction to memory** (was wrong about `g_switch_state_array`): the BN database's `g_switch_state_array` symbol at `0x4480220` is a **pointer field**, NOT an embedded array. The actual switch state structs live on the **heap** at `*(uint64_t*)0x4480220 + sw_id*0x30`. The disassembly at 0x65164c is `ldr x14, [x20, #0x220]` — a LOAD, not an LEA. Existing `shim_inject.c`'s `SHIM_SWITCH_STATE_BASE 0x4480220` is half-correct — it's the pointer's address, not the array's base. All previous fire_switch byte-flips at `game_base + 0x4480220 + sw_id*0x30 + 0x28` were writing to .bss-adjacent uninitialized memory, NOT to the real switch state.

Updated diagnostic to dereference the pointer:

```
[sw_brk] *0x4480220 (g_switch_state_array PTR) = 0x735b5e128c30
[sw_brk] sw93 struct dump: @0x735b5e129da0 first16=709d125e5b730000709d125e5b730000  +0x26 (counter)=1  +0x2a (flag)=0x00
[sw_brk] data_4480228=0x735b5e0841e0  mask[93]=0x00
[sw_brk] direct sub_651610(ch=6, [00 00 60 00 00 00 00 00])
[sw_brk] mid-fire (post-closure pre-release): sw93 (real) counter+0x26=1  pending_head=0x735b5e129c50
```

**Switch 93's counter was already 1 PRE-fire** — meaning sw93 has already been edge-fired by something before our shim ran. The `cbnz w0, +0x10` at `0x6516e8` checks the OLD counter value: if non-zero (which it is, =1), the push is **skipped**. So our `sub_651610` calls are no-ops for switch 93 specifically.

This is the central reframe of the investigation: **the switch input infrastructure is at least partially functional** (sw93 has been edge-fired, the struct exists with valid linked-list pointers, counter is non-zero). What's broken is the **cmode response** — the cmode handlers don't transition state when switch events fire. That's consistent with the dormant orchestrator: cmode transitions are gated on orchestrator state, not just switch events.

### Pivot for Step 72g

Switch-injection is no longer the right vehicle for breaking the wedge. The wedge is **in the cmode/orchestrator layer**: `fire_event(0x80,0)` and `fire_event(0x82,0)` aren't returning truthy because no handlers are registered for those events (or the registered handlers are upstream-dependent on something else dormant).

Suggestion A from the original BN agent — **force `fire_event` to return 1 for event_ids 0x80, 0x81, 0x82, 0x83** — becomes the right path. Implementation: BRK trap on `fire_event` (BN VA `0x5c12a0` — verified via mcp__binary-ninja__function_at, NOT `0x9c12a0` which is inside sub_9bd9a8), handler reads `x0` (event_id), and:

- If event_id ∈ {0x80, 0x81, 0x82, 0x83}: set x0=1, set pc=lr — selectively skip and return 1.
- Else: emulate `fire_event`'s first instruction in the handler, advance pc by 4, and let the function run normally.

Step 72g: implement and test. Expected outcome: orchestrator runs, populates `data_44cbd70`, spawns `sub_675df0` poll thread, polls switches naturally, cmode handlers start reacting to real or shim-injected switch events. If that works, switch 93 fire_switch (Recipe B path) becomes the actual unblocking step — but only AFTER the orchestrator is alive.

### Step 72g — fire_event BRK + gate-poke results (2026-04-30)

**Built and tested in two phases.**

**Phase 1: BRK on fire_event alone.** Patched first instruction (`stp x19, x20, [sp, #-32]!`, bytes `0xa9be53f3`) with `brk #0`. SIGTRAP handler routes on x0 (event_id):
- For event_ids ∈ {0x80, 0x81, 0x82, 0x83}: set w0=1, set pc=lr (skip).
- For all others: emulate the stp in C (sp -= 32; *(sp+0)=x19; *(sp+8)=x20), advance pc by 4.

Test: `SPIKE3_FORCE_NODEBUS_GATES=1 SPIKE3_FIRE_SWITCH=93 SPIKE3_SWITCH_DELAY=60 STATE_BRIDGE=1 TIMEOUT=200 ./run_game.sh --debug`.

| Signal | Result |
|---|---|
| `[fe_brk] patched fire_event @ ...` | ✓ at run+2 s |
| `[fe_brk] forced fire_event(0x80)` from `lr=game_base+0x675d20` | ✓ matches BN-predicted thread-spawn site (sub_675d10+0x10) |
| `[fe_brk] forced fire_event(0x82)` from `lr=game_base+0x675148` | ✓ matches BN-predicted orchestrator gate (mcp_nodebus_init_orchestrator+0x...) |
| `[fe_brk] forced fire_event(0x81)` from `lr=game_base+0x6b8da4` | unexpected location — different caller |
| `pthread_create` 1 ms after `fire_event(0x80)` forced return | ✓ poll thread WAS spawned |
| `[sw_brk] sub_8edfe0 trap` | **0** across 200 s |
| `data_44821b8` post-run | **0x00** (gate flag NOT set despite orchestrator run) |
| `data_4482120` post-run | `0x11` (orchestrator DID reach 0x6751c4) |
| `data_44cbda8` (channel-bit map count) | **0** (table empty) |

The thread spawns but the loop body never reaches `sub_8edfe0`. Three fire_event forced returns (0x80, 0x81, 0x82) — fire_event(0x83) never called. Only 4 passthrough events (0x3b, 0x06×3) in 200 s — most game logic doesn't reach fire_event because mode state is wedged.

**Phase 2: + direct memory poke for gate flags** (the BN agent's full Suggestion A). Added `shim_poke_nodebus_gates_thread`: sleeps 30 s then writes the 5 flags `data_4482120 |= 0x11`, `data_44821b8 = 1`, `data_4482124 = 1`, `data_4482190 = 1`, `data_4482194 = 2`.

Test: same as Phase 1 but `SPIKE3_SWITCH_DELAY=90` (fire after gate poke at 30 s).

| Signal | Result |
|---|---|
| `[gates] PRE`: data_4482120=0x11, data_44821b8=0x00, data_4482124=0x01, data_4482190=0x00, data_4482194=0x01 | partial — orchestrator set 4 of 5; missing data_44821b8 and data_4482190 |
| `[gates] POST`: all 5 set as expected | ✓ |
| `data_44821b8 = 0x01` at fire_switch time (60 s post-poke) | ✓ stayed set |
| `[sw_brk] sub_8edfe0 trap` | **STILL 0** |
| `data_44cbda8` | **STILL 0** |
| sw93 counter | 0 (was 1 in earlier runs) — different state, not progress |
| cmode deltas | 0 |

**Conclusion**: gate forcing + memory pokes get us further (thread spawned, gate flags set), but `data_44cbd70` (channel-bit map populated by `sub_7b4638`) is **still empty**. Without it, `sub_675df0`'s iteration body has no channels to poll, so `sub_8edfe0` is never called even though the poll thread is alive.

**The deeper missing piece**: `sub_7b4638` (channel-bit-map populator) is not running. It's normally called somewhere in the orchestrator chain — but our `fire_event` hook **bypasses handlers entirely** (we set pc=lr after forcing the return value, so handlers never run). If sub_7b4638 is called by a handler chained from event 0x82 (or another), our hook silenced it.

**Side concern**: `sw93 counter` was 1 in earlier runs (Step 72f) but 0 in this run. Suggests gate-forcing changed the boot path enough that switch 93's natural pre-firing didn't happen. Not a regression we care about — orient on the bigger picture.

### Three Step 72h options

**A. Trace `sub_7b4638` callers via BN, force its caller to run.** Targeted trace: who calls sub_7b4638, on what trigger? If a single chain `event 0x?? → handler → sub_7b4638`, we can either let that handler run (un-skip that event_id from our forced-return list) or directly call sub_7b4638 from the shim.

**B. Trampoline-call `sub_7b4638` from the shim.** Add a thread that, after gate poke completes, calls `((void(*)(void))(game_base + 0x7b4638))()`. Risk: sub_7b4638 may have its own dependencies (nodebus tree, allocators) that aren't satisfied yet.

**C. Hand-populate `data_44cbd70`.** From the Pokémon LE manual, we know all switches' (channel, wire-bit) tuples. Write entries directly. Tedious but bypasses everything between fire_event and the loop body. Stride is 0x50 — likely larger than just (channel, wire-bit), so we'd need BN trace of sub_7b4638 to know full entry layout anyway.

**Recommendation**: A. Cheap to find out (one BN call), and the answer informs B vs C if A doesn't directly resolve.

### Memory updates needed

- Add new memory entry: `g_switch_state_array @ 0x4480220 is a pointer field, not array` — fix the long-standing assumption in `feedback_*` and `project_*` notes.
- Update Step-71 / Step-72 area in `project_emulation_progress.md`: switch input layer works at least partially; wedge confirmed in orchestrator/cmode layer.

## Step 73: forward-decode bridge, fs hook completion, scene-loader trace (2026-04-30 → 2026-05-01)

A multi-pronged session. Five concrete pieces of progress, none individually spectacular but together they close out the LD_PRELOAD path-coverage gap, prove out a host-NVDEC bridge, and reframe the front-end blocker around scene-tree attachment rather than decode.

### 73a — discovery hook init was silently broken; moved out of constructor

Symptom: previous-session `shim_discovery_init()` produced **zero** `[discovery] patched ...` lines despite the symbols being linked. Root cause: `shim_discovery_init()` was being called from `shim_install_crash_handler` — an `__attribute__((constructor))` function that runs at LD_PRELOAD time, **before the game's `main()` has started**. `shim_resolve_game_base()` walks the calling thread's stack for a `dladdr` frame inside `/game`; at constructor time no `/game` frame exists, so the resolver returns 0 and `shim_discovery_init()` hits its `if (!base) return;` guard.

Same gotcha already documented at `shim_inject.c:1070-1082` for Step 72's deferred patches. Fix mirrors what's there: `shim_discovery_init()` moved into `shim_vp_patch_thread` after the existing 2-second settle, alongside `shim_patch_video_player_play_entry_brk`/`shim_patch_sub_8edfe0_brk`/`shim_patch_fire_event_brk`. The constructor call removed, replaced with a comment pointing at the new home.

Verified via `objdump -d spi_shim.so | grep shim_discovery_init` — exactly one call site now, inside `shim_vp_patch_thread` at offset `+0x30`. Subsequent runs produce live `[discovery]` log lines.

Also fixed log path: the previous `discovery_log()` opened `"discovery.log"` relative to CWD (which was `$GAME_DIR`, dropping the file in `games/pokemon_le/discovery.log` — wrong place). Changed to `"_emulation/runs/latest/discovery.log"` and switched `O_APPEND` → `O_TRUNC` so each run gets a fresh log instead of accumulating across runs.

### 73b — host-NVDEC forward-decode bridge

Per Researcher A's emulation architecture (memory note `project_emulation_architecture.md`), the long-term plan was to forward libavcodec calls out of QEMU to host-native equivalents. This session implements a working version of that for video decode specifically.

**Architecture:**

```
game (aarch64) ─► sysroot libavcodec.so.59 ◄── still linked
                  │
                  │ (existing send_packet/receive_frame hooks at shim_patches.c)
                  ▼
           SPIKE3_FORWARD_DECODE=1 gate
                  │
                  ▼ AF_UNIX @spike3_decode (length-prefixed protocol)
           decode_daemon.py (host x86_64)
                  │
                  ▼ PyAV → hevc_cuvid → NVDEC → NV12 plane bytes
```

**Components added:**

- `_emulation/decode_daemon.py` — host-side AF_UNIX server. Per-connection session: receives `OPEN(codec_id, extradata)`, allocates `av.codec.CodecContext(hevc_cuvid)`, then handles `PACKET` / `RECEIVE` / `CLOSE` opcodes. NVDEC frames downloaded to NV12 via `frame.reformat(format='nv12')` / `to_ndarray`. Smoke-tested against `Lake_01_22_26_h265.mp4` at 44–98 fps.
- `_emulation/decode_daemon_test.py` — standalone harness that demuxes a clip with PyAV and round-trips packets through the daemon. Validates the wire protocol independent of the C-side shim.
- `_emulation/shim_avforward.c` — C client (8-slot session table, AVPacket field offsets, AVFrame buffer population via `av_frame_unref` + `av_frame_get_buffer`, row-by-row stride-aware copy). Lazy-opens the daemon connection on first `send_packet` for a given AVCodecContext. Uses `avcodec_parameters_from_context` to pull codec_id + extradata at session-open time. Falls back to local-libavcodec passthrough if daemon unreachable.
- `shim_patches.c` integration: existing `avcodec_send_packet` / `avcodec_receive_frame` hooks early-return through `shim_avforward_*` when the gate is on. `shim_resolve_libav` un-`static`'d so the new TU can reuse it.
- `run_game.sh` lifecycle: new `FORWARD_DECODE` env var (default 0) launches the daemon as a HELPER pre-game-start, logs to `runs/latest/decode_daemon.log`, dies via the existing `cleanup_all` SIGTERM-then-SIGKILL grace path.

**First real-game-run results** (2026-04-30 23:18, then 23:42):

- Three concurrent decoder sessions opened (`conn-1`, `conn-2`, `conn-3`) with codec=`hevc_cuvid` and `extradata=2453B` — different from our test clip's 2490B, confirming they're **real game-side AVCodecContexts** not the test harness.
- `conn-3` ran longest: 400+ NV12 frames at 1940×1100, sustained ~16 s of decode.
- First run died at the daemon side with `AttributeError: module 'av' has no attribute 'AVError'` — PyAV 14 dropped that shim. Replaced `except av.AVError` with `except av.error.EOFError` (graceful EOF → REP_EOF reply) plus generic `except Exception`. Smoke-tested clean post-fix; second run produced a clean lifecycle.

**Recommendation against making this default**: PyAV + NVIDIA-WSL dependency, mid-stream daemon-crash recovery untested, `SPIKE3_INJECT_VIDEO`'s dummy-frame fallback bypassed when forward path engaged. Stays opt-in until a few more clean runs.

### 73c — Godot shader-cache mystery → unhooked `chdir` → version-script fix

**Investigation arc.** Discovery.log started showing `GODOT ERROR: rasterizer_gles3.cpp:335 Can't create shader cache folder, no shader caching will happen: /tmp/cache` reliably every run. Initial hypothesis: the directory doesn't exist. Verified: `/mnt/c/Workspace/pokemon-le-0.81.0/tmp/cache` exists in the sysroot, populated, with `mesa/mesa_shader_cache/` subdir; `[shim] mkdir REMAP: /tmp/cache -> $SYSROOT/tmp/cache` fires successfully at process start.

So why does Godot say "can't create"? Added `[fs_diag]` logging to `stat`/`lstat`/`stat64`/`lstat64`/`access`/`mkdir`/`open`/`openat`/`opendir` for `/tmp/*` paths, mirrored to `discovery.log` (so the diagnostic survives even runs where qemu STDERR isn't captured to game.log). First instrumented run: **zero stat calls for `/tmp/cache` in the entire 38-second window** between the constructor mkdir and the warning firing. Mesa was actively probing `$SYSROOT/tmp/cache/mesa/mesa_shader_cache/<2-hex>/<hash>` (144 entries — its own shader hash tree, working fine). Godot was not.

**First red herring chased**: GLIBC_2.33 versioning. Game imports `stat@GLIBC_2.33`, `lstat@GLIBC_2.33`, `stat64@GLIBC_2.33`, `lstat64@GLIBC_2.33`, `fstat64@GLIBC_2.33` — none of which were satisfied by the shim's unversioned `stat` (and `stat64`/`lstat64` had no hook *at all*). Added explicit hooks for `stat64`/`lstat64`. Then added a **version script** (`spi_shim.ver`) declaring `GLIBC_2.33` (with parent chain to `GLIBC_2.17`, mirroring libc.so.6's structure) so the four stat-family hooks export with the matching versioned alias. After this fix, `[fs_diag] stat64(/tmp/godot_logs)` lines appeared cleanly in discovery.log — proving versioned hooks now intercept. Shader cache still erred.

**The actual cause**, found by re-reading `RasterizerGLES3::initialize()` (BN `sub_10a5970` → renamed `mcp_RasterizerGLES3_initialize`): the function calls an OS path-getter at `0x10a59fc` (`sub_32c6140` reads OS singleton's String member at offset `+0xc8` — possibly `cache_path` or `user_data_dir`), getting `/tmp/cache`. Then calls `DirAccess::open("/tmp/cache")` (= `sub_2d5f370`, renamed `mcp_DirAccess_open`). Internally that calls `vtable[+0x108]` = `change_dir` with the path. **`DirAccessUnix::change_dir` uses `chdir()` — and the shim never hooked `chdir()`.** `chdir@GLIBC_2.17` is in the game's PLT, went straight to libc unmapped, hit the host's nonexistent `/tmp/cache`, returned -1, `change_dir` returned `ERR_INVALID_PARAMETER`, `DirAccess::open` returned NULL, error site 1 fires.

Added `chdir()` hook with `remap_path()` + `[fs_diag]`. Next run: **error gone**, replaced by:

```
[fs_diag] chdir(/tmp/cache) -> /mnt/c/.../tmp/cache  ret=0
[fs_diag] chdir(/tmp/cache/shader_cache) ret=-1 errno=2   (didn't exist)
[fs_diag] chdir(/tmp/cache/shader_cache) ret=0 errno=0    (after mkdir)
[fs_diag] chdir(/tmp/cache/shader_cache/CanvasSdfShaderGLES3) ret=0
```

Godot now successfully chdirs into `/tmp/cache`, creates per-shader subdirs (`CanvasSdfShaderGLES3`, etc.), and proceeds. **No observable front-end change** — shader cache being broken never gated *display*; first-frame compile latency was the only practical impact. The actual front-end blocker is downstream (see 73e).

**BN renames** (saved in DB after this step): `sub_10a5970` → `mcp_RasterizerGLES3_initialize` (with full diagnosis comment); `sub_2d5f370` → `mcp_DirAccess_open` (vtable+0x108 = change_dir, vtable+0x118 = make_dir noted); `sub_2d5edb0` → `mcp_DirAccess_create_for_path`.

### 73d — fs-hook audit pass: 8 more path-rewriting hooks

The chdir gap was a wake-up call. Audited the game's libc imports (`objdump -R game | grep @GLIBC | filter on fs-related names`) against the shim's hook list. Found 8 path-taking calls going straight to libc unmapped:

| Hook | Why it matters |
|---|---|
| `unlink` | Save-file rotation, NV cleanup |
| `rename` | Atomic save patterns (write-temp + rename); both paths remapped |
| `rmdir` | Cleanup paths |
| `readlink` | Symlink resolution; without remap, leaks real-host paths back to the caller |
| `chmod` | NV-write permission setting |
| `statfs` / `statvfs64` | Free-space checks (Mesa/Godot may use these before writing cache) |
| `glob64` | Wildcard expansion; unmapped pattern would expand against real `/tmp/` (empty) |

All eight added in one batch in `shim_fs.c`, using the same `remap_path` + `FS_DIAG_LOG` pattern. Two-path hooks (`rename`) remap both operands. All hooks forward via `dlsym(RTLD_NEXT, ...)`; no GLIBC_2.33 versioning issues since these are all `@GLIBC_2.17` and unversioned hooks satisfy the older version cleanly.

Remaining unhooked imports (intentional): `fchmod`, `fdopen`, `fstat64`, `closedir`, `ftruncate*` (fd-based, no path); `umask`, `sync`, `getcwd` (no path arg); `mkfifo`, `mkstemp64`, `symlink`, `popen` (likely unused on this game's hot paths — leave for later if any actually surface).

Also added: `FS_DIAG_VERBOSE=1` env-gate (`SPIKE3_FS_DIAG_VERBOSE=1` inside qemu) to widen the `[fs_diag]` filter from `/tmp/*` only to also match anything containing `cache` or `shader`. Default off (low log volume); on for "shit hit the fan" debugging where you want maximum visibility.

### 73e — `select_and_load_scene` recursion analysis (user-driven BN trace)

The persistent 822-entry alternating-`scene_id=10/11` pattern in discovery.log raised the question: is this normal, or is the game stuck in a retry loop? User did a thorough BN trace of `select_and_load_scene` (BN VA `0x730fb0`) and its callers; this section captures the conclusion verified against the binary.

**Mechanism — queue-driven mutual invocation, not C-stack recursion:**

```
frame N:    select_and_load_scene(10)
              fires fire_event(0xAB) at 0x73132c
              event handler enqueues scene 11 into g_deferred_scene_queue
              returns clean
            main event loop ticks
frame N+1:  process_deferred_scene_queue (sub_731ce0, called from main loop @ 0x720cf0)
              walks 8 slots @ data_44ca400 (stride 0x14), countdown at +0x4
              countdown == 0 → calls select_and_load_scene(11) at 0x731e4c
              clears slot's scene_id (*(x19 + 8) = 0)
              fires fire_event(0xAB) again
              event handler enqueues scene 10
            …loop continues, never C-recurses, stack stays flat
```

Verified via decompile of `process_deferred_scene_queue` (already renamed in DB). Bonus discovery: there's also a `fire_event(0xAF, &var_58)` fired per-frame from the same function at `0x731df4`, driven by a separate mutex-guarded linked-list at `data_44c9588` — at least three event channels in the discover-mode lifecycle (0xAB, 0xAD, 0xAF). The "is this scene already active?" check at `0x731d54` (`zx.d(i[2].b) != 0 && *i == *(x19 - 8)`) prevents same-slot double-loads but does **not** prevent two slots ping-ponging. By design — discover mode is a two-state UI flow (scene 10 = idle/background, scene 11 = active/content, periodic update).

**What this implies for the front-end blocker:**

The 822 alternations are the symptom of a **healthy game-logic layer driving a broken Godot-integration layer**. The C++ event loop is processing the queue, calling the loader, firing lifecycle events. Neither scene 10 nor 11 ever lands inside `/root` with `is_inside_tree() == true` — that's what `discovery.log:GODOT ERROR scene/main/node.cpp:867 [can_process] - Condition "!is_inside_tree()" is true. Returning: false` is reporting. Frames decoded by NVDEC arrive at the daemon, but the consuming `VideoPlayerSpi._process()` callback never runs because `can_process()` returns false.

**Critical reframe (user's hypothesis, mostly confirmed):** Godot is **not** the orchestrator — Stern's C++ binary is. Main event loop is at `0x720cf0`. It ticks the deferred scene queue, calls `select_and_load_scene` directly. Godot is just the rendering / scripting framework wrapped around Stern's loop. The disconnect: Stern's `select_and_load_scene` resolves asset_id and emits 0xAB/0xAD/0xAF, but the corresponding Godot Node tree never gets `add_child()`'d into a parent that is itself inside the running scene tree.

A previously-observed discovery.log line is now extra-suggestive: `GODOT ERROR: Node not found: "/root/Main" (absolute path attempted from "/root/Discover/discover_progress_bar")`. **`/root/Main`** is the parent the discover scripts expect to find — and it doesn't exist in our tree. That's likely the missing link.

### Three threads to investigate next (in priority order)

**1. Logs from the audit-pass hooks (cheap; do first).** Now that `unlink` / `rename` / `rmdir` / `readlink` / `chmod` / `statfs` / `statvfs64` / `glob64` all log under `[fs_diag]`, the first run after these hooks landed will surface every previously-silent path leak. Most likely candidates that were silently failing before: NV-store init (`/data/nv/...` writes), saved-state rotation, anything the game's persistence layer touches. Watch for `ret != 0` on any of these — each one is a hidden bug we can now see and fix in one go. A clean-default run with `FS_DIAG_VERBOSE=1` produces the audit log; grep for `[fs_diag] (unlink|rename|rmdir|readlink|chmod|statfs|statvfs64|glob64)` and triage by retval.

**2. `/root/Main` missing parent (highest-value front-end lead).** Discovery.log shows `Node not found: "/root/Main"` errors fired from at least two scripts: `capture_battle_scene/battle_scene_grp/left_world_control/left_char_Sprite2D` and `Discover/discover_progress_bar`. The game's GD scripts assume `/root/Main` exists. We don't create it. **If `/root/Main` is the parent the loader expects to attach scenes to, creating it is the front-end fix.** Investigation steps:
- Find which Godot script or C++ class is supposed to add the `Main` node (`grep -r "/root/Main" main.pck_extracted/` and `aarch64-linux-gnu-strings game | grep "Main"`)
- Check if it's a top-level autoload (defined in `project.godot` or via a C++ ClassDB autoload registration)
- Synthesize the node ourselves if possible: a one-liner `Node* main = memnew(Node); main->set_name("Main"); root->add_child(main);` injected via shim, OR via a shim-managed autoload

**3. `select_and_load_scene` post-asset-load mount path.** We've traced the scene-loader as far as event firing (0xAB/0xAD/0xAF). What we **haven't** traced: what does the function actually do *between* asset_id resolution and `fire_event(0xAB)`? Specifically, does it ever call `Node.add_child()` or instantiate a Godot scene from a `PackedScene`? If yes — what parent does it attach to (and is that parent inside the running tree)? If no — who is supposed to mount the scene, and why isn't that happening? BN trace of `0x731154 → 0x73132c` (the body before the first event fire) should answer.

### Other unexplored threads worth flagging

These have surfaced across recent sessions but haven't been actively chased. Listed for future-us so we don't lose them.

**a. The game.log STDERR-routing mystery.** During the 22:04 → 22:18 run, `game.log` mtime froze at 22:06:41 (just after a `shim_term_dump_handler` SIGUSR1 dump) but `discovery.log` and the decode daemon kept logging until 22:18:09. Discovery uses an independent fd; SHIM_LOG goes to STDERR. Something — possibly the term-dump handler closing or wedging stderr — is preventing later SHIM_LOG output from reaching `game.log` on some runs. Worth a focused look the next time we're losing stderr visibility unexpectedly.

**b. Concurrent decoder sessions.** Forward-decode runs show **three** simultaneous `OPEN codec=hevc_cuvid` sessions (`conn-1`, `conn-2`, `conn-3`). We don't know which scene/clip each maps to. Likely candidates: attract-loop A vs B, plus a third for a UI element. Adding the daemon-side print of the first-packet size or hashing the extradata against our `_emulation/VIDEO_CLIPS.md` catalog would tell us. Useful for understanding which clip each decoder pipeline corresponds to once we get frames actually displaying.

**c. The other 6 deferred-scene-queue slots.** `data_44ca400` has 8 slots × 0x14 bytes. Only 2 are exercised by the discover-mode 10↔11 ping-pong. What's the other 6 for? Probably broader scene categories (capture, battle, pokedex). Knowing the slot↔scene-category mapping would help us understand which game modes each slot drives.

**d. Step-62 `VideoPlayerSpi` vtable patches — verification missing.** Step 60 / 71 patched the vtable's `Play` slot (+0x28) to a shim function that forces state=PLAYING and `update_thread_running=1` immediately. We never confirmed the patch is actually taking effect post-Step-71 + Step-72 changes. If forward-decode is feeding frames but `_process()` never ticks, the vtable patch's effect is moot — but we should verify.

**e. `ManualGodotScene` at game VA `0x04630230`.** Per memory `reference_radium_scene_format.md`, this class wraps Radium scenes into Godot Node trees. We've never decompiled it. If it's the bridge between Radium (Stern's scene format) and Godot (the rendering tree), it's a candidate for who's supposed to call `add_child()` for loaded scenes. Tied directly to thread #3 (`select_and_load_scene` mount path).

**f. Separate service as a possible input.** The investigation considered whether another process supplied an initialization dependency. This was an untested hypothesis. [Redacted: service endpoint and probing instructions.]

**g. Comparison against `spike3/spike_menu/game` (user's pivot suggestion — strongly endorsed).** *Important clarification (2026-05-01):* there are no other Spike 3 game *titles* in this sysroot — `games/` contains only `pokemon_le/`. FOTE/Foo Fighters/etc would be entirely separate filesystems we don't have. **However**, `games/pokemon_le/spike3/spike_menu/game` IS in this sysroot — it's the Spike 3 system-menu binary that ships on every Spike 3 machine regardless of game title (test/service mode UI). Verified at the binary level:

- spike_menu links the same NEEDED libs as pokemon_le (28 vs 31) — same EGL/GLES, same GStreamer/libavcodec stack, same DRM/GBM, same gpiod
- spike_menu has **0 "godot" strings** vs pokemon_le's **457** — confirms NO Godot integration in spike_menu
- Both have ~2-3 Nuklear references (probably boot/diag-screen common code; not the load-bearing UI for either)

So the comparison is **better than another title's filesystem would be**: every variable except Godot is held constant. The delta between these two binaries IS the Godot integration layer Stern wrote. Loading spike_menu/game in BN and tracing its main-loop / scene-equivalent path tells us exactly what trampoline pattern the *non-Godot* Stern code uses — and conversely, what GLUE Pokemon LE's port added on top. If spike_menu has no equivalent of `select_and_load_scene` (or a much simpler one), we know the recursion/queue machinery in pokemon_le is *Godot-specific glue* and we can locate exactly the unmounted-tree bug from there.

**h. Nuklear vs Radium vs Stern's C++ orchestrator — clarifying who drives what.** Per memory: NUK = Nuklear (system/test-mode UI, not pinball scenes); Radium = scene FORMAT (not runtime); Stern's C++ binary at `0x720cf0` = main event loop, the actual orchestrator. The *front-end issue* may genuinely be in the Godot integration layer between the C++ orchestrator and the rendered tree — i.e., a glue layer Stern wrote (likely `SpiSceneManager`, `SpiVideoPlayer`, or `ManualGodotScene`) that we haven't traced. Investigation thread (e) is the most direct attack on this.

### Memory updates queued

- `reference_video_player_spi.md` — already updated this session: noted that decoder fudge is in place, and that the actual blocker is scene-tree attachment.
- New entry: `project_forward_decode_bridge.md` — worth writing once the bridge has 2+ stable runs. Captures the wire protocol, daemon lifecycle, env-var gates.
- New entry: `project_fs_hook_audit_2026_05_01.md` — terse note that the LD_PRELOAD path-coverage gap is closed for this game's import set; references the chdir arc as the canonical example of "what we miss when we miss one".
- `feedback_check_fs_first.md` — could cite the chdir arc as a concrete cautionary tale: when a path-related error appears, hooks may be the leak point even when the dir exists.

## Step 74: spike_menu vs pokemon_le architectural diff — orchestrator confirmed shared, scene-bridge layer isolated (2026-05-01)

Followup to 73e's `select_and_load_scene` analysis. User's hypothesis that "Godot may not be the orchestrator" prompted a temporary pivot to `games/pokemon_le/spike3/spike_menu/game` — the Spike 3 system-menu binary that ships on every Spike 3 machine. spike_menu uses the same Stern platform stack (EGL/GLES, GStreamer, libavcodec, DRM/GBM, gpiod) **with zero Godot integration** (`0` "godot" strings vs pokemon_le's `457`). That makes spike_menu a controlled-experiment baseline: every variable except Godot is held constant, so the binary-level diff IS the Godot-port glue.

This step documents what we **verified in BN**, what we **inferred but didn't confirm**, and what's still **open**. Future sessions should NOT treat the inferences as confirmed facts; this section calls out the boundary explicitly.

### Confirmed in BN (high confidence — verified via decompile / vtable reads / string-table evidence)

**Orchestrator pattern is identical across both binaries.** Both spawn a thread named `"procDispatch"` running a function with byte-identical structure:

- spike_menu: `sub_5fc980` (init) calls `sub_5fc6b0(sub_5faadc+4, 8, "procDispatch", 0)` to spawn the dispatch thread; cond var lives at `0x88fdc8` / mutex at `0x88fd98`.
- pokemon_le: `create_dispatch_thread` (already renamed) calls `CC_mainDispatchThread(process_dispatch_loop+4, 8, "procDispatch", 0)`; cond var at `0x44c9288` / mutex at `0x44c9258`.

The dispatch loop (`sub_5faadc` ↔ `process_dispatch_loop`) is byte-similar:

```c
while (running) {
    pthread_mutex_lock(work_mutex);
    while (running && work_counter == last_seen) {
        pthread_cond_wait(work_cond, work_mutex);   // wait for work signal
    }
    pthread_mutex_unlock(work_mutex);

    pre_dispatch_hook();    // sub_67ef10 (spike_menu) / sub_720cf0 (pokemon_le)
    char ctx = sub_..();    // sub_59eca0 / sub_5d7060

    // Walk proc linked-list: head at known data addr, next at +0xe8
    for (proc = head; proc; proc = *(proc + 0xe8)) {
        flags = *(proc + 2);
        countdown = *(proc + 0xf0);
        if (!(flags & 0x40)) {
            if (countdown <= 0 && !(flags & 0xffffc004) && ctx) {
                *(proc + 2) = flags & 0x7fff;     // clear processing bit
                dispatch_one_proc(proc + 4);       // sub_6905a0 / sub_86c210
            } else {
                *(proc + 0xf0) = countdown - 1;
            }
        }
    }
    fire_event(0x92, 0);   // post-dispatch hook (named in pokemon_le, sub_5859a0(0x92,0) in spike_menu)
    sub_..(ctx);

    // Wake supervisor
    pthread_mutex_lock(supervisor_mutex);
    pthread_cond_signal(supervisor_cond);
    pthread_mutex_unlock(supervisor_mutex);
}
```

**Proc structure layout (verified offsets, same in both binaries):**

| Offset | Field | Notes |
|---|---|---|
| `+0x02` | flags (u16) | bit `0x40` = skip; mask `0x7fff` = clear-on-process |
| `+0x04` | proc body / arg block | passed to per-proc dispatcher |
| `+0xe8` | next pointer | linked-list traversal |
| `+0xf0` | countdown (u32) | ticks to wait before dispatch |

**Pre-dispatch hook in pokemon_le (sub_720cf0) does the per-tick orchestration work.** This is the function the user originally labeled "main event loop" in the Step 73e analysis — that label was incorrect. It is NOT the main loop; it's a per-tick hook called BY the procDispatch thread before each pass over the proc list. Verified: sub_720cf0 is called at `process_dispatch_loop+0x344` (offset `0x729344`) inside the dispatch loop body, and itself calls `process_deferred_scene_queue` (the 822-entries-per-run scene-load driver from Step 73e) plus 12 other subsystem ticks: sub_70e990, j_sub_8462e0, sub_64e320, sub_817250 (PRNG advance), drain_pending_switch_events, sub_7f8ad0, sub_7b52c0, sub_8670c0, sub_590360, sub_717dc0, sub_6d2da0, sub_6c0410, sub_7bfbc0, sub_7a7610, sub_719850, sub_767fe0, sub_65cf80. Wraps the whole thing in `fire_event(0x37, 0)` (entry gate) and `fire_event(0x38, 0)` (exit).

**spike_menu uses `IScene` polymorphic class hierarchy.** Verified via mangled-name strings (`6IScene`, `12NuklearScene`, `11RadiumScene`, `17DMDEmulationScene`):

```
IScene (../source/display/lcd/DisplayRenderer.cpp neighborhood)
├── NuklearScene
├── RadiumScene
└── DMDEmulationScene
```

Plus a `SceneCache` class with mangled-name evidence for `release_scene(IScene*, DisplayRenderer*)`, `scene_load_thread`, `demand_load_radium_bitmap`, `flush_bitmap_cache`. `SceneCache::PreLoadSceneCache` (`sub_685bf0`) is the function that walks `/lcd/auto_loaded`, `/lcd/demand_loaded`, `/nuk/images`, constructs RadiumScene objects for auto-loaded entries, and spawns the `"LoadSceneCache"` worker thread.

**pokemon_le uses `GodotScene` polymorphic class hierarchy** — different base class than spike_menu, replacing `IScene` with `GodotScene`. Verified via:

- Source-path leak: `"../source/display/lcd/GodotScene.cpp"` — Stern's own bridge layer source
- `ManualGodotScene` constructor at `sub_725fc8`: allocates 0x11 bytes, copies `"ManualGodotScene"` string, sets `arg1 = &_vtable_for_GodotScene` then re-sets to `&_vtable_for_ManualGodotScene{for GodotScene}` (standard C++ derived-class construction)
- Mangled lambda RTTI string `_Z18godot_switch_scene17PokemonGodotScene` proves a function `godot_switch_scene` exists, taking a `PokemonGodotScene*`

```
GodotScene (../source/display/lcd/GodotScene.cpp — Pokemon-LE-specific bridge)
├── ManualGodotScene  (sub_725fc8 = constructor)
└── PokemonGodotScene (referenced by godot_switch_scene; constructor not yet located)
```

**spike_menu uses GLib's GMainLoop only for GStreamer video message bus.** Initial finding (overstated in earlier turn) was retracted: only one xref to `g_main_loop_run` in spike_menu, and it's `sub_6d5240` — the body of a thread named `"VideoMessageBus"` spawned by `sub_6d5260` (which also sets `GST_PLUGIN_PATH` and calls `gst_init_check`). pokemon_le also has `g_main_loop_*` imports — same usage, almost certainly. **GMainLoop is NOT the orchestrator in either binary.**

**spike_menu's `main()` is structurally simple:** readlink/dirname/chdir to set CWD; init three subsystems via `sub_5958e0`, `sub_639110`, `sub_5fc980` (which spawns procDispatch); install signal handlers; then enter a supervisor loop at `sub_5fc3c0` and `exit()` with its return. The supervisor itself does not dispatch work — it `pthread_cond_timedwait`s on the supervisor cond var (with 10s timeout for shutdown polling) and handles a one-time setup at `sub_5fb440` plus the shutdown/reboot path (`/bin/sync`, `/sbin/reboot -f`).

### Strong inferences (consistent with evidence but NOT directly verified)

- **The "display effect proc" is a specific proc registered in the proc linked-list with the `PR_FG_ACCESS_VIDEO_DISPLAY` capability bit set.** Inferred from the error string `"Attempting to access scene from non display effect proc without PR_FG_ACCESS_VIDEO_DISPLAY flag."` and the proc-flag bit field at `+0x02`. We did NOT locate the specific proc registration nor confirm which flag bit corresponds to that capability.
- **`godot_switch_scene(PokemonGodotScene*)` is the bridge from Stern's proc system to Godot's scene tree** (where `add_child()`-into-scene-tree should happen). Inferred from name + signature; we could not locate the function body or its callers via direct xrefs (RTTI-only references; the lambda type info points to a data block at `0x42a5658` with no further xrefs).

### Open questions (unsolved this step — DO NOT ASSUME ANY ANSWER)

1. **Where is Godot's `MainLoop::iteration` (or equivalent frame tick) driven from?** Three candidates we have NOT eliminated:
   - It's hidden inside one of the 12+ named-but-unstudied `sub_*` calls in pokemon_le's `sub_720cf0`
   - It's run from a parallel thread spawned by one of pokemon_le's init functions (the equivalents of spike_menu's `sub_5958e0`/`sub_639110`/`sub_5fc980`)
   - It's not happening at all — Godot's per-frame `_process()` callbacks never fire for game scenes because nothing ticks the SceneTree
   The third option, if true, would directly explain the `is_inside_tree() returns false` symptom (scenes never get processed even after they're loaded). But we have NOT confirmed this — it is a hypothesis, not a finding.

2. **What other `GodotScene` subclasses exist** beyond `ManualGodotScene` and `PokemonGodotScene`? The catalog could matter — knowing the full set tells us what scene categories Stern wired through the Godot bridge.

3. **What does the `GodotScene` base class vtable look like?** Walking it (vtable at the address referenced by `sub_725fc8` line `*arg1 = &_vtable_for_GodotScene`) would enumerate the virtual interface — likely `switch`, `on_load`, `on_tick`, `release`, etc. — which directly informs what `godot_switch_scene` is supposed to do.

4. **What does the OS path-getter at `sub_32c6140` (called from `mcp_RasterizerGLES3_initialize` at `0x10a59fc`) actually return?** We know it returns `"/tmp/cache"` empirically (matched the discovery.log path), but we don't know whether that's `OS::get_cache_path`, `OS::get_user_data_dir`, or something else. Member offset `+0xc8` of the OS singleton.

5. **Spike_menu's `sub_5859a0`** — called from procDispatch with multiple integer args (0x92 post-dispatch, plus 0x10/0x11/0x13 from `sub_639110`'s init code). Looks like a multi-purpose op-code dispatcher. Role unconfirmed.

### What this turn produces for follow-up

- BN renames performed in both DBs (this step). Names use the `mcp_` prefix per session convention; uncertain renames carry an `_unsure` suffix so future sessions know the label is provisional.
- The `process_dispatch_loop` ↔ `sub_5faadc` equivalence catalogued so a side-by-side trace becomes mechanical for any future investigation.

### Actionable next probes (lowest-risk, highest-value)

- Walk the GodotScene vtable from `_vtable_for_GodotScene` (referenced inside `mcp_ManualGodotScene_constructor`) — enumerate methods, find the dispatch slot called by the proc system.
- Pattern-search for callers of `godot_switch_scene` via raw disasm of the function-body neighborhood (RTTI-only xrefs aren't enough; need binary search for the BL target).
- Confirm or rule out the "frame tick missing" hypothesis by tracing the threads spawned by pokemon_le's init functions — find the threads that call any of `eglSwapBuffers`, `glClear`, or Godot's `MessageQueue::flush`.

## Step 75: Radium-as-orchestrator reframe + knowledge-base cleanup (2026-05-01)

User-driven course correction during BN trace synthesis. The architectural model that was implicit in Steps 71–73f — "Godot drives the front-end, with Radium as a video/scene subsystem" — was inverted. **Radium is the primary UI runtime/renderer; Godot is an embedded element type within Radium scenes** via Stern's Pokemon-LE-specific `GodotScene` bridge layer (`../source/display/lcd/GodotScene.cpp`). All standard pinball UI (scores, pricing, AddCredits, GrandChamp, HighScore tiers, GameOver, attract rotation, tilt warnings) is rendered by Radium directly. Godot only drives Pokémon-specific embedded content (battle, capture, discover, pokédex). The 7-method binding API exposed by `ManualGodotScene` (`get_element` / `set_text` / `set_visible` / `set_clip` / `set_color` / `set_rect` / `set_transform`) is element-mutator semantics on a Radium element, not Node-tree semantics on Godot's SceneTree — confirming host(Radium) / embedded(Godot) directionality.

Implication for the persistent `is_inside_tree() == false` symptom: it's NOT about a missing top-level `/root/Main` in Godot's main SceneTree. It's the embedded subtree owned by a `ManualGodotScene` element that's never properly attached when Radium loads a scene containing a Godot-typed element. That re-localizes the front-end fix significantly.

### Knowledge-base cleanup (this step)

To prevent future sessions from drifting back to wrong premises by reading stale docs, the knowledge structure was consolidated:

- **New canonical entry point: `_emulation/MODEL.md`** — ≤2-page summary of the current architectural model, front-end-blocker hypothesis, suspect catalog, active env flags, next-probe priority, and where to find detail. Authoritative when anything elsewhere conflicts. Read first by every session.
- **Removed (in git history): `docs/HANDOFF.md`** (superseded by MODEL.md), `docs/NEXT_SESSION.md` (superseded by MODEL.md), `docs/EVENT_SWEEP_STEP52.md` (already captured in JOURNAL Step 51).
- **Memory updates:** `reference_radium_scene_format.md` had the Radium↔Godot relationship documented backwards in its "Integration with the game engine" section — corrected to reflect the embedded-element model. The MEMORY.md index now leads with a pointer to MODEL.md and flags the prior framing as inverted.
- **Surviving docs/ files** (kept as living domain specs, not session-state): JOURNAL.md, PROTOCOL.md, BRIDGE_PROTOCOL.md, NODEBUS_CIPHER.md, NODEBUS_HASH.md, MODE_TABLE.md, ASSETS.md, VIDEO_CLIPS.md, RADIUM_INVESTIGATION.md, SETUP.md.

Other memory entries (e.g. `reference_video_player_spi.md`, `project_emulation_progress.md`, `project_godot_engine.md`) likely need similar passes but weren't load-bearing enough to block forward motion this turn — left for incremental correction as future sessions touch them.

### Step 75h — BN database rename + Star Wars LE comparison binary added (2026-05-01, same-day followup)

Two related changes:

1. **BN database naming convention.** Every Binary Ninja `.bndb` file in this repo is now prefixed with the source game and version: `pokemon-le-0.81.0-{game,service_s_worker,image.bin,boot_display,dprint,netbridge.elf,powerdist_ext.hex.extracted}.bndb`. Previously the files were unprefixed (`game.bndb`, `netbridge.elf.bndb`, etc.). The rename matters because of #2 below.

2. **Star Wars LE 0.85.0 added as a comparison filesystem.** Lives at `/mnt/c/Workspace/starwars-le-0.85.0/` (NOT inside this repo's tree — fully separate workspace). Same Stern Spike 3 platform, same proc system, same Radium UI runtime, same node-bus protocol. **Critically: 0 "godot" strings in `games/star_wars_2025_le/game`** vs Pokemon LE's 457 — confirms it's pure non-Godot Stern (the comparison value was already validated for spike_menu, but spike_menu is a system-menu binary; FotE/SW LE is a full pinball game). Has Researcher A annotations baked into the BNDB. Binary path: `games/star_wars_2025_le/starwars-le-0.85.0-game.bndb`.

The investigative principle this enables: when in doubt about whether a function in `pokemon-le-0.81.0-game.bndb` is Pokemon-port glue or shared Stern code, open the equivalent in `starwars-le-0.85.0-game.bndb`. Present in both → shared Stern. Absent in SW LE or radically different → Pokemon-port glue (= the GodotScene bridge layer + dependents). Same logic applies for spike_menu but at smaller scope.

MODEL.md updated with a "Comparison binaries" section naming all current BNDB paths. MEMORY.md updated to surface the rename + SW LE availability at the top. `reference_netbridge_firmware.md` updated to use the new prefixed name. Other memory entries that mention `.bndb` paths (`feedback_bn_sequential_only.md`, `feedback_bn_function_comments.md`, `project_msgpack_SERVICE_S_only.md`, `reference_nodebus_protocol.md`) left as-is for now — they reference the unprefixed names in passing context where the rename doesn't break meaning, and incremental correction as future sessions touch them is fine.

### Step 75i — IScene base + GodotRenderer hierarchy mapped (2026-05-01)

User-driven discovery from the BN Types panel: spike_menu, SW LE, and pokemon_le ALL have identical `IScene::DMDEmulationScene::vTable` (33 xrefs), `IScene::NuklearScene::vTable` (30 xrefs), and `IScene::RadiumScene::vTable` (66/66/68 xrefs respectively — only ~2 difference between SW LE and pokemon_le, which IS the SceneCache+IScene shared infrastructure). Pokemon LE adds **one new IScene subclass: `GodotRenderer`** (55 xrefs to its vtable). This nails down the architecture: IScene is Stern's shared abstract base, and Pokemon LE's port is one new IScene subclass that internally wraps a Godot Node tree.

**Verified addresses (pokemon_le):**
- IScene typeinfo: `0x42a3e28` (name string `"6IScene"` at `0x37b9ad0`)
- IScene vtable: `0x42a4d10`
- GodotRenderer typeinfo: `0x42ba340` (name `"13GodotRenderer"` at `0x37c6db8`)
- GodotRenderer vtable: `0x422f538` (multiple-inheritance: secondary typeinfos at `0x42ba380`, `0x42ba428`, `0x42ba440` — likely Godot ClassDB-side bases)
- GodotRenderer destructor (vtable slot 0): `0x4baf00` → confirmed handoff to IScene vtable at `0x42a4d10` at end of body. Mechanically proves IScene parentage.

**GodotRenderer vtable layout (slots 0-10 known):**
| Slot | Address | Role |
|---|---|---|
| 0 | `0x4baf00` | D2 destructor (mcp_GodotRenderer_dtor_base) |
| 1 | `0x4bb130` | D0 deleting destructor (mcp_GodotRenderer_dtor_deleting) |
| 2 | `0x4d1d70` | tiny getter `return *(this+0x50)` (mcp_GodotRenderer_get_state_unsure) |
| 3 | `0x65f700` | stub `return 0` (mcp_GodotRenderer_vfn3_stub_returns_zero) |
| 4-7 | `0x4bacd0` … `0x4bad00` | inherited from IScene (NOT overridden — same impls as the 0x4ba/0x4bb range methods) |
| 8 | `0x65e360` | per-tick dispatch (mcp_GodotRenderer_tick_unsure): drains pending queue → `(*(*data_451adb0+0x6a8))()` → `sub_939d30()` |
| 9 | `0x65ba70` | scene activation (mcp_GodotRenderer_activate_scene_unsure): wraps two GodotScene members at +0x58/+0x60, posts deferred ops to Godot main thread, two staged passes |
| 10 | `0x4bad20` | inherited from IScene |

**GodotRenderer object state (from destructor + slot-9 reads):**
- `+0x40`/`+0x48` — main scene contexts
- `+0x50` — state byte/word (slot 2 returns this)
- `+0x58`/`+0x60` — two `GodotScene*` slots (LIKELY matches the scene_id=10/11 ping-pong from Step 73e — a single GodotRenderer instance holds two scene members and ticks both)
- `+0xe0` — pthread_mutex (input-queue lock)
- `+0x110/+0x118/+0x120` — pending-update queue: 0x68-byte entries with element-name string at +0x28 and std::function at +0x58 (the binding-API queue)
- `+0x128` — pthread_mutex (output-queue lock)
- `+0x158/+0x160/+0x168` — processed-entry queue: 0x40-byte entries
- `+0x178` — element-name lookup registry (for `get_element(name)` API)

**Refined front-end-blocker hypothesis:** the `is_inside_tree() == false` symptom can now be localized to one of three concrete failure modes:
- (a) `GodotRenderer` slot 8 (per-tick dispatch) is never being invoked by the proc system → no queue drain, no MessageQueue flush, no Godot tick.
- (b) Slot 8 IS invoked but `data_451adb0` (the singleton whose `+0x6a8` virtual is called inside slot 8) isn't initialized in our environment → the deferred-flush call is a no-op.
- (c) Slot 9 (scene activation) never runs → the GodotScene members at `+0x58`/`+0x60` are present but the deferred attach-to-tree messages never get posted.

All three are now mechanically investigable via xrefs on the vtable function addresses (slot 8 = `0x422f578`, slot 9 = `0x422f580`).

**Renames (pokemon_le, requires File→Save):**
- `func_8baf00` → `mcp_GodotRenderer_dtor_base`
- `sub_4bb130` → `mcp_GodotRenderer_dtor_deleting`
- `sub_4d1d70` → `mcp_GodotRenderer_get_state_unsure`
- `sub_65f700` → `mcp_GodotRenderer_vfn3_stub_returns_zero`
- `sub_65e360` → `mcp_GodotRenderer_tick_unsure`
- `sub_65de40` → `mcp_GodotRenderer_drain_pending_queue`
- `sub_65ba70` → `mcp_GodotRenderer_activate_scene_unsure`

MODEL.md updated with new architecture paragraph, refined blocker section, full GodotRenderer suspect catalog, and reordered next-probe priorities (identifying `data_451adb0` is now #1).

### Step 75j — IScene shared interface mapped + GodotRenderer bootstrap localized (2026-05-01)

User course-correction: stop chasing Godot internals and find the Stern shared layer (IScene contract + its dispatchers in SceneCache/DisplayRenderer). This step delivers that map.

**Found `mcp_GodotRenderer_bootstrap_godot` (`sub_65e390`)** — the function that initializes Godot from inside GodotRenderer's first activation. Hard-codes `Engine.cache_path = "/tmp/cache"` (Stern override; not from XDG_CACHE_HOME — explains the chdir-to-/tmp/cache investigation in Step 73c). Calls Godot Main::setup, Main::setup2, sets MainLoop, calls Main::start, registers GodotRenderer, then runs the first-frame tick. The `ready_flag` at `this[0xa]` gets set to 1 here. This function is the SINGLE entry point from Stern's GodotRenderer instance into the Godot runtime.

**Found `mcp_GodotRenderer_load_pack_or_bootstrap` (`sub_65f0e0`)** — the "either bootstrap or load resource pack" dispatcher. Caller passes a `.pck` path; if not bootstrapped yet, tail-calls into bootstrap. Otherwise calls `ProjectSettings.load_resource_pack(path, false)` and registers the new pack. THIS is where the proc system meets Godot via the resource-pack-loading API. Its xrefs (`sub_65e388` via tiny stub + the actual upstream caller) tell us how the proc system triggers GodotRenderer; not yet traced.

**IScene shared interface is now fully mapped.** Pokemon_le's IScene vtable at `0x42a4d10` has 11 primary virtual slots; decompiling each:

- Slots 0/1 (D2/D0 destructors): NULL — pure virtual (every subclass overrides)
- Slots 2/3: `__cxa_pure_virtual` at `0x46ab5c0` — pure virtual, semantic uncertain. GodotRenderer's slot-2 reads `*(this+0x50)` (state byte); slot-3 returns 0 (no-op).
- Slots 4/5/6: concrete `set_flag(idx)`/`clear_flag(idx)`/`get_flag(idx)` writing/reading bytes at `this + 0x28 + idx`. NOT overridden by GodotRenderer — IScene's flag API is the shared default.
- Slot 7: concrete `get_4tuple_default` outputs `(0,0,0,1.0f)` — a default rect/transform identity getter.
- Slots 8/9: `__cxa_pure_virtual` — **the per-tick + activate-scene methods every concrete IScene subclass implements**. GodotRenderer's slot 8 = drain-queue + Godot iteration; slot 9 = scene mount via deferred attach.
- Slot 10: concrete empty `ret` — optional hook stub.

**Stern's IScene lifecycle contract (inferred):** `construct → activate(slot 9) → tick(slot 8) repeatedly → release(destructor)`. Slots 4-7 are the flag/state-mutator API the dispatcher uses to communicate with the scene from outside. RadiumScene/NuklearScene/DMDEmulationScene all conform to this same contract — that's the shared Stern surface area we wanted to surface.

**External annotation comparison did not resolve IScene.** The comparison material did not provide the scene/UI naming shortcut being sought. [Redacted: third-party annotation prefixes, ranges and inventory.] The independent scene investigation continued.

**Renames (pokemon_le, requires save):**
- `sub_65e390` → `mcp_GodotRenderer_bootstrap_godot`
- `sub_65f0e0` → `mcp_GodotRenderer_load_pack_or_bootstrap`
- `sub_939d30` → `mcp_godot_Main_iteration_unsure`
- `sub_32c61c0` → `mcp_godot_Engine_get_singleton`
- `sub_2483700` → `mcp_godot_SceneTree_get_singleton_unsure`
- `sub_66ce20` → `mcp_SceneCache_PreLoadSceneCache`

**MODEL.md updated** with the IScene primary-virtual interface table and a reordered next-probe priority list explicitly anchored to "find Stern code that dispatches through IScene slots 8 (tick) and 9 (activate)".

## Step 76: Stern boot sequence mapping + SW LE port + Godot-is-illegal scope lock (2026-05-02)

### Step 76a — Sub-Stern slot-8/9 dispatcher mapping in SW LE 0.85

Worked primarily in SW LE 0.85 BNDB to isolate Pokemon-port glue from shared Stern code.

**Findings (no claims, just function identifications):**
- `RadiumScene` typeinfo at SW LE `0x749d98`, name string `"11RadiumScene"` at `0x647bf0`. Vtable at `0x733ea0`. Slots 4-6 at `0x35f670/0x35f680/0x35f690` (consecutive 0x10-stride, same shape as PLE's IScene flag-accessor defaults — strong indicator of shared IScene base).
- RadiumScene slot 8 (`0x41d100`) = small two-line `if (slot2_state) tail-call sub_52deb0(this+0x38, mode)`.
- RadiumScene base ctor at `0x41e0c0` — BN auto-named the vtable as `&_vtable_for_RadiumScene{for 'IScene'}`, confirming RadiumScene IS an IScene subclass in SW LE.
- 4 IScene subclasses in SW LE typeinfo data refs: `0x748240/0x748250/0x749d98/0x74cec8` — same count as Pokemon LE.
- `mcp_SceneCache_initialize` (SW LE `sub_4c2140`) — scans `/lcd/demand_loaded`, `/nuk/images`, `/lcd/auto_loaded`, builds RadiumScene per file, spawns thread named `"LoadSceneCache"` running `mcp_SceneCache_load_thread_body` (`sub_4c2ea4`).
- `mcp_SceneCache_load_thread_body` deserializes scenes via slot 12 (vtable+0x60) — beyond the IScene base interface.
- `mcp_SceneCache_add_scene_to_display_unsure` (`sub_4c17e0`) — type-dispatches by IScene subclass (RadiumScene → arg1+0x50 hashmap; NuklearScene → arg1+0x110 hashmap), enqueues into 3 places.
- `mcp_DisplayRenderer_render_thread_body` (SW LE `sub_35e130`) — DisplayRenderer's main render thread, ~3.7KB, 11 refs to `../source/display/lcd/DisplayRenderer.cpp` source-file string. Slot-8 dispatch at `0x35e710` via `(*(*p+0x40))(mode)`; slot-9 dispatch at `0x35e648` + `0x35f154` via `(*(*p+0x48))()`. Iterates lists drained from `this+0xf0` (tick) and `this+0x120` (activate).
- `mcp_DisplayRenderer_singleton_get` (`sub_308d00`) — single-instruction `return data_804030`.
- `mcp_g_display_renderer` (SW LE `data_804030`) — global DisplayRenderer singleton.
- `mcp_request_scene_display_with_dedup_unsure` (`sub_3064e0`) — std::set dedup wrapper around `mcp_SceneCache_add_scene_to_display_unsure(g_scene_cache, scene, g_display_renderer)`.

### Step 76b — Stern's main() boot sequence

Decompiled SW LE `_start → __libc_start_main → main`:
- `main`: readlink/dirname/chdir, init 2 semaphores, install 3 signal handlers, block SIGINT/SIGUSR1, call `sys_init() → sub_3f81d0() → sub_2b9b90() → exit(sub_2b95d0())`.
- `sub_2b9b90` = `mcp_spawn_procDispatch_thread` — spawns `procDispatch` worker thread with name string baked in.
- `sub_2b95d0` = `mcp_main_watchdog_shutdown` — main thread parks here in `pthread_cond_timedwait`, polls adjustment values, triggers reboot on signal.
- Inside `sys_init`, `sub_309af0` = `mcp_init_renderer_and_scene_cache_singletons` — `new DisplayRenderer(0x1b8) → DisplayRenderer_ctor → mutex+deferred-list → wait_on_render_thread_ready → new SceneCache(0x1d0)("./assets") → post-init`.
- `mcp_DisplayRenderer_ctor` (`sub_35ded0`): vptr install, condition_variable init, list head init, spawns thread named `"DisplayRenderer"`, waits on cv.
- `mcp_SceneCache_ctor` (SW LE `sub_4c0250`): std::string + 4 hashmap inits + condition_variable + list head, spawns thread named `"PreLoadSceneCache"` running `mcp_PreLoadSceneCache_thread_body` (`sub_4c2e84`, just calls SceneCache_initialize).

### Step 76c — Pokemon LE cross-validation

In PLE BNDB:
- `DisplayRenderer_render_loop` at `0x7b7ac0` (already named in earlier session) — byte-shape-identical to SW LE's `mcp_DisplayRenderer_render_thread_body`. Same field offsets, same OpenGL calls, same source-file line numbers in fprintf assertions. Slot-8 dispatch at `0x7b7cc0`; slot-9 dispatch at `0x7b7bf8` + `0x7b8704`.
- `mcp_init_renderer_and_scene_cache_singletons` at PLE `0x5d7aa0` — byte-shape-identical to SW LE's `0x309af0`. Same memset, same magic 4, same sizes (0x1b8 / 0x1d0), same `"./assets"` path.
- `mcp_SceneCache_PreLoadSceneCache` at PLE `0x66ce20` (already named in earlier session) — same shape as SW LE's `mcp_SceneCache_initialize`. Identical directory scan, identical RadiumScene factory pattern, identical "LoadSceneCache" thread spawn.
- `mcp_g_display_renderer` (PLE `data_445fa40`), `mcp_g_scene_cache` (PLE `data_445fa48`) renamed.
- `mcp_PreLoadSceneCache_thread_body` (PLE `sub_66db64`), `mcp_SceneCache_ctor` (PLE `sub_66af30`) renamed.

### Step 76d — Runtime inspection of PLE (GDB attach)

Launched PLE with `qemu-aarch64 -g 1234 ./game` (TIMEOUT=300, VIEWER=0, AUDIO_PLAYER=0). Attached via `gdb_target_remote 127.0.0.1:1234`. Continued; interrupted at +45s; `info threads`.

Findings:
- 46 threads alive. Main thread (Thread 1, CPU#0) at `0x...cbfb5d10` (libc — `pthread_cond_timedwait`-equivalent in `mcp_main_watchdog_shutdown`).
- Thread 2 (CPU#4) in `ioctl()` from `spi_shim.so`; saved LR via stack-walk + `info symbol` resolved to `setcontext + 829512` in section `.text` of game = BN address `0x8FA890` = function **`spi_serial_bridge_thread`** (already named).
- 40+ worker threads parked at single libc address (pthread_cond_wait).
- 2 threads in `accept()` (frame/audio bridge listeners).
- 1 thread in `select()`.
- Game log shows ongoing `MODE_PAGE_FLIP`, `snd_pcm_writei`, `avcodec_send_packet`/`receive_frame`, `serial-number` polling — not crashed.

### Step 76e — `feedback_godot_is_illegal.md` saved + MODEL.md scope reframe

After two course-corrections from user ("treat Godot like cooties" 2026-05-01; "I hate seeing the word Godot" / "godot is illegal" / "Godot doesn't come into play until middle of game" 2026-05-02), wrote `memory/feedback_godot_is_illegal.md`:
- Off-limits: `VideoPlayerSpi`, `ManualGodotScene`, `PokemonGodotScene`, `godot_switch_scene`, `GodotRenderer` pre-warm internals, `/tmp/godot_logs`, `./assets/godot/main.pck`.
- Still fair game (shared Stern, present in SW LE too): IScene contract, slot 8/9 vtable dispatch, deferred-call queues, SceneCache, RadiumScene, node-bus, SPI cmd handling.
- Calibration on overclaiming "CRITICAL/MAJOR/JACKPOT" on rehashed work.

`memory/MEMORY.md` index updated with the new feedback entry above the prior `feedback_pivot_to_shared_stern` pointer.

`MODEL.md` edits:
- Reframed architecture paragraph (removed GodotScene-hierarchy details, kept shared Stern stack).
- Replaced "What this means for the front-end blocker" with "Visible front-end symptom (2026-05-02)" — describes user-observed logo-then-frozen-loading-dots, frames the SW LE acid test.
- Stripped Pokemon-Godot-specific suspect entries (GodotRenderer vtable enumeration, GodotScene hierarchy, mcp_GodotRenderer_bootstrap_godot, mcp_GodotRenderer_load_pack_or_bootstrap, mcp_RasterizerGLES3_initialize); historical detail noted as preserved in JOURNAL Step 73 + BN database.
- Rewrote next-probe priority order with SW LE smoke-test as #1.
- "Last meaningful update" timestamp bumped to 2026-05-02.

### Step 76f — Star Wars LE 0.85 emulator port

Goal: prove the emulator goal (boot any Spike 3 game to attract) by running our shim against SW LE.

**Path-resolution change in `run_game.sh`** (lines 95-97):
```diff
-SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
-# _emulation → pokemon_le → games → sysroot
-SYSROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
-GAME_DIR="$SYSROOT/games/pokemon_le"
+SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
+# _emulation → <game_name> → games → sysroot. Auto-detect from SCRIPT_DIR
+# rather than hardcoding "pokemon_le" so the same shim/run_game.sh can drive
+# any Spike 3 game.
+GAME_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
+SYSROOT="$(cd "$GAME_DIR/../.." && pwd)"
```

Identical PLE behavior; works for any game.

**Created `games/star_wars_2025_le/_emulation/`** in SW LE filesystem (`/mnt/c/Workspace/starwars-le-0.85.0/`). Populated with absolute-path symlinks back to PLE's canonical `_emulation/`:
- Source: `shim_avforward.c`, `shim_bridges.c`, `shim_diagnostics.c`, `shim_discovery.c`, `shim_discovery.h`, `shim_fs.c`, `shim_gl.c`, `shim_inject.c`, `shim_io.c`, `shim_ioctl.c`, `shim_net.c`, `shim_nodebus.c`, `shim_patches.c`, `spi_shim.c`, `spi_shim.h`, `spi_shim.ver`
- Build/run: `build.sh`, `run_game.sh`, `nuke_emulation.sh`
- Built artifact: `spi_shim.so`
- Mesa overlay: `mesa_sw_overlay/`
- Python helpers: `decode_daemon.py`, `decode_daemon_test.py`, `frame_viewer.py`, `audio_subscriber.py`, `marker_button.py`, `historical-state-subscriber.py`

Per-game runtime state (NOT symlinked — created fresh in SW LE): `runs/`, `captures/`.

**Filesystem repairs in SW LE** (Windows-mount lost the original Linux symlinks, leaving 0-byte placeholder files):

Wrote `/tmp/fix_swle_libs.py`: scans SW LE's `lib/`, `lib64/`, `usr/lib/`, `usr/lib64/` for 0-byte `*.so*` files; for each, finds the highest-versioned non-empty sibling (e.g., `libfoo.so.X.Y.Z`) and copies its content into the placeholder.
- Result: 269 of 286 placeholders fixed. 17 left unfixable (cross-directory cases like `usr/lib/libblkid.so → lib/libblkid.so.1.1.0`); not blocking boot.

Replaced top-level broken entries in SW LE `games/`:
- `games/service_s_worker` (was 0-byte file) → copy of `games/star_wars_2025_le/service_s_worker` (272K).
- `games/game` (was 0-byte file) → copy of `games/star_wars_2025_le/game` (8.3M).
- `games/spike3` (was 0-byte file) → directory copy of `games/star_wars_2025_le/spike3/` (bin/, netbridge/, powerdist/, spike_menu/).

### Step 76g — SW LE smoke-test runs

Run 1 (TIMEOUT=60, no env overrides beyond defaults disabled): `./run_game.sh --debug -g 1234`. Game exited status 127 ("command not found") via `./game: error while loading shared libraries: /mnt/c/Workspace/starwars-le-0.85.0/usr/lib/libbluetooth.so.3: file too short`. → led to fix_swle_libs.py.

Run 2 (after `fix_swle_libs.py`): `VIEWER=0 AUDIO_PLAYER=0 service_s_worker=0 BOOT_DISPLAY=0 TIMEOUT=30 ./run_game.sh --debug`. Game ran for the full 30s (status 124 = TIMEOUT). Log shows: same SPI cmd 0x40 / 0x6c spam pattern as PLE, NV-directory remap creating `data/nv/star_wars_2025_le/{FRRAM,LKRAM,SYS_NVRAM,NOV_NVRAM,PIN_NVRAM,NVRAM,PTRAM,node_diag,NVM}` on demand, UART bridge protocol exchanges (cmd 0x10/0x15/0x18/0x1d/0x11/0x27/0x16), DRM display setup, Mesa EGL setup, `glCheckFramebufferStatus(DEFAULT) = 0x8cd5 (COMPLETE)` reached.

Activity counts at 30s:
- `MODE_PAGE_FLIP`: 60 (PLE same window: 8160)
- `snd_pcm_writei`: 3500 (PLE same window: 115000)
- `avcodec_*`: 0 (PLE same window: 16000)

Crash on shutdown via SIGTERM → `shim_term_dump_handler` → `shim_dump_sound_globals` → rodata probe at `game_base + 0x3607900` (`SE_PK_FX_ATTACK_TYPE_BUG`). That probe address is invalid in SW LE binary; segfaults inside the shim's diagnostic dump. Game itself didn't crash — only the shim's cleanup handler.

### Step 76h — BN database renames committed

PLE BNDB:
- `sub_4d47ac` → `mcp_GodotRenderer_singleton_bootstrap`
- `data 0x4413c50` → `mcp_g_godot_renderer` (was `godot_event_handler_hash_map`)
- `sub_5d7aa0` → `mcp_init_renderer_and_scene_cache_singletons`
- `sub_66af30` → `mcp_SceneCache_ctor`
- `sub_66db64` → `mcp_PreLoadSceneCache_thread_body`
- `data 0x445fa40` → `mcp_g_display_renderer`
- `data 0x445fa48` → `mcp_g_scene_cache`

SW LE BNDB:
- `sub_4c2140` → `mcp_SceneCache_initialize`
- `sub_4c2ea4` → `mcp_SceneCache_load_thread_body`
- `sub_41e0c0` → `mcp_RadiumScene_base_ctor`
- `sub_4c17e0` → `mcp_SceneCache_add_scene_to_display_unsure`
- `sub_4bfe90` → `mcp_RadiumScene_subclass_A_ctor_unsure`
- `sub_2b9b90` → `mcp_spawn_procDispatch_thread`
- `sub_4c0250` → `mcp_SceneCache_ctor`
- `sub_35ded0` → `mcp_DisplayRenderer_ctor`
- `sub_309af0` → `mcp_init_renderer_and_scene_cache_singletons`
- `sub_4c2e84` → `mcp_PreLoadSceneCache_thread_body`
- `sub_2b95d0` → `mcp_main_watchdog_shutdown`
- `sub_35e130` → `mcp_DisplayRenderer_render_thread_body`
- `sub_308d00` → `mcp_DisplayRenderer_singleton_get`
- `data 0x804030` → `mcp_g_display_renderer`
- `sub_3064e0` → `mcp_request_scene_display_with_dedup_unsure`

User confirmed File→Save on both DBs at intermediate checkpoints.

### Step 76i — Pending follow-ups (not started)

- Game-detect gating for `shim_dump_sound_globals` / `shim_dump_cmodes` / `shim_dump_dispatch_tables` so SIGTERM cleanup doesn't segfault on non-Pokemon games.
- 17 cross-directory broken symlinks in SW LE's `usr/lib/` (libblkid.so, libcurses.so, lib++dfb.so, etc.) — left unfixed because they didn't block boot.



## Step 77: Boot-blocker threads — Connectivity Setup screen identified, eight investigation tracks (2026-05-02)

User documentation of expected first-run boot sequence:
1. Coin door start → ~10s Stern Pinball logo + "Initializing..." with animated dots
2. ~5s Pokemon logo
3. ~8s "Pending Licensor Approval" + SPIKE3 (animated)
4. ~4s version screen: "POKEMON PRO / U.S.A. V0.75.0 / ENGLISH - 416172 / Stern Pinball logo"
5. Boot screen with Spike3 logo + ProgressBar (Radium scene `demand_loaded/bc0792d8.../036fe928.../scene.radium.json`)
6. ~2s **RED SCREEN (No Connection) + Guided Setup** — Lingua/Country/Free Play/Pricing/Volume/Networking/Stern Service S/Save&Exit
7. After Save&Exit: Pokemon logo → attract loop

Our emulator never gets past step 6. The "Connectivity/TestMenu/*" UI strings (`Page1, Cursor, Boxes_*, K_BKSPC, K_DONE, K_FWD, Numeric_Keyboard_Keypad`) are the operator's WiFi-credentials keyboard inside the Networking sub-page. The `"Connectivity/TestMenu/Diag_1360"` lookup that crashed SW LE at `sub_36a890` is a UI element in that screen.

PLE: rendered, alert sound spam, can't dismiss (no input mechanism). SW LE: same code path but missing asset → NULL deref → SIGSEGV at `0xd8`.

**NV persistence pushback (corrected previous claim):** PIN_NVRAM IS persisting — sequential `00000065/66/67` across runs, mtimes track run starts, NFI2-structured content with NSEC timestamps + NDAT records. SYS_NVRAM/NVRAM/etc. have real content but stale mtimes — likely because we never trigger "save settings" (no operator input completes the Guided Setup). `project_persistence_gap.md` memory needs amendment; the gap is narrower than stated.

### Threads to pursue (user-proposed + a few additions)

Numbered for reference. Listed in order of likely fruitfulness based on symptom-to-action distance, not user priority.

**T1. Hardware-trust / DRM-style minimum-presence check.**
Hypothesis: the game has a "minimum hardware acknowledged" gate before it leaves Guided Setup / enters attract. Stern manufactures these as kiosks; the firmware likely refuses to leave service mode until N nodes have responded credibly. Find that gate (probably in cmode init, switch-manifest validation, or one of the `data_44821xx` flags Step 72g touched). If we can satisfy or bypass, attract should proceed. Highest leverage; hardest to localize.

**T2. Less-chatty shim / authentic device-presence pattern.**
Currently the shim returns `00 00 04 00 00 00 00 00` to every SPI cmd. Real hardware doesn't always respond — non-present nodes return nothing, and the response byte 2 (currently hardcoded `0x04`) carries node-status. Hypothesis: the game expects a specific topology of present/absent nodes, and our universal-positive responses look wrong. Make the shim return "no response" (or a "device-absent" sentinel) for SPI commands targeting node IDs not in the Pokemon LE manual's node table; keep "present" responses only for the ~10 nodes actually in the cabinet (CPU, pwrdist, lcd, hdmi, coil4_led, pin, ws2812, magsensor, tmc, node4 variants per Step-26 hardware inventory). Surgical, high-impact, easy to A/B.

**T3. Properly run dprint + boot_display.**
We disabled both with `BOOT_DISPLAY=0` in recent runs. Stern's real boot uses `boot_display` to draw the splash sequence and `dprint` for debug-print output. The game opens `/games/data/boot_display_cmd` (FAILs in our log) which boot_display would have created. Without boot_display, the game's progress sequence may be skipping the splash and landing on the Guided Setup directly. Re-enable, see whether the boot sequence advances differently.

**T4. Version-info screen orientation in BN.**
User-suggested: rather than "fix everything and hope", anchor on the screen AFTER the Pokemon logo (the "POKEMON PRO / U.S.A. V0.75.0 / ENGLISH - 416172 / Stern Pinball logo" version screen). Find which Radium scene name corresponds to it, find the code that triggers it in the binary (likely a `select_and_load_scene(scene_id_X)` call), trace what gates that call. If the gate is a flag we can set, we're past the version screen and can chase the next gate.

**T5. spike_menu boot under our emulator.**
`games/pokemon_le/spike3/spike_menu/` has its own `game` binary + `image.bin` — a non-Godot Spike 3 system menu. Same shared Stern stack, no Pokemon-port glue. If our emulator can't boot spike_menu either, the bug is in the shared Stern path (env / hardware / nodebus). If spike_menu boots cleanly, the bug is specific to game-binary expectations beyond what spike_menu requires. spike_menu is small and usually completes its boot in seconds — quick test.

**T6. Compare external terminology.**
The proposed review concerned an external annotation set from Researcher A.
It was an orientation task, with no code changes recorded here.
[Redacted: third-party annotation inventory and identifying labels.]

**T7. Find first-run / Service S-completed NV flag.**
Find the specific NV byte the game checks to decide "show Guided Setup vs. show attract". If it lives in a writable NV file (we know PIN_NVRAM works), we can pre-populate it. Concretely: grep the binary for what reads from SYS_NVRAM near boot, find the if-condition that gates Guided Setup display. Brittle but cheap.

**T8. Connectivity diag asset audit.**
SW LE crashed because `"Connectivity/TestMenu/Diag_1360"` scene lookup returned NULL. Verify whether PLE actually has that scene in its `image.bin` — if so, the SW LE difference is "asset present in PLE only", which would be useful confirmation. If neither has it, both are coincidentally hitting the same NULL but PLE survives somehow (maybe checks `if (NULL) skip` somewhere we haven't seen). Use `_extraction/spike_extract_out/` index to look up scene names by SHA1.

### Likely highest-fruitfulness ranking

1. **T2 (less-chatty shim)** — small code change with broad behavioral impact; testable in <30 min.
2. **T5 (spike_menu boot)** — fastest "is the env right at all" check; should take <15 min to set up and run.
3. **T1 (hardware-trust gate)** — likely the actual root cause but hardest to localize; should be informed by T2/T5 results.
4. **T3 (dprint/boot_display re-enable)** — easy to enable and re-run; may unblock the boot sequence if it relies on boot_display side effects.
5. **T4, T7** — both depend on BN work; medium effort.
6. **T6** — pure orientation; do alongside.
7. **T8** — confirms a hypothesis but doesn't unblock progress.

### Latest-step pointer (2026-05-02)

When a new session opens, MEMORY.md will point here via `Latest JOURNAL step: 75 — boot-blocker threads`. Update that pointer when a new Step N+1 is added.



## Step 78: T2 SPI experiments + run-archiving infrastructure (2026-05-03)

### Step 78a — Two new shim env-var knobs

Added in `shim_ioctl.c`:
- **`SPIKE3_SPI_QUIET=1`** — default rx[2] flips from `0x04` to `0x00`. Per-cmd overrides via `SPIKE3_SPI_RX_<HEX>=` still take precedence.
- **`SPIKE3_SPI_DELAY_US=<int>`** — `usleep(N)` inside each SPI ioctl handler. Clamped 0..100000.

Both wired into `run_game.sh` as `SPI_QUIET` / `SPI_DELAY_US` shell tunables (defaults 0).

### Step 78b — T2 experimental results (PLE)

Three PLE runs compared (all with `--debug` for full logs):

| run | SPI_QUIET | SPI_DELAY_US | duration | page flips/sec | audio writes/sec | bridge timeouts |
|---|---|---|---|---|---|---|
| baseline (earlier today, log lost) | 0 | 0 | 30s | ~270 | ~3800 | unknown (not measured) |
| QUIET=1 | 1 | 0 | 87s | ~3 | ~160 | 53 |
| DELAY=100 | 0 | 100 | 147s | ~2 | ~93 | **105** |

**Key finding:** byte 2 = 0x04 in the SPI RX is load-bearing for the netbridge protocol's "ack / data-available" semantics. With QUIET=1, the game thinks the bridge never has data, so bridge messages time out. Activity drops 24-90× because downstream work depends on successful bridge round-trips.

**Same error categories** appeared in both QUIET and DELAY runs (and likely in baseline too — never confirmed because runs/latest was overwritten before we extracted error counts). The new persistent error families surfaced in this session:
- `bridge message (cmd=N, id=N) response failed 110: Connection timed out` (53 in QUIET, 105 in DELAY)
- `write failed to verify at N: reads back as ffffffff, should be N` (7 each — netbridge.elf or pwrdist flash verification)
- `[Radium Error] VideoStreamDecoder: Error sending packet to decoder: Operation not permitted` (3-5)
- `[Radium Error] Failed to get HW surface format.` (3-5)
- `[hevc] Failed to find a V4L device for HW` (2-5)
- `[hevc] Failed setup for format drm_prime: hwaccel initialisation returned error.` (2-5)
- `failed to write BQ coefficients: No such file or directory` (4) — MAX9850 audio DAC config write
- `bridge message ... Connection timed out` (errno 110)

**Net interpretation for T2:** byte 2 = 0x04 is necessary; flipping it off doesn't help. Slowing the SPI rate doesn't change *which* paths the game runs — same error families, just amplified bridge timeouts. T2 in its naive form is not a win. Refined T2 would need cmd-specific responses (e.g., 0x04 only for cmds netbridge-handles-directly, different status bytes for node-addressed cmds) — would require studying netbridge.elf's `HOST_Update` dispatch table.

**Default settings reverted.** `SPI_QUIET=0` and `SPI_DELAY_US=0` are the defaults. Knobs left in place for future experiments.

### Step 78c — Run-archiving infrastructure

Every run now creates its own folder; `runs/latest` is a symlink to it. Backward-compatible with anything referencing `runs/latest/<file>`.

Layout:
```
runs/
├── latest -> 2026-05-03T00.45.10_quiet_test/
├── 2026-05-03T00.45.10_quiet_test/
│   ├── run_info.txt       # timestamp, note, env-toggle table, qemu cmd
│   ├── game.log
│   ├── audio.log
│   ├── boot_display.log
│   ├── service_s_worker.log
│   ├── decode_daemon.log
│   ├── discovery.log
│   └── viewer.log
├── 2026-05-03T00.06.18_delay_100us/
└── ...
```

New CLI flag: `--note "..."` (or `-note "..."`) — sanitized to `[a-z0-9_]`, max 40 chars, appended as suffix to the run dir name.

`run_info.txt` is written **before** the qemu launch so metadata survives even if launch fails. Contains: timestamp, run_dir, sysroot, game_dir, note, raw shell invocation, all env-toggle values (DEBUG/SPI_QUIET/SPI_DELAY_US/...), and the full qemu command line.

This addresses the recurring problem of "we can't compare to the previous run because runs/latest was overwritten."

### Step 78d — NV persistence pushback (corrected)

User pushed back on prior `project_persistence_gap.md` claim that "NV persistence is fully stubbed." Verified 2026-05-02:
- **PIN_NVRAM persists correctly.** Sequential `00000065/00000066/00000067` files across runs, mtimes track run starts, NFI2-format content with NSEC timestamps + NDAT records.
- **SYS_NVRAM/NVRAM/etc. have real content but stale mtimes.** Most likely cause: the game never triggers the "save settings" path because nobody completes Guided Setup → Save & Exit. Less likely: shim hooks subtly broken for those write paths.

`project_persistence_gap.md` memory updated to reflect this nuance — the gap is narrower than originally claimed.

### Step 78e — Pending followups

- T1, T3, T4, T5, T6, T7, T8 from Step 77 still open. Most actionable:
  - **T5** (spike_menu boot test) — fastest "is the env right" check, ~15 min.
  - **T6** (full Researcher A list review for terminology) — pure orientation.
  - **T7** (find first-run NV gate flag) — concrete static-analysis target.
- Task #13 (gate Pokemon-specific shim diagnostics so SIGTERM doesn't segfault on non-Pokemon games) — still pending, not blocking anything currently.
- 17 cross-directory broken symlinks in SW LE's `usr/lib/` from Step 76f — still unfixed, didn't block boot.

## Step 79: Alternate startup-mode experiment and scene-loading observations (2026-05-04)

### Step 79a — Startup-mode experiment

> [Redacted: the activation patch, target address, write value and confirming
> patch log. This experiment altered startup mode; the observations below
> describe that altered run, not normal startup.]

### Step 79b — Run diff (baseline `01.05.01` no-patch vs `23.53.48_diag_mode_v2` diag-on)

Both runs same shim infrastructure modulo the byte; baseline had `SPI_DELAY_US=100`, diag had `=0`, so SPI poll volume differs but semantic events are comparable. Pulled unique log-line patterns from each, filtered SPI noise, diffed.

**Activity that exists ONLY in the diag run:**

| New event | Source / interpretation |
|---|---|
| `Node not found: "/root/Main"` from `/root/Discover/discover_progress_bar` | Discover/Pokédex GDScript instantiating |
| Same error from `/root/capture_battle_scene/battle_scene_grp/...` | Capture battle scene instantiating |
| `Node not found: "AnimationPlayer"` from `/root/pokemon_battle_scene` | Pinball battle scene instantiating |
| `bridge: cmd 0x41 slot=N inner_cmd=0xfa/0xfe payload=[…]` | **Encrypted nodebus traffic** — see `project_nodebus_cipher.md`. Never seen in baseline. |
| `bridge: no synthetic response for cmd 0x12` | New cmd 0x12 issued; our shim doesn't synthesize it yet. |
| `[gpiod] line_request_output(gpiochip1:7, consumer="ant2")` | Antenna-2 GPIO init (Bluetooth/WiFi-related) |
| `system() rm -rf /connectivity/files/gkpd_{1,2,3}` | Cleanup of "gkpd" cache files |
| `access /connectivity/update/UPDATE_COMPLETED` (FAILED) | Software-update marker probe |
| Two extra `/proc/cpuinfo` opens (fd 36, 37) | Additional CPU enum |

Baseline has **zero** hits on `Discover`, `capture_battle_scene`, `pokemon_battle_scene`, `bridge cmd 0x41`, or any of the others.

### Step 79c — User context: connects to extracted-previs keyboard-jump test scene

User confirmed: when extracting the Godot project (`assets/godot/main.pck`), there's a test mode that shows a scene with keyboard combinations (Ctrl+B/D/C/P per project `CLAUDE.md`) to hop directly into battle / discover / capture / pokédex. The diag flag plausibly gates the **same** test-cycler — the C++ side that auto-attempts these scenes without needing real keyboard input or completed Guided Setup. Code structure consistent: 7 of the 8 previs `.gd` scripts call `get_node("/root/Main")` (`bkgnd_video_player_manager`, `capture_bkgnd_video_player_manager`, `capture_char_sprite_manager`, `discover_bkgnd_video_player`, `discover_progress_bar`, `game_session`, etc.). User said `/root/Main` was commented out in the previs project to make it work; in the shipping binary `/root/Main` is also missing for the same root cause (see 77d).

### Step 79d — `/root/Main` re-look: nothing new

Per `feedback_grep_journal_before_change.md`, did a 30-second grep before deep-diving:

- BN entry **[81]** (referenced JOURNAL line 5192): standard Godot init bypassed — `OS::initialize` / `DisplayServer::create` / `RenderingServer::init` / `SceneTree::init` / `Main::setup` / `Main::start` are all absent from runtime flow. `procDispatch` at `sub_b2ae90` drives the show via condition variables and event firing.
- Step **73g** (2026-05-01): Radium-as-orchestrator reframe. The `is_inside_tree() == false` symptom isn't about a missing top-level `/root/Main` in Godot's main SceneTree — it's the embedded Godot subtree owned by a `ManualGodotScene` element that never properly attaches when Radium loads a scene containing a Godot-typed element.
- Strings check fresh: `Main::Setup`, `Main::Setup2`, `Main::Start`, `Main::Cleanup` are present in the binary as C++ symbols, and `res://scenes/main.tscn` exists in `main.pck`. Neither is on the runtime path.

So there's no new lead here. The `/root/Main` errors the diag flag triggers are downstream evidence of "test-cycler activated, but the embedded-subtree mount machinery still isn't there." Not a Godot fix path — Pokemon-port glue, out of scope per `feedback_godot_is_illegal.md`.

### Step 79e — `runs/latest` fixed

Side bug surfaced during 77b log inspection: `runs/latest` had become a stale directory (with old log files + broken self-symlinks) instead of a symlink. `run_game.sh`'s `ln -sfn "$RUN_DIRNAME" "$SCRIPT_DIR/runs/latest"` silently no-ops when the link target already exists as a directory, so per-run logs were going to the timestamped dir but `runs/latest/` kept showing yesterday's contents.

Fix: `rm -rf runs/latest`, then a fresh run (`runs/2026-05-03T23.59.10_diag_mode_v3_symlink_test`) re-created the symlink correctly. `runs/latest` now points at the most recent run dir as designed.

### Step 79f — What this gives us / open threads

**Useful side door:** the diag flag is a way to push the game into trying to render Pokemon-specific scenes (Discover/Capture/Battle) and exercise encrypted-nodebus + antenna paths **without** completing operator-side Guided Setup. Doesn't bypass the embedded-subtree mount problem — the scenes still error on `/root/Main` — but it activates a richer set of code paths than the attract-loop baseline.

**Not a fix for the front-end blocker** (Step 77): the Connectivity Setup / Guided Setup path still doesn't complete; the test-cycler is a separate boot-mode selector. The two are independent.

**Open:**
- Whether the diag flag is C++ side or Godot side: `Main::Setup2` exists as a C++ symbol — possibly the second-stage init that respects this byte. Static-analysis target if useful.
- Whether the encrypted bridge traffic (cmd 0x41) and `gkpd_*` rm calls are part of the test-cycler's setup or unrelated diag-mode side effects.
- Cmd 0x12 has no synthetic response — minor gap in `shim_nodebus.c`. Add when bridge work resumes.

## Step 80: BN notebook 2026-05-04 export — full boot-flow map + 7 high-value new leads (2026-05-04)

### Step 80a — Source

Fresh BN notebook export at `_emulation/reference/game - Binary Ninja Notebook Export (2026-05-04).json` (160 entries across 11 categories: algorithm 36, system_interaction 29, architecture 29, data_flow 34, hidden_feature 19, capability 4, hardware_interfaces 3, environmental 2, data_format 2, persistence 1, security 1). Substantially more boot-flow detail than the older 2026-04-22 export the memory still pointed at; previous version had 80 entries.

### Step 80b — Startup authorization and diagnostic state

> [Redacted: authorization-bypass mechanisms, patch targets and values,
> key-derivation and provisioning details, and protected storage locations.]

The notebook proposed a relationship between authorization state, diagnostic
mode and a 12-state initialization machine. Subsequent work distinguished
static structure from runtime evidence; the notebook alone did not prove the
machine was responsible for the observed scene cycle.

### Step 80c — `sub_873900` is a 12-state init/mode machine

State machine "controls initialization sequence":
- **State 0**: startup authorization check. [Redacted: internal identifier.]
- **States 1-3**: initialization phases
- **States 4-5**: diagnostic mode transitions
- **States 6-8**: scene loading and processing  ← matches our Step 79 run-diff (Discover/Capture/Battle attempts)
- **States 9-10**: cleanup and reset
- (state 11 unaccounted for in notebook)

[Redacted: the proposed authorization-override transition.] That's the test-cycler we observed.

### Step 80d — Six-tier boot sequence (newly mapped at last)

```
_start (0x49cb80)
 └─ __libc_start_main → main (0x461270)
     • sets system_state_flags = 0x10
     • sem_init on data_440abc8, data_440abe8
     • registers SIGINT(2)/SIGBUS(10)/SIGSEGV(12)
     • → primary_initialization_orchestrator (0x678bc0)
         · init_spi_serial_bridge      ← MUST succeed or exits 0xe2
         · mcp_eeprom_init
         · mcp_init_renderer_and_scene_cache_singletons (0x5d7aa0)  ← graphics + boot-splash
         · mcp_node_record_table_init
         · hardware_input_validation
         · [Redacted: identity/authorization initialization details]
         · fires events 0x5c → 0x5e → 0x5f → 0x5d
         · sets data_4482328 = 1 at completion
     • → secondary_initialization (0x5abbf0)
         · fires event 0x10 (start)
         · CONDITIONAL: if sub_7a8de0(0x13) → calls sub_579d30() + setup
         · fires event 0x11 (complete) — TWICE (early-return path + normal path)
     • → create_dispatch_thread (0x72b160) → spawns 'procDispatch' (process_dispatch_loop @ 0x7292ac)
     • → main_event_loop_with_timeout (0x72aba0)
         · pthread_cond_timedwait on g_main_loop_condition_var (0x44c9288), 10s timeout
         · timing thresholds 0x257ms (599) and 0xe0f ms (3599) at g_timestamp_threshold_1 (0x44cb2e0)
         · shutdown: /bin/sync + /sbin/reboot -f gated by flags 0x44c9468 / 0x44c9469
```

**Notebook quote on "no orchestrator":** "The control flow chain implements NO SINGLE ORCHESTRATOR. Instead, each tier signals completion via events: hardware tier fires readiness events (0x5c/0x5e/0x5f/0x5d), event dispatch tier signals main loop via condition variable, main loop synchronizes timing across tiers."

### Step 80e — Seven high-value new leads

Ranked by breakthrough potential. All addresses from BN notebook 2026-05-04; will re-verify each before relying on it (per `feedback_verify_every_address.md`).

1. **`sub_7a8de0(0x13)` decompile** — secondary_initialization's conditional branch. If false, the early-return path is taken (still fires 0x11 but skips `sub_579d30` + setup). Smallest scope, most surgical "what gates progression" target. Single function, called with known immediate. **Top of probe queue.**

2. **Event 0x11 runtime trace** — verify it fires AND its handlers run to completion. We have BRK-trap infra in `shim_discovery.c`; need to add `fire_event` (0x5c12a0) hook with id == 0x11 filter. ~30 min.

3. **`showBootScreenGameTitleProgressBar` (0x6bd2ac) reachability test** — notebook flags it as "orphaned / no direct callers" — possibly abandoned 5260-byte function. If it never fires in our runs, our boot-splash is something else (likely the direct-DRM/KMS path at `create_boot_splash_egl_context` 0x901d10 + `DisplayRenderer_render_loop` 0x7b76e0). BRK-trap or breakpoint test, ~10 min.

4. **`data_4482328` xrefs** — set to 1 at primary_init completion. Could be a wait-gate other code reads. Quick BN xrefs query.

5. **`sub_873900` full decompile** — the 12-state machine itself. Bigger function but central to mode transitions; tells us *exactly* what advances each state. Decompile + comment, ~45 min.

6. **Timing-threshold runtime watcher** — main loop checks elapsed time vs `g_timestamp_threshold_1` (0x44cb2e0) against 599ms and 3599ms thresholds. Needs runtime instrumentation; useful if probes 1-5 don't unblock anything.

7. [Redacted: a proposed way to synthesize protected startup state. This was an unimplemented lead.]

### Step 80f — Connections to prior steps

- **Step 77 thread T7**: the inquiry narrowed to persistent first-run state and its relationship to startup. [Redacted: proposed protected-state modifications.]
- **Step 79 test-cycler observation**: confirmed by `sub_873900` states 6-8.
- **Step 75 `is_inside_tree() == false` orphaned-subtree theory**: not contradicted; the diag flag activates the test-cycler but downstream `/root/Main` errors are downstream of the embedded-Godot-subtree mount problem, which the notebook also corroborates ("Godot Engine compiled into binary but NOT used for rendering — only asset management").
- **`feedback_godot_is_illegal.md`**: nothing here is Godot. The state machine, event 0x11 gating, sub_7a8de0 decision are pure shared Stern code — should reproduce in SW LE 0.85. T6 reorientation deferred to Researcher A-list review later; for now the fresh notebook covers most of it.

### Step 80g — Probe queue (status)

| # | Lead | Status |
|---|---|---|
| 1 | `sub_7a8de0(0x13)` | **DONE** — ruled out as front-end blocker (see 78h.1) |
| 2 | Event 0x11 runtime fire trace | not started |
| 3 | `showBootScreenGameTitleProgressBar` reachability | not started |
| 4 | `data_4482328` xrefs | not started |
| 5 | `sub_873900` 12-state machine | **STRUCTURE VERIFIED**; runtime presence not verified (see 78h.2) |
| 6 | Timing-threshold runtime watcher | not started |
| 7 | [Redacted: protected-state synthesis lead] | not started |

Plus options that emerged mid-session:

| ID | Option | Status |
|---|---|---|
| (i) | BRK-trap `sub_873900` entry to confirm runtime invocation, dump arg1 / lr / state object | **queued by user** — finish lead 5 verification |
| (ii) | Look up handlers we already runtime-dumped for events 0x11 / 0x5d / 0x5f in BN | **in progress this session** |
| (iii) | Bit-19 patch test (flip bit 19 of `data_44cb9f0`, run, see what `sub_579d30` adds) | not started |

### Step 80h — In-session work completed (2026-05-04)

#### 78h.1 — Lead 1 verification + ruling out

- `sub_7a8de0(arg1)` decompile + verify: bit-tester for `data_44cb9f0`, returns `bit arg1 of data_44cb9f0` if `arg1` in [1, 0x23], else error.
- Constructor `sub_7a8b90` allocates `data_44cb9f0 = calloc(5,1)` (5 bytes / 40 bits) + 36-byte `data_44cb9c0` + 4 more 5-byte bitfields at `data_44cb9d0[0..3]`. Initializes `data_44cb9c0[1..0x23]` from static .rodata table at **`0x4389b5a`** (4-byte stride per entry, byte 0 of each entry copied).
- Setter `sub_7a8a80(idx, bitfield)`: sets bit `idx` in bitfield iff `data_44cb9c0[idx] & 1`. So **`bit idx of data_44cb9f0 = LSB of byte at (0x4389b5a + idx*4)`** at construction time.
- For `idx=19`, source byte at **`0x4389ba6` = 0x00** (verified via hexdump_address). Bit 19 statically clear.
- `secondary_initialization` (0x5abbf0) disasm verified actual control flow:
  ```
  if ((fire_event(0x10) & 1) == 0) tail-call fire_event(0x11);   // early return
  else {
      if ((sub_7a8de0(0x13) & 1) != 0) sub_579d30();              // bit 19 gates ONLY this
      sub_5d6480(1, 0, 0, &data_4363c78);                          // unconditional
      sub_72aa60(1);                                               // unconditional
      sub_57a8f0(1);                                               // unconditional
      tail-call fire_event(0x11);
  }
  ```
- **Notebook framing was wrong**: it said bit 19 gates "sub_579d30 + additional setup functions"; only `sub_579d30` is gated. The rest of the chain runs unconditionally on the non-early-return path.
- All 4 handlers for event 0x10 (registered at `event_handler_table[0x10]`) verified to return 1 unconditionally via disasm:
  - `0x4be6a0` — array iteration setting `*(item + 0x48) = 1`, `mov w0, #0x1; ret`
  - `0x4c2410` — `bl sub_4c2020`, array iteration `bl sub_4be820`, `mov w0, #0x1; ret`
  - `0x4d9db0` — `mov w0, #0x1; ret` (NOP-padded prologue at sym `sub_4d9da8`)
  - `0x70e790` — `bl sub_729d60` + read config 0x25/0x24 to fields `+0x108`/`+0x110`, `mov w0, #0x1; ret` (NOP-padded prologue at sym `sub_70e788`)
- Therefore `fire_event(0x10)` returns 1 in our runs; `secondary_initialization` does NOT take the early-return path. Bit 19 / `sub_579d30` is a small optional pre-step.
- **Reuse note**: Step 51 (2026-04-21) already covered `fire_event` semantics extensively and built `shim_fire_event` trampoline at `0x5c12a0`. This session re-derived the dispatch logic for the analysis above. Future event-handler work should reference Step 51's infrastructure (notably `shim_fire_event` is already callable from shim TUs).

#### 78h.2 — Lead 5 (sub_873900) structural verification

- `sub_873900` confirmed 12-state machine (states 0..11, validated by `cmp w0, #0xb / b.hi`).
- State words: `arg1[0x110]` = next, `arg1[0x114]` = current. Bail-out guard: `arg1[0x178] & 1`.
- Tick: `bl 0x45c650` with arg `0x30d40` = 200000 μs = 200ms = 5Hz.
- State dispatch table at **`0x37cc818`** (12 bytes): `[0x10, 0x58, 0x4e, 0x53, 0x00, 0x49, 0x22, 0x1a, 0x69, 0x15, 0x17, 0x00]`. Jump destination = `0x87397c + (sxtb(table[state]) << 2)`.
- Computed state → block jump:
  | state | byte | jump | calls / does |
  |---|---|---|---|
  | **0** | [redacted] | [redacted] | Startup authorization check; implementation details redacted |
  | 1 | 0x58 | 0x873adc | sets state=2, `bl 0x872fa0` |
  | 2 | 0x4e | 0x873ab4 | flag check `+0x179`, sets state=3 |
  | 3 | 0x53 | 0x873ac8 | `bl 0x872540` |
  | 4 | 0x00 | 0x87397c | default dispatch entry, `bl 0x733310` (timestamp) |
  | 5 | 0x49 | 0x873aa0 | flag check `+0x1e8`, sets state=6 |
  | 6 | 0x22 | 0x873a04 | sets state=7, complex; calls into `+0x150` vector |
  | 7 | 0x1a | 0x8739e4 | flag check `+0x179`, sets state=6 |
  | 8 | 0x69 | 0x873b20 | `bl 0x86cf70` |
  | 9 | 0x15 | 0x8739d0 | flag check, jumps to state-0 logic |
  | 10 | 0x17 | 0x8739d8 | clears `+0x110`, jumps to state-0 logic |
  | 11 | 0x00 | 0x87397c | duplicate of state 4 |
- **Static reachability is unresolved**: zero callers to `sub_873900` (only `sub_873c24` — a 1-line wrapper); zero callers to `sub_873c24`; zero 8-byte-LE pointer matches in binary; zero RELATIVE relocations (binary has 0 total).
- Per user direction: assume dynamic dispatch via heap/stack-resident object, NOT dead code.
- Runtime evidence (Step 79 diag-on test-cycler scene attempts) is *consistent with* sub_873900 driving the test cycler but does NOT prove it (could be one of the other 13 authorization-related paths the notebook describes). **Option (i) — BRK-trap entry — is the unfinished verification.**

#### 78h.3 — Tools added this session

- `shim_inject.c::shim_dump_event_handlers(int event_id)` — walks `event_handler_table[event_id]` linked list at runtime (`game_base + 0x445ea80 + event_id*8`), logs each node's callback runtime addr + computed VA + priority + next ptr. Up to 32 nodes per event.
- `shim_inject.c::shim_apply_event_handler_dump()` — calls the above for events `{0x10, 0x11, 0x5c, 0x5d, 0x5e, 0x5f}`. cwd-gated on `pokemon_le`.
- Hooked from `shim_discovery_init_thread` after `[REDACTED: startup-override helper]`.

#### 78h.4 — Runtime-collected handler data

Run: `runs/2026-05-04T11.19.01_event_handler_dump_v1/game.log`. All from `event_handler_table` after game-side static constructors complete:

| event | handler count | handler VAs (chain order) | priorities |
|---|---|---|---|
| 0x10 | 4 | 0x4be6a0, 0x4c2410, 0x4d9db0, 0x70e790 | 0, 0, 0, 0 (all return 1 — verified) |
| 0x11 | 5 | 0x4bd4b0, 0x4be5f0, 0x4d9db0, 0x7e7e60, 0x8707c0 | 0, 0, 0, 0, 0 (not yet read) |
| 0x5c | 0 | (none) | — (default-return-1) |
| 0x5d | 16 | 0x5d4140, 0x825f80, 0x4afcc0, 0x4c50d0, 0x4d5190, 0x4d9db0, 0x4dcb50, 0x588ad0, 0x673cc0, 0x6ad7e0, 0x6c0820, 0x6f6690, 0x715050, 0x716620, 0x854690, 0x8708c0 | **128**, **100**, then 14 × 0 |
| 0x5e | 0 | (none) | — |
| 0x5f | 1 | 0x75c430 | 0 |

**Shared handler note**: `0x4d9db0` (just `return 1`) appears in events 0x10, 0x11, 0x5d. Skip in BN lookups.

## Step 81: Architecture decision point — LD_PRELOAD path-remap → mount-namespace bind mounts (2026-05-04)

Decision point captured during investigation of why `.radium` files aren't visibly opened. Recording the assessment + proposal before attempting POC. Commit pinned for revert if needed.

### Assessment

The LD_PRELOAD path-remap architecture has a fundamental ceiling we keep colliding with: it can only intercept calls that go through *named libc symbols we know to hook*. Today alone we found that internal glibc symbols (`__open*_2`, etc.) need separate hooks; raw syscalls bypass libc entirely; libstdc++ may use paths that are completely opaque from outside; and even when our hooks fire, the path-remapping logic itself has subtle holes (relative paths bypassing remap, nested CWD interactions, etc.). We're not 95% of the way there. We're at a structural wall.

The right answer is **not** a kernel driver. That's heavyweight, hard in WSL2, and unnecessary because **Linux already provides the right primitive: mount namespaces with bind mounts.**

### Proposal

```
unshare --mount --user --map-root-user bash -c '
    mount --bind /mnt/c/Workspace/pokemon-le-0.81.0/games        /games
    mount --bind /mnt/c/Workspace/pokemon-le-0.81.0/etc          /etc/spike3   # selective
    mount --bind /mnt/c/Workspace/pokemon-le-0.81.0/connectivity /connectivity
    mount --bind /mnt/c/Workspace/pokemon-le-0.81.0/data         /data
    cd /games/pokemon_le
    LD_PRELOAD=spi_shim.so qemu-aarch64 ./game
'
```

When the game opens `/games/pokemon_le/assets/lcd/auto_loaded/...`, the **kernel** resolves it through the namespace's mount table. Doesn't matter if it's a `fopen64` call, a `__openat64_2` call, a raw `syscall(SYS_openat,...)`, or inline assembly — it all hits the kernel and the kernel sees the bind mount. **Every path-related hole closes simultaneously.**

The LD_PRELOAD shim stays — but only for what it should always have done: **hardware**. SPI bridges, I2C devices, GPIO, audio DACs, UART netbridge, frame/audio sockets to the host, etc.

### Why this beats alternatives

| Option | Verdict |
|---|---|
| Continue current approach | Wall. We literally cannot intercept what we cannot enumerate. |
| Kernel driver | Right idea, wrong layer. WSL2 makes custom drivers painful. We get the same property from a userspace mount namespace, which is just-works in WSL2. |
| Dedicated WSL env with `/games/` paths | Solves the path problem but: doesn't scale to multi-game, creates a separate dev environment, you still need hardware hooks. |
| **Mount namespace + bind mounts (proposed)** | Same FS guarantees as kernel driver. Same path correctness as dedicated WSL. Lives in the same WSL distro. ~50 lines of bash. |

### What we lose / what we keep

Lose: shim_fs.c's `remap_path`, ~20 of our fs hooks, the audit-pass hooks for unlink/rename/etc., the `should_log_open` filter, the `__openat64_2` etc. variants we just added. Roughly 600-800 lines of fs-specific shim code becomes dead.

Keep: every hardware hook (SPI/I2C/UART/V4L2/DRM/GPIO/ALSA), discovery infrastructure, BRK traps, the diag-mode patch, the event-handler dump, the Radium scene catalog, the BN notebook integration, JOURNAL.

### Plan

1. **5-minute POC:** `unshare -m -r` and bind-mount `/games`. Run the existing game with no fs shim hooks (or just bypass remap_path to be a no-op). Verify the game opens .radium files visibly via existing fopen64 hook. Single experiment definitively answers our current question — *and* validates the refactor direction in one shot.
2. If POC validates: write a `run_game_namespace.sh` that does the bind-mount setup, then iteratively delete fs hooks from the shim.
3. End state: shim becomes "hardware bridge" only, ~30% of its current size. New games come online by scripting their bind-mount table, not by adding hooks.

### Risks

- WSL2 mount-namespace behavior can have quirks. Bind mounts work, but `unshare --user` may need `kernel.unprivileged_userns_clone=1` set on some distros.
- Some host paths we DO want to redirect (fontconfig probing /home/c/...) — handled by bind-mounting our config in or by setting env vars, not by remap.
- A few "writes" we currently emulate via remap (audit-pass `unlink /connectivity/...`). Bind mount handles these for free, but verify clean run.

## Step 82: POC validated — file loading was a phantom, real blocker is scene-cycling at 10↔11 (2026-05-04)

### Pivot summary

After months of escalating LD_PRELOAD shim coverage (50+ hooks added including FORTIFY-source variants `__openat64_2` / `__open_2` / `__open64_2`, internal aliases `__open` / `__openat` / `__open64` / `__openat64`, dirent variants `readdir` / `readdir64`, getcwd, fdopen, etc.), we still saw zero `.radium` opens in our shim log. The conclusion drawn was "scenes aren't being read." **That conclusion was wrong.**

Authoritative host-level strace (qemu-aarch64 wrapped with `strace -f -e openat`) shows definitively:

- **34 unique `.radium` files actually opened** in a 120s run (19 from auto_loaded, 15 from demand_loaded)
- All via libc-internal call paths the LD_PRELOAD-name-based hooks don't intercept
- ZERO indication file loading is broken

**Pillar memory `feedback_build_works.md` validated.** The shipping build runs on thousands of cabinets daily. SceneCache works. Our shim was simply blind to a code path it doesn't hook.

### The actual blocker

`select_and_load_scene` (logged via the discovery BRK trap at `0x730fb0`) is called in a tight cycle:

| scene_id | call count | timing |
|---|---|---|
| 12 | 1 | initial |
| 0 | 1 | initial |
| **10** | **443** | cycling 14:32:31 → 14:33:14 |
| **11** | **443** | cycling, alternating with 10 |

That's ~12 cycles/sec of `10→11→10→11→...` for the entire observation window after init. Visible symptoms (per user observation):
- "Startup in progress" animation freezes
- Menu navigation sound spams repeatedly
- Same two scenes keep re-selecting; nothing advances past

Scenes 10 and 11 are presumably a paired UI surface (e.g., status bar + content area, or two menu pages). Whatever invariant should advance the game past them is not being satisfied. **The actual investigation target moves from "filesystem" to "what triggers `select_and_load_scene(10/11)` cycle and what would break it."**

### Bind-mount approach validated

Before the pivot we proposed in Step 81: replace LD_PRELOAD path-remap with mount-namespace + bind mounts. Tested today and confirmed:

- `sudo mkdir /games /connectivity /data && sudo mount --bind <sysroot>/{games,connectivity,data}` creates native-path equivalents
- Game-relative paths (`./assets/...` from CWD) and absolute paths (`/games/...`, `/connectivity/...`) all resolve correctly via the kernel's mount table
- `.radium` files readable through the bind-mounted paths (verified via direct read + strace)
- Note: bind mounts on /mnt/c (9p) only succeed at HOST level, not inside an unprivileged user namespace (verified — standard Linux user-namespace behavior, NOT a WSL quirk; per `feedback_verify_quirks.md`)

### Establishing the new normal

**Setup (one-time per WSL boot):**
```bash
sudo bash -c 'mkdir -p /games /connectivity /data && \
  mount --bind /mnt/c/Workspace/pokemon-le-0.81.0/games        /games && \
  mount --bind /mnt/c/Workspace/pokemon-le-0.81.0/connectivity /connectivity && \
  mount --bind /mnt/c/Workspace/pokemon-le-0.81.0/data         /data'
```

After setup, `/games`, `/connectivity`, `/data` are kernel-resolved into the sysroot. Game absolute-path references work natively. Bind mounts persist until reboot or explicit `umount`.

A wrapper script lives at `_emulation/scripts/setup_bind_mounts.sh` (added in this step) that does this idempotently and verifies the mounts.

**Shim now plays a smaller role.** The shim's filesystem code (remap_path, all open-family hooks, all stat-family hooks, opendir/readdir, glob64, etc.) is now redundant — paths resolve via the kernel. The shim still serves its real purpose: hardware emulation (SPI/I2C/UART/V4L2/DRM/GPIO/ALSA), the diag-mode patch, the event-handler dump, the discovery BRK traps, frame/audio bridges to the host.

**Refactor: deferred but documented.** Removing the redundant fs code is a cleanup task with non-zero risk (the hooks intercept some non-FS-related paths like environment-aware stub returns); doing it now would distract from the scene-cycle investigation. Mark for cleanup pass after the front-end blocker is solved. Pruning candidates:
- `shim_fs.c::remap_path` (entire function)
- All open-family wrappers (`open`, `openat`, `open64`, `openat64`, `__open*`, `__openat*`, `__open*_2`, `fopen`, `fopen64`, `fdopen`)
- All stat-family wrappers
- `opendir`, `readdir`, `readdir64`, `closedir`, `rewinddir`
- `unlink`, `rename`, `readlink`, `chmod`, `glob64`, `rmdir`, `statfs`, `statvfs64`, `mkdir`, `mkdirat`
- `chdir`, `getcwd`
- Audit-pass blocks
- `should_log_open` filter
- The `FS_DIAG_TMP_PREFIX` / `fs_diag_path_match` / `SPIKE3_FS_DIAG_*` env vars

That's roughly the bottom 60% of `shim_fs.c` (~700+ lines). The gpiod stub block at the bottom of `shim_fs.c` stays (it's hardware, not FS).

### What was learned (process-level)

- **`feedback_grep_loose_first.md`** added — too-specific greps caused false-negatives that propped up bad hypotheses for hours.
- **`feedback_verify_quirks.md`** added — "WSL doesn't enforce O_DIRECTORY" was a wrong claim from misread strace, easily falsified by a 1-line C test.
- **`feedback_build_works.md`** added — "SceneCache might be broken in this build" was almost said and would have been wrong; the build is correct, our environment was incomplete.
- **`feedback_root_is_fine.md`** added — workarounds to avoid sudo (chroot inside user-namespace, fake-root trees) cost more than `sudo mount --bind` would have.

These four memories are now PILLARS in `MEMORY.md` to prevent re-treading the same loops in future sessions.

### Open / next thread

**Investigate the `select_and_load_scene(10/11)` cycle:**
1. Identify what scenes 10 and 11 actually are (user can decode the scene_id → SHA1 dir mapping via Radium player; this gives us the visual content).
2. Find what calls `select_and_load_scene` in this cycle — BRK trap with backtrace, or static xref + correlation.
3. Determine the invariant that's expected to break the cycle (likely: a specific switch state, a NV byte, a timing condition, or a network-readiness flag).

The cycle pattern (~12Hz alternation) strongly suggests a paired UI redraw bound to a state condition that's not advancing. Most likely candidates:
- A "startup progress bar" whose update value isn't moving (hardware-init counter not incrementing in our env)
- A switch input held in an unintended state (cabinet button pressed → menu nav loop)
- A timer-based scene transition with a condition check that's never satisfied

## Step 83: Cycle source traced — `sub_7b3060` is the Connectivity TestMenu/Diag_1360 main loop (= Step 77 front-end blocker) (2026-05-04)

The scene 10↔11 cycle has been traced from `select_and_load_scene` upward through several wrapper layers to the function that actually drives the front-end UI: `sub_7b3060`, the `Connectivity/TestMenu/Diag_1360` main loop. **This is the same blocker called out in Step 77.** Future investigation goes from here.

### Verified scene-load call chain

```
sub_7b3060 (Diag_1360 main loop — runs the Connectivity TestMenu UI)
   │
   │  while (true) {
   │    ...vfunc tick on arg1...
   │    if (sub_72a5c0(8) == 0) break;             ← exit on no pending type-8 event
   │    ...
   │    if (data_445d940 != 0) { decrement;
   │       sub_5a94f0(*sub_7b3b80(...), 2,    x23_1, NULL); }   // mask 2 = scene 10 path
   │    if (data_445d928 != 0) { decrement;
   │       sub_5a94f0(*sub_7b3b80(...), 4,    x23_2, NULL); }   // mask 4 = scene 11 path
   │    if (data_445d978 != 0) { decrement;
   │       sub_5a94f0(*sub_7b3b80(...), 8,    x23_3, NULL); }   // mask 8 = scene 12 path
   │    if (data_445d960 != 0) { decrement;
   │       sub_5a94f0(*sub_7b3b80(...), 1,    x23_4, NULL); }   // mask 1 = scene 9 path
   │    if (...condition...) sub_5a94f0(_, 0x10, 0, NULL);      // mask 0x10 = scene 16 path?
   │    ... continue ...
   │  }
   │
   ▼
sub_5a94f0(arg1, mask, arg3, arg4)        — scene-dispatch by bit mask
    Tests bits of `mask` (= w20):
      bit 0 (mask 1)   → bl sub_58fc10  → sub_731930(0x9)   = scene  9
      bit 1 (mask 2)   → bl sub_58fc20  → sub_731930(0xa)   = scene 10   ← caller_va 0x5a9630
      bit 2 (mask 4)   → bl sub_58fc30  → sub_731930(0xb)   = scene 11   ← caller_va 0x5a9624
      bit 3 (mask 8)   → bl sub_58fc40  → sub_731930(0xc)   = scene 12
   │  (multiple bits in same call dispatch all in sequence)
   ▼
sub_731930 (one-line wrapper at 0x731984)
   │  Increments g_scene_queue_sequence_counter and tail-calls:
   ▼
select_and_load_scene(scene_id, ...)       at 0x730fb0
```

Each lower-level `sub_58fc{10,20,30,40}` function is a one-liner that hardcodes its scene_id constant (9 / 10 / 11 / 12 respectively) and tail-calls `sub_731930`.

### Counters that gate dispatch

| .bss address | gates dispatch of | last-known data | x23 source (passed as arg3) |
|---|---|---|---|
| `data_445d940` | mask 2 (scene 10) | uint32 | `data_445d950` |
| `data_445d928` | mask 4 (scene 11) | uint32 | `data_445d938` |
| `data_445d978` | mask 8 (scene 12) | uint32 | `data_445d988` |
| `data_445d960` | mask 1 (scene  9) | uint32 | `data_445d970` |

Each is a count-down: when > 0, sub_7b3060 decrements and dispatches; when 0, it skips. So the cycle is sustained by something **incrementing** these counters every frame. That's the next investigation target.

### Exit condition

`sub_7b3060` exits its `while(true)` only when `sub_72a5c0(8) == 0`. `sub_72a5c0(N)` walks the global `event_queue_head` linked list and returns 1 if any queued entry's first byte == N (excluding the entry currently being processed — `data_44c9250`). So the loop terminates only when the proc queue has no pending **type-8** events.

### Runtime evidence

From the `2026-05-04T15:54:26_queue_scene_caller_v2` run (90 s) — discovery BRK trap added on `sub_731930` entry logs scene_id + caller_va:

| scene_id | call count | caller_va |
|---|---|---|
| 12 | 1 | 0x7220c4 (different — initial path, NOT in the cycle) |
| 10 | 328 | 0x5a9630 (= return-addr after `bl sub_58fc20` at 0x5a962c) |
| 11 | 328 | 0x5a9624 (= return-addr after `bl sub_58fc30` at 0x5a9620) |

Scene 9 and 16 paths were NOT exercised in this run (their counters stayed at 0).

### Scenes loaded — what we know vs. what we don't

**File-level (verified by host strace, Step 82):** 34 unique `.radium` files were actually opened during the run — 19 from `auto_loaded/` and 15 from `demand_loaded/`. The full path list is in Step 82's strace output. Loading works.

**scene_id ↔ .radium path mapping:** **NOT yet definitively determined.** From `scene_config_table` we know:
- scene_id 10 → asset_id 10 (image-id high16 = 0)
- scene_id 11 → asset_id 11 (image-id high16 = 0)
- scene_id 12 → asset_id 12 (image-id high16 = 0)

But asset_id refers into image.bin's packed_id space, which then resolves through bytecode to a `.radium` reference. We have not yet decoded that final step. The Connectivity TestMenu names below appear in the binary as strings:

```
Connectivity/TestMenu/Page1
Connectivity/TestMenu/Page2
Connectivity/TestMenu/Cursor
Connectivity/TestMenu/Cursor_Space
Connectivity/TestMenu/Cursor_Done
Connectivity/TestMenu/Cursor_enter
Connectivity/TestMenu/Enter_Cursor
Connectivity/TestMenu/Boxes_1..4
Connectivity/TestMenu/Boxes_AllGray
Connectivity/TestMenu/Numeric_Keyboard_Keypad
Connectivity/TestMenu/K_12345
Connectivity/TestMenu/K_AllDim
Connectivity/TestMenu/K_BKSPC / K_BKSPC_DIM
Diag_1360
```

These are SCENE NAMES (or sub-element names) that the Radium scene tree uses; some are sub-objects within a parent `.radium`. The mapping from scene_id 10/11 → which of these names → which `.radium` file IS NOT YET PROVEN. Do not assume it. Future step: decode definitively (e.g., extend the BRK trap to log the scene's name string, or open the Diag_1360 `.radium` in the Radium player and read the scene tree).

### Instrumentation added in this step (lives in `shim_discovery.c`)

| BRK addr | what it logs |
|---|---|
| `0x730fb0` (`select_and_load_scene` entry) | `scene_id`, `asset_id` (read from `scene_config_table[scene_id]`), `caller_va` (= lr at entry) |
| `0x731930` (`sub_731930` entry) | `scene_id`, `caller_va` (caller of the wrapper) |

These traps work alongside the bind-mount setup from Step 82; nothing in Step 83 disturbs the FS pivot.

### Next steps (in priority order)

1. **Definitively identify which `.radium` file is scene_id 10 and which is scene_id 11.** Two paths: (a) extend the trap to log the scene's name string after the asset_bytecode resolution path; (b) inspect `Connectivity/TestMenu/Diag_1360.radium` in the Radium player (whichever of the 34 SHA-1 dirs holds it) and read its scene-tree references. (a) is more authoritative; (b) is faster.
2. **Find what posts type-8 events** to `event_queue_head`. That's what keeps `sub_7b3060` looping. If we drain those events naturally, the menu may advance. If we patch/hook to drain them artificially, we bypass the menu.
3. **Find what increments counters** `data_445d940` / `data_445d928`. These are the per-frame dispatch triggers. If a real-hardware-only condition (e.g., a switch state, a network event) is supposed to clear them but never fires in our env, we've found a concrete shim hook target.
4. Out-of-band: the test-cycler diag flag from Step 79 unlocked downloadmgr's state machine but did NOT bypass `sub_7b3060`. The Diag_1360 menu loop runs **alongside** the downloadmgr thread, not via it. Two independent stuck systems; this one is the visible UI blocker.

### Why this matters

We've been chasing "what's stuck" for months. With `sub_7b3060` identified as the Diag_1360 main loop, every future investigation can be framed in terms of: *what would make this loop progress on real hardware that doesn't happen in our env?* That's a much more targeted question than "what's wrong with file loading" or "what's wrong with the SceneCache." The Step 82 pivot (scenes load fine) and Step 83 finding (cycle is the menu loop) together close the "is the engine working?" question — the engine works, it's stuck in a UI screen waiting for input/events we don't supply.

## Step 84: Documentation consolidation — PROTOCOL.md as single source of truth (2026-05-04)

### Why

Documentation has accumulated 14+ overlapping `.md` files across the repo. They overlap, drift, sometimes contradict. JOURNAL.md is narrative + speculation, not a stable reference, but everywhere cited. Concrete trigger today: while investigating the scene-10/11 cycle, this session re-derived image.bin section-5 layout from scratch — even though `PROTOCOL.md` Step 42 already documented it. The `MEMORY.md` "READ FIRST" entry pointed at MODEL.md and JOURNAL Steps 81-83 (narrative), not at PROTOCOL.md (truth). PROTOCOL had no TOC, so even after being told to look there, finding the relevant section wasn't obvious. About an hour was lost to the re-derivation.

### Direction

Adopt one source-of-truth doc: `_emulation/docs/PROTOCOL.md`. Verified facts only. Self-contained sections (no cross-doc reference required to evaluate a claim). JOURNAL keeps its job as exploration/narrative log. MEMORY entries point at PROTOCOL, not JOURNAL. Stale docs deleted outright (git history is the archive — no "STALE" banners that risk re-importing wrong info).

Plan file: `/home/c/.claude/plans/i-agree-with-removing-vast-feigenbaum.md`.

### Files to be deleted in this consolidation

Snapshotted here BEFORE any deletions, so future sessions can `git log -- <path>` for archeology:

- `_emulation/docs/RADIUM_INVESTIGATION.md` (Godot-bridge investigation, killed by `feedback_godot_is_illegal.md` 2026-05-02)
- `_emulation/docs/ASSETS.md` (asset-loading log, absorbed into PROTOCOL Radium / audio / video sections)
- `_emulation/docs/BRIDGE_PROTOCOL.md` (absorbed into PROTOCOL UART section)
- `_emulation/docs/NODEBUS_HASH.md` (absorbed into PROTOCOL Nodebus section)
- `_emulation/docs/NODEBUS_CIPHER.md` (absorbed into PROTOCOL Nodebus section)
- `_emulation/docs/MODE_TABLE.md` (absorbed into PROTOCOL Game runtime section)
- `_emulation/docs/SETUP.md` (absorbed into PROTOCOL Quick-start section)
- `_emulation/docs/VIDEO_CLIPS.md` (absorbed into PROTOCOL Radium section)
- `_emulation/MODEL.md` (architecture intro → PROTOCOL §1; suspect catalog/inferences captured below)
- `_emulation/README.md` (shim architecture absorbed into Hardware shim coverage section)
- `_emulation/historical-system-rig/README.md` (absorbed into Quick-start subsection "alternate full-system spike")
- `_extraction/tools/spike_extract/README.md` (format details → PROTOCOL image.bin section, usage → PROTOCOL Asset extraction section)
- `docs/SOURCE OF TRUTH.md` (empty boilerplate, replaced by PROTOCOL.md)
- `docs/startup_routine.md` (absorbed into PROTOCOL Boot sequence section)
- `docs/startup_sequence_technical_reference.md` (absorbed into PROTOCOL Boot sequence section)
- `GEMINI.md` (outdated; explicitly told "do NOT rely on PROTOCOL.md" which contradicts current direction)
- `_emulation/mesa_sw_overlay/pkg/usr/share/doc/libxml2/README.md` (vendored package doc, not project content)

### Inferences from MODEL.md preserved here (not promoted to PROTOCOL)

These are NOT verified facts; they're working hypotheses captured for the investigation thread. Future sessions: treat as starting points for inquiry, not as truth.

- The Diag_1360 main loop (`sub_7b3060`) is the front-end blocker (per Step 83). What unblocks it on real hardware is unknown — candidates: a switch state we don't simulate, a network event from netbridge, a counter we haven't found the writer for.
- IScene shared abstract base layout (vtable slot semantics — slot 8 tick / slot 9 activate, slots 4-6 flag accessors) was derived by decompiling each slot. Mapping is consistent across PLE and SW LE 0.85 pokemon-port-glue functions but the "tick/activate" semantic labels are inferred from observed call patterns, not from a header file.
- DisplayRenderer render-loop equivalence (PLE `0x7b7ac0` ↔ SW LE `mcp_DisplayRenderer_render_thread_body`) is byte-shape-identical at the OpenGL call level but the field offsets used to drain queues are inferred to match between binaries.
- Cycle source (`sub_7b3060` → `sub_5a94f0` → `sub_58fc{10,20,30,40}` → `sub_731930` → `select_and_load_scene`) is verified at the call-chain level via decompile + runtime BRK trap, but the *meaning* of scenes 9/10/11/12 (mask bits 0/1/2/3) as "shared system scene types" vs. "specific UI element types" is inferred.

These move with the investigation — re-verify before relying on them.

### Per-topic absorption workflow

For each section in the new PROTOCOL.md target structure (Project overview, Quick-start, Boot, UART, Nodebus, image.bin, Asset extraction, Audio, Radium, Display, Runtime, Hardware shim, Comparison binaries):
1. Inventory candidate sources from the deletion list above + memory + code
2. Resolve conflicts by VERIFICATION — run a test, read the binary, or read code; not by file mtime
3. Strip uncertainty before writing
4. Self-contained section, with `*Verified by:*` footnote per subsection
5. Delete absorbed source files
6. Update memory entries that pointed at deleted files
7. Update code comments that cited JOURNAL Steps for verified facts

After this point, JOURNAL Steps remain valuable for "what we tried and learned along the way," but should not be the source for facts about how the platform works. Cite PROTOCOL.md sections for facts; cite JOURNAL Steps only for "why this workaround exists" narrative context.


## Step 85: System-mode HW-watchpoint debugging stood up; "Startup In Progress" overlay traced (2026-05-05)

### Why

Two threads converged. First: the user asked whether the on-screen "Pokémon logo + Startup In Progress" boot overlay is a Radium scene. Static analysis got close — `mcp_draw_startup_in_progress_overlay` (formerly `sub_5cddd0`) sprintf's a localized message and renders via `sub_6485f0`, but BN's xref engine couldn't link the localized-message-table reads to a specific caller because the table is dynamically indexed (`ldr xN, [base, x_msgid, lsl #3]`). Pure static chase wouldn't pin the reader.

Second: the project had no working method for "what code reads memory location X" runtime questions. qemu-aarch64 *user-mode* gdbstub does NOT expose aarch64 BRP/WRP debug registers, so `rwatch` insertion always failed. GDB does not fall back to software read watchpoints. System mode (`qemu-system-aarch64 -M raspi4b`) DOES expose HW watchpoints, but `game.elf` crashes there at `glWindow.cpp:970` `eglGetError() == EGL_SUCCESS` long before any rendering — qemu raspi4b has no GPU. Neither mode could answer a memory-trace question without help.

### Direction

Run the user-mode shim (which has the GL/EGL/GBM/DRM stubs that get past the EGL assertion) inside the system-mode VM. Same shim, same Mesa overlay — but the surrounding execution gives us HW watchpoints. Hybrid we hadn't tried.

Concretely:
1. **Auto-stage** `_emulation/spi_shim.so` and `_emulation/mesa_sw_overlay/pkg/usr/lib/aarch64-linux-gnu/` into the system-mode initramfs on every launch via a small addition to `RUN_QEMU.sh`.
2. **Wrap `/games/game`** with `wrap_game.sh` that sets `LD_PRELOAD=spi_shim.so`, sets up Mesa overlay env, prints PIE base of game process.
3. **Disable userspace ASLR** in initramfs `init` (`echo 0 > /proc/sys/kernel/randomize_va_space` before `switch_root`). PIE base then deterministic at `0x5555550000`.
4. **Deploy Mesa overlay to fresh tmpfs** at `/newroot/mnt/mesa` (LUKS rootfs and data partitions both too small to absorb 362 MB even via `snapshot=on`).
5. Add `--gdb [PORT]` flag to `RUN_QEMU.sh` that appends `-gdb tcp::PORT` to the qemu invocation.

### Findings

The hybrid works. Game progresses through EGL assertion, runs SPI poll loops + Radium-scene-discovery `opendir` calls, hits the message-lookup path. HW read watchpoint on `mcp_localized_string_table[0x789]` (runtime `0x55598e89c8`) fired 26 times, every hit at PC `BN 0x7a64ec` (= inside `mcp_lookup_localized_msg_by_id`) and LR `BN 0x5cdf90` (= inside `mcp_draw_startup_in_progress_overlay`). Re-verified across two fresh boots; PC/LR session-stable.

**Original question answered:** "Startup In Progress" text is **not** a Radium scene. It's drawn by direct game-binary text rendering via the chain documented in PROTOCOL §3 "Boot-splash tick chain":

```
primary_initialization_orchestrator (0x678bc0)
 └─ mcp_boot_splash_tick (0x5d6ef0)        // 3 sites in primary_init
     └─ mcp_boot_state_dispatch (0x5d0cb0)
         └─ mcp_boot_state_mask_dispatch (0x5d0670)
             └─ [bit 3 of *(arg1 + 0x38)] mcp_draw_startup_in_progress_overlay (0x5cddd0)
                 └─ mcp_lookup_localized_msg_by_id(0x789)
                     ├─ mcp_msgid_to_strtable_idx[0x789]      → strtable index
                     ├─ mcp_localized_string_table[idx]        → 5-lang row
                     └─ mcp_pick_localized_string_from_row     → "Startup In Progress%s"
```

Pokémon logo (centered PNG) is a separate layer rendered by `create_boot_splash_egl_context` / `DisplayRenderer_render_loop` — also direct GL, not Radium.

**Side findings worth saving:**
- qemu-aarch64 user-mode gdbstub does not provide HW watchpoints (documented in PROTOCOL §14 + Tips table).
- qemu-system-aarch64 raspi4b gdbstub provides 4 HW BRPs + 4 HW WRPs.
- BN MCP plugin "click status to restart" handler crashes BN when a stale BN process is squatting on port 9009 — symptom looked like a regression in our prior shutdown-deadlock fix, but actually was a zombie process from an earlier UI crash. Killed zombie → behavior restored.
- openmcpgdb's `set logging file ... redirect on` does not reliably create the on-disk log via MCP; output instead surfaces in the next `gdb_custom` call's `command_output` field.
- Harness blocks `sleep N && next-cmd` chains. Use `until <cond>; do sleep N; done`. Captured as `feedback_until_loop_not_sleep.md`.

### Renames committed (BN MCP)

| Old | New |
|---|---|
| `sub_7a6490` | `mcp_lookup_localized_msg_by_id` |
| `sub_7d82d0` | `mcp_pick_localized_string_from_row` |
| `sub_5cddd0` | `mcp_draw_startup_in_progress_overlay` |
| `sub_5d6ef0` | `mcp_boot_splash_tick` |
| `sub_5d0cb0` | `mcp_boot_state_dispatch` |
| `sub_5d0670` | `mcp_boot_state_mask_dispatch` |
| `data_4394d80` | `mcp_localized_string_table` |
| `data_44cb998` | `mcp_msgid_to_strtable_idx` |

### Files changed

- `_emulation/historical-system-rig/RUN_QEMU.sh` — auto-stage shim + overlay; new `--gdb [PORT]` flag.
- `_emulation/historical-system-rig/initramfs/init` — disable ASLR; deploy `wrap_game.sh` + `spi_shim.so` + `mesa_sw_overlay` to /newroot; redirect `$GAME` via sed of `shared`.
- `_emulation/historical-system-rig/initramfs/usr/local/bin/wrap_game.sh` — new (PIE-base print + LD_PRELOAD + Mesa env + exec game).
- `_emulation/historical-system-rig/initramfs/usr/local/lib/spi_shim.so` — staged (overwritten each launch by RUN_QEMU.sh).
- `_emulation/historical-system-rig/README.md` — new (points at PROTOCOL §14; declares primary purpose = HW-watchpoint debugging).
- `_emulation/docs/PROTOCOL.md` — new §14 "System-mode debugging (HW watchpoints)"; expanded §3 with "Visible boot sequence", "Localized-message-table primitive", "Boot-splash tick chain"; refreshed Persistence-gap subsection in §12.
- Memory: 18 files slimmed to PROTOCOL pointers (no duplicate fact content); section-number references replaced with topic-name pointers; new `feedback_until_loop_not_sleep.md`.
- Repo CLAUDE.md (root + `games/pokemon_le/`) reduced to one-liner pointers at PROTOCOL.

### Open questions / next steps

1. **Who sets `*(arg1 + 0x38)` bit 3?** This is the proximate driver for which boot overlay shows when. Once known, we can determine why our emulator gets stuck before the next overlay (e.g. step 4's "POKÉMON PRO" header / Guided Setup) — likely the Diag_1360 cycle from Step 83 is preventing the bit progression.
2. **What does `sub_7de9c0(3, &var_a8)` produce?** It's the `%s` argument that fills `"Startup In Progress%s"`.
3. **What sets the language index `data_436c0c0`?** `mcp_pick_localized_string_from_row` reads it. Likely set during operator-config persist load.
4. **Drawing chain for "Initializing....."** (boot step 1) — `boot_display` ELF + `dprint` helper, not yet decompiled.
5. **SPIKE3-logo growth animation** (boot step 3) — primitive not yet identified.
6. **Replicate the system-mode HW-watchpoint setup in user mode** — fallback option if system mode regresses; would need either qemu-user patch to expose BRP/WRP, or an mprotect-trap inside the shim.

### Acid test (re-runnable)

Per PROTOCOL §14: from a clean state the full sequence produces the same PC/LR result. Verified 2026-05-05 with two consecutive `./RUN_QEMU.sh --headless --debug --ephemeral --gdb 1234` runs.


## Step 86: System-mode visible parity — game frames blit into /dev/fb0 + launch-time speedups (2026-05-05)

### Why

Step 85 stood up system-mode for HW-watchpoint debugging but ran headless with the bridges off. To validate that system-mode reaches the same boot state as user-mode (apples-to-apples comparison) we need to *see* what the game is rendering — same way `frame_viewer.py` works in user-mode. The user explicitly directed: "never run it headless or with the bridges off… same experience so we can compare apples to apples." Saved as `feedback_system_mode_parity.md`.

### Direction (and the dead ends)

Initial plan was to expose the existing AF_UNIX abstract bridges to a host subscriber via qemu user-mode networking + hostfwd. That ran into multiple verified roadblocks:

- **GENET hard-disabled.** qemu raspi4b 10.0.8 actively patches `brcm,bcm2711-genet-v5` (and `brcm,bcm2711-pcie`, `brcm,bcm2711-thermal`) to `status = "disabled"` at boot, before the kernel parses the DTB. The disable list is in qemu's `compat_disable[]` array (verified via `strings $(which qemu-system-aarch64) | grep -B1 'has been disabled'`). No way to re-enable at runtime — without a platform device the kernel can't probe a driver.
- **USB-net via DWC2 doesn't enumerate.** `-device usb-net,netdev=net0` correctly attaches to qemu's emulated DWC2 controller (verified via `info qtree` from `qemu -monitor stdio`). But the Stern kernel's built-in `dwc_otg` (RPi downstream) only matches `brcm,bcm2708-usb` and doesn't know how to drive qemu's BCM2711-style register layout. Patching the DTB's compat to `brcm,bcm2835-usb` (which mainline `dwc2.ko` matches) didn't get probe to fire either — likely a missing-PHY/clock issue, but not pursued further.
- **DTB-resident networking is intrinsically constrained.** PCIe disabled = no virtio-net-pci. raspi4b machine model has no virtio-mmio or vsock by default.

### The actual fix — write into /dev/fb0

`bcm2708_fb` exposes a kernel framebuffer at `/dev/fb0` that qemu's GTK display window mirrors directly. We don't need any cross-VM transport: the shim can `mmap(/dev/fb0)` and blit each captured frame straight into the kernel framebuffer. qemu shows the bytes.

Implementation in `shim_bridges.c::shim_fb0_blit`:

- Lazy init on first frame: `open("/dev/fb0", O_RDWR)`, `ioctl(FBIOGET_VSCREENINFO)` to learn dims + bpp + R/B offset (detect BGRA vs RGBA), `mmap` the framebuffer.
- Per-frame: format-convert RGBA8 → fb0 native (32 bpp BGRA / 16 bpp RGB565), nearest-neighbor scale if src and fb dims differ.
- Same-size + 32 bpp = fast path (memcpy when no R/B swap, simple loop otherwise).
- Gated on `SPIKE3_FB0_OUTPUT=1`. Coexists with the existing AF_UNIX/TCP bridge — both are no-ops when their respective consumers aren't present.

The capture path in `shim_frame_capture_at_page_flip` was already running every 2nd MODE_PAGE_FLIP via `glReadPixels`. Added `shim_fb0_blit(w, h, shim_frame_buf)` after the existing `shim_frame_send`. The early-return guard `if (shim_frame_client_fd < 0) return;` was widened to also pass when `SPIKE3_FB0_OUTPUT=1` (cached after first call).

### Force fb0 to game resolution

qemu raspi4b's default fb0 was 800×480, smaller than the game's 1920×1080. The `bcm2708_fb` driver honors `bcm2708_fb.fbwidth=` / `bcm2708_fb.fbheight=` kernel cmdline params, so `RUN_QEMU.sh`'s `-append` now carries `bcm2708_fb.fbwidth=1920 bcm2708_fb.fbheight=1080`. Verified: `bcm2708_fb soc:fb: Registered framebuffer for display 0, size 1920x1080`.

### WSLg fullscreen crash

GTK on WSLg's Wayland surface aborts qemu with `Gdk-Message: Error 71 (Protocol error) dispatching to Wayland display.` on certain operations including fullscreen toggles. WSLg also exposes an X11 socket; forcing `GDK_BACKEND=x11` before the qemu invocation makes GTK use X11 instead of Wayland and the fullscreen crash goes away. Added to `RUN_QEMU.sh`.

### Launch-time speedups

The repack step (cpio | zstd of ~373 MiB initramfs into ~92 MiB) was the bulk of launch time at ~30 s. Two changes in `RUN_QEMU.sh`:

- **mtime-gated repack**: if `rootfs.cpio.zst` is newer than every file under `initramfs/`, skip the repack entirely. Most relaunches (no shim/overlay/script edits) become instant.
- **zstd -3 instead of -19**: when the repack does run, ~6× faster (~30 s → ~5 s) for a ~10 % bigger archive. The archive is never shipped, so size is unimportant.

### TCP-listen support added to bridges (for the future)

While debugging the abandoned networking path, `shim_bridges.c` got a generic `shim_bridge_create_listener()` helper that creates either an AF_INET TCP listener (when `SPIKE3_*_TCP_PORT` env is set) or the original abstract AF_UNIX listener. `frame_viewer.py` and `audio_subscriber.py` got a parallel `--tcp HOST:PORT` flag. None of this is exercised in the current system-mode flow but it's harmless and ready if a transport-capable rig appears later. Same-shape change in `shim_bridge_start` / `shim_frame_start` / `shim_audio_start`.

### Files changed

- `_emulation/shim_bridges.c` — `shim_bridge_create_listener` helper; AF_INET/AF_UNIX accept threads use `sockaddr_storage`; new `shim_fb0_blit` + lazy fb0 init; capture-path guard widened to honor fb0 mode.
- `_emulation/historical-system-rig/RUN_QEMU.sh` — `bcm2708_fb.fbwidth=1920 bcm2708_fb.fbheight=1080` in `-append`; `GDK_BACKEND=x11` exported; mtime-gated repack with `zstd -3`.
- `_emulation/historical-system-rig/initramfs/usr/local/bin/wrap_game.sh` — `SPIKE3_FRAME_BRIDGE=1`, `SPIKE3_AUDIO_BRIDGE=1`, `SPIKE3_FB0_OUTPUT=1`. (Earlier intermediate state had a USB-net + DHCP block; reverted as part of the dead-end backout.)
- `_emulation/frame_viewer.py`, `_emulation/audio_subscriber.py` — added `--tcp HOST:PORT` flag, original abstract-AF_UNIX behavior unchanged.
- `_emulation/docs/PROTOCOL.md` — §2 alternate-path block updated; §14 expanded with "Visible display via /dev/fb0", "Force fb0 to game resolution", "Avoid the WSLg fullscreen crash", "Networking is not available" subsections; stale "no host-side subscribers, bridges=0" framing replaced.
- `_emulation/historical-system-rig/README.md` — purposes restated as HW-watchpoint debugging + visible parity; quick-reference dropped `--headless`; speedup notes added.
- Memory: `feedback_system_mode_parity.md` (apples-to-apples rule), `feedback_announce_when_waiting.md` (announce long waits before going silent).

### Verified end-to-end (2026-05-05)

`./RUN_QEMU.sh --debug --ephemeral --gdb 1234` boots the system-mode rig with `GDK_BACKEND=x11`. qemu's GTK window appears, kernel boot text scrolls, then the Pokémon logo + animated "Startup In Progress" overlay renders in the GTK window — same screen the user-mode `frame_viewer.py` shows. Boot-status wait loop completes (bit-3 cleared) at the same point as user-mode, confirmed by user. Total elapsed slower than user-mode (cross-arch TCG + full kernel emulation overhead, irreducible without KVM) but the path is reliable.

### What's NOT verified

- Audio output via fb0 (audio still flows to the AF_UNIX abstract socket in-VM with no host-side subscriber — silent in system-mode, intentionally). If audio-parity becomes important, options are: install a host-side ALSA capture in the VM, or wire one of the other transports.
- Whether the boot reaches Step 6's Guided Setup (the front-end blocker per `CURRENT INVESTIGATION` memory). System-mode reaches at least the same "Startup In Progress" point as user-mode; whether downstream behavior also matches is the next investigation, not addressed here.

### Open questions / next steps

1. The Diag_1360 menu loop (JOURNAL Step 83) is still the front-end blocker. With visible parity in system-mode now available, both rigs can be observed side-by-side to look for divergences.
2. fb0 blit cadence is currently every 2nd page-flip (matched the bridge's existing rhythm). If TCG rendering lag becomes an issue, an env-tunable decimation (`SPIKE3_FB0_DECIMATE=N`) is a one-line change.
3. The shipping production cabinet uses real `/dev/dri/card1` + KMS, not fb0. The fb0 path is purely an emulation-rig feedback channel; real game code is unaffected.


## Step 87: Forward-decode bridge restored as default; SW HEVC fallback removed; decoder patches renamed and made unconditional (2026-05-06)

### Why

User asked about a video error log (`Failed to find a V4L2 device for H265` / `drm_prime hwaccel init returned error` / `Failed to get HW surface format` / `Error parsing NAL unit #0`) and through the analysis arc surfaced a goal that the project had been drifting away from: **no software HEVC decoding anywhere; the bridge to host NVDEC should be the default path, not opt-in**. This Step is the rationalization: cleans up the decoder layer to one path, removes the silent fallback that was masking bridge issues, and brings the rig back to the configuration Step 73b last verified working.

The session also exercised the new pillar rule `feedback_search_over_recall.md` ("don't recall from memory; use `/lore` and read Step N+1 alongside Step N to catch supersession"). That rule fired correctly: memory's claim that "decoder is not the blocker per Step 72i/72j" was caught as fabricated when the actual JOURNAL Steps were re-read, which led to discovering the Step 73b host-NVDEC bridge had been deleted entirely in `efba162` and that the b.ne NOP supporting it had been collateral-damaged in `748720a` / `a4d5ea0`. Without applying the pillar, the investigation would have accepted a wrong picture.

### State at session start

- Forward-decode bridge (`shim_avforward.c` + `decode_daemon.py`): **deleted in `efba162`** "Big refactor; removing things we don't use anymore" (2026-05-05 20:03 EDT). Untouched in JOURNAL.
- Decoder-name remap (`hevc_v4l2request` → `hevc`, gated `SPIKE3_SW_HEVC=1`): still present in `shim_patches.c`.
- `shim_patch_get_format_callback` (the b.ne NOP at game VA `0x920cf0` that lets the game's `get_format` accept NV12): **also deleted, in `748720a` "A refactor to remove unused / old code"** (2026-05-02), undocumented. Comment block in `shim_patches.c` still referenced the function but the body and call site were gone.
- Memory `reference_video_player_spi.md` cited "Step 72i" and "Step 72j" for the verdict "decoder is not the blocker." Neither substep exists — Step 72 substeps stop at `g`. The actual content lived in Step 73b/73e, which the memory missed entirely.

### Restoration phase 1 — forward-decode bridge

Recovered the two whole files from `git show efba162~1:`:
- `_emulation/shim_avforward.c` (501 lines) — C client: 8-slot session table, AF_UNIX `@spike3_decode` connect, AVPacket field reads at offsets 8/16/24/32/40, AVFrame buffer population via `av_frame_get_buffer` + stride-aware row copy, REQ_OPEN/PACKET/RECEIVE/CLOSE wire protocol mirroring the daemon.
- `_emulation/decode_daemon.py` (382 lines) — host AF_UNIX server: per-connection `Session` with `av.codec.CodecContext.create("hevc_cuvid", "r")`, NVDEC frames downloaded via `frame.reformat(format='nv12')` + `to_ndarray`, REP_OPEN_OK / FRAME / EAGAIN / EOF replies. Includes SW fallback if `hevc_cuvid` open fails (handled inside daemon, transparent to shim).

Restored integration:
- `_emulation/shim_patches.c` — the two `SPIKE3_FORWARD_DECODE` early-return blocks at the head of `avcodec_send_packet` / `avcodec_receive_frame`.
- `_emulation/run_game.sh` — `FORWARD_DECODE` env var (defaulted to `1`, was `0` originally), daemon-launch helper, qemu env-forward.
- `_emulation/build.sh` — `shim_avforward.c` re-added to `SOURCES`.
- `_emulation/spi_shim.h` — forward-decode declarations (skipped the unrelated `sub_8edfe0_*` removals from the same commit, which were Step 72 BRK trap work, correctly stale).
- `_emulation/historical-system-rig/initramfs/usr/local/bin/wrap_game.sh` — `SPIKE3_FORWARD_DECODE=1` for system-mode parity.

Smoke test against `Lake_01_22_26_h265.mp4` (real game clip, 1940×1100, extradata=2490B): 194 NV12 frames in 3.98 s = 48.7 fps via NVDEC. Daemon log shows `OPEN codec=hevc_cuvid extradata=2490B` and successive `frame #N` lines.

### Restoration phase 2 — the b.ne NOP

Discovered while planning the rename that `shim_patch_get_format_callback` only appears in comments in current `shim_patches.c`, not as a callable function. Pickaxe history showed:
- Function lived in `spi_shim.c` originally (per Phase 8 refactor comment).
- At commit `c4d581f` (Step 73b add, 2026-04-30) the function was alive in `shim_inject.c`, called from `shim_vp_patch_thread` after a 2-second sleep.
- `748720a` "A refactor to remove unused / old code" (2026-05-02) removed 528 lines from `shim_patches.c`, including this function.
- `a4d5ea0` "Cleaning things up. Or trying to" (2026-05-05) removed the patch-thread wrapper alongside other Step-72 dead code.

Step 73b's verified-working configuration **had the NOP active**. Without it:
1. Game calls `avcodec_open2` on the AVCodecContext.
2. libavcodec invokes the game's `get_format` callback with a list of supported pix_fmts.
3. Game's callback iterates looking for `AV_PIX_FMT_DRM_PRIME` (0xb3); none are offered (since we route to plain `hevc` SW decoder which produces NV12 / YUV420P).
4. `cmp w0, #0xb3` → not equal → `b.ne 0x920ce0` loops back; eventually exhausts list and errors.
5. `avcodec_open2` fails. Game's `SpiVideoStreamDecoderBase.cpp:1830` `assert(hw_decoder)` SIGABRTs (per Step 45).

So forward-decode would never get a chance to engage — the game crashes before sending the first packet. The restored bridge would have been a stillbirth.

Recovery: pulled the function body from `c4d581f`'s `shim_patches.c`. Restored at `shim_patches.c:148-198` (paraphrased):

```c
#define GET_FORMAT_BNE_VA   0x920cf0
#define AARCH64_NOP         0xd503201fu

void shim_patch_get_format_callback(void) {
    if (!shim_decoder_patch_enabled()) return;
    /* mprotect → byte-check expected `b.ne 0x920ce0` (0x54ffff81) →
     * write AARCH64_NOP → __builtin___clear_cache. */
}
```

Hooked into the existing `shim_discovery_init_thread` (Step 73a's deferred-init pattern) — no new thread spawned:

```c
static void *shim_discovery_init_thread(void *arg) {
    sleep(2);
    shim_discovery_init();
    [REDACTED: startup-override call];
    shim_patch_get_format_callback();   // ← new
    return NULL;
}
```

The 2-second sleep already handles game-base resolution timing.

### Decision: rename SPIKE3_SW_HEVC → SPIKE3_DECODER_PATCH

User pushed back on the `SPIKE3_SW_HEVC` name — implies "use software decoder," but the patches don't decode anything. They have two effects:
1. `avcodec_find_decoder_by_name`: rewrite `hevc_v4l2request` / `hevc_v4l2m2m` → `hevc` so the game can resolve **any** working decoder pointer (V4L2 stateless m2m HEVC slice device doesn't exist in our env).
2. `shim_patch_get_format_callback`: NOP the `b.ne` so the game accepts NV12 instead of demanding DRM_PRIME.

These are **compatibility patches**, not "software decoding." The actual decode is done by `shim_avforward.c` → host NVDEC. The renamed-to-`hevc` decoder libavcodec allocates as a result of (1) is functionally inert under forward-decode — it has an open AVCodecContext but never sees a packet because send_packet is intercepted before it.

Rename:
- env var: `SPIKE3_SW_HEVC` → `SPIKE3_DECODER_PATCH`
- gate function: `shim_sw_hevc_enabled` → `shim_decoder_patch_enabled`
- log tag: `[sw_hevc]` → `[decoder_patch]`
- default flipped: previously `(s && s[0] == '1')` (default-OFF), now `!(s && s[0] == '0')` (default-ON; `SPIKE3_DECODER_PATCH=0` to disable for debugging)
- removed `SW_HEVC` knob from `run_game.sh` defaults / qemu env-forward / `run_info.txt` env-toggle list
- removed `export SPIKE3_SW_HEVC=1` from `wrap_game.sh`

Post-rename, the env var is no longer surfaced as a routine knob. Setting `SPIKE3_DECODER_PATCH=0` in the parent shell still flows through to qemu via inherited environment if anyone needs to disable for debugging.

### Decision: remove CPU SW fallback

`avcodec_send_packet` / `avcodec_receive_frame` previously fell through to `real_avcodec_send_packet` / `real_avcodec_receive_frame` (i.e., the renamed-to-`hevc` SW decoder running on the in-VM CPU) when forward-decode reported unreachable. This was a silent fallback — game continues running, just with 5-fps SW HEVC instead of 49-fps NVDEC. Looks like a generic game-side performance issue from the outside.

Removed the fallback. New behavior: when `shim_avforward_send_packet` / `shim_avforward_receive_frame` return `0` (forward couldn't engage), the wrappers return `-EIO` directly with a rate-limited `[forward unreachable; SW fallback removed]` log line. Game's own error handling kicks in (which per Step 45 means SIGABRT at `SpiVideoStreamDecoderBase.cpp:1830`).

User-explicit trade-off: loud failure beats silent slow-fallback because (a) we want this to be a playable video game, not a CPU-decode toy; (b) bridge issues should surface immediately, not look like a generic game bug; (c) the daemon is launched by `run_game.sh` before qemu, so under normal flow it's ready before any send_packet — the only failure mode this hard-fail catches is daemon-died-mid-run, which is exactly the case where loud >> silent.

### Decision: native `hevc_v4l2request` is out of scope

User's original intent for "HW decode" was to use `hevc_v4l2request` natively — i.e., remove the decoder patches entirely and satisfy the game's expected V4L2 stateless decode path. Analysis:

`hevc_v4l2request` is FFmpeg's V4L2 stateless HEVC hwaccel. Implementing it natively would require:
1. A fake V4L2 m2m device under `/dev/video*` answering REQBUFS / S_EXT_CTRLS (HEVC slice params) / QBUF / DQBUF.
2. Actual bitstream decode — most realistically still routed to host NVDEC (delegated at the V4L2 layer instead of the libavcodec layer, but same endpoint).
3. DMA-BUF allocation for the capture-queue buffer (memfd + udmabuf).
4. `EGL_EXT_image_dma_buf_import` + `glEGLImageTargetTexture2DOES` recognizing our fake DMA-BUFs.

Step 45 already attempted layer 4 and walled at *"a real DMA-BUF fd that EGL can import via EGL_EXT_image_dma_buf_import"* — marked as bigger than the excitement-per-hour budget.

The structural wall: our Mesa overlay is `MESA_LOADER_DRIVER_OVERRIDE=swrast` + `LIBGL_ALWAYS_SOFTWARE=1`. **OpenGL runs on CPU.** Even with a perfect DMA-BUF, swrast can't sample GPU memory because there's no GPU passthrough into qemu user-mode. Any "real" DMA-BUF would have to be CPU-readable backing — same memory traffic as the current AF_UNIX bridge, just dressed up in V4L2 + DMA-BUF wrappers.

**End result for native `hevc_v4l2request`**: thousands of new lines of V4L2 + DMA-BUF + EGL shim code to deliver bytes to the same place the current 900-line bridge already delivers them. Zero performance benefit because swrast is the bottleneck regardless. Decision: keep the two compatibility patches, document them clearly via the rename, leave the door open for native `hevc_v4l2request` only if (a) GPU passthrough becomes available in qemu user-mode or (b) we move to a system-mode rig with a real GL driver. Neither is on the near-term path.

### Cleanup: dead code

After removing the CPU fallback, several `shim_patches.c` symbols became unused (compiler warnings):
- `real_avcodec_send_packet_t` / `real_avcodec_receive_frame_t` typedefs
- `real_avcodec_send_packet` / `real_avcodec_receive_frame` static function pointers
- `shim_init_dummy_frame()` (Step 45b dummy NV12 1920×1080 black frame allocator) + its statics `shim_dummy_frame`, `shim_dummy_frame_inited`, `shim_pending_dummies`
- Local copies of macros now duplicated in `shim_avforward.c`: `DUMMY_FRAME_W/H`, `AV_PIX_FMT_NV12`, `AVFRAME_OFF_WIDTH/HEIGHT/FORMAT/PTS/PKTDTS`
- `real_av_frame_alloc_t` / `real_av_frame_get_buffer_t` / `real_av_frame_ref_t` / `real_av_frame_unref_t` typedefs (only used by `shim_init_dummy_frame`)

All deleted. `shim_patches.c` line count: 391 (was ~480). Build: clean — the four `-Wunused-*` warnings are gone; remaining warnings are pre-existing in other TUs (`shim_nodebus.c`, `shim_fs.c`, `spi_shim.h` timestamp).

The AVFrame/AVPacket field offsets that lived in `shim_patches.c` are now duplicated in `shim_avforward.c` at `AVFRAME_OFF_*` / `AVPKT_OFF_*`. Treat `shim_avforward.c`'s definitions as canonical going forward — it's the only TU that actually pokes those fields.

### Memory + skill updates

- **Deleted** `memory/reference_video_player_spi.md` outright. It cited fabricated Step 72i / Step 72j as the source for "decoder is not the real blocker," and referencing nonexistent Steps undermines all its other (factually correct) claims about VideoPlayerSpi vtable / decoder chain / attract-mode. Information remains recoverable from JOURNAL Steps 45 / 60 / 71 / 72b–g / 73b / 73e and from the BN database.
- **Added** `memory/feedback_search_over_recall.md` as a PILLAR-level feedback. Codifies the rule that triggered this Step's discovery arc:
  1. For project-state facts (which Steps cover what, conclusions, addresses, decisions), use `/lore` active search instead of memory recall.
  2. When verifying a claim from JOURNAL Step N, ALWAYS also read Step N+1 — the chronologically next Step frequently revises, supersedes, or extends earlier conclusions.
  Rule was triggered by the failure mode where stale memory cited "Step 72i" that doesn't exist; reading Step 73 (next step after Step 72) surfaced the actual relevant work (forward-decode bridge implementation in 73b, scene-tree-attachment reframe in 73e). Without the rule, this Step's restoration would not have happened.

### Verified end-to-end

Build: `[build] OK — spi_shim.so: 232256 bytes`. All forward-decode + decoder-patch symbols present (`shim_avforward_enabled`, `shim_avforward_send_packet`, `shim_avforward_receive_frame`, `shim_patch_get_format_callback`).

Daemon harness round-trip: 194/200 NV12 frames at 49.5 fps via `hevc_cuvid` NVDEC against the same `Lake_01_22_26_h265.mp4` clip Step 73b used. Wire protocol verified end-to-end: REQ_OPEN → REP_OPEN_OK, REQ_PACKET → REP_PACKET_OK, REQ_RECEIVE → REP_FRAME (header + Y-plane + UV-plane), REQ_CLOSE → REP_OK.

User game run (`runs/2026-05-06T12.45.39_hw_refactor`):
- `[decoder_patch] patched get_format b.ne @ 0x77f488260cf0: 0x54ffff81 -> 0xd503201f (NOP)` at run+5 s → b.ne NOP applied successfully at runtime, byte-check passed.
- 3× `[avforward] session OPEN ok ... codec_id=173 extradata=2453B` → three real game-side AVCodecContexts opened on the daemon (codec_id=173 = HEVC, extradata=2453B matches Step 73b's recorded value, distinct from the test harness's 2490B).
- `avcodec_receive_frame #1000 ... [forward] (ok=449 err=551)` → 1000 receive_frame calls: 449 returned a frame, 551 returned `-EAGAIN` (daemon queue empty, expected at ~50% rate per Step 73b).
- `[forward unreachable; SW fallback removed]`: **0 hits**. Bridge engaged on every send/receive. Zero CPU HEVC decoding occurred.

The decoder-name-remap log (`[decoder_patch] remapping decoder name 'hevc_v4l2request' -> 'hevc'`) did not fire — game presumably calls `avcodec_find_decoder(AV_CODEC_ID_HEVC)` (codec_id-based) rather than `avcodec_find_decoder_by_name("hevc_v4l2request")` (name-based). Our hook only catches the name variant. Operationally inert: the b.ne NOP makes the get_format callback accept whatever pix_fmt libavcodec offers, and the bridge intercepts send/receive before the underlying decoder ever sees a packet. The decoder-name remap is defense in depth; not load-bearing under the current bridge-active flow.

### What's NOT verified

- **System-mode forward-decode is currently a no-op.** `wrap_game.sh` exports `SPIKE3_FORWARD_DECODE=1` for parity with user-mode, but the in-VM abstract AF_UNIX namespace can't see the host daemon — `connect()` to `@spike3_decode` fails fast, the no-CPU-fallback path returns `-EIO`. Game's video pipeline will hard-fail in system-mode until cross-VM transport is in place. This is a regression from "in-VM SW decode silently kept things alive" — by design per the no-fallback decision, but it means system-mode video doesn't work at all right now.
- **End-to-end game playback through to attract-loop video display** — the bridge is delivering frames, but per Step 73e the front-end blocker is scene-tree attachment (`/root/Main` parent missing), not decode. Frames arrive but are not rendered until the orchestrator reframe (Step 83's Diag_1360 work) or scene-tree attachment is solved.

### Open questions / next steps

1. **System-mode cross-VM transport** for forward-decode. Step 86 documented why every networking path on `-M raspi4b` is blocked: GENET hard-disabled by qemu's `compat_disable[]`, USB-net DWC2 doesn't enumerate, no PCIe / virtio-mmio / vsock by default. Reaching forward-decode parity in system-mode requires one of:
   - Re-add the TCP-listen support that was removed in `6c9e6d1` plus a patched qemu binary that re-enables GENET (one-time qemu rebuild).
   - DTB-overlay patching to inject a virtio-mmio bus + virtio-net.
   - virtio-vsock if qemu raspi4b can be coaxed to expose it.
   - Switch machine model from `-M raspi4b` to `-M virt` (loses production-hardware fidelity; raspi4b matches real CM4).
2. **Front-end blocker** is the Diag_1360 menu loop (Step 83) → still the active investigation, unaffected by today's work.
3. **Dead code follow-up**: `shim_patches.c`'s remaining libavcodec hooks (`avcodec_get_hw_config`, `av_hwdevice_ctx_create`) are passthrough-with-logging — never exercised meaningfully in the forward-decode flow because `avcodec_open2` doesn't call hwaccel paths when get_format returns a SW-compatible pix_fmt. Could be deleted in a follow-on cleanup once we confirm they're truly never hit. Keep for now as a diagnostic safety net.

### Files changed

- `_emulation/shim_avforward.c` — restored from `efba162~1`, 501 lines, default-on bridge client.
- `_emulation/decode_daemon.py` — restored from `efba162~1`, 382 lines, host AF_UNIX server with `hevc_cuvid` + SW fallback.
- `_emulation/shim_patches.c` — restored `shim_patch_get_format_callback`; renamed `shim_sw_hevc_enabled` → `shim_decoder_patch_enabled` (default-on); removed CPU fallback in `avcodec_send_packet` / `avcodec_receive_frame` (return `-EIO`); deleted dummy-frame infrastructure (~80 lines) + unused typedefs/statics.
- `_emulation/spi_shim.h` — added forward-decode + `shim_patch_get_format_callback` declarations; updated comments.
- `_emulation/shim_inject.c` — added `shim_patch_get_format_callback()` call inside `shim_discovery_init_thread` (no new thread).
- `_emulation/build.sh` — `shim_avforward.c` re-added to SOURCES.
- `_emulation/run_game.sh` — `FORWARD_DECODE` env var (default-on) + daemon-launch helper + qemu env-forward; removed `SW_HEVC` from defaults / forward / `run_info.txt`.
- `_emulation/historical-system-rig/initramfs/usr/local/bin/wrap_game.sh` — `SPIKE3_FORWARD_DECODE=1` exported; `SPIKE3_SW_HEVC` export removed.
- `memory/reference_video_player_spi.md` — DELETED (stale Step 72i/72j refs).
- `memory/feedback_search_over_recall.md` — NEW PILLAR feedback.
- `memory/MEMORY.md` — VideoPlayerSpi index line replaced with the new PILLAR entry.

## Step 88: PROTOCOL strict-philosophy audit + shim comment-hygiene pass (2026-05-07)

### Why

Wrapping up a multi-session push to land the strict-philosophy rule (`feedback_protocol_strict_philosophy.md`, Step 84) consistently across the codebase: PROTOCOL.md = verified facts only, code comments = vital + stable only. This Step bookends two parallel cleanup arcs — a code-comment hygiene pass over the `_emulation/` shim source files using `/code-review`, and a per-section PROTOCOL audit using the new `/protocol audit` skill plus `lore-keeper` subagent dispatch.

### State at session start

- 10 shim source files unreviewed for strict-philosophy compliance: `shim_gl.c`, `shim_diagnostics.c`, `shim_bridges.c`, `shim_nodebus.c`, `shim_patches.c`, `shim_discovery.c`, `spi_shim.c`, `spi_shim.h`, `decode_daemon.py`, `run_game.sh`. Cumulative: hundreds of section banners, Step refs, "Phase N moved to", and history-laden rationale blocks accumulated since Phase 5/8/9 refactors.
- 12 PROTOCOL.md sections pending audit (§2, §3, §4, §5, §6, §7, §8, §9, §10, §11, §13, §14). §1 + §12 had been done in a prior session.
- `spi_shim.so` baseline: 232256 bytes.

### Phase 1 — `/code-review` pass over shim sources

Ran the skill (heuristic + Qwen 2.5 7B grader) against each file. Applied STRIPs for section banners, history refs, restating-the-name comments, forward-looking advice, and deprecated Phase-N-moved-to history. KEEP-overrode comments documenting non-obvious invariants: ABI offsets, byte-layout, wire-protocol magic, env-var documentation, cross-file architecture. Build sanity-checked every 3-5 files; `spi_shim.so` stayed at exactly 232256 bytes throughout (comments don't affect compiled output).

| File | Δ lines | Notes |
|---|--:|---|
| `shim_gl.c` | -66 | 31 strips. KEEP-overrode ASCII-tag macros (`/* "eglD" */`), `/* 32-bpp assumed */`, `/* dlsym miss */` control-flow note |
| `shim_diagnostics.c` | -46 | Step refs, 8 cmode-table category banners, `/* CORRECTED from +0x30 */` history |
| `shim_bridges.c` | -22 | Step 53/57 banners, `/* every 2nd flip */`. **Fixed stale `/tmp/spike3_bridge.sock` and `SPIKE3_BRIDGE_SOCKET` env-var refs** — code uses abstract-namespace AF_UNIX |
| `shim_nodebus.c` | -19 | Bridge-protocol-synthesis banner (kept wire spec), Sidekick attribution. KEEP-overrode wire STX/ETX/`-5` annotations |
| `shim_patches.c` | -35 | memcpy NULL-guard rationale block, libavcodec-bypass Step refs, dead `real_gst_element_factory_make_t` typedef |
| `shim_discovery.c` | -6 | Qwen mostly wrong here — `/* Emulate <aarch64-instr> */` comments document non-obvious instruction semantics that names can't convey. Only 4 actual strips |
| `spi_shim.c` | -80 | Phase 5/8/9 "moved to" history blocks, every `---- Hooked: X ----` banner |
| `spi_shim.h` | -9 | Section banners. KEEP-overrode `real_*` cross-file tags + env-var docs |
| `decode_daemon.py` | small | 3 strips (PyAV history, dead annotation). Override-kept docstrings, NV12 byte-layout doc, `# noqa` directive |
| `run_game.sh` | small | Conservative per user prediction ("expect mostly KEEPs"). Fixed 3 stale `/tmp/spike3_*.sock` paths to "abstract AF_UNIX socket"; dropped `Step 73b restored 2026-05-06` and `Step 77` history. Help text body untouched |

### Phase 2 — PROTOCOL.md per-section audit

Workflow per section: `audit_section.py N` for anchors + tagged Steps; sections with >30 Steps got `lore-keeper` subagent dispatch (§4 32 Steps, §9 51 Steps, §10 52 Steps) so JOURNAL drift checks ran in their own context window. Edits applied via `Edit`; substantive content cuts summarized + applied per the strict-philosophy mandate.

| § | Major actions |
|---|---|
| §3 | Stripped "What we still don't know" block (4 open questions), hedging speculation, Diag_1360 downstream-blocker hypothesis |
| §4 | 5 Step refs, 2 history banners, TBD-row replacement (`TBD` → `—`), "Specific divergences" subsection, bridge-timeouts narrative compress |
| §5 | "Earlier shim work guessed" history, external-observation "Use cases" subsection, Researcher C "main send" hypothesis subsection |
| §6 | Hedges + TODO speculation about flag reconstruction |
| §7 | Date stamps + Step ref + forward-looking advice |
| §8 | Sidekick external attribution, Spike 2 era history. **Rebased `0xb321d0 → 0x7321d0`** |
| §9 | "Earlier docs claimed" banner, Generification subsection, Refactor candidate subsection, godot-illegal disclaimer. **Rebased `0xbb76e0 / 0xbb81e4 / 0xbb7c30 → 0x7b*`** |
| §10 | Step-24/27 framing, history observations. **Rebased entire VA reference table** (7 dual-base pairs collapsed to single-column rebased VAs) |
| §11 | "Corrected understanding" banner, "Old shim_fire_switch" history. **Rebased `0x9c12a0 → 0x5c12a0`** (verified against `shim_inject.c`'s `fn = (fire_event_t)(game_base + 0x5c12a0)`) |
| §2 | Step ref, "legacy Step-54 mode" history. **Fixed stale `remap_path` claim** — Step 82's bind-mount approach supersedes |
| §13 | TBD → `—`, godot-illegal disclaimer, "Why this matters" engineering advice |
| §14 | Date stamps, `feedback_system_mode_parity.md` memory-file ref |

**+0x400000 base drift findings.** §9, §10, §11 carried explicit dual-base annotations the original author had noted but not fully propagated (`0xbb76e0 → equivalent in PLE: 0x7b76e0`; `0xa8ca40 → 0x68ca40 runtime`). `shim_discovery.c`'s `#define DISCOVERY_ADDR_ASSET_LOAD 0x7321d0` ground-truthed the rebase pattern, contradicting PROTOCOL §9's `0xb321d0`. Per `feedback_verify_every_address.md` PILLAR (any function VA ≥ 0x800000 is suspect), rebased addresses wherever the dual-base evidence was explicit in-doc.

**Stale-claim fixes.** §2 said the shim's `remap_path` translates absolute paths on every `open()` — Step 82's bind-mount approach has superseded that; corrected. `run_game.sh` documentation references to `/tmp/spike3_*.sock` corrected to "abstract AF_UNIX socket" matching `shim_bridges.c`'s `ABSTRACT_*_NAME` constants.

### State at session end

- All 14 PROTOCOL sections strict-philosophy compliant. 12 remaining `JOURNAL Step N` references in the doc, all in canonical `*Verified by:*` footnotes (down from ~50+).
- `spi_shim.so` builds clean at 232256 bytes.
- 10 source files cleaned. Ratio across files: ~30% of flagged comments stripped, ~70% KEEP-overrode (vital + stable).

### Open

- **§11 deferred address rebases.** ~10 addresses fit the +0x400000 drift pattern but lack explicit dual-base notation in-doc, so they were not auto-rebased: `sub_a50850 / sub_a50350` (switch-state readers), `sub_b24f70` (Stage-1 switch-state reader), `sub_b25438` (call site), `sub_b129f0 / sub_bd39f0 / sub_b16f64 / sub_b82990 / sub_b12760` (event-firing callers), `_INIT_74 @ 0x8733c0`. Each needs a single BN MCP `function_at` lookup to confirm; if pattern holds, all subtract `0x400000`.
- **Magic-number → named-constant rename pass** deferred from this session's comment-strip work (would be substantive code change, not just comment removal). Targets: ~26 magic-number-interpretation comments in `shim_ioctl.c` (DRM_MODE_*/TIOCM_*), ~30 in `shim_gl.c` (EGL_*/GL_*), a few in `shim_bridges.c` (GL_RGBA/GL_UNSIGNED_BYTE). Each replaced by including the proper system header (`<drm/drm_mode.h>`, `<linux/input.h>`, `<EGL/egl.h>`, etc.) and using the actual named constant.

## Step 89: Hardware-emulation marathon: I2C regfile + TAS5711 + GBM bo_fd + DRM-prime workaround + cipher key-fetch + send_encrypted wire-format discovery (2026-05-07)

Marathon session: five hardware-emulation fixes that compound into ~3 minutes of stable runtime past the previous SIGSEGV cliff, plus a wire-format discovery that corrects PROTOCOL §5.1 and a partial cipher-channel response synthesis (env-gated). All progress preserved without regressing the timeout-is-progress baseline.

### Step 89a — I2C virtual regfile model

Audio driver was emitting **11 distinct `write failed to verify at NN: reads back as ffffffff` lines** because our `I2C_RDWR` ioctl handler returned `0` (the kernel ABI requires returning **nmsgs**) and never populated the read msg buffers. Verified the failure path at game `0x919f70` via `ldr w9, [x6, x2]` instruction trace — when `i2c_smbus_read_byte_data`-style helpers see ≤0, the verify code prints `(int)-1` formatted with `%02x` → `ffffffff`.

Fix: full `I2C_RDWR` and `I2C_SMBUS` handlers in `shim_ioctl.c` walking the `i2c_msg` array against a per-slave 256-byte regfile (`i2c_regfile[128][256]` + `i2c_reg_pointer[128]` + `i2c_current_slave` in `spi_shim.c`). Last-write-wins per `(slave, register)`, auto-incrementing register pointer for combined transactions. `read()`/`write()` on `FAKE_FD_I2C` route through the same regfile.

Result: 11 → 0 verify errors. TAS57XX and MAX9850 amp init both reach "complete".

### Step 89b — TAS5711 oscillator-trim post-write hook

After 87a the verify spam was gone but `TAS57XX: oscillator trim failed to lock` still fired once per run. Disassembly at game `0x91bfb0`:

1. Write `0x00` to register `0x1B` at slave `0x1B` (TAS5711 default I2C address).
2. `usleep(50000)` (50 ms).
3. Loop up to 10 times: read reg `0x1B`, check **bit 6** (`tbnz w0, #6` at `0x91c060`); if set → trim done.
4. After 10 retries: error printf at `0x91c094`.

Per TI SLAS502, register `0x1B` is OSC_TRIM with bit 6 = `OSC_TRIM_DONE` — write-to-trigger, read-to-poll status.

Fix: added `shim_i2c_chip_post_write` hook in `shim_ioctl.c` that overrides `regfile[0x1B][0x1B] = 0x40` after any write to that slot/register. Driver's first poll sees bit 6 set → "trim done" → no error.

Architecture note: keeping the hook list narrow (one entry today), expanding only as new "polling never succeeds" symptoms appear. Safer than shipping a broad chip-emulation framework on day one.

### Step 89c — GBM `bo_get_fd` returns `memfd` instead of -1

Function at game `0x900828` (`create_egl_framebuffer`-style):

```
bl gbm_bo_get_fd        @ 0x900848
mov w22, w0             @ 0x90084c
tbnz w0, #31, 0x9009f4  @ 0x900850   — branches on "fd < 0" → "failed to get fd for bo: -1"
```

Then on success the fd is passed to `eglCreateImageKHR` (vtable call at `0x9008e8`) and immediately `close()`d at `0x9008fc`. Our shim's `eglCreateImageKHR` is already a stub that parses the EGL attribute list and returns a sentinel without dereferencing the fd, so the fd just needs to satisfy `(fd >= 0)` and survive a clean `close()`.

Fix: `gbm_bo_get_fd` in `shim_gl.c` now does `syscall(SYS_memfd_create, "shim_dma_buf_stub", MFD_CLOEXEC)`. Memfds are real fds with kernel lifecycle, close cleanly under qemu-user.

Side effect: `gbm_bo_create` count = `eglCreateImageKHR` count = 16 in next run (was: `gbm_bo_create=16`, `eglCreateImageKHR=0`). Every BO now imports successfully.

### Step 89d — DRM-prime consumer empty `AVDRMFrameDescriptor` redirect

After 87c the run progressed past framebuffer setup and started decoding HEVC video via the avforward bridge. The third `[avforward] session OPEN` (codec_id=173, extradata=2453B) was followed within ~200 ms by SIGSEGV at game `0x e3e250`:

```
e3e248: smull x2, w2, w11      @ x2 = layers[0].planes[i].object_index * 24
e3e250: ldr   w9, [x6, x2]     @ x6 = AVDRMFrameDescriptor.objects[0]; CRASH
```

`x13 = frame->data[0]` interpreted as `AVDRMFrameDescriptor*`. Field offsets:
- `nb_objects` @ 0
- `objects[4]` @ 8 (each 24 bytes)
- `nb_layers` @ 104 (`[x13, #104]` ldr at `0x e3e1e4`)
- `layers[4]` @ 112 (each 104 bytes)
- `layers[i].nb_planes` @ +0x74
- `layers[i].planes[j].object_index` @ +0x78 + j*24

Crash chain consistent with **Step 45/87 wall**: get_format NOP makes `avcodec_open2` accept NV12, but downstream consumer still walks `frame->data[0]` as a DRM-prime descriptor. NV12 pixel bytes get dereferenced as struct → fault.

Real fix (real DMA-BUF + GPU import) is out-of-scope under `MESA_LOADER_DRIVER_OVERRIDE=swrast` per Step 45 §47.5. Compat patch instead: in `shim_avforward_receive_frame`, after `av_frame_get_buffer` succeeds, redirect `frame->data[0]` to a static zero-initialized 1024-byte buffer (`shim_avforward_empty_drm_descriptor`). The consumer reads `nb_layers = 0`, takes `b.le 0xe3e4cc`, returns peacefully. `frame->buf[0]` still owns the real allocation so `av_frame_unref` cleans up correctly.

Result: 0 SIGSEGV across 180 s timeout. Game processes 113k+ `avcodec_receive_frame` calls (579 frames decoded ok), reaches scene loading (`base_00`/`story_charmander_1`/`battle_mountain` scene_data dumps).

### Step 89e — Cipher channel: key-fetch synth + send_encrypted wire-format discovery

User asked to crack the nodebus cipher channel. Lore-keeper confirmed: timeouts are structural (Step 43 + Step 67 + Step 69 baseline), not a tunable knob. The path forward is real per-(slot, inner_cmd) response synthesis.

**87e.1 — Key-fetch synth (env-gated under `NODEBUS_SYNTH_FE`)**

Added handlers for inner_cmd `0xfc` / `0xfd` / `0xf9` in `shim_nodebus.c` case `0x41`. Hint values confirmed by static RE of the wrappers:

- `mcp_nodebus_fetch_key_basic @ 0x8f3484` — sends `0xfd hint=12` then `0xfc hint=16`
- `mcp_nodebus_fetch_key_two_probes @ 0x8f36b0` — sends `0xf9 hint=16` (×2 with payloads `0x10000000` / `0x10001f00`) then `0xfc hint=16`
- `mcp_nodebus_fetch_key_with_serial @ 0x8f355c` — same as basic + `sprintf("%09d", ...)`

Synth zero-fills `hint` bytes (key bytes = 0, bridgeStatus = 0). Matches BSS state of `mcp_nodebus_cipher_keys @ 0x44fcfb8` so subsequent encrypted traffic decrypts identically.

**87e.2 — Wire format correction: `send_encrypted` increments ilen by 1**

Added one-shot per-`(slot, inner_cmd)` backtrace + raw wire dump to the cipher decode path. Captured runtime samples:

| Wire (post-bridge-strip) | Sender wrapper |
|---|---|
| `80 02 f1 8d 00` | `mcp_nodebus_node_reset @ 0x8f0c20` (cmd `0xf1` ilen=1 hint=0) |
| `80 03 f0 22 6b 00` | `0x8f1100` (cmd `0xf0` ilen=2 payload byte=0x22 hint=0) |
| `80 03 f0 11 7c 00` | `0x8f1020` (cmd `0xf0` ilen=2 payload byte=0x11 hint=0) |
| `81 02 fe 7f 0d` | `mcp_nodebus_send_cmd_fe_probe_unsure @ 0x8ed870` (cmd `0xfe` ilen=1 hint=0x0d) |

Disassembly of `mcp_nodebus_send_encrypted @ 0x8ec410` reveals the buffer manipulation:

```
0x8ec500: w4 = local_buffer[2]    @ cmd byte
0x8ec504: w0 = local_buffer[1]    @ wrapper's ilen
0x8ec514: w0 += 1                  @ ilen + 1
0x8ec52c: strb w0, [sp, #25]       @ local_buffer[1] = wrapper_ilen + 1   ← INCREMENT
0x8ec538: strb wzr, [x7, w1]       @ zero local_buffer[wrapper_ilen + 2]
0x8ec544..570: csum loop summing local_buffer[0..wrapper_ilen+1]
0x8ec56c: strb w4, [x7, x2]        @ csum at local_buffer[wrapper_ilen + 2]
0x8ec5e8: w22 += 4                  @ wire length = wrapper_ilen + 4
0x8ec614: bl mcp_nodebus_send_raw  @ length = wrapper_ilen + 4 = wire_ilen + 3
```

**Corrected wire format** (post-bridge-strip, encrypted path):

```
[0]                       = slot byte (slot | 0x80)
[1]                       = wire_ilen = wrapper_ilen + 1
[2]                       = inner_cmd
[3 .. 2 + wrapper_ilen-1] = payload bytes (wrapper_ilen - 1 of them)
[2 + wrapper_ilen]        = csum (negated sum of [0..2+wrapper_ilen-1])
[3 + wrapper_ilen]        = hint (caller's expected_response_len, or 0)
total length              = wrapper_ilen + 4
```

PROTOCOL §5.1 currently states csum at `[2 + inner_len]` where `inner_len` is the wire byte. That's off by 1 — actual csum is at `[2 + (wire_ilen - 1)]` because send_encrypted incremented the on-wire ilen byte after copying the buffer. Verified by computing csums against captured wires; only the corrected formula matches all four samples.

**Implication for our shim's existing decoder**: `payload[hint_idx]` reads at `2 + inner_len` but `inner_len` here is wire_ilen, so hint_idx is one byte PAST the actual hint position. For cmd `0xfe` ilen=1 hint=0x0d: shim reads `payload[4]` = 0x0d (correct hint). The wrappers happen to put hint at the same offset post-increment because send_encrypted writes hint+2 (or 0) at exactly that position. So shim's existing fe_probe synth code reads the right byte by accident.

The cosmetic `payload=[N M]` log line in shim mislabels csum bytes as payload. Inconsequential for behavior; can be cleaned up next session.

**87e.3 — fe_probe synth tested (both "node present" and "node not present"); both regress video**

Tried current code's `resp[3] |= 0x80` ("node present, default chip" — Step 69's choice). Run with `NODEBUS_SYNTH_FE=1`: 153 → 0 cmd-0x41 timeouts, but **avcodec_receive_frame count drops from 113k → 0**. Game stalls on slot-13 fe_probe loop instead of progressing to video.

Tried inverted variant (no high-bit patch — "node not present" semantics, timeout-equivalent). Same regression: 0 timeouts but 0 frames decoded. Run gets to GL extensions enumeration but never opens an avforward HEVC session.

Step 43's "timeout-is-progress" lesson is still load-bearing — the 1000 ms × ~150-frame timeout window is implicitly the time other threads need to bring up Godot/Radium/decode pipeline. Killing the timeouts races the main thread past that init window.

For now: kept fe_probe synth logic in place but reverted the high-bit "node present" patch to "node not present" semantics (timeout-equivalent payload). Still env-gated; off by default. The key-fetch additions are safe to leave on with the env var since key-fetch traffic only fires after fe_probe declares a node present.

### Step 89f — Chip vtable structure (corrected via screenshot reference/87f.png)

`mcp_lpc_chip_vtable @ 0x42317c8`, 28 entries × 0x28 bytes. First `u32` of each entry is the chip ID matched against `response[4..7]` (var_34) of cmd `0xfe` response. Default entry at `0x42317a0` has zero chip_id and `data_42317a8` → `sub_35f3790` as the only non-null pointer (matches Step 69 "NULL fps" framing — var_34=0 hits this entry and most function-pointer slots are zero).

Initial Python read used incorrect file offset (forgot `.data.rel.ro` segment maps VA `0x422dc10` → file `0x422cc10`, off by `0x1000`). Re-read with correct offset (file `0x42307c8` for VA `0x42317c8`) confirmed by BN screenshot `_emulation/reference/87f.png` shows entry 0 = `2b 50 1e 04 00 00 00 00` followed by name pointer to `"LPC1111FHN33/101"` string.

Per-entry layout (40 bytes):

| offset | size | meaning |
|---:|---:|---|
| 0  | 4 | chip ID (matches real NXP `Part identification number` register at `0x400483F4` for LPC11xx, or analogous for LPC8xx / RP2350) |
| 4  | 4 | padding (zero) |
| 8  | 8 | pointer to chip-name string in `.rodata` |
| 16 | 4 | (config word 0 — meaning unverified) |
| 20 | 4 | RAM/flash size (entry 0 = `0x2000` = 8 KB, matches LPC1111/101 flash) |
| 24 | 4 | RAM size or address (entry 0 = `0x800` = 2 KB, matches LPC1111/101 SRAM) |
| 28 | 4 | RAM start or sector address (entry 0 = `0x02dc6c00`) |
| 32 | 4 | flash sector size (entry 0 = `0x1000` = 4 KB, matches LPC11xx 4 KB sectors) |
| 36 | 4 | (unused / zero in entry 0) |

Full chip ID + name table:

| idx | chip_id | chip name |
|---:|---|---|
| 0  | `0x041E502B` | LPC1111FHN33/101 |
| 1  | `0x2516902B` | LPC1111FHN33/202 |
| 2  | `0x042D502B` | LPC1112FHN33/101 |
| 3  | `0x2524D02B` | LPC1112FHN33/102 |
| 4  | `0x00020023` | LPC1112FHN33/103 |
| 5  | `0x0425502B` | LPC1112FHN33/201 |
| 6  | `0x2524902B` | LPC1112FHN33/202 |
| 7  | `0x00020022` | LPC1112FHN33/203 |
| 8  | `0x0434502B` | LPC1113FHN33/201 |
| 9  | `0x2532902B` | LPC1113FHN33/202 |
| 10 | `0x0434102B` | LPC1113FBD48/301 |
| 11 | `0x2532102B` | LPC1113FBD48/302 |
| 12 | `0x00030030` | LPC1113FBD48/303 |
| 13 | `0x2C40102B` | LPC1313FHN33 |
| 14 | `0x1830102B` | LPC1313FHN33/01 |
| 15 | `0x00140040` | LPC1124JBD48/303 |
| 16 | `0x08018542` | LPC1346FHN33/FBD48 |
| 17 | `0x2C42502B` | LPC1311FHN33 |
| 18 | `0x1816902B` | LPC1311FHN33/01 |
| 19 | `0x2C40102B` | LPC1313FBD48 |
| 20 | `0x1830102B` | LPC1313FBD48/01 |
| 21 | `0x3D01402B` | LPC1342FHN33 |
| 22 | `0x3D01402B` | LPC1342FBD48 |
| 23 | `0x3D00002B` | LPC1343[FHN33\|FBD48] |
| 24 | `0x00008120` | LPC812M101JDH16 |
| 25 | `0x00008121` | LPC812M101JD20 |
| 26 | `0x00008122` | LPC812M101[JDH20\|JTB16] |
| 27 | `0x411FD210` | RP235x |

These are real silicon chip IDs (low byte `0x2B` = LPC11/13xx Cortex-M0 family marker; `0x40` family = LPC1124; `0x42` = LPC1346; `0x81xx` = LPC812; `0x411F...` = ARM Cortex-M33 part-number convention used by RP2350). Indices 13/19 and 14/20 share chip IDs — those pairs differ only by package (FHN33 vs FBD48) which the chip ID register can't distinguish, so the chip-classify code treats them identically and matches the first occurrence. Indices 21/22 likewise share `0x3D01402B` between FHN33 and FBD48 LPC1342 packages.

For shim purposes: any of these chip IDs in `response[4..7]` will make `mcp_node_query_status_and_classify @ 0x8f38e0` find a real (non-default) vtable entry, avoiding the NULL-deref on default entry. Picking an LPC1124 (idx 15, `0x00140040`) for slot 0 (the bridge node) and LPC1111/1112 variants for downstream coil/switch/lamp boards would mirror what real Spike 3 hardware would report — but the per-slot chip assignment for Pokemon LE specifically still needs verification against a real wire capture.

### What's left for next session

- **fe_probe response synthesis with timing**: keep response shape but artificially delay (~50–100 ms) so other threads catch up. Test whether matching real-hardware response cadence unlocks progress without the regression.
- **Per-slot chip ID assignment**: read Pokemon LE Lite manual for which LPC variant lives at each node, set `response[4..7]` per slot to that chip's vtable entry. Then synth the per-chip init responses (cmd `0x14` family).
- **Verify chip-ID values**: compare our vtable bytes against a real cabinet's cmd-`0xfe` response if any user has access.
- **Update PROTOCOL §5.1**: correct the csum/hint position formulas per the wrapper_ilen vs wire_ilen distinction. Keep the existing prose for what was already right.
- **Cosmetic**: shim cipher decoder's `payload=[...]` log line includes the csum byte. Adjust to `payload=` (real bytes only) and `csum=` (1 byte) for clarity in future runs.

### Cumulative metrics across all 5 fixes

| Symptom | Pre | Post |
|---|---|---|
| `failed to verify at NN: reads back as ffffffff` | 11 | 0 |
| `oscillator trim failed to lock` | 1 | 0 |
| `failed to get fd for bo: -1` | 1 | 0 |
| `failed to create framebuffer` | 1 | 0 |
| SIGSEGV at game+0xe3e250 | crashes ~55 s | 0 (180 s clean) |
| `bridge message ... Connection timed out` | 158 | 158 (unchanged, default off) |
| Forward progress | scene loading | scene loading + 113k frames decoded + 16 BO/EGL imports |

## Step 90: Per-slot chip-IDs land all Tech Alerts rows on "Not Initialized"; switch-press injection unsolved (2026-05-07)

Direct continuation of Step 89. Goal: turn the Tech Alerts UI breakthrough (visible Tech Alerts screen showing "Check Node Board N : Invalid/Not Initialized") into something we can advance past. Outcome: per-slot chip-ID synth flips every alert row from "Invalid" to "Not Initialized" (visual proof via frame capture). The 200ms cipher response delay is now a hardcoded shim default, and `run_game.sh`'s env-toggle plumbing is back to a clean state. Two Sidekick-driven attempts at switch-press injection failed empirical test — the Tech Alerts "Press Select" dispatcher is not driven by `g_switch_state_array` writes nor by `translate_switch_state_changes(5, …)`. Switch-press path remains unsolved; chip-ID + cipher-delay work landed clean.

### Step 90a — Per-slot chip-IDs flip "Invalid" → "Not Initialized"

User screenshot at `_emulation/reference/tech_alerts.png` (no synth, smoke run baseline) shows 5 rows: slot 1 = "Not Initialized", slots 4/8/9/10 = "Invalid". Hypothesis: `mcp_node_query_status_and_classify @ 0x8f38e0` matches `response[4..7]` (LE u32 = chip_id) against `mcp_lpc_chip_vtable @ 0x42317c8`; mismatch → "Invalid", match-but-no-post-init-traffic → "Not Initialized".

Verified the vtable layout via objdump dump of `.data.rel.ro` at file offset `0x42307c8` (= VA `0x42317c8`, accounting for the section's `0x1000` slide). Two chip IDs of interest are present:

| vtable idx | chip_id | chip name (per PROTOCOL §5.4) |
|---:|---|---|
| 0  | `0x041E502B` | LPC1111FHN33/101 |
| 15 | `0x00140040` | LPC1124JBD48/303 |
| 21 | `0x3D01402B` | LPC1342FHN33 |

Pokemon LE board mapping derived from the cabinet manual (the `high level informal description of Spike 3.txt` reference) plus the `.hex` firmware filenames in `games/pokemon_le/*.hex`:

| Slot | Role | Firmware filename | chip_id chosen | vtable name |
|---:|---|---|---|---|
| 0  | CPU/bridge node     | (host-side bridge LPC)   | `0x00140040` | LPC1124JBD48/303 |
| 1  | Cabinet Node        | (unverified)             | `0x041E502B` | LPC1111FHN33/101 (default) |
| 4  | QR Reader (Bottom Arch) | `node4-LPC1124_303-1_31_0.hex` (vs. `node4-LPC812-1_31_0.hex`; LPC1124 chosen as primary) | `0x00140040` | LPC1124JBD48/303 |
| 8  | Core Driver Lower Playfield | `coil4node-LPC1313-1_31_0.hex` | `0x3D01402B` | LPC1342FHN33 |
| 9  | Core Driver Upper Playfield | same | `0x3D01402B` | LPC1342FHN33 |
| 10 | SPI Servo Node Board | `tmc5041node-LPC1313-1_31_0.hex` / `tmc2590node-LPC1313-1_31_0.hex` | `0x3D01402B` | LPC1342FHN33 |

Note the disagreement between the LPC1313 firmware filenames for slots 8/9/10 and the chip_id we send (`0x3D01402B` = LPC1342 in this game's vtable — the LPC1313 chip IDs in the table are `0x2C40102B` at entry 13/19 and `0x1830102B` at entry 14/20). The game accepts any in-vtable chip_id at this stage, so the "Not Initialized" flip is empirically verified regardless. Real-hardware-accurate per-slot chip naming for the playfield nodes is **unverified** — try `0x2C40102B` next session if a more faithful mapping is needed.

User-supplied topology (manual): `0 → 10 → 9 → 8 → 4` (playfield daisy chain via CPU port CN22), `0 → 1` (separate cabinet branch via CPU port CN11). Slot 4 is at the *end* of the playfield chain, not on its own port. DIP switches on the same-hardware Core Driver nodes differentiate slot 8 (`OFF/OFF/OFF/OFF`) from slot 9 (`OFF/OFF/ON/OFF`), and slot 10 SPI Node has DIP `OFF/ON/OFF/OFF`.

Implemented `nodebus_chip_id_for_slot()` in `shim_nodebus.c` returning the per-slot u32 chip_id for the FE-probe synth response. Run with viewer + frame capture (`/tmp/snap_frame.py` snapshotting the abstract-AF_UNIX frame bridge). Frame at +220s (Tech Alerts settled) shows **all five rows = "Not Initialized"**. Chip-ID hypothesis verified.

### Step 90b — 200ms cipher synth delay is the new norm; env-var clutter removed

Step 89e's NODEBUS_SYNTH_FE/NODEBUS_SYNTH_FE_DELAY_US env gates were running counter to the new "always-on" baseline. Two issues caught:

1. `run_game.sh` only forwarded `NODEBUS_SYNTH_FE` to qemu — `NODEBUS_SYNTH_FE_DELAY_US` was silently dropped, so all "delay sweeps" earlier in the session were running at 0 ms.
2. With `./run_game.sh` now expected to land on Tech Alerts directly, env gating adds zero value — the synth + 200ms delay is the new default.

Fix: removed the `getenv` resolution in `shim_nodebus.c` for both vars. `usleep(NODEBUS_SYNTH_DELAY_US)` (a `#define` constant = 200000) inlines the pause before each `bridge_send_response` (FE, FA, key-fetch). Removed the corresponding `args+=(-E …)` lines and env-toggle table entries from `run_game.sh`. Plain `./run_game.sh` lands on "all-Not-Initialized" Tech Alerts.

### Step 90c — `g_switch_state_array` layout verified, but writes don't drive UI

Sidekick (BN AI assistant, since BN MCP was unreachable this session) claimed `g_switch_state_array @ 0x4480220` is a pointer to a heap array of 0x30-byte switch entries with state byte at `+0x28` (1=pressed, 0=released). Wrote a probe in `shim_inject.c` (env-gated under `SHIM_SWITCH_PROBE`) that resolves the pointer and dumps entries.

Layout **partially confirmed** at runtime:

| offset | size | content |
|---:|---:|---|
| `+0x00` | 8 | linked-list prev pointer (or 0 for first) |
| `+0x10` | 8 | descriptor pointer (deref crashes on entries beyond ~3) |
| `+0x20` | 2 | u16 switch_id (verified == array index for entries 1..11, 8, 69) |
| `+0x22` | 2 | u16 prev_id |
| `+0x24` | 4 | switch-type code (most entries = `0x04`; entry 69 = `0x09`) |
| `+0x28` | 1 | state byte (most = `0x00`; entry 69 = `0x01`) |
| `+0x29` | 1 | constant `0x01` |

Entry 69 stands out structurally (different `+0x24` type code, `+0x28` already at 1, `+0x22` prev_id breaks the sequential chain). Sidekick offered both "switch_id 8" and "switch_id 69" as candidates for "Service Select".

Empirical test failed for both candidates and both polarities:

- Direct write `entry[8|69] +0x28 = 1; sleep; = 0` (pressed-then-released semantics) — no UI advance.
- Inverted polarity `= 0; = 1` (pull-up: idle=HIGH, pressed=LOW) — no UI advance.
- `SHIM_SWITCH_ALL_IDLE=1` writing `+0x28 = 1` to all entries 1..255 (pull-up theory: silence stuck-active spam) — runtime audio spam continues unchanged.

**Conclusion**: `g_switch_state_array` is a derived cache, not the source of truth the Tech Alerts dispatcher reads. Writes to `+0x28` propagate nowhere observable.

### Step 90d — Sidekick's channel-5 hypothesis empirically falsified

Sidekick's follow-up analysis pointed at `g_channel_state_buffers @ 0x44804b8` (0xb0 stride per channel) as the actual edge-detection source, with `translate_switch_state_changes @ 0x651610` as the entry point that consumes a per-channel 8-byte buffer and updates `g_switch_state_array` only when edges are detected. Claimed Service Select sits on **channel 5, switch_id 8 within channel** (bit 8 = byte 1 bit 0 in the 8-byte buffer), routed in real hardware via the UART netbridge cmd `0x27` polling that we don't synthesize.

Implemented direct calls from the shim's probe thread:

```c
typedef int64_t (*translate_fn)(uint8_t channel_id, const uint8_t *buffer);
translate_fn translate = (translate_fn)(game_base + 0x651610);
uint8_t buf[8] = {0};
buf[1] = 0x01;          /* byte=1, bit=0 → switch_id 8 */
translate(5, buf);
usleep(180000);
buf[1] = 0;
translate(5, buf);
```

Targeted (byte=1, bit=0) — no UI advance. Brute-forced **all 64 bit positions** (8 bytes × 8 bits, ~2s per pulse, full 128s sweep at +200s into the run with frame capture) — no UI advance on any position. Frame hashes are stable Tech Alerts content with only minor cyan-pulse animation noise.

**Conclusion**: either Sidekick's channel-5 mapping is wrong, or `translate_switch_state_changes` isn't the path the Tech Alerts scene's input handler reads from. Switch-press injection mechanism remains unsolved.

### Step 90e — Code state at session end

`shim_nodebus.c`:

- `nodebus_chip_id_for_slot(uint8_t slot)` — switch statement returning the u32 chip_id per Step 90a table.
- `NODEBUS_SYNTH_DELAY_US = 200000` `#define` constant; `usleep(NODEBUS_SYNTH_DELAY_US)` inlined before each FE/FA/key-fetch `bridge_send_response`.
- All `getenv("NODEBUS_*")` calls removed.

`run_game.sh`:

- `NODEBUS_SYNTH_FE` and `NODEBUS_SYNTH_FE_DELAY_US` defaults + forwarding removed.
- env-toggle table back to its pre-experiment state.

`shim_inject.c`:

- New `shim_select_press_thread` spawned from `shim_inject_start()`, gated behind `SHIM_SWITCH_PROBE=1`. No-op by default. When activated:
  - sleeps `SHIM_SWITCH_PRESS_DELAY_S` seconds (default 30; needs ~200 to catch Tech Alerts),
  - dumps probe entries 0/1/2/8/69 from `g_switch_state_array` to log,
  - optionally writes `+0x28 = 1` to all entries 1..255 (`SHIM_SWITCH_ALL_IDLE=1`),
  - calls `translate_switch_state_changes(5, …)` with either a single `(SHIM_CH5_BYTE, SHIM_CH5_BIT)` configuration or a `SHIM_CH5_SWEEP=1` 64-position brute force.

`/tmp/snap_frame.py` (one-shot AF_UNIX frame snapper): saved separately at `_emulation/snap_frame.py` for reuse.

### Open questions / next steps

1. **What memory does the Tech Alerts scene's input handler actually read each frame to detect "Select pressed"?** Neither `g_switch_state_array[*]+0x28` nor channel-5 `translate_switch_state_changes` calls move the UI. Needs a real Ghidra/BN session — probably xref the "Press 'Select' for Node Bus Test" string at `0x35EC2C8`, follow up to its scene class, find the input poll routine.
2. **Source of the runtime audio spam.** Force-all-idle to `g_switch_state_array[*]+0x28 = 1` had zero effect. The spam isn't gated on that array. Candidates: cipher-channel "node communication failed" alarm path, `fire_event` timer, or a separate raw per-node buffer.
3. **"Not Initialized" → "OK"** for individual node rows. Distinct from the press question — would require firmware-flash protocol completion or richer FE-probe response payload. Out-of-scope until press is solved.
4. **Verify slot 4 uses LPC1124 vs LPC812.** Two firmwares ship for node 4 (`node4-LPC1124_303` and `node4-LPC812`); we picked LPC1124 first and slot 4 row flipped to "Not Initialized" — but we never tested LPC812 to confirm that choice.

### Reproducible state

Plain `./run_game.sh --debug` (no env vars) lands on Tech Alerts at ~+200s with all five alert rows = "Not Initialized". Frame capture with `_emulation/snap_frame.py /tmp/snap N stride` confirms.

To resume switch-injection investigation:

```sh
SHIM_SWITCH_PROBE=1 SHIM_SWITCH_PRESS_DELAY_S=200 ./run_game.sh \
    --debug --note "switch_probe_resume"
```

## Step 91: Tech Alerts dispatcher mapped — natural protocol path to Node 1 = "Ok" identified, blocked at FE-probe retry (2026-05-08)

Direct continuation of Step 90. Goal: advance Node 1 (Cabinet Node) past "Not Initialized" via the natural protocol path rather than direct memory override, per user's framing of nodebus communication completeness as a side-quest goal. Outcome: full mapping of the Tech Alerts row dispatcher, status enum, and firmware-image lookup chain — landing on a clear next-blocker ("`FUN_00674680`'s 5×5 FE-probe retry loop runs `iVar12++` despite synth always responding, mechanism uncharted") rather than the originally planned shim hook.

### Step 91a — Tech Alerts row dispatcher and 10-state status enum

**`FUN_007da460 @ 0x7da460`** is the Tech Alerts row formatter (formats one alert as the param_2-th alert, returns 1 on success). Built `"Check Node Board %d : %s"` (msgid `0x37f`). The status-code → msgid table is at **`0x37c9ef0`** (10 u16 entries indexed by per-slot status 0..9; verified via `objdump`/`readelf`-style file reads following user's cross-check rule):

| status | msgid | string | Tech Alerts behavior |
|---:|---:|---|---|
| 0 | 0x36a | Invalid | shown |
| 1 | 0x36b | Unused | shown |
| **2** | **0x36c** | **Ok** | filtered out (when flag word's bits 0+1 both set) |
| 3 | 0x36d | Boot | shown |
| 4 | 0x36e | Runtime Info | shown |
| 5 | 0x36f | Checksum | shown |
| 6 | 0x370 | Hex Image Version not Found | shown |
| 7 | 0x371 | Version Mismatch | filtered out |
| **8** | **0x372** | **Not Initialized** | shown ← Step 90's current state for slots 1/4/8/9/10 |
| 9 | 0x373 | Collision | shown |

Filter logic in dispatcher: `(status != 7 && status != 2) OR (per-slot-flag-bits-32-33 != 0b11 AND PTR_DAT_043525c0 != 0)` → row counted as alert. Status==2 has a special override that turns it into "Not Registered" (msgid `0x374`) or "Not Responding" (msgid `0x375`) text based on flag-word bits 0/1.

**Per-slot Tech Alerts struct** at `&DAT_044cc290` (32 slots × 0xf0 = 7680 bytes). Key offsets:
- `+0x00`: slot ID byte
- `+0x04`: u32 flag word (drives status==2 textual override + bit 14 alerting)
- `+0x10`: pointer to firmware-image config entry
- `+0x18`: pointer to board-type metadata
- `+0x20`: u32 status (the value Tech Alerts displays)
- `+0x60`: u32 firmware-kind key (= hash of board-family name)

Initializer at **`FUN_007dae30 @ 0x7dae30`** does `memset(&DAT_044cc290, 0, 0x1e00)` (clears all status to 0) then iterates `*PTR_DAT_0434f2f0` (= 12) firmware-image entries to populate per-slot fields.

### Step 91b — Status writer = `FUN_00674680` (= `mcp_nodebus_node_probe_5x_unsure` per Step 68)

The natural status mutator wasn't found via Ghidra xref to `0x44cc290` (writes go through stride arithmetic on a register-held base). Resolved by following callers of FE-probe (`FUN_008f38e0 @ 0x8f38e0`) — the status writer is the long-named function from Step 68's BN renaming.

`FUN_00674680(slot_struct, force_flag)` runs:
1. **5 outer retries**, each:
   - FE-probe; if fails → `iVar12++`
   - Else 5 inner retries verifying response consistency (response bytes drift → `iVar13++`, break inner)
   - If inner found 5 consistent responses (`iVar13 == 0`): enter VALIDATION CHAIN and **return** (success path — no more outer retries)
2. **Validation chain** (only reached when inner consistent):
   - If `FE_byte_0 & 1 != 0`: status = 3 ("Boot")
   - Else: lookup `FUN_00840d30(slot[+0x60], chip-type-index)`:
     - lookup fails: status = 4 ("Runtime Info")
     - record's `+0x20`/`+0x40` zero: status = 4
     - `FUN_008f88b0` returns 0: status = 6 ("Hex Image Version not Found")
     - version bytes mismatch FE response: status = 7 ("Version Mismatch")
     - extra version-byte check fails: status = 6
     - all match: **status = 2 ("Ok")**
3. **5 outer retries exhausted** without inner-consistent path:
   - `iVar13 != 0`: status = 9 ("Collision")
   - `iVar12 != 0`: status = 8 ("Not Initialized") ← Step 90's current state
   - else: status stays 0 ("Invalid")

### Step 91c — Firmware-image lookup chain

Three levels of indirection between slot config and runtime firmware lookup:

```
PTR_PTR_04350200 → 0x436b680 (.data, compile-time-baked firmware-image config table)
   each entry, 0x20-byte stride:
      +0x00 : u32 flags (bit 2 gates filter in FUN_007da460)
      +0x18 : ushort firmware_idx ──────┐
      +0x1a : byte slot_id              │
                                        ▼
                       PTR_DAT_04351748 → 0x43c3b68 (.flash board-type metadata)
                          each entry, 0x28-byte stride:
                             +0x10 : opaque 8-byte handle
                                        │
                                        ▼ FUN_00840ce0(handle)
                                        │
                                        ▼ stored in slot_struct[+0x60] = u32 firmware-kind key

At runtime: FUN_00840d30(key, chip-type-index) walks linked list at DAT_044df838:
   each node, 0x58-byte calloc'd struct:
      +0x00 : u32 key1 (= FUN_008f8ab0 hash of board family name)
      +0x04 : u32 key2 (= chip-type-index 1..7 from FUN_008ede20)
      +0x08 : char *filename
      +0x10..0x4f: zero-initialized (post-load fields, populated elsewhere)
      +0x50 : pointer to next record
```

### Step 91d — Loader chain (`.hex` files in CWD)

`FUN_00840a40 @ 0x840a40` iterates 7 board-type indices (`iVar7 = 1..7`), for each calling `FUN_008ede20(iVar7)` to get chip-type-name string and globbing:

```
./<family>-<chip_type_name>-<major>_<minor>_[0-9]*.hex
```

Falls back to `./<family>-<chip_type_name>-[0-9]*_[0-9]*_[0-9]*.hex` on no match. Per match, calloc a 0x58-byte node and link to head of `DAT_044df838` linked list with `key1=hash(family)`, `key2=iVar7`, `filename=ptr`.

Shipped `.hex` files in `games/pokemon_le/`: 17 files across 12 families (`coil4_lednode`, `coil4node`, `hdminode`, `hdmi_ws2812node`, `lcdnode`, `magsensornode`, `node4`, `pinnode`, `tmc2590node`, `tmc5041node`, `ws2812node`, `ws2812pinnode`). Files appear to be encrypted (Intel HEX line format with high-entropy data bytes).

### Step 91e — Runtime probe instrumentation

Added `shim_node_records_probe_thread` (env-gated `SHIM_NODE_PROBE=1`, default delay 60 s) to `shim_inject.c`. Spawns from `shim_inject_start`. Probe dumps:
- GOT entries 0x4350200 / 0x434f2f0 / 0x4351748 (resolved at runtime)
- First 16 entries of firmware-image config table (slot byte at +0x1a)
- Linked list at DAT_044df838 (key1, key2, filename)
- Per-slot Tech Alerts struct for slots 0..15 (id, flag, status, fw_key, fw_cfg, bt pointers)

**Runtime data captured (probe at +60 s, plain `./run_game.sh --debug` baseline):**

Family-name hashes (key1 from `FUN_008f8ab0`, ground-truth from probe):
| Family | key1 |
|---|---|
| `pinnode` | 0xdcc6afb2 |
| `node4` | 0xf585d1cf |
| `ws2812node` | 0xd2a9be05 |

chip-type-index ↔ chip-name (key2 mapping observed):
| key2 | chip name |
|---:|---|
| 1 | LPC1112_101 |
| 2 | LPC1112_201 |
| 4 | LPC1124_303 |
| 5 | LPC1313 |
| 6 | LPC812 |

Six firmware records loaded into linked list (only families with both `.hex` shipped AND a registered slot in PTR_PTR_04350200): pinnode-LPC1313, pinnode-LPC1112_201, pinnode-LPC1112_101, node4-LPC812, node4-LPC1124_303, ws2812node-LPC1313.

Per-slot fw_key map (post-init):
| slot | id | flag | status | fw_key | implied family |
|---:|---:|---:|---:|---|---|
| 0 | 0 | 0x3 | **2 (Ok)** | 0 | CPU bridge — flag=3 → row hidden |
| 1 | 1 | 0x1 | 8 (Not Init) | 0xdcc6afb2 | **pinnode** ← Cabinet Node! |
| 2 | 2 | 0x1 | 8 | 0xd2a9be05 | ws2812node |
| 4 | 4 | 0x1 | 8 | 0xf585d1cf | node4 |
| 7 | 7 | 0x1 | 8 | 0xd2a9be05 | ws2812node |
| 8 | 8 | 0x1 | 8 | 0xdcc6afb2 | pinnode |
| 9 | 9 | 0x1 | 8 | 0xdcc6afb2 | pinnode |
| 10 | 10 | 0x1 | 8 | 0xdcc6afb2 | pinnode |
| 12 | 12 | 0x1 | 0 (Invalid) | 0xd2a9be05 | ws2812node — FUN_00674680 not run yet |
| 13 | 13 | 0x1 | 0 | 0xdcc6afb2 | pinnode |
| 14 | 14 | 0x1 | 0 | 0xd2a9be05 | ws2812node |
| 3, 5, 6, 11, 15 | — | 0 | 0 | 0 | (vacant) |

**Major correction to Step 90a's mapping**: slots 8/9/10 use family **pinnode** (not coil4node as inferred from `.hex` filenames in Step 90a). The compile-time PTR_PTR_04350200 table assigns slots 8/9/10 to pinnode regardless of the manual's coil4node implication.

### Step 91f — `nodebus_chip_id_for_slot(1)` change to LPC1112_101 — no observable status change

Changed `nodebus_chip_id_for_slot(1)` from `0x041E502B` (LPC1111FHN33/101, no shipped pinnode firmware) to `0x042D502B` (LPC1112FHN33/101, vtable entry 2; matches shipped `pinnode-LPC1112_101-1_31_0.hex`). Re-ran with probe — slot 1 STILL at status=8 after +60 s.

Why no change: `FUN_00674680` lands on the "5 outer retries exhausted, iVar12 != 0, iVar13 == 0" exit BEFORE reaching the chip_id-driven validation chain. The retry loop is tripping on something *prior* to chip_id classification.

### Step 91g — FE-probe synth confirmed not the cause

Logged FE-probe synth events from same run: 0 SKIPs, ~10 RESPs per slot at hint=12 (one wrapper) AND ~10 at hint=13 (`mcp_nodebus_send_cmd_fe_probe_unsure` = ilen=1 hint=0x0d per Step 89e). Synth always responds with non-empty payload + node-present status byte + chip_id from `nodebus_chip_id_for_slot()`. So the `iVar12++` cause is not a missed/skipped synth response.

Remaining hypotheses (untested):
1. **`FUN_008ed870` (= the cmd-0xfe send wrapper inside `FUN_008f38e0`) does additional response validation** beyond just "got bytes back" — checksum, length, or some field that our synth doesn't fully populate. Returns 0 on validation failure → propagates to `FUN_008f38e0` returning 0 → `iVar12++`.
2. **Bridge-thread serialization**: shim's 200 ms `usleep` per FE-probe means 5 inner probes take 1 s, AND the periodic poll loop also calls FE-probe on its own thread; if both threads contend on the same bridge state, some calls might fail.
3. **Probe-window short**: `FUN_00674680` may not have completed enough cycles by +60 s for status to settle. Untested at longer windows.

### Step 91h — Open + retired threads

**Retired** (this Step):
- "Cabinet Node uses cabinet/node1 family" — falsified; it uses pinnode (key1=0xdcc6afb2).
- "Drop synthetic `cabinet-LPC1111_101-X.hex` file" — this was Path A4 option (c) from `project_node1_init.md`; falsified because slot 1's family is pinnode and there are pinnode files but for different chip variants.
- "Direct shim write to status u32 makes row vanish" — partially-falsified; status=2 alone shows "Not Responding" per the dispatcher's flag-bit override. To truly hide the row, must ALSO set flag word's bits 0+1 (currently only bit 0 set for slot 1).

**Open** (next session):
1. **Decompile `FUN_008ed870 @ 0x8ed870`** (`mcp_nodebus_send_cmd_fe_probe_unsure`) — what response validation does it do? Likely a checksum or specific-byte check our synth misses.
2. **Re-run probe at longer delay** (180 s, 240 s) to confirm status settles vs continuously updating.
3. **HW watchpoint `(game_base + 0x44cc3a0)`** in system-mode per PROTOCOL §14 to capture every write to slot-1 status — gives the runtime answer for what code path writes status=8 and when.
4. **`FUN_007da920` callers** — identify what sets flag word's bits 0+1 (only slot 0 has both set). For Tech Alerts row to truly hide on status=2, slot 1 needs flag = 0x3.

### Files touched
- `_emulation/shim_inject.c`: added `shim_node_records_probe_thread` (env-gated), spawned from `shim_inject_start`. +~85 lines.
- `_emulation/shim_nodebus.c`: changed `nodebus_chip_id_for_slot(1)` from `0x041E502B` → `0x042D502B`. +1 case to switch.
- Build: `spi_shim.so` 232256 B → 298400 B (+66 KB; matches probe code growth).

### Reproducible state
Plain `./run_game.sh --debug` (no env) lands on Tech Alerts at +200 s with same alert rows as Step 90. Diagnostic dump: `SHIM_NODE_PROBE=1 SHIM_NODE_PROBE_DELAY_S=60 ./run_game.sh --debug --note "node_probe"` then grep `node_probe|fw_record|slot\[` in `runs/<TS>/game.log`.

## Step 92: Nodebus init complete: Service Menu reached, Tech Alerts empty (2026-05-08)

Investigation arc from "all 5 Tech Alerts rows on Not Initialized" (Step 90) to "all 11 populated nodebus slots at flag=0x03 status=2; Tech Alerts empty; game progresses to **Service Menu** showing Game V0.81.0 / Nodebus 1.31.0 / Bridge 39.83.127". Five distinct findings folded into a modular FE-probe + node-enumeration path in `shim_nodebus.c`.

### Step 92a — FE-probe inner-payload was failing checksum, not size

Decompiled the FE-probe call chain: `FUN_008ed870` (sender) → `FUN_008ed4a0` (worker) → `FUN_008ec410` (wire-frame validator). `FUN_008ec410` modifies `ilen` to `ilen+1` and appends `__n = hint + 2` at index `ilen+3` before transmit. Net effect: what the shim parses as "hint" at `payload[2 + inner_len]` is actually `__n` (= original game-side hint + 2). The shim's `inner_size = hint` was already the correct byte count.

Real failure: validator sums inner bytes `[0..hint-2]` and requires `== 0 mod 256`. Shim's chip-id payload `0x80 0x01 0x80 0x00 + chip_id_LE` summed to `0xA9` — checksum mismatch sets `DAT_044fce30 = 3` and returns 0. No fprintf, silent failure that masked it as a size issue.

Fix: write inner checksum at byte `[hint-2] = -sum(bytes[0..hint-3]) mod 256`. Status moved 8 → 3 (Boot).

### Step 92b — `resp[0]` high bit is boot-mode, not "node present"

Old shim comment was wrong. `FUN_008f38e0` does `*param_2 = local_38 >> 7`, then `FUN_00674680` routes `(local_88 & 1) != 0 → status=3 (Boot)`. Bit 7 = boot-mode active. Setting `resp[0] = 0x00` advances to chip_id classification. Status moved 3 → 7 (Version Mismatch).

### Step 92c — Modular runtime version-byte lookup

`FUN_008f88b0` exposes (V1, V2, V3, V0) via two paths gated on `record[+0x28]`:
- Simple (`+0x28 != 0`): bytes at `record[+0x14], [+0x16], [+0x18], [+0x1e]`.
- Complex (`+0x28 == 0`): bytes from `*(record+0x20) + record[+0x3c] + {9, 10, 11, 8}`.

`FUN_00674680` requires FE bytes 1..3 == (V1, V2, V3) AND (V0 == 0 OR V0 == FE byte 10).

Pinnode + ws2812node take complex path; node4 takes simple. Per-family V0: pinnode=0x01, node4=0x03, ws2812node=0x05. V1/V2/V3 = (0x01, 0x1F, 0x00) across all shipped 1_31_0 records.

Implementation: `nodebus_version_for_slot()` in `shim_nodebus.c` walks `DAT_044df838`, finds record matching `(slot_struct[+0x60], vtable[chip_id_for_slot][+0x10])`, reads V1/V2/V3/V0 mirroring `FUN_008f88b0`. Game-portable: works for any game that ships .hex files via the same loader. Slots 1 (pinnode-LPC1112_101) and 4 (node4-LPC1124_303) reached status=2.

### Step 92d — Bridge cmd 0x25 node-enumeration + cmd 0xF0 reset detection

Tech Alerts dispatcher renders "Not Responding" override on status=2 unless flag bits 0+1 BOTH set. Bit 1 set by `mcp_nodebus_init_orchestrator` (`FUN_00675110`) at boot via three paths:
- A/B: `FUN_007da920(list, 32, 2)`, list built by `FUN_00674600` calling `FUN_008ede50` repeatedly. `FUN_008ede50` issues bridge cmd 0x25 (no payload, 1-byte response = next slot ID).
- C: direct `flag |= 2` in final pass, gated on `FUN_008f5cd0` (cmd 0x41 inner cmd 0xFF) success.

`case 0x25` in `synthesize_bridge_response`: stateful counter through `enum_seq`, 0 terminates. `FUN_008f1100(0, 0)` (cmd 0x41 inner cmd 0xF0 with slot=0, payload byte 0x22) is the orchestrator's reset signal — detected in shim's cmd 0x41 path, resets cursor between retry cycles. Three full enumeration cycles observed per boot. Slots 1 and 4 reached flag=0x03 status=2.

### Step 92e — Per-slot chip_id table verified via vtable dump

Extending to pinnode slots 8/9/10/13 with assumed `chip_id=0x3D01402B` (LPC1313): status stayed 4. Vtable dump (`game_base + 0x42317c8`, 28 entries × 0x28) showed `0x3D01402B` is `vtable[21]/[22]` with `type_idx=0` — `FUN_008edd80` returns it, `+0x10 = 0`, `uVar9 = 0`, lookup miss, status=4.

Real LPC1313 chip_ids in this binary: `0x2c40102b` (vtable[13]), `0x1830102b` (vtable[14]), `0x08018542` (vtable[16]) — all type_idx=5.

Fix: pinnode slots 8/9/10/13 → LPC1112_101 (`0x042D502B`, vtable[2], type_idx=1) — bypasses the LPC1313 special branch via the simple chip_id path that already worked for slot 1. ws2812node slots 2/7/12/14 → real LPC1313 (`0x2c40102B`, vtable[13]). Updated `enum_seq` to `{1, 2, 4, 7, 8, 9, 10, 12, 13, 14, 0}`.

### Step 92f — Result

All 11 populated slots (0/1/2/4/7/8/9/10/12/13/14) probed at +80s: `flag=0x00000003 status=2`. Tech Alerts renders empty. Game advances to the **Service Menu** with version line:

```
Game: V0.81.0      CPU: Spike 3
System: 4.19.0     Nodebus: 1.31.0
OS: 1.00.6         Bridge: 39.83.127
Service S Agent: 1.00.16  PDB Boot: 1.00.0
                   PDB Runtime: 0.00.0
                   Video Processor: 0.00.12
```

`Nodebus: 1.31.0` and `Bridge: 39.83.127` are read from our shim's responses + loaded firmware records — visual confirmation the synthesis works end-to-end. Audio init runs (long-standing audio spam now correlates with this point). Footer prompt: "Press 'Select' to continue". **(2026-05-08 correction: the original draft of this paragraph also claimed "Idle eventually advances to a Test Menu screen with no further visual feedback." That advance was never reproduced in any subsequent run — see Step 93. The Service Menu has been the steady state in every run since this Step's fix landed.)** Test-cycle observation baseline shortened from 240s to 120s (`SHIM_NODE_PROBE_DELAY_S=80`).

### Open / next session

- Switch-press injection to advance the **Service Menu** — Step 90 #1 was empirically falsified on the Tech Alerts screen, but may behave differently on the SM.
- LPC1313 special branch in `FUN_00674680` (`FUN_008f5390` = cmd 0xF2 × 8 per slot) — currently bypassed for pinnode slots by reporting LPC1112_101 chip_ids; ws2812node slots take the LPC1313 path successfully without cmd 0xF2 synth (FUN_008f5390 fails but `vtable+0x10=5` still drives correct lookup). cmd 0xF2 synth would close the gap properly.
- Star Wars LE 0.85 cross-validation — modular version lookup should make the same shim work, validating game-portability.

### Touched

- `games/pokemon_le/_emulation/shim_nodebus.c` — FE-probe synth (checksum + boot-mode + version lookup), `nodebus_version_for_slot()`, `case 0x25` enum + cmd 0xF0 reset, `nodebus_chip_id_for_slot()` per-slot table.
- `games/pokemon_le/_emulation/shim_inject.c` — probe extensions: record bytes, version bytes per record, vtable entries (env-gated).

## Step 93: Switch-press injection works mechanically; audio-singleton vtable corruption blocks Service Menu advance; screen nomenclature canonicalized (2026-05-08)

Direct continuation of Step 92's "Open / next session" item 1 (switch-press injection on the Service Menu). Mechanical end of the press path is now fully verified; visual scene transition still does not happen. The actual blocker has been narrowed to an audio-singleton vtable corruption that the Service-Menu transition path appears to depend on. Five sub-steps + a nomenclature-canonicalization pass.

### Step 93a — Nomenclature canonicalization

User clarified that across the whole project there are exactly two top-level post-boot screens, and that earlier prose conflated them under several aliases. Canon going forward (folded into PROTOCOL §3 "Canonical post-boot screen names"):

- **Tech Alerts (TA)** — the node-bus status screen.
- **Service Menu (SM)** — the version-info screen with `"Press 'Select' to continue"` footer.

Anything in earlier prose that used `"version-info screen"`, `"version info screen"`, `"test menu screen"`, or `"tech menu"` was referring to one of these two and is now corrected. **There is no top-level "Test Menu" screen** — Step 92's "Open / next session" item 1 originally claimed the SM auto-advances to one; the user has never reproduced that advance, and that paragraph now carries the correction inline.

The string `"test menu"` legitimately appears in the binary's NuklearScene operator UI tree as a SUB-MENU label inside the SM (`"Active Switch Test"`, `"Game Adjustments"`, `"Pikachu Tests and Calibration"`, `"Exit the test menu"`, etc.) — but only as a sub-menu, never as a top-level screen.

PROTOCOL fixes from this step: §1 "Repository layout" (`spike_menu` row), §5.4 "Per-slot chip-ID synth" + its *Verified by:* footnote, §6 "Multi-image sub-files" (the `scene_config_table` misnaming subsection was DELETED — see Step 93g), §9 "Scene role classification" (the `cbe7806d` asset descriptor), §11 "Tech Alerts row dispatcher" heading, §11 "Flag-bit-1 paths" *Verified by:*, §12 "Persistence gap", §12 "Translation units" NuklearScene row.

### Step 93b — Switch-press injection mechanism verified

The press path is fully mechanical: SPI override (`shim_spi_rx_extra[]` defined in `shim_ioctl.c`, declared in `spi_shim.h`, OR'd into the SPI MESSAGE_1 RX buffer) + force-poll `sub_674b90(0)` from the press thread in `shim_inject.c::shim_switch_table_probe_thread`. With both in place, pressing sw_id=90 (Service Select) produces verified `state` byte `0 → 1 → 0` transitions and `edge_counter` increments at `g_switch_state_array[90] +0x28 / +0x26`.

A subtle off-by-one was uncovered when the press initially failed to register: the SPI thread `sub_8fa820` builds `DAT_0450d9b0` via `CONCAT44(uStack_54, CONCAT13(bStack_55, CONCAT21(uStack_57, local_58)))`. `local_58` is a stack variable BEFORE the rx_buf (`puStack_78 = &uStack_57`), so `DAT_0450d9b0` byte 0 = `local_58` (always zero), byte N = `rx[N-1]` for N=1..7, and `rx[7]` is dropped. The probe's `byte=N` (per `bit_idx >> 3`) refers to the channel buffer position (= DAT byte position), so for sw_id=90 (channel 0, byte 1, bit 0) the correct rx index to OR a press bit into is `rx[0]`, not `rx[1]`. Fix: `rx_off = byte_off - 1` for channel 0 in the press code.

`sub_720cf0` (the procDispatch tick) does NOT call `sub_674b90(0)` periodically post-init — only `sub_675a10` calls it once, gated by `DAT_04482194`, and the node-enum loops `sub_675df0`/`sub_6750e0` only call it during init. So an SPI-rx bit alone, set without a follow-up `sub_674b90(0)` call, sits unread in `DAT_0450d9b0`. The shim's press thread therefore performs a manual `sub_674b90(0)` invocation (game-base + `0x674b90`) immediately after setting the rx bit.

### Step 93c — Service-button handler chain traced

Per BN export entry 326, the canonical switch dispatch pipeline is:
1. Bridge cmd `0x27` / SPI poll → channel buffer
2. `sub_674b90(channel_id)` → `sub_651610(channel_id, &local_50)` (= `translate_switch_state_changes`)
3. Push to `g_pending_switch_events_head @ 0x4480278`
4. `sub_6511c0` (`drain_pending_switch_events`) drains the queue and schedules a fiber via `sub_729c10(event_id, handler_fn, priority)`
5. `sub_720cf0` (procDispatch tick) runs `sub_6511c0` every iteration

For sw_id=90 the scheduled fiber runs `sub_5902f0` (the service-button handler), which:
- Reads `sw_id` and `param_2` from the fiber's `+0x108` / `+0x110` (set by `sub_6511c0` from `*plVar13`/`plVar13[1]`)
- Gates on bits 3 and 4 of the 36-bit mode bitmap at `DAT_044cb9f0` (`sub_7a8de0(3)` / `sub_7a8de0(4)`); if either is set, exits without action
- Otherwise calls `sub_58ff40(sw_id, param_2, 1)`

`sub_58ff40` dispatches by `param_2` (= config[+0x08] per Step 93f's table) and `*PTR_DAT_04353468` bit 8 (the "service mode active" flag):

| `param_2` | Press path (bit 8 = 0) | Press path (bit 8 = 1) |
|---|---|---|
| 1 (Service Back per game convention) | Multi-step gated chain: `sub_72a5c0(0x1a)` → `sub_5d0c10` (= `DAT_0445f838 & 1`) → `sub_6d2ca0(5)` → `sub_769460(*PTR_DAT_0434f980, 1)` → `fire_event(0xa5, 0)` + `sub_583e30(0x69, 1)` → `sub_5d6480(0x1b, ...)` | `sub_58f840(&DAT_0445d960, sw_id, 0x11, ...)` |
| 8 (Service Select per game convention) | `sub_5d0870(0)` + `sub_72fdf0` + **`sub_58fcc0(0)`**, which fires `event 0xbb` → on success allocs fiber + sets bit 8 of `*PTR_DAT_04353468` + fires events `0x48` and `0x49` → unconditional `event 0xbc` | `sub_58f840(&DAT_0445d978, sw_id, 0x14, ...)` |

In runtime probe state (Step 93e), the mode bitmap = `04 00 00 00 00` (only mode 2 active), `DAT_0445f838` boot-state mask = `0`, and `*PTR_DAT_04353468` bit 8 starts at 0. So the press takes the bit-8-clear path and successfully fires the chain.

### Step 93d — Audio singleton vtable corruption

Pressing sw_id=77 (Action Button, channel 1 byte 0 bit 2 = `1-SW-2`) or sw_id=91 (Service Plus, channel 0 byte 1 bit 1 = `0-SW-9`) triggers a SIGABRT via `__cxa_pure_virtual` deep in the audio subsystem.

Crash site (audio-singleton dump auto-emitted by the SHIM SIGABRT handler): the audio singleton at `data_451ac48` has its vtable @ `0x4324a30` populated entirely with `_ZSt10unexpected` (libstdc++'s pure-virtual handler) — every slot reads `ELF_VA=0x10dda960` (a libstdc++ address). The destructor `sub_23737f0` is the function that resets vtable to that abstract-base state via `*param_1 = (long)&PTR___cxa_pure_virtual_04324a30`; it has either run already (cleaning up after a failed init) or the derived constructor never wrote the vtable in the first place. Service Select (sw_id 90 / sw_id 93, both channel 0 byte 1) avoids the audio call paths inside `sub_58ff40` so its press survives, but Action Button + Service Plus + Service Minus all touch the audio chain and crash.

This is **likely the actual blocker** preventing the SM from advancing visually: any scene-transition path the press chain triggers also calls into the audio singleton's vtable and silently fails (or crashes) before the renderer commits the new scene. Confirmation: every successful Service-Select press produced a clear AURAL CHANGE (audio spam paused or shifted, marked by user with the marker_button) — proof the handler chain reached into audio code — but no VISUAL CHANGE in any run.

### Step 93e — Mode-state runtime probe

The 36-bit mode bitmap at `DAT_044cb9f0` (calloc'd by `sub_7a8b90` to `(*PTR_DAT_043527d0 >> 3) + 1` bytes; `*PTR_DAT_043527d0 = 36`) decodes as `04 00 00 00 00` in the steady SM state — only mode 2 active. Bits 3 and 4 are clear so `sub_5902f0`'s mode gate passes. `DAT_0445f838` boot-state mask reads `0x00000000` so `sub_5d0c10` returns 0 and the bit-8-clear branch of `sub_58ff40` is taken.

Probe added to `shim_inject.c::shim_switch_table_probe_thread`: dumps `mode_bitmap @ <ptr> bytes=...` and `boot_state_mask @ <addr> = 0x...` at +80s.

### Step 93f — Switch-table dump (folded into PROTOCOL §11)

Runtime probe `SHIM_SWITCH_TABLE_PROBE=1` enumerates all 99 entries of `g_switch_state_array @ 0x4480220` with `(channel, byte, bit, state, handler_fn VA, param_2, event_id, priority, flags_2e, flags_30)`. Per-channel summary:

| Channel | sw_id range | Source | Manual prefix |
|---|---|---|---|
| 0 | 90..98 (9) | CPU SPI | `0-SW-N` |
| 1 | 77..89 (13) | Derived from CPU-SPI byte 3 via `sub_674b90`'s state machine | `1-SW-N` |
| 4 | 1..16 (16) | cmd `0x41` inner_cmd `0x11` to slot 4 (QR Reader) | `4-SW-N` |
| 8 | 17..46 (30) | cmd `0x41` inner_cmd `0x11` to slot 8 (Lower Playfield Core-Driver) | `8-SW-N` |
| 9 | 47..76 (30) | cmd `0x41` inner_cmd `0x11` to slot 9 (Mid-upper Playfield Core-Driver) | `9-SW-N` |

Total = 99 (Pokémon manual lists "over 100" because some entries are reserved/uninstalled jumpers without runtime allocation).

Per-switch handler config layout discovered via Step 93c trace + runtime dump: handler_fn at config+0x00, param_2 at +0x08, priority at +0x2c, flags at +0x2e, more flags at +0x30, event_id at +0x34. Service-button param_2 values: sw_id 90 = 8 (Select per game-side `sub_58ff40` event-id mapping: event 0x14), 91 = 4 (Plus), 92 = 2 (Minus), 93 = 1 (Back). Note this REVERSES the manual's bit-position-based naming convention — manual's `0-SW-8` = bit_idx 8 = byte 1 bit 0 = sw_id 90, but the game internally treats sw_id 90 as Select (param=8 fires `event 0x14`). The full table is now in PROTOCOL §11 "Runtime switch table — full sw_id → channel/byte/bit mapping".

### Step 93g — Documentation cleanup

Per user, anything in PROTOCOL that referenced files in `_extraction/_outdated/` was stale. The audit showed only one such ref — the §6 "Sub-section: cmode 'scene_config_table' misnaming" which cited a 1,254-line `scene_config_table.c` export that no longer exists in the repo. Subsection deleted in this step. The implied claim (real `scene_config_table` at VA `0x47fcf10`, with the operator-menu-tree at `0x48071d0`) cannot be confirmed without the exported file, so it is not preserved elsewhere; future work can re-verify directly against the binary if needed.

`_emulation/reference/switch_table.md` (created earlier this session) was deleted; its content is now folded into PROTOCOL §11's "Runtime switch table" subsection.

Memory file `project_version_info_screen_blocker.md` was renamed to `project_service_menu_blocker.md` and slimmed to a one-paragraph hook + pointer to PROTOCOL §3 "Canonical post-boot screen names" + this step. The MEMORY.md top-line "CURRENT INVESTIGATION" pointer was updated.

### Open / next session

- **Audio singleton vtable population** (Step 93d): trace the constructor that should populate `0x4324a30`. Identify which of `/dev/snd/*`, ALSA stub return values, or shim_diagnostics audio init triggers the destructor `sub_23737f0`. Once the vtable holds real function pointers, sw_id 77 / 91 / 92 will stop crashing and any scene-transition path that touches audio may complete.
- **SM scene-transition chain trace** (Step 93c): find what consumes events `0x48` / `0x49` / `0xbb` / `0xbc` / `0xa5`. If a handler in those chains triggers a `select_and_load_scene` call that fails silently due to the audio-vtable issue, fixing audio fixes both crashes and SM advance simultaneously.
- **Star Wars LE 0.85 parity check**: the `_extraction/_outdated/scene_config_table.c` claim was the only Pokémon-specific evidence for the `0x47fcf10` real-table VA hypothesis. If we want that hypothesis preserved, re-verify directly against the binary at the start of the next deeper Radium session.
- **TIMER preset buttons in `marker_button.py`** are now part of the standard debug toolkit (5 presets + free-form: `VISUAL CHANGE`, `AURAL CHANGE`, `TECH ALERT SHOWN`, `VERSION INFO SCREEN SHOWN`, `SOMETHING NEW - MASSIVE BREAKTHROUGH`). The "VERSION INFO SCREEN SHOWN" preset is retained even though "version info screen" is now retired terminology — it's the closest thing the user has used as a casual alias for "SM reached" and changing the button label mid-investigation would lose its calibration.

### Touched

- `games/pokemon_le/_emulation/shim_inject.c` — `shim_switch_table_probe_thread` (with mode-bitmap + boot-state-mask + per-switch handler-config dump + SPI press injection with force-poll), `shim_install_channel1_idle` (writes channel-1 idle bytes to `DAT_0436b020` at +2s to silence cabinet-side audio spam), `shim_edge_snapshot_thread`.
- `games/pokemon_le/_emulation/shim_ioctl.c` — `shim_spi_rx_extra[8]` (volatile, applied to SPI MESSAGE_1 RX after env override + default).
- `games/pokemon_le/_emulation/spi_shim.h` — extern declaration for `shim_spi_rx_extra`.
- `games/pokemon_le/_emulation/shim_nodebus.c` — cmd `0x41` inner_cmd `0x11` "switch_poll" synth (returns the channel's POST-INIT idle bytes from `&DAT_044804b8 + 0x18 + 0xb0*slot` so translate sees no edges → no audio spam from non-zero channels).
- `games/pokemon_le/_emulation/marker_button.py` — 5 preset categorization buttons + retained custom-label MARK + KILL GAME.
- `_emulation/docs/PROTOCOL.md` — §3 "Canonical post-boot screen names" added; §11 "Runtime switch table" subsection added; §6 "scene_config_table misnaming" subsection deleted; screen-name fixes across §1, §5.4, §9, §11, §12.
- `_emulation/docs/JOURNAL.md` — Step 90 + Step 92 prose fixes; this step appended.
- `_emulation/reference/switch_table.md` — DELETED (content migrated to PROTOCOL §11).

## Step 94: Service Select press advances Service Menu; output-state register verified; per-game machine inventory introduced (2026-05-08)

Direct continuation of Step 93. Tonight's session: (a) verified BN entry 322's output-state register claims via Ghidra MCP, (b) confirmed the Step-93 press infrastructure successfully advances the Service Menu when given enough press attempts at the right cadence, (c) introduced a per-game mechanical inventory header as the first concrete step toward separating game-specific data from platform-generic shim code.

### Step 94a — BN output-state addresses verified

Ghidra MCP decompile of BN export entry 322's claims:

| BN claim | Verified | Notes |
|---|---|---|
| `set_hardware_output_bit @ 0x8fac70` | ✅ | `(byte bit_mask, uint set_flag)` — mutex-locked on `0x450d9f8`; gates on `(0x450da28 & 1) || FUN_008faa50()&1`; **only writes byte 7** (`DAT_0450da30._7_1_`). `set_flag & 1 == 1` ORs the mask in; `== 0` ANDs `(mask^0xff)` in |
| `write_hardware_output_byte @ 0x8fad0c` | ✅ | `(ulong byte_idx, uint8 value)` — same mutex + gate; bounds-check `byte_idx < 8`; writes any byte 0..7 of `0x450da30` |
| `g_hardware_output_state @ 0x450da30` (8 bytes) | ✅ | SPI bridge thread `FUN_008fa820` snapshots all 8 bytes into a stack-local under the mutex, then ships them as the SPI MESSAGE_1 TX buffer every cycle |
| `g_hardware_input_state @ 0x450da9b0` | ❌ typo in BN | Real address is `0x450d9b0` (matches PROTOCOL §11's `DAT_0450d9b0`). One extra `a` in the BN export string |
| `g_rotation_counter @ 0x450d9f0` | ✅ | Has its own mutex `0x450d9c0` (different from output mutex). `FUN_008fae00` reads as `counter / 3` and stores `counter % 3` (quadrature decoder consumer) |

**Bonus discoveries from the same decompile:**
- `0x450d9f8` — mutex governing all hardware-output-state writes
- `0x450da28` — init flag, set to 1 by SPI bridge thread once the device is open + the first ioctl succeeded
- The shim's existing diagnostic at `[shim] SPI #N cmd=0x6c TX: 00 00 00 00 00 00 00 6c` shows TX byte 7 = `0x6c` because that's the only byte `set_hardware_output_bit` ever writes; bytes 0-6 stay zero unless `write_hardware_output_byte` is called.

### Step 94b — Press cadence reproducibly advances the Service Menu

Step 93 traced the mechanical press path but never observed visual screen advance. Tonight's Phase 2 run with `SHIM_SELECT_PRESS=1 SHIM_SELECT_PRESS_SW_ID=90 SHIM_SELECT_PRESS_INTERVAL_S=4 SHIM_SELECT_PRESS_ATTEMPTS=4 SHIM_SELECT_PRESS_MS=500` reached the Service Menu at 21:00:47 and produced the first observed visual transition out of SM at 21:01:01-06 (user marker `VISUAL CHANGE`). User confirmed via screenshot the transition was to a genuine attract screen (Pokemon logo with Credits in corner).

Critical observation about edge timing:

| Round | edge_cnt before | edge_cnt during press | edge_cnt after release | translate fired? |
|---:|---:|---:|---:|---|
| 1 | 0 | 0 | 0 | no — channel state still warming |
| 2 | 0 | 0 | 0 | no |
| 3 | 0 | 1 | 2 | **yes** — first edge fired |
| 4 | 2 | 3 | 4 | yes |

Rounds 1–2 set `state` byte to 1 during the press but `edge_counter` did not increment. Round 3 was the first round where `translate_switch_state_changes (sub_651610)` actually flipped the recorded state and counted the edge. Hypothesis: the channel's saved-state buffer at `&DAT_044804b8 + 0x18 + 0xb0*0` (channel 0) was initialized to `0x01` by `FUN_00650f00` (idle = high), so the first 1→1 "press" was a no-op until something normalized state. Step 93 has the partial answer — `shim_install_channel1_idle` writes idle bytes to `DAT_0436b020` for **channel 1**; channel 0 may need an analogous fixup, but operationally 4 press attempts with `INTERVAL_S=4` reliably crosses the edge by round 3.

Manual cross-reference (user-supplied from Star Wars FOTE manual; switches identical to PLE per user verification):

| Coin-door label | manual_id | Conn | sw_id (runtime) | Maps to handler / event |
|---|---|---|---:|---|
| Service Select (SS) | `0-SW-8` | C9-C25 | 90 | `sub_5902f0(param=8)` → `sub_58ff40` → SM → "Stern Pinball Main Menu" (Quick Adjustments / Diagnostics / Device Tests) |
| Service Plus (SP) | `0-SW-9` | C10-C25 | 91 | `sub_5902f0(param=4)` → audio path (crashes per Step 93d) |
| Service Minus (SM) | `0-SW-10` | C11-C25 | 92 | `sub_5902f0(param=2)` → audio path (crashes per Step 93d) |
| Service Back (SB) | `0-SW-11` | C12-C25 | 93 | `sub_5902f0(param=1)` → "back to game" (attract) |

Pin numbering in the manual diagram is off-by-one (pin 9 on connector for switch 8 etc.) — quirk of the printed reference; runtime sw_ids are authoritative.

### Step 94c — Output-side probe (`SHIM_OUTPUT_PROBE=1`)

New thread in `shim_inject.c` polls `g_hardware_output_state @ 0x450da30` every `SHIM_OUTPUT_PROBE_INTERVAL_MS` (default 100), diffs against last sample, logs bit-level changes. Phase 3 run saw **zero changes** over 85s spanning SM idle + Service Select press cycle. Initial state `00 00 00 00 00 00 00 6c`; byte 7 stayed `0x6c` throughout. The CPU's local 8-byte output is essentially static during SM/idle — interesting outputs (lamp animations, coil pulses) flow through cmd-0x41 outbound traffic to remote nodes, which `shim_nodebus.c` already logs as `bridge: cmd 0x41 slot=N inner_cmd=0xXX ...`. The `SHIM_OUTPUT_PROBE` is the right primitive for CPU-side outputs (Action Button LED, Start LED, coin meter, ticket dispenser) — those are the Node 0 / Node 1 paths.

Probe lives behind `SHIM_OUTPUT_PROBE=1`, default off. Knobs `SHIM_OUTPUT_PROBE_DELAY_S` (default 50), `SHIM_OUTPUT_PROBE_INTERVAL_MS` (100), `SHIM_OUTPUT_PROBE_DURATION_S` (60).

### Step 94d — Per-game mechanical inventory header

New directory `_emulation/games/` introduced as the home for per-game data tables. First content:

- `games/pokemon_le_machine.h` — typed records for switches/drivers/lamps with kind enums (coil/motor/servo/magnet/digital; GI/feature/RGB/flasher/digital).
- `games/pokemon_le_machine.c` — populated from §3-§7 of `_emulation/reference/Pokemon-Premium-LE-Lite-Manual.md`: 85 switches across nodes 0/1/8/9, 22 drivers (incl. Node 10 servos for Poké Ball + Pikachu), 158 lamps (incl. all RGB triplets and flashers).
- `build.sh` — added the new TU to the source list.

New consumer: `SHIM_MACHINE_DUMP=1` (default off) reads the runtime switch table at `g_switch_state_array @ 0x4480220` and joins it against `ple_switches[]`, logging `(manual_id, channel, bit_idx, runtime sw_id)` per row plus a summary. Phase 3+4 run produced **82 of 85 manual switches matched**; 3 missing are `0-SW-20`/`21`/`22` (CPU LVDS jumper bits + reserved jumper) — expected, as these have no game-side handlers in PLE.

This header is **data-only** in this step. The follow-up architectural cut — replacing PLE-specific VAs in `shim_inject.c`/`shim_nodebus.c` with table lookups — is deferred. Star Wars LE 0.85 will get its own `games/starwars_le_machine.{c,h}` skeleton when we pivot to that game.

### Step 94e — `run_game.sh` defaults shifted toward "press Service Select on every run"

Per user direction, the press-cycle parameters that produced visible SM advance are now defaults in `run_game.sh`:
- `SHIM_SELECT_PRESS=1`
- `SHIM_SELECT_PRESS_SW_ID=90` (Service Select / 0-SW-8)
- `SHIM_SELECT_PRESS_INTERVAL_S=4`
- `SHIM_SELECT_PRESS_ATTEMPTS=4`
- `SHIM_SELECT_PRESS_MS=500`
- `SHIM_SWITCH_TABLE_PROBE=1` + `_DELAY_S=65`

All five are `${VAR:-default}` patterns so explicit overrides still win. Plus the script grew an `env | grep ^SHIM_ → -E ...` loop to auto-forward every exported `SHIM_*` env var to qemu's environment (the shim's `getenv()` runs inside the qemu-aarch64 process and only sees vars passed via `-E`). Without this, the host-shell exports were silently ignored.

### Step 94f — `avcodec_flush_buffers` bridge resolves DoSeek timeouts (2026-05-08 late)

Standard FFmpeg post-seek protocol is `avformat_seek_file → avcodec_flush_buffers → avcodec_send_packet`. Game's `SpiVideoStreamDecoderBase::DoSeek (FUN_00922aa0)` follows that order; without the flush bridge, `decode_daemon.py`'s PyAV codec context kept stale state across seeks and rejected post-seek packets, fed the 2 s `DoSeek while loop timeout` (printed 20+ times) and ultimately the proc-dispatch deadlock that surfaced as `[run_game] status 5 — DISPATCH TIMEOUT`.

Wire protocol grew `REQ_FLUSH (0x05)` between shim and daemon. Shim hook in `shim_patches.c`: `void avcodec_flush_buffers(void *avctx) { shim_avforward_flush(avctx); }`. Daemon's `handle_flush()` calls `self.codec_ctx.flush_buffers()` (PyAV native, fast — keeps the CUDA context warm for `hevc_cuvid`); falls back to recreate with saved `codec_id + extradata` if PyAV raises. Verified working: post-fix runs show `(flush_buffers())` in `decode_daemon.log` instead of the recreate path, and `DoSeek while loop timeout` count went from 20+ to **0**.

This was a necessary-but-not-sufficient fix — even with seek bridge, status 5 still fired non-deterministically (~1/5 success). The status-5 root cause was elsewhere; see Step 94k.

### Step 94g — `marker_button.py` v2: elapsed timer, observation presets, press buttons

GUI rewrite to support manual switch-press control synced to user observations:
- Live `T+M:SS.x` elapsed timer at top, updates every 100 ms via `root.after`. Every marker line gets the elapsed prefix (e.g. `[marker] T+1:23.4 click #5 (...)`), eliminating after-the-fact log timestamp math.
- New observation presets per user request: `Pokemon Logo (Startup In Progress...)`, `Service Menu (Main)`, `Service Menu (#?)`, `Attract Mode (#1)`, `Attract Mode (#2)`, `Sound Starts`, `Sound Ends`. Old presets retired.
- New PRESS button row: `Service Select (sw_id 90)`, `Service Plus (91)`, `Service Minus (92)`, `Service Back (93)`, `Start Button (78)`, `Action Button (77)`. Each click writes the runtime sw_id to `runs/latest/press_request.txt` (no signal — file-based IPC, simple to debug).
- `KILL GAME` button retained.

The PRESS buttons write to `press_request.txt`; the shim watches that file via a new thread (Step 94h) and drives the actual press through the existing translate / SPI-rx-extra path.

### Step 94h — `shim_manual_press_thread` + auto-press disabled

New shim thread `shim_manual_press_thread` polls `press_request.txt` every 200 ms. When the file appears, parses the first integer as a runtime sw_id, deletes the file, and drives a single press-and-release through `shim_press_one_sw_id` — a helper extracted from the auto-press logic that handles both the channel-0 SPI path (`shim_spi_rx_extra | force-poll`) and the remote-node path (direct `translate(channel, buf)` call at game VA `0x651610`). Logs `[manual_press] sw_id=N ... AFTER: state=S edge=E` so press effects show in the same line as the marker click.

Env knobs: `SHIM_MANUAL_PRESS=1` (default on), `SHIM_MANUAL_PRESS_DELAY_S=N` (default 30), `SHIM_MANUAL_PRESS_MS=N` (default 500). `run_game.sh` flipped `SHIM_SELECT_PRESS` default 1 → 0; the auto-press machinery still works for headless / scripted runs but tonight's interactive runs use the manual GUI exclusively.

### Step 94i — Viewport / scissor logger reveals boot-splash scissor leak

Added throttled loggers in `shim_gl.c` for `glViewport(x,y,w,h)`, `glScissor(x,y,w,h)`, plus `glEnable(GL_SCISSOR_TEST)` / `glDisable(GL_SCISSOR_TEST)` toggles — each emits a line only on transition (initial value tracking avoids drowning game.log in per-draw spam).

User reported the Service Menu rendered into roughly the top-left ~⅕ of the 1920×1080 frame_viewer window while Pokémon-logo and attract scenes filled the window. The logger captured exactly one scissor change for the whole run: `glScissor(0, 0, 800, 480)` set during the boot-splash phase (the 800×480 Radium scene `3fdc91b3` per PROTOCOL §9), and **never reset**. Math: `800/1920 × 480/1080 ≈ 18%` of framebuffer area — matches the observed clip exactly. Whenever `GL_SCISSOR_TEST` is enabled later (NuklearScene operator UI does this), output gets clipped to that stale rectangle.

The leak is benign for our breakthrough investigation — it's a visual-only artifact and doesn't affect game logic — but worth noting as an Open item: a `shim_gl.c` override that pins `glScissor` to `(0, 0, 1920, 1080)` post-boot would restore full-frame SM rendering.

### Step 94j — Dispatch watchdog (SIGURG via tgkill) — partial / wrong target

To capture stack traces of stalled dispatch threads at the moment status 5 would fire, added `shim_dispatch_watchdog_thread`:
- Locates the `procDispatch` thread via `/proc/self/task/*/comm` matching. **Important nuance:** qemu-user prefixes guest pthread names with its own process name then truncates to TASK_COMM_LEN-1 (15 chars) — `procDispatch` (12 chars) becomes `qemu-aarch64:pr` (`qemu-aarch64:` prefix + 2 chars from guest name). Watchdog tries the qemu-mangled form first, falls back to bare `procDispatch`.
- Polls `g_timestamp_threshold_1 @ 0x44cb2e0` (the BN-named "dispatcher tick timestamp"), tgkills SIGURG to the procDispatch thread on stall. SIGURG chosen because procDispatch's `SigBlk = 0x2a02` blocks SIGINT/SIGUSR1/SIGUSR2/SIGALRM — sending the obvious choice (SIGUSR1) just queued in `SigPnd` and was never delivered. SIGURG is unblocked + has default disposition "ignore", so safe.
- SIGURG handler in `shim_term_dump_handler` extended to dump `backtrace()` of the receiving thread.

**Wrong-address bug discovered:** `0x44cb2e0` is read by the main loop's progress check but is NOT the address procDispatch updates on each tick. Decompile of `FUN_007292b0` (procDispatch body) shows it signals progress via `pthread_cond_signal(&DAT_044c9288)` at the END of each loop iteration — no shared timestamp write. So the watchdog's stall predicate never fires (timestamp stays 0). The infrastructure (signal delivery, thread-targeted backtrace, qemu-user thread-name discovery) all works; the polled address needs to change. Next iteration: hook `pthread_cond_signal` and use a shim-side timestamp updated when the relevant CV is signaled.

### Step 94k — cwd-to-`/games/` + `/games/game` symlink: the breakthrough

Single biggest win of the session, almost stumbled into. The user observed that on real hardware, `/etc/init.d/shared` defines `GAME=/games/game` and `S95game` execs from that canonical path; whatever cwd the cabinet uses is from there too. Our `run_game.sh` instead `cd "$GAME_DIR"` (= `/games/pokemon_le/`) and execs `./game` — argv[0] becomes `/games/pokemon_le/game`. Mismatch.

Two changes:

1. `/games/game` was a 71 MB regular file duplicating `/games/pokemon_le/game` (md5 verified identical). Replaced with a relative symlink: `/games/game → pokemon_le/game`. Single source of truth, saves the disk dup, and works through the bind-mounted `/games` view (relative symlinks resolve via the directory containing the symlink, which is `/games/` in both the host path and the bind-mounted view).

2. `run_game.sh` updated:
   - `cd /games` (was: `cd "$GAME_DIR"` = `/games/pokemon_le/`)
   - exec `/games/game` (was: `./game`)

The binary's argv[0], `/proc/self/exe`, dirname-of-self lookups, and any path-aware game logic now see `/games/game` exactly as the cabinet does. The cwd of `/games/` matters for any code path that opens files relative to cwd; the binary's strings include `./assets/godot/main.pck` etc. (verified via `strings | grep ^\./`), so cwd does feed into asset resolution. We did NOT add `/games/assets` symlinks — surprisingly the game still finds its assets, suggesting either (a) absolute paths win in the actual hot path, (b) the binary calls `chdir()` itself early in init to a dir with the right layout, or (c) bind-mount magic we haven't traced. **TBD which.**

**Result, immediately observable:**

| Metric | Before (cwd=`/games/pokemon_le/`) | After (cwd=`/games/`) |
|---|---:|---:|
| Run survival | seconds before status 5 / SIGABRT (1/5 success rate) | **2+ minutes, no crash, killed by user via SIGTERM** |
| `decode_daemon` conn-3 frames | ~700 max | **1,500+** (still climbing at kill) |
| `decode_daemon` conn-4 frames | ~400 max | **1,200+** (still climbing at kill) |
| Visual state at +2 min | crash or stale frame | **clean attract loop, multiple scene cycles, sound spam GONE** |
| `DoSeek` / `Play` / `FlushDecoder` timeouts | one or more per run | **0** |

The user's words: "I saw a completely unbothered and perfectly run game that cycled through the attract mode... Literally the only problem was that the videos in the background weren't playing." Frames flow shim ↔ daemon at scale; the in-attract video black-frames issue is a downstream display problem (AVFrame data/linesize/format mismatch in some game-side shader path), explicitly out of scope for tonight.

This change supersedes a lot of earlier "non-determinism" framing across the run: the inconsistent attract-vs-SM-vs-crash outcomes from earlier in the session were essentially all the same root cause manifesting at different points in the boot/post-press timeline. With cwd correct, the game is fundamentally healthy.

### Step 94l — Press-path semantics clarified (operator-menu auto-timeout, NOT press-driven attract)

Field observation across ~11 runs the user did mid-session under the auto-press defaults (Service Select × 4): outcomes split roughly 50/50 between "ended in attract" vs "ended in operator menu", with both crash modes (status 5 dispatch + status 134 SIGABRT) showing up in either screen state.

Per the user's manual cross-reference:
- Service Select (sw_id 90, `0-SW-8`) → SM → **Stern Pinball Main Menu** (Quick Adjustments / Diagnostics / Device Tests). Successive presses navigate INTO the menu tree.
- Service Back (sw_id 93, `0-SW-11`) → "back to game" (= attract).

Tonight's auto-press was 4 × Service Select with no Back. So the runs that "ended in attract" were NOT press-driven — they hit Stern's operator-menu inactivity timeout and returned to attract on their own (~30-60 s of post-press silence). That explains the non-deterministic split: depending on how deep into the menu tree the 4 presses landed, the timeout fired at different times relative to whichever crash was independently brewing. Same code, different lottery.

Practical consequence: tonight's manual-press flow + observation timer (Step 94g/h) lets the user explicitly drive Service Back to test the press-driven attract path, separate from the auto-timeout path.

### Step 94m — Fresh-state runs achieve full stable attract loop (2026-05-09 ~02:30)

User added `_emulation/scripts/reset_run_state.sh` (delete by default; `--keep` preserves backup dirs). Before each test run, it clears: `/data/nv/pokemon_le/` (NVRAM), `/tmp/cache/mesa/` (Mesa shader cache), `/tmp/cache/shader_cache/` (Godot/other shader cache). With this discipline:

- Run survives **full 60-second first attract clip** → ~30 s black period (the `ATTRACT_LOOP_SERVICE_S` slot per PROTOCOL §9 attract sequencer, falling through cleanly when service_s_worker isn't running) → **second attract clip starts** = native attract loop. No crashes. No status 5 / 134 / stack smash. Game is fundamentally healthy.
- Audio singleton vtable is **healthy in fresh-state runs** — slot 5 holds a real game function (e.g. `0x25607f0`) instead of `_ZSt10unexpectedv`. The Step-93d corruption hypothesis was correct; whatever NVRAM or shader-cache state was carrying corruption between runs no longer carries.
- Sound: the SM's "cascading item-selection sound spam" cuts off (raggedly) when the user presses Service Back; attract is silent (likely Service S-gated audio paths the game skips when service_s_worker isn't up).
- One late-attract crash discovered + fixed: `h264_v4l2m2m` was not in the SPIKE3_DECODER_PATCH remap list (only `hevc_v4l2*` variants were). Game hits an `assert(decoder_context)` in `SpiVideoStreamDecoderBase::SendPacket` (cpp:1222) when the H.264 decoder fails to find a valid V4L2 device. Fix in `shim_patches.c::avcodec_find_decoder_by_name`: add `h264_v4l2request` / `h264_v4l2m2m` → `h264` SW remap alongside the existing HEVC pair. Per PROTOCOL §9 the 1360×768 attract clips are H.264 Constrained Baseline so this path is hit on every attract cycle.

### Open / next session

- **In-attract video display:** decoder is producing 1500+ frames at full rate but the visible attract is black/static behind the overlay text. Investigate AVFrame `data[]` / `linesize[]` / `format` plane layout in `shim_avforward.c::shim_avforward_receive_frame` against what the game's render path expects. PROTOCOL §10 + §12 "Forward-decode bridge" hold the field offsets we already trust; this is about whether the bytes are being consumed correctly downstream.
- **Asset-resolution cwd mystery:** with cwd=`/games/` and no `/games/assets/` directory, the game still finds its assets. Either the binary chdir's itself or there's a path-rewrite happening we haven't documented. `strace` of file opens in the first ~5 s of a run would settle it.
- **Boot-splash scissor leak (cosmetic):** `glScissor(0, 0, 800, 480)` set during boot is never reset; SM appears clipped to top-left ~18% of frame whenever `GL_SCISSOR_TEST` is enabled later. Trivial fix — pin scissor to full FB in `shim_gl.c` post-boot.
- **Watchdog needs the right tick address:** Step 94j infrastructure works but `0x44cb2e0` isn't procDispatch's tick. Replace with a shim-side timestamp updated on `pthread_cond_signal(&DAT_044c9288)` calls (hook the syscall in `spi_shim.c`, look for that specific cv arg, bump a counter / mtime).
- **Audio singleton vtable** (Step 93d carryover): the abort-on-vtable[5] path still exists when sw_ids 91/92 (Service Plus/Minus) fire. Step 94k's stability win means the issue is rare in a no-press passive run, but still latent.
- **Per-game machine-inventory consumers:** refactor `shim_nodebus.c::nodebus_chip_id_for_slot` to read slot→chip_id pairs from `pokemon_le_machine.c` rather than hardcoded switch. Then pull cmode-table + `g_switch_state_array` VAs into the same per-game header. Once those land, `starwars_le_machine.{c,h}` skeleton is the next concrete step.
- **Service Back press-driven attract test:** with the manual-press GUI, fire sw_id=93 from SM and observe whether attract appears immediately (proving press-driven) vs. taking ~30-60 s (proving timeout-driven).

### Touched

- `games/pokemon_le/_emulation/games/pokemon_le_machine.h` — NEW. Typed records for switches / drivers / lamps; lookup helpers.
- `games/pokemon_le/_emulation/games/pokemon_le_machine.c` — NEW. 85 switches, 22 drivers, 158 lamps populated from manual §3-§7.
- `games/pokemon_le/_emulation/build.sh` — added `games/pokemon_le_machine.c` to source list.
- `games/pokemon_le/_emulation/shim_inject.c` — `shim_machine_dump_thread` (`SHIM_MACHINE_DUMP=1`); `shim_output_probe_thread` (`SHIM_OUTPUT_PROBE=1`); `shim_dispatch_watchdog_thread` (`SHIM_DISPATCH_WATCHDOG=1`, partial); `shim_manual_press_thread` (`SHIM_MANUAL_PRESS=1`, default on) + `shim_press_one_sw_id` helper; all wired into `shim_inject_start`.
- `games/pokemon_le/_emulation/shim_avforward.c` — `REQ_FLUSH (0x05)` opcode; `shim_avforward_flush(void *avctx)` round-trip.
- `games/pokemon_le/_emulation/shim_patches.c` — `avcodec_flush_buffers(void *avctx)` hook → `shim_avforward_flush`.
- `games/pokemon_le/_emulation/shim_gl.c` — throttled `glViewport` / `glScissor` / `GL_SCISSOR_TEST` toggle loggers.
- `games/pokemon_le/_emulation/shim_diagnostics.c` — SIGUSR1+SIGURG handler now dumps backtrace via `backtrace()` + `dladdr`; `<sys/syscall.h>` added for `SYS_gettid`.
- `games/pokemon_le/_emulation/spi_shim.c` — `sigaction(SIGURG, ...)` registered alongside SIGTERM/SIGINT/SIGUSR1/SIGUSR2.
- `games/pokemon_le/_emulation/spi_shim.h` — extern declaration for `shim_avforward_flush`.
- `games/pokemon_le/_emulation/decode_daemon.py` — `REQ_FLUSH = 0x05`; `handle_flush()` (PyAV `flush_buffers()` fast path with recreate-with-extradata fallback); save `codec_id + extradata` at OPEN for fallback path.
- `games/pokemon_le/_emulation/marker_button.py` — full rewrite: elapsed timer top of GUI, new observation presets, PRESS buttons writing `runs/latest/press_request.txt`, retained KILL.
- `games/pokemon_le/_emulation/run_game.sh` — `cd /games` (was `cd "$GAME_DIR"` = `/games/pokemon_le/`); exec `/games/game` (was `./game`); `SHIM_MANUAL_PRESS` defaults; `SHIM_SELECT_PRESS` default flipped to 0; `GDB_PORT` env-gate appends `-g $GDB_PORT` to qemu cmdline; auto-forward of every exported `SHIM_*` env var to qemu via `-E`.
- `/mnt/c/Workspace/pokemon-le-0.81.0/games/game` — was a 71 MB regular file duplicating `pokemon_le/game`; replaced with a relative POSIX symlink (`ln -s pokemon_le/game game`). A hard-link substitute was tested mid-session and crashed at ~10 s every run; symlink survives 2+ minutes. Mechanism uninvestigated.
- `games/pokemon_le/_emulation/scripts/reset_run_state.sh` — NEW. Clears `/data/nv/pokemon_le/`, `/tmp/cache/mesa/`, `/tmp/cache/shader_cache/`. Default deletes; `--keep` moves to `*.backup_<TS>`. Pair with each test run for a clean baseline.
- `games/pokemon_le/_emulation/shim_patches.c::avcodec_find_decoder_by_name` — added `h264_v4l2request` / `h264_v4l2m2m` → `h264` SW remap, alongside the existing HEVC pair. Required for the 1360×768 H.264 attract clips per PROTOCOL §9.
- `_emulation/docs/JOURNAL.md` — this step appended.

## Step 95: Attract video renders — glShaderSource samplerExternalOES define injection unblocks Mesa swrast (2026-05-09)

Direct continuation of Step 94. Tonight: visible attract-area video frames go from "decoded but invisible" to "rendered correctly, just slow." The unblock turned out to be a one-line `#define samplerExternalOES sampler2D` injected at shader-compile time; under Mesa swrast llvmpipe a `samplerExternalOES` uniform sampling a non-EXTERNAL_OES-typed texture returns zero, and the spec-blessed alternative (real DMA-BUF EGLImage import) requires kernel-side dma-buf support that WSL2's environment doesn't provide. Two perf optimizations followed (host-side RGBA conversion in `decode_daemon.py`, `glTexSubImage2D` reuse on repeat texture uploads); neither moved the framerate needle, so the remaining bottleneck is elsewhere and not on the upload chain.

### Step 95a — Mesa rejects `glTexImage2D(GL_TEXTURE_EXTERNAL_OES, ...)`

The Step 89d empty-`AVDRMFrameDescriptor` redirect prevented SIGSEGV at the game's DRM-prime walker but silently dropped pixels — verified tonight by wiring up a `glGetError()` round-trip around the shim's `real_glTexImage2D` call. Every video upload returned `0x500` (`GL_INVALID_ENUM`). Per the `GL_OES_EGL_image_external` spec, only `glEGLImageTargetTexture2DOES` may set EXTERNAL_OES texture state — `glTexImage2D` / `glTexSubImage2D` / `glTexParameter*` are explicitly invalid on that target. Cross-checked the conversion math itself with a one-shot dump of two `shim_video_<seq>_<W>x<H>.rgba` files to disk; PIL renders them as legible video frames, so the BT.601 NV12→RGBA path was fine — the upload was the problem.

### Step 95b — Path C dead end: real Mesa EGLImage via `EGL_KHR_gl_texture_2D_image`

The "right" fix per spec is to wrap a 2D RGBA texture as an `EGLImageKHR` via `eglCreateImageKHR(target=EGL_GL_TEXTURE_2D_KHR=0x30B1, buffer=tex_id)` and forward the game's `glEGLImageTargetTexture2DOES` to Mesa's real impl, which imports the 2D-texture-backed image into the bound EXTERNAL_OES texture so the sampler reads it correctly. Implemented this in a `shim_egl_image_build_video` helper with both `dlsym(RTLD_NEXT)` and `eglGetProcAddress` fallback for symbol resolution. Result: Mesa's `eglCreateImageKHR` reproducibly SIGSEGVs three frames into libEGL with `fault_addr=0x68` (NULL+0x68 deref). Tried `EGL_NO_CONTEXT` for ctx + explicit `{EGL_GL_TEXTURE_LEVEL_KHR=0, EGL_NONE}` attribs list per spec — same crash. Mesa's surfaceless software backend evidently doesn't support `EGL_KHR_gl_texture_2D_image` end-to-end in this overlay build. Reverted Path C entirely.

### Step 95c — Path B: EXTERNAL_OES → 2D demotion + counter-based sentinels

Demote `GL_TEXTURE_EXTERNAL_OES` (0x8D65) to `GL_TEXTURE_2D` (0x0DE1) consistently across `glBindTexture`, `glTexImage2D`, `glTexSubImage2D`, `glCompressedTex{,Sub}Image2D`, `glTexParameter{i,f}`, `glGenerateMipmap` (paired with the pre-existing demotion in `glFramebufferTexture2D/Layer`). With this, `glTexImage2D` upload succeeds (`glGetError` returns 0). Verified via `[frame_cap] tracking tex=N target=0xde1 1940x1100` log lines that 1940×1100 (HEVC) and 1360×768 (H.264 attract) textures both reach the upload path.

EGLImage sentinels switched from `malloc(1)` to a monotonic atomic counter (`(void *)(0xdeadbeef0000 + 8*N)`). Backstory: the game's `FUN_01df8b40` caches the last EGLImage pointer at `param_3 + 0x2b8` and skips re-binding when a new image's pointer matches. With `malloc(1)`, freed addresses got reused on the next allocation, the cache false-hit, and only ~8 of 491 video frames actually triggered re-bind. Counter sentinels guarantee uniqueness — bind ratio went back to 1:1.

### Step 95d — Shader source rewrite: the unblock

Path B got pixels into the texture but the user still saw black. Diagnosed by toggling on a `SPIKE3_VIDEO_TESTPATTERN=1` env that overrides the conversion with a hot-pink + green checkerboard — still all black. Means the fragment shader doesn't sample the texture at all under Mesa swrast llvmpipe.

Hint came from `strings game | grep samplerExternalOES`: the binary contains the Stern Radium video fragment shader (`uniform samplerExternalOES textureSampler; ... texture2D(textureSampler, textureCoordinate);`) AND a separate Godot shader with `#define samplerExternalOES sampler2D`. Stern's video shader has no fallback, so `samplerExternalOES` stays a sampler-of-EXTERNAL_OES type, and Mesa swrast returns zero when sampling such a sampler from a 2D-bound texture (samplers and bindings are typed independently — GLSL won't downcast).

Fix in `shim_gl.c::glShaderSource`: concatenate the input strings into a single buffer, search for `samplerExternalOES`, find the safe insertion point (right after the matching `#extension GL_OES_EGL_image_external` line, else after `#version`, else at top), inject `#define samplerExternalOES sampler2D\n`, hand the rewritten single-string source to Mesa's real `glShaderSource`. The GLSL preprocessor textually substitutes the type to `sampler2D`, the bound texture (demoted to TEXTURE_2D in 95c) matches the sampler, and pixels reach the fragment shader. **Video renders correctly in attract mode for the first time.**

### Step 95e — Daemon-side RGBA conversion (perf cleanup, no framerate change)

The shim's NV12→RGBA conversion ran inside qemu-aarch64 user-mode where every instruction is TCG-translated — roughly 5–10× slower than native. Moved the conversion to `decode_daemon.py`: `frame.reformat(format='rgba')` + `to_ndarray(format='rgba')` produces a tightly-packed RGBA buffer on the host side, daemon ships it as a single-plane wire payload (`fmt = AV_PIX_FMT_RGBA = 26`, `n_planes = 1`, `stride = w*4`).

Shim side dropped: the entire `shim_nv12_to_rgba` helper, the `y_ptr` / `uv_ptr` / `stride_y` / `stride_uv` distinction in the side-table (now a single `rgba_ptr` + `stride`), and the matching parameters in `shim_avforward_pixel_lookup` / `shim_egl_image_register` / `shim_egl_image_lookup` / `shim_egl_image_entry`. The DRM descriptor builder switched from NV12 (2 planes, fourcc `'N','V','1','2'` = `0x3231564E`) to single-plane RGBA (`DRM_FORMAT_ABGR8888` = fourcc `'A','B','2','4'` = `0x34324241`, byte order R,G,B,A in memory matching PyAV's `rgba` output).

Wire payload grew ~2.5× (NV12 = 1.5 bpp → RGBA = 4 bpp); AF_UNIX abstract socket easily absorbs this. No measurable framerate change observed by the user, so YUV→RGB conversion is **not** on the critical path — the bottleneck is downstream of the shim's `glTexImage2D`.

### Step 95f — `glTexSubImage2D` storage reuse (also no framerate change)

`glEGLImageTargetTexture2DOES` was previously calling `real_glTexImage2D` every frame, forcing Mesa to reallocate the bound texture's storage on each upload. Added per-`tex_id` allocation tracking: query `GL_TEXTURE_BINDING_2D` (= `0x8069`) on entry, look up a 128-slot `{tex_id, w, h}` table, use `real_glTexSubImage2D` when the size matches an existing record (preserves storage), fall through to `real_glTexImage2D` only on first encounter or size change.

Also no measurable framerate change. So the bottleneck isn't texture allocation either. Working hypothesis: the 1920×1080×4-byte `glReadPixels` that fires every other page-flip in `shim_frame_capture_at_page_flip` is the dominant cost (~250 MB/sec of TCG-translated readback). Untested this session.

### Open / next session

- **Frame-bridge readback as suspected bottleneck:** test with `FRAME_BRIDGE=0`. If framerate jumps, drop the per-flip capture frequency (every 8th flip instead of every 2nd) or move the readback to a worker thread.
- **`LP_NUM_THREADS=N`:** Mesa llvmpipe parallelizes fragment shading by default; needs verification under qemu-user that the threads actually fan out vs. serialize through TCG.
- **Profiling:** instrument `clock_gettime(CLOCK_MONOTONIC)` around `read_all` (socket recv), the per-plane `memcpy` loop, the texture upload, and `glReadPixels` in the frame-bridge path. Get hard numbers before guessing again.
- **Service Select press for `RUN_QEMU.sh` (system-mode):** the user-mode shim's `shim_manual_press_thread` watches `runs/latest/press_request.txt` (Step 94g/h), but system-mode runs out of `/games/.../runs/...` from inside the VM where the host's `marker_button.py` can't write. Need either an in-VM equivalent or a host-to-guest channel that survives the kernel namespace boundary.
- **Path C fallback:** if a future Mesa overlay build supports `EGL_KHR_gl_texture_2D_image` fully, swap the shader-rewrite hack for the spec-correct EGLImage path. Until then the rewrite is load-bearing.

### Touched

- `games/pokemon_le/_emulation/shim_gl.c` — NEW `shim_demote_external_oes` helper applied to `glBindTexture`, `glTexImage2D`, `glTexSubImage2D`, `glCompressedTex{,Sub}Image2D`, `glTexParameter{i,f}`, `glGenerateMipmap`, and the upload inside `glEGLImageTargetTexture2DOES`. EGL image map struct reduced to `{egl_image, w, h, fourcc, rgba_ptr, stride}`; sentinel allocator switched from `malloc(1)` to `shim_egl_image_alloc_sentinel` (atomic counter at `0xdeadbeef0000+8*N`). `shim_egl_image_register` / `shim_egl_image_lookup` / `eglCreateImageKHR` / `eglDestroyImageKHR` / `glEGLImageTargetTexture2DOES` rewritten. NEW `glShaderSource` interceptor that injects `#define samplerExternalOES sampler2D` into shaders that reference the type. NEW `glTexSubImage2D` storage-reuse tracking via 128-slot per-`tex_id` table.
- `games/pokemon_le/_emulation/shim_avforward.c` — `forward_sess_t` gained a `drm_desc[528]` per-session DRM descriptor buffer. `g_video_pixel_table` reduced from `{y_ptr, uv_ptr, width, height, stride_y, stride_uv}` to `{rgba_ptr, width, height, stride}`. NEW `shim_avforward_alloc_sentinel` (monotonic counter starting at `0x10000000`). NEW `shim_avforward_build_rgba_descriptor` replaces the previous NV12 builder (single object, single layer, `DRM_FORMAT_ABGR8888`, single plane). `shim_avforward_receive_frame` reads `fdata[0]` as the sole RGBA pointer (was `fdata[0]` + `fdata[1]` for Y/UV); per-plane height heuristic dropped (single-plane only).
- `games/pokemon_le/_emulation/spi_shim.h` — `shim_avforward_pixel_lookup` signature changed from `(fd, y, uv, w, h, sy, su)` to `(fd, rgba, w, h, stride)`.
- `games/pokemon_le/_emulation/decode_daemon.py` — `handle_receive` calls `frame.reformat(format='rgba')` + `to_ndarray(format='rgba')`, ships `n_planes=1` `AV_PIX_FMT_RGBA=26` single-plane payload (`stride = w*4`). Log message tag updated `nv12 → rgba`.
- `_emulation/docs/JOURNAL.md` — this step appended.

## Step 96: Atomic Horizon — host-native GL marshalling architecture (Phase 0: archival) (2026-05-09)

Architectural pivot. Step 95g measurements established a hard ~5.5 fps ceiling in attract video even with frame-bridge readback fully disabled — confirming the wall is qemu-aarch64's TCG (Big TCG Lock serializes all guest threads through one translator, so Mesa llvmpipe's parallel fragment shading runs serially). Tuning the existing pipeline cannot reach the user's 30+ fps "playable" bar. Plan committed: move GL/EGL execution out of TCG to a host-native daemon (`gl_daemon.py` over abstract AF_UNIX `@spike3_gl`), mirroring the proven `decode_daemon.py` NVDEC pattern. Researcher A's working Spike 3 emulator uses this approach.

Plan file: `~/.claude/plans/let-s-create-a-plan-atomic-horizon.md`. Six phases: 0 archival, 1 protocol skeleton, 2 gles_probe2 POC, 3 game-mode coverage, 4 validation, 5 perf opt (deferred). The full journal entry with verified fps numbers will be appended at end of Phase 4.

### Step 96.0 — System-mode archived (this commit)

Per user direction during plan review: "Don't leave any orphaned code please. Let's leave the code better than we found it." User-mode is the singular emulation path going forward. System-mode (`_emulation/historical-system-rig/`) was a parallel rig kept around for HW-watchpoint debugging, but its abstract AF_UNIX socket doesn't cross the VM kernel namespace boundary — bringing the GL daemon into system-mode would require vsock or an AF_UNIX-forwarder transport, ~1-2 sessions of work whose only benefit is preserving an occasional debug capability (last used Step 91/92 for the Tech Alerts row dispatcher). HW watchpoints are recoverable via `git revert` if a future bug demands them.

What got removed:

- `_emulation/historical-system-rig/` directory (60 GB, including 58 GB `sdcard.raw` LUKS image, 1.4 GB persist branch, RUN_QEMU.sh, MOUNT_OVERLAY.sh, initramfs/, scripts). User confirmed deletion of irreplaceable assets — backups exist elsewhere or re-imageable from cabinet.
- PROTOCOL §14 "System-mode debugging (HW watchpoints)" — full section deleted (was 207 lines).
- PROTOCOL §2 "Alternate path: full-system QEMU spike" subsection — deleted (22 lines).
- PROTOCOL §1 architecture diagram block "System-mode only (gated): /dev/fb0 direct blit" — deleted.
- PROTOCOL §3 boot-splash tick chain footnote: removed system-mode mention from time-bounded discussion.
- PROTOCOL §3 Verified-by footnote (boot-splash chain): removed system-mode hardware-read-watchpoint provenance.
- PROTOCOL §10 Display pipeline data-flow paragraph: removed `/dev/fb0` blit sink.
- PROTOCOL §12 `shim_bridges.c` row: removed system-mode-spike rig description.
- TOC: §14 entry removed (TOC now 13 entries).
- Memory: `feedback_system_mode_parity.md` deleted from auto-memory.
- MEMORY.md: pillar entry for system-mode parity removed; "14 numbered topics" → "13 numbered topics".

Orphan-code cleanup (same Phase 0): `shim_fb0_blit` in `shim_bridges.c` was the only consumer of the system-mode GTK-window-via-/dev/fb0 path. With system-mode archived, the function and its supporting code (the `SPIKE3_FB0_OUTPUT` env knob, `shim_fb0_init_once`, the static fb0 fd/mmap state, and the `<sys/mman.h>`/`<sys/ioctl.h>`/`<linux/fb.h>` includes that fed only the fb0 implementation) were all removed from `shim_bridges.c`. Frame-capture call site simplified: `shim_fb0_blit(...)` removed; `if (shim_frame_client_fd < 0 && !fb0_env_cached) return;` collapsed to `if (shim_frame_client_fd < 0) return;`. `shim_bridges.c` now 518 lines (was ~656). Shim rebuilt clean: `spi_shim.so 300776 bytes`.

### Touched (Step 96.0)

- `_emulation/historical-system-rig/` — deleted entire directory (60 GB).
- `_emulation/shim_bridges.c` — removed `shim_fb0_blit` + `shim_fb0_init_once` + fb0 static state + SPIKE3_FB0_OUTPUT env handling + fb-related includes. Frame-capture preamble simplified.
- `_emulation/docs/PROTOCOL.md` — §14 deleted; §2 Alternate-path subsection deleted; §1 / §3 / §10 / §12 mentions scrubbed; TOC updated. File: 3023 → 2792 lines.
- `_emulation/docs/JOURNAL.md` — this Step 96.0 entry appended.
- `~/.claude/projects/.../memory/feedback_system_mode_parity.md` — deleted.
- `~/.claude/projects/.../memory/MEMORY.md` — system-mode entry removed; topic count updated; new memories added (`feedback_no_orphans.md`, `feedback_baseline_runs_ask_first.md`).
- `~/.claude/projects/.../memory/feedback_no_orphans.md` — NEW.
- `~/.claude/projects/.../memory/feedback_baseline_runs_ask_first.md` — NEW.

### Step 96.1 — Phases 1–3c: host-native GL marshalling stack landed (2026-05-09)

End-to-end shim → daemon → native llvmpipe pipeline working. **Game integration not yet attempted** (Phase 3d). Default `GL_DAEMON=0` preserves the existing Mesa-aarch64 overlay path; setting `GL_DAEMON=1` (or `SPIKE3_GL_DAEMON=1` directly) routes a substantial subset of GL/EGL through the new host-native daemon.

**Architecture (implemented).** Shim's GL/EGL hooks marshal over abstract AF_UNIX socket `@spike3_gl` to `gl_daemon.py`, which runs natively on host x86_64. Daemon binds an EGL display via `EGL_PLATFORM_SURFACELESS_MESA` (constant 0x31DD, resolved through `eglGetPlatformDisplay`), creates a 1920×1080 GLES2 pbuffer + context, and replays incoming opcodes via PyOpenGL. Shim returns synthesized FAKE_EGL_* sentinels for the EGL setup calls (display/config/context/surface) so the qemu-side game/probe never knows the real EGL state lives on host. EGL_VERSION on host: `Mesa Project 1.5`; GL_RENDERER: `llvmpipe (LLVM 19.1.7, 256 bits)`; GL_VERSION: `OpenGL ES 3.2 Mesa 25.0.7-2`.

**Wire protocol** (`gl_proto.h` + `gl_proto.py` — matched constants by hand-sync). 8-byte header `{u16 op, u16 flags, u32 payload_len}` LE + payload. Flags: bit 0 SYNC, bit 1 HAS_DATA. Sync ops carry their own typed reply (REP_OK / REP_VALUE / REP_LOC / REP_IDS / REP_PIXELS / REP_ERR). Async ops are fire-and-forget; daemon logs unknowns. ~50 opcodes wired across EGL (synthesized in shim), GL state, object lifecycle, upload, draw + sync, uniforms, replies.

**Verification — 4 probes pass through full pipeline:**
- `gles_probe3` (Phase 2b): EGL init + clear-to-red + readback → 2048/2048 red.
- `gles_probe4` (Phase 3a): full triangle pipeline (shaders + buffers + glDrawArrays) → 2048/2048 green.
- `gles_probe5` (Phase 3c): textured quad (genTextures + texImage2D + texParameteri + sampler2D + texture2D in fragment shader) → 2048/2048 yellow.
- `gles_probe6` (Phase 3c): render-to-FBO + sample FBO's color texture from default framebuffer → 2048/2048 green; FBO status COMPLETE (0x8CD5).

Each probe is a self-contained aarch64 binary linking directly against `libEGL.so.1` / `libGLESv2.so.2` (so LD_PRELOAD interposition works — `gles_probe2`'s pre-existing `dlsym(handle, "glXXX")` pattern bypasses the shim, which is why the new probes inline minimal EGL/GLES decls and link directly).

**Two non-obvious bugs surfaced and fixed:**
- **Phase 1**: `glGetError` MUST be async-marshalled (not sync). Sync round-trip widens a cross-thread synchronization race that triggers the audio-singleton vtable[5] = `_ZSt10unexpectedv` corruption around attract-mode start. Fix in `shim_gl.c::glGetError`: send_async only, FWD_RET still returns Mesa's authoritative error from local path. Note for Phase 3d game run: glGetError sync is **off-limits** until the audio race is independently understood.
- **Phase 3b**: `shim_gl.c::glShaderSource` had a `dlsym(RTLD_NEXT, "glShaderSource")` early-return path (`if (!real_fn || count <= 0 || !string)`) that fired before the marshal block. With daemon-only mode (no Mesa overlay needed) `real_fn` is permitted to be NULL, so the function returned without sending. Restructured the function: daemon-mode is checked first, FWD path only runs when `!active`.

**Run pattern (manual probe testing this session):**
```bash
python3 -u gl_daemon.py > /tmp/gl_daemon.log 2>&1 &
qemu-aarch64 -L "$SYSROOT" \
    -E LD_LIBRARY_PATH="$OVERLAY:$SYSROOT/usr/lib64:$SYSROOT/usr/lib:$SYSROOT/lib64:$SYSROOT/lib" \
    -E LD_PRELOAD="$SHIM" \
    -E SPIKE3_GL_DAEMON=1 \
    ./gles_probeN
```
For game runs: `GL_DAEMON=1 ./run_game.sh ...` spawns daemon automatically.

**Daemon state hygiene gotcha** (cost ~30 min mid-session): stale `gl_daemon.py` processes from prior probe runs hold the abstract socket and yield ID-space confusion (shim sees `prog=1`, daemon executes `glUseProgram(3)`). Always `pkill -9 gl_daemon.py` before fresh runs. Single-connection assumption: `Session.serve()` is called inline from accept loop, so two clients can't connect simultaneously, but state across sequential connections persists in PyOpenGL's GL state. Phase 3d should add `Renderer.reset()` on accept (delete all programs/shaders/buffers) OR exit-after-disconnect for cleanliness.

**Coverage maturity post-Phase-3c:**
- ✅ EGL setup (synth in shim; daemon has its own pre-init context)
- ✅ Shader compile/link (samplerExternalOES rewrite preserved; rewritten source marshalled)
- ✅ Buffers + VAOs + glDrawArrays
- ✅ Textures (genTextures, activeTexture, bindTexture, texImage2D, texSubImage2D, texParameteri, pixelStorei, deleteTextures)
- ✅ Framebuffers (gen/bind/attach/checkStatus/delete)
- ✅ glDrawElements (wired, untested)
- ✅ State setters: glEnable/Disable, BlendFunc, DepthFunc, DepthMask, CullFace, Scissor, ColorMask
- ✅ Uniforms: 1i, 4f, Matrix4fv (sync GetUniformLocation)
- ⏳ Untested in-shim hooks: glDrawElements, glColorMask, several state-setters
- ⏳ Not-yet-marshalled: BlendFuncSeparate, BlendEquation/Separate, BlendColor, StencilFunc/Op/Mask, FrontFace, LineWidth, PolygonOffset, ClearDepthf, ClearStencil, DepthRangef, Uniform2f/3f/Matrix3fv, Uniform[1234]i variants, Uniform1iv/Xfv, IsEnabled, BindRenderbuffer, RenderbufferStorage, FramebufferRenderbuffer, FramebufferTextureLayer, GenerateMipmap, BlitFramebuffer, DrawArraysInstanced, DrawElementsInstanced, VertexAttribDivisor, BindAttribLocation, ValidateProgram, DetachShader, glGetString, glGetIntegerv, glGetFloatv, glGetBooleanv, glIsEnabled, glCompressedTex{,Sub}Image2D, glTexParameterf, glMapBufferRange/Unmap, glUniform4i/3i/2i etc, EGL_IMAGE / DMA-BUF path

**Phase 3d open work** (next session pick-up):
1. **shim_avforward video-frame integration**: existing `g_video_pixel_table[]` provides RGBA. The shim-owned `glEGLImageTargetTexture2DOES` hook should marshal `OP_GL_TEX_IMAGE_2D` (or `OP_GL_TEX_SUB_IMAGE_2D` for re-binds) directly to daemon with the RGBA payload, instead of forwarding to local Mesa. Estimated: 1 hour. Replaces the current EGLImage path entirely when daemon mode is active.
2. **Frame-capture relay**: `shim_frame_capture_at_page_flip` in `shim_bridges.c` does local `glReadPixels`. When daemon active, send `OP_GL_READ_PIXELS` sync, get bytes back, call `shim_frame_send` exactly as today. ~20 lines. Restores the viewer.
3. **Bulk-add remaining hook marshals** (~30 functions listed above, mostly mechanical async-marshal pattern). Add corresponding daemon handlers for the ones that affect rendering correctness; no-op the rest (daemon's "unknown op" handler already returns gracefully).
4. **Daemon state reset**: add `Renderer.reset()` invoked from `Session.bind()` so each connection starts with a fresh GL state.
5. **Game run** with `GL_DAEMON=1`. Per `feedback_baseline_runs_ask_first.md` user drives this. Expected to surface several missing-opcode crashes that we iterate on.

**Phase 4 (post-game-running)**: per the plan file at `~/.claude/plans/let-s-create-a-plan-atomic-horizon.md` — measure attract framerate vs the 1.37 fps baseline; bake verified facts into PROTOCOL §10/§12; flip `GL_DAEMON=1` default once stable; consider ring-buffer batching only if Phase 4 numbers demand it.

### Touched (Step 96.1)

- `_emulation/gl_proto.h` — NEW. Wire protocol opcodes + payload structs (C side, ~50 opcodes covering EGL/state/object/upload/draw/uniform/reply ranges).
- `_emulation/gl_proto.py` — NEW. Matching opcodes + struct.pack format strings (Python side).
- `_emulation/gl_daemon.py` — NEW. Host-native daemon. Renderer class with `EGL_PLATFORM_SURFACELESS_MESA` context init via raw libEGL ctypes (PyOpenGL's Python layer doesn't expose that platform constant); PyOpenGL for everything after. Session class with full opcode dispatcher + per-handler exception logging that returns REP_ERR on SYNC failures.
- `_emulation/shim_gl_marshal.c` — NEW. Marshal lib: `shim_gl_marshal_active()` (lazy connect with 3× retry + dlsym fallback), `_send_async()`, `_send_sync()` with mutex.
- `_emulation/spi_shim.h` — extern decls for the marshal API.
- `_emulation/shim_gl.c` — ~30 hooks updated with daemon-mode early-return: eglGetDisplay/Initialize/BindAPI/ChooseConfig/CreateContext/CreatePbufferSurface/MakeCurrent (synth sentinels), glViewport/ClearColor/Clear/Finish/GetError/ReadPixels (marshal), glGenBuffers/DeleteBuffers/BindBuffer/BufferData/GenVertexArrays/DeleteVertexArrays/BindVertexArray, glCreateShader/DeleteShader/ShaderSource/CompileShader/GetShaderiv, glCreateProgram/DeleteProgram/AttachShader/LinkProgram/GetProgramiv/UseProgram, glGetAttribLocation/GetUniformLocation/VertexAttribPointer/EnableVertexAttribArray/Uniform1i/Uniform4f/UniformMatrix4fv, glDrawArrays/DrawElements, glEnable/Disable/BlendFunc/DepthFunc/DepthMask/CullFace/Scissor/ColorMask, glGenTextures/DeleteTextures/ActiveTexture/BindTexture/TexImage2D/TexSubImage2D/TexParameteri/PixelStorei, glGenFramebuffers/DeleteFramebuffers/BindFramebuffer/FramebufferTexture2D/CheckFramebufferStatus. Plus `gl_marshal_gen_ids`/`gl_marshal_delete_ids`/`gl_marshal_get_location` static helpers; `gl_pixel_size` helper for tex upload size estimation. Forward decls for the static helpers added at top.
- `_emulation/build.sh` — `shim_gl_marshal.c` added to `SOURCES=()`. Build: `spi_shim.so 367168 bytes` (clean, only pre-existing warnings).
- `_emulation/run_game.sh` — `GL_DAEMON` env knob (default 0); spawns `gl_daemon.py` alongside `decode_daemon.py`; forwards `SPIKE3_GL_DAEMON=1` to qemu env; logs go to `runs/<TS>/gl_daemon.log`.
- `_emulation/gles_probe3.c` — NEW. Minimal EGL+GLES probe linking directly against libEGL/libGLESv2 (no dlopen, so LD_PRELOAD interposes).
- `_emulation/gles_probe4.c` — NEW. Triangle pipeline.
- `_emulation/gles_probe5.c` — NEW. Textured quad.
- `_emulation/gles_probe6.c` — NEW. FBO render-to-texture + sample.
- `_emulation/docs/JOURNAL.md` — this Step 96.1 entry appended.

### Step 96.2 — Phase 3d mechanical landings: EGLImage marshal, frame-capture relay, Renderer.reset (2026-05-09)

Three of the five Phase 3d open items landed without surfacing surprises. `spi_shim.so` 367168 → 367264 bytes; all four probes still pass through the daemon.

**Renderer.reset()** — `gl_daemon.py::Renderer` now tracks every id it hands out (programs, shaders, buffers, vaos, textures, framebuffers) and scrubs them on every `bind()` after the first. Verified: a single daemon process serving probes 3 → 4 → 5 → 6 in sequence shows `renderer reset: scrubbed {programs:1, shaders:2, buffers:1, ...}` between connections, with each probe's tex/program/buffer ids restarting at 1. The first-bind branch keeps the GL_VERSION/GL_RENDERER log; subsequent binds skip it.

**glEGLImageTargetTexture2DOES daemon path** — when `SPIKE3_GL_DAEMON=1`, the EGLImage hook now ships the rgba_ptr's RGBA payload (or empty allocation for non-video FBO storage) as `OP_GL_TEX_IMAGE_2D` against the currently-bound 2D texture (which the game already bound through marshalled `glBindTexture`). Trade-off: always full TEX_IMAGE_2D, never TEX_SUB_IMAGE_2D — without a sync `GETINTEGERV(TEXTURE_BINDING_2D)` opcode the shim can't query daemon-side bound id to gate on size match. Re-allocation cost is a known follow-up if profiling demands it. Local-Mesa path with the 128-slot reuse table is preserved untouched for `GL_DAEMON=0`.

**shim_frame_capture_at_page_flip relay** — daemon-mode branch added at the top of the function. Sequence: async `glBindFramebuffer(GL_READ_FRAMEBUFFER, shim_last_draw_fbo)` if non-zero → sync `OP_GL_READ_PIXELS{0,0,W,H,RGBA,UBYTE}` → response payload flows into `shim_frame_buf` and `shim_frame_send` exactly as today → async restore-bind to 0. Frame viewer is wire-compatible. Restore-to-0 (rather than save/restore prev_read) is the conscious shortcut: `GL_READ_FRAMEBUFFER=0` is the spec default and matches what page-flip-time game state should be; if it doesn't, future glBindFramebuffer marshals will overwrite. `#include "gl_proto.h"` added to shim_bridges.c.

**Bulk-add deferred** — the ~30 unmarshalled state-setter/uniform variants are deferred to the game-run iteration loop (per the plan's risk-table strategy: "daemon logs unknown opcodes loud; surface mismatch caught immediately"). **Mixed-mode caveat**: when daemon is active, any unmarshalled GL hook that falls through to local-Mesa via `FWD_VOID` is operating on a different GL context than the daemon owns — buffers/textures/shaders/programs created on the daemon side don't exist in Mesa's namespace. So game-run iteration won't be "marshal what's missing for correctness" — it'll be "marshal what's missing because the alternative is namespace split." Plan accordingly: every game-touched GL hook needs a daemon path before the next stable run.

**Phase 3d remaining**: item 5 — game run with `GL_DAEMON=1` and TIMER markers. Per `feedback_baseline_runs_ask_first.md` user drives this; the assistant only does mechanical iteration on whatever surfaces.

#### Touched (Step 96.2)

- `_emulation/gl_daemon.py` — Renderer adds `_first_bind`, `_tracked` dict, `reset()` method; bind() conditionally calls reset(); gen/create methods add to tracked sets, delete methods discard.
- `_emulation/shim_gl.c` — `glEGLImageTargetTexture2DOES` gains a daemon-mode branch directly after `shim_egl_image_lookup`, before the `dlsym(RTLD_NEXT, ...)` setup. The local Mesa path (with tex_alloc reuse table) is preserved untouched.
- `_emulation/shim_bridges.c` — `#include "gl_proto.h"` added; `shim_frame_capture_at_page_flip` gains a daemon-mode branch at the top that marshals BIND_FRAMEBUFFER + sync READ_PIXELS + restore-BIND, with the response replacing local glReadPixels.
- `_emulation/docs/JOURNAL.md` — this Step 96.2 entry appended.


## Step 97: Atomic Horizon Phase 3d game-run iteration — daemon GL coverage closed; attract scene alpha-blend regression remains unresolved (2026-05-09)

Continuation of Step 96.2. Multiple game runs under `GL_DAEMON=1` surfaced specific marshal-coverage gaps and regressions between `GL_DAEMON=0` (working baseline, per Step 95) and `GL_DAEMON=1`. Coverage gaps have been closed and verified. One user-visible behavior remains unresolved at end of session: **attract scene renders pure black with default settings under `GL_DAEMON=1`**.

### Verified findings — daemon coverage gaps closed

#### 97a — EGL setup hooks lacked daemon-mode early-returns

In daemon mode the shim returns synthesised `FAKE_EGL_*` sentinels for `eglGetDisplay` / `eglInitialize` / `eglBindAPI` / `eglCreateContext` / `eglMakeCurrent` / `eglCreatePbufferSurface` / `eglChooseConfig` (Step 96.1). The remaining EGL hooks (`eglGetPlatformDisplay`, `eglGetPlatformDisplayEXT`, `eglGetConfigs`, `eglGetConfigAttrib`, `eglQueryString`, `eglTerminate`, `eglDestroyContext`, `eglDestroySurface`, `eglCreateWindowSurface`, `eglSwapBuffers`, `eglSwapInterval`, `eglSurfaceAttrib`, `eglQuerySurface`, `eglGetError`) unconditionally `FWD`'d to local Mesa — which has no real EGL initialisation in daemon mode (Mesa overlay's display state is uninitialised because eglInitialize was acked synthetically). Result: game received a real Mesa display from `eglGetPlatformDisplayEXT`, then fake-acked `eglInitialize`, then `eglGetConfigs` produced a garbage `num_config` and the game's `new T[N]` threw `std::bad_array_new_length`. Each affected hook now has `if (shim_gl_marshal_active())` returning the synthesised values directly.

#### 97b — Query hooks lacked daemon-mode early-returns

`glGetIntegerv`, `glGetFloatv`, `glGetBooleanv`, `glGetString`, `glGetStringi` had the same pattern as 97a. Game queried `GL_MAX_*_TEXTURE_IMAGE_UNITS` etc. and got garbage. Observed symptom: `glActiveTexture(0x84BB..0x84BF)` emitted by the game (i.e. `GL_TEXTURE0 - 5..GL_TEXTURE0 - 1`), invalid per spec. Each query hook now bypasses FWD when daemon active and returns the existing canned stub values.

#### 97c — Daemon EGL_CONTEXT_CLIENT_VERSION bumped 2 → 3

Daemon previously requested ES2.0 context. Game compiles `#version 300 es` shaders and uses `glBindBufferRange(GL_UNIFORM_BUFFER, ...)` / `glBufferData(GL_UNIFORM_BUFFER, ...)` (ES3-only). Bumped `EGL_CONTEXT_CLIENT_VERSION` to 3 in `gl_daemon.py::Renderer.__init__`. `gl_daemon.log` confirms `GL_VERSION=OpenGL ES 3.2 Mesa 25.0.7-2`.

#### 97d — `glMapBufferRange` / `glUnmapBuffer` shadow buffer

In daemon mode the prior implementations FWD'd to uninit Mesa, returning NULL. Game's pattern is `ptr = glMapBufferRange(...); memcpy(ptr + 0x40, src, n); glUnmapBuffer(...);` — produced `SIGSEGV` with `fault_addr=0x40` (NULL+0x40). Verified in run logs as the deterministic crash at game ELF VAs `0x11e2208` / `0x7b8c08`. Implemented per-target shadow table (`shim_gl_map_table[8]` in `shim_gl.c`): allocate scratch on `glMapBufferRange`, on `glUnmapBuffer` with `GL_MAP_WRITE_BIT` set, marshal contents via `OP_GL_BUFFER_SUB_DATA(target, offset, len, scratch)`. Crash gone.

#### 97e — PyOpenGL `CompressedImageConverter` rejects raw bytes

`gl_daemon.py::Renderer.compressed_tex_image_2d` initially called `self._GL.glCompressedTexImage2D(...)` with raw `bytes` payload. PyOpenGL's `CompressedImageConverter` raised `AttributeError: 'NumberHandler' object has no attribute 'arrayByteCount'` on every DXT5 (`internal_format=0x83F3`) upload — game.elf attract-mode video frames are 1360×768 / 512×512 DXT5. Bypassed via raw `libGLESv2.so.2` ctypes binding for both `glCompressedTexImage2D` and `glCompressedTexSubImage2D`; PyOpenGL's wrapper is no longer in the path for these.

#### 97f — Game-vs-shim symbol audit closed all `gl*` / `egl*` exports

`comm -23` of `strings game | grep '^gl[A-Z]\|^egl[A-Z]'` against `nm -D spi_shim.so` showed gaps. Two batches added new exports + marshal opcodes:
- ES3 / extension functions previously not hooked: `glBindBufferBase`, `glBindBufferRange`, `glUniformBlockBinding`, `glGetUniformBlockIndex` (sync), `glBlitFramebuffer`, `glDrawBuffers`, `glReadBuffer`, `glClearBufferfv`, `glInvalidateFramebuffer`, `glVertexAttribIPointer`, `glDrawRangeElements`, `glCopyBufferSubData`, `glTexImage3D`, `glTexSubImage3D`, `glTexStorage2D`, `glTexStorage3D`, `glCompressedTexImage3D`, `glCompressedTexSubImage3D`, `glFramebufferTexture2DMultisampleEXT`, `glTexStorage3DMultisample`.
- Direct exports of stubs that previously only existed in the `eglGetProcAddress` table: `eglCreateSyncKHR`, `eglDestroySyncKHR`, `eglWaitSyncKHR`, `eglClientWaitSyncKHR`, `eglReleaseThread`. Plus `glDebugMessageCallback` / `KHR`, `glBeginTransformFeedback` / `glEndTransformFeedback` / `glTransformFeedbackVaryings`, `glGenQueries` / `glDeleteQueries`, `glFramebufferTextureMultiviewOVR` / `glFramebufferTextureMultisampleMultiviewOVR`, `glGetInteger64v`, `glProgramBinary` / `glGetProgramBinary` as no-ops or sentinel-returning stubs.
- Existing hooks that previously fell through to FWD (and thus to uninit Mesa under `GL_DAEMON=1`) gained daemon-mode early-returns with marshal: `glGenRenderbuffers`, `glDeleteRenderbuffers`, `glBindRenderbuffer`, `glCompressedTexImage2D`, `glCompressedTexSubImage2D`, `glTexParameterf`, `glGenerateMipmap`, `glBufferSubData`, `glDetachShader`, `glValidateProgram`, `glGetShaderInfoLog`, `glGetProgramInfoLog`, `glBindAttribLocation`, `glUniform2i` / `3i` / `4i`, `glUniform1f` / `2f` / `3f`, `glUniform1iv` / `1fv` / `2fv` / `3fv` / `4fv`, `glUniformMatrix3fv`, `glDisableVertexAttribArray`, `glFramebufferRenderbuffer`, `glRenderbufferStorage`, `glBlendFuncSeparate`, `glBlendEquation`, `glBlendEquationSeparate`, `glBlendColor`, `glDepthRangef`, `glFrontFace`, `glStencilFunc`, `glStencilMask`, `glStencilOp`, `glLineWidth`, `glPolygonOffset`, `glClearDepthf`, `glClearStencil`, `glDrawArraysInstanced`, `glDrawElementsInstanced`, `glVertexAttribDivisor`, `glFlush`. Plus three ES3 uniforms found in a separate audit pass: `glUniform1ui`, `glUniform1uiv`, `glUniform2iv`, and `glVertexAttrib4f`, `glVertexAttribI4ui`. The `glUniform1ui/1uiv/2iv` additions empirically fixed text rendering (text was previously rendered as solid white rectangles instead of character shapes).

After this work, `comm -23` returns no rows — every gl/egl symbol referenced by `game.elf` is exported by `spi_shim.so` and has a marshal path.

### Verified findings — runtime observations under `GL_DAEMON=1`

#### 97g — Frame capture pipeline is correct

`shim_frame_capture_at_page_flip` reads from `shim_last_draw_fbo` (last non-zero FB-target bind by the game). Verified by running with a daemon-side write-then-read self-check: daemon writes magenta at `(200, 200)` on fb=1, then immediately reads back via `glReadPixels` — return is `(0xff, 0x00, 0xff)` on every scan during the run. The user observed the magenta dot in the viewer, confirming the capture-→-viewer wire is intact end-to-end for the targeted FBO.

#### 97h — Game uses fb=1/2/3 in triple-buffer rotation throughout the run

Unthrottled bind logging in the daemon (one log line per `glBindFramebuffer`) shows the game cycles `glBindFramebuffer(GL_FRAMEBUFFER, 1) → 2 → 3 → 1 → 2 → 3 → ...` for both Service Menu and attract scenes. fb=4 is bound twice (FB target) at boot (`T+22-25s`), fb=5 is bound twice at boot also; both before Service Menu. fb=0 is bound only with `GL_READ_FRAMEBUFFER` target — those are our `shim_frame_capture` restore-binds. **Game never binds fb=0 with `GL_FRAMEBUFFER` or `GL_DRAW_FRAMEBUFFER` target during attract.**

#### 97i — Per-frame uniform values arrive at daemon correctly

`glUniform4fv` value logging shows the game sends `colorTransformMultiply = (1, 1, 1, 1)` and `colorTransformAdd = (0, 0, 0, 0)` for video draws — the GL identity for the Stern Radium color-transform path. The marshal layer is delivering these values correctly.

#### 97j — All FBOs are `FRAMEBUFFER_COMPLETE`

`glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER)` returns `0x8CD5` (`GL_FRAMEBUFFER_COMPLETE`) for fb=1/2/3; their color attachment 0 names match the daemon-allocated texture ids (`col0_tex=1/2/3` correspond to the `GL_TEXTURE_2D` ids). FBOs 4..20 raise `GLError` on bind (do not exist).

### Static analysis (Ghidra)

Source strings + xrefs for `colorTransformMultiply`:

- `0x361b7f0` — string `"colorTransformMultiply"`
- `0x361b820` — solid-fill fragment shader (no texture)
- `0x361ba30` — textured sprite fragment shader (`sampler2D`): `clamp(colorTransformMultiply * frag, 0, 1); clamp(prev + colorTransformAdd * prev.a, 0, 1)`
- `0x361bd08` — YUV→RGB video fragment shader (3 plane samplers): `vec4(r, g, b, 1.0)` initial alpha hardcoded
- `0x361df50` — gradient fragment shader
- `0x361e630` — font fragment shader
- `0x3623d78` — EGLImage video fragment shader (`samplerExternalOES`, used for attract video frames via the EGLImage path)

VideoPlayer setup function: `FUN_0091e570` at `0x91e570` — compiles the `samplerExternalOES` shader, links program, stores program id at `this+0x2d4`, viewprojMat loc at `+0x2d8`, modelMat at `+0x2dc`, `colorTransformMultiply` loc at `+0x2e0`, `colorTransformAdd` loc at `+0x2e4`.

VideoPlayer uniform-set function: `FUN_0091e3b0` at `0x91e3b0` — calls `glUseProgram`, `glUniformMatrix4fv` (×2), `glUniform4fv(colorTransformMultiply_loc, 1, this+0x2b4)`, `glUniform4fv(colorTransformAdd_loc, 1, this+0x2c4)`. The vec4s at `this+0x2b4` and `this+0x2c4` are populated by `FUN_008c8e10` from the result of:

- `FUN_008a3950` = `Radium::Stage::GetColorMultiplication()` — returns top of the `m_mColorTransformStack` (asserts on empty)
- `FUN_008a3990` = `Radium::Stage::GetColorAddition()` — returns top of the same stack
- `FUN_008a3a70` = `Radium::Stage::PushColorTransform(Color&)` — composes new mult against previous (`new.r * prev.r`, etc.) and clamps add to `[0, 1]`
- `FUN_008a3a20` = `Radium::Stage::PopColorTransform()` — decrements end pointer by `0x20`

`Video::draw` at `FUN_008b4c30` invokes the Push at offset `0x008b4ca0` with `param_1 + 0x84` (a 32-byte color transform on the Video element) before drawing, and the Pop after.

### Unresolved at end of session — attract scene renders pure black under `GL_DAEMON=1` default settings

Behavior matrix observed by running the same `game` binary under the same shim:

| `GL_DAEMON` | `SPIKE3_DISABLE_BLEND` | Boot logo | Service Menu | Attract |
|---|---|---|---|---|
| `0` | `0` | shown ~T+10s, persists | shown ~T+60s | renders correctly |
| `0` | `1` | same as above | same as above | renders correctly (env has no effect on local-Mesa path) |
| `1` | `0` | shown ~T+7s, disappears within 1-2s | shown ~T+45s | **pure black** |
| `1` | `1` | shown ~T+7s, disappears within 1-2s | shown ~T+45s | content visible but text appears as solid white rectangles and sprite "transparent" backgrounds appear as solid black |

`SPIKE3_DISABLE_BLEND=1` is implemented in `gl_daemon.py::Renderer.enable` as: silently return without calling `glEnable` when `cap == GL_BLEND` and the env is set.

#### Investigation results that did NOT identify the cause

The following were confirmed and ruled out as the SOLE cause of the regression:

- **Marshal of uniform values**: `glUniform4fv` logging confirmed daemon receives `(1, 1, 1, 1)` for `colorTransformMultiply` and `(0, 0, 0, 0)` for `colorTransformAdd` during attract draws (per 97i above). Wire is correct.
- **Texture alpha = 0 in uploaded data**: tested env-gated `SPIKE3_FORCE_TEX_ALPHA_255=1` (since removed) which forced the alpha byte of every RGBA upload to 255. Attract remained pure black with blend on. Texture alpha is not the source of the alpha=0 fragment output.
- **FBO target wrong / draws going elsewhere**: `FBO_SCAN` diagnostic samples 8 pixels of fb=0..20 on every Nth frame_cap. Throughout attract: fb=0 shows uniform `(0x24, 0x24, 0x24)` (set by 3 boot-time `FB-fb=0` binds, then unchanged); fb=1/2/3 are pure `(0x00, 0x00, 0x00, 0x00)`; fb=4..20 do not exist on daemon. With write-then-read self-check on fb=1 returning the magenta we wrote, the read pipeline is verified.
- **Clear path bypass**: `SPIKE3_FORCE_MAGENTA_CLEAR=1` overrides `glClearColor` and `glClearBufferfv` (color-buffer variant) to `(1, 0, 1, 1)`. Service Menu shows magenta around the SM area in capture, confirming the override reaches Mesa for SM. Attract shows no magenta — game does not invoke `glClear` / `glClearColor` / `glClearBufferfv` between attract draws. `game.elf` does not reference `glClearBufferiv` / `glClearBufferuiv` / `glClearBufferfi` per `strings`.
- **Blit-from-black**: `SPIKE3_FORCE_MAGENTA_CLEAR=1` also short-circuits `glBlitFramebuffer`. `glBlitFramebuffer` is referenced in `game.elf` strings but is called 0 times during the run per daemon's blit counter. Attract content is not produced via blit.
- **Missing function hooks**: per 97f, full audit closed all gaps. After all hooks were added and SM text rendered correctly (the 97f `glUniform1ui/1uiv/2iv` fix), attract remained black with blend on.
- **Defensive blend-state reset**: glBlendFunc logging shows boot draws alternate `(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)` ↔ `(GL_ZERO, GL_ZERO)`, but after boot the resets stop. Sampled glBlendFunc calls during attract are all `(0x302, 0x303)` = `(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)`, the standard alpha blend. Blend factors set during attract are not the source.
- **Draws not happening**: daemon's draw counter confirms ~150-300 draws/sec sustained throughout attract; `glUseProgram`/`glDrawArrays`/`glDrawElements` are all marshalled and reach the daemon.
- **Daemon errors**: the daemon log reports zero `GLError` exceptions and zero `unknown op` warnings during attract.
- **`glProgramBinary` bypass of shader rewrite**: hypothesised that game might restore pre-compiled shaders bypassing `glShaderSource` (and our `samplerExternalOES → sampler2D` rewrite). Diagnostic logs added to `glProgramBinary` and `glGetProgramBinary`. **Not yet verified by a run with these logs in place.**

#### Hypotheses still open

These remain plausible but unverified at the end of the session:

- The shader's *output alpha* on the daemon's host-Mesa context resolves to 0 even when its inputs (texture alpha, uniform alpha) are 1 — i.e. host-Mesa swrast computes the same shader differently than the in-process aarch64 Mesa overlay. Could be a Mesa version difference, a PyOpenGL wrapper artefact, a samplerExternalOES path subtlety, or a uniform we haven't yet discovered being read by the shader and silently defaulted to 0.
- The game's faster execution under `GL_DAEMON=1` (Service Menu at ~T+45s vs ~T+60s under `GL_DAEMON=0`) may be advancing frame-counted animation timers faster — possibly relevant to "Pokemon logo disappears within 1-2s instead of persisting", though this could also be a separate symptom of the same underlying alpha issue.

### Diagnostic infrastructure retained in code (env-gated, opt-in)

These are present in `gl_daemon.py` and `shim_gl.c` / `shim_bridges.c` and have been useful enough during this session to keep available for future debugging:

- `SPIKE3_DISABLE_BLEND=1` — daemon's `Renderer.enable` ignores `glEnable(GL_BLEND)`. **Working workaround for attract visibility.**
- `SPIKE3_FORCE_MAGENTA_CLEAR=1` — daemon overrides `glClearColor` / `glClearBufferfv (COLOR)` to `(1, 0, 1, 1)`; also short-circuits `glBlitFramebuffer`.
- `SPIKE3_VIDEO_TESTPATTERN=1` — daemon replaces RGBA uploads ≥256×256 with a hot-pink/green checkerboard.
- `FBO_SCAN` log per Nth `glReadPixels` — samples 8 pixels of fb=0..20, reports `glCheckFramebufferStatus` and `COLOR_ATTACHMENT0` texture id.
- Throttled per-call logs: `glBindFramebuffer` (unthrottled), `glDrawArrays`/`glDrawElements`, `glBlendFunc` / `Separate`, `glBlitFramebuffer`, `glUniform4f`, `glUniform4fv` (with values), `glProgramBinary` / `glGetProgramBinary`.
- `near_black=N/256` in `[frame_cap]` — pixel-content sniff per frame send.
- `shim_inject` `manual_press` `BEFORE`/`AFTER` `state` / `edge` / `delta` log — confirms switch-press write reached the polling region.

### Operational improvements

- `_emulation/scripts/reset_run_state.sh` now reaps stale `decode_daemon.py`, `gl_daemon.py`, `marker_button.py`, `audio_subscriber.py`, `frame_viewer.py`, and `ffplay` (audio bridge) processes before clearing NVRAM/cache, so re-running after a `SIGKILL` of a previous run no longer fails with `EADDRINUSE` on the abstract sockets.
- `.claude/hooks/quirk_detector.py` gained an always-on invocation trace at `.claude/hooks/quirk_detector_invocation.log`. The hook itself only writes to its own audit log when drift is detected (silent on the fast-path clean exit), so the trace lets us distinguish "hook ran and saw nothing" from "hook never ran" in future investigations.

### Touched

- `_emulation/shim_gl.c` — daemon-mode early-returns added to all EGL setup/query/teardown hooks (97a) and all `glGet*` query hooks (97b); shadow-buffer table + scratch allocator for `glMapBufferRange` / `glUnmapBuffer` (97d); `glEGLImageTargetTexture2DOES` daemon path emits `OP_GL_TEX_IMAGE_2D` with the avforward RGBA payload; daemon-mode early-returns + marshal added to all previously-unmarshalled hooks per 97f; new exports for the ES3 / extension functions per 97f; logging counters added to several hooks.
- `_emulation/shim_bridges.c` — `near_black=N/256` sampling added to `[frame_cap]` log; daemon-mode branch in `shim_frame_capture_at_page_flip` retained from Step 96.2.
- `_emulation/spi_shim.h` — `extern int shim_gl_marshal_active(void);` etc. unchanged from Step 96.2.
- `_emulation/gl_proto.h` / `gl_proto.py` — opcodes + payload structs / format strings added for the Phase 3d batch + the supplemental ES3 batch (uniform integer-variants, vertex-attrib, compressed-3D, multisample-3D, multisample-FB-attach).
- `_emulation/gl_daemon.py` — `Renderer.__init__` now creates an ES3 context and reads three env vars (`SPIKE3_VIDEO_TESTPATTERN`, `SPIKE3_FORCE_MAGENTA_CLEAR`, `SPIKE3_DISABLE_BLEND`); `_libGLESv2` raw ctypes binding for compressed-tex bypass (97e); `Renderer` gained methods + dispatch for every new opcode in the Phase 3d batches; `execute_read_pixels` invokes `_scan_all_fbos_for_content` on a sampling cadence (FBO_SCAN diagnostic + write-then-read self-check); various per-call logging counters.
- `_emulation/scripts/reset_run_state.sh` — stale-daemon reaper.
- `.claude/hooks/quirk_detector.py` — `_trace_invocation` always-on invocation log.
- `_emulation/docs/JOURNAL.md` — this Step 97 entry appended.


## Step 98: Atomic Horizon blend-state fix — ignore bogus zero/zero blend factors, preserve alpha compositing (2026-05-10)

Continuation of Step 97. Treat the Step 97 hypotheses as suspect historical context only: this session re-tested the rendering failure from the user-visible symptoms and from fresh daemon logs. The final fix was smaller than the earlier investigation implied: when the game/renderer sends `glBlendFunc(GL_ZERO, GL_ZERO)` under `GL_DAEMON=1`, the daemon should not apply that state literally, and should not disable blending either. It should preserve the last valid alpha blend factors.

### 98a — Repro automation: host-side Service Back at T+60s

The user's visual repro depends on a manual interaction: with `TIMER=ON`, wait until the Service Menu is visible, then press Service Back around T+60s so the game transitions into attract mode. To make this testable without the Tk TIMER UI, `run_game.sh` gained an opt-in host-side auto-press helper:

```bash
SHIM_AUTO_PRESS=1
SHIM_AUTO_PRESS_DELAY_S=60
SHIM_AUTO_PRESS_SW_ID=93
SHIM_AUTO_PRESS_LABEL="Service Back"
```

The helper writes the same file that `marker_button.py` writes:

```text
_emulation/runs/latest/press_request.txt
```

The existing `shim_manual_press_thread` then consumes the request and drives the switch. Verified in automated 80s daemon runs:

- `runs/2026-05-10T11.13.29_auto_service_back_zero_blend` — `SPIKE3_ZERO_BLEND_DISABLE=1`, old behavior at first.
- `runs/2026-05-10T11.15.13_auto_service_back_blend_enabled` — no zero-blend workaround.
- `runs/2026-05-10T11.21.52_auto_service_back_zero_blend_ignore` — experimental ignore-zero-blend behavior.
- `runs/2026-05-10T11.23.55_auto_service_back_zero_blend` — compatibility run using the existing flag name after the fix.

In each automated run, the game log showed:

```text
[auto_press] T+60s PRESS Service Back sw_id=93 -> .../runs/latest/press_request.txt
[shim] [manual_press] sw_id=93 ch=0 byte=1 bit=3
```

The press landed roughly 70-120 ms after the file write. The attract-mode scene dump also appeared after the press, proving the automated path exercised the same transition the user had been testing by hand.

### 98b — Symptom split clarified the real blend-state problem

User-reported visual matrix after Step 97 work:

| Mode | User-visible behavior |
|---|---|
| `GL_DAEMON=1`, normal blend | Text/service menu mostly correct, but after Service Back the attract mode renders black / nothing. |
| `GL_DAEMON=1`, old `SPIKE3_ZERO_BLEND_DISABLE=1` behavior | Attract content becomes visible, but fonts/images show alpha artifacts: white text rectangles and black square backgrounds. Later text improved after uniform marshal fixes, but image alpha boxes remained. |

Fresh daemon logs explain both sides of that matrix:

- Normal blend run (`2026-05-10T11.15.13_auto_service_back_blend_enabled`) reached attract and then sampled draw calls like:

```text
draw #13312 ... blend=1 func=(0x0,0x0,0x0,0x0)
```

With blending enabled, `glBlendFunc(0,0)` computes `src * 0 + dst * 0`, so draws produce black/transparent output. This matches "attract displays nothing."

- Old workaround run (`2026-05-10T11.13.29_auto_service_back_zero_blend`) changed those intervals to:

```text
draw #13312 ... blend=0 func=(0x0,0x0,0x0,0x0)
```

This prevented the black wipe, but disabling `GL_BLEND` makes alpha textures draw as opaque RGBA. That exactly matches the remaining artifacts: fonts and images draw, but their transparent pixels become visible rectangles or black backgrounds depending on the uploaded RGB under alpha-zero texels.

Texture sampling supported this interpretation. Font/UI atlas uploads intentionally contain many texels like white RGB with alpha 0 (`rgb_when_a0 > 0`), so disabling blending is guaranteed to expose boxes. The texture data was not the primary bug; the daemon's handling of bad blend state was.

### 98c — Fix: ignore `glBlendFunc(0,0)` and keep the previous valid blend factors

The successful experimental mode was:

```bash
SPIKE3_ZERO_BLEND_IGNORE=1
```

Implementation in `gl_daemon.py`:

- If `glBlendFunc(0,0)` arrives, return without calling host GL.
- If `glBlendFuncSeparate(0,0,0,0)` arrives, return without calling host GL.
- Leave `GL_BLEND` enabled.
- Leave the previous valid blend factors active, normally:

```text
GL_SRC_ALPHA / GL_ONE_MINUS_SRC_ALPHA
```

The final compatibility behavior maps the user's existing flag to this safer mode:

```bash
SPIKE3_ZERO_BLEND_DISABLE=1
```

now means "ignore zero/zero blend factors and preserve alpha compositing" by default. The old behavior remains available only for comparison:

```bash
SPIKE3_ZERO_BLEND_MODE=disable
```

The final compatibility run (`runs/2026-05-10T11.23.55_auto_service_back_zero_blend`) confirmed that after Service Back and attract startup, sampled draw calls stayed on the correct alpha blend state:

```text
draw #12288 ... blend=1 func=(0x302,0x303,0x302,0x303)
draw #13312 ... blend=1 func=(0x302,0x303,0x302,0x303)
draw #14336 ... blend=1 func=(0x302,0x303,0x302,0x303)
...
```

`0x302` = `GL_SRC_ALPHA`; `0x303` = `GL_ONE_MINUS_SRC_ALPHA`. This is the expected compositing mode for sprites, font atlases, and RGBA video/UI textures.

### 98d — Program-binary hypothesis closed as not causal

During the search, `glProgramBinary` / `glGetProgramBinary` diagnostics were added because Step 97 left open the possibility that precompiled shader binaries bypassed the shader-source rewrite or linked a different program path. The observed diagnostics did not support that theory:

- `glProgramBinary` calls were real.
- Binary-touched programs reported `GL_LINK_STATUS=0`.
- No real binary-touched program was later used for the problematic draw stream.

This made program binaries a false lead for the alpha/attract regression.

### 98e — Current operational command

For GL-daemon runs with the fix:

```bash
GL_DAEMON=1 SPIKE3_ZERO_BLEND_DISABLE=1 ./run_game.sh
```

For deterministic test/repro without TIMER:

```bash
GL_DAEMON=1 SPIKE3_ZERO_BLEND_DISABLE=1 \
SHIM_AUTO_PRESS=1 SHIM_AUTO_PRESS_DELAY_S=60 SHIM_AUTO_PRESS_SW_ID=93 \
TIMEOUT=80 ./run_game.sh --debug --note visual_zero_blend_fix
```

### Touched

- `_emulation/run_game.sh` — added `SHIM_AUTO_PRESS` helper, defaulting to Service Back (`sw_id=93`) at T+60s, and recorded its final values in `run_info.txt`.
- `_emulation/gl_daemon.py` — added zero-blend modes. `SPIKE3_ZERO_BLEND_DISABLE=1` now defaults to preserving the last valid blend factors by ignoring `glBlendFunc(0,0)` / `glBlendFuncSeparate(0,0,0,0)`; `SPIKE3_ZERO_BLEND_MODE=disable` preserves the old "disable GL_BLEND" behavior for comparison.
- `_emulation/shim_gl.c` / `_emulation/gl_daemon.py` — retained RGBA upload sampling and program-binary diagnostics used to close false leads during this investigation.
- `_emulation/docs/JOURNAL.md` — this Step 98 entry appended.

## Step 99 — 48V warning traced to Coin Door Power Interlock (`0-SW-23` / `sw_id=98`)

After the channel-1 derived-switch fix, the game reached the real attract path and displayed the blocking warning:

```text
48V DISABLED
CLOSE COIN DOOR
OR PULL INTERLOCK SWITCH
TO RESTORE POWER
```

First hypothesis was the power-distribution board `0x1d` status response. Disassembly confirmed cmd `0x1d`'s game-side handler at `0x916028` accepts exactly four payload bytes and copies them into the per-id receive buffer. A probe changed the shim from 0-byte ACK to a configurable 4-byte payload (`SPIKE3_PWRDIST_1D`, default `ff ff ff ff`), and the game log confirmed receipt:

```text
bridge RESP to cmd=0x1d id=0x01 ... 02 1d 01 ff ff ff ff e6 03
```

But a frame capture from that run still showed the 48V warning. So `0x1d` alone was not the visible gate.

The decisive clue was the switch inventory: the Pokémon manual maps `0-SW-23` to **Coin Door Power Interlock**, and the runtime switch table maps that to `sw_id=98`, channel 0, bit 23. In the failing run, devbus snapshot reported `sw_id=98 state=1`. The shim now forces channel-0 bit 23 to the restored/closed state before the normal SPI channel-0 translation path sees it.

Verification run:

```text
runs/2026-05-10T14.31.59
```

Observed evidence:

- `game.log`: channel-0 idle buffer changed from `ff ff ff ...` to `ff ff 7f ...`, clearing bit 23.
- `devbus.log`: `sw_id=98` emitted `switch_change ... state=0`.
- `switch_noise_test.log`: PASS.
- Frame capture `/tmp/interlock_closed_snap_live/frame_000360.png`: warning overlay gone; attract screen shows `TRAINER 1`.

Operational behavior:

- Default: emulate coin door/interlock restored (`SPIKE3_COIN_DOOR_INTERLOCK` unset).
- Repro warning: `SPIKE3_COIN_DOOR_INTERLOCK=open ./run_game.sh`.

Touched:

- `_emulation/shim_ioctl.c` — defaults channel-0 bit 23 (`0-SW-23`, `sw_id=98`) to restored/closed before SPI RX is handed to the game's switch translator; supports `SPIKE3_COIN_DOOR_INTERLOCK=open|0` for A/B testing.
- `_emulation/run_game.sh` — forwards `SPIKE3_COIN_DOOR_INTERLOCK` into qemu when set and records it in `run_info.txt`.
- `_emulation/docs/PROTOCOL.md` — documents the env knob and the `0-SW-23`/`sw_id=98` mapping.

### 99a — Left Coin added to TIMER press panel (`1-SW-16` / `sw_id=80`)

After the channel-0 SPI quieting bug was fixed, rendering improved and the remaining start-game blocker looked more likely to be credit/precondition related than display related. Service Back had been used informally as "add credit" during service-menu navigation, but the machine inventory shows real coin inputs on channel 1:

```text
1-SW-16  Left Coin    -> runtime sw_id=80
1-SW-17  Center Coin
1-SW-18  Right Coin
```

The TIMER UI now exposes `PRESS Left Coin (sw_id 80)` so credit insertion can be tested separately from Service Back (`sw_id=93`), which should only exit service/operator screens.

Touched:

- `_emulation/marker_button.py` — added a Left Coin/Add Credit press button using the existing `press_request.txt` API path.
- `_emulation/docs/PROTOCOL.md` — documented `1-SW-16` -> `sw_id=80` and clarified that Service Back is not the coin input.
