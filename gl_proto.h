#ifndef GL_PROTO_H
#define GL_PROTO_H

#include <stdint.h>

#define GL_PROTO_SOCKET_NAME "spike3_gl"

#define GL_FLAG_SYNC      0x0001
#define GL_FLAG_HAS_DATA  0x0002

#define GL_OP_PING            0x0001

#define GL_OP_GL_VIEWPORT     0x0101
#define GL_OP_GL_CLEAR_COLOR  0x0102

#define GL_OP_GL_CLEAR        0x0401
#define GL_OP_GL_GET_ERROR    0x0402
#define GL_OP_GL_FINISH       0x0403
#define GL_OP_GL_READ_PIXELS  0x0404

#define GL_OP_REP_OK          0x0F01
#define GL_OP_REP_VALUE       0x0F02
#define GL_OP_REP_ERR         0x0F03
#define GL_OP_REP_PIXELS      0x0F04

#pragma pack(push, 1)
struct gl_msg_header {
    uint16_t op;
    uint16_t flags;
    uint32_t payload_len;
};

struct gl_payload_clear {
    uint32_t mask;
};

struct gl_payload_get_error {
    uint32_t reserved;
};

struct gl_payload_clear_color {
    float r, g, b, a;
};

struct gl_payload_viewport {
    int32_t x, y, w, h;
};

struct gl_payload_read_pixels {
    int32_t  x, y, w, h;
    uint32_t format;
    uint32_t type;
};

struct gl_payload_rep_value {
    uint32_t value;
};

struct gl_payload_rep_pixels {
    int32_t  width, height;
    uint32_t format;
    uint32_t type;
    uint32_t byte_size;
};

struct gl_payload_rep_err {
    int32_t  errno_or_avcode;
    uint32_t msg_len;
};
#pragma pack(pop)

#endif
