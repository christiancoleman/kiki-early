# devbus — design scratchpad

Working notes for the host-side switch / driver / lamp observability + control bridge. Promote settled facts into PROTOCOL §12 once Phase 1 is verified.

## Goals

1. **Observability.** Real-time view of every switch's `(state, edge_counter)` so we can verify reads are clean (no noisy-wire flapping).
2. **Toggle.** Host-driven switch press/set, callable from the Tk test panel and from a Windows-side COM consumer.
3. **Subscribe model.** Clients connect, get a snapshot, then receive deltas. No polling on the consumer side.
4. **Latency.** Sub-frame visibility at 60-1000 Hz consumer cadence. Toggle-to-state-write under a few ms.
5. **Long-term shippable.** Stable wire ABI for COM consumers.

## Non-goals (Phase 1)

- TCP listener (Phase 4)
- Toggle commands (Phase 3)
- UI (Phase 2)
- Drivers / lamps (Phase 5)

## Architecture

```
   game thread                          watcher thread (1 ms)              writer thread (per client)
       │                                       │                                       │
       │  writes via sub_651610                │  diff vs g_shadow                     │  drains ring,
       ▼                                       ▼                                       ▼  serializes JSON
  +0x4480220 → heap arr → sw_id*0x30      devbus_emit() ──► per-client SPSC ring ──► AF_UNIX SOCK_STREAM
```

The watcher is the **switch event source**. It's swappable: if 1 ms sampling ever proves insufficient, replace it with a hook on `sub_651610` that calls the same `devbus_emit()`. The downstream pipeline (rings, writers, JSON, wire) doesn't change.

## Wire protocol

Line-delimited JSON over AF_UNIX abstract `@pokemon_spike3_devbus`. Future: same JSON over TCP `127.0.0.1:8347`.

### Server → client

| Event | When | Fields |
|---|---|---|
| `hello` | once on connect | `tick_us`, `switch_count` |
| `snapshot` | once on connect, after hello | `switches: [{sw_id, state, edge, channel, bit_idx}]` |
| `switch_change` | on every state or edge transition | `sw_id, state, edge, flap` |

Common fields on every event: `v` (protocol version), `seq` (per-client monotonic), `event` (type tag), `ts_ns` (CLOCK_MONOTONIC).

### Client → server (Phase 3+)

| Command | Effect |
|---|---|
| `subscribe` | (implicit on connect — Phase 1 doesn't need this command, every client is a subscriber) |
| `switch_set` | `{sw_id, state}` — write state byte directly via the canonical helper |
| `switch_press` | `{sw_id, ms}` — momentary press, same path as `shim_manual_press_thread` |
| `snapshot` | request a fresh snapshot mid-stream |

### Versioning

`v` field on every line. Bumped on breaking changes. Phase 1 is `v: 1`. Adding new event types or new fields without removing existing ones does **not** bump the version.

### Sequence numbers

`seq` is per-client, starts at 0 at the `hello` event, increments by 1 per emitted line. Subscribers detect lost events by gap detection. Drops happen on ring overflow (slow client) and are reported as a `drops` count on disconnect.

## Internal data layout

```c
typedef struct {
    uint8_t  state;
    uint16_t edge_counter;
    uint8_t  channel;
    uint16_t bit_idx;
    uint8_t  valid;
    uint64_t flap_window_start_ns;
    uint16_t flap_count_this_window;
    uint16_t flap_count_last_window;
} sw_shadow_t;
```

`g_shadow[256]` holds one record per possible `sw_id`. The watcher's first pass sets `valid=1` for every `recorded sw_id == sw_id` slot; subsequent passes diff state/edge.

Flap counter rolls in 1 s windows. `flap_count_last_window` is what gets reported to subscribers (so a client gets a stable per-second flap rate, not a counter that resets while they look at it).

## Constraint: single switch-write helper

User-imposed: **all switch-write paths route through one shared helper** to preserve edge/state semantics in one place. Today that helper is `shim_press_one_sw_id` in `shim_inject.c`. When Phase 3 adds toggle from the devbus command channel, the command handler will call that helper (after potentially exposing it via `spi_shim.h`). Watcher is read-only and does not write.

## Constraint: TCP binds 127.0.0.1

User-imposed: when Phase 4 adds the TCP listener, it binds to `127.0.0.1` not `0.0.0.0`. WSL2 mirrored networking forwards localhost between the VM and Windows; legacy NAT mode requires the WSL VM IP from Windows but TCP itself works either way.

## Phase plan

1. **Phase 1 (now)** — observability only. Watcher + listener + AF_UNIX bridge + JSON snapshot/delta. Minimal Python subscriber for verification.
2. **Phase 2** — Tk panel showing switch grid in real-time, color-coded by state, with flap indicator.
3. **Phase 3** — `switch_set` / `switch_press` commands. Routes through `shim_press_one_sw_id`.
4. **Phase 4** — TCP listener on 127.0.0.1:8347, same JSON wire.
5. **Phase 5** — drivers / lamps. Event-driven from `shim_nodebus.c` outbound cmd 0x41 observation, calls `devbus_emit_*` on the same ring infrastructure.

## Open questions

- **`g_shadow` first-pass population race.** Watcher discovers valid sw_ids on its first pass. A client connecting before the first pass completes gets an empty snapshot. Phase 1 mitigation: watcher does its first pass before the listener thread is spawned. If we miss a few sw_ids on first pass (game still booting), they show up as `switch_change` events as the watcher sees them.
- **Game restart** invalidates the heap pointer at `0x4480220`. Watcher re-reads `*arr_pp` every iteration so this is automatic — across a restart we see "everything changed" deltas as the new array gets discovered.
- **Ring overflow policy.** Drop oldest. Could also pause the watcher per-slow-client, but that hurts other clients. Drop-oldest with `seq` gap detection is cleaner.

## Things deliberately not built

- Hooking `sub_651610` (oversold the benefit; 1 ms sampling is sufficient for this workload — see conversation 2026-05-10).
- Binary protocol (JSON is fine at our event rates; binary becomes worth it above ~1000 events/sec which we won't hit).
- Shared mmap state region (doesn't cross WSL2/Windows boundary; defeats the COM use case).
- vsock transport (locks architecture to specific Hyper-V config).

## Risk register

| Risk | Mitigation |
|---|---|
| Slow client stalls watcher | Per-client SPSC ring + drop-oldest. Watcher never blocks on socket I/O |
| `g_shadow` race vs first snapshot | Watcher does first pass synchronously before listener accepts |
| `sw_struct + 0x20` recorded sw_id mismatch (uninitialized slot) | Skip per-iteration; matches `shim_press_one_sw_id` pattern |
| Game restart heap pointer invalidation | Re-read `*arr_pp` each iteration |
| Multiple writers to switch state | All toggle paths must call `shim_press_one_sw_id` (Phase 3 enforcement) |
