

























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
#include <dirent.h>
#include <fcntl.h>
#include <sys/syscall.h>

#include "spi_shim.h"
#include "shim_discovery.h"
#include "games/pokemon_le_machine.h"

/* Sweep globals — read by shim_sigsegv_handler (in shim_diagnostics.c) to
 * longjmp back out of a faulting sweep iteration. Externed in spi_shim.h. */
sigjmp_buf shim_event_sweep_jmp;
volatile sig_atomic_t shim_event_fire_in_progress = 0;
volatile int shim_current_sweep_event_id = -1;

static void shim_copy_channel_state_buffer(uintptr_t game_base,
                                           uint8_t channel_id,
                                           uint8_t out[8]);
static void shim_make_channel0_idle_buffer(uint8_t out[8]);
static void shim_log_channel_buf(const char *tag, uint8_t channel_id,
                                 const uint8_t buf[8]);

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
 * is non-zero (i.e., FUN_00650f00 has populated it), then mirror changes to
 * DAT_0436b020 while the CPU-SPI/channel-1 state machine settles during boot. */
static void shim_install_channel1_idle(uintptr_t game_base) {
    uint8_t *channel_1_saved = (uint8_t *)(uintptr_t)
        (game_base + 0x44804d0ULL + 0xb0ULL);
    volatile uint64_t *dat_0436b020 = (volatile uint64_t *)(uintptr_t)
        (game_base + 0x436b020ULL);
    uint64_t last_written = 0;
    int first_ms = -1;
    for (int i = 0; i < 300; i++) {
        uint64_t idle_u64 = 0;
        memcpy(&idle_u64, channel_1_saved, 8);
        if (idle_u64 != 0) {
            if (first_ms < 0) first_ms = i * 100;
            if (idle_u64 != last_written) {
                *dat_0436b020 = idle_u64;
                last_written = idle_u64;
                SHIM_LOG("[channel1_idle] wrote 0x%016lx to DAT_0436b020 after %dms (FUN_00674b90 spam fix)",
                         (unsigned long)idle_u64, i * 100);
            }
            if (i * 100 >= first_ms + 15000) {
                SHIM_LOG("[channel1_idle] settled at 0x%016lx after %dms",
                         (unsigned long)last_written, i * 100);
                return;
            }
        }
        usleep(100000);
    }
    if (last_written)
        SHIM_LOG("[channel1_idle] stopped monitoring at 0x%016lx after 30s",
                 (unsigned long)last_written);
    else
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
    shim_patch_channel1_derived_switches();

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

            if (channel_id == 0 && byte_off < 8) {
                uint8_t buf[8];
                shim_make_channel0_idle_buffer(buf);
                shim_log_channel_buf("[select_press]   ch0 base", channel_id, buf);
                buf[byte_off] &= (uint8_t)~bit_mask;
                shim_log_channel_buf("[select_press]   ch0 press", channel_id, buf);
                translate(channel_id, buf);
                usleep((useconds_t)press_ms * 1000);
                uint16_t edge_during = *(uint16_t *)(e + 0x26);
                uint8_t  state_during = *(uint8_t *)(e + 0x28);
                SHIM_LOG("[select_press]   RELEASE  (DURING-press: state=%u edge_cnt=%u)",
                         state_during, edge_during);
                shim_make_channel0_idle_buffer(buf);
                shim_log_channel_buf("[select_press]   ch0 release", channel_id, buf);
                translate(channel_id, buf);
            } else if (channel_id != 0) {
                uint8_t buf[8];
                if (byte_off >= 8) {
                    SHIM_LOG("[select_press]   bit_idx=%u out of remote buffer range; skip",
                             bit_idx);
                    continue;
                }
                shim_copy_channel_state_buffer(game_base, channel_id, buf);
                shim_log_channel_buf("[select_press]   remote base", channel_id, buf);
                uint8_t saved_byte = buf[byte_off];
                buf[byte_off] ^= bit_mask;
                shim_log_channel_buf("[select_press]   remote press", channel_id, buf);
                translate(channel_id, buf);
                usleep((useconds_t)press_ms * 1000);
                uint16_t edge_during = *(uint16_t *)(e + 0x26);
                uint8_t  state_during = *(uint8_t *)(e + 0x28);
                SHIM_LOG("[select_press]   RELEASE  (DURING-press: state=%u edge_cnt=%u)",
                         state_during, edge_during);
                shim_copy_channel_state_buffer(game_base, channel_id, buf);
                buf[byte_off] = saved_byte;
                shim_log_channel_buf("[select_press]   remote release", channel_id, buf);
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

/* SHIM_MACHINE_DUMP=1 — join the runtime switch table against the per-game
 * mechanical inventory in games/pokemon_le_machine.{c,h}. Logs each manual
 * entry's runtime sw_id (or "missing" if the manual lists a switch not in
 * the runtime array), plus the full driver and lamp tables.
 *
 * SHIM_MACHINE_DUMP_DELAY_S=N — wait N seconds before dumping (default 70;
 * pairs naturally with SHIM_SWITCH_TABLE_PROBE_DELAY_S=65). */
static void *shim_machine_dump_thread(void *arg) {
    (void)arg;
    const char *v = getenv("SHIM_MACHINE_DUMP");
    if (!v || !v[0] || v[0] == '0') return NULL;

    int delay_s = 70;
    if ((v = getenv("SHIM_MACHINE_DUMP_DELAY_S")) != NULL && v[0])
        delay_s = atoi(v);
    if (delay_s > 0) sleep((unsigned)delay_s);

    uintptr_t game_base = shim_resolve_game_base();
    if (!game_base) {
        SHIM_LOG("[machine_dump] game_base unresolved; abort");
        return NULL;
    }

    SHIM_LOG("[machine_dump] === START === switches=%zu drivers=%zu lamps=%zu",
             ple_switches_count, ple_drivers_count, ple_lamps_count);

    uint8_t **arr_pp = (uint8_t **)(uintptr_t)(game_base + 0x4480220ULL);
    uint8_t *arr = *arr_pp;
    int matched = 0, missing = 0;

    for (size_t i = 0; i < ple_switches_count; i++) {
        const ple_switch_t *m = &ple_switches[i];
        int found_sw_id = -1;
        if (arr) {
            for (int sw_id = 1; sw_id < 256; sw_id++) {
                uint8_t *e = arr + (size_t)sw_id * 0x30u;
                uint16_t recorded = *(uint16_t *)(e + 0x20);
                if (recorded != (uint16_t)sw_id) break;
                uint16_t bit_idx = *(uint16_t *)(e + 0x22);
                uint8_t channel = *(uint8_t *)(e + 0x24);
                if (channel == m->channel && bit_idx == m->bit_idx) {
                    found_sw_id = sw_id;
                    break;
                }
            }
        }
        if (found_sw_id >= 0) {
            SHIM_LOG("[machine_dump] SW %-7s ch=%u bit=%2u sw_id=%3d  %s",
                     m->manual_id, m->channel, m->bit_idx, found_sw_id, m->name);
            matched++;
        } else {
            SHIM_LOG("[machine_dump] SW %-7s ch=%u bit=%2u sw_id=---  %s  (MISSING from runtime)",
                     m->manual_id, m->channel, m->bit_idx, m->name);
            missing++;
        }
    }
    SHIM_LOG("[machine_dump] switches: %d matched / %d missing of %zu manual entries",
             matched, missing, ple_switches_count);

    for (size_t i = 0; i < ple_drivers_count; i++) {
        const ple_driver_t *d = &ple_drivers[i];
        const char *kind_name = "?";
        switch (d->kind) {
        case PLE_KIND_COIL: kind_name = "COIL";    break;
        case PLE_KIND_MOTOR: kind_name = "MOTOR";  break;
        case PLE_KIND_SERVO: kind_name = "SERVO";  break;
        case PLE_KIND_MAGNET: kind_name = "MAGNET"; break;
        case PLE_KIND_DIGITAL: kind_name = "DIGITAL"; break;
        }
        SHIM_LOG("[machine_dump] DR %-7s slot=%2u idx=%2u kind=%s  %s",
                 d->manual_id, d->slot, d->dr_idx, kind_name, d->name);
    }

    for (size_t i = 0; i < ple_lamps_count; i++) {
        const ple_lamp_t *l = &ple_lamps[i];
        const char *kind_name = "?";
        switch (l->kind) {
        case PLE_LAMP_GI: kind_name = "GI";          break;
        case PLE_LAMP_FEATURE: kind_name = "FEATURE"; break;
        case PLE_LAMP_RGB_R: kind_name = "RGB_R";    break;
        case PLE_LAMP_RGB_G: kind_name = "RGB_G";    break;
        case PLE_LAMP_RGB_B: kind_name = "RGB_B";    break;
        case PLE_LAMP_FLASHER: kind_name = "FLASHER"; break;
        case PLE_LAMP_DIGITAL: kind_name = "DIGITAL"; break;
        }
        SHIM_LOG("[machine_dump] LP %-7s slot=%2u idx=%3u kind=%-7s %s",
                 l->manual_id, l->slot, l->lp_idx, kind_name, l->name);
    }

    SHIM_LOG("[machine_dump] === END ===");
    return NULL;
}

/* SHIM_OUTPUT_PROBE=1 — periodically poll g_hardware_output_state at game
 * VA 0x450da30 (8 bytes, written by set_hardware_output_bit @ 0x8fac70 and
 * write_hardware_output_byte @ 0x8fad0c, transmitted by SPI bridge thread
 * to the MCU on every cycle). Diff against last sample, log byte-level
 * changes with timestamps.
 *
 * Verified Phase-0 (2026-05-08, Ghidra MCP):
 *   - 0x450da30: 8-byte g_hardware_output_state; mutex 0x450d9f8
 *   - 0x450da28: init flag (set 1 by SPI bridge thread once running)
 *
 * Env knobs:
 *   SHIM_OUTPUT_PROBE=1            — enable
 *   SHIM_OUTPUT_PROBE_DELAY_S=N    — wait N seconds before first poll (default 50)
 *   SHIM_OUTPUT_PROBE_INTERVAL_MS=N — gap between polls in ms (default 100)
 *   SHIM_OUTPUT_PROBE_DURATION_S=N — total polling duration (default 60) */
static void *shim_output_probe_thread(void *arg) {
    (void)arg;
    const char *v = getenv("SHIM_OUTPUT_PROBE");
    if (!v || !v[0] || v[0] == '0') return NULL;

    int delay_s = 50;
    int interval_ms = 100;
    int duration_s = 60;
    if ((v = getenv("SHIM_OUTPUT_PROBE_DELAY_S")) != NULL && v[0]) delay_s = atoi(v);
    if ((v = getenv("SHIM_OUTPUT_PROBE_INTERVAL_MS")) != NULL && v[0]) interval_ms = atoi(v);
    if ((v = getenv("SHIM_OUTPUT_PROBE_DURATION_S")) != NULL && v[0]) duration_s = atoi(v);

    if (delay_s > 0) sleep((unsigned)delay_s);

    uintptr_t game_base = shim_resolve_game_base();
    if (!game_base) {
        SHIM_LOG("[output_probe] game_base unresolved; abort");
        return NULL;
    }

    volatile uint8_t *out_state = (volatile uint8_t *)(uintptr_t)(game_base + 0x450da30ULL);
    volatile uint8_t *init_flag = (volatile uint8_t *)(uintptr_t)(game_base + 0x450da28ULL);
    SHIM_LOG("[output_probe] === START === out_state @ %p init_flag=%u  duration=%ds interval=%dms",
             (void *)out_state, *init_flag, duration_s, interval_ms);

    uint8_t prev[8];
    memcpy(prev, (const void *)out_state, 8);
    SHIM_LOG("[output_probe] initial: %02x %02x %02x %02x %02x %02x %02x %02x",
             prev[0], prev[1], prev[2], prev[3], prev[4], prev[5], prev[6], prev[7]);

    int total_ticks = (duration_s * 1000) / (interval_ms > 0 ? interval_ms : 100);
    int changes = 0;
    for (int tick = 0; tick < total_ticks; tick++) {
        usleep((useconds_t)interval_ms * 1000);
        uint8_t cur[8];
        memcpy(cur, (const void *)out_state, 8);
        if (memcmp(cur, prev, 8) != 0) {
            char line[256];
            int n = snprintf(line, sizeof(line),
                             "[output_probe] tick=%d  %02x %02x %02x %02x %02x %02x %02x %02x  diff:",
                             tick, cur[0], cur[1], cur[2], cur[3],
                             cur[4], cur[5], cur[6], cur[7]);
            for (int b = 0; b < 8; b++) {
                if (cur[b] != prev[b] && n < (int)sizeof(line) - 32) {
                    uint8_t diff = cur[b] ^ prev[b];
                    n += snprintf(line + n, sizeof(line) - n,
                                  " byte%d:%02x->%02x(^%02x)",
                                  b, prev[b], cur[b], diff);
                }
            }
            SHIM_LOG("%s", line);
            memcpy(prev, cur, 8);
            changes++;
        }
    }
    SHIM_LOG("[output_probe] === END === %d changes over %ds", changes, duration_s);
    return NULL;
}

/* SHIM_DISPATCH_WATCHDOG=1 — periodically check whether the procDispatch
 * thread is still ticking by reading g_timestamp_threshold_1 @ 0x44cb2e0
 * (PROTOCOL §3 Phase 5). If the timestamp doesn't advance for N seconds,
 * send SIGUSR1 to the procDispatch thread (located by /proc/self/task name
 * match) — the existing signal handler dumps a backtrace ON THE STALLED
 * THREAD, telling us which subsystem call inside FUN_00720cf0 is blocked
 * when status-5 dispatch timeout would fire.
 *
 * Knobs:
 *   SHIM_DISPATCH_WATCHDOG_DELAY_S=N      — wait N s before starting (default 30)
 *   SHIM_DISPATCH_WATCHDOG_THRESHOLD_S=N  — fire SIGUSR1 after N s of no
 *                                          timestamp change (default 7;
 *                                          must be < 10 s game-side timeout) */
static pid_t shim_find_thread_by_name(const char *name) {
    DIR *d = opendir("/proc/self/task");
    if (!d) return -1;
    pid_t result = -1;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char path[320];
        snprintf(path, sizeof(path), "/proc/self/task/%s/comm", de->d_name);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        char buf[32] = {0};
        ssize_t r = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (r <= 0) continue;
        char *nl = strchr(buf, '\n');
        if (nl) *nl = 0;
        if (strcmp(buf, name) == 0) {
            result = (pid_t)atoi(de->d_name);
            break;
        }
    }
    closedir(d);
    return result;
}

static void *shim_dispatch_watchdog_thread(void *arg) {
    (void)arg;
    const char *v = getenv("SHIM_DISPATCH_WATCHDOG");
    if (!v || !v[0] || v[0] == '0') return NULL;

    int delay_s = 30;
    int threshold_s = 7;
    if ((v = getenv("SHIM_DISPATCH_WATCHDOG_DELAY_S")) != NULL && v[0]) delay_s = atoi(v);
    if ((v = getenv("SHIM_DISPATCH_WATCHDOG_THRESHOLD_S")) != NULL && v[0]) threshold_s = atoi(v);
    if (threshold_s < 2) threshold_s = 2;
    if (threshold_s > 9) threshold_s = 9;

    if (delay_s > 0) sleep((unsigned)delay_s);

    uintptr_t game_base = shim_resolve_game_base();
    if (!game_base) {
        SHIM_LOG("[watchdog] game_base unresolved; abort");
        return NULL;
    }
    volatile uint64_t *ts_ptr = (volatile uint64_t *)(uintptr_t)
        (game_base + 0x44cb2e0ULL);
    pid_t my_pid = getpid();

    /* qemu-user prefixes guest thread names with its own process name and
     * truncates the combined string to 15 chars (TASK_COMM_LEN-1). So the
     * "procDispatch" pthread_setname_np in the game becomes the comm value
     * "qemu-aarch64:pr" in the host kernel. Try the qemu-mangled form first,
     * fall back to the unprefixed name (in case qemu version differs). */
    pid_t target_tid = -1;
    int spin = 0;
    while (target_tid < 0) {
        target_tid = shim_find_thread_by_name("qemu-aarch64:pr");
        if (target_tid < 0)
            target_tid = shim_find_thread_by_name("procDispatch");
        if (target_tid < 0) {
            if (spin == 30 || spin == 90 || spin == 180 || spin == 300)
                SHIM_LOG("[watchdog] still searching for procDispatch (waited %ds)", spin);
            spin++;
            sleep(1);
        }
    }
    if (target_tid < 0) {
        SHIM_LOG("[watchdog] could not find 'procDispatch' thread after 30s; "
                 "dumping all thread names found for diagnosis:");
        DIR *d = opendir("/proc/self/task");
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                if (de->d_name[0] == '.') continue;
                char path[320], buf[32] = {0};
                snprintf(path, sizeof(path), "/proc/self/task/%s/comm", de->d_name);
                int fd = open(path, O_RDONLY);
                if (fd < 0) continue;
                ssize_t r = read(fd, buf, sizeof(buf) - 1);
                close(fd);
                if (r <= 0) continue;
                char *nl = strchr(buf, '\n');
                if (nl) *nl = 0;
                SHIM_LOG("[watchdog]   tid=%s comm=\"%s\"", de->d_name, buf);
            }
            closedir(d);
        }
        return NULL;
    }

    SHIM_LOG("[watchdog] === START === pid=%d procDispatch_tid=%d ts_ptr=%p threshold=%ds",
             (int)my_pid, (int)target_tid, (void *)ts_ptr, threshold_s);

    /* Wait until the dispatcher has ticked at least once (timestamp != 0).
     * Without this, the first 7s after watchdog start always look like a
     * stall because procDispatch hasn't yet entered FUN_00720cf0. */
    uint64_t prev_ts = *ts_ptr;
    int warmup_logged = 0;
    while (prev_ts == 0) {
        sleep(1);
        prev_ts = *ts_ptr;
        if (!warmup_logged) {
            SHIM_LOG("[watchdog] dispatcher ts still 0 — waiting for first tick");
            warmup_logged = 1;
        }
    }
    SHIM_LOG("[watchdog] first dispatcher tick observed (ts=0x%lx); now monitoring",
             (unsigned long)prev_ts);

    int stuck_secs = 0;
    int last_signaled = 0;
    while (1) {
        sleep(1);
        uint64_t cur_ts = *ts_ptr;
        if (cur_ts != prev_ts) {
            if (last_signaled) {
                SHIM_LOG("[watchdog] resumed after stall: ts %lu -> %lu",
                         (unsigned long)prev_ts, (unsigned long)cur_ts);
                last_signaled = 0;
            }
            prev_ts = cur_ts;
            stuck_secs = 0;
            continue;
        }
        stuck_secs++;
        if (stuck_secs == threshold_s) {
            /* Use SIGURG (16): procDispatch's SigBlk (0x2a02) blocks SIGUSR1
             * (bit 9), so an SIGUSR1 sent here gets queued in SigPnd but
             * never delivered. SIGURG is unblocked and its default
             * disposition is ignore (won't crash if no handler installed),
             * so the shim's SIGURG handler can dump procDispatch's stack. */
            SHIM_LOG("[watchdog] STALL detected: ts unchanged for %ds at value 0x%lx; sending SIGURG to tid %d",
                     stuck_secs, (unsigned long)cur_ts, (int)target_tid);
            if (syscall(SYS_tgkill, my_pid, target_tid, SIGURG) != 0) {
                SHIM_LOG("[watchdog] tgkill failed: errno=%d (%s)",
                         errno, strerror(errno));
            }
            last_signaled = 1;
        }
    }
}

static void shim_copy_channel_state_buffer(uintptr_t game_base,
                                           uint8_t channel_id,
                                           uint8_t out[8]) {
    uintptr_t channel_state = game_base + 0x44804b8ULL
                            + (uintptr_t)channel_id * 0xb0ULL + 0x20ULL;
    memcpy(out, (const void *)channel_state, 8);
}

static void shim_make_channel0_idle_buffer(uint8_t out[8]) {
    static const uint8_t idle[8] =
        { 0x00, 0xff, 0xff, 0xf8, 0xff, 0xff, 0xff, 0xff };
    memcpy(out, idle, sizeof(idle));

    const char *coin_door = getenv("SPIKE3_COIN_DOOR_INTERLOCK");
    if (!coin_door || !coin_door[0] ||
        strcmp(coin_door, "closed") == 0 ||
        strcmp(coin_door, "restored") == 0 ||
        strcmp(coin_door, "1") == 0) {
        out[2] &= (uint8_t)~0x80u;
    } else if (strcmp(coin_door, "open") == 0 || strcmp(coin_door, "0") == 0) {
        out[2] |= 0x80u;
    }
}

static void shim_log_channel_buf(const char *tag, uint8_t channel_id,
                                 const uint8_t buf[8]) {
    SHIM_LOG("%s ch=%u buf=%02x %02x %02x %02x %02x %02x %02x %02x",
             tag, channel_id,
             buf[0], buf[1], buf[2], buf[3],
             buf[4], buf[5], buf[6], buf[7]);
}

/* SHIM_MANUAL_PRESS=1 — poll runs/latest/press_request.txt every 200 ms;
 * when the file appears, parse its first int as a runtime sw_id, delete the
 * file, and drive ONE press/release through the same code path that
 * shim_select_press_thread uses (SPI rx-extra + force-poll for channel 0;
 * direct translate() call for remote nodes). Enables marker_button.py to
 * trigger presses interactively, synced to what the user sees on screen.
 *
 * Env knobs:
 *   SHIM_MANUAL_PRESS=1               — enable
 *   SHIM_MANUAL_PRESS_DELAY_S=N       — wait N s before first poll (default 0)
 *   SHIM_MANUAL_PRESS_MS=N            — press hold ms (default 500) */
static void shim_press_one_sw_id(uintptr_t game_base, uint8_t *arr, int sw_id, int press_ms) {
    typedef int64_t (*translate_fn)(uint8_t channel_id, const uint8_t *buffer);
    translate_fn translate = (translate_fn)(game_base + 0x651610);

    uint8_t *e = arr + (size_t)sw_id * 0x30u;
    uint16_t recorded = *(uint16_t *)(e + 0x20);
    if (recorded != (uint16_t)sw_id) {
        SHIM_LOG("[manual_press] sw_id=%d not present (recorded=%u); skip",
                 sw_id, recorded);
        return;
    }
    uint16_t bit_idx    = *(uint16_t *)(e + 0x22);
    uint8_t  channel_id = *(uint8_t  *)(e + 0x24);
    int byte_off = bit_idx >> 3;
    uint8_t bit_mask = (uint8_t)(1u << (bit_idx & 7));
    uint16_t edge_before = *(uint16_t *)(e + 0x26);
    uint8_t  state_before = *(uint8_t *)(e + 0x28);
    SHIM_LOG("[manual_press] sw_id=%d ch=%u byte=%d bit=%u  (BEFORE: state=%u edge=%u)",
             sw_id, channel_id, byte_off, (unsigned)(bit_idx & 7),
             state_before, edge_before);

    if (channel_id == 0 && byte_off < 8) {
        uint8_t buf[8];
        shim_make_channel0_idle_buffer(buf);
        shim_log_channel_buf("[manual_press] ch0 base", channel_id, buf);
        buf[byte_off] &= (uint8_t)~bit_mask;
        shim_log_channel_buf("[manual_press] ch0 press", channel_id, buf);
        translate(channel_id, buf);
        usleep((useconds_t)press_ms * 1000);
        shim_make_channel0_idle_buffer(buf);
        shim_log_channel_buf("[manual_press] ch0 release", channel_id, buf);
        translate(channel_id, buf);
    } else if (channel_id != 0) {
        uint8_t buf[8];
        if (byte_off >= 8) {
            SHIM_LOG("[manual_press] bit_idx=%u out of remote buffer range; skip", bit_idx);
            return;
        }
        shim_copy_channel_state_buffer(game_base, channel_id, buf);
        shim_log_channel_buf("[manual_press] remote base", channel_id, buf);
        uint8_t saved_byte = buf[byte_off];
        buf[byte_off] ^= bit_mask;
        shim_log_channel_buf("[manual_press] remote press", channel_id, buf);
        translate(channel_id, buf);
        usleep((useconds_t)press_ms * 1000);
        shim_copy_channel_state_buffer(game_base, channel_id, buf);
        buf[byte_off] = saved_byte;
        shim_log_channel_buf("[manual_press] remote release", channel_id, buf);
        translate(channel_id, buf);
    }
    usleep(200000);
    uint16_t edge_after = *(uint16_t *)(e + 0x26);
    uint8_t  state_after = *(uint8_t *)(e + 0x28);
    SHIM_LOG("[manual_press] sw_id=%d AFTER: state=%u edge=%u (delta=%d)",
             sw_id, state_after, edge_after, edge_after - edge_before);
}

static void *shim_manual_press_thread(void *arg) {
    (void)arg;
    const char *v = getenv("SHIM_MANUAL_PRESS");
    if (!v || !v[0] || v[0] == '0') return NULL;

    int delay_s = 0;
    int press_ms = 500;
    if ((v = getenv("SHIM_MANUAL_PRESS_DELAY_S")) != NULL && v[0]) delay_s = atoi(v);
    if ((v = getenv("SHIM_MANUAL_PRESS_MS")) != NULL && v[0]) press_ms = atoi(v);

    /* Resolve the press_request.txt path relative to the run dir. The shim
     * runs with cwd = $GAME_DIR (per run_game.sh's `cd "$GAME_DIR"` before
     * exec qemu), so we use the absolute path through _emulation/runs/latest. */
    const char *req_path = "/mnt/c/Workspace/pokemon-le-0.81.0/games/pokemon_le/"
                           "_emulation/runs/latest/press_request.txt";

    if (delay_s > 0) sleep((unsigned)delay_s);
    unlink(req_path);

    SHIM_LOG("[manual_press] === START === press_ms=%d req_path=%s",
             press_ms, req_path);

    uintptr_t game_base = 0;
    while (1) {
        usleep(200000);  /* 200 ms poll */
        int fd = open(req_path, O_RDONLY);
        if (fd < 0) continue;
        char buf[64] = {0};
        ssize_t r = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        unlink(req_path);
        if (r <= 0) continue;
        int sw_id = atoi(buf);
        if (sw_id < 1 || sw_id > 255) {
            SHIM_LOG("[manual_press] bogus sw_id=%d in request '%.32s'; skip", sw_id, buf);
            continue;
        }

        game_base = shim_resolve_game_base();
        if (!game_base) {
            SHIM_LOG("[manual_press] game_base unresolved; drop sw_id=%d", sw_id);
            continue;
        }

        uint8_t **arr_pp = (uint8_t **)(uintptr_t)(game_base + 0x4480220ULL);
        uint8_t *arr = *arr_pp;
        if (!arr) {
            SHIM_LOG("[manual_press] g_switch_state_array NULL; drop sw_id=%d", sw_id);
            continue;
        }
        shim_press_one_sw_id(game_base, arr, sw_id, press_ms);
    }
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

    pthread_t md;
    if (pthread_create(&md, NULL, shim_machine_dump_thread, NULL) == 0)
        pthread_detach(md);

    pthread_t op;
    if (pthread_create(&op, NULL, shim_output_probe_thread, NULL) == 0)
        pthread_detach(op);

    pthread_t wd;
    if (pthread_create(&wd, NULL, shim_dispatch_watchdog_thread, NULL) == 0)
        pthread_detach(wd);

    pthread_t mp;
    if (pthread_create(&mp, NULL, shim_manual_press_thread, NULL) == 0)
        pthread_detach(mp);

    shim_devbus_init();
}
