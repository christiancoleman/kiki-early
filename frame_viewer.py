#!/usr/bin/env python3
"""
Frame viewer for the Spike 3 emulation shim's real-GL frame bridge.

Listens on $SYSROOT/tmp/spike3_frame.sock (AF_UNIX SOCK_STREAM) for raw RGBA
framebuffers emitted by the shim after each eglSwapBuffers in real-GL
mode (SPIKE3_REAL_GL=1 + SPIKE3_FRAME_BRIDGE=1). Displays live in a
tkinter window.

The default --socket path is derived from this script's location (3 dirs up
from _emulation/ = sysroot), matching where shim_bridges.c binds the socket
when SPIKE3_SYSROOT is set in qemu's env. Override with --socket if needed.

Wire protocol (little-endian, 16-byte header + W*H*4 payload):
    u32 magic       = 0xF00FBABA
    u32 seq         — monotonic sequence number
    u16 width
    u16 height
    u8  format      — 0=RGBA8, 1=BGRA8 (reserved)
    u8  flags       — bit 0: y_flipped (GL bottom-up origin)
    u16 reserved

Usage:
    # start BEFORE launching the game with SPIKE3_FRAME_BRIDGE=1
    python3 frame_viewer.py
    python3 frame_viewer.py --save-every 30        # + save every 30th frame
    python3 frame_viewer.py --scale 0.5            # half-size window
    python3 frame_viewer.py --no-display --save-every 1  # headless capture mode
"""

import argparse
import os
import pathlib
import queue
import socket
import struct
import sys
import threading
import time

# run_game.sh supplies explicit paths so symlinked _emulation dirs can drive
# different sysroots without resolving back to the canonical tree.
_HERE = pathlib.Path(os.environ.get("SPIKE3_EMULATION_DIR", pathlib.Path(__file__).parent)).absolute()
_SYSROOT = pathlib.Path(os.environ.get("SPIKE3_SYSROOT", _HERE.parent.parent.parent)).absolute()
# Default to abstract AF_UNIX namespace (leading null) — matches the shim
# bridge's bind. Avoids 9P/WSL bind-on-/mnt/c failures, zero filesystem
# touch, no cleanup. Override --socket with a filesystem path if you want
# to debug via `ss -l` or similar.
DEFAULT_SOCKET = "\0pokemon_spike3_frame"
DEFAULT_SAVE_DIR = str(_HERE / "captures" / "frames")

import numpy as np
from PIL import Image

FRAME_MAGIC       = 0xF00FBABA
FRAME_HEADER_FMT  = "<IIHHBBH"
FRAME_HEADER_SIZE = struct.calcsize(FRAME_HEADER_FMT)


def recv_exact(sock, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf.extend(chunk)
    return bytes(buf)


def connect_retry(path, attempts=120, delay=0.5):
    for i in range(attempts):
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(path)
            return s
        except (FileNotFoundError, ConnectionRefusedError):
            if i == 0:
                print(f"[viewer] waiting for {path} …", flush=True)
            time.sleep(delay)
    raise SystemExit(f"[viewer] gave up waiting for {path}")


def recv_loop(sock, out_queue, stop_event):
    """Pump frames from socket into a latest-wins queue."""
    while not stop_event.is_set():
        hdr = recv_exact(sock, FRAME_HEADER_SIZE)
        if hdr is None:
            print("[viewer] socket closed by peer", flush=True)
            stop_event.set()
            return
        magic, seq, w, h, fmt, flags, _ = struct.unpack(FRAME_HEADER_FMT, hdr)
        if magic != FRAME_MAGIC:
            print(f"[viewer] bad magic 0x{magic:08x}; dropping connection", flush=True)
            stop_event.set()
            return
        payload = recv_exact(sock, w * h * 4)
        if payload is None:
            stop_event.set()
            return
        # Latest-wins: clear any pending frame before inserting the new one.
        try:
            out_queue.get_nowait()
        except queue.Empty:
            pass
        out_queue.put((seq, w, h, fmt, flags, payload))


def decode_frame(frame, orient="none"):
    seq, w, h, fmt, flags, payload = frame
    arr = np.frombuffer(payload, dtype=np.uint8).reshape((h, w, 4))
    if flags & 1:
        arr = arr[::-1, :, :]  # Y-flip if shim advertises GL bottom-up origin
    # The shim reports `flags=0` for the current Pokémon LE build because the
    # Radium renderer fills the FBO top-down and in correct X-orientation, so
    # the raw buffer is display-ready. `--orient` is kept as an escape hatch
    # for future builds or debugging.
    if orient == "rot180":
        arr = arr[::-1, ::-1, :]
    elif orient == "rot90":
        arr = np.rot90(arr, k=1)
    elif orient == "rot270":
        arr = np.rot90(arr, k=3)
    elif orient == "flip-h":
        arr = arr[:, ::-1, :]
    elif orient == "flip-v":
        arr = arr[::-1, :, :]
    # orient == "none" falls through untouched.
    mode = "RGBA" if fmt == 0 else "BGRA"
    return seq, Image.fromarray(arr, mode)


def run_display(args, sock):
    import tkinter as tk
    from PIL import ImageTk

    q = queue.Queue(maxsize=1)
    stop = threading.Event()
    t = threading.Thread(target=recv_loop, args=(sock, q, stop), daemon=True)
    t.start()

    root = tk.Tk()
    root.title("Spike 3 frame viewer")
    # Size the window up-front so it doesn't start tiny and grow as frames
    # arrive. The shim advertises 1920×1080 (FAKE_MODE_HDISPLAY/VDISPLAY);
    # scale it by args.scale for the window. Fullscreen overrides both.
    if args.fullscreen:
        root.attributes("-fullscreen", True)
    else:
        init_w = max(1, int(1920 * args.scale))
        init_h = max(1, int(1080 * args.scale))
        root.geometry(f"{init_w}x{init_h}")
    label = tk.Label(root, bg="black")
    label.pack(fill=tk.BOTH, expand=True)

    state = {"photo": None, "frames": 0, "t0": time.time(), "last_seq": 0}

    if args.save_every > 0:
        os.makedirs(args.save_dir, exist_ok=True)

    def on_close():
        stop.set()
        root.destroy()
    root.protocol("WM_DELETE_WINDOW", on_close)

    def pump():
        if stop.is_set():
            on_close(); return
        try:
            frame = q.get_nowait()
        except queue.Empty:
            root.after(16, pump)
            return

        seq, img = decode_frame(frame, orient=args.orient)
        show = img
        if args.scale != 1.0:
            new_w = max(1, int(img.width  * args.scale))
            new_h = max(1, int(img.height * args.scale))
            show = img.resize((new_w, new_h), Image.BILINEAR)

        photo = ImageTk.PhotoImage(show.convert("RGB"))
        label.configure(image=photo)
        state["photo"] = photo  # keep ref alive
        state["frames"] += 1
        state["last_seq"] = seq

        if args.save_every > 0 and seq % args.save_every == 0:
            path = os.path.join(args.save_dir, f"frame_{seq:06d}.png")
            img.convert("RGB").save(path)
            print(f"[viewer] saved {path}", flush=True)

        if state["frames"] % 30 == 0:
            dt = time.time() - state["t0"]
            fps = state["frames"] / dt if dt > 0 else 0
            print(f"[viewer] seq={seq}  frames={state['frames']}  {fps:.1f} fps (viewer)",
                  flush=True)

        root.after(1, pump)

    root.after(50, pump)
    root.mainloop()


def run_headless(args, sock):
    os.makedirs(args.save_dir, exist_ok=True)
    q = queue.Queue(maxsize=1)
    stop = threading.Event()
    t = threading.Thread(target=recv_loop, args=(sock, q, stop), daemon=True)
    t.start()
    t0 = time.time(); frames = 0
    try:
        while not stop.is_set():
            try:
                frame = q.get(timeout=1.0)
            except queue.Empty:
                continue
            seq, img = decode_frame(frame, orient=args.orient)
            frames += 1
            if args.save_every > 0 and seq % args.save_every == 0:
                path = os.path.join(args.save_dir, f"frame_{seq:06d}.png")
                img.convert("RGB").save(path)
                print(f"[viewer] saved {path}", flush=True)
            if frames % 30 == 0:
                dt = time.time() - t0
                print(f"[viewer] seq={seq}  frames={frames}  {frames/dt:.1f} fps",
                      flush=True)
    except KeyboardInterrupt:
        print("\n[viewer] interrupted", flush=True)
    finally:
        stop.set()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--socket", default=DEFAULT_SOCKET)
    ap.add_argument("--scale", type=float, default=1.0,
                    help="window scale factor (1.0 = native, 0.5 = half-size)")
    ap.add_argument("--save-every", type=int, default=0,
                    help="save every Nth frame as PNG (0 = off)")
    ap.add_argument("--save-dir", default=DEFAULT_SAVE_DIR)
    ap.add_argument("--no-display", action="store_true",
                    help="headless mode — save frames only, no Tk window")
    ap.add_argument("--fullscreen", action="store_true",
                    help="open the viewer window fullscreen "
                         "(otherwise sized 1920×1080 × --scale at start)")
    ap.add_argument("--orient",
                    default="none",
                    choices=["none", "rot90", "rot180", "rot270", "flip-h", "flip-v"],
                    help="orientation correction applied after the shim's "
                         "y_flipped flag handling (default none — the current "
                         "Pokémon LE build needs no correction)")
    args = ap.parse_args()

    print(f"[viewer] starting (socket={args.socket}, scale={args.scale}, "
          f"save_every={args.save_every}, display={not args.no_display})", flush=True)

    sock = connect_retry(args.socket)
    print(f"[viewer] connected", flush=True)

    if args.no_display:
        run_headless(args, sock)
    else:
        run_display(args, sock)


if __name__ == "__main__":
    main()
