# Spike 3 Emulation — Verified Protocol Reference

**Purpose.** Everything in this document has been directly observed or proven by test. Hypotheses live in `JOURNAL.md`; this file only gains an entry once a test confirms it. Every entry cites the journal step that established it so you can trace the evidence. If something here turns out to be wrong, delete it — do not leave refuted claims as "maybe still true."

**Source of truth for current work status:** `JOURNAL.md`. This file is for facts; that file is for narrative.

---

## Address translation (Binary Ninja ↔ objdump)

```
file_offset = BN_VA − 0x400000
```

Binary Ninja rebases PIE aarch64 binaries by +0x400000. Sidekick cites addresses as BN VAs; `aarch64-linux-gnu-objdump` uses file offsets (which equal natural VAs for this binary). Always translate before acting on an address.

Verified by: sha256 match of the same binary across both tools, then byte-level spot-checks at four addresses. JOURNAL Step 14.0.

---

## Response dispatch (in `sub_d15aa8`)

When the bridge thread receives a complete frame from the netbridge, it extracts `CMD` and computes `idx = CMD − 0x10`. If `idx u> 0x31`, it falls through to an "unknown command" error. Otherwise it loads a 16-bit signed entry from a jump table at `.rodata` offset `0x37cf510` (50 entries = 0x32 cmds, covering 0x10 through 0x41) and jumps to `0x915e14 + entry * 4`.

Verified by decoding the table directly from the binary in JOURNAL Step 17.2.

**Handler map** (cmds that route to a per-cmd handler; omitted cmds go to generic fallbacks):

| Response CMD | Handler VA (file offset) | Payload requirement | What it does |
|--------------|--------------------------|---------------------|--------------|
| `0x10` | `0x916194` | **exactly 7 bytes** | Pwrdist version. Packs bytes 0..2 into `data_490e61c`, bytes 3..5 into `data_490e620`, byte 6 becomes boolean flag at `+0x74`. |
| `0x12` | `0x916158` | 7 bytes (unverified) | TBD |
| `0x14` | `0x9160d8` | 4 bytes | TBD |
| `0x15` | `0x9160a0` | **exactly 1 byte** | Pwrdist status (?). OR/XORs payload[0] with state at `data_490e5b0 + 128`. |
| `0x18` | `0x91607c` | variable | Records `min(existing, payload_len)` into halfword table at `data_490e5b0 + 0x88`, indexed by ID. Purpose unclear — possibly connection-quality tracker. |
| `0x1a` | `0x916064` | TBD | TBD |
| `0x1b` | `0x916038` | TBD | TBD |
| `0x1d` | `0x916028` | **exactly 4 bytes** | If payload == 4, branches to channel-copy at `0x915e88` (memcpy payload into per-id buffer). If payload != 4, silent-ACK. Semantics unknown; given pwrdist reports door/buttons/rail state, plausibly `[status_bits][rail_bits][adc_hi][adc_lo]` but unverified. |
| `0x20` | `0x915fec` | TBD | HW version. |
| `0x22` | `0x915e54` | TBD | TBD |
| `0x2d` | `0x915f08` | TBD | TBD |
| `0x2e` | `0x915ed8` | TBD | TBD |
| `0x41` | `0x915f88` | TBD | TBD |

**Generic fallbacks:**
- `0x915e14` — generic "response received with data": signals waiting channel + memcpy payload into channel buffer. Hit by cmds `0x27`, `0x28`, `0x40`.
- `0x915d28` — **shared exit / dispatch path**, NOT a pure silent-ACK (Step 19.4):
  - `id == 0` → pure ACK (signal channel, no buffer write).
  - `payload_len > 0` → falls to `0x915e8c`, which is **the same per-id channel-copy as `0x915e14`** (memcpys payload into `data_490e5b0 + 0xa0 + id*8`). This is what lets cmd 0x16's 4-byte response populate the caller's rx_buf even though cmd 0x16 routes here.
  - Else `id > 5` → pure ACK.
  - Else → state-machine based on byte at `data_490e5b0 + 0xe8 + id`.

  Hit by cmds `0x11/0x13/0x16/0x17/0x19/0x1c/0x21/0x23–0x25/0x30/0x31`.
- `0x915d08` — "unknown command" error path. Hit by cmds outside our table.

Verified by Step 17.2 table decode + handler disassembly + Step 19.4 `0x915d28` re-disassembly.

---

## Request → response CMD mapping

Current knowns:

| Request CMD | Response CMD | Response payload | Evidence |
|-------------|--------------|------------------|----------|
| `0x30` (CRC query)           | `0x20` | 4-byte CRC32 LE | Game accepts → `netbridge image okay, CRC32=0x357f5327` (Step 16) |
| `0x10` (pwrdist version)     | `0x10` | 7 bytes (see below) | Dispatch table → `0x916194` pwrdist handler (Step 17). Game prints `power dist: ...` after receipt (Step 18). |
| `0x15` (pwrdist status)      | `0x15` | 1 byte | Generic followup; game advances to cmd 0x18 after receipt (Step 18) |
| `0x18` (pwrdist connection?) | `0x18` | 1 byte | Game advances to cmd 0x1c flash write after receipt (Step 18) |
| `0x1c` (pwrdist flash chunk) | `0x1c` | 0 bytes (silent ACK) | Zero-payload response signals channel via fallback at `0x915d28`; game re-queries cmd 0x10 and proceeds (Step 18) |
| `0x11`                       | `0x11` | 0 bytes (silent ACK) | Request payload on the wire is `[0x02]`. Caller does not read response data; 0-byte ACK unblocks the channel (Step 19.1) |
| `0x1d`                       | `0x1d` | 0 bytes (silent ACK) | Request payload on the wire is `[0x02, 0x30, 0x30]`. Caller does not read response data; 0-byte ACK takes the `cmp w27,#4` else path (Step 19.1) |
| `0x27` (poll / status)       | `0x27` | 1 byte, alternating | See "Cmd 0x27 status byte" below. Four game-side callers (`0x916828/0x916fe0/0x917020/0x917264`) poll different bits of this byte (Step 19.2) |
| `0x16` (pwrdist runtime info)| `0x16` | 4 bytes `01 00 02 00` | Game's wrapper `sub_917b60` + polling caller at `0x675c20` require `rx[0]>0 AND rx[2]==2` within 1000 × 1ms (Step 19.3). Game prints `power dist. boot version / runtime version` after receipt. |

Cmds `0x20` (as request from the game): not yet verified — did not appear in the successful Step 19 run. Our current cmd 0x20 response emits cmd `0x10` with 4 bytes, which would fail the cmd 0x10 handler's 7-byte length check — needs correction if cmd 0x20 ever fires (Step 19.7 note 2).

### Cmd 0x27 status byte

Four game-side callers poll the 1-byte response with different conditions:

| Callsite | Enclosing fn | Condition |
|---|---|---|
| `0x916828` | `sub_9167e8` | one-shot, any value acceptable |
| `0x916fe0` | `sub_916f60` first loop | wait until **bit 2 = 1** |
| `0x917020` | `sub_916f60` second loop | wait until **bit 2 = 0** (paired with above) |
| `0x917264` | standalone | wait until **`(resp & 0x03) == 2`**, ≤100 × 100ms |

Static response cannot satisfy the paired loop. Shim alternates:
- odd call → `0x06` (bits 1, 2 set, `& 3 == 2`)
- even call → `0x02` (bit 1 only, `& 3 == 2`)

Both values pass the `0x917264` condition; alternation lets the paired loop exit both halves.

---

## Verified handshake values

**Netbridge firmware CRC32** (response for request cmd `0x30`):

```
0x357f5327
```

Computed as zlib CRC32 chained over `netbridge.elf` sections in order `.text` → `.rodata` → `.binary_info` (with each running CRC seeding the next). Verified against game acceptance. Pre-compute script in JOURNAL Step 13.5.

**Pwrdist "present" response payload** (response for request cmd `0x10`):

```
01 00 00 00 00 00 00
```

Payload byte 0 = `0x01` produces `data_490e61c = 0x010000`, which is non-zero → game takes the "Spike 3 pwrdist present" branch. Other non-zero values likely work; this is just the one we use.

Side effect: bytes 3..5 = `00 00 00` produce `data_490e620 = 0x000000`, which the game prints as `runtime version: 0.0.0`. Boot version is printed as `1.0.0`. This is cosmetic — the game still takes the "pwrdist present" branch on non-zero boot version.

**Cmd 0x16 response payload**:

```
01 00 02 00
```

`rx[0]=1` (must be > 0), `rx[2]=2` (must equal 2). `rx[1]` and `rx[3]` are stored to caller out-pointers but not checked. See Step 19.3 for full caller analysis.

---

## Host-side hardware stubs (LD_PRELOAD shim)

Paths intercepted by `_emulation/spi_shim.c`:

| Device | fd type | Purpose | Notes |
|--------|---------|---------|-------|
| `/dev/spidev4.0` | fake fd 1023 | CPU-board 8-byte SPI polling (local switches, not node bus) | RX[2] = `0x04` advances state machine. TX currently all-zeros (gated on bridge). |
| `/dev/i2c-10` | fake fd 1022 | EEPROM at slave 0x57 | Read/write-back works; no real state maintained. |
| `/dev/ttyAMA5` | real `socketpair(AF_UNIX, SOCK_STREAM)` | UART to netbridge | Game gets `sv[0]`; shim holds `sv[1]`. Reads block naturally on the socketpair. |
| `/dev/dri/card1` | real `socketpair(AF_UNIX, SOCK_STREAM)` | DRM KMS display (Step 21, reworked in Step 24) | Same socketpair pattern as UART — game gets `sv[0]`, shim holds `sv[1]`. DRM ioctls are intercepted before they'd reach the real socketpair (which would reject them): `VERSION` / `GET_CAP` / `SET_CLIENT_CAP` / `MODE_GETRESOURCES` (1 CRTC, 2 connectors both reporting connected, 1 encoder, 1920×1080@60) / `GETCONNECTOR` / `GETENCODER` / `GETCRTC` / `SETCRTC` / `CREATE_DUMB` / `ADDFB2` / `PAGE_FLIP` / `GEM_CLOSE`. `MAP_DUMB` returns ENOTSUP. On `PAGE_FLIP` with `DRM_MODE_PAGE_FLIP_EVENT` (flag 1) the shim writes a 32-byte `drm_event_vblank`-shaped event to `sv[1]` so the game's `select()` and `drmHandleEvent` advance. |

**Library-level hooks for the display path (Step 21/22):**

| Layer | Hooked functions | Notes |
|---|---|---|
| GBM | `gbm_create_device`, `gbm_surface_create` (+`_with_modifiers`), `gbm_bo_create` (+`_with_modifiers`), `gbm_bo_destroy`, `gbm_bo_get_{width,height,stride,format,handle,modifier,fd}`, `gbm_bo_map`, `gbm_bo_unmap`, `gbm_surface_lock_front_buffer`, `gbm_surface_release_buffer`, `gbm_surface_has_free_buffers`, `gbm_device_destroy`, `gbm_device_get_{fd,backend_name}`, `gbm_device_is_format_supported` | Returns `calloc(256)` scratch-backed pointers so the game can dereference fields without SEGV. Each BO is registered in a side-table (32 slots, mutex-guarded) holding its `(width, height, format, stride=width*4)` and an `is_dmd` flag; `gbm_bo_get_*` consult the table so the DMD BO correctly reports 1024×256 ABGR8888 instead of the screen default. `gbm_bo_map` returns `calloc(stride*height)` backing on first map; `gbm_bo_unmap` dumps `is_dmd` BOs to `_emulation/dmd/frame_NNNNNN.bgra` (1024×256×4 = 1 MB per frame) and frees the backing. |
| EGL | `eglGetDisplay` / `eglGetPlatformDisplay(EXT)`, `eglInitialize`, `eglTerminate`, `eglBindAPI`, `eglQueryString`, `eglChooseConfig` / `eglGetConfigs` / `eglGetConfigAttrib` (returns RGBA8 + `NATIVE_VISUAL_ID=0x34325258` to match XR24), `eglCreateContext` / `eglDestroyContext`, `eglCreateWindowSurface` / `eglCreatePbufferSurface` / `eglDestroySurface`, `eglMakeCurrent`, `eglSwapBuffers`, `eglSwapInterval`, `eglQuerySurface`, `eglSurfaceAttrib`, `eglGetError` (always `EGL_SUCCESS`), `eglGetProcAddress` (routes to shim-owned stubs for `eglGetPlatformDisplayEXT`, `eglCreateImageKHR`/`DestroyImageKHR`, `eglCreateSyncKHR`/`DestroySyncKHR`/`WaitSyncKHR`/`ClientWaitSyncKHR`, `glEGLImageTargetTexture2DOES`) | All fake handles. |
| GLES 3 | ~70 entry points: `glGetError/glGetString/glGetIntegerv/glGetShaderiv/glGetProgramiv`, `glGen*/glCreate*` families, all binding functions, `glTexImage2D/glTexSubImage2D/glCompressedTexImage2D/glCompressedTexSubImage2D/glTexParameteri/glPixelStorei`, `glShaderSource/glCompileShader/glAttachShader/glLinkProgram/glUseProgram`, uniform family, `glVertexAttribPointer/glEnableVertexAttribArray`, framebuffer family (`glCheckFramebufferStatus → GL_FRAMEBUFFER_COMPLETE`), state (enable/disable/blend/viewport/clear), draw (`glDrawArrays/glDrawElements`), `glReadPixels` | Most no-op. Compile/link status always `GL_TRUE` so Radium pipeline doesn't bail. Object IDs are incrementing counters. **Capture hooks (Step 24)**: `glShaderSource` dumps concatenated source to `_emulation/shaders/shader_NNN_idM_typeXXXX.glsl` (type recorded in `glCreateShader`); `glTexImage2D/glTexSubImage2D` dump non-NULL `data` to `_emulation/textures/tex_NNNNNN_WxH_fmtXXXX_typeYYYY.raw` sized via a GLES bytes-per-pixel table; `glCompressedTexImage2D/glCompressedTexSubImage2D` dump `size` bytes to `_emulation/textures/tex_compressed_*.bin`. |

Verified by Step 24 run: game completes full bridge → DRM resource enumeration → Radium compiles **12** shader programs (24 shader objects, 12 vertex `type=0x8b31` + 12 fragment `type=0x8b30` — refining prior "11 programs" count) → SternLogo PNG uploaded as 1920×1080 RGBA → 16 additional RGBA textures uploaded at 1024×{64,128,256,512} → DMD `gbm_bo_map`/`gbm_bo_unmap` path executes and produces one 1024×256 BGRA frame on disk → process aborts on ALSA assertion (new blocker, not a GL problem). The prior Step-22 SEGV at `NULL+0x48` is refuted by this run and by design — `gbm_bo_map` is now hooked.

## Run command

```bash
cd /mnt/c/Workspace/pokemon-le-0.81.0/games/pokemon_le && \
timeout 10 qemu-aarch64 -L /mnt/c/Workspace/pokemon-le-0.81.0 \
  -E LD_PRELOAD=/mnt/c/Workspace/pokemon-le-0.81.0/games/pokemon_le/_emulation/spi_shim.so \
  -E SPIKE3_SYSROOT=/mnt/c/Workspace/pokemon-le-0.81.0 \
  ./game
```

Rebuild shim after editing `spi_shim.c`:

```bash
cd /mnt/c/Workspace/pokemon-le-0.81.0/games/pokemon_le/_emulation && \
  aarch64-linux-gnu-gcc -shared -fPIC -o spi_shim.so spi_shim.c -ldl -lpthread
```

(`-lpthread` needed since the `pthread_create` override in the shim calls `pthread_attr_getinheritsched` / `pthread_attr_setinheritsched`.)

---

## Update rule

Add an entry here only when a test has proven it, and cite the journal step. Remove an entry (don't leave it marked "maybe wrong") if it's ever refuted. Scope for this file is facts about the platform and our shim's interactions — not session state, not to-do lists, not plans.
