/*
 * spi_shim.h — shared macros and forward declarations for the shim.
 *
 * Every translation unit that ends up in spi_shim.so should include this.
 * Phase 1 of the refactor: only the macros used across many call sites
 * (SHIM_LOG, FWD_*) are lifted here. Everything else stays in spi_shim.c
 * until subsequent phases carve out subsystem files.
 */
#ifndef SPI_SHIM_H
#define SPI_SHIM_H

#include <stdio.h>    /* fprintf, stderr — for SHIM_LOG */
#include <stddef.h>   /* size_t */
#include <stdint.h>   /* uintptr_t, uint32_t, uint8_t */
#include <dlfcn.h>    /* dlsym, RTLD_NEXT — for FWD_* */
#include <signal.h>   /* sig_atomic_t, siginfo_t — for handler decls below */
#include <setjmp.h>   /* sigjmp_buf */

/* Real-GL gate: when 1, FWD_* macros forward to the real Mesa/libGLES
 * implementation via dlsym(RTLD_NEXT, ...); when 0, the stub body runs.
 * Defined in spi_shim.c (eventually shim_common.c). */
int is_real_gl(void);

/* ---- Display constants ----
 * The resolution the shim advertises via fake DRM mode info, also the size
 * glReadPixels uses when capturing frames for the frame bridge. */
#define FAKE_MODE_HDISPLAY 1920
#define FAKE_MODE_VDISPLAY 1080

/* ---- Cmode registry (defined in spi_shim.c / eventually shim_diagnostics.c)
 * BN-extracted table of 42 C-mode objects + their bit-field offsets. Used
 * by the state bridge poll thread for delta emission, and by dispatch-table
 * dumpers in diagnostics. `shim_cmode_table_count` companion is needed
 * because sizeof() can't see an extern array's length. */
struct shim_cmode_entry {
    uintptr_t   bn_va;              /* BN-reported .bss address */
    uint32_t    expected_mode_id;
    uint32_t    expected_type_flags;
    const char *name_hint;
};
extern const struct shim_cmode_entry shim_cmode_table[];
extern const size_t                  shim_cmode_table_count;

/* ---- Runtime helpers (defined in shim_diagnostics.c) ---- */
uintptr_t shim_resolve_game_base(void);

/* ---- Signal handlers (defined in shim_diagnostics.c; registered by
 *      shim_install_crash_handler in spi_shim.c) ---- */
void shim_sigsegv_handler(int sig, siginfo_t *info, void *ucontext);
void shim_term_dump_handler(int sig);

/* ---- Event-sweep globals (defined in spi_shim.c; read by
 *      shim_sigsegv_handler in shim_diagnostics.c to siglongjmp back out
 *      of a faulting sweep iteration). ---- */
extern sigjmp_buf            shim_event_sweep_jmp;
extern volatile sig_atomic_t shim_event_fire_in_progress;
extern volatile int          shim_current_sweep_event_id;

/* Called from shim_term_dump_handler on SIGUSR2 (defined in spi_shim.c). */
void shim_event_sweep(void);

/* ---- Netbridge UART protocol (defined in shim_nodebus.c) ----
 * Called from the write() hook in spi_shim.c when the game sends a frame on
 * the UART socketpair; synthesizes a response back to the shim side. */
void synthesize_bridge_response(const uint8_t *raw_req, size_t raw_len);

/* ---- Filesystem layer (defined in shim_fs.c) ---- */

/* GPIO sysfs fake-fd pool — each open("/sys/class/gpio/...") allocates one
 * of these slots. Pool size and base are exposed here because the I/O hooks
 * in spi_shim.c (read/write/close/lseek) check fds against this range. */
#define FAKE_FD_GPIO_BASE   1000
#define FAKE_FD_GPIO_COUNT  16
extern char     *gpio_fake_paths[FAKE_FD_GPIO_COUNT];
extern uint8_t   gpio_fake_read_done[FAKE_FD_GPIO_COUNT];
int  is_gpio_fake_fd(int fd);
int  is_gpio_sysfs_path(const char *p);
int  alloc_gpio_fake_fd(const char *path);
void free_gpio_fake_fd(int fd);

/* ---- Device intercept dispatcher (defined in spi_shim.c) ----
 * Called from the open family in shim_fs.c. Returns a fake-fd (or a real
 * socketpair fd for DRM/UART) if pathname matches a known device node,
 * or 0 to fall through to real open(). */
int intercept_device(const char *pathname);

/* Lazy-resolve all libc function pointers (real_open / real_close / real_ioctl
 * / real_read / real_write). Idempotent — cheap to call repeatedly.
 * Defined in spi_shim.c; called from the open() hook at the top of shim_fs.c
 * so the first file open has real_open ready. */
void resolve_all(void);

/* ---- Bridge lifecycle (defined in shim_bridges.c) ---- */
void shim_bridge_start(void);             /* state bridge; no-op unless SPIKE3_BRIDGE=1 */
void shim_frame_start(void);              /* frame bridge; no-op unless SPIKE3_FRAME_BRIDGE=1 */
void shim_frame_capture_at_page_flip(void); /* called from the DRM PAGE_FLIP ioctl handler */

/* Uniform log prefix so shim output is trivially grep-filterable. */
#define SHIM_LOG(fmt, ...) fprintf(stderr, "[shim] " fmt "\n", ##__VA_ARGS__)

/* FWD_RET(RET, FN, SIG, CALL) — return-typed forward.
 * FWD_VOID(FN, SIG, CALL)     — void forward.
 * Usage inside a stub body, as the first statement:
 *   void glEnable(unsigned cap) {
 *       FWD_VOID(glEnable, (unsigned), (cap));
 *       ... existing stub body ...
 *   }
 * When SPIKE3_REAL_GL is unset, expands to a no-op and the stub runs.
 * When set, resolves the real function once via dlsym(RTLD_NEXT, ...) and
 * tail-calls it. Cached per-function in a static pointer. */
#define FWD_RET(RET, FN, SIG, CALL) do {                                 \
    if (__builtin_expect(is_real_gl(), 0)) {                             \
        static RET (*_real)SIG = NULL;                                   \
        if (!_real) _real = (RET (*)SIG)dlsym(RTLD_NEXT, #FN);           \
        if (_real) return _real CALL;                                    \
    }                                                                    \
} while (0)
#define FWD_VOID(FN, SIG, CALL) do {                                     \
    if (__builtin_expect(is_real_gl(), 0)) {                             \
        static void (*_real)SIG = NULL;                                  \
        if (!_real) _real = (void (*)SIG)dlsym(RTLD_NEXT, #FN);          \
        if (_real) { _real CALL; return; }                               \
    }                                                                    \
} while (0)
/* Verbose variants — log "FWD <fn>" before the tail-call. Use only at
 * low-frequency sites (EGL ~20 calls); never on GLES hot paths where the
 * 10k-line/20s steady state would drown any diagnostic signal. */
#define FWD_RET_V(RET, FN, SIG, CALL) do {                               \
    if (__builtin_expect(is_real_gl(), 0)) {                             \
        static RET (*_real)SIG = NULL;                                   \
        if (!_real) _real = (RET (*)SIG)dlsym(RTLD_NEXT, #FN);           \
        SHIM_LOG("FWD " #FN);                                            \
        if (_real) return _real CALL;                                    \
    }                                                                    \
} while (0)
#define FWD_VOID_V(FN, SIG, CALL) do {                                   \
    if (__builtin_expect(is_real_gl(), 0)) {                             \
        static void (*_real)SIG = NULL;                                  \
        if (!_real) _real = (void (*)SIG)dlsym(RTLD_NEXT, #FN);          \
        SHIM_LOG("FWD " #FN);                                            \
        if (_real) { _real CALL; return; }                               \
    }                                                                    \
} while (0)

#endif /* SPI_SHIM_H */
