#!/usr/bin/env python3
"""Switch inventory loader for TIMER and external switch commands."""

from __future__ import annotations

import importlib.util
import os
import re
from pathlib import Path


_SRC_DIR = Path(__file__).resolve().parents[1]
_DEFAULT_PROFILE = _SRC_DIR / "games" / "POKEMON_LE" / "0.81.0" / "switch_profile.py"
_PROFILE_PATH = Path(os.environ.get("SPIKE3_SWITCH_PROFILE", str(_DEFAULT_PROFILE)))


def _load_profile(path: Path):
    spec = importlib.util.spec_from_file_location("spike3_switch_profile", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not load switch profile: {path}")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


_PROFILE = _load_profile(_PROFILE_PATH)

GAME_TITLE = getattr(_PROFILE, "GAME_TITLE", "UNKNOWN")
GAME_VERSION = getattr(_PROFILE, "GAME_VERSION", "UNKNOWN")
MANUAL_SWITCHES = list(_PROFILE.MANUAL_SWITCHES)
SWITCH_LAYOUT = dict(getattr(_PROFILE, "SWITCH_LAYOUT", {}))

SWITCH_BY_MANUAL_ID = {row[0].upper(): row for row in MANUAL_SWITCHES}
SWITCH_BY_ADDRESS = {row[1].upper(): row for row in MANUAL_SWITCHES}
SWITCH_BY_RUNTIME_ID = {
    row[2]: row
    for row in MANUAL_SWITCHES
    if row[2] is not None
}

_ADDRESS_RE = re.compile(r"^(\d+)-SW-(\d+)$", re.IGNORECASE)


def parse_switch_address(address: str):
    match = _ADDRESS_RE.match(address)
    if match is None:
        return None
    return int(match.group(1), 10), int(match.group(2), 10)


def resolve_switch(token: str, runtime: bool = False):
    if runtime:
        sw_id = int(token, 10)
        row = SWITCH_BY_RUNTIME_ID.get(sw_id)
        if row is not None:
            return row
        return (str(sw_id), "runtime", sw_id, f"Switch {sw_id}", "-", "-", "-")

    key = token.upper()
    if key in SWITCH_BY_MANUAL_ID:
        return SWITCH_BY_MANUAL_ID[key]
    if key in SWITCH_BY_ADDRESS:
        return SWITCH_BY_ADDRESS[key]
    if token.isdigit() and token in SWITCH_BY_MANUAL_ID:
        return SWITCH_BY_MANUAL_ID[token]
    if token.isdigit():
        raise SystemExit(
            f"unknown manual switch ID {token!r}; use --runtime for runtime sw_id {token}"
        )
    raise SystemExit(f"unknown switch {token!r}; use manual ID, N-SW-K address, or --runtime")
