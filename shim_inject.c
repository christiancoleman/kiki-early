/*
 * shim_inject.c — active game-memory injection and scheduled probing.
 *
 * Phase 9 of the refactor, and the last carve-out. Owns:
 *   - Pokémon LE 105-switch manifest (transcribed from the manual) +
 *     shim_fire_switch / shim_find_switch helpers that drive it.
 *   - Event-sweep machinery (Step 52): shim_fire_event trampoline to the
 *     game's in-binary fire_event at BN VA 0x9c12a0, cmode-snapshot diff,
 *     and the blind 0..0xce sweep with per-event SIGALRM timeout + SEGV
 *     recovery via siglongjmp.
 *   - The sigjmp_buf + "sweep in progress" globals declared in spi_shim.h
 *     (live here; read by shim_sigsegv_handler in shim_diagnostics.c).
 *   - Six env-gated worker threads — cmode poke, state-flag poke,
 *     vp-patch applier, event-sweep timer, gstreamer_init trampoline,
 *     switch firer.
 *   - The SIGALRM handler used by the sweep to break out of hung event
 *     handlers.
 *   - shim_inject_start() — the single public entry point the main
 *     constructor calls to set up all of the above in one shot.
 *
 * Cross-TU dependencies resolved via spi_shim.h:
 *   - SHIM_LOG macro.
 *   - shim_resolve_game_base() (shim_diagnostics.c) — used by every
 *     trampoline / poke to translate BN VAs to runtime addresses.
 *   - shim_cmode_table[] / shim_cmode_table_count (shim_diagnostics.c)
 *     — used by the cmode snapshot diff and cmode poke.
 *   - shim_patch_spi_video_vtable / shim_patch_get_format_callback
 *     (shim_patches.c) — called from shim_vp_patch_thread.
 *   - shim_event_sweep() exported here and called from the SIGUSR2
 *     handler in shim_diagnostics.c.
 */

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

/* ========== moved from spi_shim.c (286-410): switch manifest ========== */
/* ---- Step 52: Pokémon LE switch manifest ------------------------------
 *
 * Transcribed from the Pokémon LE / Premium manual (500-55AJ-01) switch
 * reference table. Only switches actually present in this machine are
 * listed — the manual allocates ID space with gaps (3, 4, 13, 14, 24, 26,
 * 32, 38, 39, 42, 43, 47, 48, 62, 65, 68, 69, 75, 77, 81-89, 91, 92, 95)
 * that correspond to unused slots. Do not fire those IDs; they have no
 * meaning on this machine.
 *
 * The `id` field is the manual's switch number (1..105). `node_addr` is
 * the node-local address (e.g. "8-SW-17") which maps to the RS485 node
 * bus frame the netbridge builds — if we ever need to emulate the wire
 * protocol more faithfully, that column is the key.
 *
 * `issues` is a free-form comment documenting known problems when firing
 * this switch from the shim (filled in as we discover them).
 * ------------------------------------------------------------------------ */

struct shim_switch_entry {
    int         id;           /* manual ID (1..105, plus C10/C11 as 200+10, 200+11) */
    const char *name;         /* human-readable */
    const char *location;     /* Playfield / Cabinet / CoinDoor */
    const char *type;         /* Leaf / Opto / Rollover / Target / Micro / MagSw / Plumb */
    const char *node_addr;    /* e.g. "8-SW-17" */
    const char *issues;       /* NULL if none known, otherwise terse comment */
};

static const struct shim_switch_entry shim_switch_manifest[] = {
    /* Playfield rollovers / ramps / lanes — Node 8 */
    {  1, "Left Outlane",              "Playfield", "Rollover", "8-SW-17", NULL },
    {  2, "Left Return Lane",          "Playfield", "Rollover", "8-SW-18", NULL },
    {  5, "Right Return Lane",         "Playfield", "Rollover", "8-SW-19", NULL },
    {  6, "Right Outlane",             "Playfield", "Rollover", "8-SW-20", NULL },
    /* Flippers / slingshots */
    {  7, "Left Slingshot",            "Playfield", "Leaf",     "8-SW-30", NULL },
    {  8, "Right Slingshot",           "Playfield", "Leaf",     "8-SW-29", NULL },
    {  9, "Left Flipper Button",       "Cabinet",   "Leaf",     "8-SW-25", NULL },
    { 10, "Right Flipper Button",      "Cabinet",   "Leaf",     "8-SW-24", NULL },
    { 11, "Left Flipper EOS",          "Playfield", "Leaf",     "8-SW-16", NULL },
    { 12, "Right Flipper EOS",         "Playfield", "Leaf",     "8-SW-31", NULL },
    /* Trough — Node 8 extension 8a (opto array) */
    { 15, "Trough 6",                  "Playfield", "Opto",     "8-SW-32", NULL },
    { 16, "Trough 5",                  "Playfield", "Opto",     "8-SW-33", NULL },
    { 17, "Trough 4",                  "Playfield", "Opto",     "8-SW-34", NULL },
    { 18, "Trough 3",                  "Playfield", "Opto",     "8-SW-35", NULL },
    { 19, "Trough 2",                  "Playfield", "Opto",     "8-SW-36", NULL },
    { 20, "Trough 1",                  "Playfield", "Opto",     "8-SW-37", NULL },
    { 21, "Trough Jam",                "Playfield", "Opto",     "8-SW-38", NULL },
    /* Shooter lane + bowl */
    { 22, "Shooter Lane",              "Playfield", "Micro",    "8-SW-28", NULL },
    { 25, "Bowl Opto",                 "Playfield", "Opto",     "8-SW-27", NULL },
    /* Berry targets + left ramp cluster */
    { 27, "Berry Target Left",         "Playfield", "Target",   "8-SW-9",  NULL },
    { 28, "Berry Target Center",       "Playfield", "Target",   "8-SW-10", NULL },
    { 29, "Left Ramp Enter Opto",      "Playfield", "Opto",     "8-SW-11", NULL },
    { 30, "Left Ramp Made 2 Gate",     "Playfield", "Micro",    "8-SW-12", NULL },
    { 31, "Subway Enter",              "Playfield", "Opto",     "8-SW-8",  NULL },
    /* Lower standup target bank */
    { 33, "Lower Left Target-Top",     "Playfield", "Target",   "8-SW-0",  NULL },
    { 34, "Lower Left Target-Mid",     "Playfield", "Target",   "8-SW-1",  NULL },
    { 35, "Lower Left Target-Bot",     "Playfield", "Target",   "8-SW-2",  NULL },
    { 36, "Lower Right Target-Top",    "Playfield", "Target",   "8-SW-3",  NULL },
    { 37, "Lower Right Target-Bot",    "Playfield", "Target",   "8-SW-4",  NULL },
    /* Ejects */
    { 40, "Left Eject Opto",           "Playfield", "Opto",     "8-SW-5",  NULL },
    { 41, "Right Eject",               "Playfield", "Micro",    "9-SW-1",  NULL },
    /* Node 9 — orbits / ramps / arena / BATTLE / Team Rocket */
    { 44, "Left Orbit",                "Playfield", "Rollover", "9-SW-8",  NULL },
    { 45, "Left Ramp Made 1 Opto",     "Playfield", "Opto",     "9-SW-9",  NULL },
    { 46, "Rear Ramp Made",            "Playfield", "Micro",    "9-SW-29", NULL },
    { 49, "Arena Left 10 Pt",          "Playfield", "Leaf",     "9-SW-13", NULL },
    { 50, "Arena Right 10 Pt",         "Playfield", "Leaf",     "9-SW-14", NULL },
    { 51, "(B)ATTLE Target",           "Playfield", "Target",   "9-SW-26", NULL },
    { 52, "B(A)TTLE Target",           "Playfield", "Target",   "9-SW-27", NULL },
    { 53, "BA(T)TLE Target",           "Playfield", "Target",   "9-SW-17", NULL },
    { 54, "BAT(T)LE Target",           "Playfield", "Target",   "9-SW-18", NULL },
    { 55, "BATT(L)E Target",           "Playfield", "Target",   "9-SW-19", NULL },
    { 56, "BATTL(E) Target",           "Playfield", "Target",   "9-SW-20", NULL },
    { 57, "Team Rocket Target Left",   "Playfield", "Target",   "9-SW-21", NULL },
    { 58, "Team Rocket Target Right",  "Playfield", "Target",   "9-SW-22", NULL },
    { 59, "Meowth Hit Opto",           "Playfield", "Opto",     "9-SW-23", NULL },
    { 60, "Meowth Up",                 "Playfield", "MagSw",    "9-SW-24", NULL },
    { 61, "Meowth Down",               "Playfield", "MagSw",    "9-SW-25", NULL },
    { 63, "Top Pop Bumper",            "Playfield", "Leaf",     "9-SW-28", NULL },
    { 64, "Bot Pop Bumper",            "Playfield", "Leaf",     "9-SW-30", NULL },
    { 66, "Top Lane Left",             "Playfield", "Rollover", "9-SW-10", NULL },
    { 67, "Top Lane Right",            "Playfield", "Rollover", "9-SW-11", NULL },
    /* Berry right target + captive ball + spinner + right ramp */
    { 70, "Berry Target Right",        "Playfield", "Target",   "9-SW-0",  NULL },
    { 71, "Captive Ball-Lower",        "Playfield", "Rollover", "9-SW-4",  NULL },
    { 72, "Captive Ball-Upper",        "Playfield", "Rollover", "9-SW-5",  NULL },
    { 73, "Captive Ball-Target",       "Playfield", "Target",   "9-SW-6",  NULL },
    { 74, "Right Ramp Hold",           "Playfield", "Micro",    "9-SW-15", NULL },
    { 76, "Right Ramp Made Opto",      "Playfield", "Opto",     "9-SW-31", NULL },
    { 78, "Spinner Opto",              "Playfield", "Opto",     "9-SW-7",  NULL },
    { 79, "Team Rocket Enter",         "Playfield", "Rollover", "9-SW-2",  NULL },
    { 80, "Right Orbit",               "Playfield", "Rollover", "9-SW-3",  NULL },
    /* Cabinet / coin door — Node 1 */
    { 90, "Action Button",             "Cabinet",   "Leaf",     "1-SW-2",  NULL },
    { 93, "Start Button",              "Cabinet",   "Micro",    "1-SW-11", NULL },
    { 94, "Tournament Start Button",   "Cabinet",   "Micro",    "1-SW-12", NULL },
    { 96, "Left Coin",                 "CoinDoor",  "Micro",    "1-SW-16", NULL },
    { 97, "Right Coin",                "CoinDoor",  "Micro",    "1-SW-18", NULL },
    { 98, "Center Coin",               "CoinDoor",  "Micro",    "1-SW-17", NULL },
    { 99, "Fourth Coin",               "CoinDoor",  "Micro",    "1-SW-19", NULL },
    {100, "Fifth Coin",                "CoinDoor",  "Micro",    "1-SW-20", NULL },
    {101, "Tilt Pendulum",             "Cabinet",   "Plumb",    "1-SW-14", NULL },
    {102, "Sixth Coin",                "Cabinet",   "Micro",    "1-SW-21", NULL },
    {103, "Ticket Notch",              "Cabinet",   "Micro",    "1-SW-8",  NULL },
    {104, "Slam Tilt",                 "Cabinet",   "Micro",    "1-SW-22", NULL },
    {105, "Ticket Low",                "Cabinet",   "Micro",    "1-SW-7",  NULL },
    /* Coin-door service switches (ID prefixed with "C" in the manual) */
    {210, "Service Plus",              "CoinDoor",  "Micro",    "0-SW-9",  NULL },
    /* Note: the manual's C11 entry is truncated in the source we have;
     * when we have the full row, add {211, "Service", ...}. */
};

/* Lookup helper. Returns NULL if switch id isn't in the manifest. */
static const struct shim_switch_entry *shim_find_switch(int id) {
    size_t N = sizeof(shim_switch_manifest) / sizeof(shim_switch_manifest[0]);
    for (size_t i = 0; i < N; i++) {
        if (shim_switch_manifest[i].id == id) return &shim_switch_manifest[i];
    }
    return NULL;
}

/* ========== moved from spi_shim.c (412-678): event-sweep machinery ========== */
/* ---- Step 52: fire_event trampoline + event sweep ---------------------
 *
 * BN Step 52 revealed:
 *   fire_event @ 0x9c12a0: uint64_t(int32_t event_id, int64_t event_data)
 *     - bounds-checks event_id <= 0xce
 *     - walks linked list at data_485ea80[event_id*8]
 *     - each node: {callback_fn_ptr @ +0, priority @ +0x08, next @ +0x10}
 *     - calls callback(event_data); short-circuits on !(ret & 1)
 *
 * Trampoline pattern:
 *   fire_event_addr = game_base + 0x5c12a0
 *
 * Event sweep fires every event_id 0..0xce with event_data=0 and diffs
 * cmode state before/after. Events whose handlers dereference event_data
 * as a pointer will SEGV with fault_addr=0; we catch and recover.
 *
 * Triggered by SIGUSR2 (kept out of the main dump path so we can still
 * use SIGUSR1 for state snapshots without firing events).
 * ------------------------------------------------------------------------ */

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

/* ---- Step 52: switch-closure API -------------------------------------
 *
 * shim_fire_switch(switch_id) simulates a physical switch closure at the
 * game's in-memory switch state array (data_4880220, per PROTOCOL.md).
 *
 * Current behaviour (Plan-C pending):
 *   1. Look up the switch in the manifest; refuse if not listed (manual
 *      enumerates all real switches; unlisted IDs are unused slots).
 *   2. Snapshot cmode state (+0x308/+0x60 across 42 cmodes).
 *   3. Write 1 to data_4880220 + id * 0x30 + 0x28.
 *   4. Re-snapshot; log deltas with switch name.
 *   5. After a short dwell, write 0 back (simulating release).
 *
 * What's missing until Plan C returns: the "switch decoder" — the
 * function that consumes UART cmd 0x27 bytes, writes into data_4880220,
 * AND calls fire_event(derived_event_id, struct_ptr). Without it, the
 * game sees the state byte flip but no event fires, so it may not react
 * until its own poll cycle notices. Once we find the decoder we can
 * either call it from shim_fire_switch or fire the right event ourselves.
 * ------------------------------------------------------------------------ */

#define SHIM_SWITCH_STATE_BASE 0x4480220   /* stride 0x30, state @ +0x28 */
#define SHIM_SWITCH_COUNT_ADDR 0x44cbaa0   /* dynamic switch count */

static int shim_fire_switch(int switch_id) {
    const struct shim_switch_entry *sw = shim_find_switch(switch_id);
    if (!sw) {
        char msg[128];
        int mn = snprintf(msg, sizeof(msg),
            "\n--- shim_fire_switch: id %d not in manifest (unused slot?) ---\n",
            switch_id);
        ssize_t u = write(STDERR_FILENO, msg, (size_t)mn); (void)u;
        return -1;
    }

    uintptr_t game_base = shim_resolve_game_base();
    if (!game_base) {
        const char *m = "\n--- shim_fire_switch: game_base unresolved ---\n";
        ssize_t u = write(STDERR_FILENO, m, strlen(m)); (void)u;
        return -1;
    }

    /* Bounds-check against the game's runtime switch count. */
    uint32_t sw_count = 0;
    memcpy(&sw_count,
           (void*)(game_base + SHIM_SWITCH_COUNT_ADDR),
           4);
    if (sw_count > 0 && (uint32_t)switch_id >= sw_count) {
        char msg[192];
        int mn = snprintf(msg, sizeof(msg),
            "\n--- shim_fire_switch: id %d (%s) out of range — runtime "
            "count is %u ---\n",
            switch_id, sw->name, sw_count);
        ssize_t u = write(STDERR_FILENO, msg, (size_t)mn); (void)u;
        return -1;
    }

    uintptr_t state_addr = game_base +
        SHIM_SWITCH_STATE_BASE + (uintptr_t)switch_id * 0x30 + 0x28;

    char hdr[256];
    int hn = snprintf(hdr, sizeof(hdr),
        "\n--- shim_fire_switch: %d '%s' (%s, %s, %s) state@0x%lx ---\n",
        switch_id, sw->name, sw->location, sw->type, sw->node_addr,
        (unsigned long)state_addr);
    ssize_t u = write(STDERR_FILENO, hdr, (size_t)hn); (void)u;

    uint64_t snap_before[42 * 2];
    uint64_t snap_after[42 * 2];
    shim_capture_cmode_snap(game_base, snap_before);

    uint8_t before_state = 0;
    memcpy(&before_state, (void*)state_addr, 1);

    /* Close the switch. */
    uint8_t closed = 1;
    memcpy((void*)state_addr, &closed, 1);

    /* Give the game a tick to notice. In practice the game's switch
     * poll runs off netbridge cmd 0x27 responses — so the shim-side
     * flip may go unnoticed until Plan C's translator hook is added.
     * We still sleep briefly in case any polling thread samples the
     * state array directly. */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 20 * 1000 * 1000 }; /* 20 ms */
    nanosleep(&ts, NULL);

    shim_capture_cmode_snap(game_base, snap_after);

    /* Release the switch. */
    memcpy((void*)state_addr, &before_state, 1);

    /* Report deltas. */
    size_t N = shim_cmode_table_count;
    char delta_buf[1800];
    size_t delta_off = 0;
    int delta_count = 0;
    for (size_t i = 0; i < N; i++) {
        if (snap_after[i*2 + 0] != snap_before[i*2 + 0] ||
            snap_after[i*2 + 1] != snap_before[i*2 + 1]) {
            delta_count++;
            if (delta_off < sizeof(delta_buf) - 128) {
                int added = snprintf(
                    delta_buf + delta_off, sizeof(delta_buf) - delta_off,
                    "  %s(+0x308:%lx->%lx)",
                    shim_cmode_table[i].name_hint,
                    (unsigned long)snap_before[i*2 + 0],
                    (unsigned long)snap_after[i*2 + 0]);
                delta_off += (size_t)added;
            }
        }
    }

    char tail[2048];
    int tn = snprintf(tail, sizeof(tail),
        "  switch %d '%s': before_state=%u Δ=%d%s\n",
        switch_id, sw->name, before_state, delta_count,
        delta_count > 0 ? delta_buf : " (no cmode state change — translator "
                                      "not yet hooked, see PROTOCOL.md)");
    u = write(STDERR_FILENO, tail, (size_t)tn); (void)u;

    return delta_count;
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
            /* setjmp_result == 1 — came from SEGV */
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
                /* Use post as new baseline for cumulative tracking */
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

/* ========== moved from spi_shim.c (683-817): poke threads ========== */
/* SPIKE3_POKE_CMODE=<name>:<hex_value>:<delay_sec> — force a cmode's +0x308
 * field to <hex_value> after <delay_sec> seconds, then reset to 0 after
 * another 3 s. Lets us simulate mode activations without needing the
 * game's Stage-2 event logic to agree. Name matches shim_cmode_table.name_hint
 * (e.g. "story_charmander_1", "battle_coast"). */
static void *shim_poke_cmode_thread(void *arg) {
    char *spec = (char*)arg;
    if (!spec) return NULL;

    char *colon1 = strchr(spec, ':');
    if (!colon1) { free(spec); return NULL; }
    *colon1 = 0;
    char *name = spec;
    char *rest = colon1 + 1;
    char *colon2 = strchr(rest, ':');
    uint64_t value = 0;
    long delay = 35;
    if (colon2) {
        *colon2 = 0;
        value = strtoull(rest, NULL, 0);
        delay = atol(colon2 + 1);
    } else {
        value = strtoull(rest, NULL, 0);
    }

    /* Look up cmode. */
    size_t N = shim_cmode_table_count;
    const struct shim_cmode_entry *entry = NULL;
    for (size_t i = 0; i < N; i++) {
        if (strcmp(shim_cmode_table[i].name_hint, name) == 0) {
            entry = &shim_cmode_table[i];
            break;
        }
    }
    if (!entry) {
        SHIM_LOG("[poke_cmode] unknown cmode name: %s", name);
        free(spec);
        return NULL;
    }

    SHIM_LOG("[poke_cmode] will poke %s +0x308 = 0x%lx after %ld s",
             name, (unsigned long)value, delay);

    sleep((unsigned int)delay);

    uintptr_t game_base = shim_resolve_game_base();
    if (!game_base) {
        SHIM_LOG("[poke_cmode] game_base unresolved; aborting");
        free(spec);
        return NULL;
    }

    uintptr_t addr = game_base + entry->va + 0x308;
    SHIM_LOG("[poke_cmode] writing 0x%lx to %s @ rt 0x%lx (activation)",
             (unsigned long)value, name, (unsigned long)addr);
    memcpy((void*)addr, &value, 8);

    /* Hold for 3 seconds so the bridge has time to emit + subscriber reacts */
    sleep(3);

    uint64_t zero = 0;
    SHIM_LOG("[poke_cmode] writing 0 to %s @ rt 0x%lx (deactivation)",
             name, (unsigned long)addr);
    memcpy((void*)addr, &zero, 8);

    free(spec);
    return NULL;
}

/* Step 60: direct poke of system_state_flags bit 0x10.
 *
 * BN entry [85] established that the main loop at 0xb2aba0 gates downstream
 * progression on `system_state_flags bit 0x10` being set AND elapsed time in
 * [599 ms, 3599 ms]. The two legitimate writers (sub_a61380 / sub_a61040)
 * only set the bit on fire_event(0x46|0x4d) success, which means firing those
 * events via our shim_fire_event trampoline won't set the bit — only the game
 * internally calling sub_a61380 can do that.
 *
 * This thread brute-forces the experiment: after a configurable delay into
 * the timing window, atomically OR the bit in directly. Three outcomes per
 * the journal Step-60 plan:
 *   - boot-splash advances  → bit was the gate, find the legitimate trigger
 *   - no change             → bit is part of conjunction with other state
 *   - crash                 → downstream code needs more setup than just the bit
 *
 * Spec: SPIKE3_POKE_STATE_FLAG=<hex_or_dec_bit_mask>:<delay_ms>
 *   default mask  = 0x10
 *   default delay = 1500 ms (solidly inside the [599, 3599] ms window).
 * Example: SPIKE3_POKE_STATE_FLAG=0x10:1500 */
#define SYSTEM_STATE_FLAGS_VA 0x4482388

static void *shim_poke_state_flag_thread(void *arg) {
    char *spec = (char*)arg;
    unsigned mask = 0x10;
    long delay_ms = 1500;
    if (spec && *spec) {
        char *copy = spec;
        char *colon = strchr(copy, ':');
        if (colon) {
            *colon = 0;
            if (copy[0]) mask = (unsigned)strtoul(copy, NULL, 0);
            delay_ms = atol(colon + 1);
        } else {
            /* bare value = mask only, keep default delay */
            mask = (unsigned)strtoul(copy, NULL, 0);
        }
    }
    SHIM_LOG("[poke_state_flag] will OR 0x%x into system_state_flags @ 0x%x after %ld ms",
             mask, SYSTEM_STATE_FLAGS_VA, delay_ms);

    if (delay_ms > 0) {
        struct timespec ts = {
            .tv_sec  = delay_ms / 1000,
            .tv_nsec = (delay_ms % 1000) * 1000000L,
        };
        nanosleep(&ts, NULL);
    }

    uintptr_t game_base = shim_resolve_game_base();
    if (!game_base) {
        SHIM_LOG("[poke_state_flag] game_base unresolved; aborting");
        free(spec);
        return NULL;
    }
    volatile uint16_t *flag_ptr =
        (volatile uint16_t *)(game_base + SYSTEM_STATE_FLAGS_VA);
    uint16_t before = *flag_ptr;
    __atomic_or_fetch(flag_ptr, (uint16_t)mask, __ATOMIC_SEQ_CST);
    uint16_t after = *flag_ptr;
    SHIM_LOG("[poke_state_flag] @ rt %p: 0x%04x → 0x%04x (mask 0x%x applied)",
             (void*)flag_ptr, before, after, mask);

    free(spec);
    return NULL;
}

/* ========== moved from spi_shim.c (823-831): vp_patch thread ========== */
static void *shim_vp_patch_thread(void *arg) {
    (void)arg;
    /* Wait for game's C++ static initializers to complete before patching.
     * 2 seconds is generous; could shrink later once we know it's stable. */
    sleep(2);
    shim_patch_spi_video_vtable();
    shim_patch_get_format_callback();
    shim_patch_audio_driver_vtable();
    return NULL;
}

/* ========== moved from spi_shim.c (836-977): alarm handler + timer threads ========== */
/* Step 52: SIGALRM handler used to break out of hung event handlers during
 * the sweep. Installed in the constructor; blocked on all threads except
 * the sweep timer thread so alarm() delivers to the right place. */
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
    /* else: unexpected SIGALRM outside the sweep — just ignore */
}

/* Step 52: SIGUSR2 gets intercepted by QEMU user-mode itself, so we can't
 * rely on external signals to trigger action from outside. Instead we use
 * env-var-driven timer threads:
 *
 *   SPIKE3_SWEEP_AFTER=<sec>      → run shim_event_sweep() after N seconds
 *                                    (requires SPIKE3_SWEEP_EVENTS=1 opt-in)
 *   SPIKE3_FIRE_SWITCH=<id[,id]>  → run shim_fire_switch() on each id after
 *                                    <SPIKE3_SWITCH_DELAY or 30> seconds
 *   SPIKE3_SWITCH_DELAY=<sec>     → delay before fire_switch calls (default 30)
 */
static void *shim_sweep_timer_thread(void *arg) {
    long seconds = (long)arg;
    if (seconds <= 0) return NULL;

    /* Unblock SIGALRM on this thread only — the constructor blocked it on
     * the main thread (so all threads created by the game inherit that
     * mask), leaving this thread as the sole recipient of alarm() signals.
     * Keeps siglongjmp-on-timeout well-defined (same thread as sigsetjmp
     * in shim_event_sweep). */
    sigset_t unblock;
    sigemptyset(&unblock);
    sigaddset(&unblock, SIGALRM);
    pthread_sigmask(SIG_UNBLOCK, &unblock, NULL);

    char msg[128];
    int mn = snprintf(msg, sizeof(msg),
        "\n=== shim_sweep_timer: sleeping %ld seconds then firing sweep ===\n",
        seconds);
    ssize_t u = write(STDERR_FILENO, msg, (size_t)mn); (void)u;

    sleep((unsigned int)seconds);

    const char *m = "\n=== shim_sweep_timer: firing sweep NOW ===\n";
    u = write(STDERR_FILENO, m, strlen(m)); (void)u;

    shim_event_sweep();
    return NULL;
}

/* SPIKE3_FORCE_GSTREAMER=<seconds> — after N seconds, trampoline into the
 * game's gstreamer_init function at BN VA 0xcc1ec0. Diagnostic: tells us
 * whether gstreamer is reachable + functional when explicitly called.
 * If our gst_init_check hook fires afterwards, gstreamer is callable and
 * the dormancy we observed is purely "the game never chooses to call it
 * under current conditions" (hardware-gated, config-gated, etc.). */
static void *shim_force_gstreamer_thread(void *arg) {
    long seconds = (long)arg;
    if (seconds <= 0) return NULL;

    char msg[160];
    int mn = snprintf(msg, sizeof(msg),
        "\n=== shim_force_gstreamer: sleeping %ld s then calling game's gstreamer_init @ 0xcc1ec0 ===\n",
        seconds);
    ssize_t u = write(STDERR_FILENO, msg, (size_t)mn); (void)u;

    sleep((unsigned int)seconds);

    uintptr_t game_base = shim_resolve_game_base();
    if (!game_base) {
        const char *m = "[shim] force_gstreamer: game_base unresolved; aborting\n";
        u = write(STDERR_FILENO, m, strlen(m)); (void)u;
        return NULL;
    }

    typedef void (*gstreamer_init_t)(void);
    uintptr_t addr = game_base + 0x8c1ec0;
    gstreamer_init_t gstreamer_init_fn = (gstreamer_init_t)addr;

    char hdr[200];
    int hn = snprintf(hdr, sizeof(hdr),
        "[shim] force_gstreamer: calling gstreamer_init via trampoline @ %p  (game_base=0x%lx)\n",
        (void*)addr, game_base);
    u = write(STDERR_FILENO, hdr, (size_t)hn); (void)u;

    gstreamer_init_fn();

    const char *done = "[shim] force_gstreamer: gstreamer_init returned\n";
    u = write(STDERR_FILENO, done, strlen(done)); (void)u;
    return NULL;
}

/* SPIKE3_FIRE_SWITCH=<id1:id2:...> — fire one or more switches after
 * SPIKE3_SWITCH_DELAY seconds (default 30).
 *
 * NOTE: colon-delimited, not comma — QEMU's `-E var=value` uses comma as
 * the var separator (discovered via Step 46 V4L2 probe). If you invoke
 * the shim directly (not via QEMU), commas also work.
 *
 * Each ID is spaced by ~200 ms to let the game's poll cycle breathe. */
static void *shim_fire_switch_thread(void *arg) {
    char *list_copy = (char*)arg;
    if (!list_copy || !list_copy[0]) { free(list_copy); return NULL; }

    const char *delay_s = getenv("SPIKE3_SWITCH_DELAY");
    long delay = delay_s && delay_s[0] ? atol(delay_s) : 30;
    if (delay < 0) delay = 0;
    if (delay > 3600) delay = 3600;

    char msg[200];
    int mn = snprintf(msg, sizeof(msg),
        "\n=== shim_fire_switch_thread: sleeping %ld s then firing switches [%s] ===\n",
        delay, list_copy);
    ssize_t u = write(STDERR_FILENO, msg, (size_t)mn); (void)u;

    sleep((unsigned int)delay);

    /* Parse colon-or-comma-separated IDs (":," covers both for safety). */
    char *save = NULL;
    char *tok = strtok_r(list_copy, ":,", &save);
    while (tok) {
        while (*tok == ' ') tok++;
        int id = atoi(tok);
        if (id > 0) {
            shim_fire_switch(id);
            /* short gap between fires so the game's poll can observe
             * one state transition at a time */
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 200*1000*1000 };
            nanosleep(&ts, NULL);
        }
        tok = strtok_r(NULL, ":,", &save);
    }

    free(list_copy);
    return NULL;
}

/* ---- Public entry point called from shim_install_crash_handler ---- */

/* Registers SIGALRM handler, blocks SIGALRM on the main thread (so threads
 * spawned below inherit the block, and only the sweep-timer thread unblocks
 * SIGALRM for itself), then spawns each env-gated worker thread. All six
 * env vars are independent — any subset can be set on a given run. */
void shim_inject_start(void) {
    /* SIGALRM handler (used by shim_event_sweep alarm-per-event timeout). */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = shim_event_sweep_alarm_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);

    /* Block SIGALRM on the main (current) thread. Threads created by the
     * game after this point inherit the mask, so SIGALRM won't disturb
     * anything other than the sweep-timer thread (which unblocks SIGALRM
     * on itself before calling shim_event_sweep). */
    sigset_t block_alarm;
    sigemptyset(&block_alarm);
    sigaddset(&block_alarm, SIGALRM);
    pthread_sigmask(SIG_BLOCK, &block_alarm, NULL);

    /* SPIKE3_SWEEP_EVENTS=1 + SPIKE3_SWEEP_AFTER=<sec> — run shim_event_sweep
     * after N seconds. Opt-in; blind sweeps are low-signal (see
     * EVENT_SWEEP_STEP52.md). */
    const char *sweep_enabled = getenv("SPIKE3_SWEEP_EVENTS");
    if (sweep_enabled && sweep_enabled[0] == '1') {
        const char *s = getenv("SPIKE3_SWEEP_AFTER");
        if (s && s[0]) {
            long sec = atol(s);
            if (sec > 0 && sec < 3600) {
                pthread_t t;
                if (pthread_create(&t, NULL, shim_sweep_timer_thread,
                                   (void*)(intptr_t)sec) == 0) {
                    pthread_detach(t);
                }
            }
        }
    }

    /* SPIKE3_FIRE_SWITCH=<id1[:id2:...]> — fire one or more switches. */
    const char *fs = getenv("SPIKE3_FIRE_SWITCH");
    if (fs && fs[0]) {
        char *copy = strdup(fs);
        if (copy) {
            pthread_t t;
            if (pthread_create(&t, NULL, shim_fire_switch_thread, copy) == 0) {
                pthread_detach(t);
            } else {
                free(copy);
            }
        }
    }

    /* SPIKE3_FORCE_GSTREAMER=<sec> — trampoline into the game's
     * gstreamer_init after N seconds (diagnostic). */
    const char *fg = getenv("SPIKE3_FORCE_GSTREAMER");
    if (fg && fg[0]) {
        long sec = atol(fg);
        if (sec > 0 && sec < 3600) {
            pthread_t t;
            if (pthread_create(&t, NULL, shim_force_gstreamer_thread,
                               (void*)(intptr_t)sec) == 0) {
                pthread_detach(t);
            }
        }
    }

    /* SPIKE3_POKE_CMODE=<name>:<hex>:<delay_s> — force a cmode field. */
    const char *pc = getenv("SPIKE3_POKE_CMODE");
    if (pc && pc[0]) {
        char *copy = strdup(pc);
        if (copy) {
            pthread_t t;
            if (pthread_create(&t, NULL, shim_poke_cmode_thread, copy) == 0) {
                pthread_detach(t);
            } else {
                free(copy);
            }
        }
    }

    /* SPIKE3_POKE_STATE_FLAG=<mask>:<delay_ms> — OR bits into
     * system_state_flags. Accepts bare "1" as "use defaults". */
    const char *psf = getenv("SPIKE3_POKE_STATE_FLAG");
    if (psf && psf[0]) {
        char *copy = strdup(psf[0] == '1' && psf[1] == 0 ? "" : psf);
        if (copy) {
            pthread_t t;
            if (pthread_create(&t, NULL, shim_poke_state_flag_thread, copy) == 0) {
                pthread_detach(t);
            } else {
                free(copy);
            }
        }
    }

    /* SPIKE3_PATCH_SPI_VIDEO=1 — apply the Step 62 SpiDecoder::Play vtable
     * patch (and Step 64 get_format NOP) after a 2-second settle. The
     * patch functions themselves live in shim_patches.c. */
    {
        const char *pv = getenv("SPIKE3_PATCH_SPI_VIDEO");
        if (pv && pv[0] == '1') {
            pthread_t t;
            if (pthread_create(&t, NULL, shim_vp_patch_thread, NULL) == 0)
                pthread_detach(t);
        }
    }
}
