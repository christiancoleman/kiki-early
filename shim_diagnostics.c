/*
 * shim_diagnostics.c — crash/termination dumps, cmode table, BN VA resolver.
 *
 * Phase 3 of the refactor. Contents carved from spi_shim.c:
 *   - shim_safe_read / shim_hexdump_stderr (guest-memory dump helpers)
 *   - shim_dump_sound_globals (Pokemon LE sound registry snapshot)
 *   - shim_cmode_entry / shim_cmode_table / shim_cmode_table_count (the 42
 *     C-mode registry; shared with shim_bridges.c via spi_shim.h extern decls)
 *   - shim_resolve_game_base (BN VA -> runtime addr resolver, used widely)
 *   - shim_dump_cmodes / shim_dump_dispatch_tables
 *   - shim_sigsegv_handler / shim_term_dump_handler (installed by
 *     shim_install_crash_handler in spi_shim.c)
 *
 * Dependencies resolved via spi_shim.h:
 *   - SHIM_LOG
 *   - event-sweep externs (shim_event_fire_in_progress,
 *     shim_current_sweep_event_id, shim_event_sweep_jmp) — defined in
 *     spi_shim.c, read by shim_sigsegv_handler to decide whether to longjmp
 *     back out of a faulting sweep iteration.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <setjmp.h>
#include <execinfo.h>
#include <dlfcn.h>
#include <sys/types.h>

#include "spi_shim.h"

/* ========== moved from spi_shim.c (369-963): diagnostics: SIGSEGV comment + safe_read + hexdump + dump_sound_globals + cmode_table + resolve_game_base + dump_cmodes + dump_dispatch_tables ========== */
/* ---- SIGSEGV/SIGBUS backtrace handler ----
 *
 * QEMU user-mode currently reports `uncaught target signal 11` for every
 * crash in the guest binary. Installing a guest-side sigaction lets us
 * catch the fault inside the guest process (the shim is loaded into the
 * guest's address space via LD_PRELOAD) and dump a backtrace before
 * chaining to the default handler for the core dump.
 *
 * Frame addresses are runtime addresses in the guest's address space.
 * `dladdr()` resolves each frame to module + offset, so we can translate
 * to file offsets by subtracting the module base and map into Binary
 * Ninja via `BN_VA = file_offset + 0x400000` (PROTOCOL.md).
 *
 * Async-signal safety: snprintf() isn't strictly async-signal-safe, but
 * in practice it doesn't take locks or allocate, and we only care about
 * getting output out before the inevitable re-crash.
 */

/* Read `size` bytes from guest VA `addr` into `dst`. Under QEMU user-mode
 * the shim runs as guest code, so a direct memcpy hits QEMU's address
 * translation and reaches guest memory (unlike /proc/self/mem, which is
 * host-view). If `addr` points to an unmapped guest page the memcpy
 * faults — if we're already inside a SIGSEGV handler, the second fault
 * will terminate the process. Caller should write diagnostic context
 * BEFORE calling this so at least the header makes it to stderr. */
static ssize_t shim_safe_read(void *dst, uintptr_t addr, size_t size) {
    memcpy(dst, (const void *)addr, size);
    return (ssize_t)size;
}

/* Hexdump `size` bytes starting at guest VA `addr` to stderr in
 * `offset: hh hh hh ... |ascii|` format. Uses shim_safe_read so
 * unmapped addresses report an error rather than crashing. */
static void shim_hexdump_stderr(const char *label, uintptr_t addr, size_t size) {
    char hdr[256];
    int hn = snprintf(hdr, sizeof(hdr),
        "\n--- DUMP %s @ 0x%lx (%zu bytes) ---\n", label, addr, size);
    ssize_t u = write(STDERR_FILENO, hdr, (size_t)hn);
    (void)u;

    unsigned char buf[4096];
    size_t off = 0;
    while (off < size) {
        size_t chunk = size - off;
        if (chunk > sizeof(buf)) chunk = sizeof(buf);
        ssize_t got = shim_safe_read(buf, addr + off, chunk);
        if (got <= 0) {
            char err[128];
            int en = snprintf(err, sizeof(err),
                "  (unreadable at 0x%lx: pread=%zd errno=%d)\n",
                addr + off, got, errno);
            u = write(STDERR_FILENO, err, (size_t)en);
            break;
        }
        for (ssize_t i = 0; i < got; i += 16) {
            char line[128];
            int ln = snprintf(line, sizeof(line), "%08lx: ", addr + off + (size_t)i);
            ssize_t j;
            for (j = 0; j < 16 && (i + j) < got; j++)
                ln += snprintf(line + ln, sizeof(line) - (size_t)ln,
                               "%02x ", buf[i + j]);
            while (ln < 60) line[ln++] = ' ';
            line[ln++] = '|';
            for (j = 0; j < 16 && (i + j) < got; j++) {
                unsigned char c = buf[i + j];
                line[ln++] = (c >= 32 && c < 127) ? (char)c : '.';
            }
            line[ln++] = '|';
            line[ln++] = '\n';
            u = write(STDERR_FILENO, line, (size_t)ln);
        }
        off += (size_t)got;
    }
}

/* One-shot diagnostic: dump /proc/self/maps grep'd for the game binary
 * and the two sound-system globals. Called from SIGSEGV handler. */
static void shim_dump_sound_globals(void) {
    /* Print the shim's own handler address so we know what VA range
     * our code landed at under QEMU user-mode. If this is in the
     * 0x7xxx... range, the shim is in high-memory mmap and we need
     * to rebase guest addresses accordingly. */
    char addr_line[256];
    int aln = snprintf(addr_line, sizeof(addr_line),
        "\n--- shim handler addresses (for computing QEMU guest base) ---\n"
        "&shim_dump_sound_globals = %p\n"
        "(shim .so was compiled with default aarch64 Linux ABI)\n",
        (void*)shim_dump_sound_globals);
    ssize_t u = write(STDERR_FILENO, addr_line, (size_t)aln);
    (void)u;

    /* Print the FULL /proc/self/maps so we can see file-backed
     * mappings for the game binary. */
    int mf = open("/proc/self/maps", O_RDONLY);
    if (mf >= 0) {
        char buf[32768];
        ssize_t total = 0;
        ssize_t n;
        while (total < (ssize_t)sizeof(buf) - 1 &&
               (n = read(mf, buf + total, sizeof(buf) - 1 - (size_t)total)) > 0) {
            total += n;
        }
        close(mf);
        if (total > 0) {
            u = write(STDERR_FILENO,
                "\n--- /proc/self/maps (full) ---\n", 33);
            (void)u;
            u = write(STDERR_FILENO, buf, (size_t)total);
            (void)u;
            u = write(STDERR_FILENO, "\n--- end /proc/self/maps ---\n", 30);
            (void)u;
        }
    }

    /* Find the game binary's load base at runtime by walking backtrace
     * frames for one inside ./game and subtracting its file offset.
     * QEMU user-mode + ASLR puts the game at a randomized base
     * (~0x7ab...) rather than at ELF VA 0x400000. Filter out .so
     * frames — the shim path contains the substring "games/" which
     * would falsely match a bare "/game" strstr. */
    uintptr_t game_base = 0;
    void *bt[64];
    int nb = backtrace(bt, 64);
    for (int i = 0; i < nb; i++) {
        Dl_info bi;
        if (dladdr(bt[i], &bi) && bi.dli_fname &&
            strstr(bi.dli_fname, "/game") &&
            !strstr(bi.dli_fname, ".so")) {
            game_base = (uintptr_t)bi.dli_fbase;
            break;
        }
    }

    char base_line[256];
    int bln;
    if (game_base == 0) {
        bln = snprintf(base_line, sizeof(base_line),
            "\n--- could NOT resolve game binary load base via dladdr; aborting dump ---\n");
        u = write(STDERR_FILENO, base_line, (size_t)bln);
        return;
    }
    bln = snprintf(base_line, sizeof(base_line),
        "\n--- resolved game load base = 0x%lx ---\n"
        "(hashmap = base + 0x44c56d0; registry = base + 0x44ca4d0)\n",
        game_base);
    u = write(STDERR_FILENO, base_line, (size_t)bln);

    /* Self-probe: read 32 bytes at our own function's address. If
     * THIS fails, memcpy can't read anything — something is wrong
     * with how the shim executes in its own memory (unlikely). */
    shim_hexdump_stderr("self-probe (&shim_dump_sound_globals)",
                        (uintptr_t)&shim_dump_sound_globals, 0x20);

    /* Frame-probe: read at the actual backtrace frame we found in
     * the game binary. We know this address IS valid because the
     * game is executing there right now. */
    if (nb > 0) {
        /* Find first frame in ./game and probe there directly. */
        for (int i = 0; i < nb; i++) {
            Dl_info bi;
            if (dladdr(bt[i], &bi) && bi.dli_fname && strstr(bi.dli_fname, "/game")) {
                shim_hexdump_stderr("frame-probe (live game PC from backtrace)",
                                    (uintptr_t)bt[i], 0x20);
                break;
            }
        }
    }

    /* Sanity probe: dump 64 bytes at the known rodata location of
     * SE_PK_FX_ATTACK_TYPE_BUG. If this doesn't print an ASCII run
     * starting with "SE_PK_FX_ATTACK_TYPE_BUG", game_base resolution
     * is wrong and all subsequent dumps are suspect. */
    shim_hexdump_stderr("rodata probe (SE_PK_FX_ATTACK_TYPE_BUG)",
                        game_base + 0x3607900, 0x40);

    /* Parse the g_asset_registry _Hashtable header and walk into
     * the first few nodes. BN VA 0x48ca4d0 → ELF VA 0x44ca4d0. */
    uintptr_t reg_addr = game_base + 0x44ca4d0;
    uint64_t buckets_ptr = 0, bucket_count = 0, first_node = 0;
    uint64_t element_count = 0, next_resize = 0;
    uint32_t max_lf_bits = 0;
    memcpy(&buckets_ptr,  (void*)(reg_addr + 0x00), 8);
    memcpy(&bucket_count, (void*)(reg_addr + 0x08), 8);
    memcpy(&first_node,   (void*)(reg_addr + 0x10), 8);
    memcpy(&element_count,(void*)(reg_addr + 0x18), 8);
    memcpy(&max_lf_bits,  (void*)(reg_addr + 0x20), 4);
    memcpy(&next_resize,  (void*)(reg_addr + 0x28), 8);
    float max_lf;
    memcpy(&max_lf, &max_lf_bits, 4);

    char summary[512];
    int sn = snprintf(summary, sizeof(summary),
        "\n=== g_asset_registry _Hashtable @ 0x%lx ===\n"
        "  _M_buckets       = 0x%lx\n"
        "  _M_bucket_count  = %lu\n"
        "  _M_first_node    = 0x%lx\n"
        "  _M_element_count = %lu\n"
        "  _M_max_load_factor = %.2f\n"
        "  _M_next_resize   = %lu\n",
        reg_addr, buckets_ptr, bucket_count, first_node,
        element_count, max_lf, next_resize);
    u = write(STDERR_FILENO, summary, (size_t)sn);

    /* Walk the full _M_nxt chain and dump each node's raw bytes to
     * _emulation/captures/asset_registry.bin for the standalone parser. Each
     * node is assumed ≤128 bytes (libstdc++ _Hash_node<pair<K,V>,true>
     * with cached hash; larger than typical but conservative). */
    const size_t NODE_SIZE = 128;
    uint64_t node_count = 0;
    int fd = open("_emulation/captures/asset_registry.bin",
                  O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        const char *msg = "\n[shim] failed to open _emulation/captures/asset_registry.bin\n";
        u = write(STDERR_FILENO, msg, strlen(msg));
    } else {
        /* File layout:
         *   [0x00..0x40)   64B file header (magic, game_base, registry_va, counts)
         *   [0x40..0x80)   64B raw copy of the _Hashtable struct
         *   [0x80..end)    node_count × NODE_SIZE bytes, each node raw */
        struct {
            char     magic[8];         // "SPK3ASR\0"
            uint64_t game_base;
            uint64_t registry_va;
            uint64_t hashtable_bytes;  // 64
            uint64_t node_size;        // NODE_SIZE
            uint64_t node_count;       // filled after walk
            uint64_t bucket_count;
            uint64_t element_count;
        } hdr = {{0}};
        memcpy(hdr.magic, "SPK3ASR", 8);
        hdr.game_base = game_base;
        hdr.registry_va = reg_addr;
        hdr.hashtable_bytes = 64;
        hdr.node_size = NODE_SIZE;
        hdr.bucket_count = bucket_count;
        hdr.element_count = element_count;
        /* Placeholder — rewritten after walk. */
        u = (ssize_t)write(fd, &hdr, sizeof(hdr));
        /* Pad to 64 bytes. */
        char pad[64];
        memset(pad, 0, sizeof(pad));
        u = (ssize_t)write(fd, pad, 64 - sizeof(hdr));

        /* Dump the _Hashtable struct verbatim (64 bytes covers it). */
        u = (ssize_t)write(fd, (void*)reg_addr, 64);

        /* Walk the chain. */
        char node_buf[128];
        uintptr_t node = (uintptr_t)first_node;
        while (node != 0 && node_count < 3000) {
            memcpy(node_buf, (void*)node, NODE_SIZE);
            u = (ssize_t)write(fd, node_buf, NODE_SIZE);
            /* _M_nxt is at +0x00 of every node. */
            uint64_t nxt;
            memcpy(&nxt, &node_buf[0], 8);
            node = (uintptr_t)nxt;
            node_count++;
        }

        /* Rewrite header with final node_count. */
        hdr.node_count = node_count;
        lseek(fd, 0, SEEK_SET);
        u = (ssize_t)write(fd, &hdr, sizeof(hdr));
        close(fd);

        char msg[256];
        int ml = snprintf(msg, sizeof(msg),
            "\n[shim] asset_registry dumped: %lu nodes × %zu bytes → "
            "_emulation/captures/asset_registry.bin (%lu bytes total, elem_count=%lu)\n",
            node_count, NODE_SIZE, 64 + 64 + node_count * NODE_SIZE, element_count);
        u = write(STDERR_FILENO, msg, (size_t)ml);
    }

    /* Keep a small in-band hexdump so the user can eyeball structure
     * without opening the binary file. 0x200 bytes = ~4 nodes. */
    if (first_node) {
        shim_hexdump_stderr("asset_registry first nodes (eyeball sample)",
                            first_node, 0x200);
    }
    (void)node_count;
}

/* ---- cmode dump (Step 50) ----
 *
 * _INIT_74 registers 42 crule::cmode_* objects at fixed BN VA addresses.
 * Struct layout (verified from sub_90d840 decomp):
 *   +0x00  vtable ptr
 *   +0x08  mode_id          (uint32)
 *   +0x10  arg5 behaviour   (uint32)
 *   +0x18  const 0x700
 *   +0x30  type_flags       (uint32)
 *   +0x58  scene_data ptr   (from get_scene_data_by_id(scene_id))
 *   +0xb0  timer_obj ptr
 *   +0x308 init_flag        (int — changes from 0 when mode activates)
 *
 * BN VA → runtime address = game_base + (bn_va - 0x400000).
 *
 * For 42 slots, dump (mode_id, type_flags, scene_ptr, init_flag) to give
 * us a live snapshot of the game's mode state. If static init ran
 * successfully these will all have the expected mode_id / type_flags
 * values matching MODE_TABLE.md. If any init_flag is non-zero, that mode
 * has transitioned from static-registration to "activated" state.
 */

/* struct shim_cmode_entry definition moved to spi_shim.h — the state bridge
 * (shim_bridges.c) reads this table. */

const struct shim_cmode_entry shim_cmode_table[] = {
    /* Simple/base modes (type_flags=0x01) — 20 entries */
    { 0x4856ae0, 0x00, 0x01, "base_00" },
    { 0x4856be0, 0x01, 0x01, "base_01" },
    { 0x4856ce0, 0x02, 0x01, "base_02" },
    { 0x4856df0, 0x03, 0x01, "base_03" },
    { 0x4856f38, 0x04, 0x01, "base_04" },
    { 0x4856fb8, 0x05, 0x01, "base_05" },
    { 0x4857020, 0x06, 0x01, "base_06" },
    { 0x48570c0, 0x08, 0x01, "base_08" },
    { 0x48571a0, 0x07, 0x01, "base_07" },
    { 0x4857288, 0x0b, 0x01, "base_0b" },
    { 0x4857320, 0x0a, 0x01, "base_0a" },
    { 0x48573d0, 0x0c, 0x01, "base_0c" },
    { 0x4857448, 0x1f, 0x01, "base_1f" },
    { 0x48574b8, 0x0d, 0x01, "base_0d" },
    { 0x4857548, 0x0e, 0x01, "base_0e" },
    { 0x4857608, 0x10, 0x01, "base_10" },
    { 0x48576f8, 0x0f, 0x01, "base_0f" },
    { 0x4857760, 0x11, 0x01, "base_11" },
    { 0x48577d0, 0x1e, 0x01, "base_1e" },
    { 0x4857950, 0x12, 0x01, "base_12" },
    /* type_flags=0x08 utility modes */
    { 0x48579d0, 0x25, 0x08, "util_25" },
    { 0x4858360, 0x24, 0x08, "util_24" },
    { 0x4859288, 0x1c, 0x08, "util_1c" },
    /* Battle environment modes (type_flags=0x48) */
    { 0x4857aa0, 0x20, 0x48, "battle_mountain" },
    { 0x4857cd0, 0x21, 0x48, "battle_forest" },
    { 0x4857f00, 0x22, 0x48, "battle_desert" },
    { 0x4858130, 0x23, 0x48, "battle_coast" },
    /* Pokemon story modes (type_flags=0x28) */
    { 0x4858480, 0x13, 0x28, "story_bulbasaur_1" },
    { 0x48585e0, 0x14, 0x28, "story_bulbasaur_2" },
    { 0x48586b8, 0x15, 0x28, "story_charmander_1" },  /* CONFIRMED */
    { 0x48587f8, 0x16, 0x28, "story_charmander_2" },
    { 0x4858900, 0x19, 0x28, "story_squirtle_1" },
    { 0x4858a50, 0x1a, 0x28, "story_squirtle_2" },
    /* Pikachu modes (type_flags=0x24) */
    { 0x4858df0, 0x17, 0x24, "pikachu_1" },
    { 0x4858f90, 0x18, 0x24, "pikachu_2" },
    /* Multiball family (type_flags=0x04) */
    { 0x4858b28, 0x09, 0x04, "mb_09" },
    { 0x4858c80, 0x26, 0x04, "mb_26" },
    { 0x4858d38, 0x27, 0x04, "mb_27" },
    /* Multiball / wizard (type_flags=0x84) */
    { 0x48590a0, 0x28, 0x84, "rocket_mb_1" },
    { 0x4859160, 0x1b, 0x84, "rocket_mb_2" },
    { 0x4859368, 0x1d, 0x84, "rocket_mb_3" },
    { 0x4859438, 0x29, 0x84, "rocket_wizard" },
};
/* Companion count — external TUs (shim_bridges.c) can't sizeof an extern array. */
const size_t shim_cmode_table_count =
    sizeof(shim_cmode_table) / sizeof(shim_cmode_table[0]);

/* Resolve the game binary's load base. Caches on first success — needed
 * because some shim code paths (e.g. the Step 52 sweep timer thread) run
 * from threads that have no game frames on their stack, making backtrace
 * + dladdr return nothing. Any shim hook called from a game thread will
 * populate the cache via this function before the timer thread needs it. */
static uintptr_t shim_cached_game_base = 0;
uintptr_t shim_resolve_game_base(void) {
    uintptr_t cached = __atomic_load_n(&shim_cached_game_base, __ATOMIC_ACQUIRE);
    if (cached) return cached;

    void *bt[64];
    int nb = backtrace(bt, 64);
    for (int i = 0; i < nb; i++) {
        Dl_info bi;
        if (dladdr(bt[i], &bi) && bi.dli_fname &&
            strstr(bi.dli_fname, "/game") &&
            !strstr(bi.dli_fname, ".so")) {
            uintptr_t base = (uintptr_t)bi.dli_fbase;
            __atomic_store_n(&shim_cached_game_base, base, __ATOMIC_RELEASE);
            return base;
        }
    }
    return 0;
}

static void shim_dump_cmodes(void) {
    uintptr_t game_base = shim_resolve_game_base();
    if (game_base == 0) {
        const char *m = "\n--- cmode dump: game_base not resolved, skipping ---\n";
        ssize_t u = write(STDERR_FILENO, m, strlen(m));
        (void)u;
        return;
    }

    const char *hdr = "\n--- cmode state dump (42 entries from _INIT_74) ---\n"
                      "slot  bn_va        rt_va            mode_id  type_flags  "
                      "scene_ptr        init_flag  name\n";
    ssize_t u = write(STDERR_FILENO, hdr, strlen(hdr));
    (void)u;

    size_t N = sizeof(shim_cmode_table) / sizeof(shim_cmode_table[0]);
    int ok_count = 0, mismatch_count = 0, activated_count = 0;

    for (size_t i = 0; i < N; i++) {
        const struct shim_cmode_entry *e = &shim_cmode_table[i];
        uintptr_t rt_va = game_base + (e->bn_va - 0x400000);

        uint32_t mode_id = 0, type_flags = 0, init_flag = 0;
        uintptr_t scene_ptr = 0;

        /* Direct memcpy — under QEMU user-mode the shim runs as guest code,
         * so accessing guest VAs works (same as asset_registry dump). */
        memcpy(&mode_id,     (void*)(rt_va + 0x08), 4);
        memcpy(&type_flags,  (void*)(rt_va + 0x0c), 4);   /* CORRECTED from +0x30 */
        memcpy(&scene_ptr,   (void*)(rt_va + 0x58), 8);
        memcpy(&init_flag,   (void*)(rt_va + 0x308), 4);

        int ok = (mode_id == e->expected_mode_id &&
                  type_flags == e->expected_type_flags);
        if (ok) ok_count++; else mismatch_count++;
        if (init_flag != 0) activated_count++;

        char line[256];
        int ln = snprintf(line, sizeof(line),
            "%4zu  0x%08lx   0x%012lx   0x%02x      0x%02x        "
            "0x%012lx   0x%08x  %s%s\n",
            i, e->bn_va, (unsigned long)rt_va,
            mode_id, type_flags,
            (unsigned long)scene_ptr, init_flag,
            e->name_hint,
            ok ? "" : "  [MISMATCH]");
        u = write(STDERR_FILENO, line, (size_t)ln);
        (void)u;
    }

    char tail[256];
    int tl = snprintf(tail, sizeof(tail),
        "\n--- cmode dump summary: %d/%zu mode_id+type_flags matched, "
        "%d activated (init_flag != 0), %d mismatches ---\n"
        "(type_flags mismatches are expected — we haven't pinned the offset yet; "
        "see hexdump below)\n",
        ok_count, N, activated_count, mismatch_count);
    u = write(STDERR_FILENO, tail, (size_t)tl);
    (void)u;

    /* Dereference scene_data_ptr for a few representative cmodes and
     * hexdump the target memory. If scene_data contains a SHA-1 (Radium
     * scene identifier) or a pointer to one, we'll see it as ASCII hex
     * text in the dump. Three cmodes chosen:
     *   - base_00 (mode 0x00, scene_id 0x29) — shared attract scene
     *   - story_charmander_1 (mode 0x15, scene_id 0x1a) — known Radium
     *     scene SHA-1 "7949bb14a9a21abf5e2885a01eb5ed4f9c3a8eca"
     *   - battle_mountain (mode 0x20, scene_id 0x14) — distinct */
    uintptr_t scene_ptrs[3];
    memcpy(&scene_ptrs[0], (void*)(game_base + (0x4856ae0 - 0x400000) + 0x58), 8);
    memcpy(&scene_ptrs[1], (void*)(game_base + (0x48586b8 - 0x400000) + 0x58), 8);
    memcpy(&scene_ptrs[2], (void*)(game_base + (0x4857aa0 - 0x400000) + 0x58), 8);

    if (scene_ptrs[0]) shim_hexdump_stderr(
        "scene_data for base_00 (scene_id 0x29, shared attract)",
        scene_ptrs[0], 0x80);
    if (scene_ptrs[1]) shim_hexdump_stderr(
        "scene_data for story_charmander_1 (scene_id 0x1a, SHA-1 7949bb14...)",
        scene_ptrs[1], 0x80);
    if (scene_ptrs[2]) shim_hexdump_stderr(
        "scene_data for battle_mountain (scene_id 0x14)",
        scene_ptrs[2], 0x80);
}

/* ---- Step 51: dispatch table dump ----
 *
 * From BN analysis (user-provided):
 *   data_4767b30 = cmode pointer registry (42 entries × 8 bytes = 336 bytes)
 *     Iterated by sub_950d60 (event handler) + sub_94f140 (update callback)
 *   data_485ea80 = event dispatch linked-list heads (207 slots × 8 bytes = 1656 bytes)
 *     206 event types, slot [event_id] -> head of callback linked list
 *   data_48c52c0 = update-type linked-list heads (5 slots × 8 bytes = 40 bytes)
 *   data_480eab0 = scene_data lookup table (40 entries × 0x1a0 = 16640 bytes)
 *
 * Dump: (a) the 42 cmode registry pointers and verify they match the .bss
 * addresses we already expect. (b) which event slots have handlers — a
 * histogram of "# events with ≥1 handler registered".
 */
static void shim_dump_dispatch_tables(void) {
    uintptr_t game_base = shim_resolve_game_base();
    if (!game_base) {
        const char *m = "\n--- dispatch tables: game_base unresolved, skipping ---\n";
        ssize_t u = write(STDERR_FILENO, m, strlen(m)); (void)u;
        return;
    }

    /* --- cmode registry at data_4767b30 --- */
    uintptr_t reg_rt = game_base + (0x4767b30 - 0x400000);
    uintptr_t reg_end_rt = game_base + (0x4767c80 - 0x400000);
    size_t reg_entries = (reg_end_rt - reg_rt) / 8;

    char hdr[256];
    int hn = snprintf(hdr, sizeof(hdr),
        "\n--- cmode registry @ data_4767b30 (runtime 0x%lx, %zu entries) ---\n"
        " idx  registry_ptr_value   expected_bss     match?\n",
        reg_rt, reg_entries);
    ssize_t u = write(STDERR_FILENO, hdr, (size_t)hn); (void)u;

    /* Expected cmode BSS addresses from _INIT_74, in the ORDER we think
     * they're registered into data_4767b30. This is a guess — the actual
     * order may differ. If the guess is wrong, the "match?" column flags
     * it and we learn what the real order is from the registry itself. */
    static const uintptr_t expected_order[] = {
        0x4856ae0, 0x4856be0, 0x4856ce0, 0x4856df0, 0x4856f38, 0x4856fb8,
        0x4857020, 0x48571a0, 0x48570c0, 0x4857288, 0x4857320, 0x48573d0,
        0x4857448, 0x48574b8, 0x4857548, 0x4857608, 0x48576f8, 0x4857760,
        0x48577d0, 0x4857950, 0x48579d0, 0x4857aa0, 0x4857cd0, 0x4857f00,
        0x4858130, 0x4858360, 0x4858480, 0x48585e0, 0x48586b8, 0x48587f8,
        0x4858900, 0x4858a50, 0x4858b28, 0x4858c80, 0x4858d38, 0x4858df0,
        0x4858f90, 0x48590a0, 0x4859160, 0x4859288, 0x4859368, 0x4859438,
    };
    size_t N_expected = sizeof(expected_order) / sizeof(expected_order[0]);

    int matched = 0;
    for (size_t i = 0; i < reg_entries && i < 64; i++) {
        uintptr_t entry = 0;
        memcpy(&entry, (void*)(reg_rt + i * 8), 8);
        /* Unmap runtime→BN VA: subtract game_base, add 0x400000 */
        uintptr_t entry_bn = entry - game_base + 0x400000;
        uintptr_t expected_bn = (i < N_expected) ? expected_order[i] : 0;
        int mismatch = (expected_bn == 0 || entry_bn != expected_bn);
        if (!mismatch) matched++;

        char line[200];
        int ln = snprintf(line, sizeof(line),
            " %3zu  0x%014lx  0x%08lx  %s\n",
            i, (unsigned long)entry, (unsigned long)expected_bn,
            mismatch ? "[differs]" : "ok");
        u = write(STDERR_FILENO, line, (size_t)ln); (void)u;
    }
    char sum[128];
    int sl = snprintf(sum, sizeof(sum),
        "  cmode registry: %d/%zu matched expected .bss ordering\n",
        matched, reg_entries);
    u = write(STDERR_FILENO, sum, (size_t)sl); (void)u;

    /* --- event handler histogram at data_485ea80 (207 slots) --- */
    uintptr_t evt_base = game_base + (0x485ea80 - 0x400000);
    int populated = 0;
    int total_handlers = 0;
    char evt_detail[16384] = "\n--- event dispatch @ data_485ea80 (207 slots; non-empty shown) ---\n";
    size_t evt_off = strlen(evt_detail);
    for (int ev = 0; ev <= 0xce; ev++) {
        uintptr_t head = 0;
        memcpy(&head, (void*)(evt_base + ev * 8), 8);
        if (!head) continue;
        populated++;
        /* Count chain length by following a 'next' pointer at offset +0x10
         * (guessed from sub_9c11d0's linked-list insertion pattern). */
        int chain = 0;
        uintptr_t cur = head;
        for (int step = 0; step < 32 && cur; step++) {
            chain++;
            total_handlers++;
            uintptr_t next = 0;
            memcpy(&next, (void*)(cur + 0x10), 8);
            cur = next;
        }
        size_t space_left = sizeof(evt_detail) - evt_off;
        if (space_left > 128) {
            int added = snprintf(evt_detail + evt_off, space_left,
                "  event 0x%02x: head=0x%lx  chain_len=%d\n",
                ev, (unsigned long)head, chain);
            evt_off += (size_t)added;
        }
    }
    u = write(STDERR_FILENO, evt_detail, evt_off); (void)u;
    char esum[128];
    int esl = snprintf(esum, sizeof(esum),
        "  event registry summary: %d/207 slots populated, %d total handlers\n",
        populated, total_handlers);
    u = write(STDERR_FILENO, esum, (size_t)esl); (void)u;

    /* --- update-type dispatch at data_48c52c0 (5 slots) --- */
    uintptr_t upd_base = game_base + (0x48c52c0 - 0x400000);
    char upd_hdr[128];
    int uhl = snprintf(upd_hdr, sizeof(upd_hdr),
        "\n--- update-type dispatch @ data_48c52c0 (5 slots) ---\n");
    u = write(STDERR_FILENO, upd_hdr, (size_t)uhl); (void)u;
    for (int ut = 0; ut < 5; ut++) {
        uintptr_t head = 0;
        memcpy(&head, (void*)(upd_base + ut * 8), 8);
        char line[128];
        int lnn = snprintf(line, sizeof(line),
            "  update_type %d: head=0x%lx\n", ut, (unsigned long)head);
        u = write(STDERR_FILENO, line, (size_t)lnn); (void)u;
    }
}

/* ========== moved from spi_shim.c (1358-1444): diagnostics: SIGSEGV handler + SIGTERM/INT/USR handler ========== */
void shim_sigsegv_handler(int sig, siginfo_t *info, void *ucontext) {
    (void)ucontext;

    /* Event sweep recovery path: if a SEGV fires while we're inside
     * shim_fire_event(), log the failure and longjmp back to the sweep
     * loop so we can continue with the next event instead of aborting. */
    if (shim_event_fire_in_progress) {
        char msg[192];
        int mn = snprintf(msg, sizeof(msg),
            "\n  [sweep] event 0x%02x CRASHED sig=%d fault=%p — recovering\n",
            shim_current_sweep_event_id, sig, info->si_addr);
        ssize_t u2 = write(STDERR_FILENO, msg, (size_t)mn); (void)u2;
        shim_event_fire_in_progress = 0;
        siglongjmp(shim_event_sweep_jmp, 1);
    }

    char hdr[256];
    int hn = snprintf(hdr, sizeof(hdr),
        "\n=== SHIM SIGSEGV: sig=%d si_code=%d fault_addr=%p ===\n",
        sig, info->si_code, info->si_addr);
    ssize_t u = write(STDERR_FILENO, hdr, (size_t)hn);
    (void)u;

    void *frames[64];
    int n = backtrace(frames, 64);
    for (int i = 0; i < n; i++) {
        Dl_info di;
        char line[512];
        int ln;
        if (dladdr(frames[i], &di) && di.dli_fname) {
            unsigned long off =
                (unsigned long)((uintptr_t)frames[i] - (uintptr_t)di.dli_fbase);
            ln = snprintf(line, sizeof(line),
                "[%02d] %p  %s+0x%lx  %s\n",
                i, frames[i], di.dli_fname, off,
                di.dli_sname ? di.dli_sname : "");
        } else {
            ln = snprintf(line, sizeof(line),
                "[%02d] %p  (no dladdr)\n", i, frames[i]);
        }
        u = write(STDERR_FILENO, line, (size_t)ln);
    }

    /* Dump sound-system globals for offline inspection (Step 42). */
    shim_dump_sound_globals();

    /* Dump the 42 cmode objects from _INIT_74 (Step 50). */
    shim_dump_cmodes();

    /* Dump dispatch tables: cmode registry, event handlers, update types (Step 51). */
    shim_dump_dispatch_tables();

    /* Chain to default handler so QEMU's "uncaught target signal 11"
     * + core dump still happen (unchanged external behaviour). */
    signal(sig, SIG_DFL);
    raise(sig);
}

/* SIGABRT handler — catches __cxa_pure_virtual / std::terminate paths.
 *
 * When a pure-virtual call fires (e.g. during a destructor on a partially-
 * constructed object), libstdc++'s default terminate handler calls abort()
 * which raises SIGABRT. Without this handler, qemu just prints
 * "uncaught target signal 6" and we get no information about WHERE the
 * abort happened — useless for diagnosing dtor-order bugs in the
 * VideoPlayerSpi chain.
 *
 * The handler does the minimum: dump the backtrace with dladdr resolution
 * AND an ELF VA for any frame inside the game binary (so we can map the
 * address directly to a BN function without subtracting the load base by
 * hand). For game-binary frames the ELF VA can be plugged directly into
 * mcp__binary-ninja__function_at to identify the offending call site.
 *
 * No sound-globals / cmode / dispatch dumps — those are heavy and
 * irrelevant for a pure-virtual abort. The stack trace is the prize.
 *
 * Re-raises with SIG_DFL so the process still terminates with exit code 134
 * and qemu's behavior is preserved. */
void shim_sigabrt_handler(int sig, siginfo_t *info, void *ucontext) {
    (void)ucontext;

    char hdr[256];
    int hn = snprintf(hdr, sizeof(hdr),
        "\n=== SHIM SIGABRT: sig=%d si_code=%d ===\n"
        "(__cxa_pure_virtual / std::terminate path — backtrace follows)\n",
        sig, info ? info->si_code : 0);
    ssize_t u = write(STDERR_FILENO, hdr, (size_t)hn);
    (void)u;

    uintptr_t game_base = shim_resolve_game_base();
    char base_line[192];
    int bln;
    if (game_base) {
        bln = snprintf(base_line, sizeof(base_line),
            "(game load base = 0x%lx; ELF_VA shown for game frames — "
            "feed directly to BN function_at)\n", game_base);
    } else {
        bln = snprintf(base_line, sizeof(base_line),
            "(game load base unresolved — could not compute ELF VAs)\n");
    }
    u = write(STDERR_FILENO, base_line, (size_t)bln);

    void *frames[64];
    int n = backtrace(frames, 64);
    for (int i = 0; i < n; i++) {
        Dl_info di;
        char line[640];
        int ln;
        if (dladdr(frames[i], &di) && di.dli_fname) {
            unsigned long off_module =
                (unsigned long)((uintptr_t)frames[i] - (uintptr_t)di.dli_fbase);
            int is_game = strstr(di.dli_fname, "/game") &&
                          !strstr(di.dli_fname, ".so");
            unsigned long elf_va = is_game && game_base
                ? (unsigned long)((uintptr_t)frames[i] - game_base) : 0;
            if (elf_va) {
                ln = snprintf(line, sizeof(line),
                    "[%02d] %p  %s+0x%lx  %s  ELF_VA=0x%lx\n",
                    i, frames[i], di.dli_fname, off_module,
                    di.dli_sname ? di.dli_sname : "(no symbol)",
                    elf_va);
            } else {
                ln = snprintf(line, sizeof(line),
                    "[%02d] %p  %s+0x%lx  %s\n",
                    i, frames[i], di.dli_fname, off_module,
                    di.dli_sname ? di.dli_sname : "(no symbol)");
            }
        } else {
            ln = snprintf(line, sizeof(line),
                "[%02d] %p  (no dladdr)\n", i, frames[i]);
        }
        u = write(STDERR_FILENO, line, (size_t)ln);
    }

    /* Chain to default so the process dies with exit code 134. */
    signal(sig, SIG_DFL);
    raise(sig);
}

/* SIGTERM / SIGUSR1 / SIGUSR2 handler:
 *   SIGTERM/SIGINT — dump state and exit cleanly.
 *   SIGUSR1        — dump state and keep running.
 *   SIGUSR2        — run event sweep (fire events 0x00..0xce) and keep running.
 *
 * Lets us extract memory state when `timeout` kills QEMU (SIGTERM)
 * or when the user triggers a manual dump via `kill -USR1 <pid>` /
 * a sweep via `kill -USR2 <pid>`. */
void shim_term_dump_handler(int sig) {
    if (sig == SIGUSR2) {
        const char *m = "\n=== SHIM SIGUSR2: RUNNING EVENT SWEEP ===\n";
        ssize_t u = write(STDERR_FILENO, m, strlen(m)); (void)u;
        shim_event_sweep();
        return;
    }

    char hdr[128];
    int hn = snprintf(hdr, sizeof(hdr),
        "\n=== SHIM SIGTERM/USR1 DUMP (sig=%d) ===\n", sig);
    ssize_t u = write(STDERR_FILENO, hdr, (size_t)hn);
    (void)u;
    shim_dump_sound_globals();
    shim_dump_cmodes();
    shim_dump_dispatch_tables();
    /* For SIGTERM, exit cleanly; for SIGUSR1, keep running. */
    if (sig == SIGTERM || sig == SIGINT) {
        _exit(0);
    }
}

