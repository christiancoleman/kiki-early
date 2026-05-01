#define _GNU_SOURCE
#include "spi_shim.h"
#include "shim_discovery.h"
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <stdio.h>

static int discovery_log_fd = -1;

void discovery_log(const char *fmt, ...) {
    if (__builtin_expect(discovery_log_fd < 0, 0)) {
        /* CWD when the game runs is games/pokemon_le/ (set by run_game.sh
         * via `cd "$GAME_DIR"`), so this relative path lands the log
         * alongside game.log / audio.log / viewer.log in the standard
         * runs/latest/ directory. */
        /* O_TRUNC: each run starts with a fresh log instead of growing
         * across runs. The first call from this process wipes whatever
         * was left from the previous run. */
        discovery_log_fd = open("_emulation/runs/latest/discovery.log",
                                O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (discovery_log_fd < 0) {
            discovery_log_fd = 2; /* stderr fallback */
        }
    }

    char ts[32];
    shim_format_log_timestamp(ts, sizeof(ts));

    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    char line[2048];
    int ln = snprintf(line, sizeof(line), "%s[discovery] %s\n", ts, msg);
    if (ln > 0) {
        (void)write(discovery_log_fd, line, (size_t)ln);
    }
}

/* ELF VAs from Binary Ninja analysis */
#define DISCOVERY_ADDR_GODOT_ERR      0x32e4f30
#define DISCOVERY_ADDR_NODEBUS_INIT   0x675110
#define DISCOVERY_ADDR_ASSET_LOAD     0x7321d0
#define DISCOVERY_ADDR_NODE_QUERY     0x8f38e0
#define DISCOVERY_ADDR_POLL_SPAWN     0x675d10
#define DISCOVERY_ADDR_SCENE_LOAD     0x730fb0

/* Expected first instructions (aarch64) to ensure binary compatibility */
#define EXPECTED_GODOT_ERR      0xa9bb7bfdu  /* stp fp, lr, [sp, #-0x50]! */
#define EXPECTED_NODEBUS_INIT   0xd001e6e2u  /* adrp x2, 0x4353000 */
#define EXPECTED_ASSET_LOAD     0xb001e102u  /* adrp x2, 0x4353000 */
#define EXPECTED_NODE_QUERY     0x9001d302u  /* adrp x2, 0x4353000 */
#define EXPECTED_POLL_SPAWN     0xa9bf7bf3u  /* stp x19, lr, [sp, #-0x10]! */
#define EXPECTED_SCENE_LOAD     0xf001e108u  /* adrp x8, 0x4353000 */

#define AARCH64_BRK_0           0xd4200000u

static uintptr_t runtime_godot_err = 0;
static uintptr_t runtime_nodebus_init = 0;
static uintptr_t runtime_asset_load = 0;
static uintptr_t runtime_node_query = 0;
static uintptr_t runtime_poll_spawn = 0;
static uintptr_t runtime_scene_load = 0;

static void patch_point(uintptr_t base, uintptr_t offset, uint32_t expected, uintptr_t *runtime_out, const char *name) {
    uint32_t *instr = (uint32_t *)(base + offset);
    uintptr_t page = (uintptr_t)instr & ~(uintptr_t)4095;
    if (mprotect((void *)page, 8192, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        SHIM_LOG("[discovery] mprotect RWX @ %p failed: %s", (void *)page, strerror(errno));
        return;
    }

    uint32_t old = *instr;
    if (old == AARCH64_BRK_0) {
        *runtime_out = (uintptr_t)instr;
        return;
    }
    if (old != expected) {
        SHIM_LOG("[discovery] REFUSED to patch %s @ %p: expected 0x%08x, got 0x%08x", name, (void *)instr, expected, old);
        return;
    }

    *runtime_out = (uintptr_t)instr;
    __sync_synchronize();
    *instr = AARCH64_BRK_0;
    __builtin___clear_cache((char *)instr, (char *)instr + 4);
    SHIM_LOG("[discovery] patched %s @ %p", name, (void *)instr);
}

void shim_discovery_init(void) {
    uintptr_t base = shim_resolve_game_base();
    if (!base) return;

    patch_point(base, DISCOVERY_ADDR_GODOT_ERR,    EXPECTED_GODOT_ERR,    &runtime_godot_err,    "godot_error");
    patch_point(base, DISCOVERY_ADDR_NODEBUS_INIT, EXPECTED_NODEBUS_INIT, &runtime_nodebus_init, "nodebus_init");
    patch_point(base, DISCOVERY_ADDR_ASSET_LOAD,   EXPECTED_ASSET_LOAD,   &runtime_asset_load,   "asset_load");
    patch_point(base, DISCOVERY_ADDR_NODE_QUERY,   EXPECTED_NODE_QUERY,   &runtime_node_query,   "node_query");
    patch_point(base, DISCOVERY_ADDR_POLL_SPAWN,   EXPECTED_POLL_SPAWN,   &runtime_poll_spawn,   "poll_spawn");
    patch_point(base, DISCOVERY_ADDR_SCENE_LOAD,   EXPECTED_SCENE_LOAD,   &runtime_scene_load,   "scene_load");
}

int shim_discovery_handler(uintptr_t pc, void *ucontext) {
    ucontext_t *ctx = (ucontext_t *)ucontext;
    mcontext_t *mc = &ctx->uc_mcontext;
    uintptr_t game_base = shim_resolve_game_base();

    if (pc == runtime_godot_err) {
        const char *p_func = (const char *)mc->regs[0];
        const char *p_file = (const char *)mc->regs[1];
        int p_line = (int)mc->regs[2];
        const char *p_desc = (const char *)mc->regs[3];
        const char *p_err  = (const char *)mc->regs[4];
        discovery_log("GODOT ERROR: %s (at %s:%d) [%s] - %s",
                      p_err ? p_err : "(null)", p_file ? p_file : "(null)", p_line,
                      p_func ? p_func : "(null)", p_desc ? p_desc : "(null)");
        
        /* Emulate stp fp, lr, [sp, #-0x50]! */
        uintptr_t new_sp = (uintptr_t)mc->sp - 0x50;
        *(volatile uint64_t *)(new_sp + 0) = mc->regs[29]; /* fp */
        *(volatile uint64_t *)(new_sp + 8) = mc->regs[30]; /* lr */
        mc->sp = new_sp;
        mc->pc += 4;
        return 1;
    }

    if (pc == runtime_nodebus_init) {
        discovery_log("mcp_nodebus_init_orchestrator entered");
        /* Emulate adrp x2, 0x4353000 */
        mc->regs[2] = (game_base + 0x4353000);
        mc->pc += 4;
        return 1;
    }

    if (pc == runtime_asset_load) {
        discovery_log("load_scene_assets(scene_id=%d)", (int)mc->regs[0]);
        /* Emulate adrp x2, 0x4353000 */
        mc->regs[2] = (game_base + 0x4353000);
        mc->pc += 4;
        return 1;
    }

    if (pc == runtime_node_query) {
        discovery_log("mcp_node_query_status_and_classify(slot=%d)", (int)mc->regs[0]);
        /* Emulate adrp x2, 0x4353000 */
        mc->regs[2] = (game_base + 0x4353000);
        mc->pc += 4;
        return 1;
    }

    if (pc == runtime_poll_spawn) {
        discovery_log("sub_675d10 (Poll Thread Spawner) entered");
        /* Emulate stp x19, lr, [sp, #-0x10]! */
        uintptr_t new_sp = (uintptr_t)mc->sp - 0x10;
        *(volatile uint64_t *)(new_sp + 0) = mc->regs[19];
        *(volatile uint64_t *)(new_sp + 8) = mc->regs[30];
        mc->sp = new_sp;
        mc->pc += 4;
        return 1;
    }

    if (pc == runtime_scene_load) {
        discovery_log("select_and_load_scene(scene_id=%d)", (int)mc->regs[0]);
        /* Emulate adrp x8, 0x4353000 */
        mc->regs[8] = (game_base + 0x4353000);
        mc->pc += 4;
        return 1;
    }

    return 0;
}
