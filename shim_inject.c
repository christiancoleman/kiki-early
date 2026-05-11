

























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
#include <sys/file.h>
#include <sys/syscall.h>

#include "spi_shim.h"
#include "shim_discovery.h"
#include "games/pokemon_le_machine.h"

/* Sweep globals — read by shim_sigsegv_handler (in shim_diagnostics.c) to
 * longjmp back out of a faulting sweep iteration. Externed in spi_shim.h. */
sigjmp_buf shim_event_sweep_jmp;
volatile sig_atomic_t shim_event_fire_in_progress = 0;
volatile int shim_current_sweep_event_id = -1;

static pthread_mutex_t shim_switch_hold_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint8_t shim_switch_hold_mask[16][8];

static void shim_copy_channel_idle_buffer(uintptr_t game_base,
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
 */
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

    SHIM_LOG("[switch_table] === END ===");
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

static void shim_copy_channel_idle_buffer(uintptr_t game_base,
                                          uint8_t channel_id,
                                          uint8_t out[8]) {
    uintptr_t channel_idle = game_base + 0x44804d0ULL
                           + (uintptr_t)channel_id * 0xb0ULL;
    memcpy(out, (const void *)channel_idle, 8);
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

void shim_switch_hold_set(uint8_t channel_id, uint16_t bit_idx, int held) {
    if (channel_id >= 16 || bit_idx >= 64) return;
    uint8_t byte_off = (uint8_t)(bit_idx >> 3);
    uint8_t bit_mask = (uint8_t)(1u << (bit_idx & 7));

    pthread_mutex_lock(&shim_switch_hold_mutex);
    if (held)
        shim_switch_hold_mask[channel_id][byte_off] |= bit_mask;
    else
        shim_switch_hold_mask[channel_id][byte_off] &= (uint8_t)~bit_mask;
    pthread_mutex_unlock(&shim_switch_hold_mutex);
}

void shim_switch_hold_apply(uint8_t channel_id, uint8_t buf[8]) {
    if (!buf || channel_id >= 16) return;

    uint8_t mask[8];
    pthread_mutex_lock(&shim_switch_hold_mutex);
    memcpy(mask, shim_switch_hold_mask[channel_id], sizeof(mask));
    pthread_mutex_unlock(&shim_switch_hold_mutex);

    for (int i = 0; i < 8; i++) {
        if (channel_id == 0)
            buf[i] &= (uint8_t)~mask[i];
        else
            buf[i] ^= mask[i];
    }
}

static void shim_log_channel_buf(const char *tag, uint8_t channel_id,
                                 const uint8_t buf[8]) {
    SHIM_LOG("%s ch=%u buf=%02x %02x %02x %02x %02x %02x %02x %02x",
             tag, channel_id,
             buf[0], buf[1], buf[2], buf[3],
             buf[4], buf[5], buf[6], buf[7]);
}

static void shim_log_start_gate_snapshot(uintptr_t game_base, const char *phase,
                                         uint8_t *switch_entry) {
    typedef int (*switch_read_fn)(int);
    typedef int (*mode_gate_fn)(int);

    switch_read_fn switch_read = (switch_read_fn)(uintptr_t)(game_base + 0x650850ULL);
    mode_gate_fn mode_gate = (mode_gate_fn)(uintptr_t)(game_base + 0x7a8de0ULL);

    uint8_t raw_state = switch_entry ? *(uint8_t *)(switch_entry + 0x28) : 0xff;
    uint16_t edge = switch_entry ? *(uint16_t *)(switch_entry + 0x26) : 0xffff;
    int logical_state = switch_read ? switch_read(78) : -1;
    int gate2 = mode_gate ? mode_gate(2) : -1;
    int gate3 = mode_gate ? mode_gate(3) : -1;
    int gate4 = mode_gate ? mode_gate(4) : -1;

    uint16_t mode_flags = 0xffff;
    uintptr_t mode_flags_ptr = 0;
    uintptr_t mode_flags_pp = game_base + 0x4353468ULL;
    memcpy(&mode_flags_ptr, (const void *)mode_flags_pp, sizeof(mode_flags_ptr));
    if (mode_flags_ptr) {
        memcpy(&mode_flags, (const void *)mode_flags_ptr, sizeof(mode_flags));
    }

    uint16_t start_arg = 0xffff;
    uintptr_t player_outer = 0;
    uintptr_t player_inner = 0;
    uintptr_t player_outer_p = game_base + 0x43500c0ULL;
    memcpy(&player_outer, (const void *)player_outer_p, sizeof(player_outer));
    if (player_outer) {
        memcpy(&player_inner, (const void *)player_outer, sizeof(player_inner));
        if (player_inner) {
            memcpy(&start_arg, (const void *)(player_inner + 0x108ULL),
                   sizeof(start_arg));
        }
    }

    SHIM_LOG("[start_probe] %s raw_state=%u logical=%d edge=%u "
             "gate2=%d gate3=%d gate4=%d mode_flags=0x%04x start_arg=%u",
             phase, raw_state, logical_state, edge,
             gate2, gate3, gate4, mode_flags, start_arg);
}

/* SHIM_MANUAL_PRESS=1 — poll runs/latest/press_request.txt every 200 ms.
 * Requests may be legacy numeric one-shot sw_ids or explicit lines:
 *   down <sw_id>
 *   up <sw_id>
 *   tap <sw_id> [hold_ms]
 * Down/up update the held-switch overlay, so multiple buttons can be held
 * simultaneously and the SPI/nodebus idle synth keeps reporting them until
 * released. Each command also calls translate() immediately so the runtime
 * switch array sees the edge without waiting for the next hardware poll.
 *
 * Env knobs:
 *   SHIM_MANUAL_PRESS=1               — enable
 *   SHIM_MANUAL_PRESS_DELAY_S=N       — wait N s before first poll (default 0)
 *   SHIM_MANUAL_PRESS_MS=N            — press hold ms (default 500) */
static void shim_translate_held_switch_state(uintptr_t game_base,
                                             uint8_t channel_id) {
    typedef int64_t (*translate_fn)(uint8_t channel_id, const uint8_t *buffer);
    translate_fn translate = (translate_fn)(game_base + 0x651610);

    uint8_t buf[8];
    if (channel_id == 0)
        shim_make_channel0_idle_buffer(buf);
    else
        shim_copy_channel_idle_buffer(game_base, channel_id, buf);
    shim_switch_hold_apply(channel_id, buf);
    shim_log_channel_buf("[manual_press] translate held", channel_id, buf);
    translate(channel_id, buf);
}

static int shim_set_switch_held_by_sw_id(uintptr_t game_base, uint8_t *arr,
                                         int sw_id, int held,
                                         const char *action_tag) {
    uint8_t *e = arr + (size_t)sw_id * 0x30u;
    uint16_t recorded = *(uint16_t *)(e + 0x20);
    if (recorded != (uint16_t)sw_id) {
        SHIM_LOG("[manual_press] sw_id=%d not present (recorded=%u); skip",
                 sw_id, recorded);
        return 0;
    }
    uint16_t bit_idx    = *(uint16_t *)(e + 0x22);
    uint8_t  channel_id = *(uint8_t  *)(e + 0x24);
    int byte_off = bit_idx >> 3;
    if (byte_off >= 8) {
        SHIM_LOG("[manual_press] sw_id=%d bit_idx=%u out of buffer range; skip",
                 sw_id, bit_idx);
        return 0;
    }
    uint16_t edge_before = *(uint16_t *)(e + 0x26);
    uint8_t  state_before = *(uint8_t *)(e + 0x28);
    SHIM_LOG("[manual_press] %s sw_id=%d ch=%u byte=%d bit=%u  (BEFORE: state=%u edge=%u)",
             action_tag, sw_id, channel_id, byte_off, (unsigned)(bit_idx & 7),
             state_before, edge_before);

    if (sw_id == 78) {
        shim_log_start_gate_snapshot(game_base, "before", e);
    }

    shim_switch_hold_set(channel_id, bit_idx, held);
    shim_translate_held_switch_state(game_base, channel_id);

    if (sw_id == 78) {
        shim_log_start_gate_snapshot(game_base, held ? "pressed" : "released", e);
    }

    usleep(200000);
    uint16_t edge_after = *(uint16_t *)(e + 0x26);
    uint8_t  state_after = *(uint8_t *)(e + 0x28);
    SHIM_LOG("[manual_press] %s sw_id=%d AFTER: state=%u edge=%u (delta=%d)",
             action_tag, sw_id, state_after, edge_after,
             edge_after - edge_before);
    return 1;
}

static void shim_press_one_sw_id(uintptr_t game_base, uint8_t *arr,
                                 int sw_id, int press_ms) {
    if (press_ms < 20) press_ms = 20;
    if (press_ms > 5000) press_ms = 5000;
    if (!shim_set_switch_held_by_sw_id(game_base, arr, sw_id, 1, "tap/down"))
        return;
    usleep((useconds_t)press_ms * 1000);
    shim_set_switch_held_by_sw_id(game_base, arr, sw_id, 0, "tap/up");
}

static int shim_parse_press_request_line(char *line, char *action,
                                         size_t action_len, int *sw_id,
                                         int *press_ms_override) {
    char *p = line;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (!*p || *p == '#') return 0;

    if ((*p >= '0' && *p <= '9') ||
        ((*p == '-' || *p == '+') && p[1] >= '0' && p[1] <= '9')) {
        *sw_id = atoi(p);
        if (press_ms_override) *press_ms_override = -1;
        snprintf(action, action_len, "tap");
        return 1;
    }

    char op[16] = {0};
    int id = 0;
    int ms = -1;
    if (sscanf(p, "%15s %d %d", op, &id, &ms) < 2) return -1;
    if (strcmp(op, "down") == 0 || strcmp(op, "hold") == 0 ||
        strcmp(op, "press") == 0) {
        snprintf(action, action_len, "down");
    } else if (strcmp(op, "up") == 0 || strcmp(op, "release") == 0) {
        snprintf(action, action_len, "up");
    } else if (strcmp(op, "tap") == 0 || strcmp(op, "pulse") == 0 ||
               strcmp(op, "click") == 0) {
        snprintf(action, action_len, "tap");
    } else {
        return -1;
    }
    *sw_id = id;
    if (press_ms_override) *press_ms_override = ms;
    return 1;
}

static void *shim_manual_press_thread(void *arg) {
    (void)arg;
    const char *v = getenv("SHIM_MANUAL_PRESS");
    if (!v || !v[0] || v[0] == '0') return NULL;

    int delay_s = 0;
    int press_ms = 500;
    if ((v = getenv("SHIM_MANUAL_PRESS_DELAY_S")) != NULL && v[0]) delay_s = atoi(v);
    if ((v = getenv("SHIM_MANUAL_PRESS_MS")) != NULL && v[0]) press_ms = atoi(v);

    const char *req_path = getenv("SHIM_PRESS_REQUEST_PATH");
    if (!req_path || !req_path[0])
        req_path = "_emulation/runs/latest/press_request.txt";

    if (delay_s > 0) sleep((unsigned)delay_s);
    unlink(req_path);

    SHIM_LOG("[manual_press] === START === press_ms=%d req_path=%s",
             press_ms, req_path);

    uintptr_t game_base = 0;
    while (1) {
        usleep(200000);  /* 200 ms poll */
        int fd = open(req_path, O_RDWR);
        if (fd < 0) continue;
        if (flock(fd, LOCK_EX) != 0) {
            close(fd);
            continue;
        }
        char buf[4096] = {0};
        ssize_t r = read(fd, buf, sizeof(buf) - 1);
        ftruncate(fd, 0);
        lseek(fd, 0, SEEK_SET);
        flock(fd, LOCK_UN);
        close(fd);
        if (r <= 0) continue;

        game_base = shim_resolve_game_base();
        if (!game_base) {
            SHIM_LOG("[manual_press] game_base unresolved; drop request '%.64s'", buf);
            continue;
        }

        uint8_t **arr_pp = (uint8_t **)(uintptr_t)(game_base + 0x4480220ULL);
        uint8_t *arr = *arr_pp;
        if (!arr) {
            SHIM_LOG("[manual_press] g_switch_state_array NULL; drop request '%.64s'", buf);
            continue;
        }

        char *save = NULL;
        for (char *line = strtok_r(buf, "\n", &save);
             line;
             line = strtok_r(NULL, "\n", &save)) {
            char action[8] = {0};
            int sw_id = 0;
            int press_ms_override = -1;
            int parsed = shim_parse_press_request_line(line, action,
                                                       sizeof(action), &sw_id,
                                                       &press_ms_override);
            if (parsed == 0) continue;
            if (parsed < 0 || sw_id < 1 || sw_id > 255) {
                SHIM_LOG("[manual_press] bogus request line '%.64s'; skip", line);
                continue;
            }

            if (strcmp(action, "down") == 0)
                shim_set_switch_held_by_sw_id(game_base, arr, sw_id, 1, "down");
            else if (strcmp(action, "up") == 0)
                shim_set_switch_held_by_sw_id(game_base, arr, sw_id, 0, "up");
            else
                shim_press_one_sw_id(game_base, arr, sw_id,
                                     press_ms_override > 0 ?
                                     press_ms_override : press_ms);
        }
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
