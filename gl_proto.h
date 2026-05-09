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

#define GL_OP_GL_GEN_BUFFERS         0x0201
#define GL_OP_GL_DELETE_BUFFERS      0x0202
#define GL_OP_GL_BIND_BUFFER         0x0203
#define GL_OP_GL_BUFFER_DATA         0x0301
#define GL_OP_GL_GEN_VERTEX_ARRAYS   0x0204
#define GL_OP_GL_DELETE_VERTEX_ARRAYS 0x0205
#define GL_OP_GL_BIND_VERTEX_ARRAY   0x0206

#define GL_OP_GL_CREATE_SHADER       0x0207
#define GL_OP_GL_DELETE_SHADER       0x0208
#define GL_OP_GL_SHADER_SOURCE       0x0302
#define GL_OP_GL_COMPILE_SHADER      0x0405
#define GL_OP_GL_GET_SHADER_IV       0x0406

#define GL_OP_GL_CREATE_PROGRAM      0x0209
#define GL_OP_GL_DELETE_PROGRAM      0x020A
#define GL_OP_GL_ATTACH_SHADER       0x020B
#define GL_OP_GL_LINK_PROGRAM        0x0407
#define GL_OP_GL_GET_PROGRAM_IV      0x0408
#define GL_OP_GL_USE_PROGRAM         0x020C

#define GL_OP_GL_GET_ATTRIB_LOCATION 0x0409
#define GL_OP_GL_VERTEX_ATTRIB_PTR   0x010A
#define GL_OP_GL_ENABLE_VERTEX_ATTRIB 0x010B
#define GL_OP_GL_GET_UNIFORM_LOCATION 0x040A
#define GL_OP_GL_UNIFORM_1I          0x010C
#define GL_OP_GL_UNIFORM_4F          0x010D
#define GL_OP_GL_UNIFORM_MATRIX4FV   0x010E

#define GL_OP_GL_DRAW_ARRAYS         0x040B
#define GL_OP_GL_DRAW_ELEMENTS       0x040C

#define GL_OP_GL_ENABLE              0x010F
#define GL_OP_GL_DISABLE             0x0110
#define GL_OP_GL_BLEND_FUNC          0x0111
#define GL_OP_GL_DEPTH_FUNC          0x0112
#define GL_OP_GL_DEPTH_MASK          0x0113
#define GL_OP_GL_CULL_FACE           0x0114
#define GL_OP_GL_SCISSOR             0x0115
#define GL_OP_GL_COLOR_MASK          0x0116

#define GL_OP_GL_GEN_TEXTURES        0x020D
#define GL_OP_GL_DELETE_TEXTURES     0x020E
#define GL_OP_GL_ACTIVE_TEXTURE      0x0117
#define GL_OP_GL_BIND_TEXTURE        0x0118
#define GL_OP_GL_TEX_IMAGE_2D        0x0303
#define GL_OP_GL_TEX_SUB_IMAGE_2D    0x0304
#define GL_OP_GL_TEX_PARAMETER_I     0x0119
#define GL_OP_GL_PIXEL_STORE_I       0x011A

#define GL_OP_GL_GEN_FRAMEBUFFERS    0x020F
#define GL_OP_GL_DELETE_FRAMEBUFFERS 0x0210
#define GL_OP_GL_BIND_FRAMEBUFFER    0x011B
#define GL_OP_GL_FRAMEBUFFER_TEX_2D  0x011C
#define GL_OP_GL_CHECK_FRAMEBUFFER   0x040D

/* --- Phase 3d batch: missing state setters / uniforms / UBOs / FBO ops --- */

#define GL_OP_GL_DISABLE_VERTEX_ATTRIB 0x011D
#define GL_OP_GL_BLEND_FUNC_SEPARATE   0x011E
#define GL_OP_GL_BLEND_EQUATION        0x011F
#define GL_OP_GL_BLEND_EQUATION_SEPARATE 0x0120
#define GL_OP_GL_BLEND_COLOR           0x0121
#define GL_OP_GL_STENCIL_FUNC          0x0122
#define GL_OP_GL_STENCIL_OP            0x0123
#define GL_OP_GL_STENCIL_MASK          0x0124
#define GL_OP_GL_DEPTH_RANGEF          0x0125
#define GL_OP_GL_FRONT_FACE            0x0126
#define GL_OP_GL_LINE_WIDTH            0x0127
#define GL_OP_GL_POLYGON_OFFSET        0x0128
#define GL_OP_GL_CLEAR_DEPTHF          0x0129
#define GL_OP_GL_CLEAR_STENCIL         0x012A
#define GL_OP_GL_TEX_PARAMETER_F       0x012B
#define GL_OP_GL_VERTEX_ATTRIB_DIVISOR 0x012C
#define GL_OP_GL_BIND_BUFFER_BASE      0x012D
#define GL_OP_GL_BIND_BUFFER_RANGE     0x012E
#define GL_OP_GL_UNIFORM_BLOCK_BINDING 0x012F
#define GL_OP_GL_BIND_RENDERBUFFER     0x0130
#define GL_OP_GL_FRAMEBUFFER_RB        0x0131
#define GL_OP_GL_RENDERBUFFER_STORAGE  0x0132
#define GL_OP_GL_GENERATE_MIPMAP       0x0133
#define GL_OP_GL_DRAW_BUFFERS          0x0134
#define GL_OP_GL_READ_BUFFER           0x0135
#define GL_OP_GL_INVALIDATE_FB         0x0136
#define GL_OP_GL_VERTEX_ATTRIB_I_PTR   0x0137
#define GL_OP_GL_CLEAR_BUFFER_FV       0x0138
#define GL_OP_GL_DETACH_SHADER         0x0139
#define GL_OP_GL_BIND_ATTRIB_LOC       0x013A
#define GL_OP_GL_VALIDATE_PROGRAM      0x013B
#define GL_OP_GL_FLUSH                 0x013C

#define GL_OP_GL_BUFFER_SUB_DATA       0x0305
#define GL_OP_GL_TEX_IMAGE_3D          0x0306
#define GL_OP_GL_TEX_SUB_IMAGE_3D      0x0307
#define GL_OP_GL_TEX_STORAGE_2D        0x0308
#define GL_OP_GL_TEX_STORAGE_3D        0x0309
#define GL_OP_GL_COMPRESSED_TEX_IMAGE_2D    0x030A
#define GL_OP_GL_COMPRESSED_TEX_SUB_IMAGE_2D 0x030B

#define GL_OP_GL_GEN_RENDERBUFFERS     0x0214
#define GL_OP_GL_DELETE_RENDERBUFFERS  0x0215

#define GL_OP_GL_BLIT_FRAMEBUFFER      0x040E
#define GL_OP_GL_DRAW_RANGE_ELEMENTS   0x040F
#define GL_OP_GL_DRAW_ARRAYS_INSTANCED 0x0410
#define GL_OP_GL_DRAW_ELEMENTS_INSTANCED 0x0411
#define GL_OP_GL_GET_UBLOCK_INDEX      0x0412
#define GL_OP_GL_COPY_BUFFER_SUB_DATA  0x0413

#define GL_OP_GL_UNIFORM_1F            0x0150
#define GL_OP_GL_UNIFORM_2F            0x0151
#define GL_OP_GL_UNIFORM_3F            0x0152
#define GL_OP_GL_UNIFORM_2I            0x0153
#define GL_OP_GL_UNIFORM_3I            0x0154
#define GL_OP_GL_UNIFORM_4I            0x0155
#define GL_OP_GL_UNIFORM_1IV           0x0156
#define GL_OP_GL_UNIFORM_1FV           0x0157
#define GL_OP_GL_UNIFORM_2FV           0x0158
#define GL_OP_GL_UNIFORM_3FV           0x0159
#define GL_OP_GL_UNIFORM_4FV           0x015A
#define GL_OP_GL_UNIFORM_MATRIX_3FV    0x015B

#define GL_OP_REP_OK          0x0F01
#define GL_OP_REP_VALUE       0x0F02
#define GL_OP_REP_ERR         0x0F03
#define GL_OP_REP_PIXELS      0x0F04
#define GL_OP_REP_IDS         0x0F05
#define GL_OP_REP_LOC         0x0F06

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

struct gl_payload_count {
    uint32_t count;
};

struct gl_payload_id_op {
    uint32_t id;
};

struct gl_payload_two_id {
    uint32_t id_a;
    uint32_t id_b;
};

struct gl_payload_bind {
    uint32_t target;
    uint32_t id;
};

struct gl_payload_buffer_data {
    uint32_t target;
    uint32_t size;
    uint32_t usage;
};

struct gl_payload_create_shader {
    uint32_t shader_type;
};

struct gl_payload_shader_source {
    uint32_t shader_id;
    uint32_t source_len;
};

struct gl_payload_get_iv {
    uint32_t id;
    uint32_t pname;
};

struct gl_payload_get_location {
    uint32_t program;
    uint32_t name_len;
};

struct gl_payload_vap {
    uint32_t index;
    int32_t  size;
    uint32_t type;
    uint8_t  normalized;
    uint8_t  pad[3];
    int32_t  stride;
    uint64_t offset;
};

struct gl_payload_uniform_1i {
    int32_t location;
    int32_t value;
};

struct gl_payload_uniform_4f {
    int32_t location;
    float v0, v1, v2, v3;
};

struct gl_payload_uniform_matrix4fv {
    int32_t  location;
    int32_t  count;
    uint8_t  transpose;
    uint8_t  pad[3];
};

struct gl_payload_draw_arrays {
    uint32_t mode;
    int32_t  first;
    int32_t  count;
};

struct gl_payload_draw_elements {
    uint32_t mode;
    int32_t  count;
    uint32_t type;
    uint64_t offset;
};

struct gl_payload_enable {
    uint32_t cap;
};

struct gl_payload_blend_func {
    uint32_t sfactor;
    uint32_t dfactor;
};

struct gl_payload_depth_func {
    uint32_t func;
};

struct gl_payload_byte_param {
    uint8_t  value;
    uint8_t  pad[3];
};

struct gl_payload_color_mask {
    uint8_t  r, g, b, a;
};

struct gl_payload_rect {
    int32_t  x, y, w, h;
};

struct gl_payload_active_texture {
    uint32_t texture_unit;
};

struct gl_payload_tex_image_2d {
    uint32_t target;
    int32_t  level;
    int32_t  internal_format;
    int32_t  width;
    int32_t  height;
    int32_t  border;
    uint32_t format;
    uint32_t type;
    uint32_t data_size;
};

struct gl_payload_tex_sub_image_2d {
    uint32_t target;
    int32_t  level;
    int32_t  xoffset;
    int32_t  yoffset;
    int32_t  width;
    int32_t  height;
    uint32_t format;
    uint32_t type;
    uint32_t data_size;
};

struct gl_payload_tex_parameter_i {
    uint32_t target;
    uint32_t pname;
    int32_t  param;
};

struct gl_payload_pixel_store_i {
    uint32_t pname;
    int32_t  param;
};

struct gl_payload_framebuffer_tex_2d {
    uint32_t target;
    uint32_t attachment;
    uint32_t textarget;
    uint32_t texture;
    int32_t  level;
};

struct gl_payload_rep_ids {
    uint32_t count;
};

struct gl_payload_rep_loc {
    int32_t location;
};

struct gl_payload_rep_err {
    int32_t  errno_or_avcode;
    uint32_t msg_len;
};

struct gl_payload_blend_func_separate {
    uint32_t srcRGB, dstRGB, srcA, dstA;
};
struct gl_payload_blend_eq_separate {
    uint32_t modeRGB, modeA;
};
struct gl_payload_4f {
    float a, b, c, d;
};
struct gl_payload_3u {
    uint32_t a, b, c;
};
struct gl_payload_3i {
    int32_t a, b, c;
};
struct gl_payload_2f {
    float a, b;
};
struct gl_payload_2u {
    uint32_t a, b;
};
struct gl_payload_1f {
    float v;
};
struct gl_payload_1i {
    int32_t v;
};
struct gl_payload_1u {
    uint32_t v;
};
struct gl_payload_tex_param_f {
    uint32_t target;
    uint32_t pname;
    float    param;
};
struct gl_payload_bind_buffer_base {
    uint32_t target;
    uint32_t index;
    uint32_t buffer;
};
struct gl_payload_bind_buffer_range {
    uint32_t target;
    uint32_t index;
    uint32_t buffer;
    int64_t  offset;
    int64_t  size;
};
struct gl_payload_uniform_block_binding {
    uint32_t program;
    uint32_t block_index;
    uint32_t binding;
};
struct gl_payload_framebuffer_rb {
    uint32_t target;
    uint32_t attachment;
    uint32_t rb_target;
    uint32_t rb_id;
};
struct gl_payload_renderbuffer_storage {
    uint32_t target;
    uint32_t internalformat;
    int32_t  width;
    int32_t  height;
};
struct gl_payload_blit_framebuffer {
    int32_t  srcX0, srcY0, srcX1, srcY1;
    int32_t  dstX0, dstY0, dstX1, dstY1;
    uint32_t mask;
    uint32_t filter;
};
struct gl_payload_draw_range_elements {
    uint32_t mode;
    uint32_t start;
    uint32_t end;
    int32_t  count;
    uint32_t type;
    uint64_t offset;
};
struct gl_payload_draw_arrays_instanced {
    uint32_t mode;
    int32_t  first;
    int32_t  count;
    int32_t  instances;
};
struct gl_payload_draw_elements_instanced {
    uint32_t mode;
    int32_t  count;
    uint32_t type;
    uint64_t offset;
    int32_t  instances;
};
struct gl_payload_vap_i {
    uint32_t index;
    int32_t  size;
    uint32_t type;
    int32_t  stride;
    uint64_t offset;
};
struct gl_payload_buffer_sub_data {
    uint32_t target;
    int64_t  offset;
    uint32_t size;
};
struct gl_payload_tex_image_3d {
    uint32_t target;
    int32_t  level;
    int32_t  internal_format;
    int32_t  width, height, depth;
    int32_t  border;
    uint32_t format;
    uint32_t type;
    uint32_t data_size;
};
struct gl_payload_tex_sub_image_3d {
    uint32_t target;
    int32_t  level;
    int32_t  xoffset, yoffset, zoffset;
    int32_t  width, height, depth;
    uint32_t format;
    uint32_t type;
    uint32_t data_size;
};
struct gl_payload_tex_storage_2d {
    uint32_t target;
    int32_t  levels;
    uint32_t internal_format;
    int32_t  width, height;
};
struct gl_payload_tex_storage_3d {
    uint32_t target;
    int32_t  levels;
    uint32_t internal_format;
    int32_t  width, height, depth;
};
struct gl_payload_compressed_tex_image_2d {
    uint32_t target;
    int32_t  level;
    uint32_t internal_format;
    int32_t  width, height, border;
    uint32_t image_size;
};
struct gl_payload_compressed_tex_sub_image_2d {
    uint32_t target;
    int32_t  level;
    int32_t  xoffset, yoffset;
    int32_t  width, height;
    uint32_t format;
    uint32_t image_size;
};
struct gl_payload_uniform_1f {
    int32_t  location;
    float    v0;
};
struct gl_payload_uniform_2f {
    int32_t  location;
    float    v0, v1;
};
struct gl_payload_uniform_3f {
    int32_t  location;
    float    v0, v1, v2;
};
struct gl_payload_uniform_2i {
    int32_t  location;
    int32_t  v0, v1;
};
struct gl_payload_uniform_3i {
    int32_t  location;
    int32_t  v0, v1, v2;
};
struct gl_payload_uniform_4i {
    int32_t  location;
    int32_t  v0, v1, v2, v3;
};
struct gl_payload_uniform_xfv {
    int32_t  location;
    int32_t  count;
    /* followed by `count * elem_count * 4` bytes of float data */
};
struct gl_payload_uniform_xiv {
    int32_t  location;
    int32_t  count;
    /* followed by `count * elem_count * 4` bytes of int data */
};
struct gl_payload_uniform_matrix_3fv {
    int32_t  location;
    int32_t  count;
    uint8_t  transpose;
    uint8_t  pad[3];
    /* followed by `count * 9 * 4` bytes of float data */
};
struct gl_payload_clear_buffer_fv {
    uint32_t buffer;
    int32_t  drawbuffer;
    /* followed by 4 floats (always 4) */
};
struct gl_payload_invalidate_fb {
    uint32_t target;
    uint32_t count;
    /* followed by count * uint32 attachments */
};
struct gl_payload_draw_buffers {
    uint32_t count;
    /* followed by count * uint32 buffers */
};
struct gl_payload_copy_buffer_sub_data {
    uint32_t read_target;
    uint32_t write_target;
    int64_t  read_offset;
    int64_t  write_offset;
    int64_t  size;
};
struct gl_payload_get_ublock_index {
    uint32_t program;
    uint32_t name_len;
    /* followed by name */
};
struct gl_payload_bind_attrib_loc {
    uint32_t program;
    uint32_t index;
    uint32_t name_len;
    /* followed by name */
};
#pragma pack(pop)

#endif
