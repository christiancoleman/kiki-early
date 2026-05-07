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
            discovery_log_fd = 2;
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
#define DISCOVERY_ADDR_QUEUE_SCENE    0x731930  /* sub_731930 — wrapper that calls select_and_load_scene; trap here to find its caller */
#define DISCOVERY_ADDR_ASSET_PTR      0x7310f4  /* inside select_and_load_scene, instruction right after bl get_asset_ptr_by_packed_id (str x0, [sp]); x0 = bytecode ptr, x19 = asset_id, x26 = scene_id */

/* Expected first instructions (aarch64) to ensure binary compatibility */
#define EXPECTED_GODOT_ERR      0xa9bb7bfdu  /* stp fp, lr, [sp, #-0x50]! */
#define EXPECTED_NODEBUS_INIT   0xd001e6e2u  /* adrp x2, 0x4353000 */
#define EXPECTED_ASSET_LOAD     0xb001e102u  /* adrp x2, 0x4353000 */
#define EXPECTED_NODE_QUERY     0x9001d302u  /* adrp x2, 0x4353000 */
#define EXPECTED_POLL_SPAWN     0xa9bf7bf3u  /* stp x19, lr, [sp, #-0x10]! */
#define EXPECTED_SCENE_LOAD     0xf001e108u  /* adrp x8, 0x4353000 */
#define EXPECTED_QUEUE_SCENE    0xb001ecc8u  /* adrp x8, 0x44ca000 */
#define EXPECTED_ASSET_PTR      0xf90003e0u  /* str x0, [sp] */

#define AARCH64_BRK_0           0xd4200000u

static uintptr_t runtime_godot_err = 0;
static uintptr_t runtime_nodebus_init = 0;
static uintptr_t runtime_asset_load = 0;
static uintptr_t runtime_node_query = 0;
static uintptr_t runtime_poll_spawn = 0;
static uintptr_t runtime_scene_load = 0;
static uintptr_t runtime_queue_scene = 0;
static uintptr_t runtime_asset_ptr = 0;

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
    patch_point(base, DISCOVERY_ADDR_QUEUE_SCENE,  EXPECTED_QUEUE_SCENE,  &runtime_queue_scene,  "queue_scene");
    patch_point(base, DISCOVERY_ADDR_ASSET_PTR,    EXPECTED_ASSET_PTR,    &runtime_asset_ptr,    "asset_ptr");
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

    if (pc == runtime_asset_ptr) {
        /* Right after `bl get_asset_ptr_by_packed_id` inside select_and_load_scene.
         * x0 = bytecode pointer, x19 = packed_id (asset_id), x26 = scene_id.
         * Dedup: only log first occurrence per (scene_id) so we don't spam. */
        static int seen_mask[1024] = {0};
        void *bytecode = (void *)mc->regs[0];
        int32_t asset_id = (int32_t)mc->regs[19];
        int scene_id = (int)mc->regs[26];
        if (scene_id >= 0 && scene_id < 1024 && !seen_mask[scene_id]) {
            seen_mask[scene_id] = 1;
            if (bytecode) {
                discovery_log("scene_bytecode_BEGIN(scene_id=%d, asset_id=%d, ptr=%p)", scene_id, asset_id, bytecode);
                for (int chunk = 0; chunk < 4; chunk++) {
                    char hex[260] = {0};
                    char ascii[80] = {0};
                    int base = chunk * 64;
                    for (int i = 0; i < 64; i++) {
                        unsigned char b = ((unsigned char *)bytecode)[base + i];
                        snprintf(hex + i*3, 4, "%02x ", b);
                        ascii[i] = (b >= 32 && b < 127) ? b : '.';
                    }
                    discovery_log("  +%03x: %s | %s", base, hex, ascii);
                }
                discovery_log("scene_bytecode_END(scene_id=%d)", scene_id);
            } else {
                discovery_log("scene_bytecode(scene_id=%d, asset_id=%d) bytecode=NULL", scene_id, asset_id);
            }
        }
        /* Emulate str x0, [sp] */
        *(volatile uint64_t *)mc->sp = mc->regs[0];
        mc->pc += 4;
        return 1;
    }

    if (pc == runtime_queue_scene) {
        int scene_id = (int)mc->regs[0];
        uintptr_t lr = (uintptr_t)mc->regs[30];
        long caller_va = (long)(lr - game_base);
        discovery_log("queue_scene_w_731930(scene_id=%d) caller_va=0x%lx", scene_id, caller_va);
        /* Emulate adrp x8, 0x44ca000 */
        mc->regs[8] = (game_base + 0x44ca000);
        mc->pc += 4;
        return 1;
    }

    if (pc == runtime_scene_load) {
        int scene_id = (int)mc->regs[0];
        /* lr at function entry is the caller's return address — the instruction
         * right after their `bl select_and_load_scene`. Subtracting the runtime
         * game_base gives us the BN VA of the caller's site. */
        uintptr_t lr = (uintptr_t)mc->regs[30];
        long caller_va = (long)(lr - game_base);
        const uintptr_t TABLE_VA = 0x43fcf10;
        const uintptr_t STRIDE   = 32;
        int32_t aid = -1;
        if (scene_id > 0 && scene_id < 0x375) {
            uintptr_t entry_addr = game_base + TABLE_VA + (uintptr_t)scene_id * STRIDE;
            void *array_ptr = *(void **)entry_addr;
            if (array_ptr) aid = *(int32_t *)array_ptr;
        }
        discovery_log("select_and_load_scene(scene_id=%d, asset_id=%d) caller_va=0x%lx",
                      scene_id, aid, caller_va);
        /* Emulate adrp x8, 0x4353000 */
        mc->regs[8] = (game_base + 0x4353000);
        mc->pc += 4;
        return 1;
    }

    return 0;
}
