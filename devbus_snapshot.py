#!/usr/bin/env python3
"""Print one joined devbus switch snapshot."""

from __future__ import annotations

import argparse
import json
import socket
import sys
import time
from pathlib import Path

from switch_inventory import SWITCH_BY_RUNTIME_ID, parse_switch_address


ABSTRACT_NAME = "\0pokemon_spike3_devbus"


def parse_args():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--retry", action="store_true", help="wait for devbus to appear")
    ap.add_argument("--raw-json", action="store_true", help="print the raw snapshot JSON")
    ap.add_argument("--from-file", type=Path,
                    help="read a saved snapshot JSON line instead of connecting to devbus")
    ap.add_argument("--mismatches", action="store_true",
                    help="only print rows whose manual address disagrees with runtime channel/bit")
    return ap.parse_args()


def connect(retry: bool):
    while True:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            sock.connect(ABSTRACT_NAME)
            return sock
        except (FileNotFoundError, ConnectionRefusedError):
            sock.close()
            if not retry:
                raise
            time.sleep(1.0)


def recv_messages(sock):
    buf = b""
    while True:
        chunk = sock.recv(8192)
        if not chunk:
            return
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            if not line:
                continue
            yield json.loads(line)


def switch_name(sw_id: int):
    row = SWITCH_BY_RUNTIME_ID.get(sw_id)
    if row is None:
        return "-", "-", "Switch {}".format(sw_id), None
    manual_id, address, _runtime_sw_id, name, _conn, _location, _switch_type = row
    return manual_id, address, name, parse_switch_address(address)


def load_snapshot_from_file(path: Path):
    with path.open("r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                msg = json.loads(line)
            except json.JSONDecodeError:
                continue
            if msg.get("event") == "snapshot":
                return msg
    raise SystemExit("no snapshot event found in {}".format(path))


def polarity_hint(raw, state):
    if raw == "-" or state == "-":
        return "-"
    if int(raw) == int(state):
        return "raw=state"
    return "raw!=state"


def mapping_status(address_bits, channel, bit_idx):
    if address_bits is None:
        return "-"
    if channel == "-" or bit_idx == "-":
        return "missing"
    address_channel, address_bit = address_bits
    if int(channel) == address_channel and int(bit_idx) == address_bit:
        return "ok"
    return "manual={} bit={}".format(address_channel, address_bit)


def print_snapshot(snapshot, mismatches_only=False):
    print("channels:")
    for channel in snapshot.get("channels", []):
        print("  {:>2}: {}".format(channel.get("channel"), channel.get("bytes")))

    print()
    print("{:>5} {:>3} {:>3} {:>5} {:>3} {:>5} {:>5} {:>5} {:>10} {:>12}  {:>8}  {}".format(
        "sw_id", "ch", "bit", "byte", "raw", "state", "edge", "flap",
        "relation", "map", "address", "name"
    ))
    print("-" * 110)
    for sw in snapshot.get("switches", []):
        sw_id = int(sw["sw_id"])
        _manual_id, address, name, address_bits = switch_name(sw_id)
        channel = sw.get("channel", "-")
        bit_idx = sw.get("bit_idx", "-")
        raw = sw.get("raw", "-")
        state = sw.get("state", "-")
        if bit_idx == "-":
            byte_bit = "-"
        else:
            byte_bit = "{}.{}".format(int(bit_idx) >> 3, int(bit_idx) & 7)
        relation = polarity_hint(raw, state)
        map_status = mapping_status(address_bits, channel, bit_idx)
        if mismatches_only and map_status == "ok":
            continue
        print("{:>5} {:>3} {:>3} {:>5} {:>3} {:>5} {:>5} {:>5} {:>10} {:>12}  {:>8}  {}".format(
            sw_id,
            channel,
            bit_idx,
            byte_bit,
            raw,
            state,
            sw.get("edge", "-"),
            sw.get("flap", "-"),
            relation,
            map_status,
            address,
            name,
        ))


def main() -> int:
    args = parse_args()
    if args.from_file is not None:
        snapshot = load_snapshot_from_file(args.from_file)
        if args.raw_json:
            print(json.dumps(snapshot, separators=(",", ":")))
        else:
            print_snapshot(snapshot, args.mismatches)
        return 0

    try:
        sock = connect(args.retry)
    except OSError as exc:
        print("devbus unavailable: {}".format(exc), file=sys.stderr)
        return 1

    with sock:
        for msg in recv_messages(sock):
            if msg.get("event") != "snapshot":
                continue
            if args.raw_json:
                print(json.dumps(msg, separators=(",", ":")))
            else:
                print_snapshot(msg, args.mismatches)
            return 0

    print("devbus closed before snapshot", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
