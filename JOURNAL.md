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

**What we found.** Same hash (`7eaae301bb4491652adb38a84c5d7c3cf8a85071ab9630b299c3f50b47724eaa`), same 71,349,568-byte file. The discrepancy was entirely that **Binary Ninja rebases PIE binaries by +0x400000**, so every address Sidekick cites is `file_offset + 0x400000`. My objdump uses natural ELF VAs (which equal file offsets for this binary). Once we subtracted 0x400000 from Sidekick's addresses, bytes matched perfectly at all four spot-checks. Sidekick's analysis had been correct all along.

**Address translation rule for future sessions:** `file_offset = BN_VA − 0x400000`. Keeping a translation table in Step 14.1 below.

### 14.1: Address translation table

| Symbol (Sidekick name) | BN VA | File offset / natural VA |
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

Before landing on the above, we spent a detour verifying Sidekick's addresses. Sidekick claimed specific bytes at `0xd15aa8` / `0xcf7c10` that didn't match what my objdump showed at those file offsets. Hash comparison proved we were both looking at the same binary (`7eaae301...`, 71349568 bytes). The discrepancy was entirely **Binary Ninja rebasing PIE binaries by +0x400000**. Once I subtracted 0x400000 from BN VAs to get file offsets, everything matched.

**Convention going forward:** when talking to Sidekick or using BN's Python console, use BN VAs. When doing our own objdump / shim work, use file offsets. `file_offset = BN_VA − 0x400000`. (Full translation table in Step 14.1.)

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

### Address translation rule (Binary Ninja ↔ our tools)
`file_offset = BN_VA − 0x400000`
Lookup table: Step 14.1.

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
