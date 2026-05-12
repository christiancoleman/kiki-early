#!/usr/bin/env python3
"""Smoke-test for decode_daemon.py.

Demuxes a real game clip with PyAV, ships the H.265 packets to the daemon
over AF_UNIX, and validates that NV12 frames come back. Does NOT touch the
shim — this isolates daemon-side correctness from QEMU/C-side plumbing.

Assumes decode_daemon.py is already running on @spike3_decode.

Usage:
    python3 decode_daemon_test.py [clip_path]
"""

from __future__ import annotations

import socket
import struct
import sys
import time

import av

CLIP_DEFAULT = (
    "/mnt/c/Workspace/pokemon-le-0.81.0/games/pokemon_le/"
    "assets/godot/main.pck_extracted/gfx/bknd/Coast/Lake_01_22_26_h265.mp4"
)

REQ_OPEN = 0x01
REQ_PACKET = 0x02
REQ_RECEIVE = 0x03
REQ_CLOSE = 0x04
REP_OPEN_OK = 0x81
REP_PACKET_OK = 0x82
REP_FRAME = 0x83
REP_OK = 0x84
REP_ERR = 0xF0
REP_EAGAIN = 0xF1
REP_EOF = 0xF2

AV_CODEC_ID_HEVC = 173
AV_NOPTS = -0x8000000000000000


def recv_exact(s, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk:
            raise ConnectionError(f"peer closed at {len(buf)}/{n}")
        buf.extend(chunk)
    return bytes(buf)


def read_msg(s):
    hdr = recv_exact(s, 8)
    t, ln = struct.unpack("<II", hdr)
    return t, recv_exact(s, ln) if ln else b""


def send_msg(s, t, payload=b""):
    s.sendall(struct.pack("<II", t, len(payload)) + payload)


def parse_frame(payload):
    w, h, fmt, pts, n = struct.unpack("<iiiqI", payload[:24])
    off = 24
    planes = []
    for _ in range(n):
        stride, size = struct.unpack("<II", payload[off : off + 8])
        off += 8
        planes.append((stride, size, payload[off : off + size]))
        off += size
    return w, h, fmt, pts, planes


def main():
    clip = sys.argv[1] if len(sys.argv) > 1 else CLIP_DEFAULT

    # 1. Demux
    container = av.open(clip)
    vstream = container.streams.video[0]
    extradata = bytes(vstream.codec_context.extradata or b"")
    print(
        f"clip: {clip}\n"
        f"  codec: {vstream.codec_context.name}  "
        f"{vstream.width}x{vstream.height}  extradata={len(extradata)}B"
    )

    # 2. Connect to daemon (abstract @spike3_decode)
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect("\0spike3_decode")

    # 3. OPEN
    payload = struct.pack("<II", AV_CODEC_ID_HEVC, len(extradata)) + extradata
    send_msg(s, REQ_OPEN, payload)
    t, p = read_msg(s)
    if t != REP_OPEN_OK:
        print(f"OPEN failed: type=0x{t:x} payload={p!r}")
        return 1
    print("OPEN ok")

    # 4. Pump packets, interleave receives
    n_pkts = 0
    n_frames = 0
    t0 = time.time()
    for pkt in container.demux(vstream):
        if pkt.size == 0 and pkt.dts is None:
            continue
        pkt_bytes = bytes(pkt)
        pts = pkt.pts if pkt.pts is not None else AV_NOPTS
        dts = pkt.dts if pkt.dts is not None else AV_NOPTS
        flags = 1 if pkt.is_keyframe else 0
        body = (
            struct.pack("<qqII", pts, dts, flags, len(pkt_bytes)) + pkt_bytes
        )
        send_msg(s, REQ_PACKET, body)
        t, p = read_msg(s)
        if t != REP_PACKET_OK:
            print(f"PACKET #{n_pkts} failed: type=0x{t:x} payload={p!r}")
            return 1
        n_pkts += 1

        # Drain frames available right now
        while True:
            send_msg(s, REQ_RECEIVE)
            t, p = read_msg(s)
            if t == REP_EAGAIN:
                break
            if t != REP_FRAME:
                print(f"RECEIVE got type=0x{t:x}: {p!r}")
                return 1
            w, h, fmt, frame_pts, planes = parse_frame(p)
            n_frames += 1
            if n_frames <= 3 or n_frames % 60 == 0:
                strides = [pl[0] for pl in planes]
                sizes = [pl[1] for pl in planes]
                print(
                    f"  frame {n_frames}: {w}x{h} fmt={fmt} pts={frame_pts} "
                    f"planes={len(planes)} strides={strides} sizes={sizes}"
                )

        if n_pkts >= 200:  # cap so the smoke test stays quick
            break

    elapsed = time.time() - t0
    print(
        f"\nsent {n_pkts} packets, got {n_frames} frames in {elapsed:.2f}s "
        f"({n_frames / elapsed:.1f} fps)"
    )

    send_msg(s, REQ_CLOSE)
    t, p = read_msg(s)
    print(f"CLOSE: type=0x{t:x}")
    s.close()
    return 0 if n_frames > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
