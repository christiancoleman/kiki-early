





























#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>

#include "spi_shim.h"

/* Defined in spi_shim.c — the shim-side end of the UART socketpair, and the
 * libc write() function pointer resolved once in resolve_all(). */
extern int uart_shim_fd;
typedef ssize_t (*real_write_t)(int, const void *, size_t);
extern real_write_t real_write;

/* ========== moved from spi_shim.c (3898-4232): netbridge protocol: CRC32 + checksum + escape codec + send_response + synthesize_bridge_response ========== */
/* ---- Bridge protocol response synthesis ----
 *
 * Per Sidekick Round 4 RE of sub_d164a0 (send_bridge_message_with_response)
 * and the parser in sub_d15aa8:
 *
 *   Request frame:  [0x02][CMD][ID][payload...][CKSUM][0x03]
 *   Response frame: [0x02][RESP_CMD][ID][payload...][CKSUM][0x03]
 *   Checksum:       CMD + ID + payload... + CKSUM == 0 (mod 256)
 *   ID:             Channel ID; response MUST echo the request's ID
 *
 * Request-to-response CMD mapping and payload size:
 *   0x10 (pwrdist version query) -> 0x00 (7 payload bytes)
 *   0x20 (hw version query)      -> 0x10 (4 payload bytes)
 *   0x30 (expected CRC query)    -> 0x20 (4 payload bytes)
 *
 * For 0x30 we return the zlib CRC32 of netbridge.elf's .text+.rodata+
 * .binary_info sections, computed offline in Step 13.5 (journal).
 *
 * For 0x10 we return seven zero bytes. sub_d19580 interprets a zero
 * version as "Spike 1/2 board — use fallback, don't flash powerdist" —
 * which is exactly what we want in emulation (no real powerdist to flash).
 *
 * For 0x20 we return four zero bytes; the game stores the value into
 * data_490e644/data_490e648 without (as far as we've seen) validating.
 */

#define NETBRIDGE_CRC32  0x357f5327u

static uint8_t bridge_checksum(const uint8_t *bytes, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) sum += bytes[i];
    return (uint8_t)(-(int32_t)sum);
}

/* ---- Bridge frame escape encoding ----
 *
 * The Spike 3 bridge protocol reserves three byte values for framing:
 *   0x02 = STX (start of frame)
 *   0x03 = ETX (end of frame)
 *   0x1b = ESC (next data byte has bit 7 cleared)
 *
 * Any CMD/ID/PAYLOAD/CKSUM byte that equals one of these three values is
 * transmitted as `0x1b (byte | 0x80)`. The receiver's byte-by-byte parser
 * in sub_d15aa8 (see file 0x915c38 — the `cmp w0, #0x1b` and the
 * `bic w0, w0, w26` at 0x915c10) un-escapes by AND-ing with ~0x80.
 *
 * We discovered this via cmd 0x18 responses where id=0x02 (channel 2) was
 * sent by the game as `02 18 1b 82 cksum 03` (escape-encoded). Our naive
 * req[2] extraction read 0x1b as the channel and sent back a malformed
 * frame, which the receiver then rejected with "bridge rx checksum error".
 */

/* Decode an incoming escape-encoded frame into its logical bytes. Returns
 * decoded length including STX/ETX, or -1 on malformed input. */
static int bridge_decode_escape(const uint8_t *raw, size_t raw_len,
                                uint8_t *out, size_t out_cap) {
    if (raw_len < 5 || raw[0] != 0x02 || raw[raw_len - 1] != 0x03) return -1;
    if (out_cap < 2) return -1;
    out[0] = 0x02;
    size_t o = 1;
    for (size_t i = 1; i < raw_len - 1; i++) {
        if (o >= out_cap - 1) return -1;
        if (raw[i] == 0x1b && i + 1 < raw_len - 1) {
            out[o++] = raw[i + 1] & 0x7f;
            i++;
        } else {
            out[o++] = raw[i];
        }
    }
    out[o++] = 0x03;
    return (int)o;
}

/* Encode a logical frame into its on-wire (escape-encoded) form. Returns
 * encoded length. STX (byte 0) and ETX (last byte) are emitted literally;
 * any 0x02/0x03/0x1b byte between them is prefixed with 0x1b. */
static int bridge_encode_escape(const uint8_t *logical, size_t logical_len,
                                uint8_t *out, size_t out_cap) {
    if (logical_len < 2 || logical[0] != 0x02 ||
        logical[logical_len - 1] != 0x03 || out_cap < 1) return -1;
    out[0] = 0x02;
    size_t o = 1;
    for (size_t i = 1; i < logical_len - 1; i++) {
        uint8_t b = logical[i];
        if (b == 0x02 || b == 0x03 || b == 0x1b) {
            if (o + 2 >= out_cap) return -1;
            out[o++] = 0x1b;
            out[o++] = b | 0x80;
        } else {
            if (o + 1 >= out_cap) return -1;
            out[o++] = b;
        }
    }
    if (o + 1 > out_cap) return -1;
    out[o++] = 0x03;
    return (int)o;
}

static void bridge_send_response(const uint8_t *resp, size_t resp_len,
                                 uint8_t req_cmd, uint8_t id) {
    if (uart_shim_fd < 0) return;
    fprintf(stderr, "[shim] bridge RESP to cmd=0x%02x id=0x%02x (%zu logical bytes):",
            req_cmd, id, resp_len);
    for (size_t i = 0; i < resp_len; i++) fprintf(stderr, " %02x", resp[i]);
    fprintf(stderr, "\n");
    uint8_t wire[64];
    int wire_len = bridge_encode_escape(resp, resp_len, wire, sizeof(wire));
    if (wire_len < 0) {
        SHIM_LOG("bridge: encode_escape failed");
        return;
    }
    if ((size_t)wire_len != resp_len) {
        fprintf(stderr, "[shim]   (escape-encoded to %d bytes:", wire_len);
        for (int i = 0; i < wire_len; i++) fprintf(stderr, " %02x", wire[i]);
        fprintf(stderr, ")\n");
    }
    ssize_t n = real_write(uart_shim_fd, wire, wire_len);
    if (n < 0) SHIM_LOG("bridge: real_write response failed: %s", strerror(errno));
}

void synthesize_bridge_response(const uint8_t *raw_req, size_t raw_len) {
    uint8_t req[64];
    int req_len = bridge_decode_escape(raw_req, raw_len, req, sizeof(req));
    if (req_len < 5) return;
    uint8_t cmd = req[1];
    uint8_t id  = req[2];

    uint8_t resp[16];

    switch (cmd) {
    case 0x30: {
        /* Response 0x20 with 4-byte expected CRC (little-endian). */
        resp[0] = 0x02;
        resp[1] = 0x20;
        resp[2] = id;
        resp[3] = (uint8_t)(NETBRIDGE_CRC32 >>  0);
        resp[4] = (uint8_t)(NETBRIDGE_CRC32 >>  8);
        resp[5] = (uint8_t)(NETBRIDGE_CRC32 >> 16);
        resp[6] = (uint8_t)(NETBRIDGE_CRC32 >> 24);
        resp[7] = bridge_checksum(resp + 1, 6);
        resp[8] = 0x03;
        bridge_send_response(resp, 9, cmd, id);
        return;
    }
    case 0x20: {
        /* Response 0x10 with 4-byte hardware version (all zeros for now). */
        resp[0] = 0x02;
        resp[1] = 0x10;
        resp[2] = id;
        resp[3] = 0x00; resp[4] = 0x00; resp[5] = 0x00; resp[6] = 0x00;
        resp[7] = bridge_checksum(resp + 1, 6);
        resp[8] = 0x03;
        bridge_send_response(resp, 9, cmd, id);
        return;
    }
    case 0x10: {
        /* Response cmd 0x10 with 7-byte pwrdist version payload.
         *
         * IMPORTANT: the response cmd must be in range [0x10, 0x41]. The
         * dispatch in sub_d15aa8 checks (cmd - 0x10) <= 0x31; anything
         * outside that range falls through to "BRIDGE_ProcessMessage
         * unknown command" which still signals the waiting channel (so
         * send_bridge_message_with_response returns 0) but the dispatch
         * handler that writes data_490e61c never runs. Sidekick's earlier
         * "0x10 -> 0x00" mapping turned out to be off; the actual response
         * cmd for pwrdist version is 0x10 (matches the request cmd).
         *
         * data_490e61c is set by the dispatch handler as roughly
         * (payload[0] << 16) | (payload[1] << 8). Non-zero value puts the
         * game on the "has Spike 3 pwrdist board" path, which calls
         * sub_d18c20("powerdist_ext.hex", 0). If sub_d18c20 shells out to
         * openocd, our system() hook returns 0 and the flash "succeeds",
         * then the game re-queries (we return the same response), ver
         * stays non-zero, sub_d19580 returns 0 (x19 untouched from its
         * initial 0). */
        resp[0]  = 0x02;
        resp[1]  = 0x10;
        resp[2]  = id;
        resp[3]  = 0x01;
        resp[4]  = 0x00;
        resp[5]  = 0x00;
        resp[6]  = 0x00;
        resp[7]  = 0x00;
        resp[8]  = 0x00;
        resp[9]  = 0x00;
        resp[10] = bridge_checksum(resp + 1, 9);
        resp[11] = 0x03;
        bridge_send_response(resp, 12, cmd, id);
        return;
    }
    case 0x15: {
        /* Pwrdist status (follow-up after 0x10). Handler at file 0x9160a0
         * requires exactly 1 payload byte; OR/XORs it into state word at
         * data_490e5b0+128. First guess: 0x00 = "all clear, no faults". */
        resp[0] = 0x02;
        resp[1] = 0x15;
        resp[2] = id;
        resp[3] = 0x00;
        resp[4] = bridge_checksum(resp + 1, 3);
        resp[5] = 0x03;
        bridge_send_response(resp, 6, cmd, id);
        return;
    }
    case 0x18: {
        /* Pwrdist followup #2. Handler at file 0x91607c records min(existing,
         * payload_len) into a halfword table at data_490e5b0+0x88[id]; the
         * value isn't read. Send minimal 1-byte payload with value 0x00. */
        resp[0] = 0x02;
        resp[1] = 0x18;
        resp[2] = id;
        resp[3] = 0x00;
        resp[4] = bridge_checksum(resp + 1, 3);
        resp[5] = 0x03;
        bridge_send_response(resp, 6, cmd, id);
        return;
    }
    case 0x1c: {
        /* Pwrdist firmware flash chunk write. Jump table routes cmd 0x1c to
         * the generic "silent no-op + ACK" handler at file 0x915d28 (shared
         * with cmds 0x11/0x13/0x16/0x17/0x19/0x21/0x23–0x25/0x30/0x31). A
         * zero-payload frame takes the cbnz-w27 path and signals the waiting
         * channel, which is what send_bridge_message_with_response wants to
         * unblock. Game treats that as "chunk write acknowledged." */
        resp[0] = 0x02;
        resp[1] = 0x1c;
        resp[2] = id;
        resp[3] = bridge_checksum(resp + 1, 2);
        resp[4] = 0x03;
        bridge_send_response(resp, 5, cmd, id);
        return;
    }
    case 0x11: {
        /* Jump table routes cmd 0x11 to the generic silent-ACK fallback at
         * 0x915d28 (same family as 0x1c). Zero-payload frame unblocks the
         * channel; no state is written. */
        resp[0] = 0x02;
        resp[1] = 0x11;
        resp[2] = id;
        resp[3] = bridge_checksum(resp + 1, 2);
        resp[4] = 0x03;
        bridge_send_response(resp, 5, cmd, id);
        return;
    }
    case 0x16: {
        /* Cmd 0x16 routes to the shared handler path at 0x915d28. That path
         * has two branches:
         *   - payload_len == 0 AND id == 0: pure ACK, no buffer write.
         *   - payload_len > 0: falls to 0x915e8c which memcpys the payload
         *     into the channel's rx_buf registered at
         *     data_490e5b0+0xa0+id*8.
         * The caller at 0x675c20 (wrapper sub_917b60) sends cmd 0x16 with a
         * 1-byte tx payload (0x00) and expects a 4-byte response. It then
         * polls up to 1000×1ms waiting for:
         *   return value (rx[0]) > 0  AND  rx[2] == 0x02
         * Values for rx[1] and rx[3] are stored to caller out-ptrs but not
         * tested. Send `01 00 02 00` so the poll exits immediately. */
        resp[0] = 0x02;
        resp[1] = 0x16;
        resp[2] = id;
        resp[3] = 0x01;
        resp[4] = 0x00;
        resp[5] = 0x02;
        resp[6] = 0x00;
        resp[7] = bridge_checksum(resp + 1, 6);
        resp[8] = 0x03;
        bridge_send_response(resp, 9, cmd, id);
        return;
    }
    case 0x1d: {
        /* Dedicated handler at 0x916028: `cmp w27,#4 / b.eq 0x915e88`. If
         * payload_len == 4 it memcpys into the per-id buffer at
         * data_490e5b0+0xa0+id*8; otherwise it falls through to the silent-
         * ACK fallback at 0x915d28. Sending 0-byte payload takes the else
         * path — unblocks the waiting channel without writing stale data
         * into a buffer whose semantics we haven't yet verified (plausibly
         * pwrdist status bits + rail state + ADC reading, per FotE §4.16). */
        resp[0] = 0x02;
        resp[1] = 0x1d;
        resp[2] = id;
        resp[3] = bridge_checksum(resp + 1, 2);
        resp[4] = 0x03;
        bridge_send_response(resp, 5, cmd, id);
        return;
    }
    case 0x27: {
        /* Four game-side callsites polling a 1-byte response byte:
         *   0x916828 one-shot getter (stores resp[0] to caller out-ptr)
         *   0x916fe0 loop: spin until bit 2 == 1
         *   0x917020 loop: spin until bit 2 == 0  (paired with 916fe0)
         *   0x917264 loop: spin (100ms × ≤100) until (resp & 3) == 2
         * The paired loop at 916fe0/917020 requires bit 2 to toggle 1→0
         * within the same function call, so a static response stalls one
         * of the two halves. Alternate odd/even calls:
         *   odd  -> 0x06 (bits 1,2 set; resp&3 == 2)
         *   even -> 0x02 (bit 1 set only; resp&3 == 2)
         * Both values satisfy 917264; alternation resolves the pair. */
        static unsigned int cmd27_count = 0;
        cmd27_count++;
        uint8_t status = (cmd27_count & 1) ? 0x06 : 0x02;
        resp[0] = 0x02;
        resp[1] = 0x27;
        resp[2] = id;
        resp[3] = status;
        resp[4] = bridge_checksum(resp + 1, 3);
        resp[5] = 0x03;
        bridge_send_response(resp, 6, cmd, id);
        SHIM_LOG("bridge: cmd 0x27 #%u -> status 0x%02x (id=0x%02x)",
                 cmd27_count, status, id);
        return;
    }
    /* Step 45: cmd 0x25 / 0x41 silent-ACK reverted.
     *
     * ATTEMPTED: zero-payload ACK for both. Unblocked the send-side channel
     * wait, but progress REGRESSED — the crashing run reached Godot init +
     * Radium + pokemon_battle_scene + texture 400 before SEGV. With ACKs
     * enabled, the game never reached Godot at all in 90 s (stdout stopped
     * at the /data/ mkdir errors; no Godot Engine banner; texture/shader
     * counts unchanged from pre-init steady state).
     *
     * Interpretation: the previous timeout path WAS the progress path. Game
     * sends cmd 0x41, waits 1000 ms, times out, logs "Connection timed out",
     * falls back to default/null state, proceeds to Godot. Our fast zero-
     * payload ACK arrives before the timeout fires; game interprets "ACK
     * without the expected status payload" as "keep polling", and spins.
     *
     * cmd 0x41 dispatches to dedicated handler 0x915f88 (PROTOCOL.md) with
     * payload contract unknown. Resolving it properly needs a BN dig we
     * don't want to spend budget on today. For now: let the timeout fire,
     * memcpy NULL-guard catches the downstream PopupMenu SEGV.
     */
    default:
        SHIM_LOG("bridge: no synthetic response for cmd 0x%02x", cmd);
        return;
    }
}

