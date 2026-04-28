#!/usr/bin/env python3
"""
Audio subscriber for the Spike 3 emulation shim's audio bridge.

Listens on $SYSROOT/tmp/spike3_audio.sock (AF_UNIX SOCK_STREAM). The shim
tees every ALSA buffer it intercepts via snd_pcm_writei to this socket as raw
S16_LE interleaved stereo at 44.1 kHz — no framing, no headers, just a
continuous PCM byte stream. Latency is buffer-granular (~46 ms at 2048
frames per ALSA buffer).

The default --socket is derived from this script's location (3 dirs up from
_emulation/ = sysroot), matching where the shim binds. Override with
--socket if needed.

Typical invocation — pipe into ffplay while ./run_game.sh runs:

    python3 audio_subscriber.py | ffplay -f s16le -ar 44100 -ac 2 -nodisp -autoexit -i -

Alternative sinks:

    # ALSA (needs `sudo apt install alsa-utils`; WSL2 may route silently)
    python3 audio_subscriber.py | aplay -f S16_LE -c 2 -r 44100 -t raw -
    # archive to a file instead of playing
    python3 audio_subscriber.py > captured.s16le

The shim must be launched with SPIKE3_AUDIO_BRIDGE=1 (default-on in
run_game.sh). Only one subscriber is honoured at a time; connecting a
second evicts the first.
"""

import argparse
import pathlib
import socket
import sys
import time

_HERE = pathlib.Path(__file__).resolve().parent
_SYSROOT = _HERE.parent.parent.parent
DEFAULT_SOCKET = str(_SYSROOT / "tmp" / "spike3_audio.sock")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--socket", default=DEFAULT_SOCKET,
                    help="path to the shim's AF_UNIX audio socket")
    ap.add_argument("--chunk", type=int, default=8192,
                    help="recv() chunk size in bytes (default 8192)")
    args = ap.parse_args()

    # Retry-connect so the subscriber can be started before the shim.
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    attempts = 0
    while True:
        try:
            s.connect(args.socket)
            break
        except (FileNotFoundError, ConnectionRefusedError):
            if attempts == 0:
                print(f"[audio_sub] waiting for {args.socket} …",
                      file=sys.stderr, flush=True)
            attempts += 1
            time.sleep(0.5)
    print(f"[audio_sub] connected", file=sys.stderr, flush=True)

    try:
        while True:
            data = s.recv(args.chunk)
            if not data:
                print("[audio_sub] socket closed by shim", file=sys.stderr, flush=True)
                return
            sys.stdout.buffer.write(data)
            sys.stdout.buffer.flush()
    except KeyboardInterrupt:
        pass
    finally:
        s.close()


if __name__ == "__main__":
    main()
