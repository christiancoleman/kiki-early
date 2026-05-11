





























#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <execinfo.h>
#include <dlfcn.h>

#include "spi_shim.h"

/* Defined in spi_shim.c — the shim-side end of the UART socketpair, and the
 * libc write() function pointer resolved once in resolve_all(). */
extern int uart_shim_fd;
typedef ssize_t (*real_write_t)(int, const void *, size_t);
extern real_write_t real_write;

/* Cursor for bridge cmd 0x25 (node-enumeration). Reset to 0 when the game
 * sends FUN_008f1100(0, 0) → cmd 0x41 inner cmd 0xF0 with slot=0 and
 * payload byte 0x22. Detected and reset in the cmd 0x41 dispatcher below. */
static int nodebus_enum_idx = 0;

/* Per-slot 16-byte cipher key blocks for the encrypted cmd-0x41 channel.
 * Mirrors the game binary's mcp_nodebus_cipher_keys @ 0x44fcfb8 (32 slots ×
 * 16 bytes). Zero at init = the boot state. The shim doesn't synthesize
 * responses to the key-fetch commands (inner cmds 0xfc / 0xfd / 0xf9), so the
 * game's per-slot keys also stay zero — meaning every cmd-0x41 frame the
 * game emits is encrypted under a zero key block. We reproduce that here so
 * decryption matches.
 *
 * Algorithm and full RE notes: docs/PROTOCOL.md §5.1.
 *
 * If we ever start synthesizing key-fetch responses, this table is where we
 * record the (fake) key bytes we hand out, so subsequent traffic decrypts
 * with those keys instead.
 */
static uint8_t nodebus_cipher_keys[32][16] = {{0}};
static uint8_t nodebus_switch_idle_baseline[32][8];
static uint8_t nodebus_switch_idle_baseline_valid[32];

/* Per-response delay before sending a synthetic cipher reply. Without it the
 * game races past Godot/Radium init (Step 43 "timeout is progress" — the path
 * that worked relied on the wall-clock pause from real-hardware timeouts, not
 * on the response shape). 200ms matches what's needed in practice. */
#define NODEBUS_SYNTH_DELAY_US 200000

/* Per-slot chip-ID assignment for fe_probe synthesis.
 *
 * mcp_lpc_chip_vtable @ 0x42317c8 is a 28-entry table of {uint32_t chip_id,
 * void* fns[...]} (40 bytes per entry). mcp_node_query_status_and_classify
 * @ 0x8f38e0 reads response[4..7] LE, and walks the vtable for a match;
 * mismatch produces "Invalid" in the Tech Alerts UI; match (but no
 * post-init traffic complete) produces "Not Initialized".
 *
 * Pokémon LE board mapping (from the LE manual + games/pokemon_le/*.hex
 * filename conventions):
 *
 *   slot 0  : CPU/bridge node    → 0x00140040 (entry 15, LPC1124JBD48/303)
 *   slot 4  : QR reader          → 0x00140040 (entry 15, LPC1124JBD48/303)
 *   slot 8  : Core Driver Lower  → 0x3D01402B (entry 21, LPC1342FHN33)
 *   slot 9  : Core Driver Upper  → 0x3D01402B (entry 21, LPC1342FHN33)
 *   slot 10 : SPI Node Board     → 0x3D01402B (entry 21, LPC1342FHN33)
 *   default : everything else    → 0x041E502B (entry 0,  LPC1111FHN33/101)
 *
 * The .hex firmware filenames (coil4node-LPC1313, tmc5041node-LPC1313, etc.)
 * suggest LPC1313 silicon for slots 8/9/10, but the in-game chip vtable's
 * 0x3D01402B is LPC1342 per PROTOCOL §5.4. The game accepts any vtable-
 * matching chip_id at this stage — empirical: all five Tech Alerts rows
 * flip from "Invalid" to "Not Initialized" with this mapping. */
static uint32_t nodebus_chip_id_for_slot(uint8_t slot) {
    switch (slot) {
        case 0:  return 0x00140040u;  /* LPC1124_303 (vtable[15], type_idx=4) — CPU bridge */
        case 1:  return 0x042D502Bu;  /* LPC1112_101 (vtable[2], type_idx=1) — Cabinet */
        case 4:  return 0x00140040u;  /* LPC1124_303 — QR reader */
        /* Pinnode slots — LPC1112_101 takes the simple chip_id path. */
        case 8:  return 0x042D502Bu;  /* Core Driver Lower */
        case 9:  return 0x042D502Bu;  /* Core Driver Upper */
        case 10: return 0x042D502Bu;  /* SPI Node Board */
        case 13: return 0x042D502Bu;  /* Topper-Servo (pinnode firmware) */
        /* ws2812node slots — only LPC1313 firmware shipped, so chip_id must
         * classify as type_idx=5. vtable[13] = 0x2c40102b → type_idx=5.
         * Earlier guess 0x3D01402B was vtable[21]/[22] with type_idx=0 →
         * FUN_008edd80 returned entry with +0x10=0 → uVar9=0 → lookup miss
         * → status=4. Verified via runtime vtable dump. */
        case 2:  return 0x2c40102Bu;  /* LPC1313 — Cabinet-Express */
        case 7:  return 0x2c40102Bu;  /* LPC1313 — Speaker-Expression */
        case 12: return 0x2c40102Bu;  /* LPC1313 — Topper-Lamps */
        case 14: return 0x2c40102Bu;  /* LPC1313 — Universal Topper */
        default: return 0x041E502Bu;  /* LPC1111_101 (vtable[0], type_idx=0) — default */
    }
}

/* Read the version bytes (V1, V2, V3, V0) the game expects for slot's loaded
 * firmware record. Mirrors FUN_008f88b0 so the FE-probe synth can return a
 * matching response → status=2 (Ok). Returns 0 on success.
 *
 * Walks the runtime firmware-record list at DAT_044df838 — modular across
 * games: PLE 0.81 (1_31_0), Star Wars 0.85, future titles all just work
 * because each game ships its own .hex set and the shim parrots whatever
 * version the loader ends up with. */
static int nodebus_version_for_slot(uint8_t slot,
                                    uint8_t *v1, uint8_t *v2,
                                    uint8_t *v3, uint8_t *v0) {
    uintptr_t game_base = shim_resolve_game_base();
    if (!game_base) return -1;

    uint8_t *slot_struct = (uint8_t *)(game_base + 0x44cc290ULL +
                                       (uint64_t)slot * 0xf0ULL);
    uint32_t key1 = *(uint32_t *)(slot_struct + 0x60);
    if (key1 == 0) return -1;

    uint32_t chip_id = nodebus_chip_id_for_slot(slot);
    uint8_t *vtable = (uint8_t *)(game_base + 0x42317c8ULL);
    uint32_t key2 = 0;
    int found = 0;
    for (int i = 0; i < 28; i++) {
        if (*(uint32_t *)(vtable + i * 0x28) == chip_id) {
            key2 = *(uint32_t *)(vtable + i * 0x28 + 0x10);
            found = 1;
            break;
        }
    }
    if (!found) return -1;

    uint8_t *cur = *(uint8_t **)(game_base + 0x44df838ULL);
    while (cur) {
        if (*(uint32_t *)(cur + 0) == key1 && *(uint32_t *)(cur + 4) == key2)
            break;
        cur = *(uint8_t **)(cur + 0x50);
    }
    if (!cur) return -1;

    uint64_t simple_gate = *(uint64_t *)(cur + 0x28);
    if (simple_gate != 0) {
        *v1 = cur[0x14]; *v2 = cur[0x16]; *v3 = cur[0x18]; *v0 = cur[0x1e];
    } else {
        uint64_t buf = *(uint64_t *)(cur + 0x20);
        if (!buf) return -1;
        uint32_t off = *(uint32_t *)(cur + 0x3c);
        uint8_t *p = (uint8_t *)(uintptr_t)buf;
        *v1 = p[off + 9];
        *v2 = p[off + 10];
        *v3 = p[off + 11];
        *v0 = p[off + 8];
    }
    return 0;
}

/* Bridge response synthesis (parsed by netbridge.elf send_bridge_message_with_response
 * + sub_d15aa8):
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
 * .binary_info sections.
 *
 * For 0x10 we return seven zero bytes. sub_d19580 interprets a zero
 * version as "Spike 1/2 board — use fallback, don't flash powerdist" —
 * which is exactly what we want in emulation (no real powerdist to flash).
 *
 * For 0x20 we return four zero bytes; the game stores the value into
 * data_490e644/data_490e648 without validating.
 */

#define NETBRIDGE_CRC32  0x357f5327u

static uint8_t bridge_checksum(const uint8_t *bytes, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) sum += bytes[i];
    return (uint8_t)(-(int32_t)sum);
}

static void pwrdist_1d_payload(uint8_t out[4]) {
    out[0] = 0xff;
    out[1] = 0xff;
    out[2] = 0xff;
    out[3] = 0xff;
}

/* Bridge frame escape encoding. The Spike 3 bridge protocol reserves three
 * byte values for framing:
 *   0x02 = STX (start of frame)
 *   0x03 = ETX (end of frame)
 *   0x1b = ESC (next data byte has bit 7 cleared)
 *
 * Any CMD/ID/PAYLOAD/CKSUM byte that equals one of these three values is
 * transmitted as `0x1b (byte | 0x80)`. The receiver's byte-by-byte parser
 * in netbridge.elf un-escapes by AND-ing with ~0x80.
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
        const char *zone = "extn";
        if (ret >= GAME_TEXT_LO && ret < GAME_TEXT_HI) {
            zone = "GAME";
        }
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
 * See docs/PROTOCOL.md §5.1 "Cipher algorithm" for the algorithm.
 */
static void nodebus_cipher_decrypt(uint8_t slot, uint8_t inner_len, uint8_t *buf) {
    uint8_t mask = (uint8_t)(((slot & 0x1F) << 4) | ((inner_len + 1) & 0x0F));
    for (int i = 0; i < inner_len; i++) {
        uint8_t k_table = nodebus_cipher_keys[slot & 0x1F][i & 0x0F];
        uint8_t k_mask = mask;
        if (i & 1) {
            k_mask = (uint8_t)(~mask);
        }
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
         * ACK fallback at 0x915d28. This carries pwrdist rail/interlock
         * status; default to all-bits-set until the exact bitfield is mapped. */
        uint8_t pd[4];
        pwrdist_1d_payload(pd);
        resp[0] = 0x02;
        resp[1] = 0x1d;
        resp[2] = id;
        resp[3] = pd[0];
        resp[4] = pd[1];
        resp[5] = pd[2];
        resp[6] = pd[3];
        resp[7] = bridge_checksum(resp + 1, 6);
        resp[8] = 0x03;
        bridge_send_response(resp, 9, cmd, id);
        SHIM_LOG("bridge: cmd 0x1d pwrdist payload id=0x%02x [%02x %02x %02x %02x]",
                 id, pd[0], pd[1], pd[2], pd[3]);
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
        uint8_t status = 0x02;
        if (cmd27_count & 1) {
            status = 0x06;
        }
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
    /* Step 43: cmd 0x25 / 0x41 silent-ACK reverted.
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
     * outbound payload here for visibility — see cases below + PROTOCOL.md §5.1.
     */
    case 0x25: {
        /* Bridge node-enumeration. mcp_nodebus_init_orchestrator
         * (FUN_00675110) calls FUN_008ede50 in a loop; each call sends bridge
         * cmd 0x25 (no payload) and expects a 1-byte response = next detected
         * slot ID. Bridge yields detected slots in sequence; 0 terminates.
         *
         * The orchestrator wraps each enumeration cycle in
         * FUN_008f1100(0, 0) reset calls (cmd 0x41 inner cmd 0xF0 with
         * slot=0, payload byte 0x22). We reset the cursor when we observe
         * that signature in the cmd 0x41 path below.
         *
         * Slot list = nodes our FE-probe synth advances to status=2 (slot 1
         * pinnode-LPC1112_101, slot 4 node4-LPC1124_303). After enumeration
         * completes, FUN_007da920(list, 32, 2) sets flag bit 1 for those
         * slots → row truly hides at Tech Alerts. */
        static const uint8_t enum_seq[] = { 1, 2, 4, 7, 8, 9, 10, 12, 13, 14, 0 };
        uint8_t next = (nodebus_enum_idx < (int)sizeof(enum_seq))
            ? enum_seq[nodebus_enum_idx++]
            : 0;
        resp[0] = 0x02;
        resp[1] = 0x25;
        resp[2] = id;
        resp[3] = next;
        resp[4] = bridge_checksum(resp + 1, 3);
        resp[5] = 0x03;
        SHIM_LOG("bridge: synth cmd 0x25 yield slot=%u (idx=%d)",
                 next, nodebus_enum_idx);
        bridge_send_response(resp, 6, cmd, id);
        return;
    }
    case 0x40: {
        /* Cleartext bridge sub-protocol. payload[0] is the sub-cmd byte.
         * Today known: 0x05 = snapshot+clear bridgeStatus,
         *              0x0c = factory test trap (NEVER respond — bridge enters
         *              an infinite loop intentionally),
         *              0x0d = NODEBUS_Hash (not currently sent by this game).
         * No synthetic response — game tolerates the timeout (Step 43 lesson).
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
         * boot state (Step 43 lesson stands).
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
        if (req_len < 6) {
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
        uint8_t inner_len = payload[1];
        if (2 + (int)inner_len + 1 > payload_len) {
            SHIM_LOG("bridge: cmd 0x41 slot=%u inner_len=%u doesn't fit in payload (%d)",
                     slot, inner_len, payload_len);
            return;
        }
        if (inner_len == 0) {
            SHIM_LOG("bridge: cmd 0x41 slot=%u zero-ping (no inner_cmd)", slot);
            return;
        }

        /* Read the cmd byte CLEARTEXT. Important: in the BSS-init shim state,
         * the per-slot status flag at mcp_nodebus_per_slot_status[slot]+3 is
         * always 0, so the game's `mcp_nodebus_send_encrypted` skips the
         * encryption loop entirely and the wire bytes ARE the original buffer
         * bytes. (Verified 2026-04-27 via GDB BP at send_encrypted entry —
         * captured cmd byte at offset 2 directly matches the wire byte for
         * slots 0 and 1.) An earlier shim version applied an unconditional
         * cipher decrypt and produced false "decoded" cmd values per slot
         * (0xED/0xDD/0x8D/etc) — those were artifacts of XOR'ing 0xFE with
         * (slot<<4)|((ilen+1)&F). Now we treat wire as plaintext. */
        uint8_t inner_cmd = payload[2];

        /* Detect FUN_008f1100(0, 0) reset signal: cmd 0x41 frame with
         * slot=0, inner_cmd=0xF0, payload[3]=0x22. Resets the cmd 0x25
         * enumeration cursor so the orchestrator's retry cycles each get
         * a fresh slot list (else FUN_007da920 would clear flag bit 1 on
         * any cycle that finds an empty enumeration). */
        if (slot == 0 && inner_cmd == 0xF0 && inner_len >= 2 &&
            payload_len >= 4 && payload[3] == 0x22) {
            if (nodebus_enum_idx != 0) {
                SHIM_LOG("bridge: cmd 0xF0 reset (enum_idx %d → 0)",
                         nodebus_enum_idx);
                nodebus_enum_idx = 0;
            }
        }
        char line[512];
        int n = snprintf(line, sizeof(line),
                         "[shim] bridge: cmd 0x41 slot=%u inner_cmd=0x%02x ilen=%u payload=[",
                         slot, inner_cmd, inner_len);
        for (int i = 1; i < inner_len && n < (int)sizeof(line) - 4; i++) {
            const char *byte_fmt = " %02x";
            if (i == 1) {
                byte_fmt = "%02x";
            }
            n += snprintf(line + n, sizeof(line) - n, byte_fmt, payload[2 + i]);
        }
        if (n < (int)sizeof(line) - 2) {
            line[n++] = ']';
            line[n++] = '\n';
        }
        if (n > (int)sizeof(line)) n = (int)sizeof(line);
        (void)write(STDERR_FILENO, line, (size_t)n);

        /* RAW wire dump for cipher channel — the entire cmd-0x41 bridge
         * frame (post-escape decode), so we can see exactly what bytes
         * the wrapper produced + what send_encrypted wrote on top
         * (csum, encryption). One-shot per (slot, inner_cmd). */
        {
            static uint8_t raw_seen[32 * 256 / 8] = {0};
            unsigned key = (unsigned)slot * 256u + (unsigned)inner_cmd;
            unsigned byte = key >> 3;
            unsigned bit  = key & 7;
            if (!(raw_seen[byte] & (1u << bit))) {
                raw_seen[byte] |= (uint8_t)(1u << bit);
                char raw[512];
                int rn = snprintf(raw, sizeof(raw),
                    "[shim] bridge: RAW slot=%u inner_cmd=0x%02x req_len=%d:",
                    slot, inner_cmd, req_len);
                for (int i = 0; i < req_len && rn < (int)sizeof(raw) - 4; i++)
                    rn += snprintf(raw + rn, sizeof(raw) - rn, " %02x", req[i]);
                if (rn < (int)sizeof(raw) - 1) raw[rn++] = '\n';
                if (rn > (int)sizeof(raw)) rn = (int)sizeof(raw);
                (void)write(STDERR_FILENO, raw, (size_t)rn);
            }
        }

        /* Caller-identification trace. Logs the first occurrence of each
         * (slot, inner_cmd) pair with a backtrace, so we can resolve the
         * specific wrapper function that emitted this command without
         * having to chase strb sequences in the disasm. ONE-SHOT per pair —
         * gated by a 256-bit table to avoid log flooding once we've ID'd
         * a particular send site. Frames are logged as game-binary VAs
         * (offset from libgame base) so they map directly to objdump /
         * BN function lookups. */
        {
            static uint8_t cipher_trace_seen[32 /*slots*/ * 256 /*inner_cmds*/ / 8] = {0};
            unsigned key = (unsigned)slot * 256u + (unsigned)inner_cmd;
            unsigned byte = key >> 3;
            unsigned bit  = key & 7;
            if (!(cipher_trace_seen[byte] & (1u << bit))) {
                cipher_trace_seen[byte] |= (uint8_t)(1u << bit);
                /* dlopen / backtrace are imported via execinfo / dl — both
                 * already linked because shim_sigsegv_handler uses them. */
                extern int  backtrace(void **, int);
                extern int  dladdr(const void *, Dl_info *);
                void *bt[16];
                int bt_n = backtrace(bt, 16);
                char tline[1024];
                int tn = snprintf(tline, sizeof(tline),
                    "[shim] bridge: TRACE slot=%u inner_cmd=0x%02x — caller backtrace:\n",
                    slot, inner_cmd);
                for (int i = 0; i < bt_n && tn < (int)sizeof(tline) - 80; i++) {
                    Dl_info di;
                    if (dladdr(bt[i], &di) && di.dli_fname) {
                        unsigned long off = (unsigned long)
                            ((uintptr_t)bt[i] - (uintptr_t)di.dli_fbase);
                        const char *base = strrchr(di.dli_fname, '/');
                        if (base) {
                            base++;
                        } else {
                            base = di.dli_fname;
                        }
                        tn += snprintf(tline + tn, sizeof(tline) - tn,
                            "[shim]   [%02d] %s+0x%lx\n", i, base, off);
                    } else {
                        tn += snprintf(tline + tn, sizeof(tline) - tn,
                            "[shim]   [%02d] %p (no dladdr)\n", i, bt[i]);
                    }
                }
                if (tn > (int)sizeof(tline)) tn = (int)sizeof(tline);
                (void)write(STDERR_FILENO, tline, (size_t)tn);
            }
        }

        /* fe_probe synthesis (inner_cmd=0xFE).
         *
         * Wire `hint` byte the shim parses is actually __n in the game's
         * FUN_008ec410 (= original game-side hint + 2; FUN_008ec410 bumps
         * ilen by 1 and appends __n at index ilen+3 before transmit). Game
         * expects exactly `hint` inner bytes back. Layout the validator wants:
         *   bytes [0..hint-3] = node data (game reads bytes[0..hint-3], with
         *                       FUN_008f38e0 using byte[0] node-present flag,
         *                       bytes[1..3] version (>0x011700 sets out[11]),
         *                       bytes[4..7] LE chip-id vs mcp_lpc_chip_vtable).
         *   byte  [hint-2]    = inner checksum: sum(bytes[0..hint-2]) == 0
         *                       mod 256 (FUN_008ec410 fails with code 3 else).
         *   byte  [hint-1]    = inner status; bits 2,3 must be clear (else
         *                       FUN_008ec410 fails with code 1/2). */
        if (inner_cmd == 0xFE) {
            int hint_idx = 2 + (int)inner_len;
            if (hint_idx >= payload_len) {
                SHIM_LOG("bridge: synth fe_probe SKIP slot=%u (hint_idx=%d ≥ payload_len=%d)",
                         slot, hint_idx, payload_len);
                return;
            }
            uint8_t hint = payload[hint_idx];
            if (hint == 0) {
                SHIM_LOG("bridge: synth fe_probe SKIP slot=%u hint=0 (fire-and-forget)",
                         slot);
                return;
            }
            if (hint > 32) {
                SHIM_LOG("bridge: synth fe_probe SKIP slot=%u hint=%u (>32, sanity clamp)",
                         slot, hint);
                return;
            }
            uint8_t resp[64];
            int o = 0;
            resp[o++] = 0x02;            /* STX */
            resp[o++] = 0x41;
            resp[o++] = id;
            int payload_start = o;
            int inner_size = (int)hint;
            for (int i = 0; i < inner_size; i++)
                resp[o++] = 0x00;
            if (hint >= 10) {
                uint32_t chip_id = nodebus_chip_id_for_slot(slot);
                uint8_t v1 = 0x01, v2 = 0x1F, v3 = 0x00, v0 = 0x00;
                (void)nodebus_version_for_slot(slot, &v1, &v2, &v3, &v0);
                /* byte[0] bit 7 = boot-mode flag (FUN_00674680 routes set→3,
                 * clear→chip_id classify path → status 2/4/6/7). */
                resp[payload_start + 0] = 0x00;
                resp[payload_start + 1] = v1;
                resp[payload_start + 2] = v2;
                resp[payload_start + 3] = v3;
                resp[payload_start + 4] = (uint8_t)(chip_id      );
                resp[payload_start + 5] = (uint8_t)(chip_id >>  8);
                resp[payload_start + 6] = (uint8_t)(chip_id >> 16);
                resp[payload_start + 7] = (uint8_t)(chip_id >> 24);
                /* Byte 10 is FE response byte 10 in FUN_008f38e0's output;
                 * FUN_00674680 requires V0==0 || V0==byte10 for status=2.
                 * Only safe to write when inner_size > 12 (else byte 10 is
                 * the inner-cksum slot). */
                if (inner_size > 12) {
                    resp[payload_start + 10] = v0;
                }
            }
            if (inner_size >= 2) {
                uint32_t inner_sum = 0;
                for (int i = 0; i < inner_size - 2; i++)
                    inner_sum += resp[payload_start + i];
                resp[payload_start + inner_size - 2] = (uint8_t)(-(int32_t)inner_sum);
            }
            uint8_t cks = bridge_checksum(resp + 1, (size_t)(o - 1));
            resp[o++] = cks;
            resp[o++] = 0x03;            /* ETX */
            SHIM_LOG("bridge: synth fe_probe RESP slot=%u hint=%u inner_sz=%d chip_id=0x%08x (node-present)",
                     slot, hint, inner_size, nodebus_chip_id_for_slot(slot));
            usleep(NODEBUS_SYNTH_DELAY_US);
            bridge_send_response(resp, (size_t)o, 0x41, id);
            return;
        }

        /* fa_probe (inner_cmd=0xFA): 16-bit follow-up after fe_probe. Hint
         * observed = 3 (too small to carry a chip ID). Semantics not RE'd —
         * keep all-zero (status OK, no data) until a regression localizes
         * the actual field meaning. */
        if (inner_cmd == 0xFA) {
            int hint_idx = 2 + (int)inner_len;
            if (hint_idx >= payload_len) {
                SHIM_LOG("bridge: synth fa_probe SKIP slot=%u (hint_idx=%d ≥ payload_len=%d)",
                         slot, hint_idx, payload_len);
                return;
            }
            uint8_t hint = payload[hint_idx];
            if (hint == 0 || hint > 32) {
                SHIM_LOG("bridge: synth fa_probe SKIP slot=%u hint=%u (out of range)",
                         slot, hint);
                return;
            }
            uint8_t resp[64];
            int o = 0;
            resp[o++] = 0x02;
            resp[o++] = 0x41;
            resp[o++] = id;
            for (int i = 0; i < hint; i++)
                resp[o++] = 0x00;
            uint8_t cks = bridge_checksum(resp + 1, (size_t)(o - 1));
            resp[o++] = cks;
            resp[o++] = 0x03;
            SHIM_LOG("bridge: synth fa_probe RESP slot=%u hint=%u (all-zero)",
                     slot, hint);
            usleep(NODEBUS_SYNTH_DELAY_US);
            bridge_send_response(resp, (size_t)o, 0x41, id);
            return;
        }

        /* Cipher-key fetch synthesis. Key-fetch is the natural follow-up after
         * fe_probe declares a node present. Step 69 documented the stall:
         * with fe_probe synth on but no key-fetch synth, Godot fails to init
         * because the game sits in mcp_nodebus_fetch_key_* waiting for 0xfc/
         * 0xfd/0xf9 replies.
         *
         * Fetcher chains (PROTOCOL §5.1 "Cipher-key fetchers"):
         *   - mcp_nodebus_fetch_key_basic       : 0xfd hint=12 → 0xfc hint=16
         *   - mcp_nodebus_fetch_key_with_serial : same as basic + sprintf
         *   - mcp_nodebus_fetch_key_two_probes  : 0xf9 hint=16 → 0xf9 hint=16
         *                                                     → 0xfc hint=16
         * The 0xfc 16-byte response is memcpy'd directly into
         * mcp_nodebus_cipher_keys[slot*16]. By returning all zeros we keep
         * the BSS-state semantics: per-slot keys remain zero, and the
         * existing decryptor (which assumes zero keys) still produces the
         * correct cleartext for any subsequent encrypted traffic.
         *
         * Hint values hardcoded above are confirmed via static RE of each
         * wrapper's strh of arg1[2..3] / arg1[4]; they don't vary per-call. */
        /* switch_poll synthesis (inner_cmd=0x11). FUN_008edec0 issues this to
         * each slot N>0 to read 8 bytes of switch state into channel N's
         * input buffer for translate_switch_state_changes. With NO synth, the
         * stack-allocated local_50 buffer in FUN_00674b90 holds whatever
         * garbage was on the stack — translate compares it against the per-
         * channel saved state and fires "edge" events on every mismatched
         * bit. That's the source of the audio spam we hear at runtime.
         *
         * Fix: return the channel's own POST-INIT idle state. FUN_00650f00
         * wrote per-bit polarity-aware idle bytes into the channel struct's
         * "saved state" buffer. Reading those 8 bytes and sending
         * them as the synth payload means the FIRST translate cycle on the
         * channel sees current=idle / previous=initial-zero (one cycle of
         * edges, then steady) and SUBSEQUENT cycles see current=idle /
         * previous=idle (no edges, no spam).
         *
         * Per-slot status flag in mcp_nodebus_per_slot_status[slot]+3 is zero
         * at boot (BSS init), so mcp_nodebus_send_encrypted takes the
         * cleartext path: uVar4=0, rotated_mask=0, payload XOR is identity.
         * Idle bytes go straight to the caller's switch buffer. */
        if (inner_cmd == 0x11) {
            int hint_idx = 2 + (int)inner_len;
            if (hint_idx >= payload_len) {
                SHIM_LOG("bridge: synth switch_poll SKIP slot=%u (hint_idx=%d ≥ payload_len=%d)",
                         slot, hint_idx, payload_len);
                return;
            }
            uint8_t hint = payload[hint_idx];
            if (hint == 0 || hint > 32) {
                SHIM_LOG("bridge: synth switch_poll SKIP slot=%u hint=%u (out of range)",
                         slot, hint);
                return;
            }
            uint8_t idle[SHIM_SWITCH_CHANNEL_BYTES] = {0};
            uintptr_t game_base = shim_resolve_game_base();
            if (game_base) {
                if (slot < 32 && nodebus_switch_idle_baseline_valid[slot]) {
                    memcpy(idle, nodebus_switch_idle_baseline[slot],
                           SHIM_SWITCH_CHANNEL_BYTES);
                } else {
                    shim_switch_copy_channel_idle(game_base, slot, idle);
                    if (slot < 32) {
                        int nonzero = 0;
                        for (int i = 0; i < SHIM_SWITCH_CHANNEL_BYTES; i++) {
                            nonzero |= idle[i];
                        }
                        if (nonzero) {
                            memcpy(nodebus_switch_idle_baseline[slot], idle,
                                   SHIM_SWITCH_CHANNEL_BYTES);
                            nodebus_switch_idle_baseline_valid[slot] = 1;
                        }
                    }
                }
            }
            shim_switch_hold_apply(slot, idle);
            uint8_t resp[64];
            int o = 0;
            resp[o++] = 0x02;
            resp[o++] = 0x41;
            resp[o++] = id;
            int payload_start = o;
            int inner_size = (int)hint;
            for (int i = 0; i < inner_size; i++)
                if (i < SHIM_SWITCH_CHANNEL_BYTES) {
                    resp[o++] = idle[i];
                } else {
                    resp[o++] = 0x00;
                }
            if (inner_size >= 2) {
                uint32_t inner_sum = 0;
                for (int i = 0; i < inner_size - 2; i++)
                    inner_sum += resp[payload_start + i];
                resp[payload_start + inner_size - 2] = (uint8_t)(-(int32_t)inner_sum);
            }
            uint8_t cks = bridge_checksum(resp + 1, (size_t)(o - 1));
            resp[o++] = cks;
            resp[o++] = 0x03;
            SHIM_LOG("bridge: synth switch_poll RESP slot=%u hint=%u idle=%02x %02x %02x %02x %02x %02x %02x %02x",
                     slot, hint, idle[0], idle[1], idle[2], idle[3],
                     idle[4], idle[5], idle[6], idle[7]);
            usleep(NODEBUS_SYNTH_DELAY_US);
            bridge_send_response(resp, (size_t)o, 0x41, id);
            return;
        }

        if (inner_cmd == 0xFC || inner_cmd == 0xFD || inner_cmd == 0xF9) {
            int hint_idx = 2 + (int)inner_len;
            if (hint_idx >= payload_len) {
                SHIM_LOG("bridge: synth key-fetch SKIP slot=%u inner_cmd=0x%02x (hint_idx=%d ≥ payload_len=%d)",
                         slot, inner_cmd, hint_idx, payload_len);
                return;
            }
            uint8_t hint = payload[hint_idx];
            if (hint == 0 || hint > 32) {
                SHIM_LOG("bridge: synth key-fetch SKIP slot=%u inner_cmd=0x%02x hint=%u (out of range)",
                         slot, inner_cmd, hint);
                return;
            }
            uint8_t resp[64];
            int o = 0;
            resp[o++] = 0x02;            /* STX */
            resp[o++] = 0x41;
            resp[o++] = id;
            for (int i = 0; i < hint; i++)
                resp[o++] = 0x00;        /* zero key bytes + bridgeStatus=0 */
            uint8_t cks = bridge_checksum(resp + 1, (size_t)(o - 1));
            resp[o++] = cks;
            resp[o++] = 0x03;            /* ETX */
            SHIM_LOG("bridge: synth key-fetch RESP slot=%u inner_cmd=0x%02x hint=%u (zero key, status OK)",
                     slot, inner_cmd, hint);
            usleep(NODEBUS_SYNTH_DELAY_US);
            bridge_send_response(resp, (size_t)o, 0x41, id);
            return;
        }
        return;
    }
    default:
        SHIM_LOG("bridge: no synthetic response for cmd 0x%02x", cmd);
        return;
    }
}
