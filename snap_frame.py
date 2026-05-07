#!/usr/bin/env python3
"""Connect once to the shim's frame bridge, save N frames as PNG, exit.
Used so we can see the rendered Tech Alerts screen without keeping a Tk
viewer alive across the run."""
import socket, struct, sys, os, time
import numpy as np
from PIL import Image

ABSTRACT = "\0pokemon_spike3_frame"
HEADER = "<IIHHBBH"
HEADER_SIZE = struct.calcsize(HEADER)
MAGIC = 0xF00FBABA
SAVE_DIR = sys.argv[1] if len(sys.argv) > 1 else "/tmp/snap"
FRAMES_TO_SAVE = int(sys.argv[2]) if len(sys.argv) > 2 else 5
SAVE_EVERY = int(sys.argv[3]) if len(sys.argv) > 3 else 30

os.makedirs(SAVE_DIR, exist_ok=True)

def recv_exact(s, n):
    buf = bytearray()
    while len(buf) < n:
        c = s.recv(n - len(buf))
        if not c: return None
        buf.extend(c)
    return bytes(buf)

# Wait up to 30 s for the shim listener
for i in range(60):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(ABSTRACT)
        break
    except (FileNotFoundError, ConnectionRefusedError):
        time.sleep(0.5)
else:
    print("[snap] gave up waiting for shim listener", flush=True)
    sys.exit(1)

print("[snap] connected", flush=True)
seq_seen = 0
saved = 0
last_seq = -1
while saved < FRAMES_TO_SAVE:
    hdr = recv_exact(s, HEADER_SIZE)
    if hdr is None:
        print("[snap] EOF", flush=True)
        break
    magic, seq, w, h, fmt, flags, _ = struct.unpack(HEADER, hdr)
    if magic != MAGIC:
        print(f"[snap] bad magic 0x{magic:08x}", flush=True)
        break
    payload = recv_exact(s, w * h * 4)
    if payload is None:
        break
    seq_seen += 1
    if seq_seen % SAVE_EVERY != 0:
        continue
    arr = np.frombuffer(payload, dtype=np.uint8).reshape(h, w, 4)
    if flags & 1:
        arr = arr[::-1]
    img = Image.fromarray(arr, mode="RGBA")
    path = os.path.join(SAVE_DIR, f"frame_{seq:06d}.png")
    img.convert("RGB").save(path)
    print(f"[snap] saved {path} ({w}x{h})", flush=True)
    saved += 1

print(f"[snap] done — saved {saved} frame(s) of {seq_seen} seen", flush=True)
