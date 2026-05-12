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
#define START_TRACE_EVENT6E_BRANCH    0x661c78
#define START_TRACE_DISPLAY_BRANCH    0x661c80
#define START_TRACE_BALL_BRANCH       0x661c8c
#define START_TRACE_START_ENTRY       0x661120
#define START_TRACE_OUTER_5A3170      0x661e90
#define START_TRACE_OUTER_PROC31      0x661eac
#define START_TRACE_OUTER_758F20      0x661eb4
#define START_TRACE_OUTER_GATE2       0x661ec0
#define START_TRACE_OUTER_GATE3       0x661ecc
#define START_TRACE_OUTER_GATE4       0x661ed8
#define START_TRACE_ENTER_MAIN        0x661ee8
#define START_TRACE_MODE100_BRANCH    0x661a20
#define START_TRACE_MODE40_BRANCH     0x661a24
#define START_TRACE_QUEUED49_BRANCH   0x661a60
#define START_TRACE_QUEUED46_BRANCH   0x661a70
#define START_TRACE_START_VALUES      0x661c48
#define START_TRACE_MODE10_BRANCH     0x661ab8
#define START_TRACE_READY_BRANCH      0x661b58
#define START_TRACE_HAS_CREDIT_BRANCH 0x661b64
#define START_TRACE_BVAR10_BRANCH     0x661bc8
#define START_TRACE_PROC32_BRANCH     0x661bdc
#define START_TRACE_PROC33_BRANCH     0x661bf0
#define START_TRACE_GATE13_BRANCH     0x661cb8
#define START_TRACE_REAL_CURRENT      0x661154
#define START_TRACE_REAL_LOCAL4A      0x661158
#define START_TRACE_REAL_CREDIT_CMP   0x66117c
#define START_TRACE_REAL_EVENT4A      0x66118c
#define START_TRACE_READY_EVENT6D     0x73a604
#define START_TRACE_READY_RELATION    0x73a624
#define START_TRACE_READY_PROC32      0x73a630
#define MBALL_TRACE_FIRE32_RETURN     0x846184
#define MBALL_TRACE_QUERY32_BRANCH    0x7369d0
#define MBALL_TRACE_QUERY_BALL_COUNT  0x7369e0
#define MBALL_TRACE_CLEAR_ENTRY       0x72a950
#define MBALL_TRACE_CLEAR_HIT         0x72a9d4
#define BALL_TRACE_COUNT_ENTRY        0x7366c0

/* Expected first instructions (aarch64) to ensure binary compatibility */
#define EXPECTED_GODOT_ERR      0xa9bb7bfdu  /* stp fp, lr, [sp, #-0x50]! */
#define EXPECTED_NODEBUS_INIT   0xd001e6e2u  /* adrp x2, 0x4353000 */
#define EXPECTED_ASSET_LOAD     0xb001e102u  /* adrp x2, 0x4353000 */
#define EXPECTED_NODE_QUERY     0x9001d302u  /* adrp x2, 0x4353000 */
#define EXPECTED_POLL_SPAWN     0xa9bf7bf3u  /* stp x19, lr, [sp, #-0x10]! */
#define EXPECTED_SCENE_LOAD     0xf001e108u  /* adrp x8, 0x4353000 */
#define EXPECTED_QUEUE_SCENE    0xb001ecc8u  /* adrp x8, 0x44ca000 */
#define EXPECTED_ASSET_PTR      0xf90003e0u  /* str x0, [sp] */
#define EXPECTED_START_EVENT6E_BRANCH 0x360000c0u  /* tbz w0,#0,0x661c90 */
#define EXPECTED_START_DISPLAY_BRANCH 0x3707f2c0u  /* tbnz w0,#0,0x661ad8 */
#define EXPECTED_START_BALL_BRANCH    0x54fff260u  /* b.eq 0x661ad8 */
#define EXPECTED_START_ENTRY          0xa9bc53f3u  /* stp x19,x20,[sp,#-64]! */
#define EXPECTED_START_OUTER_5A3170   0x37000060u  /* tbnz w0,#0,0x661e9c */
#define EXPECTED_START_OUTER_PROC31   0x36000060u  /* tbz w0,#0,0x661eb8 */
#define EXPECTED_START_OUTER_758F20   0x370001c0u  /* tbnz w0,#0,0x661eec */
#define EXPECTED_START_OUTER_GATE2    0x3607fea0u  /* tbz w0,#0,0x661e94 */
#define EXPECTED_START_OUTER_GATE3    0x3707fe40u  /* tbnz w0,#0,0x661e94 */
#define EXPECTED_START_OUTER_GATE4    0x3707fde0u  /* tbnz w0,#0,0x661e94 */
#define EXPECTED_START_ENTER_MAIN     0x17fffebau  /* b 0x6619d0 */
#define EXPECTED_START_MODE100_BRANCH    0x54000601u  /* b.ne 0x661ae0 */
#define EXPECTED_START_MODE40_BRANCH     0x363001b4u  /* tbz w20,#6,0x661a58 */
#define EXPECTED_START_QUEUED49_BRANCH   0x3707fe40u  /* tbnz w0,#0,0x661a28 */
#define EXPECTED_START_QUEUED46_BRANCH   0x3707fdc0u  /* tbnz w0,#0,0x661a28 */
#define EXPECTED_START_VALUES            0x12001c17u  /* and w23,w0,#0xff */
#define EXPECTED_START_MODE10_BRANCH     0x372004f4u  /* tbnz w20,#4,0x661b54 */
#define EXPECTED_START_READY_BRANCH      0x3607fc00u  /* tbz w0,#0,0x661ad8 */
#define EXPECTED_START_HAS_CREDIT_BRANCH 0x54000321u  /* b.ne 0x661bc8 */
#define EXPECTED_START_BVAR10_BRANCH     0x360000d7u  /* tbz w23,#0,0x661be0 */
#define EXPECTED_START_PROC32_BRANCH     0x370006a0u  /* tbnz w0,#0,0x661cb0 */
#define EXPECTED_START_PROC33_BRANCH     0x36000300u  /* tbz w0,#0,0x661c50 */
#define EXPECTED_START_GATE13_BRANCH     0x3607f940u  /* tbz w0,#0,0x661be0 */
#define EXPECTED_START_REAL_CURRENT      0x37000160u  /* tbnz w0,#0,0x661180 */
#define EXPECTED_START_REAL_LOCAL4A      0x350000d5u  /* cbnz w21,0x661170 */
#define EXPECTED_START_REAL_CREDIT_CMP   0x54ffff08u  /* b.hi 0x66115c */
#define EXPECTED_START_REAL_EVENT4A      0x3607fe80u  /* tbz w0,#0,0x66115c */
#define EXPECTED_START_READY_EVENT6D     0x37000080u  /* tbnz w0,#0,0x73a614 */
#define EXPECTED_START_READY_RELATION    0x3707ff20u  /* tbnz w0,#0,0x73a608 */
#define EXPECTED_START_READY_PROC32      0x12001c01u  /* and w1,w0,#0xff */
#define EXPECTED_MBALL_FIRE32_RETURN     0x79401340u  /* ldrh w0,[x26,#8] after fire_event(0x32, payload) */
#define EXPECTED_MBALL_QUERY32_BRANCH    0x36000060u  /* tbz w0,#0,0x7369dc */
#define EXPECTED_MBALL_QUERY_BALL_COUNT  0xd001e0c1u  /* adrp x1,0x4350000 after balls-in-game count */
#define EXPECTED_MBALL_CLEAR_ENTRY       0xa9bd53f3u  /* stp x19,x20,[sp,#-48]! */
#define EXPECTED_MBALL_CLEAR_HIT         0xaa1303e0u  /* mov x0,x19 before clearing active entry */
#define EXPECTED_BALL_COUNT_ENTRY        0xd001e0c0u  /* adrp x0,0x4350000 */

#define AARCH64_BRK_0           0xd4200000u

static uintptr_t runtime_godot_err = 0;
static uintptr_t runtime_nodebus_init = 0;
static uintptr_t runtime_asset_load = 0;
static uintptr_t runtime_node_query = 0;
static uintptr_t runtime_poll_spawn = 0;
static uintptr_t runtime_scene_load = 0;
static uintptr_t runtime_queue_scene = 0;
static uintptr_t runtime_asset_ptr = 0;
static uintptr_t runtime_start_event6e_branch = 0;
static uintptr_t runtime_start_display_branch = 0;
static uintptr_t runtime_start_ball_branch = 0;
static uintptr_t runtime_start_entry = 0;
static uintptr_t runtime_start_outer_5a3170 = 0;
static uintptr_t runtime_start_outer_proc31 = 0;
static uintptr_t runtime_start_outer_758f20 = 0;
static uintptr_t runtime_start_outer_gate2 = 0;
static uintptr_t runtime_start_outer_gate3 = 0;
static uintptr_t runtime_start_outer_gate4 = 0;
static uintptr_t runtime_start_enter_main = 0;
static uintptr_t runtime_start_mode100_branch = 0;
static uintptr_t runtime_start_mode40_branch = 0;
static uintptr_t runtime_start_queued49_branch = 0;
static uintptr_t runtime_start_queued46_branch = 0;
static uintptr_t runtime_start_values = 0;
static uintptr_t runtime_start_mode10_branch = 0;
static uintptr_t runtime_start_ready_branch = 0;
static uintptr_t runtime_start_has_credit_branch = 0;
static uintptr_t runtime_start_bvar10_branch = 0;
static uintptr_t runtime_start_proc32_branch = 0;
static uintptr_t runtime_start_proc33_branch = 0;
static uintptr_t runtime_start_gate13_branch = 0;
static uintptr_t runtime_start_real_current = 0;
static uintptr_t runtime_start_real_local4a = 0;
static uintptr_t runtime_start_real_credit_cmp = 0;
static uintptr_t runtime_start_real_event4a = 0;
static uintptr_t runtime_start_ready_event6d = 0;
static uintptr_t runtime_start_ready_relation = 0;
static uintptr_t runtime_start_ready_proc32 = 0;
static uintptr_t runtime_mball_fire32_return = 0;
static uintptr_t runtime_mball_query32_branch = 0;
static uintptr_t runtime_mball_query_ball_count = 0;
static uintptr_t runtime_mball_clear_entry = 0;
static uintptr_t runtime_mball_clear_hit = 0;
static uintptr_t runtime_ball_count_entry = 0;

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

static int pstate_z(const mcontext_t *mc) {
    return (int)((mc->pstate >> 30) & 1);
}

static int pstate_c(const mcontext_t *mc) {
    return (int)((mc->pstate >> 29) & 1);
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
    patch_point(base, START_TRACE_EVENT6E_BRANCH,  EXPECTED_START_EVENT6E_BRANCH, &runtime_start_event6e_branch, "start_event6e_branch");
    patch_point(base, START_TRACE_DISPLAY_BRANCH,  EXPECTED_START_DISPLAY_BRANCH, &runtime_start_display_branch, "start_display_branch");
    patch_point(base, START_TRACE_BALL_BRANCH,     EXPECTED_START_BALL_BRANCH,    &runtime_start_ball_branch,    "start_ball_branch");
    patch_point(base, START_TRACE_START_ENTRY,     EXPECTED_START_ENTRY,          &runtime_start_entry,          "start_entry");
    patch_point(base, START_TRACE_OUTER_5A3170,    EXPECTED_START_OUTER_5A3170,   &runtime_start_outer_5a3170,   "start_outer_5a3170");
    patch_point(base, START_TRACE_OUTER_PROC31,    EXPECTED_START_OUTER_PROC31,   &runtime_start_outer_proc31,   "start_outer_proc31");
    patch_point(base, START_TRACE_OUTER_758F20,    EXPECTED_START_OUTER_758F20,   &runtime_start_outer_758f20,   "start_outer_758f20");
    patch_point(base, START_TRACE_OUTER_GATE2,     EXPECTED_START_OUTER_GATE2,    &runtime_start_outer_gate2,    "start_outer_gate2");
    patch_point(base, START_TRACE_OUTER_GATE3,     EXPECTED_START_OUTER_GATE3,    &runtime_start_outer_gate3,    "start_outer_gate3");
    patch_point(base, START_TRACE_OUTER_GATE4,     EXPECTED_START_OUTER_GATE4,    &runtime_start_outer_gate4,    "start_outer_gate4");
    patch_point(base, START_TRACE_ENTER_MAIN,      EXPECTED_START_ENTER_MAIN,     &runtime_start_enter_main,     "start_enter_main");
    patch_point(base, START_TRACE_MODE100_BRANCH,  EXPECTED_START_MODE100_BRANCH, &runtime_start_mode100_branch, "start_mode100_branch");
    patch_point(base, START_TRACE_MODE40_BRANCH,   EXPECTED_START_MODE40_BRANCH,  &runtime_start_mode40_branch,  "start_mode40_branch");
    patch_point(base, START_TRACE_QUEUED49_BRANCH, EXPECTED_START_QUEUED49_BRANCH, &runtime_start_queued49_branch, "start_queued49_branch");
    patch_point(base, START_TRACE_QUEUED46_BRANCH, EXPECTED_START_QUEUED46_BRANCH, &runtime_start_queued46_branch, "start_queued46_branch");
    patch_point(base, START_TRACE_START_VALUES,    EXPECTED_START_VALUES,         &runtime_start_values,         "start_values");
    patch_point(base, START_TRACE_MODE10_BRANCH,   EXPECTED_START_MODE10_BRANCH,  &runtime_start_mode10_branch,  "start_mode10_branch");
    patch_point(base, START_TRACE_READY_BRANCH,    EXPECTED_START_READY_BRANCH,   &runtime_start_ready_branch,   "start_ready_branch");
    patch_point(base, START_TRACE_HAS_CREDIT_BRANCH, EXPECTED_START_HAS_CREDIT_BRANCH, &runtime_start_has_credit_branch, "start_has_credit_branch");
    patch_point(base, START_TRACE_BVAR10_BRANCH,   EXPECTED_START_BVAR10_BRANCH,  &runtime_start_bvar10_branch,  "start_bvar10_branch");
    patch_point(base, START_TRACE_PROC32_BRANCH,   EXPECTED_START_PROC32_BRANCH,  &runtime_start_proc32_branch,  "start_proc32_branch");
    patch_point(base, START_TRACE_PROC33_BRANCH,   EXPECTED_START_PROC33_BRANCH,  &runtime_start_proc33_branch,  "start_proc33_branch");
    patch_point(base, START_TRACE_GATE13_BRANCH,   EXPECTED_START_GATE13_BRANCH,  &runtime_start_gate13_branch,  "start_gate13_branch");
    patch_point(base, START_TRACE_REAL_CURRENT,    EXPECTED_START_REAL_CURRENT,   &runtime_start_real_current,   "start_real_current");
    patch_point(base, START_TRACE_REAL_LOCAL4A,    EXPECTED_START_REAL_LOCAL4A,   &runtime_start_real_local4a,   "start_real_local4a");
    patch_point(base, START_TRACE_REAL_CREDIT_CMP, EXPECTED_START_REAL_CREDIT_CMP, &runtime_start_real_credit_cmp, "start_real_credit_cmp");
    patch_point(base, START_TRACE_REAL_EVENT4A,    EXPECTED_START_REAL_EVENT4A,   &runtime_start_real_event4a,   "start_real_event4a");
    patch_point(base, START_TRACE_READY_EVENT6D,   EXPECTED_START_READY_EVENT6D,  &runtime_start_ready_event6d,  "start_ready_event6d");
    patch_point(base, START_TRACE_READY_RELATION,  EXPECTED_START_READY_RELATION, &runtime_start_ready_relation, "start_ready_relation");
    patch_point(base, START_TRACE_READY_PROC32,    EXPECTED_START_READY_PROC32,   &runtime_start_ready_proc32,   "start_ready_proc32");
    patch_point(base, MBALL_TRACE_FIRE32_RETURN,   EXPECTED_MBALL_FIRE32_RETURN,  &runtime_mball_fire32_return,  "mball_fire32_return");
    patch_point(base, MBALL_TRACE_QUERY32_BRANCH,  EXPECTED_MBALL_QUERY32_BRANCH, &runtime_mball_query32_branch, "mball_query32_branch");
    patch_point(base, MBALL_TRACE_QUERY_BALL_COUNT, EXPECTED_MBALL_QUERY_BALL_COUNT, &runtime_mball_query_ball_count, "mball_query_ball_count");
    patch_point(base, MBALL_TRACE_CLEAR_ENTRY,     EXPECTED_MBALL_CLEAR_ENTRY,    &runtime_mball_clear_entry,    "mball_clear_entry");
    patch_point(base, MBALL_TRACE_CLEAR_HIT,       EXPECTED_MBALL_CLEAR_HIT,      &runtime_mball_clear_hit,      "mball_clear_hit");
    patch_point(base, BALL_TRACE_COUNT_ENTRY,      EXPECTED_BALL_COUNT_ENTRY,     &runtime_ball_count_entry,     "ball_count_entry");
}

int shim_discovery_handler(uintptr_t pc, void *ucontext) {
    ucontext_t *ctx = (ucontext_t *)ucontext;
    mcontext_t *mc = &ctx->uc_mcontext;
    uintptr_t game_base = shim_resolve_game_base();

    if (pc == runtime_start_event6e_branch) {
        int event6e_ret = (int)(mc->regs[0] & 1);
        uintptr_t target = game_base + 0x661c90;
        discovery_log("start_trace event6e branch: ret_bit=%d -> %s",
                      event6e_ret,
                      event6e_ret ? "continue to display gate" : "call start path");
        mc->pc = event6e_ret ? pc + 4 : target;
        return 1;
    }

    if (pc == runtime_start_outer_5a3170) {
        int ret_bit = (int)(mc->regs[0] & 1);
        uintptr_t target = game_base + 0x661e9c;
        discovery_log("start_trace outer 5a3170 gate: ret_bit=%d -> %s",
                      ret_bit,
                      ret_bit ? "continue toward start handler" : "abort before main start handler");
        mc->pc = ret_bit ? target : pc + 4;
        return 1;
    }

    if (pc == runtime_start_outer_proc31) {
        int proc31 = (int)(mc->regs[0] & 1);
        uintptr_t target = game_base + 0x661eb8;
        discovery_log("start_trace outer proc31 queued: ret_bit=%d -> %s",
                      proc31,
                      proc31 ? "check 758f20 reroute" : "continue to mode gates");
        mc->pc = proc31 ? pc + 4 : target;
        return 1;
    }

    if (pc == runtime_start_outer_758f20) {
        int reroute = (int)(mc->regs[0] & 1);
        uintptr_t target = game_base + 0x661eec;
        discovery_log("start_trace outer 758f20 reroute: ret_bit=%d -> %s",
                      reroute,
                      reroute ? "reroute to proc31 and abort start" : "continue to mode gates");
        mc->pc = reroute ? target : pc + 4;
        return 1;
    }

    if (pc == runtime_start_outer_gate2) {
        int gate2 = (int)(mc->regs[0] & 1);
        uintptr_t target = game_base + 0x661e94;
        discovery_log("start_trace outer mode gate2: ret_bit=%d -> %s",
                      gate2,
                      gate2 ? "continue to gate3" : "abort before main start handler");
        mc->pc = gate2 ? pc + 4 : target;
        return 1;
    }

    if (pc == runtime_start_outer_gate3) {
        int gate3 = (int)(mc->regs[0] & 1);
        uintptr_t target = game_base + 0x661e94;
        discovery_log("start_trace outer mode gate3: ret_bit=%d -> %s",
                      gate3,
                      gate3 ? "abort before main start handler" : "continue to gate4");
        mc->pc = gate3 ? target : pc + 4;
        return 1;
    }

    if (pc == runtime_start_outer_gate4) {
        int gate4 = (int)(mc->regs[0] & 1);
        uintptr_t target = game_base + 0x661e94;
        discovery_log("start_trace outer mode gate4: ret_bit=%d -> %s",
                      gate4,
                      gate4 ? "abort before main start handler" : "enter main start handler");
        mc->pc = gate4 ? target : pc + 4;
        return 1;
    }

    if (pc == runtime_start_enter_main) {
        discovery_log("start_trace outer wrapper entering main start handler sw_id=%lu",
                      (unsigned long)(mc->regs[0] & 0xffff));
        mc->pc = game_base + 0x6619d0;
        return 1;
    }

    if (pc == runtime_start_mode100_branch) {
        int z_set = pstate_z(mc);
        uintptr_t target = game_base + 0x661ae0;
        discovery_log("start_trace main mode100 branch: z=%d -> %s",
                      z_set,
                      z_set ? "continue in start handler" : "route to mode100 handler");
        mc->pc = z_set ? pc + 4 : target;
        return 1;
    }

    if (pc == runtime_start_mode40_branch) {
        int bit6 = (int)((mc->regs[20] >> 6) & 1);
        uintptr_t target = game_base + 0x661a58;
        discovery_log("start_trace main mode40 branch: mode_flags=0x%04lx bit6=%d -> %s",
                      (unsigned long)(mc->regs[20] & 0xffff),
                      bit6,
                      bit6 ? "abort start" : "continue to queued49 check");
        mc->pc = bit6 ? pc + 4 : target;
        return 1;
    }

    if (pc == runtime_start_queued49_branch) {
        int queued49 = (int)(mc->regs[0] & 1);
        uintptr_t target = game_base + 0x661a28;
        discovery_log("start_trace main queued49: ret_bit=%d -> %s",
                      queued49,
                      queued49 ? "abort start" : "continue to queued46 check");
        mc->pc = queued49 ? target : pc + 4;
        return 1;
    }

    if (pc == runtime_start_queued46_branch) {
        int queued46 = (int)(mc->regs[0] & 1);
        uintptr_t target = game_base + 0x661a28;
        discovery_log("start_trace main queued46: ret_bit=%d -> %s",
                      queued46,
                      queued46 ? "abort start" : "continue to start value derivation");
        mc->pc = queued46 ? target : pc + 4;
        return 1;
    }

    if (pc == runtime_start_values) {
        uint8_t local4a = *(uint8_t *)(mc->sp + 6);
        uint8_t local49 = *(uint8_t *)(mc->sp + 7);
        mc->regs[23] = mc->regs[0] & 0xff;
        discovery_log("start_trace values after 768790/768830: local4a=%u local49=%u bVar10=%lu bVar4=%lu mode_flags=0x%04lx",
                      local4a,
                      local49,
                      (unsigned long)(mc->regs[23] & 0xff),
                      (unsigned long)(mc->regs[22] & 0xff),
                      (unsigned long)(mc->regs[20] & 0xffff));
        mc->pc = pc + 4;
        return 1;
    }

    if (pc == runtime_start_mode10_branch) {
        int bit4 = (int)((mc->regs[20] >> 4) & 1);
        uintptr_t target = game_base + 0x661b54;
        discovery_log("start_trace main mode10 branch: mode_flags=0x%04lx bit4=%d -> %s",
                      (unsigned long)(mc->regs[20] & 0xffff),
                      bit4,
                      bit4 ? "use service/attract start path" : "use normal start path");
        mc->pc = bit4 ? target : pc + 4;
        return 1;
    }

    if (pc == runtime_start_ready_branch) {
        int ready_ret = (int)(mc->regs[0] & 1);
        uintptr_t target = game_base + 0x661ad8;
        discovery_log("start_trace readiness gate 73a5f0: ret_bit=%d -> %s",
                      ready_ret,
                      ready_ret ? "continue in main start handler" : "abort start");
        mc->pc = ready_ret ? pc + 4 : target;
        return 1;
    }

    if (pc == runtime_start_ready_event6d) {
        int event6d_ret = (int)(mc->regs[0] & 1);
        uintptr_t target = game_base + 0x73a614;
        discovery_log("start_trace 73a5f0 event6d: ret_bit=%d -> %s",
                      event6d_ret,
                      event6d_ret ? "check relation 0x35/0x44" : "readiness returns 1");
        mc->pc = event6d_ret ? target : pc + 4;
        return 1;
    }

    if (pc == runtime_start_ready_relation) {
        int relation_ret = (int)(mc->regs[0] & 1);
        uintptr_t target = game_base + 0x73a608;
        discovery_log("start_trace 73a5f0 relation 72a610(0x35,0x44): ret_bit=%d -> %s",
                      relation_ret,
                      relation_ret ? "readiness returns 0" : "check queued32");
        mc->pc = relation_ret ? target : pc + 4;
        return 1;
    }

    if (pc == runtime_start_ready_proc32) {
        int queued32 = (int)(mc->regs[0] & 1);
        mc->regs[1] = mc->regs[0] & 0xff;
        discovery_log("start_trace 73a5f0 queued32: ret_bit=%d -> readiness returns %d",
                      queued32,
                      queued32 ? 0 : 1);
        mc->pc = pc + 4;
        return 1;
    }

    if (pc == runtime_mball_fire32_return) {
        uint16_t mode_id = *(uint16_t *)(mc->sp + 8);
        uintptr_t payload_ptr = *(uintptr_t *)(mc->sp + 16);
        uintptr_t parent_lr = *(uintptr_t *)(mc->sp + 104);
        uint16_t active_mode = 0xffff;
        uint16_t mode_flags = 0xffff;
        uint8_t priority = 0xff;
        if (payload_ptr) {
            active_mode = *(uint16_t *)(payload_ptr + 0xf4);
            priority = *(uint8_t *)(payload_ptr + 0x100);
        }
        if (mc->regs[26]) {
            mode_flags = *(uint16_t *)(mc->regs[26] + 8);
        }
        discovery_log("mball_trace fire_event32 returned ret_bit=%lu mode_id=%u owner=%p table_entry=%p payload=%p active_mode=%u priority=%u mode_flags=0x%04x parent_lr=0x%lx",
                      (unsigned long)(mc->regs[0] & 1),
                      mode_id,
                      (void *)mc->regs[20],
                      (void *)mc->regs[26],
                      (void *)payload_ptr,
                      active_mode,
                      priority,
                      mode_flags,
                      (unsigned long)(parent_lr - game_base));
        mc->regs[0] = mode_flags;
        mc->pc = pc + 4;
        return 1;
    }

    if (pc == runtime_mball_query32_branch) {
        int queued32 = (int)(mc->regs[0] & 1);
        uintptr_t target = game_base + 0x7369dc;
        discovery_log("mball_trace sub_736988 queued32: ret_bit=%d -> %s",
                      queued32,
                      queued32 ? "return 0, multiball mode active" : "check balls-in-game");
        mc->pc = queued32 ? pc + 4 : target;
        return 1;
    }

    if (pc == runtime_mball_query_ball_count) {
        uint8_t balls_in_game = (uint8_t)(mc->regs[0] & 0xff);
        discovery_log("mball_trace sub_736988 balls_in_game=%u requested=%lu arg0=%p",
                      balls_in_game,
                      (unsigned long)(mc->regs[20] & 0xffffffffUL),
                      (void *)mc->regs[21]);
        mc->regs[1] = game_base + 0x4350000;
        mc->pc = pc + 4;
        return 1;
    }

    if (pc == runtime_mball_clear_entry) {
        uint16_t mode_id = (uint16_t)(mc->regs[0] & 0xffff);
        uintptr_t owner = mc->regs[1] ? mc->regs[1] : game_base + 0x4351a38;
        if (mode_id == 1 || mode_id == 6 || mode_id == 13 || mode_id == 0x32) {
            discovery_log("mball_trace clear_entry mode_id=%u owner=%p caller_lr=0x%lx",
                          mode_id,
                          (void *)owner,
                          (unsigned long)(mc->regs[30] - game_base));
        }
        uintptr_t new_sp = mc->sp - 48;
        uint64_t *saved = (uint64_t *)new_sp;
        saved[0] = mc->regs[19];
        saved[1] = mc->regs[20];
        mc->sp = new_sp;
        mc->pc = pc + 4;
        return 1;
    }

    if (pc == runtime_mball_clear_hit) {
        uint16_t active_mode = *(uint16_t *)(mc->regs[19] + 0xf4);
        uintptr_t active_owner = *(uintptr_t *)(mc->regs[19] + 0xf8);
        uint16_t flags = *(uint16_t *)(mc->regs[19] + 2);
        discovery_log("mball_trace clear_hit active_mode=%u owner=%p flags=0x%04x node=%p",
                      active_mode,
                      (void *)active_owner,
                      flags,
                      (void *)mc->regs[19]);
        mc->regs[0] = mc->regs[19];
        mc->pc = pc + 4;
        return 1;
    }

    if (pc == runtime_ball_count_entry) {
        uint8_t cached = *(uint8_t *)(game_base + 0x44ca740);
        uint8_t cached_check = *(uint8_t *)(game_base + 0x44ca741);
        uint16_t device_idx = *(uint16_t *)(game_base + 0x44ca714);
        uint8_t expected = *(uint8_t *)(game_base + 0x44ca716);
        uint8_t delta = *(uint8_t *)(game_base + 0x44ca717);
        uintptr_t override_ptr = *(uintptr_t *)(game_base + 0x4350d80);
        uint8_t override = override_ptr ? *(uint8_t *)override_ptr : 0xff;
        uintptr_t device_count_ptr = *(uintptr_t *)(game_base + 0x434fa38);
        uintptr_t device_table = *(uintptr_t *)(game_base + 0x4352138);
        uint64_t device_count = device_count_ptr ? *(uint64_t *)device_count_ptr : 0;
        uint8_t configured = 0;
        if (device_idx != 0 && device_idx < device_count && device_table) {
            uintptr_t entry = device_table + (((uintptr_t)device_idx * 8 + device_idx) * 8);
            uintptr_t list = *(uintptr_t *)(entry + 0x10);
            if (list) {
                uint16_t *p = (uint16_t *)list;
                while (*p && configured < 64) {
                    configured++;
                    p++;
                }
            }
        }
        discovery_log("ball_trace count_entry cache=%u check=0x%02x override=%u device_idx=%u configured=%u expected=%u delta=%u device_count=%lu",
                      cached,
                      cached_check,
                      override,
                      device_idx,
                      configured,
                      expected,
                      delta,
                      (unsigned long)device_count);
        mc->regs[0] = game_base + 0x4350000;
        mc->pc = pc + 4;
        return 1;
    }

    if (pc == runtime_start_has_credit_branch) {
        int z_set = pstate_z(mc);
        uintptr_t target = game_base + 0x661bc8;
        discovery_log("start_trace credit/new-player branch: bVar10=%lu bVar4=%lu z=%d -> %s",
                      (unsigned long)(mc->regs[23] & 0xff),
                      (unsigned long)(mc->regs[22] & 0xff),
                      z_set,
                      z_set ? "go to no-credit/new-player-pending path" : "continue toward start");
        mc->pc = z_set ? pc + 4 : target;
        return 1;
    }

    if (pc == runtime_start_bvar10_branch) {
        int bvar10 = (int)(mc->regs[23] & 1);
        uintptr_t target = game_base + 0x661be0;
        discovery_log("start_trace bVar10 proc32 branch: bVar10_bit=%d -> %s",
                      bvar10,
                      bvar10 ? "check proc32" : "skip proc32 and check proc33");
        mc->pc = bvar10 ? pc + 4 : target;
        return 1;
    }

    if (pc == runtime_start_proc32_branch) {
        int queued32 = (int)(mc->regs[0] & 1);
        uintptr_t target = game_base + 0x661cb0;
        discovery_log("start_trace proc32 queued: ret_bit=%d -> %s",
                      queued32,
                      queued32 ? "check mode gate13" : "continue to proc33 check");
        mc->pc = queued32 ? target : pc + 4;
        return 1;
    }

    if (pc == runtime_start_proc33_branch) {
        int queued33 = (int)(mc->regs[0] & 1);
        uintptr_t target = game_base + 0x661c50;
        discovery_log("start_trace proc33 queued: ret_bit=%d -> %s",
                      queued33,
                      queued33 ? "cleanup proc33 and abort start" : "continue to capacity/event gates");
        mc->pc = queued33 ? pc + 4 : target;
        return 1;
    }

    if (pc == runtime_start_gate13_branch) {
        int gate13 = (int)(mc->regs[0] & 1);
        uintptr_t target = game_base + 0x661be0;
        discovery_log("start_trace gate13 after proc32: ret_bit=%d -> %s",
                      gate13,
                      gate13 ? "clear proc32/queue proc33 and abort start" : "continue to proc33 check");
        mc->pc = gate13 ? pc + 4 : target;
        return 1;
    }

    if (pc == runtime_start_display_branch) {
        int display_ret = (int)(mc->regs[0] & 1);
        uintptr_t target = game_base + 0x661ad8;
        discovery_log("start_trace display gate: ret_bit=%d -> %s",
                      display_ret,
                      display_ret ? "abort start" : "check ball_select gate");
        mc->pc = display_ret ? target : pc + 4;
        return 1;
    }

    if (pc == runtime_start_ball_branch) {
        int z_set = (int)((mc->pstate >> 30) & 1);
        uintptr_t target = game_base + 0x661ad8;
        discovery_log("start_trace ball_select gate: w0=0x%lx z=%d -> %s",
                      (unsigned long)(mc->regs[0] & 0xffffffffUL),
                      z_set,
                      z_set ? "abort start" : "call start path");
        mc->pc = z_set ? target : pc + 4;
        return 1;
    }

    if (pc == runtime_start_entry) {
        uintptr_t new_sp = mc->sp - 64;
        uint64_t *saved = (uint64_t *)new_sp;
        saved[0] = mc->regs[19];
        saved[1] = mc->regs[20];
        discovery_log("start_trace ENTER start_game path args local4a=%lu local49=%lu bVar10=%lu bVar4=%lu capacity=%lu",
                      (unsigned long)(mc->regs[0] & 0xff),
                      (unsigned long)(mc->regs[1] & 0xff),
                      (unsigned long)(mc->regs[2] & 0xff),
                      (unsigned long)(mc->regs[3] & 0xff),
                      (unsigned long)(mc->regs[4] & 0xffffffffUL));
        mc->sp = new_sp;
        mc->pc = pc + 4;
        return 1;
    }

    if (pc == runtime_start_real_current) {
        int current_started = (int)(mc->regs[0] & 1);
        uintptr_t target = game_base + 0x661180;
        discovery_log("start_trace real_start current-player gate 767090: ret_bit=%d local4a=%lu local49=%lu bVar10=%lu bVar4=%lu capacity=%lu -> %s",
                      current_started,
                      (unsigned long)(mc->regs[21] & 0xff),
                      (unsigned long)(mc->regs[23] & 0xff),
                      (unsigned long)(mc->regs[24] & 0xff),
                      (unsigned long)(mc->regs[22] & 0xff),
                      (unsigned long)(mc->regs[20] & 0xffffffffUL),
                      current_started ? "skip credit-local4a gates" : "check local4a");
        mc->pc = current_started ? target : pc + 4;
        return 1;
    }

    if (pc == runtime_start_real_local4a) {
        int local4a = (int)(mc->regs[21] & 0xff);
        uintptr_t target = game_base + 0x661170;
        discovery_log("start_trace real_start local4a gate: local4a=%d -> %s",
                      local4a,
                      local4a ? "check credit units" : "return without starting");
        mc->pc = local4a ? target : pc + 4;
        return 1;
    }

    if (pc == runtime_start_real_credit_cmp) {
        int z_set = pstate_z(mc);
        int c_set = pstate_c(mc);
        int b_hi = c_set && !z_set;
        uintptr_t target = game_base + 0x66115c;
        discovery_log("start_trace real_start credit compare: local4a=%lu credit_units=%lu c=%d z=%d -> %s",
                      (unsigned long)(mc->regs[21] & 0xff),
                      (unsigned long)(mc->regs[0] & 0xff),
                      c_set,
                      z_set,
                      b_hi ? "return without starting" : "fire start event4a");
        mc->pc = b_hi ? target : pc + 4;
        return 1;
    }

    if (pc == runtime_start_real_event4a) {
        int event4a_ret = (int)(mc->regs[0] & 1);
        uintptr_t target = game_base + 0x66115c;
        discovery_log("start_trace real_start event4a: ret_bit=%d -> %s",
                      event4a_ret,
                      event4a_ret ? "commit start state" : "return without starting");
        mc->pc = event4a_ret ? pc + 4 : target;
        return 1;
    }

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
