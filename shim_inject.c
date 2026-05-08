

























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
}
