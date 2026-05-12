#!/usr/bin/env python3
"""Append a switch command for the shim manual-input watcher.

This is intentionally UI-free so it can be called from WSL, Windows Python,
PowerShell, AutoHotkey, or a future COM wrapper. It writes the same command
format consumed by shim_manual_press_thread:

    down <runtime_sw_id>
    up <runtime_sw_id>
    tap <runtime_sw_id>
"""

from __future__ import annotations

import argparse
import datetime as _dt
import os
from pathlib import Path

from switch_inventory import resolve_switch

try:
    import fcntl
except ImportError:  # Windows
    fcntl = None

try:
    import msvcrt
except ImportError:  # POSIX
    msvcrt = None


_HERE = Path(os.environ.get("SPIKE3_EMULATION_DIR", Path(__file__).parent)).absolute()
_DEFAULT_REQUEST = _HERE / "runs" / "latest" / "press_request.txt"
_DEFAULT_GAME_LOG = _HERE / "runs" / "latest" / "game.log"


def _resolve_latest_path(path: Path) -> Path:
    """Map runs/latest/... to the current concrete run dir.

    WSL creates runs/latest as a symlink, but Windows callers can hit WinError
    183 when they try to create or append below that symlink. run_game.sh also
    writes runs/latest_path.txt as a normal file for Windows-friendly lookup.
    """
    parts = path.parts
    for idx, part in enumerate(parts[:-1]):
        if part == "runs" and idx + 1 < len(parts) and parts[idx + 1] == "latest":
            runs_dir = Path(*parts[:idx + 1])
            latest_path = runs_dir / "latest_path.txt"
            try:
                run_dirname = latest_path.read_text(encoding="ascii").strip()
            except OSError:
                try:
                    run_dirname = os.readlink(runs_dir / "latest")
                except OSError:
                    run_dirname = ""
            if not run_dirname:
                try:
                    candidates = [p for p in runs_dir.iterdir() if p.is_dir() and p.name != "latest"]
                except OSError:
                    return path
                if not candidates:
                    return path
                candidates.sort(key=lambda p: p.stat().st_mtime, reverse=True)
                run_dirname = candidates[0].name
            return runs_dir / run_dirname / Path(*parts[idx + 2:])
    return path


def _lock_file(f):
    if fcntl is not None:
        fcntl.flock(f.fileno(), fcntl.LOCK_EX)
    elif msvcrt is not None:
        f.seek(0, os.SEEK_END)
        msvcrt.locking(f.fileno(), msvcrt.LK_LOCK, 1)


def _unlock_file(f):
    if fcntl is not None:
        fcntl.flock(f.fileno(), fcntl.LOCK_UN)
    elif msvcrt is not None:
        try:
            f.seek(0, os.SEEK_END)
            msvcrt.locking(f.fileno(), msvcrt.LK_UNLCK, 1)
        except OSError:
            pass


def now_timestamp() -> str:
    n = _dt.datetime.now()
    return f"{n:%Y/%m/%d - %H:%M:%S}.{n.microsecond // 1000:03d}"


def append_locked(path: Path, text: str) -> None:
    path = _resolve_latest_path(path)
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
    except FileExistsError:
        if not path.parent.exists():
            raise
    with path.open("a+", encoding="ascii") as f:
        _lock_file(f)
        try:
            f.write(text)
            f.flush()
            os.fsync(f.fileno())
        finally:
            _unlock_file(f)


def main() -> int:
    ap = argparse.ArgumentParser(description="Send a switch down/up/tap command to the running shim.")
    ap.add_argument("action", choices=("down", "up", "tap"))
    ap.add_argument("switch", help="manual ID like C9/96, address like 1-SW-16, or runtime id with --runtime")
    ap.add_argument("--runtime", action="store_true", help="interpret switch as runtime sw_id")
    ap.add_argument("--ms", type=int, default=120, help="tap hold duration in milliseconds")
    ap.add_argument("--request-path", default=os.environ.get("SHIM_PRESS_REQUEST_PATH", str(_DEFAULT_REQUEST)))
    ap.add_argument("--game-log", default=os.environ.get("SPIKE3_GAME_LOG_PATH", str(_DEFAULT_GAME_LOG)))
    ap.add_argument("--no-log", action="store_true")
    args = ap.parse_args()

    manual_id, address, sw_id, name, _conn, _location, _switch_type = resolve_switch(
        args.switch, args.runtime
    )
    if args.action == "tap":
        command = f"tap {sw_id} {args.ms}\n"
    else:
        command = f"{args.action} {sw_id}\n"
    append_locked(Path(args.request_path), command)

    if not args.no_log:
        line = (f"{now_timestamp()} - [external_switch] "
                f"{args.action.upper()} {manual_id} {address} {name} runtime_sw_id={sw_id}\n")
        append_locked(Path(args.game_log), line)

    print(f"{args.action} {manual_id} {address} {name} -> runtime sw_id {sw_id}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
