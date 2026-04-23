# Event-ID Sweep Findings — Step 52 (2026-04-21)

Captured by `shim_event_sweep()` triggered via `SPIKE3_SWEEP_AFTER=35`. The sweep fires each event_id 0x00..0xce through `fire_event @ 0x9c12a0` with `event_data=0`, then diffs a minimal cmode state snapshot (+0x308 and +0x60 per cmode) before and after.

**Verdict up front:** blind sweeping is low-signal. Of 207 event IDs tested, only 2 produced observable cmode state changes on a single run. Most of the observed "crashes" are handlers that dereference `event_data` as a pointer — a SEGV with `event_data=0` just means the handler expected a valid struct, not that the event is broken. Moving to a switch-first API (Step 52 follow-up) to drive cmode transitions through valid paths rather than random pokes.

## Totals (run_step52e)

| Category | Count |
|---|---:|
| Fired cleanly (ret=1, Δ=0) | 164 |
| Reactive (Δ>0) | **2** |
| Crashed (SEGV from handler dereferencing event_data=0) | 37 |
| Timed out (handler did not return within 1 s) | 4 |
| **Total covered** | 207 |

## The 2 reactive events

### event 0x3e → activates `battle_coast`

```
event 0x3e ret=1 Δ=2  base_03(+0x308:0->0)  battle_coast(+0x308:0->100000000)
```

- `battle_coast` is the Pokémon LE mode `cmode_battle_coast` (mode_id 0x23, type_flags 0x48, scene_id 0x17). One of the four battle environment modes (Mountain / Forest / Desert / Coast).
- `+0x308` flip from `0` → `0x100000000` is a meaningful activation state transition (bit 32 set — treating the u64 at +0x308 as a subclass-interpreted field).
- `base_03` got "touched" (field changed then reverted to 0 within the event path).

Reproducibility: observed in both runs (run_step52c at 40 s, run_step52e at 35 s).

### event 0x43 → touches `base_03`

```
event 0x43 ret=1 Δ=1  base_03(+0x308:0->0)
```

No actual field change — the word was written back to its prior value. Marked as "changed" because the snapshot-compare is bitwise. Real-world effect: none visible at the +0x308 level; may have changed other fields we don't sample.

Reproducibility: observed in both runs.

## Events that were reactive on one run but not another

| Event | Run 52c (sweep @ 40 s) | Run 52e (sweep @ 35 s) |
|---|---|---|
| `0x26` | Δ=1 `base_03(+0x308:0->700)` | Δ=0 |

Timing-dependent — reactivity depends on whether a prior game-state transition has already fired. Reinforces that blind sweeps don't produce stable signals.

## Events that crash with `event_data=0`

37 events whose handlers immediately dereference `event_data` as a pointer. The fault address is usually NULL (0x0) or a small offset from NULL (0x1, 0x4, 0x8, 0xa) — the handler read a field via a fixed offset from a NULL base.

Known-crash list (from run_step52e):

```
0x00 0x01 0x03 0x05 0x06 0x08 0x0b 0x0c 0x12 0x13 0x14 0x17 0x1a 0x1e
0x22 0x29 0x30 0x56 0x58 + ~18 others past 0x60
```

These are not "broken" — they just need a valid `event_data` struct. Finding one requires tracing a caller of `fire_event` that uses that event_id (one of `sub_b129f0`, `sub_bd39f0`, `sub_b16f64`, `sub_b82990`, `sub_b12760` — BN Plan C in flight).

## Events that hang (handler doesn't return within 1 s)

4 events — at least one with a tight poll / wait loop. Broken out by the SIGALRM `alarm(1)` timeout. Exact IDs not useful to pin down right now; if Plan C identifies the switch translator, we'll know which events correspond to hardware-waiting paths and can filter them.

## Events fired cleanly with no observable state change

164 events completed without changing +0x308 or +0x60 of any cmode. Two interpretations, both plausible:

1. The event is meaningful but its effect is in a cmode field we don't sample, or in a non-cmode subsystem (lamps, coils, sound triggers).
2. The event is reserved-but-unused in Pokémon LE.

Given the manual's switch-ID gaps (3, 4, 13, 14, 24, 26, 32, 38, 39, 42, 43, 47, 48, 62, 65, 68, 69, 75, 77, 81-89, 91, 92, 95), Stern clearly allocates ranges with unused slots. The event_id space almost certainly has the same pattern.

## Why we're pivoting away from the sweep

- Signal-to-noise was ~1% (2 / 207).
- Crash recovery is working fine (sigsetjmp + SIGALRM), but it's scaffolding for a search strategy we should abandon.
- The right primitive is **switch-indexed**, not event-indexed. The manual enumerates every real switch in Pokémon LE; the translator (Plan C) converts switch closures into events. Once we can drive a named switch (`Left Flipper Button`, `Trough 1`, `Action Button`), we get stable, meaningful stimuli that correspond to things a player would actually do.

## Kept for reuse

- `shim_fire_event(event_id, event_data)` — the trampoline into `0x9c12a0`. Will be called by the switch API once the translator is understood (Plan C) or can be called directly for specific event probes.
- `shim_event_sweep()` — the code stays in `spi_shim.c`, gated behind `SPIKE3_SWEEP_EVENTS=1`. Useful later when we have valid `event_data` structs to try.
- The SIGALRM + sigsetjmp recovery harness — applies to any fire-and-see experiment, not just the sweep.

## Source runs

- `_emulation/run_step52c.stderr` — sweep @ 40 s, game exited before completion (got to 0x5c).
- `_emulation/run_step52d.stderr` — sweep @ 25 s, hung at 0x5d (before the alarm timeout was added).
- `_emulation/run_step52e.stderr` — sweep @ 35 s, completed 207/207 with alarm timeout in place. **Authoritative for the numbers above.**
