#!/usr/bin/env python3
"""test_gl_proto_consistency.py — keep display/gl_proto.{h,py} in lockstep.

The shim (`display/shim_gl.c` / `shim_gl_marshal.c`) and the host daemon
(`display/gl_daemon.py`) talk over `@spike3_gl` using a wire format declared
twice: `gl_proto.h` (C — the on-wire truth, packed structs + op macros) and
`gl_proto.py` (Python — `GL_OP_*` / `GL_FLAG_*` constants + `*_FMT` struct
format strings, plus the framing helpers). Adding an op/struct to one and
forgetting the other is a silent wire mismatch. This test reads `gl_proto.h`
as source-of-truth and asserts `gl_proto.py` agrees:

  - `GL_OP_*` / `GL_FLAG_*` constants: same names, same values, both ways
  - op id space: every value distinct (name↔value bijection); reply ops in
    the 0x0F00..0x0FFF band
  - the 8-byte message header: `HEADER_FMT == "<HHI"`, calcsize 8
  - a curated set of payload structs: the size parsed from the C struct ==
    `struct.calcsize` of the matching `*_FMT` string == the literal byte count
    (the same literals `test_gl_proto.c` asserts on the C side)
  - `gl_proto.py`'s `OP_NAMES` keys are all real op values

Pure-logic; no game, no daemon, no sysroot. Run via ../run_tests.sh.
"""
from __future__ import annotations

import re
import struct
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parents[2]          # tests/display/ -> tests/ -> src/
DISPLAY = SRC / "display"
HEADER_PATH = DISPLAY / "gl_proto.h"

_pass = 0
_fail = 0


def check(cond: bool, msg: str) -> None:
    global _pass, _fail
    if cond:
        _pass += 1
    else:
        _fail += 1
        print(f"  FAIL: {msg}")


# --- parse gl_proto.h ------------------------------------------------------
_HEADER_TEXT = HEADER_PATH.read_text(encoding="utf-8")

RE_DEFINE = re.compile(r"^#define\s+(GL_(?:OP|FLAG)_\w+)\s+(0x[0-9A-Fa-f]+|\d+)\s*$", re.M)

def parse_defines(text: str) -> dict[str, int]:
    out: dict[str, int] = {}
    for name, val in RE_DEFINE.findall(text):
        out[name] = int(val, 0)
    return out

H_DEFINES = parse_defines(_HEADER_TEXT)
H_OPS   = {k: v for k, v in H_DEFINES.items() if k.startswith("GL_OP_")}
H_FLAGS = {k: v for k, v in H_DEFINES.items() if k.startswith("GL_FLAG_")}

# Packed size of a C struct from `gl_proto.h` (everything is under
# `#pragma pack(push,1)`, so it's just the sum of field widths).
_TYPE_W = {
    "uint8_t": 1, "int8_t": 1, "char": 1,
    "uint16_t": 2, "int16_t": 2,
    "uint32_t": 4, "int32_t": 4, "float": 4,
    "uint64_t": 8, "int64_t": 8, "double": 8,
}
RE_STRUCT = re.compile(r"struct\s+(gl_\w+)\s*\{(.*?)\}\s*;", re.S)
RE_FIELD  = re.compile(r"^\s*([A-Za-z_]\w*)\s+(.+?);\s*$", re.M)

def parse_struct_sizes(text: str) -> dict[str, int]:
    sizes: dict[str, int] = {}
    for name, body in RE_STRUCT.findall(text):
        # strip /* ... */ comments inside the body
        body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
        total = 0
        for ctype, decls in RE_FIELD.findall(body):
            if ctype not in _TYPE_W:
                # not a scalar field we model — bail on this struct
                total = -1
                break
            w = _TYPE_W[ctype]
            for d in decls.split(","):
                d = d.strip()
                m = re.match(r"^\w+\s*\[\s*(\d+)\s*\]$", d) or re.match(r"^\w+$", d)
                if m and m.lastindex:
                    total += w * int(m.group(1))   # array: name[N]
                else:
                    total += w                     # plain scalar
        sizes[name] = total
    return sizes

H_STRUCT_SIZES = parse_struct_sizes(_HEADER_TEXT)


# --- (0) sanity-check the .h parsers themselves ----------------------------
check(len(H_OPS) >= 100, f".h parser found {len(H_OPS)} GL_OP_* defines (expected >=100)")
check(H_FLAGS == {"GL_FLAG_SYNC": 0x0001, "GL_FLAG_HAS_DATA": 0x0002},
      f".h GL_FLAG_* = {H_FLAGS}")
check(H_STRUCT_SIZES.get("gl_msg_header") == 8,
      f".h parser sizes gl_msg_header at {H_STRUCT_SIZES.get('gl_msg_header')}, want 8")
check(H_STRUCT_SIZES.get("gl_payload_vap") == 28,
      f".h parser sizes gl_payload_vap at {H_STRUCT_SIZES.get('gl_payload_vap')}, want 28")


# --- (1) op id space: distinct values, name<->value bijection --------------
vals = list(H_OPS.values())
check(len(vals) == len(set(vals)),
      "GL_OP_* values are NOT all distinct — "
      + ", ".join(f"{a}=={b}=0x{H_OPS[a]:04x}"
                  for i, a in enumerate(H_OPS) for b in list(H_OPS)[i+1:] if H_OPS[a] == H_OPS[b]))
for name, v in H_OPS.items():
    if name.startswith("GL_OP_REP_"):
        check(0x0F00 <= v <= 0x0FFF, f"{name}=0x{v:04x} not in reply band 0x0F00..0x0FFF")
    else:
        check(not (0x0F00 <= v <= 0x0FFF), f"{name}=0x{v:04x} sits in the reply band but isn't a REP_ op")


# --- (2) gl_proto.py constants == gl_proto.h constants ---------------------
sys.path.insert(0, str(DISPLAY))
import gl_proto as P  # noqa: E402

PY_OPS   = {k: getattr(P, k) for k in dir(P) if k.startswith("GL_OP_")}
PY_FLAGS = {k: getattr(P, k) for k in dir(P) if k.startswith("GL_FLAG_")}

check(PY_OPS == H_OPS,
      "gl_proto.py GL_OP_* != gl_proto.h: "
      + f"only in .py={sorted(set(PY_OPS)-set(H_OPS))}; "
      + f"only in .h={sorted(set(H_OPS)-set(PY_OPS))}; "
      + f"value mismatch={[(k, hex(PY_OPS[k]), hex(H_OPS[k])) for k in PY_OPS if k in H_OPS and PY_OPS[k]!=H_OPS[k]]}")
check(PY_FLAGS == H_FLAGS, f"gl_proto.py GL_FLAG_* {PY_FLAGS} != gl_proto.h {H_FLAGS}")
check(P.GL_PROTO_SOCKET_NAME == "spike3_gl"
      and f'"{P.GL_PROTO_SOCKET_NAME}"' in _HEADER_TEXT, "socket name matches GL_PROTO_SOCKET_NAME")


# --- (3) message header framing -------------------------------------------
check(P.HEADER_FMT == "<HHI", f"HEADER_FMT = {P.HEADER_FMT!r}, want '<HHI'")
check(P.HEADER_LEN == 8 and struct.calcsize(P.HEADER_FMT) == 8, "HEADER_LEN / calcsize == 8")
# pack_header / unpack_header round-trip
check(P.unpack_header(P.pack_header(P.GL_OP_PING, P.GL_FLAG_SYNC, 0)) == (P.GL_OP_PING, P.GL_FLAG_SYNC, 0),
      "pack_header/unpack_header round-trip")


# --- (4) curated payload structs: .h size == .py calcsize == literal -------
# {C struct name: (gl_proto.py format-string attr, expected packed bytes)}.
# Same literals test_gl_proto.c asserts on the C side.
CURATED: dict[str, tuple[str, int]] = {
    "gl_payload_clear":                  ("CLEAR_FMT",                4),
    "gl_payload_get_error":              ("GET_ERROR_FMT",            4),
    "gl_payload_clear_color":            ("CLEAR_COLOR_FMT",         16),
    "gl_payload_viewport":               ("VIEWPORT_FMT",            16),
    "gl_payload_read_pixels":            ("READ_PIXELS_FMT",         24),
    "gl_payload_rep_value":              ("REP_VALUE_FMT",            4),
    "gl_payload_rep_pixels":             ("REP_PIXELS_FMT",          20),
    "gl_payload_rep_err":                ("REP_ERR_FMT",              8),
    "gl_payload_count":                  ("COUNT_FMT",                4),
    "gl_payload_id_op":                  ("ID_FMT",                   4),
    "gl_payload_two_id":                 ("TWO_ID_FMT",               8),
    "gl_payload_bind":                   ("BIND_FMT",                 8),
    "gl_payload_buffer_data":            ("BUFFER_DATA_FMT",         12),
    "gl_payload_vap":                    ("VAP_FMT",                 28),
    "gl_payload_uniform_1i":             ("UNIFORM_1I_FMT",           8),
    "gl_payload_uniform_4f":             ("UNIFORM_4F_FMT",          20),
    "gl_payload_uniform_matrix4fv":      ("UNIFORM_MAT4_FMT",        12),
    "gl_payload_draw_arrays":            ("DRAW_ARRAYS_FMT",         12),
    "gl_payload_draw_elements":          ("DRAW_ELEMENTS_FMT",       20),
    "gl_payload_byte_param":             ("BYTE_PARAM_FMT",           4),
    "gl_payload_color_mask":             ("COLOR_MASK_FMT",           4),
    "gl_payload_tex_image_2d":           ("TEX_IMAGE_2D_FMT",        36),
    "gl_payload_tex_sub_image_2d":       ("TEX_SUB_IMAGE_2D_FMT",    36),
    "gl_payload_tex_image_3d":           ("TEX_IMAGE_3D_FMT",        40),
    "gl_payload_tex_sub_image_3d":       ("TEX_SUB_IMAGE_3D_FMT",    44),
    "gl_payload_bind_buffer_range":      ("BIND_BUF_RANGE_FMT",      28),
    "gl_payload_blit_framebuffer":       ("BLIT_FB_FMT",             40),
    "gl_payload_draw_elements_instanced":("DRAW_ELEMENTS_INST_FMT",  24),
    "gl_payload_tex_storage_3d_ms":      ("TEX_STORAGE_3D_MS_FMT",   28),
    "gl_payload_copy_buffer_sub_data":   ("COPY_BUFFER_SUB_DATA_FMT",32),
}
for sname, (fmt_attr, want) in CURATED.items():
    h_size = H_STRUCT_SIZES.get(sname)
    check(h_size == want, f"{sname}: gl_proto.h packs to {h_size}, want {want}")
    fmt = getattr(P, fmt_attr, None)
    check(fmt is not None, f"gl_proto.py is missing {fmt_attr}")
    if fmt is not None:
        check(struct.calcsize(fmt) == want,
              f"{fmt_attr}={fmt!r} calcsize {struct.calcsize(fmt)}, want {want}")


# --- (5) OP_NAMES keys are all real op values ------------------------------
op_values = set(H_OPS.values())
for k in P.OP_NAMES:
    check(k in op_values, f"gl_proto.py OP_NAMES has key 0x{k:04x} that isn't any GL_OP_*")
check(P.op_name(P.GL_OP_PING) == "PING", "op_name(PING) == 'PING'")
check(P.op_name(0xBEEF).startswith("UNKNOWN_"), "op_name of an unknown op -> UNKNOWN_*")


print(f"\n[test_gl_proto_consistency] {_pass} passed, {_fail} failed")
sys.exit(1 if _fail else 0)
