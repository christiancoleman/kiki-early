#!/usr/bin/env python3
"""decode_daemon.py — host-side video decode forwarder.

Listens on an abstract AF_UNIX socket (default @spike3_decode). Each client
connection is one decode session. The shim inside QEMU sends encoded packets
and pulls back decoded frames; this daemon runs the actual decode on the host
via NVDEC (hevc_cuvid / h264_cuvid), returning frames as NV12 plane bytes.

Wire protocol — all integers little-endian:

  request  :=  [u32 type][u32 length][payload]
  reply    :=  [u32 type][u32 length][payload]

  request types:
    0x01 OPEN     payload = [u32 codec_id][u32 extradata_len][extradata]
    0x02 PACKET   payload = [i64 pts][i64 dts][u32 flags][u32 size][bytes]
    0x03 RECEIVE  payload = (empty)
    0x04 CLOSE    payload = (empty)

  reply types:
    0x81 OPEN_OK   payload = (empty)
    0x82 PACKET_OK payload = (empty)
    0x83 FRAME     payload = [i32 w][i32 h][i32 fmt][i64 pts][u32 n_planes]
                              foreach plane: [u32 stride][u32 size][bytes]
    0x84 OK        payload = (empty)              -- close ack
    0xF0 ERR       payload = [i32 errno_or_avcode][u32 msg_len][msg]
    0xF1 EAGAIN    payload = (empty)
    0xF2 EOF       payload = (empty)

Codec IDs are FFmpeg's AV_CODEC_ID_*: HEVC=173, H264=27. The daemon maps to
the corresponding *_cuvid decoder.

Run:
    python3 decode_daemon.py [--socket @spike3_decode] [--no-hwaccel] [--verbose]

Lifecycle: started by run_game.sh before QEMU launches, killed on cleanup.
"""

from __future__ import annotations

import argparse
import logging
import os
import signal
import socket
import struct
import sys
import threading

try:
    import av
except ImportError:
    sys.stderr.write(
        "decode_daemon: PyAV not installed. Install with: pip3 install av\n"
    )
    sys.exit(2)


REQ_OPEN = 0x01
REQ_PACKET = 0x02
REQ_RECEIVE = 0x03
REQ_CLOSE = 0x04
REQ_FLUSH = 0x05

REP_OPEN_OK = 0x81
REP_PACKET_OK = 0x82
REP_FRAME = 0x83
REP_OK = 0x84
REP_ERR = 0xF0
REP_EAGAIN = 0xF1
REP_EOF = 0xF2

AV_CODEC_ID_HEVC = 173
AV_CODEC_ID_H264 = 27

CODEC_MAP_HW = {
    AV_CODEC_ID_HEVC: "hevc_cuvid",
    AV_CODEC_ID_H264: "h264_cuvid",
}
CODEC_MAP_SW = {
    AV_CODEC_ID_HEVC: "hevc",
    AV_CODEC_ID_H264: "h264",
}


def recv_exact(sock: socket.socket, n: int) -> bytes:
    """Read exactly n bytes from sock; raises ConnectionError on short read."""
    buf = bytearray(n)
    view = memoryview(buf)
    got = 0
    while got < n:
        chunk = sock.recv_into(view[got:])
        if chunk == 0:
            raise ConnectionError(f"peer closed after {got}/{n} bytes")
        got += chunk
    return bytes(buf)


def read_msg(sock: socket.socket) -> tuple[int, bytes]:
    hdr = recv_exact(sock, 8)
    msg_type, length = struct.unpack("<II", hdr)
    payload = recv_exact(sock, length) if length else b""
    return msg_type, payload


def send_msg(sock: socket.socket, msg_type: int, payload: bytes = b"") -> None:
    sock.sendall(struct.pack("<II", msg_type, len(payload)) + payload)


def send_err(sock: socket.socket, code: int, msg: str) -> None:
    msg_b = msg.encode("utf-8", errors="replace")[:1024]
    send_msg(sock, REP_ERR, struct.pack("<iI", code, len(msg_b)) + msg_b)


class Session:
    """One decode session, bound to one client connection."""

    def __init__(
        self, sock: socket.socket, peer: str, use_hwaccel: bool, log: logging.Logger
    ):
        self.sock = sock
        self.peer = peer
        self.use_hwaccel = use_hwaccel
        self.log = log
        self.codec_ctx: av.codec.context.CodecContext | None = None
        self.codec_name: str = ""
        self.codec_id: int = 0
        self.extradata: bytes = b""
        self.frames_decoded = 0
        self.packets_in = 0

    def handle_open(self, payload: bytes) -> None:
        if len(payload) < 8:
            send_err(self.sock, -22, "OPEN payload too short")
            return
        codec_id, extra_len = struct.unpack("<II", payload[:8])
        if len(payload) != 8 + extra_len:
            send_err(self.sock, -22, "OPEN extradata length mismatch")
            return
        extradata = payload[8 : 8 + extra_len]

        codec_name = (
            CODEC_MAP_HW.get(codec_id) if self.use_hwaccel else None
        ) or CODEC_MAP_SW.get(codec_id)
        if codec_name is None:
            send_err(self.sock, -22, f"unsupported codec_id {codec_id}")
            return

        try:
            ctx = av.codec.CodecContext.create(codec_name, "r")
            if extradata:
                ctx.extradata = extradata
            ctx.open()
        except Exception as exc:
            self.log.warning(
                "[%s] open(%s) failed: %s; trying SW fallback",
                self.peer,
                codec_name,
                exc,
            )
            sw_name = CODEC_MAP_SW.get(codec_id)
            if sw_name is None or sw_name == codec_name:
                send_err(self.sock, -22, f"open failed: {exc}")
                return
            try:
                ctx = av.codec.CodecContext.create(sw_name, "r")
                if extradata:
                    ctx.extradata = extradata
                ctx.open()
                codec_name = sw_name
            except Exception as exc2:
                send_err(self.sock, -22, f"SW fallback also failed: {exc2}")
                return

        self.codec_ctx = ctx
        self.codec_name = codec_name
        self.codec_id = codec_id
        self.extradata = bytes(extradata)
        self.log.info(
            "[%s] OPEN codec=%s extradata=%dB", self.peer, codec_name, extra_len
        )
        send_msg(self.sock, REP_OPEN_OK)

    def handle_packet(self, payload: bytes) -> None:
        if self.codec_ctx is None:
            send_err(self.sock, -22, "PACKET before OPEN")
            return
        if len(payload) < 24:
            send_err(self.sock, -22, "PACKET header too short")
            return
        pts, dts, flags, size = struct.unpack("<qqII", payload[:24])
        if len(payload) != 24 + size:
            send_err(self.sock, -22, "PACKET size mismatch")
            return
        data = payload[24 : 24 + size]

        try:
            pkt = av.Packet(data) if size else av.Packet()
            if pts != -0x8000000000000000:  # AV_NOPTS_VALUE
                pkt.pts = pts
            if dts != -0x8000000000000000:
                pkt.dts = dts
            # Flags bit 0 = AV_PKT_FLAG_KEY; PyAV exposes it via .is_keyframe
            pkt.is_keyframe = bool(flags & 1)
            # Stash decoded frames in a per-session queue — receive_frame pulls.
            for frame in self.codec_ctx.decode(pkt):
                self._frame_queue.append(frame)
            self.packets_in += 1
        except av.error.EOFError:
            # Normal end-of-stream from FFmpeg — flag the queue as drained;
            # next RECEIVE returns REP_EOF after any in-flight frames.
            self._eof_pending = True
        except Exception as exc:
            send_err(self.sock, -22, f"packet: {type(exc).__name__}: {exc}")
            return
        send_msg(self.sock, REP_PACKET_OK)

    def handle_receive(self) -> None:
        if self.codec_ctx is None:
            send_err(self.sock, -22, "RECEIVE before OPEN")
            return
        if not self._frame_queue:
            if self._eof_pending:
                send_msg(self.sock, REP_EOF)
            else:
                send_msg(self.sock, REP_EAGAIN)
            return
        frame = self._frame_queue.pop(0)
        # Convert to NV12 in system memory. If frame is already NV12 / on CPU
        # this is a fast path; for hwaccel frames it triggers a download.
        try:
            nv12 = frame.reformat(format="nv12")
        except Exception as exc:
            send_err(self.sock, -22, f"reformat to nv12: {exc}")
            return

        AV_PIX_FMT_NV12 = 23
        pts = nv12.pts if nv12.pts is not None else -0x8000000000000000
        # PyAV's bytes(plane) is unreliable for NV12 plane[1] (the planes
        # share a single underlying buffer; the UV view doesn't always
        # expose its bytes through the buffer protocol). Use to_ndarray,
        # which returns a contiguous (h*3/2, w) uint8 view: rows [0:h] are
        # Y, rows [h:h+h/2] are interleaved UV.
        try:
            arr = nv12.to_ndarray(format="nv12")
        except Exception as exc:
            send_err(self.sock, -22, f"to_ndarray nv12: {exc}")
            return
        h = nv12.height
        w = nv12.width
        y_bytes = arr[:h, :].tobytes()
        uv_bytes = arr[h : h + h // 2, :].tobytes()
        n_planes = 2
        # Wire layout (per protocol docstring): header, then per-plane
        # [stride][size][bytes] interleaved. Stride = w (to_ndarray returns
        # tightly packed rows).
        header = struct.pack(
            "<iiiqI", w, h, AV_PIX_FMT_NV12, pts, n_planes
        )
        plane0 = struct.pack("<II", w, len(y_bytes)) + y_bytes
        plane1 = struct.pack("<II", w, len(uv_bytes)) + uv_bytes
        send_msg(self.sock, REP_FRAME, header + plane0 + plane1)
        self.frames_decoded += 1
        if self.frames_decoded <= 3 or (self.frames_decoded % 100) == 0:
            self.log.info(
                "[%s] frame #%d %dx%d nv12 pts=%s",
                self.peer,
                self.frames_decoded,
                nv12.width,
                nv12.height,
                pts,
            )

    def handle_flush(self) -> None:
        """Reset codec state for post-seek packet stream.

        Use PyAV's flush_buffers() — fast path, keeps the CUDA context warm
        for hevc_cuvid (avoids ~hundreds-of-ms cold-init). Falls back to
        recreate-with-extradata if the codec doesn't expose it. Drains the
        in-flight frame queue + clears EOF flag.
        """
        if self.codec_ctx is None:
            send_err(self.sock, -22, "FLUSH before OPEN")
            return
        used_recreate = False
        flush_err = None
        try:
            self.codec_ctx.flush_buffers()
        except Exception as exc:
            flush_err = f"{type(exc).__name__}: {exc}"
            try:
                ctx = av.codec.CodecContext.create(self.codec_name, "r")
                if self.extradata:
                    ctx.extradata = self.extradata
                ctx.open()
                self.codec_ctx = ctx
                used_recreate = True
            except Exception as exc2:
                send_err(self.sock, -22, f"flush failed: {flush_err} / recreate: {exc2}")
                return
        self._frame_queue = []
        self._eof_pending = False
        self.log.info(
            "[%s] FLUSH codec=%s pkts_in=%d frames_out=%d (%s)",
            self.peer, self.codec_name, self.packets_in, self.frames_decoded,
            f"ctx reopened — flush_buffers err: {flush_err}" if used_recreate
            else "flush_buffers()",
        )
        send_msg(self.sock, REP_OK)

    def handle_close(self) -> None:
        send_msg(self.sock, REP_OK)
        self.log.info(
            "[%s] CLOSE codec=%s pkts_in=%d frames_out=%d",
            self.peer,
            self.codec_name,
            self.packets_in,
            self.frames_decoded,
        )

    _frame_queue: list = []
    _eof_pending: bool = False

    def run(self) -> None:
        # per-instance state (avoid sharing the class-level defaults)
        self._frame_queue = []
        self._eof_pending = False
        try:
            while True:
                msg_type, payload = read_msg(self.sock)
                if msg_type == REQ_OPEN:
                    self.handle_open(payload)
                elif msg_type == REQ_PACKET:
                    self.handle_packet(payload)
                elif msg_type == REQ_RECEIVE:
                    self.handle_receive()
                elif msg_type == REQ_CLOSE:
                    self.handle_close()
                    return
                elif msg_type == REQ_FLUSH:
                    self.handle_flush()
                else:
                    send_err(self.sock, -22, f"unknown msg_type 0x{msg_type:x}")
                    return
        except (ConnectionError, OSError) as exc:
            self.log.info("[%s] disconnected: %s", self.peer, exc)
        except Exception as exc:
            self.log.exception("[%s] session error: %s", self.peer, exc)
        finally:
            try:
                self.sock.close()
            except Exception:
                pass


def serve(socket_path: str, use_hwaccel: bool, log: logging.Logger) -> None:
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    # Abstract namespace if path starts with @
    addr = "\0" + socket_path[1:] if socket_path.startswith("@") else socket_path
    if not socket_path.startswith("@"):
        try:
            os.unlink(socket_path)
        except FileNotFoundError:
            pass
    srv.bind(addr)
    srv.listen(8)
    log.info(
        "decode_daemon listening on %s (hwaccel=%s)",
        socket_path,
        "on" if use_hwaccel else "off",
    )

    def shutdown(signum, frame):  # noqa: ARG001
        log.info("signal %d → shutting down", signum)
        try:
            srv.close()
        except Exception:
            pass
        sys.exit(0)

    signal.signal(signal.SIGTERM, shutdown)
    signal.signal(signal.SIGINT, shutdown)

    conn_id = 0
    while True:
        try:
            conn, _ = srv.accept()
        except OSError:
            break
        conn_id += 1
        peer = f"conn-{conn_id}"
        sess = Session(conn, peer, use_hwaccel, log)
        t = threading.Thread(target=sess.run, name=peer, daemon=True)
        t.start()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--socket",
        default="@spike3_decode",
        help="AF_UNIX socket path. Prefix with '@' for abstract namespace.",
    )
    ap.add_argument(
        "--no-hwaccel",
        action="store_true",
        help="Force software decode (skip *_cuvid).",
    )
    ap.add_argument("--verbose", "-v", action="store_true")
    args = ap.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
        datefmt="%H:%M:%S",
    )
    log = logging.getLogger("decode_daemon")
    serve(args.socket, not args.no_hwaccel, log)
    return 0


if __name__ == "__main__":
    sys.exit(main())
