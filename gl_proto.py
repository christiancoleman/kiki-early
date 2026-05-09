import struct

GL_PROTO_SOCKET_NAME = "spike3_gl"

GL_FLAG_SYNC     = 0x0001
GL_FLAG_HAS_DATA = 0x0002

GL_OP_PING            = 0x0001

GL_OP_GL_VIEWPORT     = 0x0101
GL_OP_GL_CLEAR_COLOR  = 0x0102

GL_OP_GL_CLEAR        = 0x0401
GL_OP_GL_GET_ERROR    = 0x0402
GL_OP_GL_FINISH       = 0x0403
GL_OP_GL_READ_PIXELS  = 0x0404

GL_OP_REP_OK          = 0x0F01
GL_OP_REP_VALUE       = 0x0F02
GL_OP_REP_ERR         = 0x0F03
GL_OP_REP_PIXELS      = 0x0F04

OP_NAMES = {
    GL_OP_PING:            "PING",
    GL_OP_GL_VIEWPORT:     "GL_VIEWPORT",
    GL_OP_GL_CLEAR_COLOR:  "GL_CLEAR_COLOR",
    GL_OP_GL_CLEAR:        "GL_CLEAR",
    GL_OP_GL_GET_ERROR:    "GL_GET_ERROR",
    GL_OP_GL_FINISH:       "GL_FINISH",
    GL_OP_GL_READ_PIXELS:  "GL_READ_PIXELS",
    GL_OP_REP_OK:          "REP_OK",
    GL_OP_REP_VALUE:       "REP_VALUE",
    GL_OP_REP_ERR:         "REP_ERR",
    GL_OP_REP_PIXELS:      "REP_PIXELS",
}

HEADER_FMT = "<HHI"
HEADER_LEN = struct.calcsize(HEADER_FMT)

CLEAR_FMT       = "<I"
GET_ERROR_FMT   = "<I"
CLEAR_COLOR_FMT = "<ffff"
VIEWPORT_FMT    = "<iiii"
READ_PIXELS_FMT = "<iiiiII"
REP_VALUE_FMT   = "<I"
REP_ERR_FMT     = "<iI"
REP_PIXELS_FMT  = "<iiIII"


def pack_header(op: int, flags: int, payload_len: int) -> bytes:
    return struct.pack(HEADER_FMT, op, flags, payload_len)


def unpack_header(buf: bytes) -> tuple[int, int, int]:
    return struct.unpack(HEADER_FMT, buf)


def op_name(op: int) -> str:
    return OP_NAMES.get(op, f"UNKNOWN_0x{op:04x}")
