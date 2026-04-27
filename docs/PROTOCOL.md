# Spike 3 Emulation — Verified Protocol Reference

**Purpose.** Everything in this document has been directly observed or proven by test. Hypotheses live in `JOURNAL.md`; this file only gains an entry once a test confirms it. Every entry cites the journal step that established it so you can trace the evidence. If something here turns out to be wrong, delete it — do not leave refuted claims as "maybe still true."

**Source of truth for current work status:** `JOURNAL.md`. This file is for facts; that file is for narrative.

---

## Address convention

The BN database is rebased to base 0x0, so BN VA == ELF VA == file offset for this binary. Runtime address is `game_base + va`. No translation between BN and objdump views is required.

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

## Sound event registration — verified (Step 41)

**Function:** `sub_8d3460` at BN VA `0x8d3460` / file offset `0x8d3460`. Called during C++ static init before GDScript runs.

**Effect:** Populates `std::unordered_map<std::string, uint16_t>` at `data_48c56d0` (BN VA `0x48c56d0`) with **25 sound-event name → token entries**. Each registration:

```
sub_b094c0(&data_48c56d0, &"SE_PK_FX_...", token, 0xffffffff)
```

Insertion helper `sub_b094c0` → mutex-guarded (`sub_b09370`) → hash via `sub_b09850` using `std::_Hash_bytes` with seed `0xc70f6907` (= `std::hash<std::string>`).

**Token namespace** (contiguous ranges):

| Token range | Count | Name prefix |
|---|---:|---|
| `0x6b–0x71` | 7 | `SE_PK_FX_EOB_POKEDEX_*` |
| `0xba–0xcb` | 18 | `SE_PK_FX_ATTACK_TYPE_*` |

**Full 25-entry table:**

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

**Signature** (Itanium mangled `_ZN16GodotSpikeBridge14register_soundERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEtiE…`):

- 1st: `std::string const&` (name)
- 2nd: **`uint16_t`** (token) — `t` = unsigned short, **not** uint32_t
- 3rd: `int` (flags — always `0xffffffff` at observed registration sites)

The Godot bridge exposes this method to GDScript via a `std::function<void(Array const&)>` adapter lambda (typeinfo at BN VA `0x46bb1f8`, libstdc++ manager at `sub_b086b4`). The lambda unpacks an `Array` from GDScript and calls into `register_sound` with typed arguments.

**GDScript call pattern:** `Spike.play_sound(SpikeSoundIds.SE_PK_FX_<NAME>)` — passes the string; the C++ bridge looks it up in the hashmap to resolve to a token.

**What's NOT in the hashmap:** ~725 other `SE_PK_*` strings from the previs `spike_game_sound_ids.gd` autoload (VO, TUNE, MX, BONUS, THEME, and several FX families beyond attack-type / EOB-pokedex). Their dispatch path is not located statically — see 5-stage pipeline below.

**Full 5-stage resolution pipeline** (added Step 42):

| Stage | Function | Effect |
|---|---|---|
| 1 | `sub_8d3460` (static init) | Name string → uint16_t token, into hashmap at `data_48c56d0` (25 entries only) |
| 2 | `sub_b09660` | Name lookup in the hashmap; returns hash node with token at offset `+0x28` |
| 3 | **NONE — runtime only** | Token → asset_id: **not in compiled code**. Populated from scene bytecodes (type-0x0b records) by `load_scene_assets` at `0xb321d0` when each scene loads. Per-scene variants possible. |
| 4 | `asset_registry_find` over `g_asset_registry` at `data_48ca4d0` | Encoded asset_id → `{file_offset, size, type}` |
| 5 | `get_asset_ptr_by_packed_id` → `image_mapped_base_plus_offset` | File offset → absolute pointer inside mmap'd `image.bin` |

**Why static extraction fails** (Sidekick, Step 42): "This separation allows content updates without binary recompilation — a regulatory compliance requirement for gaming machines." No `uint32_t token_to_asset[]` array exists; token constants never appear in asset-ID construction paths. Same `SE_PK_*` can map to different asset_ids across scenes.

**Implication**: to obtain the full name → asset_id mapping, either (a) runtime-capture under emulation by hooking `asset_registry_find` / `sub_b09660` after `load_scene_assets` has run for every scene, or (b) dump `g_asset_registry` from a running game on real hardware (CM4). Both are deferred as of Step 42.

Verified by: JOURNAL Step 41 (stages 1–2) and Step 42 (stages 3–5, architectural rationale).

---

## Game event dispatch — `fire_event` verified (Step 52)

**Function:** `sub_9c12a0` at BN VA `0x9c12a0` / file offset `0x5c12a0`.

**Signature:** `uint64_t fire_event(int32_t event_id, int64_t event_data)`

```c
uint64_t fire_event(int32_t event_id, int64_t event_data) {
    if (event_id > 0xce) {
        CC_criticalErrorHandler(0x6a);
        return 1;
    }
    for (int64_t* i = data_485ea80[event_id]; i; i = i[2]) {
        if (i[0]) {
            int32_t r = ((int32_t (*)(int64_t))i[0])(event_data);
            if (!(r & 1)) return r & 0xff;
        }
    }
    return 1;
}
```

**Linked-list node layout at `data_485ea80 + event_id * 8`**:

| Offset | Field | Notes |
|---|---|---|
| `+0x00` | `callback_fn` | `int32_t (*)(int64_t event_data)` — handler function pointer |
| `+0x08` | `priority` | byte; insertion is descending-priority (verified Step 51 via `sub_9c11d0`) |
| `+0x10` | `next` | `node*` — loop variable `i[2]` (LP64: stride 8, so `i[2]` = byte offset +0x10) |

**Bounds:** `event_id` 0..0xce (206 event types max). Exceeding triggers critical error code `0x6a`.

**Return semantic:** dispatcher returns `1` on full-chain completion. If any handler returns `!(r & 1)` (i.e. even value), dispatch short-circuits and the dispatcher returns `r & 0xff`. Handlers that want the chain to continue return an odd value (conventionally `1`).

**Registered handlers (observed Step 51 runtime):** 102 / 207 event slots populated, 236 total handlers. Event `0xc5` has 2 handlers — one is `sub_950d60`, which iterates the 42-cmode registry at `data_4767b30` and calls vtable slot `+0xf0` on each.

**Known callers of `fire_event`** (needs further BN resolution, Step 52): `sub_b129f0`, `sub_bd39f0`, `sub_b16f64`, `sub_b82990`, `sub_b12760`. One (directly or transitively) is the switch→event translator.

**Trampoline pattern for the shim:**

```c
typedef uint64_t (*fire_event_t)(int32_t event_id, int64_t event_data);
uintptr_t game_base = shim_resolve_game_base();
fire_event_t fire_event = (fire_event_t)(game_base + 0x5c12a0);
uint64_t r = fire_event(event_id, event_data);
```

`event_data` is opaque to the dispatcher and handler-specific. Event `0xc5` ignores it (handler invokes cmode vtable with no args). Other events may require a valid pointer; calling with `event_data=0` for unknown events can SEGV in the handler.

---

## Switch state array — verified (Step 52)

The game maintains an in-memory mirror of each physical switch's state. The array is populated by the UART cmd `0x27` receive path (netbridge → CPU, 1-byte status payload per poll cycle — see "Cmd 0x27 status byte" above); the specific decoder function that writes into this array is not yet identified.

**Layout:**
- Base: `data_4880220` (BN VA)
- Stride: `0x30` bytes per record
- Count: `data_48cbaa0` (runtime-determined; Pokemon LE has ~105 switch IDs per the manual, but the live count reflects only installed+valid switches)

**Per-switch record:**

| Offset | Field | Notes |
|---|---|---|
| `+0x10` | `config_ptr` | pointer to a config struct whose `+0x30` byte encodes flags |
| `+0x10 → +0x30` | config flags | bit 2 (`0x04`) = invert, bits 5+6 (`0x60`) = broken/stuck masks |
| `+0x28` | `state` | current state byte — 1 closed, 0 open. Inverted before return if invert flag is set. |

**Parallel arrays keyed by switch number:**

- `data_4880228[sw_num]` — `sw_broken` flag (NVRAM-backed, persists across reboots)
- `data_4880230[sw_num]` — `sw_stuck_active` flag (NVRAM-backed)
- `data_4880238[sw_num * 2]` — `sw_alert_count` (16-bit, NVRAM-backed)

**Readers:**

- `sub_a50850(sw_num) @ 0xa50850` — returns the state byte with the invert flag applied. Short-circuits to 0 on `sw_num == 0` or `sw_num >= data_48cbaa0`.
- `sub_a50350(sw_num) @ 0xa50350` — returns 1 if the switch is "OK" (not broken, not stuck, alert count clear); 0 otherwise. Gates event firing.

**Injection target for shim-side VPX bridge:** writing a byte to `game_base + 0x4480220 + sw_num * 0x30 + 0x28` simulates a switch closure at the state-array level.

---

## Switch → event dispatch is a two-stage architecture (verified Step 53)

Step 52 hypothesised a single "switch translator" function that would both read `data_4880220` and call `fire_event`. **Refuted.** The actual design decouples hardware input from event dispatch:

### Stage 1 — `sub_b24f70 @ 0xb24f70` — switch-state reader / display updater

Reads switch state via `sub_a50850(sw_num)` at call site `0xb25438`. Iterates over switch-id arrays at `data_47643a8` (offset tables that group switches by function). **Does not call `fire_event`**. Its output is "updated game/display state" — writes to cmode fields, lamp state, display elements.

### Stage 2 — game logic observes state changes and fires events

Separate functions (not yet fully enumerated) observe the state changes Stage 1 produces and call `fire_event` with specific event_ids + payloads. Each of the 5 direct callers of `fire_event` does one narrow job:

| Caller | VA | event_id | event_data | Purpose |
|---|---|---:|---|---|
| `sub_b129f0` | `0xb129f0` | `0xc2` | `0` | Mode state transitions |
| `sub_bd39f0` | `0xbd39f0` | `0x12` | `&uint16` | Event validator — checks against lookup tables at `0x4787fa8` and `0x4780724` before firing |
| `sub_b16f64` | `0xb16f64` | `0xc3` | `1..4` | Lamp timer events; sources data from `data_4807430` |
| `sub_b82990` | `0xb82990` | `0x06` | `&struct` | Award / score events; reads `data_47790f8`, range-checks `0 < arg1 < 0xf0` |
| `sub_b12760` | `0xb12760` | `0xc1` | `0` | Mode init completion — fires once at end of setup |

None of them reads the switch state array.

### Implications for the shim

- **Direct write to `data_4880220 + n*0x30 + 0x28` is the correct lowest-level input** — it's the same byte the UART cmd 0x27 receive path writes. Stage 1 (`sub_b24f70`) picks it up on its next poll iteration.
- **Calling `fire_event(id, 0)` blindly bypasses Stage 1 entirely** — it sidesteps all the game logic that decides *which* event to fire based on state. That's why the Step 52 blind sweep had 1 % signal.
- **To test a specific game behaviour, close the relevant switch via the state array and let the game's own Stage 2 decide what events to fire.** This is what `shim_fire_switch` already does.
- A shim hook on `sub_b24f70` would let us observe which switch closures trigger which state changes — useful introspection.

### Known event_id semantics (Step 53)

| event_id | Observed meaning | Evidence |
|---:|---|---|
| `0x06` | Awards / score events | `sub_b82990` payload is a struct from `data_47790f8`; range check `0 < id < 0xf0` |
| `0x12` | Validated events (filtered through lookup tables before firing) | `sub_bd39f0` validates against tables at `0x4787fa8` + `0x4780724` |
| `0x3e` | **Battle-environment activation (empirical)** — activated `cmode_battle_coast` during Step 52e sweep | `+0x308: 0 → 0x100000000` |
| `0xc1` | Mode init complete | `sub_b12760` fires once at end of setup |
| `0xc2` | Mode state transition | `sub_b129f0` fires unconditionally |
| `0xc3` | Lamp timer event (payload 1..4 selects lamp subgroup) | `sub_b16f64` cycles values 1-4 |
| `0xc5` | "Broadcast to all 42 cmodes" — calls vtable slot `+0xf0` on each | Step 51: `sub_950d60` is event-0xc5 handler |

All other event_ids remain semantically unknown. The full mapping lives in the chain of callers that reach each event via `sub_9c12a0`.

---

## GStreamer — linked but unused in Pokémon LE 0.81 (verified Step 53)

**The game binary does not actively use GStreamer.** This was empirically confirmed by:

1. **Import analysis**: the game imports exactly TWO gst-prefixed symbols — `gst_init_check` and `gst_deinit`. Zero imports from `libgstapp`, `libgstvideo`, `libgstbase`, `libgstallocators`. No `gst_element_factory_make`, no `gst_parse_launch`, no `gst_pipeline_*`, no `gst_bin_*`, no `gst_pad_*`, no `gst_caps_*`, no `gst_buffer_*`. Compare: **28 libavcodec/libavutil/libavformat imports** that ARE actively used.
2. **String analysis**: no element-name strings (`"appsink"`, `"kmssink"`, `"decodebin"`, `"playbin"`, `"videoconvert"`, etc.) anywhere in the binary. No `GstElement`/`GstPipeline`/`GstPad`/etc. type-name strings.
3. **Dynamic loading**: no `dlopen`/`dlsym`/`dlvsym` imports — the game does not runtime-load a gstreamer plugin module.
4. **Shim force-call test**: trampoline into `game_base + 0x8c1ec0` at runtime successfully invokes `gstreamer_init`, which calls `gst_init_check` → returns 1, gstreamer initialises, plugin-scanner warns about missing helper binary, `VideoMessageBus` thread starts. Everything works when explicitly invoked — but the game itself never invokes it.

**`gstreamer_init`** (`sub_cc1ec0`), **`gstreamer_shutdown`** (`sub_cc2158`), **`gstreamer_message_bus_thread`** (`sub_cc1ea0`) are all defined in the game binary but appear to be leftover framework code from Stern's shared Spike 3 platform. Other Spike 3 titles may use gstreamer; this one does not.

### Why this matters for rendering

- **Gstreamer offers zero leverage** for intercepting Pokémon LE's video pipeline.
- The actual video path is **100% direct libavcodec** via Stern's `spi_video_player` module (`SpiVideoStreamDecoderBase` class) plus Radium's own `VideoStreamDecoder`.
- To render video, the Step-47 DRM-prime approach (fabricate `AVDRMFrameDescriptor` with a real DMA-BUF fd, consume via `glEGLImageTargetTexture2DOES`) remains the correct path. Or a separate approach: swap in a real EGL/GLES3 backend (Mesa on WSLg) and let the pipeline run end-to-end on a real GPU.
- The shim's gstreamer hooks stay in place — harmless, forward to real libgstreamer, and will fire if any code we haven't seen yet does reach them — but they are not an active interception point.

### Shim trampoline (kept as diagnostic)

`SPIKE3_FORCE_GSTREAMER=<seconds>` fires a timer thread that, after N seconds, calls `game_base + 0x8c1ec0` — the game's `gstreamer_init`. Useful for confirming the function is callable without the game initiating the call. Not part of the normal run configuration.

---

## g_asset_registry — `std::unordered_map` layout verified (Step 43)

**Location:** BN VA `0x48ca4d0` → ELF VA `0x44ca4d0` → runtime VA `game_base + 0x44ca4d0` (QEMU ASLR-dependent game_base resolved via `dladdr(frames[i])` on any backtrace frame whose `dli_fname` ends with `/game` and is NOT a `.so`).

**Struct** (libstdc++ `std::_Hashtable`, 56 bytes, populated by `load_scene_assets` during C++ static init):

| Offset | Type | Field | Pokémon LE 0.81 value |
|---|---|---|---|
| +0x00 | `uintptr_t` | `_M_buckets` | heap ptr (bucket array) |
| +0x08 | `uint64_t` | `_M_bucket_count` | 5087 (prime for 2452 elements) |
| +0x10 | `uintptr_t` | `_M_before_begin._M_nxt` | heap ptr (first node in chain) |
| +0x18 | `uint64_t` | `_M_element_count` | **2452** (= image.bin section-7 record count) |
| +0x20 | `float` | `_M_max_load_factor` | 1.0 |
| +0x28 | `uint64_t` | `_M_next_resize` | 5087 |
| +0x30 | `uintptr_t` | `_M_single_bucket` | NULL |

**Node layout** (libstdc++ `_Hash_node<pair<const uint64_t, AssetRecord>, true>`):

| Offset | Type | Field |
|---|---|---|
| +0x00 | `uintptr_t` | `_M_nxt` — next node in bucket chain |
| +0x08 | `uint64_t` | **key** = `(flags << 32) \| asset_id_low` — matches image.bin section-7 bytes 8–15 |
| +0x10 | `uint64_t` | `_M_hash_code` (cached) OR first value field |
| +0x18..+0x2f | *varies* | `AssetRecord` value fields (exact layout TBD) |

Across all 2452 nodes: **unique key count = 2452** (confirmed via shim dump). U32-at-+0x08 has only 64 unique values (the `asset_id_low` category codes); u32-at-+0x0c has 256 unique values (the `flags` field).

**Cross-reference with image.bin section 7 (Step 43 verification):** the 2452 u64 keys from the runtime registry intersect **exactly** with the 2452 `(flags << 32) | asset_id_low` composite values parsed from image.bin section-7 records. Intersection = 2452, `reg-only` = 0, `img-only` = 0. This proves the registry is a runtime-populated cache of section-7 data and nothing more — **static extraction from image.bin alone is sufficient**.

**Implication for extraction:** a standalone tool (`_extraction/tools/spike_extract/`) parses image.bin section 7 and extracts every asset without needing the game binary or emulator. The shim's `asset_registry.bin` dump serves only as validation.

**Discovery path** (followed without additional Binary Ninja budget):
1. Lambda typeinfo at BN VA `0x46bb1f8` → rodata string at `0x3bc8f08`.
2. libstdc++ `std::function` manager `sub_b086b4` at file offset `0xb086b4` (stateless-lambda variant: op 0 = get_type_info, op 1 = get_functor_ptr, op 2 = clone_functor).
3. Xrefs to the manager → `sub_8d3460` at file offset `0x8d3460` (25-string sound-event registration, documented above).
4. Shim memory dump (`_emulation/shim_diagnostics.c` → `shim_dump_sound_globals`) reads the `_Hashtable` struct + walks `_M_nxt` chain → `_emulation/captures/asset_registry.bin` binary dump (format documented in `_extraction/tools/spike_extract/registry_dump.py`).
5. Parser matches 2452/2452 against image.bin section 7 → registry is a cache, not a source of new info.

Verified by: JOURNAL Step 43.

---

## image.bin section 5 — per-asset bytecode descriptor table (verified Step 44)

**Scope correction vs. earlier assumption.** Section 5 is NOT a scene → asset list. It is a per-asset descriptor table: one bytecode program per asset, describing how to load/render that single asset. 12,568 uint64-slot table grouped in 5-tuples (language variants) → 2,492 unique bytecode offsets in the bulk region `[0x42EEEB3C, 0x42F23E06)`.

Across all 2,492 bulk bytecodes, 2,414 contain exactly one type-`0x0b` "asset reference" record (the remaining ~78 are meta-descriptors without direct asset references). Correct decoder behavior: bound each walk by the next unique section-5 offset (not by tag 0x00 alone).

**Tag frequency per typical bytecode** (eos-terminated):

| Tag | Size | Count across all bulk bytecodes | Typical role |
|---:|---:|---:|---|
| `0x05` | 6 | 2,496 | Header — byte 2 is 256-distinct (matches section-7 flag count) |
| `0x02` | 2 | 2,577 | Short reference |
| `0x0b` | 10 | 2,414 | **Asset reference** — carries asset_id_low in payload |
| `0x09` | 15 | 4,483 | Animation/transform data |
| `0x04` | 2 | 108 | Short reference (secondary variant) |

### Type-0x0b asset reference payload layout (10 bytes)

| Byte | Distinct values (across 2414 records) | Meaning |
|---:|---:|---|
| 0-2 | always `0x00` | padding |
| 3 | 16 | `asset_id_low` bits 0-7 |
| 4 | `{0x00, 0x04}` | `asset_id_low` bits 8-15 |
| 5 | `{0x00, 0x02}` | `asset_id_low` bits 16-23 (bit 17 is the stereo-audio marker) |
| 6 | `{0x00, 0x20}` | `asset_id_low` bits 24-31 (bit 29) |
| 7 | `{0x00, 0x02}` | partial flag bit (word bit 1) |
| 8 | always `0x00` | padding |
| 9 | 8 values | partial flag bits (word bits 20-22 after `<< 16`) |

**Verified:** u32 LE read at payload offset +3 is a valid `asset_id_low` for every one of the 2,414 records (100% match against the 64 distinct id_lows in section 7).

**Not fully verified:** the 32-bit `flags` word of the section-7 composite key (`(flags << 32) | id_low`). Bytes 7 & 9 of 0x0b alone encode at most 16 combinations but section 7 has 256 distinct flag values. The likely missing source is byte 2 of the preceding type-`0x05` header (which has 256 distinct values across all 2,496 `0x05` records — matching the section-7 flag cardinality exactly), but we don't yet have a formula that joins them to 100% resolution. Best static brute-force: `flags = (H[0] << 24) | P[7] | (P[9] << 16)` yields 660/2414 matches. See JOURNAL Step 44.4.

Practical implication: to produce a complete `(asset_id_low, flags)` extract statically from image.bin alone, either finish the 0x05-byte-2 decoding work or capture at runtime. For partial extraction (asset_id_low only), this is sufficient.

---

## image.bin section 2 — playfield/mode grid records (verified Step 44)

Section 2 @ 0x189b8, length 0x2E28 (11,816 bytes), is a table of 1,477 uint64 pointers into section 0 (and adjacent regions). Pointers step by variable deltas (408 most common; then 40, 64, 48, 56, 88). Each pointer targets a variable-size **grid record**.

First 12 records (and most `delta == 408` records) are identical-shaped:

```
+0x00  uint32 LE  record_id (sequential 1, 2, 3, ...)
+0x04  uint32 LE  flag/variant (alternating 0 / 2)
+0x08  uint16 LE  width  (17)
+0x0a  uint16 LE  height (23)
+0x0c  uint8[5]   padding
+0x11  uint8[391] grid cells (17 × 23, values are small enums: 0x00, 0x03, 0x0f, …)
+0x188 total size 408
```

Later records (delta != 408) likely have different grid dimensions.

**Not a scene → asset mapping.** Zero of the section-2 uint32 values match any `asset_id_low` from section 7, and zero uint64 values match any section-5 offset. The README's label "numbered grid data" is literal — these records hold numbered grid patterns, most plausibly playfield lamp/target/insert state tables used by the gameplay logic per mode or per animation frame.

Verified by: JOURNAL Step 44.1.

---

## `scene_config_table.c` in _extraction/ is the Spike 3 service menu, NOT the 885 gameplay scenes (noted Step 44)

The 1,254-line `scene_config_table.c` export covers VA 0x48071d0 onward, which holds the Spike 3 **service/test/adjustments menu tree** (Active Switch Test, Game Adjustments, Pikachu Tests and Calibration, Exit the test menu, etc., plus a handful of audio-test items: "MUSIC: MAINPLAY", "SOUND: SUPER SKILL SHOT", "SPEECH: 'SQUIRTLE'").

Each entry is a `{function_ptr, label_string, u16_index, u16_parent_id}` quadruple. This is the menu structure Nuklear walks when the operator enters test mode.

The real `scene_config_table` lives at VA 0x47fcf10 (CLAUDE.md's claim), 41,664 bytes earlier. It is either BSS (populated at runtime by an unidentified setup function) or static data not yet exported from the `.bndb`. Closing the 885 scene → asset_id mapping requires either re-exporting that region or capturing it at runtime.

File should be renamed to `service_menu_table.c` for clarity.

Verified by: JOURNAL Step 44.5.

---

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

## Bridge-command timeouts as progress signal (verified Step 45)

Some bridge commands **require** the 1000 ms channel timeout to fire — the timeout IS the state-machine transition, not the failure mode. If we synthesize an early ACK, the game interprets our ACK as "response received but no status payload" and keeps polling forever.

Confirmed for cmd **0x41** (handler at BN VA `0x915f88`): replacing the default "no synthetic response" branch with a zero-payload ACK sent progress BACKWARD — the game stopped reaching Godot init entirely (run `_emulation/run_step45_fix1.stderr`). Reverting to the timeout path restored progress past the Step-31 PopupMenu SEGV and reached four Godot scenes (run `_emulation/run_step45_fix2.stderr`).

Likely also applies to cmds **0x25, 0x12, 0x19** — all of them show up as `bridge message (cmd=XX, id=N) response failed 110: Connection timed out` in the game's own stdout, and the game continues after. Do not ACK them without first reversing their handler's expected response payload. Marker to look for in a fresh run: the game's stdout line `bridge message (cmd=NN, id=M) response failed 110: Connection timed out` is **expected**, not a bug, until proven otherwise.

---

## Update rule

Add an entry here only when a test has proven it, and cite the journal step. Remove an entry (don't leave it marked "maybe wrong") if it's ever refuted. Scope for this file is facts about the platform and our shim's interactions — not session state, not to-do lists, not plans.
