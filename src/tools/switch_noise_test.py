#!/usr/bin/env python3
"""Post-run devbus switch-noise and injection cross-talk check.

Reads a run directory's devbus.log and fails when one observed timestamp
contains changes for multiple watched switches. That is the signature seen
when a host-side press for one remote switch was translated with a sparse
zero-filled channel buffer, causing sibling switches on the same channel to
toggle too.

Usage:
    python3 switch_noise_test.py
    python3 switch_noise_test.py --run-dir runs/2026-05-10T13.03.18
    python3 switch_noise_test.py --watch 77-98
"""
from __future__ import annotations

import argparse
import json
import sys
from collections import defaultdict
from pathlib import Path


DEFAULT_WATCH = "77-98"
DEFAULT_FLAP_WARN_LIMIT = 1


def parse_watch(spec: str) -> set[int]:
    out: set[int] = set()
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            lo_s, hi_s = part.split("-", 1)
            lo, hi = int(lo_s), int(hi_s)
            if hi < lo:
                raise ValueError(f"bad range {part!r}")
            out.update(range(lo, hi + 1))
        else:
            out.add(int(part))
    return out


def load_switch_changes(path: Path) -> list[dict]:
    events: list[dict] = []
    with path.open("r", encoding="utf-8", errors="replace") as f:
        for line_no, line in enumerate(f, 1):
            line = line.strip()
            if not line.startswith("{"):
                continue
            try:
                msg = json.loads(line)
            except json.JSONDecodeError as e:
                raise SystemExit(f"{path}:{line_no}: JSON parse failed: {e}") from e
            if msg.get("event") == "switch_change":
                events.append(msg)
    return events


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--run-dir", default="runs/latest",
                    help="run directory containing devbus.log")
    ap.add_argument("--watch", default=DEFAULT_WATCH,
                    help="comma/range sw_id list to check")
    ap.add_argument("--flap-warn-limit", type=int, default=DEFAULT_FLAP_WARN_LIMIT,
                    help="warn when a watched switch reports flap above this")
    ap.add_argument("--fail-flap-limit", type=int, default=None,
                    help="also fail when a watched switch reports flap above this")
    ap.add_argument("--include-startup", action="store_true",
                    help="include the first watched timestamp cluster")
    ap.add_argument("--show-all", action="store_true",
                    help="print every watched switch_change event")
    args = ap.parse_args()

    run_dir = Path(args.run_dir)
    if not run_dir.is_absolute():
        run_dir = Path(__file__).resolve().parents[2] / run_dir  # src/tools/x.py → _emulation/
    devbus_log = run_dir / "devbus.log"
    if not devbus_log.exists():
        raise SystemExit(f"missing {devbus_log}")

    watched = parse_watch(args.watch)
    events = load_switch_changes(devbus_log)
    watched_events = [e for e in events if int(e.get("sw_id", -1)) in watched]

    by_ts: dict[int, list[dict]] = defaultdict(list)
    for e in watched_events:
        by_ts[int(e["ts_ns"])].append(e)

    crosstalk = []
    grouped = sorted(by_ts.items())
    if grouped and not args.include_startup:
        grouped = grouped[1:]
    for ts_ns, group in grouped:
        ids = sorted({int(e["sw_id"]) for e in group})
        if len(ids) > 1:
            crosstalk.append((ts_ns, group))

    noisy_warn = [
        e for e in watched_events
        if int(e.get("flap", 0)) > args.flap_warn_limit
    ]
    noisy_fail = []
    if args.fail_flap_limit is not None:
        noisy_fail = [
            e for e in watched_events
            if int(e.get("flap", 0)) > args.fail_flap_limit
        ]

    print(f"[switch_noise_test] run_dir={run_dir}")
    print(f"[switch_noise_test] watched={sorted(watched)} events={len(watched_events)}")

    if args.show_all:
        for e in watched_events:
            print("  event "
                  f"seq={e.get('seq')} ts={e.get('ts_ns')} "
                  f"sw={e.get('sw_id')} state={e.get('state')} "
                  f"edge={e.get('edge')} flap={e.get('flap', 0)}")

    if crosstalk:
        print("[switch_noise_test] FAIL: same-timestamp watched switch clusters:")
        for ts_ns, group in crosstalk[:20]:
            summary = " ".join(
                f"sw{int(e['sw_id'])}:state={e.get('state')} edge={e.get('edge')} flap={e.get('flap', 0)}"
                for e in sorted(group, key=lambda x: int(x["sw_id"]))
            )
            print(f"  ts={ts_ns}: {summary}")
        if len(crosstalk) > 20:
            print(f"  ... {len(crosstalk) - 20} more clusters")

    if noisy_warn:
        print(f"[switch_noise_test] WARN: watched switches with flap>{args.flap_warn_limit}:")
        for e in noisy_warn[:20]:
            print("  "
                  f"seq={e.get('seq')} ts={e.get('ts_ns')} "
                  f"sw={e.get('sw_id')} state={e.get('state')} "
                  f"edge={e.get('edge')} flap={e.get('flap')}")
        if len(noisy_warn) > 20:
            print(f"  ... {len(noisy_warn) - 20} more noisy events")

    if noisy_fail:
        print(f"[switch_noise_test] FAIL: watched switches with flap>{args.fail_flap_limit}")

    if crosstalk or noisy_fail:
        return 1

    print("[switch_noise_test] PASS: no post-startup watched switch cross-talk")
    return 0


if __name__ == "__main__":
    sys.exit(main())
