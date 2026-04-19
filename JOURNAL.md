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

