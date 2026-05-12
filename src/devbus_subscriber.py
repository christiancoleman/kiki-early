#!/usr/bin/env python3
"""devbus_subscriber.py — minimal CLI tail for the shim's devbus bridge.

Connects to the abstract AF_UNIX socket @pokemon_spike3_devbus, prints
each event line as it arrives. Detects sequence gaps. Phase 1 verifier
for shim_devbus.c.

Usage:
    python3 devbus_subscriber.py                    # raw mode: print every line as-is
    python3 devbus_subscriber.py --pretty           # decode + indent JSON
    python3 devbus_subscriber.py --filter switch_change   # only matching events
"""
import argparse
import json
import socket
import sys
import time

ABSTRACT_NAME = "\0pokemon_spike3_devbus"


def parse_args():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--pretty", action="store_true",
                    help="indent decoded JSON")
    ap.add_argument("--filter", default=None,
                    help="only print events matching this 'event' field")
    ap.add_argument("--retry", action="store_true",
                    help="reconnect on disconnect (useful during long runs)")
    return ap.parse_args()


def emit(msg, pretty, line):
    if pretty:
        print(json.dumps(msg, indent=2))
    else:
        print(line.decode("utf-8", errors="replace"), end="")


def run_once(args):
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.connect(ABSTRACT_NAME)
    print(f"[devbus_subscriber] connected to @{ABSTRACT_NAME[1:]}",
          file=sys.stderr)

    last_seq = None
    buf = b""
    while True:
        chunk = sock.recv(8192)
        if not chunk:
            print("[devbus_subscriber] server closed connection",
                  file=sys.stderr)
            return
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            if not line:
                continue
            try:
                msg = json.loads(line)
            except json.JSONDecodeError as e:
                print(f"[devbus_subscriber] parse error: {e} — {line!r}",
                      file=sys.stderr)
                continue

            seq = msg.get("seq")
            if seq is not None:
                if last_seq is not None and seq != last_seq + 1:
                    print(f"[devbus_subscriber] seq gap: "
                          f"expected {last_seq+1}, got {seq} "
                          f"(missed {seq - last_seq - 1})",
                          file=sys.stderr)
                last_seq = seq

            if args.filter and msg.get("event") != args.filter:
                continue

            emit(msg, args.pretty, line + b"\n")


def main():
    args = parse_args()
    while True:
        try:
            run_once(args)
        except (FileNotFoundError, ConnectionRefusedError) as e:
            print(f"[devbus_subscriber] socket not available: {e}",
                  file=sys.stderr)
        except KeyboardInterrupt:
            return
        except Exception as e:
            print(f"[devbus_subscriber] {type(e).__name__}: {e}",
                  file=sys.stderr)

        if not args.retry:
            return
        time.sleep(1.0)


if __name__ == "__main__":
    main()
