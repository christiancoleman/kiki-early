





























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

/* Per-slot 16-byte cipher key blocks for the encrypted cmd-0x41 channel.
 * Mirrors the game binary's mcp_nodebus_cipher_keys @ 0x44fcfb8 (32 slots ×
 * 16 bytes). Zero at init = the boot state. The shim doesn't synthesize
 * responses to the key-fetch commands (inner cmds 0xfc / 0xfd / 0xf9), so the
 * game's per-slot keys also stay zero — meaning every cmd-0x41 frame the
 * game emits is encrypted under a zero key block. We reproduce that here so
 * decryption matches.
 *
 * Algorithm and full RE notes: docs/NODEBUS_CIPHER.md.
 *
 * If we ever start synthesizing key-fetch responses, this table is where we
 * record the (fake) key bytes we hand out, so subsequent traffic decrypts
 * with those keys instead.
 */
static uint8_t nodebus_cipher_keys[32][16] = {{0}};

/* Experimental cmd-0x41 inner-cmd response synthesis. Off by default — set
 * NODEBUS_SYNTH_FE=1 in the environment to enable. The shim will then emit a
 * bridge response for fe_probe traffic (decoded inner_cmd 0xFE / 0xFA) instead
 * of letting the game time out.
 *
 * Step 45 lesson stands: zero-length ACKs regressed progress. This response is
 * a STRUCTURED "node not present" payload — byte 0 has its high bit clear
 * (game reads that as "no node"), all other bytes zero. var_34 (the chip-ID
 * field at response offset 4) reads zero, which doesn't match any of the 28
 * mcp_lpc_chip_vtable entries — so the game falls through to the default at
 * data_42317a0 (NULL fps), exactly as it does today on timeout.
 *
 * Goal: shave the ~300ms-per-slot timeout by short-circuiting fe_probe with a
 * known-shape response, and confirm wire framing assumptions are right before
 * we attempt anything more interesting (claiming a specific chip type).
 *
 * We resolve the env var lazily on first cmd-0x41 frame so an unset shim
 * stays silent. Set once, stays cached for the process lifetime. */
static int nodebus_synth_fe_state = -1;  /* -1 = unresolved, 0 = off, 1 = on */
static int nodebus_synth_fe_enabled(void) {
    if (nodebus_synth_fe_state == -1) {
        const char *v = getenv("NODEBUS_SYNTH_FE");
        nodebus_synth_fe_state = (v && v[0] && v[0] != '0') ? 1 : 0;
        SHIM_LOG("nodebus: NODEBUS_SYNTH_FE=%s (env %s)",
                 nodebus_synth_fe_state ? "ENABLED" : "disabled",
                 v ? v : "<unset>");
    }
    return nodebus_synth_fe_state;
}

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

/* Walk the ARM64 frame-pointer chain from our own frame, capturing return
 * addresses up to MAX_DEPTH frames. Both the shim and the game are aarch64
 * (running under qemu-aarch64 TCG), so they share the same stack discipline:
 * x29 holds FP; at [FP] is the saved parent FP, at [FP+8] is the saved LR.
 *
 * This lets us identify the game-side function that ultimately wrote a
 * cmd-0x41 frame (callsite of mcp_nodebus_send_raw → bridge → write()).
 *
 * Filters: only return addresses inside the game binary's .text range get
 * logged. (.text @ 0x460000, size 0x03167100 → upper bound 0x35c7100.) Shim-
 * internal frames and libc frames are skipped.
 */
#define GAME_TEXT_LO 0x0000460000ULL
#define GAME_TEXT_HI 0x00035c7100ULL  /* exclusive */

static void log_caller_chain(const char *prefix) {
    char line[1024];
    int n = snprintf(line, sizeof(line), "[shim]   %s caller chain (raw):", prefix);
    uintptr_t fp = (uintptr_t)__builtin_frame_address(0);
    n += snprintf(line + n, sizeof(line) - n, " fp_start=0x%lx", fp);
    for (int depth = 0; depth < 12 && fp; depth++) {
        if (fp & 0xF) {
            n += snprintf(line + n, sizeof(line) - n, " [d%d unaligned 0x%lx]", depth, fp);
            break;
        }
        if (fp < 0x1000 || fp > 0x800000000000ULL) {
            n += snprintf(line + n, sizeof(line) - n, " [d%d out-of-range fp=0x%lx]", depth, fp);
            break;
        }
        uintptr_t next_fp = *(volatile uintptr_t*)fp;
        uintptr_t ret    = *(volatile uintptr_t*)(fp + 8);
        const char *zone = (ret >= GAME_TEXT_LO && ret < GAME_TEXT_HI) ? "GAME" : "extn";
        n += snprintf(line + n, sizeof(line) - n, " d%d=%s:0x%lx(fp->0x%lx)",
                      depth, zone, ret, next_fp);
        if (ret == 0) break;
        if (next_fp == 0 || next_fp <= fp || next_fp - fp > 0x100000) break;
        fp = next_fp;
    }
    n += snprintf(line + n, sizeof(line) - n, "\n");
    if (n > (int)sizeof(line)) n = (int)sizeof(line);
    (void)write(STDERR_FILENO, line, (size_t)n);
}

/* Decrypt the inner_cmd + payload portion of a cmd-0x41 frame in place.
 *
 * Mirrors mcp_nodebus_send_encrypted's encryption loop in the game binary
 * (sub_8ec410, the "high bit set" path, BN HLIL line 0x8ec5dc). The cipher
 * is involutive (XOR with key + alternating mask), so the same operation
 * undoes encryption.
 *
 * `slot` is arg1[0] & 0x1F from the cipher frame. `inner_len` is the cipher
 * frame's arg1[1] (count of bytes to encrypt: inner_cmd + payload). `buf`
 * points at the first encrypted byte (inner_cmd), and is overwritten with
 * the decrypted bytes.
 *
 * See docs/NODEBUS_CIPHER.md § 1.2 for the algorithm.
 */
static void nodebus_cipher_decrypt(uint8_t slot, uint8_t inner_len, uint8_t *buf) {
    uint8_t mask = (uint8_t)(((slot & 0x1F) << 4) | ((inner_len + 1) & 0x0F));
    for (int i = 0; i < inner_len; i++) {
        uint8_t k_table = nodebus_cipher_keys[slot & 0x1F][i & 0x0F];
        uint8_t k_mask  = (i & 1) ? (uint8_t)(~mask) : mask;
        buf[i] ^= (uint8_t)(k_table ^ k_mask);
    }
}

static void bridge_send_response(const uint8_t *resp, size_t resp_len,
                                 uint8_t req_cmd, uint8_t id) {
    if (uart_shim_fd < 0) return;
    /* Build the response log into a single buffer + one write() so it
     * stays atomic against concurrent SHIM_LOG calls from other threads. */
    {
        char line[512];
        int n = snprintf(line, sizeof(line),
                         "[shim] bridge RESP to cmd=0x%02x id=0x%02x (%zu logical bytes):",
                         req_cmd, id, resp_len);
        for (size_t i = 0; i < resp_len && n < (int)sizeof(line); i++)
            n += snprintf(line + n, sizeof(line) - n, " %02x", resp[i]);
        if (n < (int)sizeof(line))
            n += snprintf(line + n, sizeof(line) - n, "\n");
        if (n > (int)sizeof(line)) n = (int)sizeof(line);
        (void)write(STDERR_FILENO, line, (size_t)n);
    }
    uint8_t wire[64];
    int wire_len = bridge_encode_escape(resp, resp_len, wire, sizeof(wire));
    if (wire_len < 0) {
        SHIM_LOG("bridge: encode_escape failed");
        return;
    }
    if ((size_t)wire_len != resp_len) {
        char line[512];
        int n = snprintf(line, sizeof(line),
                         "[shim]   (escape-encoded to %d bytes:", wire_len);
        for (int i = 0; i < wire_len && n < (int)sizeof(line); i++)
            n += snprintf(line + n, sizeof(line) - n, " %02x", wire[i]);
        if (n < (int)sizeof(line))
            n += snprintf(line + n, sizeof(line) - n, ")\n");
        if (n > (int)sizeof(line)) n = (int)sizeof(line);
        (void)write(STDERR_FILENO, line, (size_t)n);
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
     *
     * Step 67 update: cmd 0x40 / 0x41 senders identified. Cmd 0x41 is the
     * encrypted node-bus channel (sub_8ec410 → sub_9166c0); cmd byte is
     * runtime-computed. We don't synthesize responses, but we DO decrypt the
     * outbound payload here for visibility — see cases below + NODEBUS_CIPHER.md.
     */
    case 0x40: {
        /* Cleartext bridge sub-protocol. payload[0] is the sub-cmd byte.
         * Today known: 0x05 = snapshot+clear bridgeStatus,
         *              0x0c = factory test trap (NEVER respond — bridge enters
         *              an infinite loop intentionally),
         *              0x0d = NODEBUS_Hash (not currently sent by this game).
         * No synthetic response — game tolerates the timeout (Step 45 lesson).
         */
        if (req_len >= 5) {
            uint8_t sub = req[3];
            SHIM_LOG("bridge: cmd 0x40 sub_cmd=0x%02x payload_len=%d (no synth response)",
                     sub, req_len - 5);
        } else {
            SHIM_LOG("bridge: cmd 0x40 (req too short, no sub-cmd byte)");
        }
        return;
    }
    case 0x41: {
        /* Encrypted node-bus channel. Decrypt inner_cmd + payload for log
         * visibility. We still don't synthesize a response — keys-stay-zero
         * boot state (Step 45 lesson stands).
         *
         * Bridge frame layout after escape decode:
         *   req[0]=STX  req[1]=cmd  req[2]=id
         *   req[3..req_len-3] = bridge payload (= cipher frame)
         *   req[req_len-2]=bridge-csum  req[req_len-1]=ETX
         *
         * Cipher frame layout inside that bridge payload:
         *   payload[0]            = (slot & 0x1F) | 0x80
         *   payload[1]            = inner_len  (count of bytes after this)
         *   payload[2..2+ilen-1]  = encrypted bytes (inner_cmd then payload)
         *   payload[2+inner_len]  = cipher sum-checksum
         */
        if (req_len < 6 /* STX cmd id slot ilen ETX is the bare minimum */) {
            SHIM_LOG("bridge: cmd 0x41 frame too short (req_len=%d)", req_len);
            return;
        }
        const uint8_t *payload = &req[3];
        int payload_len = req_len - 5;  /* minus STX, cmd, id, csum, ETX */
        if (payload_len < 3) {
            SHIM_LOG("bridge: cmd 0x41 payload too short (%d bytes)", payload_len);
            return;
        }
        uint8_t slot_byte = payload[0];
        uint8_t slot      = slot_byte & 0x1F;
        int     enc_flag  = (slot_byte & 0x80) ? 1 : 0;
        uint8_t inner_len = payload[1];
        if (2 + (int)inner_len + 1 > payload_len) {
            SHIM_LOG("bridge: cmd 0x41 slot=%u inner_len=%u doesn't fit in payload (%d)",
                     slot, inner_len, payload_len);
            return;
        }
        if (inner_len == 0) {
            /* Special: zero-length inner means this is a 1-byte zero-ping
             * (sub_8ec410 first branch). Should never carry the encrypt flag,
             * but log it cleanly either way. */
            SHIM_LOG("bridge: cmd 0x41 slot=%u zero-ping (no inner_cmd)", slot);
            return;
        }
        uint8_t scratch[64];
        if (inner_len > sizeof(scratch)) {
            SHIM_LOG("bridge: cmd 0x41 slot=%u inner_len=%u exceeds scratch buffer",
                     slot, inner_len);
            return;
        }
        memcpy(scratch, &payload[2], inner_len);
        if (enc_flag) {
            nodebus_cipher_decrypt(slot, inner_len, scratch);
        }
        /* scratch[0] = decoded inner_cmd, scratch[1..inner_len-1] = decoded payload bytes */
        char line[512];
        int n = snprintf(line, sizeof(line),
                         "[shim] bridge: cmd 0x41 slot=%u %s inner_cmd=0x%02x payload=[",
                         slot, enc_flag ? "ENC" : "CLR", scratch[0]);
        for (int i = 1; i < inner_len && n < (int)sizeof(line) - 4; i++)
            n += snprintf(line + n, sizeof(line) - n,
                          (i == 1 ? "%02x" : " %02x"), scratch[i]);
        if (n < (int)sizeof(line) - 2) {
            line[n++] = ']';
            line[n++] = '\n';
        }
        if (n > (int)sizeof(line)) n = (int)sizeof(line);
        (void)write(STDERR_FILENO, line, (size_t)n);
        log_caller_chain("cmd 0x41");

        /* Optional cmd-0x41 response synthesis. Behind NODEBUS_SYNTH_FE flag.
         *
         * Wire format from netbridge.elf RE (case 0x21 in HOST_Update):
         *   request:  [STX][0x41][id][slot|0x80][inner_len][cipher_region][csum][ETX]
         *   response: [STX][0x41][id][N bytes payload][csum][ETX]
         *
         * where:
         *   - cipher_region is (inner_len + 1) encrypted bytes:
         *       offset 2: inner_cmd
         *       offset 3..2+inner_len-1: payload bytes
         *       offset inner_len+2: HINT byte (response-length determinant)
         *   - N = decoded hint byte (= request[inner_len+2] after decryption)
         *   - First N-1 response payload bytes come from the node bus on real HW
         *   - Last response payload byte = bridgeStatus
         *   - If hint == 0, request is fire-and-forget (no response expected,
         *     game's `if (x23 != 0)` gate skips the receive path)
         *
         * For "no node present" semantics: fill payload with zeros (incl. final
         * bridgeStatus = 0). Game's send_encrypted's bridgeStatus check
         * (`x2_16 & 0xC`) sees 0 → no error path triggered → mcp_nodebus_last_error
         * stays 0 → caller proceeds normally. Game's caller (e.g. for fe_probe)
         * sees var_38 = 0 → high-bit clear → "no node" → default vtable entry.
         * For polling cmds, all-zero response means "no switch state changed",
         * which is benign for steady state.
         *
         * To extract the hint byte we need to decrypt the LAST encrypted byte
         * (offset 2+inner_len = position i=inner_len). That byte sits at
         * payload[2 + inner_len] in the bridge request payload. */
        if (nodebus_synth_fe_enabled() && enc_flag) {
            int hint_idx_in_payload = 2 + (int)inner_len;
            if (hint_idx_in_payload < payload_len) {
                /* Decrypt the hint byte. Position i = inner_len (one past the
                 * inner_len bytes the shim already decoded into scratch). */
                uint8_t mask = (uint8_t)(((slot & 0x1F) << 4) |
                                         ((inner_len + 1) & 0x0F));
                uint8_t k_mask_h = (inner_len & 1) ? (uint8_t)(~mask) : mask;
                uint8_t k_table_h = nodebus_cipher_keys[slot & 0x1F][inner_len & 0x0F];
                uint8_t hint = payload[hint_idx_in_payload] ^ k_table_h ^ k_mask_h;

                if (hint > 0 && hint <= 32 /* sane clamp */) {
                    uint8_t resp[64];
                    int o = 0;
                    resp[o++] = 0x02;             /* STX */
                    resp[o++] = 0x41;             /* response cmd echoes request */
                    resp[o++] = id;               /* echo channel id */
                    /* hint bytes of payload, all zero (last is bridgeStatus=0) */
                    for (int i = 0; i < hint; i++)
                        resp[o++] = 0x00;
                    uint8_t cks = bridge_checksum(resp + 1, (size_t)(o - 1));
                    resp[o++] = cks;
                    resp[o++] = 0x03;             /* ETX */
                    SHIM_LOG("bridge: synth cmd 0x41 RESP slot=%u inner_cmd=0x%02x hint=%u (%d-byte payload + bridgeStatus=0)",
                             slot, scratch[0], hint, hint);
                    bridge_send_response(resp, (size_t)o, 0x41, id);
                } else {
                    SHIM_LOG("bridge: synth cmd 0x41 SKIP slot=%u inner_cmd=0x%02x hint=%u (out of range or zero)",
                             slot, scratch[0], hint);
                }
            }
        }
        return;
    }
    default:
        SHIM_LOG("bridge: no synthetic response for cmd 0x%02x", cmd);
        return;
    }
}

