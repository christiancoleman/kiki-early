/*
 * test_gl_proto.c — host-compiled layout tests for display/gl_proto.h.
 *
 * Compiled with the HOST compiler by _emulation/src/tests/run_tests.sh, which
 * passes -I pointing at _emulation/src/ so `#include "display/gl_proto.h"`
 * below resolves. gl_proto.h is the wire format for the shim ↔ gl_daemon.py
 * channel (`@spike3_gl`); the bytes that go on the wire are exactly the packed
 * struct layouts here, so the things worth pinning in C are:
 *   - the 8-byte fixed message header (op/flags/payload_len at 0/2/4)
 *   - that `#pragma pack(push,1)` actually took effect (no padding sneaks in)
 *   - a curated set of payload struct sizes — the same numbers the Python side
 *     (gl_proto.py's `*_FMT` strings) must agree with; test_gl_proto_consistency.py
 *     checks the .h ↔ .py side and that the op-id space is distinct/bijective.
 *
 * No shim externs needed — gl_proto.h is self-contained (just <stdint.h>).
 *
 * Run:  ../run_tests.sh   (from _emulation/src/tests/: ./run_tests.sh)
 */
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

#include "display/gl_proto.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...) do {                                              \
    if (cond) { g_pass++; }                                                \
    else { g_fail++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

#define CHECK_SIZE(type, want) \
    CHECK(sizeof(type) == (want), "sizeof(%s) = %zu, want %d", #type, sizeof(type), (want))

int main(void) {
    /* --- message header: 8 bytes, fields at fixed offsets --- */
    CHECK_SIZE(struct gl_msg_header, 8);
    CHECK(offsetof(struct gl_msg_header, op)          == 0, "header.op @ %zu, want 0", offsetof(struct gl_msg_header, op));
    CHECK(offsetof(struct gl_msg_header, flags)       == 2, "header.flags @ %zu, want 2", offsetof(struct gl_msg_header, flags));
    CHECK(offsetof(struct gl_msg_header, payload_len) == 4, "header.payload_len @ %zu, want 4", offsetof(struct gl_msg_header, payload_len));
    CHECK(sizeof(((struct gl_msg_header*)0)->op)    == 2, "header.op is u16");
    CHECK(sizeof(((struct gl_msg_header*)0)->flags) == 2, "header.flags is u16");
    CHECK(sizeof(((struct gl_msg_header*)0)->payload_len) == 4, "header.payload_len is u32");

    /* --- flag bits: distinct, single-bit, non-overlapping --- */
    CHECK(GL_FLAG_SYNC == 0x0001 && GL_FLAG_HAS_DATA == 0x0002, "flag bit values");
    CHECK((GL_FLAG_SYNC & GL_FLAG_HAS_DATA) == 0, "flag bits don't overlap");

    /* --- reply ops live in the 0x0F00..0x0FFF band --- */
    CHECK((GL_OP_REP_OK     & 0xFF00) == 0x0F00, "REP_OK in 0x0Fxx");
    CHECK((GL_OP_REP_VALUE  & 0xFF00) == 0x0F00, "REP_VALUE in 0x0Fxx");
    CHECK((GL_OP_REP_ERR    & 0xFF00) == 0x0F00, "REP_ERR in 0x0Fxx");
    CHECK((GL_OP_REP_PIXELS & 0xFF00) == 0x0F00, "REP_PIXELS in 0x0Fxx");
    CHECK((GL_OP_REP_IDS    & 0xFF00) == 0x0F00, "REP_IDS in 0x0Fxx");
    CHECK((GL_OP_REP_LOC    & 0xFF00) == 0x0F00, "REP_LOC in 0x0Fxx");
    /* a request op is never in that band */
    CHECK((GL_OP_PING          & 0xFF00) != 0x0F00, "PING not in reply band");
    CHECK((GL_OP_GL_DRAW_ARRAYS & 0xFF00) != 0x0F00, "DRAW_ARRAYS not in reply band");

    /* --- pack(1) sanity on a curated set of payload structs.
     * Numbers mirror gl_proto.py's `*_FMT` calcsizes; if a struct or a format
     * string drifts, this and test_gl_proto_consistency.py both catch it. --- */
    CHECK_SIZE(struct gl_payload_clear,                  4);   /* <I        */
    CHECK_SIZE(struct gl_payload_get_error,              4);   /* <I        */
    CHECK_SIZE(struct gl_payload_clear_color,           16);   /* <ffff     */
    CHECK_SIZE(struct gl_payload_viewport,              16);   /* <iiii     */
    CHECK_SIZE(struct gl_payload_read_pixels,           24);   /* <iiiiII   */
    CHECK_SIZE(struct gl_payload_rep_value,              4);   /* <I        */
    CHECK_SIZE(struct gl_payload_rep_pixels,            20);   /* <iiIII    */
    CHECK_SIZE(struct gl_payload_rep_err,                8);   /* <iI       */
    CHECK_SIZE(struct gl_payload_count,                  4);
    CHECK_SIZE(struct gl_payload_id_op,                  4);
    CHECK_SIZE(struct gl_payload_two_id,                 8);
    CHECK_SIZE(struct gl_payload_bind,                   8);
    CHECK_SIZE(struct gl_payload_buffer_data,           12);
    CHECK_SIZE(struct gl_payload_vap,                   28);   /* <IiIB3xiQ */
    CHECK_SIZE(struct gl_payload_uniform_1i,             8);
    CHECK_SIZE(struct gl_payload_uniform_4f,            20);   /* <iffff    */
    CHECK_SIZE(struct gl_payload_uniform_matrix4fv,     12);   /* <iiB3x    */
    CHECK_SIZE(struct gl_payload_draw_arrays,           12);
    CHECK_SIZE(struct gl_payload_draw_elements,         20);   /* <IiIQ     */
    CHECK_SIZE(struct gl_payload_byte_param,             4);   /* <B3x      */
    CHECK_SIZE(struct gl_payload_color_mask,             4);   /* <BBBB     */
    CHECK_SIZE(struct gl_payload_tex_image_2d,          36);   /* 9 × 4     */
    CHECK_SIZE(struct gl_payload_tex_sub_image_2d,      36);
    CHECK_SIZE(struct gl_payload_tex_image_3d,          40);   /* 10 × 4    */
    CHECK_SIZE(struct gl_payload_tex_sub_image_3d,      44);   /* 11 × 4    */
    CHECK_SIZE(struct gl_payload_bind_buffer_range,     28);   /* <IIIqq    */
    CHECK_SIZE(struct gl_payload_blit_framebuffer,      40);   /* 10 × 4    */
    CHECK_SIZE(struct gl_payload_draw_elements_instanced, 24); /* <IiIQi    */
    CHECK_SIZE(struct gl_payload_tex_storage_3d_ms,     28);   /* <IiIiiiB3x */
    CHECK_SIZE(struct gl_payload_copy_buffer_sub_data,  32);   /* <IIqqq    */

    printf("\n[test_gl_proto] %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
