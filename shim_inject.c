

























#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <time.h>
#include <signal.h>
#include <setjmp.h>
#include <pthread.h>

#include "spi_shim.h"
#include "shim_discovery.h"

/* Sweep globals — read by shim_sigsegv_handler (in shim_diagnostics.c) to
 * longjmp back out of a faulting sweep iteration. Externed in spi_shim.h. */
sigjmp_buf shim_event_sweep_jmp;
volatile sig_atomic_t shim_event_fire_in_progress = 0;
volatile int shim_current_sweep_event_id = -1;

static uint64_t shim_fire_event(int32_t event_id, int64_t event_data) {
    uintptr_t game_base = shim_resolve_game_base();
    if (!game_base) return (uint64_t)-1;
    typedef uint64_t (*fire_event_t)(int32_t, int64_t);
    fire_event_t fn = (fire_event_t)(game_base + 0x5c12a0);
    return fn(event_id, event_data);
}

/* Minimal cmode state snapshot. For each of the 42 cmodes captures
 * 2 u64 words: +0x308 (the field most likely to change on activation,
 * though it's subclass-reinterpreted) and +0x60 (zero at static init,
 * non-zero post-activation in some cmodes). 42×16 = 672 bytes. */
static void shim_capture_cmode_snap(uintptr_t game_base, uint64_t *out) {
    size_t N = shim_cmode_table_count;
    for (size_t i = 0; i < N; i++) {
        uintptr_t rt = game_base + shim_cmode_table[i].va;
        memcpy(&out[i*2 + 0], (void*)(rt + 0x308), 8);
        memcpy(&out[i*2 + 1], (void*)(rt + 0x60),  8);
    }
}

void shim_event_sweep(void) {
    uintptr_t game_base = shim_resolve_game_base();
    if (!game_base) {
        const char *m = "\n--- event sweep: game_base not resolved ---\n";
        ssize_t u = write(STDERR_FILENO, m, strlen(m)); (void)u;
        return;
    }

    const char *hdr =
        "\n--- event sweep: firing events 0x00..0xce with event_data=0 ---\n"
        "format:  event <id>  ret=<ret>  Δ=<#changed>[  <cmode>(+0x308: <before>-><after>)]...\n";
    ssize_t u = write(STDERR_FILENO, hdr, strlen(hdr)); (void)u;

    size_t N = shim_cmode_table_count;
    uint64_t baseline[42 * 2];
    uint64_t post[42 * 2];
    shim_capture_cmode_snap(game_base, baseline);

    int crashed_events = 0;
    int reactive_events = 0;
    int fired_ok = 0;

    int timed_out_events = 0;
    for (int ev = 0; ev <= 0xce; ev++) {
        shim_current_sweep_event_id = ev;
        shim_event_fire_in_progress = 1;

        uint64_t ret = (uint64_t)-1;
        int did_fire = 0;
        int setjmp_result;

        /* alarm(1) delivers SIGALRM to this thread (blocked elsewhere by the
         * sigmask set on the timer thread). SIGALRM handler recognises
         * in_progress and siglongjmps back with value 2. */
        alarm(1);
        setjmp_result = sigsetjmp(shim_event_sweep_jmp, 1);
        if (setjmp_result == 0) {
            ret = shim_fire_event(ev, 0);
            alarm(0);
            did_fire = 1;
            fired_ok++;
        } else if (setjmp_result == 2) {
            alarm(0);
            timed_out_events++;
        } else {
            alarm(0);
            crashed_events++;
        }
        shim_event_fire_in_progress = 0;

        if (did_fire) {
            shim_capture_cmode_snap(game_base, post);

            char delta_buf[1800];
            size_t delta_off = 0;
            int delta_count = 0;
            for (size_t i = 0; i < N; i++) {
                if (post[i*2 + 0] != baseline[i*2 + 0] ||
                    post[i*2 + 1] != baseline[i*2 + 1]) {
                    delta_count++;
                    if (delta_off < sizeof(delta_buf) - 128) {
                        int added = snprintf(
                            delta_buf + delta_off,
                            sizeof(delta_buf) - delta_off,
                            "  %s(+0x308:%lx->%lx)",
                            shim_cmode_table[i].name_hint,
                            (unsigned long)baseline[i*2 + 0],
                            (unsigned long)post[i*2 + 0]);
                        delta_off += (size_t)added;
                    }
                }
            }

            char line[2048];
            int ln;
            if (delta_count > 0) {
                reactive_events++;
                ln = snprintf(line, sizeof(line),
                    "  event 0x%02x ret=%lu Δ=%d %s\n",
                    ev, (unsigned long)ret, delta_count, delta_buf);
                memcpy(baseline, post, sizeof(baseline));
            } else {
                ln = snprintf(line, sizeof(line),
                    "  event 0x%02x ret=%lu Δ=0\n",
                    ev, (unsigned long)ret);
            }
            u = write(STDERR_FILENO, line, (size_t)ln); (void)u;
        }
    }

    char sum[220];
    int sl = snprintf(sum, sizeof(sum),
        "\n--- sweep complete: fired=%d reactive=%d crashed=%d timeouts=%d (of 207) ---\n",
        fired_ok, reactive_events, crashed_events, timed_out_events);
    u = write(STDERR_FILENO, sum, (size_t)sl); (void)u;
}



/* Wait until channel-1's saved-state buffer at game_base + 0x44804d0 + 0xb0
 * is non-zero (i.e., FUN_00650f00 has populated it), then write that buffer
 * to DAT_0436b020 so FUN_00674b90's channel-0 path reads back idle bytes
 * instead of zero. Polls every 100 ms with a 30 s timeout. */
static void shim_install_channel1_idle(uintptr_t game_base) {
    uint8_t *channel_1_saved = (uint8_t *)(uintptr_t)
        (game_base + 0x44804d0ULL + 0xb0ULL);
    volatile uint64_t *dat_0436b020 = (volatile uint64_t *)(uintptr_t)
        (game_base + 0x436b020ULL);
    for (int i = 0; i < 300; i++) {
        uint64_t idle_u64;
        memcpy(&idle_u64, channel_1_saved, 8);
        if (idle_u64 != 0) {
            *dat_0436b020 = idle_u64;
            SHIM_LOG("[channel1_idle] wrote 0x%016lx to DAT_0436b020 after %dms (FUN_00674b90 spam fix)",
                     (unsigned long)idle_u64, i * 100);
            return;
        }
        usleep(100000);
    }
    SHIM_LOG("[channel1_idle] gave up after 30s — channel 1 saved state stayed zero");
}

static void *shim_discovery_init_thread(void *arg) {
    (void)arg;
    sleep(2);
    shim_discovery_init();

    /* Decoder compatibility patch: NOP the b.ne at game VA 0x920cf0 so the
     * game's get_format callback accepts NV12 from forward-decode (or from
     * local SW hevc as a diagnostic). Must run after game-base is resolvable
     * — the 2 s sleep above already handles that timing. Default-on; gate
     * via SPIKE3_DECODER_PATCH=0. */
    shim_patch_get_format_callback();

    uintptr_t game_base = shim_resolve_game_base();
    if (game_base) shim_install_channel1_idle(game_base);
    return NULL;
}

static void shim_event_sweep_alarm_handler(int sig, siginfo_t *info, void *uc) {
    (void)info; (void)uc;
    if (shim_event_fire_in_progress) {
        char msg[160];
        int n = snprintf(msg, sizeof(msg),
            "\n  [sweep] event 0x%02x TIMEOUT (sig=%d) — recovering\n",
            shim_current_sweep_event_id, sig);
        ssize_t u = write(STDERR_FILENO, msg, (size_t)n); (void)u;
        shim_event_fire_in_progress = 0;
        siglongjmp(shim_event_sweep_jmp, 2);
    }
}



/* Switch-state probing & press injection — opt-in scaffolding for the
 * unsolved Tech-Alerts → Node Bus Test transition.
 *
 * What we know empirically (this session):
 *   - g_switch_state_array @ BN VA 0x4480220 resolves to a real heap array.
 *     Layout: 0x30-byte stride, +0x20 = u16 switch_id (== array index),
 *     +0x28 = state byte. But direct writes here don't drive UI.
 *
 * What we don't know:
 *   - Which channel/switch the Tech Alerts screen actually listens to for
 *     "Press Select". Sidekick's claim that channel 5, switch_id 8 within-
 *     channel maps to Service Select did NOT advance the UI — neither via
 *     direct array writes nor via translate_switch_state_changes(5, ...)
 *     brute-forced across all 64 bit positions.
 *
 * Env-tunables (all opt-in; default is no-op):
 *   SHIM_SWITCH_PROBE=1            — log array dump + run press path
 *   SHIM_SWITCH_PRESS_DELAY_S=N    — wait N seconds before probing
 *                                    (default 30; Tech Alerts needs ~200)
 *   SHIM_CH5_BYTE / SHIM_CH5_BIT   — single targeted press on channel 5
 *   SHIM_CH5_SWEEP=1               — sweep all 64 channel-5 bit positions
 *   SHIM_SWITCH_ALL_IDLE=1         — force every entry's +0x28 to 1
 */
static void *shim_select_press_thread(void *arg) {
    (void)arg;
    const char *v = getenv("SHIM_SWITCH_PROBE");
    if (!v || !v[0] || v[0] == '0') return NULL;

    int sw_id = -1;
    int delay_s = 30;
    if ((v = getenv("SHIM_SWITCH_PRESS_ID")) != NULL && v[0]) sw_id = atoi(v);
    if ((v = getenv("SHIM_SWITCH_PRESS_DELAY_S")) != NULL && v[0]) delay_s = atoi(v);

    sleep((unsigned)delay_s);

    uintptr_t game_base = shim_resolve_game_base();
    if (!game_base) {
        SHIM_LOG("[switch_inject] game_base never resolved; skipping");
        return NULL;
    }

    uintptr_t arr_ptr_loc = game_base + 0x4480220ULL;
    SHIM_LOG("[switch_inject] game_base=0x%lx  arr_ptr_loc=0x%lx",
             (unsigned long)game_base, (unsigned long)arr_ptr_loc);

    uint8_t **arr_pp = (uint8_t **)arr_ptr_loc;
    uint8_t *arr = *arr_pp;
    SHIM_LOG("[switch_inject] *arr_ptr_loc = %p (heap pointer if non-null)", (void *)arr);
    if (!arr) {
        SHIM_LOG("[switch_inject] array pointer is NULL — wrong VA, or array not yet allocated");
        return NULL;
    }

    /* Layout (verified):
     *   +0x10 : descriptor pointer (deref-unsafe for some entries)
     *   +0x20 : uint16 switch_id (== array index)
     *   +0x28 : 1-byte state. Pinball pull-up: 1=idle, 0=pressed.
     * Dump probe IDs to log for visibility. */
    char line[512];
    int probe_ids[] = { 0, 1, 2, 8, 69 };
    for (size_t pi = 0; pi < sizeof(probe_ids)/sizeof(probe_ids[0]); pi++) {
        int i = probe_ids[pi];
        uint8_t *e = arr + ((size_t)i * 0x30u);
        int n = snprintf(line, sizeof(line), "[switch_inject] entry %3d @ %p:", i, (void *)e);
        for (int j = 0; j < 0x30 && n < (int)sizeof(line) - 4; j++)
            n += snprintf(line + n, sizeof(line) - n, " %02x", e[j]);
        SHIM_LOG("%s", line);
    }

    /* SHIM_SWITCH_ALL_IDLE=1 sets state=1 (idle/pulled-up) on every entry
     * 1..255 to silence the "everything reads as pressed" stuck-switch spam.
     * Skips entry 0 (looks uninitialized in dumps). */
    const char *all_idle = getenv("SHIM_SWITCH_ALL_IDLE");
    if (all_idle && all_idle[0] && all_idle[0] != '0') {
        int touched = 0;
        for (int i = 1; i < 256; i++) {
            uint8_t *e = arr + ((size_t)i * 0x30u);
            /* sanity: only touch entries whose +0x20 u16 actually equals i.
             * Stops us writing if we walk off the end of the real array. */
            uint16_t recorded_id = *(uint16_t *)(e + 0x20);
            if (recorded_id != (uint16_t)i) break;
            if (e[0x28] == 0) {
                e[0x28] = 1;
                touched++;
            }
        }
        SHIM_LOG("[switch_inject] SHIM_SWITCH_ALL_IDLE: forced %d entries to state=1", touched);
    }

    /* Legacy SHIM_SWITCH_PRESS_ID is now superseded by the channel-5 path
     * below (Sidekick analysis: g_switch_state_array is a derived cache;
     * the source of truth for edge detection is g_channel_state_buffers,
     * fed by translate_switch_state_changes per channel). */

    /* Channel 5 = Service Select / operator-button channel (UART netbridge
     * cmd 0x27 in real hardware, NOT polled in our shim). To inject a press
     * we call translate_switch_state_changes(5, buffer) directly with the
     * bit set for our target switch.
     *
     *   SHIM_CH5_BYTE / SHIM_CH5_BIT — single targeted press; default
     *     byte=1 bit=0 per Sidekick's switch_id-8 educated guess.
     *   SHIM_CH5_SWEEP=1 — brute-force all 64 bit positions (2s each),
     *     watch viewer/log for the bit that advances Tech Alerts.
     */
    int ch5_byte = 1;
    int ch5_bit  = 0;
    int sweep    = 0;
    if ((v = getenv("SHIM_CH5_BYTE")) != NULL && v[0]) ch5_byte = atoi(v);
    if ((v = getenv("SHIM_CH5_BIT"))  != NULL && v[0]) ch5_bit  = atoi(v);
    if ((v = getenv("SHIM_CH5_SWEEP")) != NULL && v[0] && v[0] != '0') sweep = 1;

    typedef int64_t (*translate_fn)(uint8_t channel_id, const uint8_t *buffer);
    translate_fn translate = (translate_fn)(game_base + 0x651610);

    if (sweep) {
        SHIM_LOG("[switch_inject] CH5 SWEEP starting (64 positions × 2s each)");
        for (int b = 0; b < 8; b++) {
            for (int k = 0; k < 8; k++) {
                uint8_t buffer[8] = {0};
                buffer[b] = (uint8_t)(1u << k);
                SHIM_LOG("[switch_inject] CH5 sweep: byte=%d bit=%d PRESS", b, k);
                translate(5, buffer);
                usleep(800000);
                buffer[b] = 0;
                translate(5, buffer);
                usleep(1200000);
            }
        }
        SHIM_LOG("[switch_inject] CH5 sweep done");
        return NULL;
    }

    if (sw_id >= 0) {
        SHIM_LOG("[switch_inject] (legacy SHIM_SWITCH_PRESS_ID=%d ignored — channel 5 path used instead)",
                 sw_id);
    }

    uint8_t buffer[8] = {0};
    if (ch5_byte >= 0 && ch5_byte < 8 && ch5_bit >= 0 && ch5_bit < 8) {
        buffer[ch5_byte] = (uint8_t)(1u << ch5_bit);
    }
    SHIM_LOG("[switch_inject] CH5 press byte=%d bit=%d (buffer[%d]=0x%02x)",
             ch5_byte, ch5_bit, ch5_byte, buffer[ch5_byte]);
    translate(5, buffer);
    usleep(180000);
    buffer[ch5_byte] = 0;
    translate(5, buffer);
    SHIM_LOG("[switch_inject] CH5 release done");
    return NULL;
}

/* SHIM_NODE_PROBE=1 — one-shot dump of nodebus runtime state to log:
 *   - .got entries 0x4350200 / 0x434f2f0 / 0x4351748 (firmware-image config
 *     table base + count pointer + board-type metadata table)
 *   - first ~8 entries of firmware-image config table (slot byte at +0x1a,
 *     firmware index at +0x18, flags at +0x00)
 *   - linked list of loaded firmware records at DAT_044df838 (key1, key2,
 *     filename) — each record is the 88-byte calloc'd struct from
 *     mcp_node_firmware_glob @ 0x840a40
 *   - per-slot Tech Alerts struct (slots 0..15) at 0x44cc290+slot*0xf0:
 *     slot id (+0x00), flag word (+0x04), status u32 (+0x20),
 *     firmware-kind key (+0x60), config ptr (+0x10), board-type ptr (+0x18)
 *
 * Purpose: ground-truth the static analysis for Phase A of project_node1_init —
 * tells us what slot 1's firmware-kind key needs to be in a synthetic record.
 *
 * SHIM_NODE_PROBE_DELAY_S=N — wait N seconds before dumping (default 60;
 * needs ~50+ to ensure mcp_node_record_table_init @ 0x7dae30 has run). */
static void *shim_node_records_probe_thread(void *arg) {
    (void)arg;
    const char *v = getenv("SHIM_NODE_PROBE");
    if (!v || !v[0] || v[0] == '0') return NULL;

    int delay_s = 60;
    if ((v = getenv("SHIM_NODE_PROBE_DELAY_S")) != NULL && v[0]) delay_s = atoi(v);
    sleep((unsigned)delay_s);

    uintptr_t game_base = shim_resolve_game_base();
    if (!game_base) {
        SHIM_LOG("[node_probe] game_base unresolved; skipping");
        return NULL;
    }
    SHIM_LOG("[node_probe] === START === game_base=0x%lx", (unsigned long)game_base);

    uint64_t fw_cfg_base  = *(uint64_t *)(game_base + 0x4350200ULL);
    uint64_t fw_cnt_addr  = *(uint64_t *)(game_base + 0x434f2f0ULL);
    uint64_t bt_meta_base = *(uint64_t *)(game_base + 0x4351748ULL);
    uint64_t fw_count = fw_cnt_addr ? *(uint64_t *)fw_cnt_addr : 0;
    SHIM_LOG("[node_probe] PTR_PTR_04350200 -> 0x%lx (static-derived would be 0x%lx)",
             (unsigned long)fw_cfg_base, (unsigned long)(game_base + 0x436b680ULL));
    SHIM_LOG("[node_probe] PTR_DAT_0434f2f0 -> 0x%lx  count=%lu",
             (unsigned long)fw_cnt_addr, (unsigned long)fw_count);
    SHIM_LOG("[node_probe] PTR_DAT_04351748 -> 0x%lx", (unsigned long)bt_meta_base);

    int show = (int)(fw_count + 2);
    if (show < 4) show = 4;
    if (show > 16) show = 16;
    for (int i = 0; fw_cfg_base && i < show; i++) {
        uint8_t *e = (uint8_t *)(uintptr_t)(fw_cfg_base + (uint64_t)i * 0x20);
        char line[300];
        int n = snprintf(line, sizeof(line),
            "[node_probe] fw_cfg[%2d] @ 0x%lx slot=%u fw_idx=%u flags=0x%08x bytes:",
            i, (unsigned long)(uintptr_t)e,
            e[0x1a], *(uint16_t *)(e + 0x18), *(uint32_t *)(e + 0x00));
        for (int j = 0; j < 0x20 && n < (int)sizeof(line) - 4; j++)
            n += snprintf(line + n, sizeof(line) - n, " %02x", e[j]);
        SHIM_LOG("%s", line);
    }

    uint8_t **head_loc = (uint8_t **)(game_base + 0x44df838ULL);
    uint8_t *cur = *head_loc;
    SHIM_LOG("[node_probe] DAT_044df838 = %p", (void *)cur);
    int n_records = 0;
    while (cur && n_records < 32) {
        uint32_t key1 = *(uint32_t *)(cur + 0x00);
        uint32_t key2 = *(uint32_t *)(cur + 0x04);
        char *fname = *(char **)(cur + 0x08);
        SHIM_LOG("[node_probe] fw_record[%d] @ %p key1=0x%08x key2=%u file=\"%s\"",
                 n_records, (void *)cur, key1, key2, fname ? fname : "(null)");
        char line[400];
        int n = snprintf(line, sizeof(line), "[node_probe]   bytes [+0x10..+0x4f]:");
        for (int j = 0x10; j < 0x50 && n < (int)sizeof(line) - 4; j++)
            n += snprintf(line + n, sizeof(line) - n, " %02x", cur[j]);
        SHIM_LOG("%s", line);
        uint64_t simple_gate = *(uint64_t *)(cur + 0x28);
        uint8_t v[4];
        const char *path;
        if (simple_gate != 0) {
            v[0] = cur[0x14]; v[1] = cur[0x16]; v[2] = cur[0x18]; v[3] = cur[0x1e];
            path = "simple";
        } else {
            uint64_t buf = *(uint64_t *)(cur + 0x20);
            uint32_t off = *(uint32_t *)(cur + 0x3c);
            uint8_t *p = (uint8_t *)(uintptr_t)buf;
            v[0] = p[off + 9];
            v[1] = p[off + 10];
            v[2] = p[off + 11];
            v[3] = p[off + 8];
            path = "complex";
        }
        SHIM_LOG("[node_probe]   version (V1,V2,V3,V0) = (0x%02x, 0x%02x, 0x%02x, 0x%02x) [%s path]",
                 v[0], v[1], v[2], v[3], path);
        cur = *(uint8_t **)(cur + 0x50);
        n_records++;
    }
    SHIM_LOG("[node_probe] %d firmware records loaded", n_records);

    /* LPC chip vtable @ 0x42317c8 — 28 entries × 0x28. Each entry has the
     * 32-bit chip_id at +0x00 and the chip-type-index (key2) at +0x10.
     * Dumping these lets us pick chip_ids for nodebus_chip_id_for_slot()
     * without guessing — especially for chip-type-index 5 (LPC1313) used
     * by ws2812node, where guessing the wrong chip_id makes FUN_008edd80
     * return the default entry → uVar9=0 → lookup miss → status=4. */
    {
        uint8_t *vt = (uint8_t *)(game_base + 0x42317c8ULL);
        for (int i = 0; i < 28; i++) {
            uint32_t chip_id  = *(uint32_t *)(vt + i * 0x28 + 0x00);
            uint32_t type_idx = *(uint32_t *)(vt + i * 0x28 + 0x10);
            if (chip_id != 0 || type_idx != 0) {
                SHIM_LOG("[node_probe] vtable[%2d] chip_id=0x%08x type_idx=%u",
                         i, chip_id, type_idx);
            }
        }
    }

    for (int slot = 0; slot < 16; slot++) {
        uint8_t *s = (uint8_t *)(game_base + 0x44cc290ULL + (uint64_t)slot * 0xf0ULL);
        SHIM_LOG("[node_probe] slot[%2d] @ %p id=%u flag=0x%08x status=%u fw_key=0x%08x fw_cfg=%p bt=%p",
                 slot, (void *)s, s[0x00], *(uint32_t *)(s + 0x04),
                 *(uint32_t *)(s + 0x20), *(uint32_t *)(s + 0x60),
                 *(void **)(s + 0x10), *(void **)(s + 0x18));
    }
    SHIM_LOG("[node_probe] === END ===");
    return NULL;
}

/* SHIM_SWITCH_TABLE_PROBE=1 — read g_switch_state_array @ 0x4480220 (heap;
 * pointer-of-pointer-to-array calloc'd by FUN_00650f00). Per FUN_00650f00,
 * each 0x30-byte entry has the resolved (channel, bit_idx) already filled
 * in:
 *
 *   +0x20 : u16 switch_id     (== array index)
 *   +0x22 : u16 bit_idx       (full bit position 0..63 in channel buffer)
 *   +0x24 : byte channel_id   (0 = CPU SPI; >0 = remote node slot)
 *
 * (Step 90c labeled +0x22 "prev_id" and +0x24 "switch-type code" — both
 * mislabeled. FUN_00650f00 line `*(short *)(lVar13 + 0x22) = uVar4` writes
 * bit_idx, and `*(byte *)(lVar13 + 0x24) = bVar3` writes channel_id.)
 *
 * If SHIM_SELECT_PRESS=1, pulses Service Select (sw_id=8 by default;
 * SHIM_SELECT_PRESS_SW_ID overrides) via
 * translate_switch_state_changes(channel, buffer) with the resolved bit set
 * then cleared, every SHIM_SELECT_PRESS_INTERVAL_S seconds (default 5), for
 * SHIM_SELECT_PRESS_ATTEMPTS attempts (default 30). */
static void *shim_switch_table_probe_thread(void *arg) {
    (void)arg;
    const char *v = getenv("SHIM_SWITCH_TABLE_PROBE");
    if (!v || !v[0] || v[0] == '0') return NULL;

    int delay_s = 80;
    if ((v = getenv("SHIM_SWITCH_TABLE_PROBE_DELAY_S")) != NULL && v[0]) delay_s = atoi(v);
    sleep((unsigned)delay_s);

    uintptr_t game_base = shim_resolve_game_base();
    if (!game_base) {
        SHIM_LOG("[switch_table] game_base unresolved; skipping");
        return NULL;
    }
    SHIM_LOG("[switch_table] === START === game_base=0x%lx", (unsigned long)game_base);

    uint8_t **arr_pp = (uint8_t **)(uintptr_t)(game_base + 0x4480220ULL);
    uint8_t *arr = *arr_pp;
    SHIM_LOG("[switch_table] g_switch_state_array @ %p", (void *)arr);
    if (!arr) {
        SHIM_LOG("[switch_table] array pointer is NULL; abort");
        return NULL;
    }

    int max_sw = 256;
    int valid = 0;
    for (int sw_id = 1; sw_id < max_sw; sw_id++) {
        uint8_t *e = arr + (size_t)sw_id * 0x30u;
        uint16_t recorded_sw_id = *(uint16_t *)(e + 0x20);
        if (recorded_sw_id != (uint16_t)sw_id) {
            SHIM_LOG("[switch_table] sw_id=%d  recorded_sw_id=%u (mismatch — end of valid entries)",
                     sw_id, recorded_sw_id);
            break;
        }
        uint16_t bit_idx    = *(uint16_t *)(e + 0x22);
        uint8_t  channel_id = *(uint8_t  *)(e + 0x24);
        uint8_t  state_byte = *(uint8_t  *)(e + 0x28);

        /* Per sub_6511c0 (drain): on transition, calls sub_729c10 with
         *   arg1 = u16 at config_ptr[+0x34]  (event_id)
         *   arg2 = u64 at config_ptr[+0x00]  (handler function pointer)
         *   arg3 = u16 at config_ptr[+0x2c]  (priority/flags)
         * config_ptr lives at switch_struct +0x10. Per sub_6511c0 the press/
         * release fire-gate is config_ptr[+0x2e]: bit 11 = fire on press,
         * bit 10 = fire on release, bit 12 = call sub_79fe00, bit 13 = call
         * sub_79fd70, bit 14 = wraps edge_counter mod 2. */
        uint64_t config_ptr_raw = *(uint64_t *)(e + 0x10);
        uint64_t handler_fn     = 0;
        uint64_t param_2        = 0;
        uint16_t event_id       = 0;
        uint16_t priority       = 0;
        uint16_t flags_2e       = 0;
        uint8_t  flags_30       = 0;
        if (config_ptr_raw != 0) {
            uint8_t *cfg = (uint8_t *)(uintptr_t)config_ptr_raw;
            handler_fn = *(uint64_t *)(cfg + 0x00);
            param_2    = *(uint64_t *)(cfg + 0x08);
            priority   = *(uint16_t *)(cfg + 0x2c);
            flags_2e   = *(uint16_t *)(cfg + 0x2e);
            flags_30   = *(uint8_t  *)(cfg + 0x30);
            event_id   = *(uint16_t *)(cfg + 0x34);
        }
        uint64_t handler_va = handler_fn ? (handler_fn - game_base) : 0;

        SHIM_LOG("[switch_table] sw_id=%3d  ch=%u byte=%u bit=%u  state=%u  cfg=%p  handler=0x%lx  param=0x%lx  evt=0x%04x  prio=0x%04x  flag2e=0x%04x  flag30=0x%02x",
                 sw_id, channel_id,
                 (unsigned)(bit_idx >> 3), (unsigned)(bit_idx & 7), state_byte,
                 (void *)(uintptr_t)config_ptr_raw,
                 (unsigned long)handler_va,
                 (unsigned long)param_2,
                 event_id, priority, flags_2e, flags_30);
        valid++;
    }
    SHIM_LOG("[switch_table] enumerated %d valid entries", valid);

    /* Decode the 36-bit mode bitmap at DAT_044cb9f0 (5 bytes calloc'd). The
     * service-button handler at sub_5902f0 gates on bits 3 and 4 being
     * INACTIVE — if either is set, sub_5902f0 returns without firing
     * sub_58ff40. Knowing which bits are set in the current state tells us
     * whether the service-button event path is even reachable. */
    {
        uint8_t **mode_bitmap_pp = (uint8_t **)(uintptr_t)(game_base + 0x44cb9f0ULL);
        uint8_t *mode_bitmap = *mode_bitmap_pp;
        uint64_t mode_count_p = *(uint64_t *)(uintptr_t)(game_base + 0x43527d0ULL);
        uint64_t mode_count = mode_count_p ? *(uint64_t *)(uintptr_t)mode_count_p : 0;
        if (mode_bitmap) {
            uint8_t b[5] = {0};
            memcpy(b, mode_bitmap, 5);
            char active[256] = "";
            int n = 0;
            for (int i = 0; i < 36; i++) {
                int set = (b[i >> 3] >> (i & 7)) & 1;
                if (set && n < (int)sizeof(active) - 8) {
                    n += snprintf(active + n, sizeof(active) - n, " %d", i);
                }
            }
            SHIM_LOG("[switch_table] mode_bitmap @ %p (count=%lu)  bytes=%02x %02x %02x %02x %02x  active modes:%s",
                     (void *)mode_bitmap, (unsigned long)mode_count,
                     b[0], b[1], b[2], b[3], b[4],
                     n ? active : " (none)");
        } else {
            SHIM_LOG("[switch_table] mode_bitmap pointer NULL (sub_7a8b90 hasn't run yet?)");
        }

        /* Boot-state mask at DAT_0445f838 (per PROTOCOL §3). bit 0 of this
         * gates sub_58ff40's Service Select path: if set, handler takes
         * sub_731930(9) path instead of firing event 0xa5. */
        uint32_t boot_mask = *(uint32_t *)(uintptr_t)(game_base + 0x445f838ULL);
        SHIM_LOG("[switch_table] boot_state_mask @ 0x%lx = 0x%08x (bit 0 = %d  bit 3 = %d  -- bit 0 routes Service Select away from event 0xa5)",
                 (unsigned long)(game_base + 0x445f838ULL),
                 boot_mask, boot_mask & 1, (boot_mask >> 3) & 1);

        /* Check FUN_0072a5c0(0x1a) result — if 1, sub_58ff40 takes another
         * early-exit path (Service-Select-while-something path). */
        typedef uint64_t (*chk_fn)(int);
        chk_fn check_72a5c0 = (chk_fn)(uintptr_t)(game_base + 0x72a5c0ULL);
        uint64_t r_72a5c0 = check_72a5c0(0x1a);
        SHIM_LOG("[switch_table] sub_72a5c0(0x1a) = 0x%lx (bit 0 = %d  -- if 1, Service Select hits early-return path)",
                 (unsigned long)r_72a5c0, (int)(r_72a5c0 & 1));
    }

    const char *p = getenv("SHIM_SELECT_PRESS");
    if (!p || !p[0] || p[0] == '0') {
        SHIM_LOG("[switch_table] === END === (no inject)");
        return NULL;
    }

    /* Comma-separated list of sw_ids to press in sequence. Default: just 90
     * (Service Select per PLE manual mapping 0-SW-8). Set
     * SHIM_SELECT_PRESS_SW_ID="90,91,92,93" to sweep all four service
     * buttons in turn. */
    const char *id_list = getenv("SHIM_SELECT_PRESS_SW_ID");
    if (!id_list || !id_list[0]) id_list = "90";

    int inject_ids[16];
    int n_inject = 0;
    {
        const char *p = id_list;
        while (*p && n_inject < 16) {
            int id = atoi(p);
            if (id >= 1 && id <= valid) inject_ids[n_inject++] = id;
            while (*p && *p != ',') p++;
            if (*p == ',') p++;
        }
    }
    if (n_inject == 0) {
        SHIM_LOG("[select_press] no valid sw_ids in '%s'; abort", id_list);
        return NULL;
    }

    int attempts = 4;
    if ((v = getenv("SHIM_SELECT_PRESS_ATTEMPTS")) != NULL && v[0]) attempts = atoi(v);
    int press_ms = 1500;
    if ((v = getenv("SHIM_SELECT_PRESS_MS")) != NULL && v[0]) press_ms = atoi(v);
    int interval_s = 3;
    if ((v = getenv("SHIM_SELECT_PRESS_INTERVAL_S")) != NULL && v[0]) interval_s = atoi(v);

    typedef int64_t (*translate_fn)(uint8_t channel_id, const uint8_t *buffer);
    translate_fn translate = (translate_fn)(game_base + 0x651610);

    for (int round = 0; round < attempts; round++) {
        for (int k = 0; k < n_inject; k++) {
            int sw_id = inject_ids[k];
            uint8_t *e = arr + (size_t)sw_id * 0x30u;
            uint16_t recorded = *(uint16_t *)(e + 0x20);
            if (recorded != (uint16_t)sw_id) {
                SHIM_LOG("[select_press] sw_id=%d not present (recorded=%u); skip",
                         sw_id, recorded);
                continue;
            }
            uint16_t bit_idx    = *(uint16_t *)(e + 0x22);
            uint8_t  channel_id = *(uint8_t  *)(e + 0x24);
            int byte_off = bit_idx >> 3;
            uint8_t bit_mask = (uint8_t)(1u << (bit_idx & 7));

            uint16_t edge_before = *(uint16_t *)(e + 0x26);
            uint8_t  state_before = *(uint8_t *)(e + 0x28);
            SHIM_LOG("[select_press] round %d/%d  sw_id=%d  channel=%u byte=%d bit=%u  (BEFORE: state=%u edge_cnt=%u)",
                     round + 1, attempts, sw_id, channel_id, byte_off,
                     (unsigned)(bit_idx & 7), state_before, edge_before);

            if (channel_id == 0 && byte_off >= 1 && byte_off < 8) {
                /* SPI thread builds DAT_0450d9b0 via
                 *   CONCAT44(uStack_54, CONCAT13(bStack_55, CONCAT21(uStack_57, local_58)))
                 * with local_58 zero-init at DAT byte 0:
                 *   DAT byte 0 = 0 (always)
                 *   DAT byte N = rx[N-1] for N=1..7   (rx[7] dropped)
                 *
                 * Setting rx_extra alone DOES NOT WORK — sub_8fa820 stores
                 * to DAT_0450d9b0 but never calls translate. The translate
                 * path (FUN_00674b90 → FUN_008fab80 → FUN_00651610) is only
                 * invoked from sub_675a10 (one-shot, init-gated) and from
                 * nodebus init/enum loops. Post-init, no periodic caller.
                 *
                 * So we BOTH set rx_extra (in case some path reads
                 * DAT_0450d9b0) AND call FUN_00674b90(0) directly to force
                 * a translate(0, ...) cycle that picks up the bit. */
                int rx_off = byte_off - 1;
                typedef void (*poll_ch0_fn)(int);
                poll_ch0_fn poll_ch0 = (poll_ch0_fn)(uintptr_t)
                    (game_base + 0x674b90ULL);

                SHIM_LOG("[select_press]   PRESS via SPI+poll  (channel-byte=%d → rx[%d] |= 0x%02x; calling FUN_00674b90(0))",
                         byte_off, rx_off, bit_mask);
                shim_spi_rx_extra[rx_off] |= bit_mask;
                usleep(5000);  /* let SPI thread cycle once with new rx */
                poll_ch0(0);   /* force translate(0, ...) with current DAT_0450d9b0 */
                usleep((useconds_t)press_ms * 1000);
                uint16_t edge_during = *(uint16_t *)(e + 0x26);
                uint8_t  state_during = *(uint8_t *)(e + 0x28);
                SHIM_LOG("[select_press]   RELEASE  (DURING-press: state=%u edge_cnt=%u)",
                         state_during, edge_during);
                shim_spi_rx_extra[rx_off] &= (uint8_t)~bit_mask;
                usleep(5000);
                poll_ch0(0);   /* force translate(0, ...) with bit cleared (release edge) */
            } else if (channel_id != 0) {
                uint8_t buf[8];
                SHIM_LOG("[select_press]   PRESS via translate(ch=%u, buf[%d]=0x%02x)",
                         channel_id, byte_off, bit_mask);
                memset(buf, 0, sizeof(buf));
                buf[byte_off] = bit_mask;
                translate(channel_id, buf);
                usleep((useconds_t)press_ms * 1000);
                uint16_t edge_during = *(uint16_t *)(e + 0x26);
                uint8_t  state_during = *(uint8_t *)(e + 0x28);
                SHIM_LOG("[select_press]   RELEASE  (DURING-press: state=%u edge_cnt=%u)",
                         state_during, edge_during);
                memset(buf, 0, sizeof(buf));
                translate(channel_id, buf);
            }

            usleep(200000);  /* 200ms after release for drain to process */
            uint16_t edge_after = *(uint16_t *)(e + 0x26);
            uint8_t  state_after = *(uint8_t *)(e + 0x28);
            SHIM_LOG("[select_press]   AFTER (post-release+200ms): state=%u edge_cnt=%u",
                     state_after, edge_after);

            sleep((unsigned)interval_s);
        }
    }
    SHIM_LOG("[switch_table] === END === (inject complete)");
    return NULL;
}

/* SHIM_EDGE_SNAPSHOT=1 — periodically snapshot every switch's edge counter
 * (+0x26 of g_switch_state_array entry) and log only the entries that
 * incremented since the previous snapshot. Identifies which switches are
 * firing edges continuously (= the audio-spam source).
 *
 * SHIM_EDGE_SNAPSHOT_DELAY_S=N — wait N seconds before first snapshot
 *   (default 30; long enough that channel-1-idle fix has taken effect)
 * SHIM_EDGE_SNAPSHOT_INTERVAL_S=N — gap between snapshots (default 3)
 * SHIM_EDGE_SNAPSHOT_CYCLES=N — total snapshots (default 30) */
static void *shim_edge_snapshot_thread(void *arg) {
    (void)arg;
    const char *v = getenv("SHIM_EDGE_SNAPSHOT");
    if (!v || !v[0] || v[0] == '0') return NULL;

    int start_delay_s = 30;
    if ((v = getenv("SHIM_EDGE_SNAPSHOT_DELAY_S")) != NULL && v[0]) start_delay_s = atoi(v);
    int interval_s = 3;
    if ((v = getenv("SHIM_EDGE_SNAPSHOT_INTERVAL_S")) != NULL && v[0]) interval_s = atoi(v);
    int cycles = 30;
    if ((v = getenv("SHIM_EDGE_SNAPSHOT_CYCLES")) != NULL && v[0]) cycles = atoi(v);

    sleep((unsigned)start_delay_s);

    uintptr_t game_base = shim_resolve_game_base();
    if (!game_base) {
        SHIM_LOG("[edge_snap] game_base unresolved; skip");
        return NULL;
    }

    uint8_t **arr_pp = (uint8_t **)(uintptr_t)(game_base + 0x4480220ULL);
    uint8_t *arr = *arr_pp;
    if (!arr) {
        SHIM_LOG("[edge_snap] g_switch_state_array NULL; skip");
        return NULL;
    }

    enum { MAX_SW = 100 };
    uint16_t prev[MAX_SW] = {0};

    SHIM_LOG("[edge_snap] starting %d cycles × %ds — logs switches with edge_counter changes",
             cycles, interval_s);

    for (int cycle = 0; cycle < cycles; cycle++) {
        sleep((unsigned)interval_s);
        char line[2048];
        int n = snprintf(line, sizeof(line), "[edge_snap] cycle %d:", cycle);
        int any = 0;
        for (int sw_id = 1; sw_id < MAX_SW; sw_id++) {
            uint8_t *e = arr + (size_t)sw_id * 0x30u;
            uint16_t recorded = *(uint16_t *)(e + 0x20);
            if (recorded != (uint16_t)sw_id) break;
            uint16_t cur = *(uint16_t *)(e + 0x26);
            if (cur != prev[sw_id]) {
                if (n < (int)sizeof(line) - 32) {
                    n += snprintf(line + n, sizeof(line) - n,
                                  " sw%d:%u→%u", sw_id, prev[sw_id], cur);
                }
                prev[sw_id] = cur;
                any++;
            }
        }
        if (any > 0) {
            SHIM_LOG("%s (%d changed)", line, any);
        }
    }
    SHIM_LOG("[edge_snap] complete");
    return NULL;
}

void shim_inject_start(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = shim_event_sweep_alarm_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);

    sigset_t block_alarm;
    sigemptyset(&block_alarm);
    sigaddset(&block_alarm, SIGALRM);
    pthread_sigmask(SIG_BLOCK, &block_alarm, NULL);

    pthread_t t;
    if (pthread_create(&t, NULL, shim_discovery_init_thread, NULL) == 0)
        pthread_detach(t);

    pthread_t s;
    if (pthread_create(&s, NULL, shim_select_press_thread, NULL) == 0)
        pthread_detach(s);

    pthread_t n;
    if (pthread_create(&n, NULL, shim_node_records_probe_thread, NULL) == 0)
        pthread_detach(n);

    pthread_t st;
    if (pthread_create(&st, NULL, shim_switch_table_probe_thread, NULL) == 0)
        pthread_detach(st);

    pthread_t es;
    if (pthread_create(&es, NULL, shim_edge_snapshot_thread, NULL) == 0)
        pthread_detach(es);
}
