# Bridge protocol (host ↔ netbridge.elf)

Living spec for the protocol the game speaks to the netbridge MCU over
`/dev/ttyAMA5`. The netbridge in turn talks to the node bus (coil/LED/sensor
nodes) and a separate powerdist UART. Everything below is grounded in the
unstripped, debug-info-bearing `spike3/netbridge/netbridge.elf` (its `.bndb`
is open in BN under view 11).

We update this doc as we chip away at the protocol surface. Tables have a
**Status** column tracking shim coverage; the **TODO** section at the bottom
captures research questions ("which cmd reports switch 57 closures?" lives
there).

## Architecture

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

Two UARTs on the bridge:
- `HOSTUART` — to the CM4 game. Bidirectional. The game's outbound cmds and the
  bridge's responses both ride here.
- `POWERDISTUART` — to the powerdist board. Bridge forwards game cmds in the
  `0x10-0x1F` range here when `pdstate ∈ {5,6,7}` (Spike 3 powerdist
  detected — `POWERDIST_IsSpike3 @ 0x10002098` returns 1).

When `pdstate < 5` the bridge handles a *subset* of `0x10-0x1F` cmds locally
(see "Local fallback handlers" below). The remainder default-fall-through and
the host times out.

## Frame format (escape protocol)

Single-byte values `0x02` (STX) / `0x03` (ETX) / `0x1b` (ESC) are reserved
for framing. To send any of them as data, emit `0x1b` followed by the byte
`OR'd with 0x80`. The receiver inverts. Implemented on the bridge in
`POWERDIST_SendCmd @ 0x100020b4` and on our shim in
`bridge_encode_escape` / `bridge_decode_escape` in `shim_nodebus.c`.

Logical frame (pre-escape):
```
STX | cmd | id | data... | csum | ETX
```
where `csum = -sum(cmd, id, data...)` modulo 256.

## Game's outbound cmd usage (latest run log)

From `_emulation/runs/latest/game.log` distinct cmd codes:

`0x10  0x11  0x15  0x16  0x18  0x1c  0x1d  0x27  0x30`

(9 unique. Most are POWERDIST-range; LCD `0x27` for the LCD-node setup;
CRC32 `0x30` for the bridge firmware integrity check.)

**Cmd `0x40` and `0x41` are sent at runtime** via `mcp_nodebus_send_raw @ 0x9166c0`,
which computes the cmd byte as `(arg1[0] >> 7) + 0x40`. The encrypted variant
(cmd 0x41 path) carries the node firmware-flash channel and 160+ per-(slot, cmd)
wrappers — see [`NODEBUS_CIPHER.md`](NODEBUS_CIPHER.md). Earlier "never observed"
note for cmd 0x40 was based on a static `mov w0, #0x40` immediate scan that the
runtime-computed senders bypass; it stood for less than a day before being
superseded.

## Bridge dispatch matrix

`HOST_Update @ 0x10001210` reads frames via `HOSTUART_GetMessage`, then:

1. **Pre-switch check 1**: if `POWERDIST_IsSpike3() == 1` AND `cmd ∈ [0x10,0x1F]`
   → tail-call `POWERDIST_SendCmd(cmd, id, data, len)`. No bridge-side
   response (powerdist replies async on POWERDISTUART; bridge forwards back).
2. **Switch on `cmd - 0x20`** for cmds `0x20-0x2F`.
3. **Range check `cmd - 0x20 == 0x10`** for cmd `0x30`.
4. **Range checks `cmd - 0x20 == 0x20` / `0x21`** for cmds `0x40` / `0x41`.
5. **Post-switch fallbacks** for cmds `0x10 / 0x12 / 0x15 / 0x19`
   (Spike 2 powerdist substitutes when `pdstate < 5`).

### Top-level cmd map

Legend for **Status**: ✅ = handled with deliberate engineered shape; ⚠️ = handled but shape may not match the bridge's real reply (see notes); ❓ = uncovered, but the game has been observed sending this cmd; ❌ = uncovered and never observed in any run.

| Cmd  | Handler @ bridge | Reply shape | Status (shim) |
|------|------------------|-------------|---------------|
| `0x10`-`0x1F` | POWERDIST_SendCmd passthrough (when Spike 3) | async via POWERDISTUART | n/a — see per-cmd entries below |
| `0x10` | local fallback: ADC version triple + 4th byte | `SendMessage(0x10, id, 3 bytes)` | ✅ shim sends 7-byte pwrdist version (response cmd `0x10`); shape locked-in by cross-RE of the game's dispatch handler |
| `0x11` | POWERDIST passthrough (Spike 3); local fall-through (Spike 2) | undefined locally | ✅ shim sends 0-byte ACK (matches generic silent-ACK fallback at firmware 0x915d28) |
| `0x12` | `ADC_GetLineFrequency` | `SendMessage(cmd, id, 1 byte)` | ❌ never observed in any run |
| `0x15` | local fallback: `0xff` ack | `SendMessage(0x15, id, 1 byte)` | ✅ shim sends 1-byte `0x00` (pwrdist status — "all clear") |
| `0x16` | POWERDIST passthrough; OR shared handler at 0x915d28 | game polls (`rx[0]>0 && rx[2]==0x02`) | ✅ shim sends `01 00 02 00` to satisfy the poll on first read |
| `0x18` | POWERDIST passthrough; firmware handler at 0x91607c records min(existing, len) | the value isn't read | ✅ shim sends 1-byte 0x00 |
| `0x19` | `ADC_GetLineVoltage` | `SendMessage(cmd, id, 2 bytes)` | ❌ never observed |
| `0x1c` | POWERDIST passthrough; shared silent-ACK at 0x915d28 | unblocks the channel | ✅ shim sends 0-byte ACK |
| `0x1d` | POWERDIST passthrough; dedicated handler at 0x916028 (path depends on len) | varies | ✅ shim sends 0-byte ACK (takes the else branch — avoids writing stale data into per-id buffer) |
| `0x20` | bridge version triple (strtol over rodata) + ADC version | `SendMessage(0x20, id, 4 bytes)` | ⚠️ shim returns 4 zero bytes WITH RESPONSE CMD `0x10` — diverges from the bridge's actual reply (which uses cmd `0x20`). Possibly intentional placeholder; flagged for re-RE. |
| `0x21` | `BBL_Set` / `BBL_Get` (battery backup level) | `SendMessage(cmd, id, 1 byte)` | ❌ never observed |
| `0x22` | `resetFlags` (also sent unsolicited at boot) | `SendMessage(0x22, 0, 1 byte)` | ❌ never observed (but should be sent unsolicited at shim init — see TODO) |
| `0x23` | snapshot+clear `bridgeStatus` | `SendMessage(0x23, id, 4 bytes)` | ❌ never observed |
| `0x24` | `ADC_GetVBBmV` (battery voltage) | `SendMessage(cmd, id, 4 bytes)` | ❌ never observed |
| `0x25` | `NODEBUS_Poll` | `SendMessage(cmd, id, 1 byte)` | ❓ never observed in this run, but inline shim comment notes "cmd 0x25 / 0x41 silent-ACK reverted" — earlier attempt to handle it broke something; likely a key cmd to revisit. |
| `0x27` | LCD subcommands (sub-cmd at byte 0) | varies — see LCD subprotocol | ✅ shim alternates `0x06` / `0x02` per call to satisfy a paired bit-2-toggle poll loop in the game |
| `0x28` | DMD: GetStatus / TestPins / WriteData | varies | ❌ never observed |
| `0x29` / `0x2A` / `0x2B` / `0x2F` | WS2812 ops | varies | ❌ never observed |
| `0x2C` | `I2S_Enable(arg & 1)` | none | ❌ never observed |
| `0x30` | `CRC32(0, &__VECTOR_TABLE, 0x12a40)` over bridge firmware | `SendMessage(0x30, id, 4 bytes)` | ✅ shim returns precomputed CRC32 of `netbridge.elf` `.text+.rodata` — only handler that returns the *actually-correct* runtime value |
| `0x40` | nodebus-passthrough subprotocol (cleartext path: bridge sub-cmds, see below) | varies | ✅ **sent at runtime** via `mcp_nodebus_send_raw @ 0x9166c0` — cmd byte computed as `(arg1[0] >> 7) + 0x40`, so the static-immediate scan that called it "dead" missed it. Senders include `mcp_nodebus_query_bridge_status` (sub-cmd 0x05) and `mcp_nodebus_factory_test_trap` (sub-cmd 0x0c). |
| `0x41` | node-bus query, encrypted transport — see [`NODEBUS_CIPHER.md`](NODEBUS_CIPHER.md) | varies | ✅ **sent at runtime, encrypted**, via `mcp_nodebus_send_encrypted @ 0x8ec410` → `mcp_nodebus_send_raw`. 160+ wrappers around `mcp_nodebus_send_with_retry @ 0x8ed4a0` cover every (slot, inner_cmd) pair the game sends — node firmware flashing, key fetching, normal node communication. Shim's "timeout-is-progress" still works because the senders tolerate timeouts. |

### Cmd `0x27` — LCD subprotocol

Sub-cmd at `data[0]`:

| Sub | Op |
|-----|----|
| 0 | `LCD_GetStatus` → 1-byte status |
| 1 | `LCD_Reset` → 1-byte status |
| 2 | `LCD_WriteImage(data[1] - 1)` → 1-byte status |
| 3 | `LCD_WriteBuffer(data[1], &data[2])` |
| 4 | `LCD_Halt` → `LCDBL_Set(data[1], halt_result)` |
| 5 | `LCDBL_Set(data[1], halt_result)` |

### Cmd `0x28` — DMD subprotocol

Disambiguated by length:

| len | Op |
|-----|----|
| 0   | `DMD_GetStatus` → 1-byte status |
| 1   | `DMD_TestPins(data[0])` |
| 0x81 | `DMD_WriteData(data[0], &data[1])` |

### Cmd `0x29` / `0x2A` / `0x2B` / `0x2F` — WS2812 LED chains

Disambiguated by len + sub-cmd at `data[0]`:

| Cmd | len | Op |
|-----|-----|-----|
| `0x29` | 3 | `WS2812_SetNumLEDs(chain, n, 0)` |
| `0x29` | 9 | `WS2812_SetNumLEDs(chain, 0, &per_segment_lengths)` |
| `0x29` | 1 | `WS2812_Debug(chain)` |
| `0x2A` | varies | `WS2812_SendData(chain, data, len)` |
| `0x2B` | varies | `WS2812_SetLEDPattern(chain, range, pattern, len, count, period)` |
| `0x2F` | varies | `WS2812_NodeSetLED(payload)` |
| `0x2A` (alt) | varies | `WS2812_SetLEDs(chain, range, payload, len, count)` |

### Cmd `0x40` — nodebus-passthrough subprotocol

Sub-cmd at `data[0]`. All replies use `SendMessage(0x40, id, ...)`.

| Sub  | Op | Reply |
|------|----|-------|
| `0x00` | `NODEBUS_Poll` | 1 byte |
| `0x03` | bridge version triple (3 bytes from rodata) | 3 bytes |
| `0x04` | `NODE_SendBreak(data[2] ? 1 : 0)` | none |
| `0x05` | snapshot+clear `bridgeStatus` | 4 bytes |
| `0x06` | configure response timeout: `respTimeus = (data[2..3] LE) * 1000`, `respCnt = 1` | none |
| `0x09` | `BBL_Set(data[2], data[3] << 4, 0)` (when len==2) | none |
| `0x0a` | `BBL_Get` | 2 bytes (`{0x00, bbl}`) |
| `0x0c` | infinite loop (factory test trap / watchdog test) | never returns |
| `0x0d` | **NODEBUS_Hash** (4 secret seeds, 5 sub-sub-cases) | 4 bytes |

### Cmd `0x41` — Node-bus query (the real node communication path)

**This is the wrapper around all actual node-bus communication.**
Confirmed via `HOSTUART_GetMessage @ 0x100029dc`: when the bridge sees a
host frame whose cmd byte is `0x41`, it activates `nodeEcho` mode and
**transparently forwards the payload bytes to the node bus while parsing
the host frame**. Then `HOST_Update` reads the node's reply back.

Wire format (host → bridge):
```
STX | 0x41 | id | node_addr | length | tx_bytes[length] | rx_length | csum | ETX
```

What the bridge does as bytes arrive:
1. Byte `0x41`: enter nodeEcho mode (`HOSTUART_GetMessage:0x10002a7c`).
2. `id` byte (rxIdx==1): just stash; don't forward to node bus.
3. `node_addr` byte (rxIdx==2): `NODEUART_WriteAddress(node_addr)` — raises the bus "address" line so the multi-drop node bus can target one node.
4. `length` byte (rxIdx==3): `NODEUART_Write(length)`, set echoLen = length + 1.
5. Each subsequent payload byte: `NODEUART_Write(byte)` until echoLen exhausted.
6. After echoLen exhausted: `NODEUART_SendEnd` to terminate the bus frame.

Then `HOST_Update`'s cmd `0x41` handler runs:
1. Compute expected rx_length from `data[length+2]`.
2. `memset` response buffer with `0x55` (placeholder).
3. `UART_NodeGetMessageBlock(buf, rx_length - 1, NULL)` — block-read from the node bus with bounded timeout (`HOST_GetResponseTime() + nodeRxBitTime` per byte).
4. Append `bridgeStatus` as the last byte.
5. `HOSTUART_SendMessage(0x41, id, buf, rx_length)` — return to host.

Wire format (bridge → host):
```
STX | 0x41 | id | rx_bytes[rx_length-1] | bridgeStatus | csum | ETX
```

**Implication for "switch 57 was triggered":** the actual switch-state
encoding is determined by the **node firmware** (e.g. `pinnode-LPC1124_303-X.Y.Z.hex`,
`coil4node-...hex`). The bridge is just a transparent UART relay for the
node-bus framing — it doesn't know what "switch 57" means. To decode
those events we need to RE the node firmware, or capture wire traffic
on a real machine and pattern-match against switch closures.

The game-side is the higher-leverage end — its handler for cmd `0x41`
responses (somewhere downstream of `sub_d15aa8` per PROTOCOL.md) will
parse the node bytes and update the in-memory switch-state array at
`game_base + SHIM_SWITCH_STATE_BASE` (= ELF VA `0x4480220`). That
parsing logic *is* the spec for the node-bus message format.

## Async (unsolicited) bridge → host notifications

The bridge emits these on its own — the host doesn't request them.

| Cmd  | Trigger | Payload | Source |
|------|---------|---------|--------|
| `0x22` | Sent once at bridge boot, before the main loop starts | 1 byte `resetFlags` (from `watchdog_caused_reboot()`) | `main @ 0x10001e3c` line `0x10001ed6` |
| `0x2D` | Sent whenever HOST_XOFF or NODE_XOFF flow-control state changes (checked at the top of every `HOST_Update`) | 1 byte: `host_xoff | (node_xoff << 1)` | `HOST_Update` lines `0x10001214-0x10001242` |

The shim must accept these even though no host cmd was sent — they're how the
bridge initially announces itself ("I just rebooted, here's why") and signals
ongoing flow-control state. Shim should *probably* synthesize the boot `0x22`
once after the host's UART socketpair is connected, to mimic that handshake.

## Local fallback handlers (post-switch checks)

These match cmds `0x10`/`0x12`/`0x15`/`0x19` only when the main switch
didn't already dispatch (i.e. when `POWERDIST_IsSpike3() != 1` so the
passthrough doesn't fire, and the cmd byte happens to be one of these
four). For our shim this is the natural model — there's no real powerdist
board for the bridge to forward to.

| Cmd | Reply contents | Length |
|-----|----------------|--------|
| `0x10` | `{strtol("0",,0), strtol(@0x10011f70,,0), strtol(@0x10011f74,,0)}` (likely a 3-byte version triple) | 3 |
| `0x12` | `min(ADC_GetLineFrequency, 0xff)` clamped | 1 |
| `0x15` | `0xff` (acknowledge stub) | 1 |
| `0x19` | `clamp(ADC_GetLineVoltage, 0..0xffff)` | 2 |

The shim should synthesize these to keep the host's polling loop happy,
since `POWERDIST_SendCmd` would otherwise produce no synchronous reply
under our (no-powerdist-board) emulation.

## Specific divergences worth re-checking

- **`0x20` reply uses response cmd `0x10`** (not `0x20`). The game's dispatch
  computes `idx = CMD - 0x10`, so this routes to slot 0 instead of slot
  0x10. May be intentional alignment with a different game-side handler,
  but worth verifying against the bridge's actual reply (which uses CMD
  `0x20`).
- **`0x25` (NODEBUS_Poll) and `0x41` (NODE block read) silent-ACK revert.**
  An earlier attempt to silent-ACK these caused unspecified breakage; the
  shim now lets them time out. These are high-value to understand because
  `0x41` in particular is the most likely vehicle for asynchronous
  node-bus events surfacing to the host (switches, coils).

## TODO / open questions

Tracked here so we don't lose the thread between sessions. Roughly priority-ordered.

### High value (path to "switch N was triggered")
- [x] ~~Decompile cmd `0x41` handler~~ — DONE. In netbridge: cmd 0x41 is the node-bus wrapper; bridge transparently relays payload bytes to NODEUART while parsing the host frame, then reads back via `UART_NodeGetMessageBlock`. In the game-side dispatcher (`sub_915aa8` case 0x41): memcpys the response into the caller's per-id buffer.
- [x] ~~Decompile `NODEBUS_Poll`~~ — DONE. Flow-control probe (binary-search of free TX buffer space), not switch state.
- [x] ~~Find game-side cmd `0x41` senders~~ — **DONE.** ~~Surprising negative result~~ — that conclusion was **wrong**. The static `mov w0, #0x41` immediate scan missed the senders because the cmd byte is **runtime-computed**: `mcp_nodebus_send_raw @ 0x9166c0` builds it as `(arg1[0] >> 7) + 0x40`. The actual sender chain is `mcp_nodebus_send_encrypted @ 0x8ec410` → `mcp_nodebus_send_raw @ 0x9166c0` → `send_bridge_message_with_response @ 0x9164a0`. The encrypted path has 160+ templated callers via `mcp_nodebus_send_with_retry @ 0x8ed4a0`, including the node firmware flasher (`mcp_nodebus_flash_node_firmware @ 0x8f5484`) and the per-slot cipher-key fetchers (`mcp_nodebus_fetch_key_basic/_with_serial/_two_probes`). See [`NODEBUS_CIPHER.md`](NODEBUS_CIPHER.md).
- [ ] **Switch-state update path** — Now that we know cmd 0x41 IS the live encrypted node-bus channel, switch state may travel here. Trace the response side: where do callers of `mcp_nodebus_send_with_retry` write into `g_switch_state_array @ 0x4480220`? Original reference points still apply for the writer hunt in the `0x650xxx` "switch driver" cluster.
  - Switch state lives at `g_switch_state_array @ 0x4480220` (BN-named); 40+ xrefs all in `0x650xxx` (the "switch driver" cluster).
  - `read_switch_state @ 0x650884` is a pure reader (`*(arr + sw*0x30 + 0x28)`); we need the WRITER.
  - The writer is presumably called from a polling loop that reads bridge responses out of a per-id buffer (`data_450e650[id]`) and decodes them into individual switch slots.
  - **Trace path:** decompile a `sub_650xxx` that *writes* to the array (e.g. `sub_650f00`, `sub_6511c0`, `sub_651370` — multiple xrefs each). Find what data it reads. That'll identify which bridge cmd carries switch updates.
  - Candidates by shape: cmd `0x1d` (4-byte per-id), cmd `0x27/0x28` (variable per-id buffer), cmd `0x12` (single-byte hw input), cmd `0x18` (per-id length cache + paired data cmd). All five are sent by the game per the static scan.
- [ ] **Catalog the game's static cmd-byte loads** — done as part of this session. 28 distinct cmds load constants in the `0x916000-0x91a000` cluster (the bridge-protocol caller cluster). Documented in next section.
- [ ] **RE the node firmware** for node-bus message format. Lower priority now since the host-side parser is the practical spec — and the host doesn't even open the cmd 0x41 channel in this build.

### Game's static cmd-load catalog (from full-binary scan against `send_bridge_message_with_response`)

Cmds with `mov w0, #N` callsites in `0x916xxx-0x919xxx` (the bridge-caller cluster). Counts in parentheses. Not all sites necessarily call into `send_bridge_message_with_response` directly — some are wrapper-internal — but the total count gives a sense of how busy each cmd is in the static surface.

| Cmd | Sites | Notable named sender |
|-----|-------|----------------------|
| 0x10 | 9 | (pwrdist version) |
| 0x11 | 7 | |
| 0x12 | 2 | |
| 0x13 | 1 | |
| 0x14 | 1 | |
| 0x15 | 2 | `read_hardware_input_state` (one of the two) |
| 0x16 | 3 | |
| 0x17 | 1 | |
| 0x18 | 8 | (pwrdist flash chunk write) |
| 0x19 | 6 | |
| 0x1b | 3 | |
| 0x1c | 2 | |
| 0x1d | 3 | |
| 0x1e | 1 | |
| 0x20 | 5 | |
| 0x21 | 3 | |
| 0x24 | 1 | |
| 0x25 | 2 | |
| 0x27 | 9 | |
| 0x28 | 4 | |
| 0x29 | 3 | |
| 0x2a | 1 | |
| 0x2b | 1 | |
| 0x2c | 1 | |
| 0x2e | 1 | |
| 0x2f | 1 | |
| 0x30 | 1 | (bridge firmware CRC) |
| 0x31 | 1 | |
| **0x40** | **runtime** | ✅ live — cmd byte built dynamically by `mcp_nodebus_send_raw`, missed by static-immediate scan |
| **0x41** | **runtime** | ✅ live — same path, encrypted; see `NODEBUS_CIPHER.md` |

**Implications for the shim:**
- Cmd 0x40/0x41 ARE sent (corrected). The shim's current "timeout-is-progress" path still works because the encrypted senders tolerate timeouts, but properly handling them is a follow-up — see `NODEBUS_CIPHER.md` for the wire format and the cipher recipe.
- The "timeout-is-progress" theory definitively holds for cmds outside the game's static-load set; for cmds inside it (28 of the 33 dispatchable values) the response shape matters and the shim does have engineered handlers for the 9 actually-observed ones.
- "Switch 57 was triggered" goes through a non-0x41 path. Best lead: trace the writer side of `g_switch_state_array` in the `0x650xxx` cluster.

### Medium — protocol-shape disambiguation
- [ ] **Re-RE the `0x20` reply shape** in the shim — does it really need response cmd `0x10`, or is that a stale placeholder? The bridge sends `SendMessage(0x20, id, 4 bytes)` (response cmd `0x20`).
- [ ] **Capture & log raw `data` payloads** for all 9 game-observed cmds in `shim_nodebus.c`. We currently log first-of-cmd + sample cadence, but raw bytes per call would let us see the sub-cmd bytes inside `0x40` / `0x41` / `0x27` and disambiguate the actual sub-protocol the game is using.
- [ ] **Spec out cmd `0x27` LCD subcommands** the game actually sends — currently we alternate a magic `0x06`/`0x02` byte but don't act on `WriteImage`/`WriteBuffer`/etc. requests.

### Lower — surface area not currently exercised
- [ ] **Implement unsolicited boot `0x22` notification** at shim init. Mimics the bridge's "I just rebooted, here's why" message that the host expects on first connection. May matter for a feature path we haven't reached.
- [ ] **Decompile what `POWERDIST_SendCmd` would produce for `0x11`/`0x16`/`0x18`/`0x1c`/`0x1d` response-wise.** The bridge forwards transparently to the powerdist board; we'd need a powerdist firmware dump (not currently in this repo) to know its replies. Game seems happy with current ACK shapes — only chase if we find a feature that needs powerdist-shaped data.
- [x] ~~**Spec out cmd `0x40 / 0x0d` NODEBUS_Hash usage** in the game.~~ **Resolved (Step 65, 2026-04-27).** The polynomial is **not** inlined in the Pokemon LE 0.81 game binary in any form we can detect: zero co-located `LSL #15` + `SUB` shift-pair signatures, zero `SUB Wd, Wn, Wn, LSL #15` self-shifts, only one `SUB-LSL-#11` self-shift (in an unrelated audio saturation routine `sub_1042214`). Combined with the existing static-cmd-byte negative result for cmd `0x40` senders, this game build does **not** call the bridge's NODEBUS_Hash and does **not** compute the polynomial on the CPU side either. The hash machinery is effectively dormant in this title. (Side note: the doc's stated polynomial constants were also wrong — see polynomial subsection above.)
- [x] ~~**Identify the cmd `0x40 / 0x0c` infinite-loop test path**~~ — **Resolved.** Caller is `mcp_nodebus_factory_test_trap @ 0x8f5f4c`; sends frame `{0x0c, 0x00, 0x00}` (cleartext, no response expected). Should be guarded by a factory-mode predicate before being relied on as benign — verify before any new shim work that hooks cmd 0x40.
- [ ] **Implement async `0x2D` flow-control notification** if/when XOFF backpressure becomes relevant. Currently the shim sends responses synchronously and uses unbounded buffers, so flow control isn't observed.

## Sources

- `spike3/netbridge/netbridge.elf` (32-bit ARM, statically linked, debug_info present, .bndb under BN view 11)
- `_emulation/runs/latest/game.log` — game's actual cmd usage in the latest run
- `_emulation/shim_nodebus.c` — current shim coverage
- BN renames in netbridge.elf: original symbol names from debug info; no `mcp_` prefix needed since they're authoritative

## Document history

- 2026-04-27 — Initial draft. Captured full dispatch matrix from `HOST_Update`, `POWERDIST_SendCmd` passthrough behavior, `POWERDIST_IsSpike3` predicate, NODEBUS_Hash mechanism. Open TODOs queued.
- 2026-04-27 — Reconciled shim coverage status — earlier "9 cmds unsynthesized" claim was wrong; all 9 game-observed cmds are handled with carefully-engineered shapes that target game-side poll loops. Added async (unsolicited) notification table (`0x22` boot, `0x2D` flow control). Added two specific divergences: `0x20` reply uses response cmd `0x10` (suspicious), `0x25`/`0x41` silent-ACK was tried + reverted.
- 2026-04-27 — **Cracked cmd `0x41` semantics.** It's the node-bus wrapper: `HOSTUART_GetMessage` parses host frames inline and transparently relays payload bytes to `NODEUART_Write` when cmd is `0x41`. Bridge is just a UART relay for node-bus traffic; switch/coil/sensor encodings live in the node-MCU firmware. `NODEBUS_Poll` is a flow-control probe, not a switch-state query.
- 2026-04-27 — ~~Surprising negative result: game never sends cmd `0x40` or `0x41`.~~ **SUPERSEDED — see entry below.** Original conclusion based on a static `mov w0, #0x4{0,1}` immediate scan; it missed the senders because the cmd byte is runtime-computed.
- 2026-04-27 — **Cmd 0x40 and 0x41 are alive — encrypted nodebus channel discovered.** `mcp_nodebus_send_raw @ 0x9166c0` builds the cmd byte as `(arg1[0] >> 7) + 0x40`, so 0x40 (cleartext) or 0x41 (encrypted node-bus traffic) depending on the high bit of the input frame. The encrypted side is gated by `mcp_nodebus_send_encrypted @ 0x8ec410` which stream-ciphers the payload with per-slot 16-byte keys at `mcp_nodebus_cipher_keys @ 0x44fcfb8` (32 slots × 16 bytes), an alternating-parity header-derived mask, and a sum checksum. **Keys are fetched from each node at runtime via inner cmd 0xfc** (after a 12-byte preamble query via inner cmd 0xfd) — at boot the table is zero, so the very first key-fetch goes out with effective key=0 (cipher reduces to position-mask XOR), and the response key bytes travel cleartext on the wire. The cipher is outbound-only as far as we can see. Per-slot bookkeeping at `mcp_nodebus_per_slot_status @ 0x44fce38` (12-byte stride × 32) and `mcp_nodebus_send_counters @ 0x44fd1c0` (4-byte stride × 32). The encrypted channel is used for the node firmware flasher (`mcp_nodebus_flash_node_firmware @ 0x8f5484`) and 160+ other (slot, inner_cmd) wrappers. Full algorithm and hunting checklist in [`NODEBUS_CIPHER.md`](NODEBUS_CIPHER.md).
