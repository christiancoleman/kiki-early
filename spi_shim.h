/*
 * spi_shim.h — shared macros and forward declarations for the shim.
 *
 * Every translation unit that ends up in spi_shim.so should include this.
 */
#ifndef SPI_SHIM_H
#define SPI_SHIM_H

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>    /* getenv — for shim_sysroot_tmp_path inline below */
#include <unistd.h>    /* write, STDERR_FILENO — for SHIM_LOG single-syscall path */
#include <dlfcn.h>     /* dlsym, RTLD_NEXT — for FWD_* */
#include <signal.h>    /* sig_atomic_t, siginfo_t — for handler decls below */
#include <setjmp.h>
#include <sys/types.h> /* ssize_t — for real_read / real_write typedefs */

/* Real-GL gate: when 1, FWD_* macros forward to the real Mesa/libGLES
 * implementation via dlsym(RTLD_NEXT, ...); when 0, the stub body runs.
 * Defined in spi_shim.c (eventually shim_common.c). */
int is_real_gl(void);

/* Display constants. The resolution the shim advertises via fake DRM mode
 * info, also the size glReadPixels uses when capturing frames for the frame
 * bridge. */
#define FAKE_MODE_HDISPLAY 1920
#define FAKE_MODE_VDISPLAY 1080

/* Cmode registry (defined in shim_diagnostics.c).
 * BN-extracted table of 42 C-mode objects + their bit-field offsets. Used
 * by the state bridge poll thread for delta emission, and by dispatch-table
 * dumpers in diagnostics. `shim_cmode_table_count` companion is needed
 * because sizeof() can't see an extern array's length. */
struct shim_cmode_entry {
    uintptr_t   va;              /* ELF VA of the .bss object */
    uint32_t    expected_mode_id;
    uint32_t    expected_type_flags;
    const char *name_hint;
};
extern const struct shim_cmode_entry shim_cmode_table[];
extern const size_t                  shim_cmode_table_count;

uintptr_t shim_resolve_game_base(void);

/* libavcodec/libavutil symbol resolver (defined in shim_patches.c).
 * Tries dlvsym(version) → dlsym → dlopen-handle, in that order. Used by
 * any shim TU that needs to call into libav* by name. */
void *shim_resolve_libav(const char *sym, const char *version);

/* Forward-decode hooks (defined in shim_avforward.c).
 * Called from shim_patches.c's avcodec_send_packet / avcodec_receive_frame
 * when SPIKE3_FORWARD_DECODE=1. Each returns 1 if it handled the call (and
 * wrote the libav return code into *out_ret), or 0 if the bridge couldn't
 * engage (daemon unreachable / all slots in use). The caller HARD-FAILS
 * on the 0 return — there is no SW fallback; see shim_patches.c rationale. */
int shim_avforward_enabled(void);
int shim_avforward_send_packet(void *avctx, const void *avpkt, int *out_ret);
int shim_avforward_receive_frame(void *avctx, void *frame, int *out_ret);
int shim_avforward_flush(void *avctx);

/* Per-frame video pixel side-table. shim_avforward populates the entry when
 * a frame is decoded (RGBA, post-conversion in the host daemon);
 * shim_gl::eglCreateImageKHR pulls it out of the EGL attribs (via
 * EGL_DMA_BUF_PLANE0_FD_EXT, which carries our sentinel fd) so
 * glEGLImageTargetTexture2DOES can upload real pixels instead of NULL.
 * Returns 1 with all out-pointers populated on hit, 0 on miss. */
int shim_avforward_pixel_lookup(int sentinel_fd,
                                const uint8_t **rgba_ptr,
                                int *width, int *height, int *stride);

/* GL marshalling (defined in shim_gl_marshal.c).
 * Atomic Horizon Phase 1: ferries shim GL/EGL calls to the host-native
 * gl_daemon.py over abstract AF_UNIX @spike3_gl. Active iff
 * SPIKE3_GL_DAEMON=1 AND the daemon is reachable. send_sync returns the
 * reply opcode (>= 0) or -1 on failure; out_reply_len is the number of
 * payload bytes copied into reply_payload (truncated to reply_max). */
int shim_gl_marshal_active(void);
int shim_gl_marshal_send_async(uint16_t op, const void *args, uint32_t args_len);
int shim_gl_marshal_send_sync(uint16_t op,
                              const void *args, uint32_t args_len,
                              void *reply_payload, uint32_t reply_max,
                              uint32_t *out_reply_len);

/* Runtime SPI RX override (defined in shim_ioctl.c).
 * Legacy one-shot CPU-local overlay. New held switch input should use the
 * channel/bit mixer below so presses can clear/toggle raw channel bytes and
 * multiple buttons can be held simultaneously. */
extern volatile uint8_t shim_spi_rx_extra[8];

/* Held switch overlay (defined in shim_inject.c).
 * Host-side input APIs set/clear held channel bits. Channel 0 raw presses
 * clear bits in the CPU SPI idle buffer; node-bus channels toggle bits
 * relative to their saved idle bytes, matching the existing manual press
 * behavior. */
#define SHIM_SWITCH_CHANNEL_COUNT       16
#define SHIM_SWITCH_CHANNEL_BYTES       8
#define SHIM_SWITCH_BITS_PER_CHANNEL    64
#define SHIM_SWITCH_MAX_RUNTIME_ID      255

#define SHIM_GAME_VA_SWITCH_ARRAY_PTR   0x4480220ULL
#define SHIM_GAME_VA_CHANNEL_IDLE_BASE  0x44804d0ULL
#define SHIM_GAME_CHANNEL_IDLE_STRIDE   0xb0ULL

#define SHIM_COIN_DOOR_INTERLOCK_BYTE   2
#define SHIM_COIN_DOOR_INTERLOCK_MASK   0x80u

#define SHIM_SWITCH_ENTRY_SIZE          0x30u
#define SHIM_SWITCH_ENTRY_CONFIG_OFF    0x10u
#define SHIM_SWITCH_ENTRY_ID_OFF        0x20u
#define SHIM_SWITCH_ENTRY_BIT_IDX_OFF   0x22u
#define SHIM_SWITCH_ENTRY_CHANNEL_OFF   0x24u
#define SHIM_SWITCH_ENTRY_EDGE_OFF      0x26u
#define SHIM_SWITCH_ENTRY_STATE_OFF     0x28u

void shim_switch_hold_set(uint8_t channel_id, uint16_t bit_idx, int held);
void shim_switch_hold_apply(uint8_t channel_id, uint8_t buf[SHIM_SWITCH_CHANNEL_BYTES]);
void shim_switch_set_coin_door_open(int open);
void shim_switch_make_channel0_idle(uint8_t out[SHIM_SWITCH_CHANNEL_BYTES]);
void shim_switch_copy_channel_idle(uintptr_t game_base,
                                   uint8_t channel_id,
                                   uint8_t out[SHIM_SWITCH_CHANNEL_BYTES]);
uint8_t *shim_switch_array(uintptr_t game_base);
uint8_t *shim_switch_entry(uint8_t *switch_array, int sw_id);

/* Decoder compatibility patch (defined in shim_patches.c).
 * Patches the b.ne at game VA 0x920cf0 in the game's get_format callback
 * so it accepts NV12 instead of insisting on DRM_PRIME. Required for any
 * non-V4L2-stateless decoder to function — both the diagnostic SW hevc
 * path and the forward-decode bridge. Default-on; gate via
 * SPIKE3_DECODER_PATCH=0 for debugging only. Called from shim_inject.c's
 * deferred-init thread (after game-base is resolvable). */
void shim_patch_get_format_callback(void);
void shim_patch_channel1_derived_switches(void);

/* ---- Signal handlers (defined in shim_diagnostics.c; registered by
 *      shim_install_crash_handler in spi_shim.c) ---- */
void shim_sigsegv_handler(int sig, siginfo_t *info, void *ucontext);
void shim_sigabrt_handler(int sig, siginfo_t *info, void *ucontext);
void shim_term_dump_handler(int sig);

/* ---- Event-sweep globals (defined in shim_inject.c; read by
 *      shim_sigsegv_handler in shim_diagnostics.c to siglongjmp back out
 *      of a faulting sweep iteration). ---- */
extern sigjmp_buf            shim_event_sweep_jmp;
extern volatile sig_atomic_t shim_event_fire_in_progress;
extern volatile int          shim_current_sweep_event_id;

void shim_event_sweep(void);

/* Netbridge UART protocol (defined in shim_nodebus.c).
 * Called from the write() hook in shim_io.c when the game sends a frame on
 * the UART socketpair; synthesizes a response back to the shim side. */
void synthesize_bridge_response(const uint8_t *raw_req, size_t raw_len);

/* Fake-fd constants. SPI / I2C / V4L2 use pure fake fds (no backing socketpair).
 * DRM and UART use real socketpair fds and check via is_drm_fd() / is_uart_fd(). */
#define FAKE_FD_SPI          1023
#define FAKE_FD_I2C          1022
#define FAKE_FD_V4L2_MEDIA   1021
#define FAKE_FD_V4L2_VIDEO   1020
#define FAKE_FD_BOOT_DISPLAY 1019  /* /dev/boot_display — Stern char device */

/* Device-layer state (defined in spi_shim.c). intercept_device() writes
 * these on open; the I/O and ioctl hooks in shim_io.c / shim_ioctl.c read
 * them to dispatch per-device. */
extern int      spi_active;
extern int      i2c_active;
extern int      v4l2_media_active;
extern int      v4l2_video_active;
extern int      boot_display_active;
extern int      uart_game_fd;
extern int      uart_shim_fd;   /* shim side of the socketpair — used by shim_io.c close and shim_nodebus.c response synth */
extern int      drm_game_fd;
extern int      drm_shim_fd;
extern uint32_t spi_transfer_count;
extern uint32_t i2c_ioctl_count;
extern uint32_t v4l2_ioctl_count;
extern uint32_t uart_read_count;
extern uint32_t uart_write_count;

/* I2C virtual register file (defined in spi_shim.c). Read/written by the
 * helpers below; both shim_ioctl.c (RDWR/SMBUS handlers) and shim_io.c
 * (raw read/write on FAKE_FD_I2C) go through the helpers. */
extern uint8_t  i2c_regfile[128][256];
extern uint8_t  i2c_reg_pointer[128];
extern uint8_t  i2c_current_slave;

/* Apply a write transaction to the regfile.
 * Convention: buf[0] sets the per-slave register pointer; buf[1..len-1]
 * (if any) are written sequentially starting at that pointer with
 * auto-increment. len==0 is accepted (zero-length quick-poll-style msg).
 * slave is masked to 7 bits. */
void shim_i2c_handle_write(uint8_t slave, const uint8_t *buf, size_t len);

/* Apply a read transaction. Reads len bytes from regfile[slave] starting at
 * the per-slave register pointer; advances the pointer past the read window
 * (clamped at 256). Returns the number of bytes actually placed in buf. */
size_t shim_i2c_handle_read(uint8_t slave, uint8_t *buf, size_t len);

/* fd predicates (defined in spi_shim.c). Used by shim_io.c + shim_ioctl.c +
 * termios hooks still in spi_shim.c. */
int is_fake_fd(int fd);
int is_uart_fd(int fd);
int is_drm_fd(int fd);
int is_v4l2_fd(int fd);
int is_boot_display_fd(int fd);

/* Real libc function pointers — resolved by resolve_all() and referenced
 * across the I/O / ioctl / fs / nodebus TUs. */
typedef int     (*real_open_t)(const char *, int, ...);
typedef int     (*real_close_t)(int);
typedef int     (*real_ioctl_t)(int, unsigned long, ...);
typedef ssize_t (*real_read_t)(int, void *, size_t);
typedef ssize_t (*real_write_t)(int, const void *, size_t);
extern real_open_t   real_open;    /* used by shim_fs.c open() */
extern real_close_t  real_close;   /* used by shim_io.c close() */
extern real_ioctl_t  real_ioctl;   /* used by shim_ioctl.c ioctl() */
extern real_read_t   real_read;    /* used by shim_io.c read() */
extern real_write_t  real_write;   /* used by shim_io.c write() + shim_nodebus.c + shim_ioctl.c */

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

/* Device intercept dispatcher (defined in spi_shim.c).
 * Called from the open family in shim_fs.c. Returns a fake-fd (or a real
 * socketpair fd for DRM/UART) if pathname matches a known device node,
 * or 0 to fall through to real open(). */
int intercept_device(const char *pathname);

/* Lazy-resolve all libc function pointers (real_open / real_close / real_ioctl
 * / real_read / real_write). Idempotent — cheap to call repeatedly.
 * Defined in spi_shim.c; called from the open() hook at the top of shim_fs.c
 * so the first file open has real_open ready. */
void resolve_all(void);

void shim_bridge_start(void);             /* state bridge; no-op unless SPIKE3_BRIDGE=1 */
void shim_frame_start(void);              /* frame bridge; no-op unless SPIKE3_FRAME_BRIDGE=1 */
void shim_frame_capture_at_page_flip(void); /* called from the DRM PAGE_FLIP ioctl handler */
void shim_audio_start(void);              /* audio bridge; no-op unless SPIKE3_AUDIO_BRIDGE=1 */
void shim_audio_emit(const uint8_t *buf, size_t bytes); /* called from snd_pcm_writei */

void shim_devbus_init(void);              /* devbus subscriber bridge — see shim_devbus.c */

/* AF_UNIX abstract-namespace addressing.
 * WSL2's 9P mount of /mnt/c/... does NOT support AF_UNIX bind (errno 95
 * EOPNOTSUPP). Putting sockets under $SYSROOT/tmp/ on /mnt/c is therefore
 * impossible. Putting them under host /tmp works (real tmpfs) but leaks
 * transient files into WSL's filesystem.
 *
 * The Linux abstract namespace (sun_path[0] = '\0', followed by name) lets
 * the kernel manage the binding name in a private namespace — zero
 * filesystem touch. This is the canonical IPC strategy for our case:
 * shim and helpers use the same abstract name, kernel matches them up,
 * nobody touches WSL or any filesystem.
 *
 * Names are unique system-wide (per network namespace) — collisions would
 * cause EADDRINUSE if two installations run simultaneously. The
 * "pokemon_spike3_" prefix avoids collisions with anything else likely.
 *
 * Logging convention: prepend "@" to the name when printing (matches
 * `ss -l` and lsof formatting for abstract sockets). */
#include <sys/socket.h>
#include <sys/un.h>
#include <string.h>

static inline socklen_t shim_unix_abstract_addr(struct sockaddr_un *out,
                                                const char *name) {
    memset(out, 0, sizeof(*out));
    out->sun_family = AF_UNIX;
    out->sun_path[0] = '\0';  /* the abstract-namespace marker */
    size_t name_len = strlen(name);
    if (name_len > sizeof(out->sun_path) - 2)
        name_len = sizeof(out->sun_path) - 2;
    memcpy(&out->sun_path[1], name, name_len);
    /* addrlen = offsetof(sun_path) + 1 (NUL marker) + name length.
     * Trailing NUL is NOT included — Linux uses the addrlen to delimit. */
    return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + name_len);
}

/* Injection layer (defined in shim_inject.c).
 * Sets up the SIGALRM handler, blocks SIGALRM on the main thread, and
 * spawns the six env-gated worker threads (sweep timer, switch firer,
 * gstreamer force-init, cmode poke, state-flag poke, vp-patch). Called
 * from shim_install_crash_handler at __attribute__((constructor)) time. */
void shim_inject_start(void);

/* Uniform log prefix so shim output is trivially grep-filterable.
 *
 * Atomicity: builds the whole line into a stack buffer and ships it to
 * STDERR_FILENO with one write() syscall. Without this, fprintf() on
 * unbuffered stderr issues a write() per format chunk, so concurrent
 * threads can interleave (we've seen one log line spliced into the
 * middle of another). For lines under PIPE_BUF (4096 on Linux) a
 * single write() is atomic against pipes/sockets/terminals; for
 * regular files atomicity isn't guaranteed by POSIX but is observed
 * in practice for sub-page writes.
 *
 * The 1024-byte buffer is generous for any current call site; lines
 * that would exceed it are truncated rather than split — preserving
 * line atomicity. Subprocess stderr (e.g. from system() calls) still
 * interleaves; that's outside our control.
 */
/* Timestamp helper for SHIM_LOG. Format: "YYYY/MM/DD - HH:MM:SS.mmm - "
 * (24 chars + null). gettimeofday + localtime_r are async-signal-safe on
 * glibc, so this is safe to call from inside the SIGTRAP / SIGSEGV
 * handlers that also use SHIM_LOG. */
#include <time.h>
#include <sys/time.h>

static inline int shim_format_log_timestamp(char *buf, size_t buflen) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm;
    localtime_r(&tv.tv_sec, &tm);
    return snprintf(buf, buflen,
                    "%04d/%02d/%02d - %02d:%02d:%02d.%03ld - ",
                    tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                    tm.tm_hour, tm.tm_min, tm.tm_sec,
                    (long)(tv.tv_usec / 1000));
}

#define SHIM_LOG(fmt, ...) do {                                              \
    char _shim_log_buf[1024];                                                \
    int  _shim_ts_n = shim_format_log_timestamp(_shim_log_buf,               \
                                                sizeof(_shim_log_buf));      \
    if (_shim_ts_n < 0 || (size_t)_shim_ts_n >= sizeof(_shim_log_buf))       \
        _shim_ts_n = 0;                                                      \
    int  _shim_log_n = snprintf(_shim_log_buf + _shim_ts_n,                  \
                                sizeof(_shim_log_buf) - (size_t)_shim_ts_n,  \
                                "[shim] " fmt "\n", ##__VA_ARGS__);          \
    if (_shim_log_n > 0) {                                                   \
        size_t _shim_log_w = (size_t)_shim_ts_n + (size_t)_shim_log_n;       \
        if (_shim_log_w >= sizeof(_shim_log_buf))                            \
            _shim_log_w = sizeof(_shim_log_buf) - 1;                         \
        (void)write(STDERR_FILENO, _shim_log_buf, _shim_log_w);              \
    }                                                                        \
} while (0)

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
