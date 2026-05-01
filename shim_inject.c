/*
 * shim_inject.c — active game-memory injection and scheduled probing.
 *
 * Phase 9 of the refactor, and the last carve-out. Owns:
 *   - Pokémon LE 105-switch manifest (transcribed from the manual) +
 *     shim_fire_switch / shim_find_switch helpers that drive it.
 *   - Event-sweep machinery (Step 52): shim_fire_event trampoline to the
 *     game's in-binary fire_event at BN VA 0x5c12a0, cmode-snapshot diff,
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
#include "shim_discovery.h"

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
 *   fire_event @ 0x5c12a0: uint64_t(int32_t event_id, int64_t event_data)
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

/* ---- Step 72: switch-93 BRK-trap arm helper ----
 *
 * Publishes channel/payload/deadline atomically for the SIGTRAP handler in
 * spi_shim.c. The handler reads these volatiles when sub_8edfe0 traps; if
 * arg1 (channel) matches `channel` and now < deadline, the handler writes
 * the payload into the caller's 8-byte buffer instead of synthesizing zeros.
 *
 * The deadline auto-expires; no disarm is needed. Setting hold_ms=0 disarms
 * immediately (channel is also reset to -1).
 *
 * NOTE (Step 72c): The wire-arm (sub_8edfe0 BRK) only fires if the upstream
 * poll thread (sub_675df0 → sub_674b90 → sub_8edfe0) is alive. In the
 * current dormant-orchestrator state, that path is inactive and the arm
 * is a no-op. Step 72c added a parallel "direct call" path that calls
 * sub_651610(channel, payload) from the shim thread; that bypasses the
 * dormant poll entirely and is the actual injection vehicle until/unless
 * the orchestrator is unblocked.
 */
void shim_sub_8edfe0_arm(int channel, const uint8_t payload[8], uint32_t hold_ms) {
    if (hold_ms == 0) {
        shim_sub_8edfe0_arm_channel = -1;
        shim_sub_8edfe0_arm_deadline_ms = 0;
        __sync_synchronize();
        return;
    }
    /* Write payload + channel BEFORE deadline so the handler sees a
     * consistent (channel, payload, deadline) tuple — we ratchet the
     * deadline last as the publication barrier. */
    for (int i = 0; i < 8; i++) shim_sub_8edfe0_payload[i] = payload[i];
    shim_sub_8edfe0_arm_channel = channel;
    __sync_synchronize();

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now_ms = (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
    shim_sub_8edfe0_arm_deadline_ms = now_ms + hold_ms;
    __sync_synchronize();

    SHIM_LOG("[sw_brk] arm: channel=%d payload=%02x%02x%02x%02x%02x%02x%02x%02x hold_ms=%u",
             channel,
             payload[0], payload[1], payload[2], payload[3],
             payload[4], payload[5], payload[6], payload[7],
             hold_ms);
}

/* Per-switch wire-side info for switches whose channel/bit positions have
 * been confirmed via BN. Switches NOT in this table fall back to the
 * legacy state-byte flip (which is acknowledged half-hooked but keeps
 * the existing diagnostic working).
 *
 * `channel`        — value passed as arg1 to sub_8edfe0 (the inbound poll
 *                    channel id; e.g. 6 for Node 1 cabinet inputs).
 * `byte_index`     — index 0..7 into the 8-byte channel buffer.
 * `bit_a_mask`     — bit-A "wire 1" mask (the rising-edge bit per
 *                    sub_7b4d00's `+0xa` decode).
 * `bit_b_mask`     — paired sentinel bit per `+0xb` decode; per BN trace
 *                    it sets *(entry+0x23)=1 and selects state 2 vs 4.
 *                    We set both for the safest closed-paired pattern.
 */
struct shim_switch_wire {
    int      switch_id;
    int      channel;
    int      byte_index;
    uint8_t  bit_a_mask;
    uint8_t  bit_b_mask;
};

static const struct shim_switch_wire shim_switch_wire_map[] = {
    /* Switch 93 "Start Button" (1-SW-11): descriptor at 0x43e7cf0
     * (BN session 2026-04-30). Field 0x43e7d18 = 06 00 0b 00 01 00 01 00
     * → channel=6, wire-bit=11. sub_7b4d00 decode: bit-A = 11+0xa = 21
     * = byte 2 bit 5 (0x20); bit-B = 11+0xb = 22 = byte 2 bit 6 (0x40).
     * Combined byte 2 = 0x60 for the closed-paired-sentinel pattern. */
    { 93, 6, 2, 0x20, 0x40 },
};

static const struct shim_switch_wire *shim_find_switch_wire(int switch_id) {
    size_t N = sizeof(shim_switch_wire_map) / sizeof(shim_switch_wire_map[0]);
    for (size_t i = 0; i < N; i++) {
        if (shim_switch_wire_map[i].switch_id == switch_id)
            return &shim_switch_wire_map[i];
    }
    return NULL;
}

/* ---- Step 72c: direct call to sub_651610 (Recipe B) ----
 *
 * Bypasses the dormant orchestrator/poll-thread chain by calling the
 * translator directly from the shim thread. sub_651610(channel, buf8)
 * locks pthread_mutex_lock(0x4480240), XOR-diffs the buffer against an
 * old per-channel snapshot, and pushes any changed switch onto the
 * pending list at data_4480278 — where the drainer sub_6511c0 picks it
 * up and fires the event.
 *
 * Why this works when the BRK trap doesn't: the upstream poll path
 * (sub_675df0 → sub_674b90 → sub_8edfe0 → sub_7b4d00 → sub_651610) is
 * dormant because the orchestrator gate (fire_event(0x80,0) returning
 * truthy) hasn't fired. We jump straight to sub_651610 from outside,
 * skipping the gate entirely.
 *
 * Caveats (per BN trace 2026-04-30):
 *   - sub_651610 needs the pthread_mutex at 0x4480240 to be initialized.
 *     C++ static init runs before any user code, so it should be live by
 *     the time fire_switch fires (30 s after start).
 *   - sub_651610 reads g_switch_state_array (0x4480220) to compute diffs.
 *     If init (sub_650f00) hasn't run yet, the array may be partially
 *     uninitialized. We accept that risk — first-run diff at start-of-day
 *     just produces a "switch X opened" event for whichever bits look
 *     non-zero in the uninitialized array.
 *   - Drainer sub_6511c0 must be running on its own thread for the
 *     pushed event to actually fire. Per BN, it is — independent of the
 *     poll path that we know is dormant.
 *
 * Signature inferred from BN: void sub_651610(uint8_t channel, uint8_t *buf8).
 * Return type unused — we don't read it.
 */
typedef void (*shim_sub_651610_fn_t)(uint8_t /*channel*/, uint8_t * /*buf8*/);

#define SHIM_SUB_651610_VA  0x651610

static int shim_direct_call_sub_651610(int channel, const uint8_t payload[8]) {
    uintptr_t game_base = shim_resolve_game_base();
    if (!game_base) {
        SHIM_LOG("[sw_brk] direct sub_651610: game_base unresolved; skip");
        return -1;
    }
    /* Local mutable copy — sub_651610 may read its arg2 from registers
     * differently than from a const string in .rodata. Stack buffer is
     * the safest. */
    uint8_t buf[8];
    for (int i = 0; i < 8; i++) buf[i] = payload[i];

    shim_sub_651610_fn_t fn = (shim_sub_651610_fn_t)(game_base + SHIM_SUB_651610_VA);
    SHIM_LOG("[sw_brk] direct sub_651610(ch=%d, [%02x %02x %02x %02x %02x %02x %02x %02x]) @ %p",
             channel,
             buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7],
             (void *)fn);
    fn((uint8_t)channel, buf);
    SHIM_LOG("[sw_brk] direct sub_651610 returned");
    return 0;
}

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

    /* Wire-side info: when present, drive the BRK trap on sub_8edfe0
     * (channel buffer write) instead of the post-translator state byte.
     * The wire-side path actually exercises sub_651610's diff-and-enqueue
     * into the pending list — which is what the byte-flip approach
     * skipped. */
    const struct shim_switch_wire *wire = shim_find_switch_wire(switch_id);

    uintptr_t state_addr = game_base +
        SHIM_SWITCH_STATE_BASE + (uintptr_t)switch_id * 0x30 + 0x28;

    char hdr[256];
    int hn = snprintf(hdr, sizeof(hdr),
        "\n--- shim_fire_switch: %d '%s' (%s, %s, %s) state@0x%lx mode=%s ---\n",
        switch_id, sw->name, sw->location, sw->type, sw->node_addr,
        (unsigned long)state_addr,
        wire ? "wire-arm" : "state-flip");
    ssize_t u = write(STDERR_FILENO, hdr, (size_t)hn); (void)u;

    uint64_t snap_before[42 * 2];
    uint64_t snap_after[42 * 2];
    shim_capture_cmode_snap(game_base, snap_before);

    uint8_t before_state = 0;
    memcpy(&before_state, (void*)state_addr, 1);

    if (wire) {
        /* Step 72d — plant the switch_id in `data_44804b8`'s per-channel
         * mapping slot. Per BN decompilation of sub_651610 (Step 72c),
         * for each (channel, bit) pair sub_651610 reads the switch_id
         * from `data_44804b8 + channel*0xb0 + 0x28 + abs_bit*2` and
         * pushes the per-switch state struct (g_switch_state_array +
         * switch_id*0x30) onto the pending list when the bit changes
         * AND the slot is non-zero.
         *
         * The orchestrator-driven init (sub_7b4638) normally populates
         * this slot. Since it's dormant, the slot reads 0 → sub_651610
         * sees "no switch mapped to this bit" and silently no-ops.
         * Plant the value ourselves to make our sub_651610 call land.
         *
         * Read = 4 bytes (zx.d in BN's IL), so we write u32. Adjacent
         * bits' u32 reads overlap, but only the bit we plant has a
         * non-zero value — adjacent bits read the SAME u32 with the
         * planted byte at the wrong position, which numerically is some
         * very large value treated as out-of-range switch_id. Worth
         * mentioning: for switch 93 channel 6 bit 21, slot offset is
         * 0x44808d8 + 0x28 + 42 = 0x4480902 + 0x28 = 0x448092a.
         */
        int abs_bit_a = wire->byte_index * 8 + __builtin_ctz(wire->bit_a_mask);
        uintptr_t cbd_slot_addr = game_base + 0x44804b8
                                + (uintptr_t)wire->channel * 0xb0
                                + 0x28
                                + (uintptr_t)abs_bit_a * 2;
        uint32_t cbd_slot_old = 0;
        memcpy(&cbd_slot_old, (void*)cbd_slot_addr, 4);
        uint32_t cbd_slot_new = (uint32_t)switch_id;
        memcpy((void*)cbd_slot_addr, &cbd_slot_new, 4);
        SHIM_LOG("[sw_brk] planted switch_id=%d at data_44804b8+0x%lx (=%p): "
                 "0x%08x -> 0x%08x  (channel=%d, abs_bit=%d)",
                 switch_id,
                 (unsigned long)(cbd_slot_addr - game_base),
                 (void*)cbd_slot_addr,
                 cbd_slot_old, cbd_slot_new,
                 wire->channel, abs_bit_a);

        /* Step 72c diagnostic dump. */
        {
            uint32_t cbd_count = 0;
            uintptr_t pend_head = 0;
            uint8_t   gate_44821b8 = 0;
            uint32_t  gate_4482120 = 0;
            memcpy(&cbd_count,    (void*)(game_base + 0x44cbda8), 4);
            memcpy(&pend_head,    (void*)(game_base + 0x4480278), 8);
            memcpy(&gate_44821b8, (void*)(game_base + 0x44821b8), 1);
            memcpy(&gate_4482120, (void*)(game_base + 0x4482120), 4);
            SHIM_LOG("[sw_brk] pre-fire: data_44cbda8=%u  pending_head=%p  "
                     "data_44821b8=0x%02x  data_4482120=0x%08x",
                     cbd_count, (void*)pend_head,
                     gate_44821b8, gate_4482120);
        }

        /* Step 72f — sub_651610 disassembly shows `ldr x14, [x20, #0x220]`
         * at 0x65164c, meaning g_switch_state_array @ 0x4480220 is a
         * POINTER FIELD, not an embedded array. The actual per-switch
         * structs live at *(uint64_t*)0x4480220 + sw_id*0x30, on the
         * heap. We've been reading/writing the static .bss instead.
         *
         * Read the pointer here and use it for the real diagnostic. */
        uintptr_t real_sw_array_ptr = 0;
        memcpy(&real_sw_array_ptr, (void*)(game_base + 0x4480220), 8);
        SHIM_LOG("[sw_brk] *0x4480220 (g_switch_state_array PTR) = %p",
                 (void*)real_sw_array_ptr);

        /* Step 72e — sub_651610's push gate diagnostic. Three conditions
         * MUST hold for the push to happen on a rising edge:
         *   1. *(g_switch_state_array + sw*0x30 + 0x2a) & 1 == 0
         *   2. *(*(uint64_t*)data_4480228 + sw) & 1 == 0   (disable mask)
         *   3. *(g_switch_state_array + sw*0x30 + 0x26) == 0  (counter)
         * Plus the channel-bit slot (planted above) must hold sw_id != 0.
         * Dump all three so we know which one (if any) blocks us. */
        {
            uintptr_t sw_struct = (real_sw_array_ptr ? real_sw_array_ptr
                                                    : (game_base + 0x4480220))
                                + (uintptr_t)switch_id * 0x30;
            uint8_t  flag_2a = 0;
            uint16_t counter_26 = 0;
            uint8_t  flag_2a_pre[16] = {0};
            memcpy(&flag_2a,     (void*)(sw_struct + 0x2a), 1);
            memcpy(&counter_26,  (void*)(sw_struct + 0x26), 2);
            memcpy(flag_2a_pre,  (void*)(sw_struct),        16);

            uintptr_t mask_ptr = 0;
            memcpy(&mask_ptr, (void*)(game_base + 0x4480228), 8);
            uint8_t mask_byte = 0;
            int mask_readable = 0;
            if (mask_ptr >= 0x10000) {
                /* Read as best we can; if it crashes our SIGSEGV handler
                 * catches it. mask_ptr might already be game-relative. */
                memcpy(&mask_byte, (void*)(mask_ptr + (uintptr_t)switch_id), 1);
                mask_readable = 1;
            }
            SHIM_LOG("[sw_brk] sw93 struct dump: "
                     "@%p first16=%02x%02x%02x%02x%02x%02x%02x%02x"
                     "%02x%02x%02x%02x%02x%02x%02x%02x  "
                     "+0x26 (counter)=%u  +0x2a (flag)=0x%02x",
                     (void*)sw_struct,
                     flag_2a_pre[0], flag_2a_pre[1], flag_2a_pre[2], flag_2a_pre[3],
                     flag_2a_pre[4], flag_2a_pre[5], flag_2a_pre[6], flag_2a_pre[7],
                     flag_2a_pre[8], flag_2a_pre[9], flag_2a_pre[10], flag_2a_pre[11],
                     flag_2a_pre[12], flag_2a_pre[13], flag_2a_pre[14], flag_2a_pre[15],
                     counter_26, flag_2a);
            SHIM_LOG("[sw_brk] data_4480228=%p  mask[%d]=0x%02x readable=%d",
                     (void*)mask_ptr, switch_id, mask_byte, mask_readable);
        }

        /* Build the closed-state payload (rising edge): bit-A | bit-B set
         * at the wire-mapped byte index. */
        uint8_t closed_payload[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        if (wire->byte_index >= 0 && wire->byte_index < 8) {
            closed_payload[wire->byte_index] =
                wire->bit_a_mask | wire->bit_b_mask;
        }
        uint8_t open_payload[8] = {0, 0, 0, 0, 0, 0, 0, 0};

        const uint32_t SHIM_FIRE_HOLD_MS = 120;

        /* Path 1 — arm the sub_8edfe0 BRK synthesizer. Only effective if
         * the orchestrator/poll thread happens to be alive during the
         * arm window. In the current dormant-orchestrator state this
         * is a no-op; left in as a diagnostic / future-proofing for
         * runs where the gate fires. */
        shim_sub_8edfe0_arm(wire->channel, closed_payload, SHIM_FIRE_HOLD_MS);

        /* Path 2 — direct call to sub_651610 (Recipe B). Pushes the
         * "switch closed" diff into the pending list immediately,
         * regardless of orchestrator state. The drainer sub_6511c0
         * (running on its own thread per BN) picks it up and fires the
         * event. */
        shim_direct_call_sub_651610(wire->channel, closed_payload);

        /* Mid-fire dump — IMMEDIATELY after the closure call, before
         * any drainer thread can run. Confirms whether sub_651610
         * reached the push code path (counter would be 1) or bailed
         * earlier (counter still 0). */
        {
            uintptr_t sw_struct = (real_sw_array_ptr ? real_sw_array_ptr
                                                    : (game_base + 0x4480220))
                                + (uintptr_t)switch_id * 0x30;
            uint16_t counter_26 = 0;
            uintptr_t pend_head = 0;
            uint8_t   ch_buf[8] = {0};
            memcpy(&counter_26, (void*)(sw_struct + 0x26), 2);
            memcpy(&pend_head,  (void*)(game_base + 0x4480278), 8);
            memcpy(ch_buf, (void*)(game_base + 0x44804b8
                          + (uintptr_t)wire->channel * 0xb0 + 0x20), 8);
            SHIM_LOG("[sw_brk] mid-fire (post-closure pre-release): "
                     "sw93 (real) counter+0x26=%u  pending_head=%p  "
                     "per-ch state[0x20..0x27]=%02x%02x%02x%02x%02x%02x%02x%02x",
                     counter_26, (void*)pend_head,
                     ch_buf[0], ch_buf[1], ch_buf[2], ch_buf[3],
                     ch_buf[4], ch_buf[5], ch_buf[6], ch_buf[7]);
        }

        /* Hold — give the drainer + event handlers time to process the
         * closed edge before we fire the release. */
        struct timespec hold_ts = {
            .tv_sec  = 0,
            .tv_nsec = (long)SHIM_FIRE_HOLD_MS * 1000 * 1000
        };
        nanosleep(&hold_ts, NULL);

        /* Release: zero payload triggers sub_651610's diff to record a
         * "switch opened" transition. Crucial for switches that fire
         * events on the falling edge (most pinball switches). */
        shim_direct_call_sub_651610(wire->channel, open_payload);

        /* Settle window so the release-edge events finish processing
         * before we snap the cmode state and report Δ. */
        struct timespec settle_ts = {
            .tv_sec  = 0,
            .tv_nsec = 80 * 1000 * 1000
        };
        nanosleep(&settle_ts, NULL);

        /* Post-fire diagnostic — did anything land on the pending list,
         * did the counter at +0x26 increment (proves sub_651610 reached
         * the bit-walk for switch_id), and what's the per-channel state
         * byte at +0x22 (proves arg2 reached the per-channel buffer)? */
        {
            uintptr_t pend_head = 0;
            memcpy(&pend_head, (void*)(game_base + 0x4480278), 8);
            uint8_t state_byte_in_g_arr = 0;
            memcpy(&state_byte_in_g_arr, (void*)state_addr, 1);

            uintptr_t sw_struct = game_base + 0x4480220 + (uintptr_t)switch_id * 0x30;
            uint16_t counter_26 = 0;
            uint8_t flag_2a = 0;
            memcpy(&counter_26, (void*)(sw_struct + 0x26), 2);
            memcpy(&flag_2a,    (void*)(sw_struct + 0x2a), 1);

            uintptr_t per_channel = game_base + 0x44804b8
                                  + (uintptr_t)wire->channel * 0xb0;
            uint8_t channel_state[8] = {0};
            memcpy(channel_state, (void*)(per_channel + 0x20), 8);

            SHIM_LOG("[sw_brk] post-fire: pending_head=%p  "
                     "g_switch93 state=0x%02x  "
                     "sw_struct +0x26 counter=%u  +0x2a flag=0x%02x  "
                     "per-ch[6] state[0x20..0x27]=%02x%02x%02x%02x%02x%02x%02x%02x",
                     (void*)pend_head, state_byte_in_g_arr,
                     counter_26, flag_2a,
                     channel_state[0], channel_state[1], channel_state[2],
                     channel_state[3], channel_state[4], channel_state[5],
                     channel_state[6], channel_state[7]);
        }
    } else {
        /* Legacy byte-flip path — kept for switches whose channel/bit
         * mapping hasn't been traced yet. Known half-hooked: writes the
         * post-translator state byte but skips sub_651610's pending-list
         * push, so the drainer never fires the event. Diagnostic value
         * only. */
        uint8_t closed = 1;
        memcpy((void*)state_addr, &closed, 1);

        struct timespec ts = { .tv_sec = 0, .tv_nsec = 20 * 1000 * 1000 };
        nanosleep(&ts, NULL);

        memcpy((void*)state_addr, &before_state, 1);
    }

    shim_capture_cmode_snap(game_base, snap_after);

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
        "  switch %d '%s': mode=%s before_state=%u Δ=%d%s\n",
        switch_id, sw->name,
        wire ? "wire-arm" : "state-flip",
        before_state, delta_count,
        delta_count > 0 ? delta_buf
                        : (wire
                            ? " (no cmode state change — bridge poll may not have"
                              " reached sub_8edfe0(channel) during the arm window)"
                            : " (no cmode state change — translator not yet"
                              " hooked, see PROTOCOL.md)"));
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

    shim_patch_video_player_assertion();
    shim_patch_video_player_play_entry_brk();
    /* Step 72: in-binary BRK trap that synthesizes per-channel switch
     * state; needs game_base resolvable, so deferred to here too. The
     * function self-gates on SPIKE3_PATCH_SUB_8EDFE0=1 OR SPIKE3_FIRE_SWITCH
     * being set. */
    shim_patch_sub_8edfe0_brk();
    /* Step 72g: fire_event selective-return BRK — forces the
     * orchestrator-gating events (0x80..0x83) to return 1 so the
     * nodebus poll thread spawns and the orchestrator completes.
     * Self-gates on SPIKE3_FORCE_NODEBUS_GATES=1. */
    shim_patch_fire_event_brk();
    /* Discovery module: same game_base-resolution dependency as the
     * patches above, so deferred to the same 2 s settle window. */
    shim_discovery_init();
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

/* Step 72 deferred patch installer.
 *
 * Cannot be called from shim_inject_start at constructor time because
 * shim_resolve_game_base() works by walking the calling thread's stack for
 * a /game frame; at constructor time no game thread has run yet, so the
 * resolver returns 0 and the patch aborts. Step 71's
 * shim_patch_video_player_play_entry_brk has the same dependency and is
 * already deferred by shim_vp_patch_thread (2 s sleep then call).
 *
 * shim_vp_patch_thread is gated on SPIKE3_PATCH_SPI_VIDEO=1 (default 1 in
 * run_game.sh). The patches inside it self-gate on their own env vars or
 * are unconditionally always-on. Adding our patch there inherits both the
 * settle delay AND the gating discipline. */
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

/* ---- Step 72g: nodebus gate poker -----------------------------------
 *
 * Per BN trace (Step 72c): the nodebus poll loop checks 5 state flags
 * before doing useful work. The orchestrator normally sets them, but in
 * our environment the orchestrator runs only partially (fire_event hook
 * forces returns but downstream writes don't all land — we see
 * data_4482120 = 0x11 but data_44821b8 = 0 after the run). Poking the
 * remaining flags directly bypasses the missing orchestrator code paths.
 *
 * The thread sleeps a beat (30 s) so the orchestrator has had time to
 * run and update what it CAN, then writes the values it normally
 * SHOULD have set on a fully-successful run.
 *
 * Self-gated on SPIKE3_FORCE_NODEBUS_GATES=1 (same gate as fire_event
 * BRK; they're paired).
 */
static void *shim_poke_nodebus_gates_thread(void *arg) {
    (void)arg;
    const char *en = getenv("SPIKE3_FORCE_NODEBUS_GATES");
    if (!en || en[0] != '1') return NULL;

    /* Wait long enough that orchestrator has tried to run + fire_event
     * hooks have fired their forced returns. ~30 s gives plenty of
     * margin (orchestrator typically fires within 10 s of boot). */
    sleep(30);

    uintptr_t game_base = shim_resolve_game_base();
    if (!game_base) {
        SHIM_LOG("[gates] game_base unresolved; aborting nodebus-gates poke");
        return NULL;
    }

    /* Pre-poke read for the log. */
    uint32_t pre_4482120 = 0, pre_44821b8 = 0;
    uint32_t pre_4482124 = 0, pre_4482190 = 0, pre_4482194 = 0;
    memcpy(&pre_4482120, (void*)(game_base + 0x4482120), 4);
    memcpy(&pre_44821b8, (void*)(game_base + 0x44821b8), 1);
    memcpy(&pre_4482124, (void*)(game_base + 0x4482124), 4);
    memcpy(&pre_4482190, (void*)(game_base + 0x4482190), 4);
    memcpy(&pre_4482194, (void*)(game_base + 0x4482194), 4);
    SHIM_LOG("[gates] PRE: 0x4482120=0x%08x  0x44821b8=0x%02x  0x4482124=0x%08x  0x4482190=0x%08x  0x4482194=0x%08x",
             pre_4482120, pre_44821b8, pre_4482124, pre_4482190, pre_4482194);

    /* Per BN agent (Step 72c): values orchestrator sets on a successful
     * run. data_4482120 |= 0x11 (OR — preserves any other bits already
     * set); the rest are direct assignments. */
    uint32_t v_4482120 = pre_4482120 | 0x11;
    uint8_t  v_44821b8 = 1;
    uint32_t v_4482124 = 1;
    uint32_t v_4482190 = 1;
    uint32_t v_4482194 = 2;
    memcpy((void*)(game_base + 0x4482120), &v_4482120, 4);
    memcpy((void*)(game_base + 0x44821b8), &v_44821b8, 1);
    memcpy((void*)(game_base + 0x4482124), &v_4482124, 4);
    memcpy((void*)(game_base + 0x4482190), &v_4482190, 4);
    memcpy((void*)(game_base + 0x4482194), &v_4482194, 4);
    SHIM_LOG("[gates] POST: 0x4482120=0x%08x  0x44821b8=0x%02x  0x4482124=0x%08x  0x4482190=0x%08x  0x4482194=0x%08x",
             v_4482120, v_44821b8, v_4482124, v_4482190, v_4482194);
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

    /* SPIKE3_FIRE_SWITCH=<id1[:id2:...]> — fire one or more switches.
     *
     * The companion BRK patch (sub_8edfe0) is installed inside
     * shim_vp_patch_thread's 2 s settle, NOT here. shim_resolve_game_base
     * doesn't work this early — backtrace has no /game frames at
     * constructor-end time. SPIKE3_SWITCH_DELAY (default 30) is far longer
     * than shim_vp_patch_thread's 2 s sleep, so by the time the firer
     * arms, the BRK is in place. */
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

    /* SPIKE3_FORCE_NODEBUS_GATES=1 — Step 72g: spawn the gate-poker
     * thread. Sleeps 30 s then writes the 5 nodebus orchestrator
     * gate flags. Pairs with the fire_event BRK installed by
     * shim_vp_patch_thread. */
    {
        const char *fg = getenv("SPIKE3_FORCE_NODEBUS_GATES");
        if (fg && fg[0] == '1') {
            pthread_t t;
            if (pthread_create(&t, NULL, shim_poke_nodebus_gates_thread, NULL) == 0)
                pthread_detach(t);
        }
    }
}
