# `_emulation/src/tests/` — host-side unit tests

Fast, game-free tests for the shim's logic. They run on the **host** compiler
and Python — not the aarch64 cross-compiler, and not inside qemu. Test sources
mirror `src/`'s module layout: `tests/nodebus/` tests `src/nodebus/`. Run after
touching a covered TU (or its protocol model):

```sh
cd _emulation/src/tests && ./run_tests.sh   # build + run everything; non-zero exit on any failure
CC=clang ./run_tests.sh                      # pick a different host compiler
```

Build artifacts go to `.build/` (git-ignored). No game binary, no `/games` bind
mount, no sysroot — pure logic tests. `run_tests.sh` passes `-I` pointing at
`_emulation/src/`, so a test in `tests/<module>/` can `#include "spi_shim.h"`
and `#include "<module>/<file>.c"`.

## What's here

| File | Role |
|---|---|
| `nodebus/test_nodebus.c` | C tests for `src/nodebus/shim_nodebus.c`. `#include`s the TU directly so it can reach the `static` helpers; supplies the externs it expects from the rest of the shim (`uart_shim_fd`, `real_write`, `shim_resolve_game_base`, `shim_switch_*`, `usleep`) as test doubles. `real_write` is pointed at a capture buffer, so each test feeds a request frame to `synthesize_bridge_response()` and asserts on the bytes that came back. |
| `nodebus/nodebus_model.py` | The **golden model** — an independent re-implementation of the verified pieces of PROTOCOL §4/§5 (bridge frame framing, the netbridge CRC32, the cmd-0x41 cipher builder verbatim from §5.1, the cmd-0x41 inbound reply validator). Used as the oracle: when the model and `shim_nodebus.c` disagree, exactly one is wrong. |
| `nodebus/test_nodebus_model.py` | Self-tests for `nodebus_model.py` against the values PROTOCOL already pins down (`02 30 01 cf 03` checksum, `85 01 ae cc` cipher frame, the 3 validator gates, escape round-trips). |
| `run_tests.sh` | Builds + runs everything; add new module test dirs to its list as they appear. |

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

## Current coverage (pilot)

- **Framing primitives:** `bridge_checksum`, `bridge_{encode,decode}_escape`,
  `nodebus_fill_checked_inner`, and `nodebus_cipher_decrypt` involution.
- **Bridge-level command contracts:** cmd `0x10 / 0x11 / 0x15 / 0x16 / 0x18 /
  0x1c / 0x1d / 0x20 / 0x25 (+ 0xF0 enum-reset) / 0x27 / 0x30`, plus the
  deliberate-timeout cases (unknown cmd, cmd `0x40`, short cmd-`0x41` frame).
- **Model:** framing round-trips, the `02 30 01 cf 03` / CRC32 frames, the
  `85 01 ae cc` cipher vector, the cmd-0x41 reply validator's 3 gates.

## Not covered yet (next increments)

- **cmd-0x41 inner-command contracts** (`0xFE` fe_probe, `0x11` switch poll,
  `0xFA`, `0xF2` block read, `0xFF` discovery, `0xFC/0xFD/0xF9` key fetch, the
  slot-1 `0xed`/`0xee` poll loop, the Service-Menu Node Bus Test scan traffic).
  These need the live-capture catalog + a `shim_resolve_game_base()` fixture
  region (slot structs, the LPC chip vtable, the firmware-record list) so the
  version-lookup and switch-idle paths can be exercised.
- **The `hint` / `__n` naming reconciliation** — PROTOCOL §5.1 and
  `shim_nodebus.c` use "hint" for two values that differ by 2 (the wrapper's
  expected payload length vs. the on-wire response-length byte). And the §5.1
  prose ("increments the ilen byte by one") vs. the §5.1 Python ref (which
  doesn't) vs. `nodebus_cipher_decrypt`'s mask formula need cross-checking
  against `mcp_nodebus_send_encrypted @ 0x8ec410`. The model documents the
  collision; resolving it is a catalog-phase RE task.

## Other test/diagnostic scripts (not yet under `tests/`)

`src/decode_daemon_test.py` (decode-daemon round-trip) and
`src/switch_noise_test.py` (integration: watches the runtime switch array during
a live run) still live flat in `src/`. They move under `tests/<module>/` when
their subsystems get a TDD pass.
