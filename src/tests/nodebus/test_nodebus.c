/*
 * test_nodebus.c — host-compiled unit tests for shim_nodebus.c.
 *
 * Compiled with the HOST compiler (not the aarch64 cross-compiler) by
 * _emulation/src/tests/run_tests.sh, which passes -I pointing at _emulation/src/
 * so the bare `#include "spi_shim.h"` / `#include "nodebus/shim_nodebus.c"`
 * below resolve. shim_nodebus.c is portable C; we #include it here so the tests
 * can reach its `static` helpers, and we supply the handful of externs it
 * expects from the rest of the shim (uart_shim_fd, real_write,
 * shim_resolve_game_base, shim_switch_*) as test doubles.
 *
 * This is the layer-1+2 pilot from the nodebus TDD plan: the framing
 * primitives (escape codec, sum-zero checksum) and the bridge-level command
 * contracts that PROTOCOL.md "CPU ↔ netbridge UART bridge protocol" already pins down. cmd-0x41 inner-command
 * contracts come in a later increment, after the live-capture catalog and
 * after the relevant game-side validators are re-verified against the binary.
 *
 * Run:  ../run_tests.sh   (from _emulation/src/tests/: ./run_tests.sh)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

#include "spi_shim.h"      /* real_write_t, SHIM_* defines, extern decls (via -I.../src) */

/* ---- test doubles for the externs shim_nodebus.c pulls from spi_shim.c --- */

/* Capture buffer: bridge_send_response() writes the on-wire response frame
 * via real_write(uart_shim_fd, ...). We point real_write at this. */
#define CAP_MAX 8192
static uint8_t g_cap[CAP_MAX];
static size_t  g_cap_len   = 0;
static int     g_cap_calls = 0;

static ssize_t cap_write(int fd, const void *buf, size_t n) {
    (void)fd;
    g_cap_calls++;
    if (g_cap_len + n <= sizeof(g_cap)) {
        memcpy(g_cap + g_cap_len, buf, n);
        g_cap_len += n;
    }
    return (ssize_t)n;
}
static void cap_reset(void) { g_cap_len = 0; g_cap_calls = 0; }

int          uart_shim_fd = 7;          /* any non-negative; cap_write ignores it */
real_write_t real_write   = cap_write;

/* shim_nodebus.c reads game memory through these when game_base != 0. The
 * pilot doesn't exercise those paths, so a 0 base + zero-fill stubs are
 * enough; the cmd-0x41 increment will swap in a real fixture region. */
uintptr_t shim_resolve_game_base(void) { return 0; }
void shim_switch_copy_channel_idle(uintptr_t gb, uint8_t ch,
                                   uint8_t out[SHIM_SWITCH_CHANNEL_BYTES]) {
    (void)gb; (void)ch;
    memset(out, 0, SHIM_SWITCH_CHANNEL_BYTES);
}
void shim_switch_hold_apply(uint8_t ch, uint8_t buf[SHIM_SWITCH_CHANNEL_BYTES]) {
    (void)ch; (void)buf;
}

/* Don't actually sleep in tests — record the request so timing logic stays
 * assertable. Shadows libc usleep within this TU. */
static unsigned long g_slept_us = 0;
int usleep(useconds_t us) { g_slept_us += us; return 0; }

/* ---- unit under test ------------------------------------------------------ */
#include "nodebus/shim_nodebus.c"   /* resolved via -I.../src ; see header */

/* ---- tiny assertion harness ----------------------------------------------- */
static int g_pass = 0, g_fail = 0;
static const char *g_group = "?";

static void g_begin(const char *name) { g_group = name; }

static void hex(const char *label, const uint8_t *b, size_t n) {
    printf("    %s (%zu):", label, n);
    for (size_t i = 0; i < n; i++) printf(" %02x", b[i]);
    printf("\n");
}

#define CHECK(cond, ...) do {                                              \
    if (cond) { g_pass++; }                                                \
    else { g_fail++; printf("  FAIL [%s] ", g_group);                      \
           printf(__VA_ARGS__); printf("  (%s:%d)\n", __FILE__, __LINE__); }\
} while (0)

static void check_bytes(const uint8_t *got, size_t gn,
                        const uint8_t *exp, size_t en, const char *what) {
    if (gn == en && memcmp(got, exp, gn) == 0) { g_pass++; return; }
    g_fail++;
    printf("  FAIL [%s] %s: byte mismatch\n", g_group, what);
    hex("got", got, gn);
    hex("exp", exp, en);
}

/* ---- helpers --------------------------------------------------------------
 * mk_frame builds the on-wire (escape-encoded) bridge frame
 *   STX | cmd | id | payload... | cksum | ETX
 * exactly the way the protocol specifies — INDEPENDENTLY of shim_nodebus.c's
 * own bridge_checksum / bridge_encode_escape, so a bug in either of those
 * can't hide behind an identically-wrong expectation. Returns wire length. */
static size_t mk_frame(uint8_t *out, uint8_t cmd, uint8_t id,
                       const uint8_t *payload, size_t pn) {
    uint8_t logical[256];
    size_t L = 0;
    logical[L++] = 0x02;
    logical[L++] = cmd;
    logical[L++] = id;
    if (pn) { memcpy(logical + L, payload, pn); L += pn; }
    uint32_t s = 0;
    for (size_t i = 1; i < L; i++) s += logical[i];   /* sum cmd..payload */
    logical[L++] = (uint8_t)(-(int32_t)s);            /* cksum: makes sum ≡ 0 */
    logical[L++] = 0x03;
    /* escape-encode (independent of bridge_encode_escape) */
    size_t o = 0;
    out[o++] = 0x02;
    for (size_t i = 1; i + 1 < L; i++) {
        uint8_t b = logical[i];
        if (b == 0x02 || b == 0x03 || b == 0x1b) { out[o++] = 0x1b; out[o++] = (uint8_t)(b | 0x80); }
        else out[o++] = b;
    }
    out[o++] = 0x03;
    return o;
}

/* Feed one request to the synthesizer and capture whatever it wrote. */
static void feed(const uint8_t *req, size_t rn) { cap_reset(); synthesize_bridge_response(req, rn); }

/* Assert: feeding (cmd,id,reqpayload) produces exactly one response frame
 * equal to mk_frame(rcmd, id, resppayload). */
static void expect_reply(uint8_t cmd, uint8_t id, const uint8_t *reqp, size_t rpn,
                         uint8_t rcmd, const uint8_t *resp, size_t respn,
                         const char *what) {
    uint8_t req[256]; size_t rn = mk_frame(req, cmd, id, reqp, rpn);
    feed(req, rn);
    uint8_t exp[256]; size_t en = mk_frame(exp, rcmd, id, resp, respn);
    CHECK(g_cap_calls == 1, "%s: expected 1 write, got %d", what, g_cap_calls);
    check_bytes(g_cap, g_cap_len, exp, en, what);
}

/* Assert: feeding this request produces no response (deliberate timeout). */
static void expect_no_reply(uint8_t cmd, uint8_t id, const uint8_t *reqp, size_t rpn,
                            const char *what) {
    uint8_t req[256]; size_t rn = mk_frame(req, cmd, id, reqp, rpn);
    feed(req, rn);
    CHECK(g_cap_calls == 0, "%s: expected no write, got %d", what, g_cap_calls);
}

/* =========================== layer 1: primitives ========================== */

static void test_bridge_checksum(void) {
    g_begin("bridge_checksum");
    /* PROTOCOL.md "CPU ↔ netbridge UART bridge protocol" worked examples: (CMD + ID + cksum) ≡ 0 (mod 256). */
    CHECK(bridge_checksum((const uint8_t[]){0x30, 0x01}, 2) == 0xcf, "0x30+0x01 -> 0xcf");
    CHECK(bridge_checksum((const uint8_t[]){0x20, 0x01}, 2) == 0xdf, "0x20+0x01 -> 0xdf");
    CHECK(bridge_checksum((const uint8_t[]){0x10, 0x01}, 2) == 0xef, "0x10+0x01 -> 0xef");
    /* invariant: frame with its own cksum sums to 0 mod 256 */
    uint8_t b[3] = {0x16, 0x42, 0};
    b[2] = bridge_checksum(b, 2);
    CHECK(((b[0] + b[1] + b[2]) & 0xff) == 0, "cksum zeroes the running sum");
}

static void test_escape_decode(void) {
    g_begin("bridge_decode_escape");
    uint8_t out[64];
    /* A valid-length frame whose ID byte equals STX: on the wire that byte is
     * 0x1b 0x82, which must un-escape to 0x02. logical = 02 30 02 ce 03
     * (cmd 0x30, id 0x02, cksum 0xce); wire = 02 30 1b 82 ce 03. */
    int n = bridge_decode_escape((const uint8_t[]){0x02,0x30,0x1b,0x82,0xce,0x03}, 6, out, sizeof out);
    CHECK(n == 5 && out[0]==0x02 && out[1]==0x30 && out[2]==0x02 && out[3]==0xce && out[4]==0x03,
          "0x1b82 in payload -> 0x02");
    /* A frame whose CMD byte equals ESC: 0x1b 0x9b un-escapes to 0x1b.
     * logical = 02 1b 05 e0 03; wire = 02 1b 9b 05 e0 03. */
    n = bridge_decode_escape((const uint8_t[]){0x02,0x1b,0x9b,0x05,0xe0,0x03}, 6, out, sizeof out);
    CHECK(n == 5 && out[1]==0x1b && out[2]==0x05, "0x1b9b in payload -> 0x1b");
    /* a frame with no reserved bytes passes through unchanged (PROTOCOL.md "CPU ↔ netbridge UART bridge protocol" example) */
    CHECK(bridge_decode_escape((const uint8_t[]){0x02,0x30,0x01,0xcf,0x03}, 5, out, sizeof out) == 5
          && out[0]==0x02 && out[1]==0x30 && out[2]==0x01 && out[3]==0xcf && out[4]==0x03,
          "02 30 01 cf 03 unchanged");
    /* malformed: missing ETX / missing STX / below the 5-byte minimum -> -1 */
    CHECK(bridge_decode_escape((const uint8_t[]){0x02,0x30,0x01,0xcf,0x00}, 5, out, sizeof out) == -1, "no ETX -> -1");
    CHECK(bridge_decode_escape((const uint8_t[]){0x30,0x01,0xcf,0x03}, 4, out, sizeof out) == -1, "no STX -> -1");
    CHECK(bridge_decode_escape((const uint8_t[]){0x02,0x03}, 2, out, sizeof out) == -1, "too short -> -1");
    CHECK(bridge_decode_escape((const uint8_t[]){0x02,0x1b,0x82,0x03}, 4, out, sizeof out) == -1, "4-byte wire -> -1 (below minimum)");
}

static void test_escape_encode_roundtrip(void) {
    g_begin("bridge_encode_escape");
    uint8_t wire[64], back[64];
    /* a payload byte equal to STX must go out as 0x1b 0x82 */
    int wn = bridge_encode_escape((const uint8_t[]){0x02,0x02,0x03}, 3, wire, sizeof wire);
    CHECK(wn == 4 && wire[0]==0x02 && wire[1]==0x1b && wire[2]==0x82 && wire[3]==0x03, "STX payload -> 1b 82");
    /* no reserved bytes: unchanged */
    wn = bridge_encode_escape((const uint8_t[]){0x02,0x30,0x01,0xcf,0x03}, 5, wire, sizeof wire);
    CHECK(wn == 5, "no-escape frame stays 5 bytes");
    /* round-trip over frames carrying every reserved byte in the payload */
    const uint8_t *cases[] = {
        (const uint8_t[]){0x02,0x41,0x07,0x02,0x03,0x1b,0xaa,0x55,0x03},
        (const uint8_t[]){0x02,0x1b,0x02,0x1b,0x03,0x03},
    };
    const size_t lens[] = {9, 6};
    for (int i = 0; i < 2; i++) {
        wn = bridge_encode_escape(cases[i], lens[i], wire, sizeof wire);
        CHECK(wn > 0, "encode case %d ok", i);
        int bn = bridge_decode_escape(wire, (size_t)wn, back, sizeof back);
        CHECK(bn == (int)lens[i] && memcmp(back, cases[i], lens[i]) == 0, "decode(encode(case %d)) == case %d", i, i);
    }
}

static void test_fill_checked_inner(void) {
    g_begin("nodebus_fill_checked_inner");
    uint8_t d[6] = {0xaa, 0xbb, 0xcc, 0x00, 0x99, 0x99};   /* size 6 -> cksum at [4] */
    nodebus_fill_checked_inner(d, 6);
    uint32_t s = 0; for (int i = 0; i < 5; i++) s += d[i];   /* sum [0..4] (incl. cksum) */
    CHECK((s & 0xff) == 0, "cksum at [n-2] zeroes sum of [0..n-2]");
    /* inner_size < 2: no-op */
    uint8_t e[1] = {0x77};
    nodebus_fill_checked_inner(e, 1);
    CHECK(e[0] == 0x77, "size<2 is a no-op");
}

static void test_cipher_involution(void) {
    g_begin("nodebus_cipher_decrypt (involution)");
    /* XOR cipher is its own inverse regardless of key/mask details, so a
     * double-apply must be the identity. (The exact mask formula vs. the
     * "NODEBUS_Cipher — Stern's per-slot XOR stream cipher" reference still needs binary re-verification — phase 2.) */
    for (uint8_t slot = 0; slot < 16; slot += 3) {
        for (uint8_t ilen = 1; ilen <= 17; ilen += 4) {
            uint8_t orig[32], buf[32];
            for (int i = 0; i < 32; i++) orig[i] = (uint8_t)(0x37 * (i + 1) + slot * 11 + ilen);
            memcpy(buf, orig, 32);
            nodebus_cipher_decrypt(slot, ilen, buf);
            CHECK(memcmp(buf, orig, ilen) != 0 || ilen == 0, "slot=%u ilen=%u: one apply changes bytes", slot, ilen);
            nodebus_cipher_decrypt(slot, ilen, buf);
            CHECK(memcmp(buf, orig, ilen) == 0, "slot=%u ilen=%u: double apply == identity", slot, ilen);
        }
    }
}

/* ====================== layer 2: bridge-level contracts =================== */

static void test_cmd_27_alternates(void) {
    /* MUST run before any other cmd-0x27 trigger: relies on the function-local
     * cmd27_count static starting at 0 in this fresh process. */
    g_begin("cmd 0x27 (LCD poll, alternating)");
    expect_reply(0x27, 0x03, NULL, 0, 0x27, (const uint8_t[]){0x06}, 1, "cmd 0x27 #1 -> 0x06");
    expect_reply(0x27, 0x03, NULL, 0, 0x27, (const uint8_t[]){0x02}, 1, "cmd 0x27 #2 -> 0x02");
    expect_reply(0x27, 0x03, NULL, 0, 0x27, (const uint8_t[]){0x06}, 1, "cmd 0x27 #3 -> 0x06");
}

static void test_cmd_30_crc32(void) {
    g_begin("cmd 0x30 (netbridge CRC32 handshake)");
    /* PROTOCOL.md "CPU ↔ netbridge UART bridge protocol": response cmd 0x20, payload = NETBRIDGE_CRC32 little-endian. */
    uint8_t crc_le[4] = {0x27, 0x53, 0x7f, 0x35};   /* 0x357f5327 LE */
    expect_reply(0x30, 0x01, NULL, 0, 0x20, crc_le, 4, "cmd 0x30 -> cmd 0x20 + CRC32 LE");
}

static void test_cmd_10_pwrdist_version(void) {
    g_begin("cmd 0x10 (pwrdist version)");
    /* PROTOCOL.md "CPU ↔ netbridge UART bridge protocol" "Pwrdist 'present' response payload": 7 bytes, byte0=0x01. */
    uint8_t pl[7] = {0x01, 0, 0, 0, 0, 0, 0};
    expect_reply(0x10, 0x01, NULL, 0, 0x10, pl, 7, "cmd 0x10 -> 7-byte present payload");
}

static void test_cmd_16_status(void) {
    g_begin("cmd 0x16 (pwrdist boot/runtime version)");
    /* PROTOCOL.md "CPU ↔ netbridge UART bridge protocol" "Cmd 0x16 response payload": 01 00 02 00 (rx[0]>0, rx[2]==2). */
    uint8_t reqp[1] = {0x00};
    expect_reply(0x16, 0x01, reqp, 1, 0x16, (const uint8_t[]){0x01, 0x00, 0x02, 0x00}, 4, "cmd 0x16 -> 01 00 02 00");
}

static void test_cmd_1d_pwrdist_status(void) {
    g_begin("cmd 0x1d (pwrdist rail/interlock, 4 bytes)");
    uint8_t reqp[4] = {0, 0, 0, 0};
    expect_reply(0x1d, 0x01, reqp, 4, 0x1d, (const uint8_t[]){0xff, 0xff, 0xff, 0xff}, 4, "cmd 0x1d -> ff ff ff ff");
}

static void test_cmd_15_18_byte_acks(void) {
    g_begin("cmd 0x15 / 0x18 (single-byte ACKs)");
    expect_reply(0x15, 0x02, NULL, 0, 0x15, (const uint8_t[]){0x00}, 1, "cmd 0x15 -> 0x00");
    uint8_t r[1] = {0xaa};
    expect_reply(0x18, 0x02, r, 1, 0x18, (const uint8_t[]){0x00}, 1, "cmd 0x18 -> 0x00");
}

static void test_cmd_11_1c_zero_acks(void) {
    g_begin("cmd 0x11 / 0x1c (zero-payload ACKs)");
    expect_reply(0x11, 0x00, NULL, 0, 0x11, NULL, 0, "cmd 0x11 -> 0-byte ACK");
    expect_reply(0x1c, 0x05, NULL, 0, 0x1c, NULL, 0, "cmd 0x1c -> 0-byte ACK");
}

static void test_cmd_20_hw_version(void) {
    g_begin("cmd 0x20 (hw version — note: replies with cmd 0x10)");
    /* The shim answers cmd 0x20 with response cmd 0x10 + 4 zero bytes. PROTOCOL
     * "CPU ↔ netbridge UART bridge protocol" flags this as a divergence from the bridge's real reply; the test
     * pins the current behavior so a future fix is a deliberate change. */
    uint8_t z4[4] = {0, 0, 0, 0};
    expect_reply(0x20, 0x01, NULL, 0, 0x10, z4, 4, "cmd 0x20 -> cmd 0x10 + 4 zero bytes");
}

/* Build the cmd-0x41 inner-cmd-0xF0 enum-reset frame (slot 0, inner_cmd 0xF0,
 * payload byte 0x22). The reset detector checks slot/inner_cmd/payload[3]==0x22;
 * the cipher sum-checksum byte isn't validated, so we just supply a real one. */
static size_t mk_f0_reset(uint8_t *out, uint8_t id) {
    uint8_t cipher[5] = {0x80, 0x02, 0xF0, 0x22, 0};   /* slot|0x80, ilen=2, 0xF0, 0x22, csum */
    uint32_t s = 0; for (int i = 0; i < 4; i++) s += cipher[i];
    cipher[4] = (uint8_t)(-(int32_t)s);
    return mk_frame(out, 0x41, id, cipher, sizeof cipher);
}

static void test_cmd_25_enum_cursor_and_f0_reset(void) {
    g_begin("cmd 0x25 enum cursor + 0xF0 reset");
    nodebus_enum_idx = 999;                       /* scribble — reset must clear it */

    uint8_t req[64]; size_t rn = mk_f0_reset(req, 0x00);
    feed(req, rn);
    CHECK(g_cap_calls == 0, "0xF0 reset produces no response");
    CHECK(nodebus_enum_idx == 0, "0xF0 reset clears the enum cursor");

    /* The shim yields the 11 populated slots, then 0 (and 0 forever after). */
    static const uint8_t want[] = {1, 2, 4, 7, 8, 9, 10, 12, 13, 14, 0, 0};
    for (int i = 0; i < (int)(sizeof want); i++)
        expect_reply(0x25, 0x05, NULL, 0, 0x25, &want[i], 1, "cmd 0x25 yield");

    rn = mk_f0_reset(req, 0x00);
    feed(req, rn);
    CHECK(nodebus_enum_idx == 0, "second 0xF0 reset re-clears the cursor");
    expect_reply(0x25, 0x05, NULL, 0, 0x25, (const uint8_t[]){1}, 1, "cmd 0x25 after reset yields 1 again");
}

static void test_deliberate_timeouts(void) {
    g_begin("deliberate timeouts (no synthetic reply)");
    /* unknown bridge cmd */
    expect_no_reply(0x99, 0x01, NULL, 0, "unknown cmd 0x99 -> timeout");
    /* cmd 0x40 cleartext sub-protocol — logged, never answered */
    expect_no_reply(0x40, 0x01, (const uint8_t[]){0x05}, 1, "cmd 0x40 sub 0x05 -> timeout");
    /* cmd 0x41 frame too short to carry a cipher frame -> bail, no reply */
    uint8_t tiny[] = {0x02, 0x41, 0x01, 0x03};
    feed(tiny, sizeof tiny);
    CHECK(g_cap_calls == 0, "cmd 0x41 short frame -> no reply");
}

/* =============================== main ===================================== */
int main(void) {
    /* layer 1 — framing primitives (order-independent) */
    test_bridge_checksum();
    test_escape_decode();
    test_escape_encode_roundtrip();
    test_fill_checked_inner();
    test_cipher_involution();

    /* layer 2 — bridge-level command contracts.
     * cmd 0x27 first: it depends on a function-local static starting at 0. */
    test_cmd_27_alternates();
    test_cmd_30_crc32();
    test_cmd_10_pwrdist_version();
    test_cmd_16_status();
    test_cmd_1d_pwrdist_status();
    test_cmd_15_18_byte_acks();
    test_cmd_11_1c_zero_acks();
    test_cmd_20_hw_version();
    test_cmd_25_enum_cursor_and_f0_reset();
    test_deliberate_timeouts();

    printf("\n[test_nodebus] %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
