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


# manual_id, address, runtime_sw_id, name
_SWITCHES = [
    ("1", "8-SW-17", 17, "Left Outlane"),
    ("2", "8-SW-18", 18, "Left Return Lane"),
    ("5", "8-SW-19", 19, "Right Return Lane"),
    ("6", "8-SW-20", 20, "Right Outlane"),
    ("7", "8-SW-30", 21, "Left Slingshot"),
    ("8", "8-SW-29", 22, "Right Slingshot"),
    ("9", "8-SW-25", 23, "Left Flipper Button"),
    ("10", "8-SW-24", 24, "Right Flipper Button"),
    ("11", "8-SW-16", 25, "Left Flipper EOS"),
    ("12", "8-SW-31", 26, "Right Flipper EOS"),
    ("15", "8-SW-32", 27, "Trough 6"),
    ("16", "8-SW-33", 28, "Trough 5"),
    ("17", "8-SW-34", 29, "Trough 4"),
    ("18", "8-SW-35", 30, "Trough 3"),
    ("19", "8-SW-36", 31, "Trough 2"),
    ("20", "8-SW-37", 32, "Trough 1"),
    ("21", "8-SW-38", 33, "Trough Jam"),
    ("22", "8-SW-28", 34, "Shooter Lane"),
    ("25", "8-SW-27", 35, "Bowl Opto"),
    ("27", "8-SW-9", 36, "Berry Target Left"),
    ("28", "8-SW-10", 37, "Berry Target Center"),
    ("29", "8-SW-11", 38, "Left Ramp Enter Opto"),
    ("30", "8-SW-12", 39, "Left Ramp Made 2 Gate"),
    ("31", "8-SW-8", 40, "Subway Enter"),
    ("33", "8-SW-0", 41, "Lower Left Target-Top"),
    ("34", "8-SW-1", 42, "Lower Left Target-Mid"),
    ("35", "8-SW-2", 43, "Lower Left Target-Bot"),
    ("36", "8-SW-3", 44, "Lower Right Target-Top"),
    ("37", "8-SW-4", 45, "Lower Right Target-Bot"),
    ("40", "8-SW-5", 46, "Left Eject Opto"),
    ("41", "9-SW-1", 47, "Right Eject"),
    ("44", "9-SW-8", 48, "Left Orbit"),
    ("45", "9-SW-9", 49, "Left Ramp Made 1 Opto"),
    ("46", "9-SW-29", 50, "Rear Ramp Made"),
    ("49", "9-SW-13", 51, "Arena Left 10 Pt Sw"),
    ("50", "9-SW-14", 52, "Arena Right 10 Pt Switch"),
    ("51", "9-SW-26", 53, "(B)ATTLE Target"),
    ("52", "9-SW-27", 54, "B(A)TTLE Target"),
    ("53", "9-SW-17", 55, "BA(T)TLE Target"),
    ("54", "9-SW-18", 56, "BAT(T)LE Target"),
    ("55", "9-SW-19", 57, "BATT(L)E Target"),
    ("56", "9-SW-20", 58, "BATTL(E) Target"),
    ("57", "9-SW-21", 59, "Team Rocket Target Left"),
    ("58", "9-SW-22", 60, "Team Rocket Target Right"),
    ("59", "9-SW-23", 61, "Meowth Hit Opto"),
    ("60", "9-SW-24", 62, "Meowth Up"),
    ("61", "9-SW-25", 63, "Meowth Down"),
    ("63", "9-SW-28", 64, "Top Pop Bumper"),
    ("64", "9-SW-30", 65, "Bot Pop Bumper"),
    ("66", "9-SW-10", 66, "Top Lane Left"),
    ("67", "9-SW-11", 67, "Top Lane Right"),
    ("70", "9-SW-0", 68, "Berry Target Right"),
    ("71", "9-SW-4", 69, "Captive Ball-Lower"),
    ("72", "9-SW-5", 70, "Captive Ball-Upper"),
    ("73", "9-SW-6", 71, "Captive Ball-Target"),
    ("74", "9-SW-15", 72, "Right Ramp Hold"),
    ("76", "9-SW-31", 73, "Right Ramp Made Opto"),
    ("78", "9-SW-7", 74, "Spinner Opto"),
    ("79", "9-SW-2", 75, "Team Rocket Enter"),
    ("80", "9-SW-3", 76, "Right Orbit"),
    ("90", "1-SW-2", 77, "Action Button"),
    ("93", "1-SW-11", 78, "Start Button"),
    ("94", "1-SW-12", 79, "Tournament Start Button"),
    ("96", "1-SW-16", 80, "Left Coin"),
    ("97", "1-SW-18", 81, "Right Coin"),
    ("98", "1-SW-17", 82, "Center Coin"),
    ("99", "1-SW-19", 83, "Fourth Coin"),
    ("100", "1-SW-20", 84, "Fifth Coin"),
    ("101", "1-SW-14", 85, "Tilt Pendulum"),
    ("102", "1-SW-21", 86, "Sixth Coin"),
    ("103", "1-SW-8", 87, "Ticket Notch"),
    ("104", "1-SW-22", 88, "Slam Tilt"),
    ("105", "1-SW-7", 89, "Ticket Low"),
    ("C9", "0-SW-8", 90, "Service Select"),
    ("C10", "0-SW-9", 91, "Service Plus"),
    ("C11", "0-SW-10", 92, "Service Minus"),
    ("C12", "0-SW-11", 93, "Service Back"),
    ("C17", "0-SW-16", 94, "Headphone Detect"),
    ("C18", "0-SW-17", 95, "Headphone Kit Cable Detect"),
    ("C19", "0-SW-18", 96, "Volume Encoder 1"),
    ("C20", "0-SW-19", 97, "Volume Encoder 2"),
    ("C24", "0-SW-23", 98, "Coin Door Power Interlock"),
]

_BY_MANUAL = {manual.upper(): row for row in _SWITCHES for manual in [row[0]]}
_BY_ADDRESS = {addr.upper(): row for row in _SWITCHES for addr in [row[1]]}
_BY_RUNTIME = {runtime: row for row in _SWITCHES for runtime in [row[2]]}


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


def resolve_switch(token: str, runtime: bool):
    if runtime:
        sw_id = int(token, 10)
        return _BY_RUNTIME.get(sw_id, (str(sw_id), "runtime", sw_id, f"Switch {sw_id}"))
    key = token.upper()
    if key in _BY_MANUAL:
        return _BY_MANUAL[key]
    if key in _BY_ADDRESS:
        return _BY_ADDRESS[key]
    if token.isdigit():
        # Bare numbers mean manual/service IDs by default. Use --runtime for
        # the shim array index.
        if token in _BY_MANUAL:
            return _BY_MANUAL[token]
        raise SystemExit(f"unknown manual switch ID {token!r}; use --runtime for runtime sw_id {token}")
    raise SystemExit(f"unknown switch {token!r}; use manual ID, N-SW-K address, or --runtime")


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

    manual_id, address, sw_id, name = resolve_switch(args.switch, args.runtime)
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
