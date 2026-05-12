# `_emulation/src/tests/` — host-side unit tests

Fast, game-free tests for the shim's logic. They run on the **host** compiler
and Python — not the aarch64 cross-compiler, and not inside qemu. Test sources
mirror `src/`'s module layout: `tests/nodebus/` tests `src/nodebus/`,
`tests/display/` tests `src/display/`. Run after touching a covered TU (or its
protocol model):

```sh
cd _emulation/src/tests && ./run_tests.sh   # build + run everything; non-zero exit on any failure
CC=clang ./run_tests.sh                      # pick a different host compiler
```

Build artifacts go to `.build/` (git-ignored). No game binary, no `/games` bind
mount, no sysroot — pure logic tests. `run_tests.sh` passes `-I` pointing at
`_emulation/src/`, so a test in `tests/<module>/` can `#include "spi_shim.h"`,
`#include "<module>/<file>.c"`, or `#include "<module>/<file>.h"`.

## What's here

| File | Role |
|---|---|
| `nodebus/test_nodebus.c` | C tests for `src/nodebus/shim_nodebus.c`. `#include`s the TU directly so it can reach the `static` helpers; supplies the externs it expects from the rest of the shim (`uart_shim_fd`, `real_write`, `shim_resolve_game_base`, `shim_switch_*`, `usleep`) as test doubles. `real_write` is pointed at a capture buffer, so each test feeds a request frame to `synthesize_bridge_response()` and asserts on the bytes that came back. |
| `nodebus/nodebus_model.py` | The **golden model** — an independent re-implementation of the verified pieces of PROTOCOL.md "CPU ↔ netbridge UART bridge protocol"/"Nodebus (cipher, hash, framing)" (bridge frame framing, the netbridge CRC32, the cmd-0x41 cipher builder verbatim from "NODEBUS_Cipher — Stern's per-slot XOR stream cipher", the cmd-0x41 inbound reply validator). Used as the oracle: when the model and `shim_nodebus.c` disagree, exactly one is wrong. |
| `nodebus/test_nodebus_model.py` | Self-tests for `nodebus_model.py` against the values PROTOCOL already pins down (`02 30 01 cf 03` checksum, `85 01 ae cc` cipher frame, the 3 validator gates, escape round-trips). |
| `display/test_gl_proto.c` | C layout tests for `src/display/gl_proto.h` — the `@spike3_gl` wire format. The bytes on the wire *are* the packed structs, so this pins: the 8-byte message header (op@0/flags@2/payload_len@4), that `#pragma pack(push,1)` actually took effect, and a curated set of payload struct sizes. Self-contained (no shim externs). |
| `display/test_gl_proto_consistency.py` | Keeps `gl_proto.h` and `gl_proto.py` in lockstep: parses the `.h` as source-of-truth (op/flag `#define`s + struct sizes), imports `gl_proto.py`, asserts the `GL_OP_*` / `GL_FLAG_*` constant sets match both ways, op values are distinct (name↔value bijection) with reply ops in the `0x0F00..0x0FFF` band, the header framing helpers round-trip, and the curated payload struct sizes equal `struct.calcsize` of the matching `*_FMT` strings (same literals `test_gl_proto.c` uses). |
| `run_tests.sh` | Builds + runs everything; add new module test sections as they appear. |

## TDD discipline

The plan is: a failing test from the spec **first**, then make the shim TU pass
it. Sources of "the spec", in order of trust:

1. **The game-side consumer's decompile** — what the caller actually reads from a
   reply (`sub_d15aa8`'s per-cmd handlers, `mcp_nodebus_send_encrypted`'s 3-gate
   validator, `FUN_008f38e0` / `FUN_00674680` for fe_probe, `FUN_008f5390` for
   the LPC1313 block read, the ~160 wrapper hint bytes). Re-verify against the
   binary; PROTOCOL is a starting point, not the final word.
2. **Live boot captures** — replay real request frames harvested from a run log,
   snapshot the responses, regress on change. (Catalog phase — not wired up yet.)
3. **The golden model** — `nodebus_model.py`. A C test that synthesizes a
   cmd-0x41 reply can assert it round-trips through `validate_node_reply()`.

## Current coverage

**nodebus** (pilot):
- **Framing primitives:** `bridge_checksum`, `bridge_{encode,decode}_escape`,
  `nodebus_fill_checked_inner`, and `nodebus_cipher_decrypt` involution.
- **Bridge-level command contracts:** cmd `0x10 / 0x11 / 0x15 / 0x16 / 0x18 /
  0x1c / 0x1d / 0x20 / 0x25 (+ 0xF0 enum-reset) / 0x27 / 0x30`, plus the
  deliberate-timeout cases (unknown cmd, cmd `0x40`, short cmd-`0x41` frame).
- **Model:** framing round-trips, the `02 30 01 cf 03` / CRC32 frames, the
  `85 01 ae cc` cipher vector, the cmd-0x41 reply validator's 3 gates.

**display** (pilot):
- **`gl_proto.h` on-wire layout:** 8-byte message header (field offsets/widths),
  reply ops live in `0x0F00..0x0FFF`, `#pragma pack(push,1)` is effective, ~30
  payload struct sizes.
- **`gl_proto.h` ↔ `gl_proto.py` consistency:** `GL_OP_*` / `GL_FLAG_*` constant
  sets match both ways; op values distinct (name↔value bijection); header
  framing helpers round-trip; `*_FMT` calcsizes match the C struct sizes;
  `OP_NAMES` keys are real ops.

## Not covered yet (next increments)

- **nodebus — cmd-0x41 inner-command contracts** (`0xFE` fe_probe, `0x11` switch
  poll, `0xFA`, `0xF2` block read, `0xFF` discovery, `0xFC/0xFD/0xF9` key fetch,
  the slot-1 `0xed`/`0xee` poll loop, the Service-Menu Node Bus Test scan
  traffic). These need the live-capture catalog + a `shim_resolve_game_base()`
  fixture region (slot structs, the LPC chip vtable, the firmware-record list)
  so the version-lookup and switch-idle paths can be exercised.
- **nodebus — the `hint` / `__n` naming reconciliation** — PROTOCOL.md
  "NODEBUS_Cipher — Stern's per-slot XOR stream cipher" and `shim_nodebus.c` use
  "hint" for two values that differ by 2 (the wrapper's expected payload length
  vs. the on-wire response-length byte). And the prose ("increments the ilen
  byte by one") vs. the Python ref (which doesn't) vs. `nodebus_cipher_decrypt`'s
  mask formula need cross-checking against `mcp_nodebus_send_encrypted @
  0x8ec410`. The model documents the collision; resolving it is a catalog-phase
  RE task.
- **display — `shim_gl_marshal.c` framing** (`_send_async` / `_send_sync` put the
  right bytes on the wire for an op + payload) and **`shim_avforward.c`** (the
  `AVDRMFrameDescriptor` builder, the libavcodec ABI offsets, the 8-slot session
  table). Both need `socket`/`connect`/`write` mocked the way `test_nodebus.c`
  mocks `write` — a later increment. `shim_gl.c` itself (the ~70 GLES entry
  points + dispatch) is mostly stubs/marshalling; a unit-test pass on it isn't
  high-value yet.

## Other test/diagnostic scripts (not under `tests/`)

`src/tools/decode_daemon_test.py` (decode-daemon round-trip) and
`src/tools/switch_noise_test.py` (watches the runtime switch array during a live
run) are **smoke probes** — they need a live daemon / running game, so they don't
belong here (this dir is pure logic, no game, no sysroot). They live in
`src/tools/`. They'd move under `tests/<module>/` only if a subsystem gets a real
TDD pass that lets them run game-free with a golden model.
