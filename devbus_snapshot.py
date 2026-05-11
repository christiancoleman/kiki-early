#!/usr/bin/env python3
"""Print one joined devbus switch snapshot."""

from __future__ import annotations

import argparse
import json
import socket
import sys
import time

from switch_inventory import SWITCH_BY_RUNTIME_ID


ABSTRACT_NAME = "\0pokemon_spike3_devbus"


def parse_args():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--retry", action="store_true", help="wait for devbus to appear")
    ap.add_argument("--raw-json", action="store_true", help="print the raw snapshot JSON")
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
        return "-", "-", "Switch {}".format(sw_id)
    manual_id, address, _runtime_sw_id, name, _conn, _location, _switch_type = row
    return manual_id, address, name


def print_snapshot(snapshot):
    print("channels:")
    for channel in snapshot.get("channels", []):
        print("  {:>2}: {}".format(channel.get("channel"), channel.get("bytes")))

    print()
    print("{:>5} {:>3} {:>3} {:>3} {:>5} {:>5} {:>5} {:>8}  {}".format(
        "sw_id", "ch", "bit", "raw", "state", "edge", "flap", "address", "name"
    ))
    print("-" * 74)
    for sw in snapshot.get("switches", []):
        sw_id = int(sw["sw_id"])
        _manual_id, address, name = switch_name(sw_id)
        print("{:>5} {:>3} {:>3} {:>3} {:>5} {:>5} {:>5} {:>8}  {}".format(
            sw_id,
            sw.get("channel", "-"),
            sw.get("bit_idx", "-"),
            sw.get("raw", "-"),
            sw.get("state", "-"),
            sw.get("edge", "-"),
            sw.get("flap", "-"),
            address,
            name,
        ))


def main() -> int:
    args = parse_args()
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
                print_snapshot(msg)
            return 0

    print("devbus closed before snapshot", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
