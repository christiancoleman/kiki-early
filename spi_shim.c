/*
 * spike3_shim.c - LD_PRELOAD shim for Spike 3 hardware emulation
 *
 * Intercepts open/close/ioctl/read/write on hardware device files to emulate
 * Spike 3 peripherals when running the game binary under QEMU user-mode.
 *
 * Currently intercepts:
 *   /dev/spidev4.0  — SPI node bus (coils, switches, LEDs, motors)
 *   /dev/i2c-10     — I2C bus (likely LCD backlight, NUK, power management)
 *   /dev/ttyAMA5    — UART bridge (netbridge communication)
 *
 * Build (cross-compile for aarch64):
 *   aarch64-linux-gnu-gcc -shared -fPIC -o spi_shim.so spi_shim.c -ldl
 *
 * Run:
 *   cd games/pokemon_le && qemu-aarch64 -L /path/to/sysroot \
 *     -E LD_PRELOAD=/path/to/spi_shim.so ./game
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <termios.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/mman.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>

/* ---- Path remapping ----
 * QEMU user-mode's -L only affects the dynamic linker, not regular file ops.
 * We remap absolute paths so the game's filesystem references resolve against
 * the sysroot. Set SPIKE3_SYSROOT env var, or it defaults to empty (no remap).
 */
static const char *sysroot = NULL;
static int sysroot_len = 0;

static void init_sysroot(void) {
    if (sysroot) return;
    sysroot = getenv("SPIKE3_SYSROOT");
    if (!sysroot) sysroot = "";
    sysroot_len = strlen(sysroot);
}

/* Remap an absolute path through the sysroot. Returns static buffer. */
static char remapped_buf[4096];
static const char *remap_path(const char *path) {
    if (!path || !path[0]) return path;
    init_sysroot();
    if (sysroot_len == 0) return path;
    /* Only remap absolute paths (not /dev/, /proc/, /sys/, /tmp/) */
    if (path[0] != '/') return path;
    if (strncmp(path, "/dev/", 5) == 0) return path;
    if (strncmp(path, "/proc/", 6) == 0) return path;
    if (strncmp(path, "/sys/", 5) == 0) return path;
    if (strncmp(path, "/tmp/", 5) == 0) return path;
    snprintf(remapped_buf, sizeof(remapped_buf), "%s%s", sysroot, path);
    return remapped_buf;
}

/* ---- SPI ioctl definitions ---- */

#define SPI_IOC_WR_MODE          0x40016b01
#define SPI_IOC_WR_MAX_SPEED_HZ  0x40046b04
#define SPI_IOC_MESSAGE_1        0x40206b00

struct spi_ioc_transfer {
    uint64_t tx_buf;
    uint64_t rx_buf;
    uint32_t len;
    uint32_t speed_hz;
    uint16_t delay_usecs;
    uint8_t  bits_per_word;
    uint8_t  cs_change;
    uint8_t  tx_nbits;
    uint8_t  rx_nbits;
    uint8_t  word_delay_usecs;
    uint8_t  pad;
};

/* ---- I2C ioctl definitions ---- */

#define I2C_SLAVE       0x0703
#define I2C_SLAVE_FORCE 0x0706
#define I2C_RDWR        0x0707
#define I2C_SMBUS       0x0720

/* ---- DRM fake resource IDs ----
 *
 * We expose: 1 CRTC (id 1), 1 encoder (id 20), 2 connectors (ids 10, 11).
 * First connector is "connected" with one 800x480@60 mode; second is
 * disconnected (just exists to satisfy the >=2 connector check the game
 * does before falling back to card0 — see BN analysis of sub_d01160).
 * No framebuffers initially.
 */

#define FAKE_CRTC_ID       1
#define FAKE_ENCODER_ID    20
#define FAKE_CONNECTOR_A   10   /* connected, primary display */
#define FAKE_CONNECTOR_B   11   /* disconnected, but exists */
#define FAKE_MODE_HDISPLAY 1920
#define FAKE_MODE_VDISPLAY 1080

static uint32_t drm_next_dumb_handle = 1;
static uint32_t drm_next_fb_id = 100;

/* ---- Fake fd assignments ----
 *
 * SPI and I2C are pure fake fds — all traffic is via ioctl(), which we
 * intercept; read/write never happen on them, so no real fd is needed.
 *
 * UART is DIFFERENT. The game's bridge thread writes request frames and
 * then blocks in read() waiting for response frames. If we return a fake
 * fd and make read() return EAGAIN, the thread may treat that as a
 * protocol error. If we return a real socketpair endpoint, read() blocks
 * naturally until we (the shim) write a response to the other end. The
 * socketpair gives us a clean bidirectional channel:
 *   - game holds uart_game_fd; its read()s block until we feed responses
 *   - shim holds uart_shim_fd; write()ing to it unblocks the game's read()
 *   - the game's write()s to uart_game_fd become readable on uart_shim_fd,
 *     which lets the shim parse outgoing request frames (when we're ready
 *     to synthesize replies)
 */

#define FAKE_FD_SPI   1023
#define FAKE_FD_I2C   1022
#define FAKE_FD_DRM   1020   /* /dev/dri/card1 */

/* ---- Device state ---- */

static int spi_active = 0;
static int i2c_active = 0;
static int drm_active = 0;
static int uart_game_fd = -1;   /* returned from open(); real socketpair end */
static int uart_shim_fd = -1;   /* held by the shim for pushing responses */
static uint32_t spi_transfer_count = 0;
static uint32_t i2c_ioctl_count = 0;
static uint32_t uart_read_count = 0;
static uint32_t uart_write_count = 0;

static int uart_is_active(void) { return uart_game_fd >= 0; }

#define SHIM_LOG(fmt, ...) fprintf(stderr, "[shim] " fmt "\n", ##__VA_ARGS__)

/* ---- Real function pointers ---- */

typedef int (*real_open_t)(const char *, int, ...);
typedef int (*real_close_t)(int);
typedef int (*real_ioctl_t)(int, unsigned long, ...);
typedef ssize_t (*real_read_t)(int, void *, size_t);
typedef ssize_t (*real_write_t)(int, const void *, size_t);

static real_open_t real_open = NULL;
static real_close_t real_close = NULL;
static real_ioctl_t real_ioctl = NULL;
static real_read_t real_read = NULL;
static real_write_t real_write = NULL;

static void resolve_all(void) {
    if (!real_open)  real_open  = (real_open_t)dlsym(RTLD_NEXT, "open");
    if (!real_close) real_close = (real_close_t)dlsym(RTLD_NEXT, "close");
    if (!real_ioctl) real_ioctl = (real_ioctl_t)dlsym(RTLD_NEXT, "ioctl");
    if (!real_read)  real_read  = (real_read_t)dlsym(RTLD_NEXT, "read");
    if (!real_write) real_write = (real_write_t)dlsym(RTLD_NEXT, "write");
}

/* ---- Check if path is a device we intercept, return fake fd or 0 ---- */

static int intercept_device(const char *pathname) {
    if (!pathname) return 0;

    if (strcmp(pathname, "/dev/spidev4.0") == 0) {
        spi_active = 1;
        spi_transfer_count = 0;
        SHIM_LOG("OPEN %s -> fake fd %d (SPI node bus)", pathname, FAKE_FD_SPI);
        return FAKE_FD_SPI;
    }
    if (strcmp(pathname, "/dev/i2c-10") == 0) {
        i2c_active = 1;
        i2c_ioctl_count = 0;
        SHIM_LOG("OPEN %s -> fake fd %d (I2C bus)", pathname, FAKE_FD_I2C);
        return FAKE_FD_I2C;
    }
    if (strcmp(pathname, "/dev/dri/card1") == 0) {
        drm_active = 1;
        SHIM_LOG("OPEN %s -> fake fd %d (DRM)", pathname, FAKE_FD_DRM);
        return FAKE_FD_DRM;
    }
    if (strcmp(pathname, "/dev/ttyAMA5") == 0 ||
        strcmp(pathname, "/dev/ttyAMA2") == 0) {
        if (uart_game_fd < 0) {
            int sv[2];
            if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
                SHIM_LOG("socketpair() failed for %s: %s", pathname, strerror(errno));
                return 0;
            }
            uart_game_fd = sv[0];
            uart_shim_fd = sv[1];
            /* Non-blocking on the shim side so we can safely drain game
             * request bytes without stalling. Game side stays blocking. */
            int flags = fcntl(uart_shim_fd, F_GETFL, 0);
            if (flags >= 0) fcntl(uart_shim_fd, F_SETFL, flags | O_NONBLOCK);
        }
        uart_read_count = 0;
        uart_write_count = 0;
        SHIM_LOG("OPEN %s -> real socketpair game=%d shim=%d (UART bridge)",
                 pathname, uart_game_fd, uart_shim_fd);
        return uart_game_fd;
    }
    return 0;
}

static int is_fake_fd(int fd) {
    /* UART is NOT fake anymore — it's a real socketpair fd; callers that
     * need to check for "any intercepted device" should call both this
     * and uart_game_fd explicitly. */
    return (fd == FAKE_FD_SPI && spi_active) ||
           (fd == FAKE_FD_I2C && i2c_active) ||
           (fd == FAKE_FD_DRM && drm_active);
}

static int is_uart_fd(int fd) {
    return uart_game_fd >= 0 && fd == uart_game_fd;
}

/* ---- Hooked: open / openat ---- */

/* Log interesting opens: device nodes, sysfs/procfs, and anything under
 * /games/ that isn't plain-file library loading. Ignore noisy common paths
 * (shared libraries, locale, tzdata) and already-covered devices. */
static int should_log_open(const char *pathname) {
    if (!pathname) return 0;
    if (strncmp(pathname, "/dev/", 5) == 0) return 1;
    if (strncmp(pathname, "/sys/", 5) == 0) return 1;
    if (strncmp(pathname, "/proc/", 6) == 0) return 1;
    if (strncmp(pathname, "/games/", 7) == 0) return 1;
    if (strncmp(pathname, "/run/", 5) == 0) return 1;
    if (strncmp(pathname, "/tmp/", 5) == 0) return 1;
    if (strncmp(pathname, "/var/", 5) == 0) return 1;
    if (strncmp(pathname, "/etc/", 5) == 0) {
        /* Skip /etc/ld.so.*, /etc/nsswitch.conf, /etc/localtime — these are
         * libc bootstrap and don't tell us anything about game state. */
        if (strstr(pathname, "ld.so")) return 0;
        if (strstr(pathname, "nsswitch")) return 0;
        if (strstr(pathname, "localtime")) return 0;
        return 1;
    }
    return 0;
}

int open(const char *pathname, int flags, ...) {
    resolve_all();
    int fake = intercept_device(pathname);
    if (fake) return fake;

    const char *actual = remap_path(pathname);

    int ret;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        int mode = va_arg(args, int);
        va_end(args);
        ret = real_open(actual, flags, mode);
    } else {
        ret = real_open(actual, flags);
    }

    if (should_log_open(pathname)) {
        if (ret < 0) {
            SHIM_LOG("OPEN FAIL: %s (flags=0x%x, errno=%d %s)",
                     pathname, flags, errno, strerror(errno));
        } else {
            SHIM_LOG("OPEN OK:   %s -> fd %d (flags=0x%x)",
                     pathname, ret, flags);
        }
    }
    return ret;
}

typedef int (*real_openat_t)(int, const char *, int, ...);
static real_openat_t real_openat = NULL;

int openat(int dirfd, const char *pathname, int flags, ...) {
    if (!real_openat) real_openat = (real_openat_t)dlsym(RTLD_NEXT, "openat");

    int fake = intercept_device(pathname);
    if (fake) return fake;

    const char *actual = remap_path(pathname);

    int ret;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        int mode = va_arg(args, int);
        va_end(args);
        ret = real_openat(dirfd, actual, flags, mode);
    } else {
        ret = real_openat(dirfd, actual, flags);
    }

    if (should_log_open(pathname)) {
        if (ret < 0) {
            SHIM_LOG("OPENAT FAIL: %s (dirfd=%d flags=0x%x, errno=%d %s)",
                     pathname, dirfd, flags, errno, strerror(errno));
        } else {
            SHIM_LOG("OPENAT OK:   %s -> fd %d (dirfd=%d flags=0x%x)",
                     pathname, ret, dirfd, flags);
        }
    }
    return ret;
}

/* ---- Hooked: faccessat ---- */

typedef int (*real_faccessat_t)(int, const char *, int, int);
static real_faccessat_t real_faccessat = NULL;

int faccessat(int dirfd, const char *pathname, int mode, int flags) {
    if (!real_faccessat) real_faccessat = (real_faccessat_t)dlsym(RTLD_NEXT, "faccessat");
    const char *actual = remap_path(pathname);
    if (actual != pathname) {
        SHIM_LOG("faccessat REMAP: %s -> %s", pathname, actual);
    }
    int ret = real_faccessat(dirfd, actual, mode, flags);
    if (ret != 0 && pathname[0] == '/') {
        SHIM_LOG("faccessat FAILED: %s (errno=%d)", actual, errno);
    }
    return ret;
}

/* ---- Hooked: newfstatat / fstatat ---- */

typedef int (*real_fstatat_t)(int, const char *, void *, int);
typedef int (*real_fxstatat_t)(int, int, const char *, void *, int);

int fstatat(int dirfd, const char *pathname, void *statbuf, int flags) {
    static real_fstatat_t real_fn = NULL;
    if (!real_fn) real_fn = (real_fstatat_t)dlsym(RTLD_NEXT, "fstatat");
    const char *actual = remap_path(pathname);
    return real_fn(dirfd, actual, statbuf, flags);
}

int __fxstatat(int ver, int dirfd, const char *pathname, void *statbuf, int flags) {
    static real_fxstatat_t real_fn = NULL;
    if (!real_fn) real_fn = (real_fxstatat_t)dlsym(RTLD_NEXT, "__fxstatat");
    const char *actual = remap_path(pathname);
    return real_fn(ver, dirfd, actual, statbuf, flags);
}

int __fxstatat64(int ver, int dirfd, const char *pathname, void *statbuf, int flags) {
    static real_fxstatat_t real_fn = NULL;
    if (!real_fn) real_fn = (real_fxstatat_t)dlsym(RTLD_NEXT, "__fxstatat64");
    const char *actual = remap_path(pathname);
    return real_fn(ver, dirfd, actual, statbuf, flags);
}

/* ---- Hooked: access ----
 * glibc's access() goes straight to the faccessat syscall via INLINE_SYSCALL_CALL,
 * bypassing the libc faccessat() wrapper. So hooking faccessat() alone misses
 * every access() call. We must hook access() directly.
 */

typedef int (*real_access_t)(const char *, int);

int access(const char *pathname, int mode) {
    static real_access_t real_fn = NULL;
    if (!real_fn) real_fn = (real_access_t)dlsym(RTLD_NEXT, "access");
    const char *actual = remap_path(pathname);
    if (actual != pathname) {
        SHIM_LOG("access REMAP: %s -> %s", pathname, actual);
    }
    int ret = real_fn(actual, mode);
    if (ret != 0 && pathname[0] == '/') {
        SHIM_LOG("access FAILED: %s (errno=%d)", actual, errno);
    }
    return ret;
}

/* ---- Hooked: stat / lstat (and __xstat/__lxstat glibc compat) ---- */

typedef int (*real_stat_t)(const char *, void *);
typedef int (*real_xstat_t)(int, const char *, void *);

int stat(const char *pathname, void *statbuf) {
    static real_stat_t real_fn = NULL;
    if (!real_fn) real_fn = (real_stat_t)dlsym(RTLD_NEXT, "stat");
    const char *actual = remap_path(pathname);
    return real_fn(actual, statbuf);
}

int lstat(const char *pathname, void *statbuf) {
    static real_stat_t real_fn = NULL;
    if (!real_fn) real_fn = (real_stat_t)dlsym(RTLD_NEXT, "lstat");
    const char *actual = remap_path(pathname);
    return real_fn(actual, statbuf);
}

int __xstat(int ver, const char *pathname, void *statbuf) {
    static real_xstat_t real_fn = NULL;
    if (!real_fn) real_fn = (real_xstat_t)dlsym(RTLD_NEXT, "__xstat");
    const char *actual = remap_path(pathname);
    return real_fn(ver, actual, statbuf);
}

int __lxstat(int ver, const char *pathname, void *statbuf) {
    static real_xstat_t real_fn = NULL;
    if (!real_fn) real_fn = (real_xstat_t)dlsym(RTLD_NEXT, "__lxstat");
    const char *actual = remap_path(pathname);
    return real_fn(ver, actual, statbuf);
}

int __xstat64(int ver, const char *pathname, void *statbuf) {
    static real_xstat_t real_fn = NULL;
    if (!real_fn) real_fn = (real_xstat_t)dlsym(RTLD_NEXT, "__xstat64");
    const char *actual = remap_path(pathname);
    return real_fn(ver, actual, statbuf);
}

int __lxstat64(int ver, const char *pathname, void *statbuf) {
    static real_xstat_t real_fn = NULL;
    if (!real_fn) real_fn = (real_xstat_t)dlsym(RTLD_NEXT, "__lxstat64");
    const char *actual = remap_path(pathname);
    return real_fn(ver, actual, statbuf);
}

/* ---- Hooked: system ----
 * sub_d19580 shells out to `openocd ...` to flash the RP2040 netbridge if the
 * CRC check fails. Under emulation there is no real RP2040; blocking the
 * openocd invocation (returning success) prevents the game from erroring out
 * on programming failure and lets init continue. The thread-ready flag
 * (data_490e5b0 & 1) is independently set by the UART bridge thread, not by
 * programming success — so returning 0 here is safe.
 */

typedef int (*real_system_t)(const char *);

int system(const char *cmd) {
    static real_system_t real_fn = NULL;
    if (!real_fn) real_fn = (real_system_t)dlsym(RTLD_NEXT, "system");
    if (cmd && (strstr(cmd, "openocd") || strstr(cmd, "/games/spike3/"))) {
        SHIM_LOG("BLOCKED system(): %.160s", cmd);
        return 0;
    }
    SHIM_LOG("system(): %.160s", cmd ? cmd : "(null)");
    return real_fn(cmd);
}

/* ---- Hooked: pthread_create (force inherit-sched) ----
 * The game spawns at least one worker thread with explicit SCHED_RR
 * attributes (seen in strace: `sched_setscheduler(3614, 2, ...) = EPERM`).
 * Under QEMU user-mode running as a regular user we lack CAP_SYS_NICE AND
 * ulimit -r is 0 (RT priority disabled at the kernel level). glibc's
 * pthread_create, when its internal setup-handshake call to
 * sched_setscheduler fails, signals the suspended child to exit WITHOUT
 * running its entry function. That thread was the dispatch-subsystem
 * worker whose code path ultimately sets `data_4882108` to 1 — the flag
 * polled by the orchestrator `sub_a78bc0` (file 0x678bc0) in a 480-iter
 * × 250 ms loop. Flag never gets set → orchestrator loops forever.
 *
 * glibc uses INTERNAL_SYSCALL_CALL for sched_setscheduler inside
 * pthread_create, which bypasses LD_PRELOAD on the public wrapper (same
 * INLINE_SYSCALL pattern as access/stat). So we hook pthread_create itself:
 * if the caller passed an attr with PTHREAD_EXPLICIT_SCHED, copy it,
 * force PTHREAD_INHERIT_SCHED on the copy, and pass the copy through.
 * The child then inherits the parent's scheduling (SCHED_OTHER) — no
 * privileged syscall involved. Priority differences don't matter under
 * QEMU user-mode emulation anyway.
 */

typedef int (*real_pthread_create_t)(pthread_t *, const pthread_attr_t *,
                                     void *(*)(void *), void *);

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg) {
    static real_pthread_create_t real_fn = NULL;
    if (!real_fn) real_fn = (real_pthread_create_t)dlsym(RTLD_NEXT, "pthread_create");

    if (attr != NULL) {
        int inherit = 0;
        if (pthread_attr_getinheritsched(attr, &inherit) == 0
            && inherit == PTHREAD_EXPLICIT_SCHED) {
            pthread_attr_t copy;
            memcpy(&copy, attr, sizeof(pthread_attr_t));
            pthread_attr_setinheritsched(&copy, PTHREAD_INHERIT_SCHED);
            SHIM_LOG("pthread_create: forcing PTHREAD_INHERIT_SCHED (was EXPLICIT)");
            return real_fn(thread, &copy, start_routine, arg);
        }
    }
    return real_fn(thread, attr, start_routine, arg);
}

/* ---- Hooked: GBM library functions ----
 * libgbm's real gbm_create_device inspects the DRM fd's driver identity via
 * /sys and DRM_IOCTL_VERSION-derived driver-name matching. Our fake DRM fd
 * has no backing /sys entry, so libgbm rejects it with EINVAL. We short-
 * circuit with fake handles so the game's "does GBM work" check succeeds.
 * Pointers returned are just magic sentinels — the real gbm library never
 * sees them because we also intercept every subsequent gbm_* call. */

/* gbm_device/bo/surface pointers need to be dereferenceable.
 * The game's own buffer-management code (or mesa internals that our
 * LD_PRELOAD misses) dereferences the pointers we return, bypassing our
 * getter stubs. Originally we used sentinel constants like 0x61626d44
 * ("abmD") and saw SEGV at address 0x61626d60 — fake-pointer + ~30 bytes.
 * Allocate zero-filled scratch buffers instead so direct-deref reads hit
 * valid memory. Values read back are zero, which the game's gbm_bo_*
 * getters override via our hooks anyway. */

#define FAKE_GBM_BACKING_SIZE 256

static int gbm_drm_fd = -1;
static void *fake_gbm_device_mem  = NULL;
static void *fake_gbm_surface_mem = NULL;

static void *alloc_fake_gbm_object(void) {
    void *p = calloc(1, FAKE_GBM_BACKING_SIZE);
    return p;
}

void *gbm_create_device(int fd) {
    if (!fake_gbm_device_mem) fake_gbm_device_mem = alloc_fake_gbm_object();
    SHIM_LOG("gbm_create_device(fd=%d) -> %p (%d-byte backing)",
             fd, fake_gbm_device_mem, FAKE_GBM_BACKING_SIZE);
    gbm_drm_fd = fd;
    return fake_gbm_device_mem;
}

void gbm_device_destroy(void *device) {
    SHIM_LOG("gbm_device_destroy");
    (void)device;
}

int gbm_device_get_fd(void *device) {
    (void)device;
    return gbm_drm_fd;
}

const char *gbm_device_get_backend_name(void *device) {
    (void)device;
    return "stub";
}

int gbm_device_is_format_supported(void *device, uint32_t format, uint32_t usage) {
    (void)device; (void)format; (void)usage;
    return 1;
}

void *gbm_surface_create(void *gbm, uint32_t width, uint32_t height,
                         uint32_t format, uint32_t flags) {
    if (!fake_gbm_surface_mem) fake_gbm_surface_mem = alloc_fake_gbm_object();
    SHIM_LOG("gbm_surface_create(%ux%u, fmt=0x%x, flags=0x%x) -> %p",
             width, height, format, flags, fake_gbm_surface_mem);
    (void)gbm;
    return fake_gbm_surface_mem;
}

void *gbm_surface_create_with_modifiers(void *gbm, uint32_t width, uint32_t height,
                                        uint32_t format, const uint64_t *modifiers,
                                        unsigned int count) {
    if (!fake_gbm_surface_mem) fake_gbm_surface_mem = alloc_fake_gbm_object();
    SHIM_LOG("gbm_surface_create_with_modifiers(%ux%u) -> %p", width, height,
             fake_gbm_surface_mem);
    (void)gbm; (void)format; (void)modifiers; (void)count;
    return fake_gbm_surface_mem;
}

void gbm_surface_destroy(void *surface) {
    (void)surface;
}

/* Each BO needs its own backing so the game can distinguish them. Track
 * the most-recent one in a small LRU so gbm_surface_lock_front_buffer
 * can return a valid BO. */
static void *gbm_bo_latest = NULL;

void *gbm_bo_create(void *gbm, uint32_t width, uint32_t height,
                    uint32_t format, uint32_t flags) {
    void *bo = alloc_fake_gbm_object();
    gbm_bo_latest = bo;
    SHIM_LOG("gbm_bo_create(%ux%u, fmt=0x%x, flags=0x%x) -> %p",
             width, height, format, flags, bo);
    (void)gbm;
    return bo;
}

void *gbm_bo_create_with_modifiers(void *gbm, uint32_t width, uint32_t height,
                                   uint32_t format, const uint64_t *modifiers,
                                   unsigned int count) {
    void *bo = alloc_fake_gbm_object();
    gbm_bo_latest = bo;
    SHIM_LOG("gbm_bo_create_with_modifiers(%ux%u) -> %p", width, height, bo);
    (void)gbm; (void)format; (void)modifiers; (void)count;
    return bo;
}

void gbm_bo_destroy(void *bo) { free(bo); }

uint32_t gbm_bo_get_width(void *bo)  { (void)bo; return FAKE_MODE_HDISPLAY; }
uint32_t gbm_bo_get_height(void *bo) { (void)bo; return FAKE_MODE_VDISPLAY; }
uint32_t gbm_bo_get_stride(void *bo) { (void)bo; return FAKE_MODE_HDISPLAY * 4; }
uint32_t gbm_bo_get_format(void *bo) { (void)bo; return 0x34325258; /* XR24 */ }
uint64_t gbm_bo_get_modifier(void *bo) { (void)bo; return 0; /* LINEAR */ }
int gbm_bo_get_fd(void *bo) { (void)bo; return -1; }
uint32_t gbm_bo_get_handle_u(void *bo) { (void)bo; return 1; }

/* gbm_bo_get_handle returns a union; declaring it as returning uint32_t
 * by value works because union gbm_bo_handle is layout-compatible with a
 * single 64-bit integer. */
uint64_t gbm_bo_get_handle(void *bo) { (void)bo; return 1; }

void *gbm_surface_lock_front_buffer(void *surface) {
    (void)surface;
    /* Return the most recent BO; if none yet, allocate one. */
    if (!gbm_bo_latest) gbm_bo_latest = alloc_fake_gbm_object();
    return gbm_bo_latest;
}
void gbm_surface_release_buffer(void *surface, void *bo) {
    (void)surface; (void)bo;
}
int gbm_surface_has_free_buffers(void *surface) { (void)surface; return 1; }

/* ---- Hooked: EGL ----
 * Once gbm_create_device returns our fake handle, mesa's libEGL would
 * dereference it — crash. We intercept the public EGL entry points the
 * game uses (per BN analysis of glWindow::setGlContext) so no mesa code
 * runs against our fake GBM handle. Returns are fake but stable.
 */

#define FAKE_EGL_DISPLAY   ((void *)0x65676c44)  /* "eglD" */
#define FAKE_EGL_CONFIG    ((void *)0x65676c43)  /* "eglC" */
#define FAKE_EGL_CONTEXT   ((void *)0x65676c58)  /* "eglX" */
#define FAKE_EGL_SURFACE   ((void *)0x65676c53)  /* "eglS" */
#define FAKE_EGL_IMAGE     ((void *)0x65676c49)  /* "eglI" */
#define FAKE_EGL_SYNC      ((void *)0x65676c59)  /* "eglY" */

/* EGL constants used in our stubs (from <EGL/egl.h>). Declared locally so
 * the shim doesn't need EGL headers installed. */
#define EGL_SUCCESS          0x3000
#define EGL_TRUE             1
#define EGL_FALSE            0
#define EGL_NO_DISPLAY       ((void *)0)
#define EGL_NO_CONTEXT       ((void *)0)
#define EGL_NO_SURFACE       ((void *)0)
#define EGL_OPENGL_ES_API    0x30A0

void *eglGetDisplay(void *native_display) {
    SHIM_LOG("eglGetDisplay(%p) -> fake display", native_display);
    return FAKE_EGL_DISPLAY;
}

void *eglGetPlatformDisplay(unsigned platform, void *native_display, const intptr_t *attrib_list) {
    SHIM_LOG("eglGetPlatformDisplay(platform=0x%x, dpy=%p) -> fake", platform, native_display);
    (void)attrib_list; return FAKE_EGL_DISPLAY;
}

void *eglGetPlatformDisplayEXT(unsigned platform, void *native_display, const int *attrib_list) {
    SHIM_LOG("eglGetPlatformDisplayEXT(platform=0x%x, dpy=%p) -> fake", platform, native_display);
    (void)attrib_list; return FAKE_EGL_DISPLAY;
}

unsigned eglInitialize(void *display, int *major, int *minor) {
    if (major) *major = 1;
    if (minor) *minor = 5;
    SHIM_LOG("eglInitialize(%p) -> 1.5", display);
    return EGL_TRUE;
}

unsigned eglTerminate(void *display) { (void)display; return EGL_TRUE; }

const char *eglQueryString(void *display, int name) {
    (void)display;
    switch (name) {
    case 0x3053: return "1.5";                       /* EGL_VERSION */
    case 0x3054: return "Stub EGL";                  /* EGL_VENDOR */
    case 0x3055: return "";                          /* EGL_EXTENSIONS */
    case 0x308D: return "OpenGL_ES";                 /* EGL_CLIENT_APIS */
    default: return "";
    }
}

unsigned eglBindAPI(unsigned api) {
    SHIM_LOG("eglBindAPI(0x%x)", api);
    return EGL_TRUE;
}

unsigned eglGetConfigs(void *display, void **configs, int config_size, int *num_config) {
    (void)display;
    if (num_config) *num_config = 1;
    if (configs && config_size >= 1) configs[0] = FAKE_EGL_CONFIG;
    return EGL_TRUE;
}

unsigned eglChooseConfig(void *display, const int *attrib_list,
                         void **configs, int config_size, int *num_config) {
    (void)display; (void)attrib_list;
    if (num_config) *num_config = 1;
    if (configs && config_size >= 1) configs[0] = FAKE_EGL_CONFIG;
    return EGL_TRUE;
}

unsigned eglGetConfigAttrib(void *display, void *config, int attribute, int *value) {
    (void)display; (void)config;
    if (!value) return EGL_FALSE;
    /* EGL attribute constants from <EGL/egl.h>; my earlier guesses had
     * RED_SIZE and NATIVE_VISUAL_ID on the wrong numbers, which made the
     * game log "failed to find egl config for gbm format" because the
     * returned visual id didn't match XR24. */
    switch (attribute) {
    case 0x3020: *value = 32; break;                 /* EGL_BUFFER_SIZE */
    case 0x3021: *value = 8;  break;                 /* EGL_ALPHA_SIZE */
    case 0x3022: *value = 8;  break;                 /* EGL_BLUE_SIZE */
    case 0x3023: *value = 8;  break;                 /* EGL_GREEN_SIZE */
    case 0x3024: *value = 8;  break;                 /* EGL_RED_SIZE */
    case 0x3025: *value = 0;  break;                 /* EGL_DEPTH_SIZE */
    case 0x3026: *value = 0;  break;                 /* EGL_STENCIL_SIZE */
    case 0x3027: *value = 0x3038; break;             /* EGL_CONFIG_CAVEAT = NONE */
    case 0x3028: *value = 1;  break;                 /* EGL_CONFIG_ID */
    case 0x302E: *value = 0x34325258; break;         /* EGL_NATIVE_VISUAL_ID = XR24 (matches GBM) */
    case 0x302F: *value = 0; break;                  /* EGL_NATIVE_VISUAL_TYPE */
    case 0x3033: *value = 0x0004; break;             /* EGL_SURFACE_TYPE = WINDOW_BIT */
    case 0x3040: *value = 0x0040; break;             /* EGL_RENDERABLE_TYPE = OPENGL_ES2_BIT */
    case 0x3050: *value = 0x0040; break;             /* EGL_CONFORMANT */
    default:     *value = 0; break;
    }
    return EGL_TRUE;
}

void *eglCreateContext(void *display, void *config, void *share_context,
                       const int *attrib_list) {
    SHIM_LOG("eglCreateContext -> fake context");
    (void)display; (void)config; (void)share_context; (void)attrib_list;
    return FAKE_EGL_CONTEXT;
}

unsigned eglDestroyContext(void *display, void *context) {
    (void)display; (void)context; return EGL_TRUE;
}

void *eglCreateWindowSurface(void *display, void *config, void *native_window,
                             const int *attrib_list) {
    SHIM_LOG("eglCreateWindowSurface(native=%p) -> fake surface", native_window);
    (void)display; (void)config; (void)attrib_list;
    return FAKE_EGL_SURFACE;
}

void *eglCreatePbufferSurface(void *display, void *config, const int *attrib_list) {
    (void)display; (void)config; (void)attrib_list;
    return FAKE_EGL_SURFACE;
}

unsigned eglDestroySurface(void *display, void *surface) {
    (void)display; (void)surface; return EGL_TRUE;
}

unsigned eglMakeCurrent(void *display, void *draw, void *read, void *context) {
    SHIM_LOG("eglMakeCurrent(dpy=%p, draw=%p, read=%p, ctx=%p)",
             display, draw, read, context);
    return EGL_TRUE;
}

unsigned eglSwapBuffers(void *display, void *surface) {
    (void)display; (void)surface; return EGL_TRUE;
}

unsigned eglSwapInterval(void *display, int interval) {
    (void)display; (void)interval; return EGL_TRUE;
}

unsigned eglSurfaceAttrib(void *display, void *surface, int attribute, int value) {
    (void)display; (void)surface; (void)attribute; (void)value; return EGL_TRUE;
}

unsigned eglQuerySurface(void *display, void *surface, int attribute, int *value) {
    (void)display; (void)surface;
    if (!value) return EGL_FALSE;
    switch (attribute) {
    case 0x3057: *value = FAKE_MODE_HDISPLAY; break; /* EGL_WIDTH */
    case 0x3056: *value = FAKE_MODE_VDISPLAY; break; /* EGL_HEIGHT */
    default:     *value = 0; break;
    }
    return EGL_TRUE;
}

int eglGetError(void) { return EGL_SUCCESS; }

/* Stubs for extensions resolved via eglGetProcAddress. The game queries
 * these after eglCreateContext (per BN analysis of sub_d01d10). Each
 * returns a fake sentinel so the game's subsequent calls unblock. */

static void *stub_eglCreateImageKHR(void *display, void *ctx, unsigned target,
                                    void *buffer, const intptr_t *attrib_list) {
    (void)display; (void)ctx; (void)target; (void)buffer; (void)attrib_list;
    return FAKE_EGL_IMAGE;
}
static unsigned stub_eglDestroyImageKHR(void *display, void *image) {
    (void)display; (void)image; return EGL_TRUE;
}
static void *stub_eglCreateSyncKHR(void *display, unsigned type, const intptr_t *attrib_list) {
    (void)display; (void)type; (void)attrib_list; return FAKE_EGL_SYNC;
}
static unsigned stub_eglDestroySyncKHR(void *display, void *sync) {
    (void)display; (void)sync; return EGL_TRUE;
}
static int stub_eglWaitSyncKHR(void *display, void *sync, int flags) {
    (void)display; (void)sync; (void)flags; return EGL_TRUE;
}
static int stub_eglClientWaitSyncKHR(void *display, void *sync, int flags, uint64_t timeout) {
    (void)display; (void)sync; (void)flags; (void)timeout;
    return 0x30F6;  /* EGL_CONDITION_SATISFIED_KHR */
}
static void stub_glEGLImageTargetTexture2DOES(unsigned target, void *image) {
    (void)target; (void)image;
}

/* ---- Hooked: GLES 3.x ----
 * Pure no-op stubs to satisfy the game's rendering calls without a real
 * context. Returns are designed to be plausible enough for the game to
 * believe rendering succeeded:
 *   - glGen*  returns an incrementing handle per object
 *   - glGetShaderiv/glGetProgramiv return GL_TRUE for COMPILE_STATUS /
 *     LINK_STATUS so the Radium shader pipeline doesn't bail on "failed
 *     to compile" branches
 *   - glCheckFramebufferStatus returns GL_FRAMEBUFFER_COMPLETE
 *   - glGetError returns GL_NO_ERROR
 *   - glGetString advertises GLES 3.1 + the OES/EXT extensions the game's
 *     Texture2dDirect checks for
 *
 * When we pivot to real host GL (Step 21.8 option B), each stub body
 * gets replaced with a forwarded call; signatures stay the same.
 */

#define GL_NO_ERROR              0
#define GL_TRUE                  1
#define GL_FALSE                 0
#define GL_FRAMEBUFFER_COMPLETE  0x8CD5
#define GL_VENDOR                0x1F00
#define GL_RENDERER              0x1F01
#define GL_VERSION               0x1F02
#define GL_EXTENSIONS            0x1F03
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#define GL_COMPILE_STATUS        0x8B81
#define GL_LINK_STATUS           0x8B82
#define GL_INFO_LOG_LENGTH       0x8B84
#define GL_ACTIVE_UNIFORMS       0x8B86
#define GL_ACTIVE_ATTRIBUTES     0x8B89
#define GL_MAX_TEXTURE_SIZE      0x0D33
#define GL_MAX_VIEWPORT_DIMS     0x0D3A
#define GL_MAX_TEXTURE_IMAGE_UNITS 0x8872
#define GL_MAX_VERTEX_ATTRIBS    0x8869

static uint32_t gl_next_tex_id     = 1;
static uint32_t gl_next_buf_id     = 1;
static uint32_t gl_next_fb_id      = 1;
static uint32_t gl_next_rb_id      = 1;
static uint32_t gl_next_vao_id     = 1;
static uint32_t gl_next_shader_id  = 1;
static uint32_t gl_next_program_id = 1;

/* Logging is selective — GL calls can be extremely high-volume during
 * draw. Only the first N of each kind, and first-time calls for creation
 * functions. */
static int gl_log_budget_object_create = 40;
static int gl_log_budget_draw          = 20;
static int gl_log_budget_uniform       = 20;
static int gl_log_budget_state         = 40;
#define GLLOG(kind, fmt, ...) do { \
    if (gl_log_budget_##kind > 0) { \
        gl_log_budget_##kind--; \
        SHIM_LOG("GL " fmt, ##__VA_ARGS__); \
    } \
} while (0)

/* --- Query / error --- */

unsigned glGetError(void) { return GL_NO_ERROR; }

const unsigned char *glGetString(unsigned name) {
    switch (name) {
    case GL_VERSION:                  return (const unsigned char *)"OpenGL ES 3.1 (stub)";
    case GL_VENDOR:                   return (const unsigned char *)"shim";
    case GL_RENDERER:                 return (const unsigned char *)"shim GLES3";
    case GL_SHADING_LANGUAGE_VERSION: return (const unsigned char *)"OpenGL ES GLSL ES 3.10";
    case GL_EXTENSIONS:
        /* Advertise the OES/EXT extensions Texture2dDirect / Radium check. */
        return (const unsigned char *)
            "GL_OES_EGL_image GL_OES_EGL_image_external "
            "GL_OES_texture_float GL_OES_texture_half_float "
            "GL_OES_vertex_array_object GL_OES_mapbuffer "
            "GL_EXT_texture_format_BGRA8888 GL_EXT_discard_framebuffer "
            "GL_EXT_color_buffer_float GL_EXT_blend_minmax";
    default: return (const unsigned char *)"";
    }
}

const unsigned char *glGetStringi(unsigned name, unsigned index) {
    (void)name; (void)index; return (const unsigned char *)"";
}

void glGetIntegerv(unsigned pname, int *params) {
    if (!params) return;
    switch (pname) {
    case GL_MAX_TEXTURE_SIZE:         *params = 4096; break;
    case GL_MAX_VIEWPORT_DIMS:        params[0] = 4096; params[1] = 4096; break;
    case GL_MAX_TEXTURE_IMAGE_UNITS:  *params = 16; break;
    case GL_MAX_VERTEX_ATTRIBS:       *params = 16; break;
    default:                          *params = 0; break;
    }
}
void glGetFloatv(unsigned pname, float *params) {
    if (params) *params = 0.0f;
    (void)pname;
}
void glGetBooleanv(unsigned pname, unsigned char *params) {
    if (params) *params = GL_FALSE;
    (void)pname;
}

/* --- Object creation --- */

void glGenTextures(int n, unsigned *out) {
    for (int i = 0; i < n; i++) out[i] = gl_next_tex_id++;
    GLLOG(object_create, "glGenTextures(%d) -> [%u..]", n, n ? out[0] : 0);
}
void glDeleteTextures(int n, const unsigned *ids) { (void)n; (void)ids; }

void glGenBuffers(int n, unsigned *out) {
    for (int i = 0; i < n; i++) out[i] = gl_next_buf_id++;
    GLLOG(object_create, "glGenBuffers(%d) -> [%u..]", n, n ? out[0] : 0);
}
void glDeleteBuffers(int n, const unsigned *ids) { (void)n; (void)ids; }

void glGenFramebuffers(int n, unsigned *out) {
    for (int i = 0; i < n; i++) out[i] = gl_next_fb_id++;
    GLLOG(object_create, "glGenFramebuffers(%d) -> [%u..]", n, n ? out[0] : 0);
}
void glDeleteFramebuffers(int n, const unsigned *ids) { (void)n; (void)ids; }

void glGenRenderbuffers(int n, unsigned *out) {
    for (int i = 0; i < n; i++) out[i] = gl_next_rb_id++;
    GLLOG(object_create, "glGenRenderbuffers(%d) -> [%u..]", n, n ? out[0] : 0);
}
void glDeleteRenderbuffers(int n, const unsigned *ids) { (void)n; (void)ids; }

void glGenVertexArrays(int n, unsigned *out) {
    for (int i = 0; i < n; i++) out[i] = gl_next_vao_id++;
    GLLOG(object_create, "glGenVertexArrays(%d) -> [%u..]", n, n ? out[0] : 0);
}
void glDeleteVertexArrays(int n, const unsigned *ids) { (void)n; (void)ids; }

unsigned glCreateShader(unsigned type) {
    unsigned id = gl_next_shader_id++;
    GLLOG(object_create, "glCreateShader(type=0x%x) -> %u", type, id);
    return id;
}
void glDeleteShader(unsigned shader) { (void)shader; }

unsigned glCreateProgram(void) {
    unsigned id = gl_next_program_id++;
    GLLOG(object_create, "glCreateProgram() -> %u", id);
    return id;
}
void glDeleteProgram(unsigned program) { (void)program; }

/* --- Binding --- */

void glBindTexture(unsigned target, unsigned tex)       { (void)target; (void)tex; }
void glBindBuffer(unsigned target, unsigned buf)        { (void)target; (void)buf; }
void glBindFramebuffer(unsigned target, unsigned fb)    { (void)target; (void)fb; }
void glBindRenderbuffer(unsigned target, unsigned rb)   { (void)target; (void)rb; }
void glBindVertexArray(unsigned array)                   { (void)array; }
void glActiveTexture(unsigned tex)                       { (void)tex; }

/* --- Texture data / params --- */

void glTexImage2D(unsigned target, int level, int internalfmt,
                  int w, int h, int border, unsigned format, unsigned type,
                  const void *data) {
    GLLOG(object_create, "glTexImage2D %dx%d internal=0x%x fmt=0x%x type=0x%x data=%s",
          w, h, internalfmt, format, type, data ? "<ptr>" : "NULL");
    (void)target; (void)level; (void)border;
}
void glTexSubImage2D(unsigned target, int level, int xoff, int yoff,
                     int w, int h, unsigned format, unsigned type,
                     const void *data) {
    (void)target; (void)level; (void)xoff; (void)yoff;
    (void)w; (void)h; (void)format; (void)type; (void)data;
}
void glCompressedTexImage2D(unsigned target, int level, unsigned internalfmt,
                            int w, int h, int border, int size, const void *data) {
    (void)target; (void)level; (void)internalfmt;
    (void)w; (void)h; (void)border; (void)size; (void)data;
}
void glTexParameteri(unsigned target, unsigned pname, int param) {
    (void)target; (void)pname; (void)param;
}
void glTexParameterf(unsigned target, unsigned pname, float param) {
    (void)target; (void)pname; (void)param;
}
void glPixelStorei(unsigned pname, int param) { (void)pname; (void)param; }
void glGenerateMipmap(unsigned target) { (void)target; }

/* --- Buffer data --- */

void glBufferData(unsigned target, intptr_t size, const void *data, unsigned usage) {
    (void)target; (void)size; (void)data; (void)usage;
}
void glBufferSubData(unsigned target, intptr_t offset, intptr_t size, const void *data) {
    (void)target; (void)offset; (void)size; (void)data;
}
void *glMapBufferRange(unsigned target, intptr_t offset, intptr_t len, unsigned access) {
    (void)target; (void)offset; (void)len; (void)access; return NULL;
}
unsigned char glUnmapBuffer(unsigned target) { (void)target; return GL_TRUE; }

/* --- Shader --- */

void glShaderSource(unsigned shader, int count, const char *const *string, const int *length) {
    (void)shader; (void)count; (void)string; (void)length;
}
void glCompileShader(unsigned shader) { (void)shader; }
void glAttachShader(unsigned program, unsigned shader) { (void)program; (void)shader; }
void glDetachShader(unsigned program, unsigned shader) { (void)program; (void)shader; }
void glLinkProgram(unsigned program) { (void)program; }
void glValidateProgram(unsigned program) { (void)program; }
void glUseProgram(unsigned program) { (void)program; }

void glGetShaderiv(unsigned shader, unsigned pname, int *params) {
    (void)shader;
    if (!params) return;
    /* Claim success for every status query — keeps Radium from bailing. */
    switch (pname) {
    case GL_COMPILE_STATUS:  *params = GL_TRUE; break;
    case GL_INFO_LOG_LENGTH: *params = 0; break;
    default:                 *params = 0; break;
    }
}
void glGetProgramiv(unsigned program, unsigned pname, int *params) {
    (void)program;
    if (!params) return;
    switch (pname) {
    case GL_LINK_STATUS:      *params = GL_TRUE; break;
    case GL_ACTIVE_UNIFORMS:  *params = 0; break;
    case GL_ACTIVE_ATTRIBUTES:*params = 0; break;
    case GL_INFO_LOG_LENGTH:  *params = 0; break;
    default:                  *params = 0; break;
    }
}
void glGetShaderInfoLog(unsigned shader, int max, int *len, char *log) {
    (void)shader; (void)max;
    if (len) *len = 0;
    if (log && max > 0) log[0] = 0;
}
void glGetProgramInfoLog(unsigned program, int max, int *len, char *log) {
    (void)program; (void)max;
    if (len) *len = 0;
    if (log && max > 0) log[0] = 0;
}

int glGetUniformLocation(unsigned program, const char *name) {
    (void)program; (void)name; return 0;
}
int glGetAttribLocation(unsigned program, const char *name) {
    (void)program; (void)name; return 0;
}
void glBindAttribLocation(unsigned program, unsigned index, const char *name) {
    (void)program; (void)index; (void)name;
}

/* Uniforms — many variants, all no-op */
#define GL_UNIFORM_1(n, t) void glUniform1##n(int loc, t v0) { \
    (void)loc; (void)v0; GLLOG(uniform, "glUniform1" #n); }
#define GL_UNIFORM_2(n, t) void glUniform2##n(int loc, t v0, t v1) { \
    (void)loc; (void)v0; (void)v1; GLLOG(uniform, "glUniform2" #n); }
#define GL_UNIFORM_3(n, t) void glUniform3##n(int loc, t v0, t v1, t v2) { \
    (void)loc; (void)v0; (void)v1; (void)v2; GLLOG(uniform, "glUniform3" #n); }
#define GL_UNIFORM_4(n, t) void glUniform4##n(int loc, t v0, t v1, t v2, t v3) { \
    (void)loc; (void)v0; (void)v1; (void)v2; (void)v3; GLLOG(uniform, "glUniform4" #n); }
GL_UNIFORM_1(i, int) GL_UNIFORM_2(i, int) GL_UNIFORM_3(i, int) GL_UNIFORM_4(i, int)
GL_UNIFORM_1(f, float) GL_UNIFORM_2(f, float) GL_UNIFORM_3(f, float) GL_UNIFORM_4(f, float)
void glUniform1iv(int loc, int count, const int *v)   { (void)loc; (void)count; (void)v; }
void glUniform1fv(int loc, int count, const float *v) { (void)loc; (void)count; (void)v; }
void glUniform2fv(int loc, int count, const float *v) { (void)loc; (void)count; (void)v; }
void glUniform3fv(int loc, int count, const float *v) { (void)loc; (void)count; (void)v; }
void glUniform4fv(int loc, int count, const float *v) { (void)loc; (void)count; (void)v; }
void glUniformMatrix3fv(int loc, int count, unsigned char trans, const float *v) {
    (void)loc; (void)count; (void)trans; (void)v;
}
void glUniformMatrix4fv(int loc, int count, unsigned char trans, const float *v) {
    (void)loc; (void)count; (void)trans; (void)v;
}

/* Vertex attribs */
void glVertexAttribPointer(unsigned idx, int size, unsigned type, unsigned char norm,
                           int stride, const void *ptr) {
    (void)idx; (void)size; (void)type; (void)norm; (void)stride; (void)ptr;
}
void glEnableVertexAttribArray(unsigned idx)  { (void)idx; }
void glDisableVertexAttribArray(unsigned idx) { (void)idx; }

/* --- Framebuffer --- */

void glFramebufferTexture2D(unsigned target, unsigned att, unsigned ttarget,
                            unsigned tex, int level) {
    (void)target; (void)att; (void)ttarget; (void)tex; (void)level;
}
void glFramebufferRenderbuffer(unsigned target, unsigned att, unsigned rbtarget, unsigned rb) {
    (void)target; (void)att; (void)rbtarget; (void)rb;
}
void glRenderbufferStorage(unsigned target, unsigned internalfmt, int w, int h) {
    (void)target; (void)internalfmt; (void)w; (void)h;
}
unsigned glCheckFramebufferStatus(unsigned target) {
    (void)target; return GL_FRAMEBUFFER_COMPLETE;
}

/* --- State --- */

void glEnable(unsigned cap)  { GLLOG(state, "glEnable(0x%x)",  cap); }
void glDisable(unsigned cap) { GLLOG(state, "glDisable(0x%x)", cap); }
unsigned char glIsEnabled(unsigned cap) { (void)cap; return GL_FALSE; }
void glBlendFunc(unsigned src, unsigned dst) { (void)src; (void)dst; }
void glBlendFuncSeparate(unsigned srcRGB, unsigned dstRGB, unsigned srcA, unsigned dstA) {
    (void)srcRGB; (void)dstRGB; (void)srcA; (void)dstA;
}
void glBlendEquation(unsigned mode) { (void)mode; }
void glBlendEquationSeparate(unsigned rgb, unsigned a) { (void)rgb; (void)a; }
void glBlendColor(float r, float g, float b, float a) {
    (void)r; (void)g; (void)b; (void)a;
}
void glViewport(int x, int y, int w, int h) {
    (void)x; (void)y; (void)w; (void)h;
}
void glScissor(int x, int y, int w, int h) {
    (void)x; (void)y; (void)w; (void)h;
}
void glDepthFunc(unsigned func) { (void)func; }
void glDepthMask(unsigned char mask) { (void)mask; }
void glDepthRangef(float n, float f) { (void)n; (void)f; }
void glCullFace(unsigned mode) { (void)mode; }
void glFrontFace(unsigned mode) { (void)mode; }
void glStencilFunc(unsigned func, int ref, unsigned mask) {
    (void)func; (void)ref; (void)mask;
}
void glStencilMask(unsigned mask) { (void)mask; }
void glStencilOp(unsigned f, unsigned zf, unsigned zp) { (void)f; (void)zf; (void)zp; }
void glColorMask(unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
    (void)r; (void)g; (void)b; (void)a;
}
void glLineWidth(float w) { (void)w; }
void glPolygonOffset(float f, float u) { (void)f; (void)u; }
void glClearColor(float r, float g, float b, float a) {
    (void)r; (void)g; (void)b; (void)a;
}
void glClearDepthf(float d) { (void)d; }
void glClearStencil(int s)  { (void)s; }
void glClear(unsigned mask) { (void)mask; }

/* --- Draw --- */

void glDrawArrays(unsigned mode, int first, int count) {
    (void)mode; (void)first;
    GLLOG(draw, "glDrawArrays(count=%d)", count);
}
void glDrawElements(unsigned mode, int count, unsigned type, const void *idx) {
    (void)mode; (void)type; (void)idx;
    GLLOG(draw, "glDrawElements(count=%d)", count);
}
void glDrawArraysInstanced(unsigned mode, int first, int count, int inst) {
    (void)mode; (void)first; (void)count; (void)inst;
}
void glDrawElementsInstanced(unsigned mode, int count, unsigned type, const void *idx, int inst) {
    (void)mode; (void)count; (void)type; (void)idx; (void)inst;
}
void glVertexAttribDivisor(unsigned idx, unsigned divisor) { (void)idx; (void)divisor; }

/* --- Sync --- */

void glFinish(void) {}
void glFlush(void) {}
void glReadPixels(int x, int y, int w, int h, unsigned fmt, unsigned type, void *data) {
    (void)x; (void)y; (void)w; (void)h; (void)fmt; (void)type;
    if (data) memset(data, 0, (size_t)w * (size_t)h * 4);
}

/* --- end GL stubs --- */

void (*eglGetProcAddress(const char *name))(void) {
    /* Funnel every game-queried extension through our stubs. These match
     * the names BN showed the game resolving inside glWindow::setGlContext
     * (KHR image/sync + GL_OES_EGL_image) plus eglGetPlatformDisplayEXT
     * itself (the game queries it before calling it, and falls back if
     * it's NULL). */
    typedef void (*pfn_t)(void);
    struct { const char *name; pfn_t fn; } table[] = {
        { "eglGetPlatformDisplayEXT",         (pfn_t)eglGetPlatformDisplayEXT },
        { "eglGetPlatformDisplay",            (pfn_t)eglGetPlatformDisplay },
        { "eglCreateImageKHR",                (pfn_t)stub_eglCreateImageKHR },
        { "eglDestroyImageKHR",               (pfn_t)stub_eglDestroyImageKHR },
        { "eglCreateSyncKHR",                 (pfn_t)stub_eglCreateSyncKHR },
        { "eglDestroySyncKHR",                (pfn_t)stub_eglDestroySyncKHR },
        { "eglWaitSyncKHR",                   (pfn_t)stub_eglWaitSyncKHR },
        { "eglClientWaitSyncKHR",             (pfn_t)stub_eglClientWaitSyncKHR },
        { "glEGLImageTargetTexture2DOES",     (pfn_t)stub_glEGLImageTargetTexture2DOES },
        { NULL, NULL },
    };
    for (int i = 0; table[i].name; i++) {
        if (strcmp(table[i].name, name) == 0) {
            SHIM_LOG("eglGetProcAddress(\"%s\") -> stub %p", name, (void *)table[i].fn);
            return table[i].fn;
        }
    }
    SHIM_LOG("eglGetProcAddress(\"%s\") -> NULL (no stub)", name);
    return NULL;
}

/* ---- Hooked: termios wrappers ----
 * glibc's tcsetattr / tcgetattr / tcflush / tcflow / tcdrain / tcsendbreak
 * use INLINE_SYSCALL to invoke the ioctl(TCSETS/TCGETS/TCFLSH/...) syscall
 * DIRECTLY, bypassing the libc ioctl() wrapper. Our LD_PRELOAD of ioctl()
 * therefore never fires for these calls — same bug class as access() /
 * faccessat(). On our fake UART fd (1021), QEMU passes the syscall to the
 * host kernel, which returns -EBADF because 1021 isn't a real fd, causing
 * sub_cf7c10 to close and return -1.
 *
 * Fix: hook the termios functions at the libc level. Return success for
 * our fake UART fd; pass through to real functions for everything else.
 */

/* ---- Diagnostic: log errno at perror() call site ---- */
void perror(const char *s) {
    typedef void (*fn_t)(const char *);
    static fn_t real_fn = NULL;
    if (!real_fn) real_fn = (fn_t)dlsym(RTLD_NEXT, "perror");
    int saved = errno;
    char msg[200];
    int n = snprintf(msg, sizeof(msg), "[shim] perror('%s') errno=%d\n",
                     s ? s : "(null)", saved);
    if (n > 0) (void)!real_write(2, msg, n);
    errno = saved;
    real_fn(s);
}

/* Pragmatic approach: if something inside sub_d19580 is legitimately
 * setting errno=22 during its success-ish path (the Spike 1/2 fallback is
 * not a fatal error — the game intends to continue), clearing errno right
 * before sub_a75ae0's perror call means perror will just print "Success".
 * But we can't hook sub_d19580 directly (internal symbol). Instead, clear
 * errno in our perror hook when we see the specific "failed to initialize
 * bridge" message — because we've proven this branch is taken when the
 * pwrdist fallback fires, which is the expected no-powerdist-board case.
 *
 * Actually the cleaner fix: sub_d19580 should return 0 on the "no powerdist
 * but that's OK" path. It returns -6, making sub_a75ae0 treat it as
 * failure. The underlying errno=22 is incidental.
 *
 * For now, just log diagnostically and leave behavior alone. */

int tcsetattr(int fd, int optional_actions, const struct termios *termios_p) {
    if (is_uart_fd(fd)) {
        SHIM_LOG("UART tcsetattr(actions=%d) -> fake OK", optional_actions);
        return 0;
    }
    typedef int (*fn_t)(int, int, const struct termios *);
    static fn_t real_fn = NULL;
    if (!real_fn) real_fn = (fn_t)dlsym(RTLD_NEXT, "tcsetattr");
    return real_fn(fd, optional_actions, termios_p);
}

int tcgetattr(int fd, struct termios *termios_p) {
    if (is_uart_fd(fd)) {
        if (termios_p) memset(termios_p, 0, sizeof(*termios_p));
        SHIM_LOG("UART tcgetattr -> fake OK (zeroed termios)");
        return 0;
    }
    typedef int (*fn_t)(int, struct termios *);
    static fn_t real_fn = NULL;
    if (!real_fn) real_fn = (fn_t)dlsym(RTLD_NEXT, "tcgetattr");
    return real_fn(fd, termios_p);
}

int tcflush(int fd, int queue_selector) {
    if (is_uart_fd(fd)) {
        SHIM_LOG("UART tcflush(q=%d) -> fake OK", queue_selector);
        return 0;
    }
    typedef int (*fn_t)(int, int);
    static fn_t real_fn = NULL;
    if (!real_fn) real_fn = (fn_t)dlsym(RTLD_NEXT, "tcflush");
    return real_fn(fd, queue_selector);
}

int tcflow(int fd, int action) {
    if (is_uart_fd(fd)) {
        SHIM_LOG("UART tcflow(action=%d) -> fake OK", action);
        return 0;
    }
    typedef int (*fn_t)(int, int);
    static fn_t real_fn = NULL;
    if (!real_fn) real_fn = (fn_t)dlsym(RTLD_NEXT, "tcflow");
    return real_fn(fd, action);
}

int tcdrain(int fd) {
    if (is_uart_fd(fd)) {
        SHIM_LOG("UART tcdrain -> fake OK");
        return 0;
    }
    typedef int (*fn_t)(int);
    static fn_t real_fn = NULL;
    if (!real_fn) real_fn = (fn_t)dlsym(RTLD_NEXT, "tcdrain");
    return real_fn(fd);
}

int tcsendbreak(int fd, int duration) {
    if (is_uart_fd(fd)) {
        SHIM_LOG("UART tcsendbreak(dur=%d) -> fake OK", duration);
        return 0;
    }
    typedef int (*fn_t)(int, int);
    static fn_t real_fn = NULL;
    if (!real_fn) real_fn = (fn_t)dlsym(RTLD_NEXT, "tcsendbreak");
    return real_fn(fd, duration);
}

/* cfmakeraw / cfsetspeed / cfsetispeed / cfsetospeed are pure userspace —
 * they modify the termios struct in memory and do not make syscalls.
 * No hook needed. */

/* ---- Hooked: close ---- */

int close(int fd) {
    resolve_all();
    if (fd == FAKE_FD_SPI && spi_active) {
        SHIM_LOG("CLOSE SPI (fd %d) after %u transfers", fd, spi_transfer_count);
        spi_active = 0;
        return 0;
    }
    if (fd == FAKE_FD_I2C && i2c_active) {
        SHIM_LOG("CLOSE I2C (fd %d) after %u ioctls", fd, i2c_ioctl_count);
        i2c_active = 0;
        return 0;
    }
    if (is_uart_fd(fd)) {
        SHIM_LOG("CLOSE UART (game=%d shim=%d) after %u reads, %u writes",
                 uart_game_fd, uart_shim_fd, uart_read_count, uart_write_count);
        int shim_fd = uart_shim_fd;
        int game_fd = uart_game_fd;
        uart_game_fd = -1;
        uart_shim_fd = -1;
        real_close(shim_fd);
        return real_close(game_fd);
    }
    return real_close(fd);
}

/* ---- Hooked: read ----
 *
 * UART reads go through to the real socketpair fd — they block until the
 * shim pushes bytes via uart_shim_fd (or forever, which is fine; the game
 * has its own pthread_cond_timedwait around these reads). We log a tally
 * when bytes actually arrive.
 */

ssize_t read(int fd, void *buf, size_t count) {
    resolve_all();

    if (is_uart_fd(fd)) {
        ssize_t n = real_read(fd, buf, count);
        uart_read_count++;
        if (n > 0 && (uart_read_count <= 10 || (uart_read_count % 1000) == 0)) {
            const uint8_t *data = (const uint8_t *)buf;
            fprintf(stderr, "[shim] UART read #%u got %zd bytes:", uart_read_count, n);
            for (ssize_t i = 0; i < n && i < 32; i++) fprintf(stderr, " %02x", data[i]);
            if (n > 32) fprintf(stderr, " ...");
            fprintf(stderr, "\n");
        }
        return n;
    }

    if (is_fake_fd(fd)) {
        /* SPI/I2C shouldn't be read() directly */
        SHIM_LOG("read(fd=%d, %zu bytes) on fake device — returning 0", fd, count);
        return 0;
    }

    return real_read(fd, buf, count);
}

/* ---- Hooked: write ---- */

/* ---- Bridge protocol response synthesis ----
 *
 * Per Sidekick Round 4 RE of sub_d164a0 (send_bridge_message_with_response)
 * and the parser in sub_d15aa8:
 *
 *   Request frame:  [0x02][CMD][ID][payload...][CKSUM][0x03]
 *   Response frame: [0x02][RESP_CMD][ID][payload...][CKSUM][0x03]
 *   Checksum:       CMD + ID + payload... + CKSUM == 0 (mod 256)
 *   ID:             Channel ID; response MUST echo the request's ID
 *
 * Request-to-response CMD mapping and payload size:
 *   0x10 (pwrdist version query) -> 0x00 (7 payload bytes)
 *   0x20 (hw version query)      -> 0x10 (4 payload bytes)
 *   0x30 (expected CRC query)    -> 0x20 (4 payload bytes)
 *
 * For 0x30 we return the zlib CRC32 of netbridge.elf's .text+.rodata+
 * .binary_info sections, computed offline in Step 13.5 (journal).
 *
 * For 0x10 we return seven zero bytes. sub_d19580 interprets a zero
 * version as "Spike 1/2 board — use fallback, don't flash powerdist" —
 * which is exactly what we want in emulation (no real powerdist to flash).
 *
 * For 0x20 we return four zero bytes; the game stores the value into
 * data_490e644/data_490e648 without (as far as we've seen) validating.
 */

#define NETBRIDGE_CRC32  0x357f5327u

static uint8_t bridge_checksum(const uint8_t *bytes, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) sum += bytes[i];
    return (uint8_t)(-(int32_t)sum);
}

/* ---- Bridge frame escape encoding ----
 *
 * The Spike 3 bridge protocol reserves three byte values for framing:
 *   0x02 = STX (start of frame)
 *   0x03 = ETX (end of frame)
 *   0x1b = ESC (next data byte has bit 7 cleared)
 *
 * Any CMD/ID/PAYLOAD/CKSUM byte that equals one of these three values is
 * transmitted as `0x1b (byte | 0x80)`. The receiver's byte-by-byte parser
 * in sub_d15aa8 (see file 0x915c38 — the `cmp w0, #0x1b` and the
 * `bic w0, w0, w26` at 0x915c10) un-escapes by AND-ing with ~0x80.
 *
 * We discovered this via cmd 0x18 responses where id=0x02 (channel 2) was
 * sent by the game as `02 18 1b 82 cksum 03` (escape-encoded). Our naive
 * req[2] extraction read 0x1b as the channel and sent back a malformed
 * frame, which the receiver then rejected with "bridge rx checksum error".
 */

/* Decode an incoming escape-encoded frame into its logical bytes. Returns
 * decoded length including STX/ETX, or -1 on malformed input. */
static int bridge_decode_escape(const uint8_t *raw, size_t raw_len,
                                uint8_t *out, size_t out_cap) {
    if (raw_len < 5 || raw[0] != 0x02 || raw[raw_len - 1] != 0x03) return -1;
    if (out_cap < 2) return -1;
    out[0] = 0x02;
    size_t o = 1;
    for (size_t i = 1; i < raw_len - 1; i++) {
        if (o >= out_cap - 1) return -1;
        if (raw[i] == 0x1b && i + 1 < raw_len - 1) {
            out[o++] = raw[i + 1] & 0x7f;
            i++;
        } else {
            out[o++] = raw[i];
        }
    }
    out[o++] = 0x03;
    return (int)o;
}

/* Encode a logical frame into its on-wire (escape-encoded) form. Returns
 * encoded length. STX (byte 0) and ETX (last byte) are emitted literally;
 * any 0x02/0x03/0x1b byte between them is prefixed with 0x1b. */
static int bridge_encode_escape(const uint8_t *logical, size_t logical_len,
                                uint8_t *out, size_t out_cap) {
    if (logical_len < 2 || logical[0] != 0x02 ||
        logical[logical_len - 1] != 0x03 || out_cap < 1) return -1;
    out[0] = 0x02;
    size_t o = 1;
    for (size_t i = 1; i < logical_len - 1; i++) {
        uint8_t b = logical[i];
        if (b == 0x02 || b == 0x03 || b == 0x1b) {
            if (o + 2 >= out_cap) return -1;
            out[o++] = 0x1b;
            out[o++] = b | 0x80;
        } else {
            if (o + 1 >= out_cap) return -1;
            out[o++] = b;
        }
    }
    if (o + 1 > out_cap) return -1;
    out[o++] = 0x03;
    return (int)o;
}

static void bridge_send_response(const uint8_t *resp, size_t resp_len,
                                 uint8_t req_cmd, uint8_t id) {
    if (uart_shim_fd < 0) return;
    fprintf(stderr, "[shim] bridge RESP to cmd=0x%02x id=0x%02x (%zu logical bytes):",
            req_cmd, id, resp_len);
    for (size_t i = 0; i < resp_len; i++) fprintf(stderr, " %02x", resp[i]);
    fprintf(stderr, "\n");
    uint8_t wire[64];
    int wire_len = bridge_encode_escape(resp, resp_len, wire, sizeof(wire));
    if (wire_len < 0) {
        SHIM_LOG("bridge: encode_escape failed");
        return;
    }
    if ((size_t)wire_len != resp_len) {
        fprintf(stderr, "[shim]   (escape-encoded to %d bytes:", wire_len);
        for (int i = 0; i < wire_len; i++) fprintf(stderr, " %02x", wire[i]);
        fprintf(stderr, ")\n");
    }
    ssize_t n = real_write(uart_shim_fd, wire, wire_len);
    if (n < 0) SHIM_LOG("bridge: real_write response failed: %s", strerror(errno));
}

static void synthesize_bridge_response(const uint8_t *raw_req, size_t raw_len) {
    uint8_t req[64];
    int req_len = bridge_decode_escape(raw_req, raw_len, req, sizeof(req));
    if (req_len < 5) return;
    uint8_t cmd = req[1];
    uint8_t id  = req[2];

    uint8_t resp[16];

    switch (cmd) {
    case 0x30: {
        /* Response 0x20 with 4-byte expected CRC (little-endian). */
        resp[0] = 0x02;
        resp[1] = 0x20;
        resp[2] = id;
        resp[3] = (uint8_t)(NETBRIDGE_CRC32 >>  0);
        resp[4] = (uint8_t)(NETBRIDGE_CRC32 >>  8);
        resp[5] = (uint8_t)(NETBRIDGE_CRC32 >> 16);
        resp[6] = (uint8_t)(NETBRIDGE_CRC32 >> 24);
        resp[7] = bridge_checksum(resp + 1, 6);
        resp[8] = 0x03;
        bridge_send_response(resp, 9, cmd, id);
        return;
    }
    case 0x20: {
        /* Response 0x10 with 4-byte hardware version (all zeros for now). */
        resp[0] = 0x02;
        resp[1] = 0x10;
        resp[2] = id;
        resp[3] = 0x00; resp[4] = 0x00; resp[5] = 0x00; resp[6] = 0x00;
        resp[7] = bridge_checksum(resp + 1, 6);
        resp[8] = 0x03;
        bridge_send_response(resp, 9, cmd, id);
        return;
    }
    case 0x10: {
        /* Response cmd 0x10 with 7-byte pwrdist version payload.
         *
         * IMPORTANT: the response cmd must be in range [0x10, 0x41]. The
         * dispatch in sub_d15aa8 checks (cmd - 0x10) <= 0x31; anything
         * outside that range falls through to "BRIDGE_ProcessMessage
         * unknown command" which still signals the waiting channel (so
         * send_bridge_message_with_response returns 0) but the dispatch
         * handler that writes data_490e61c never runs. Sidekick's earlier
         * "0x10 -> 0x00" mapping turned out to be off; the actual response
         * cmd for pwrdist version is 0x10 (matches the request cmd).
         *
         * data_490e61c is set by the dispatch handler as roughly
         * (payload[0] << 16) | (payload[1] << 8). Non-zero value puts the
         * game on the "has Spike 3 pwrdist board" path, which calls
         * sub_d18c20("powerdist_ext.hex", 0). If sub_d18c20 shells out to
         * openocd, our system() hook returns 0 and the flash "succeeds",
         * then the game re-queries (we return the same response), ver
         * stays non-zero, sub_d19580 returns 0 (x19 untouched from its
         * initial 0). */
        resp[0]  = 0x02;
        resp[1]  = 0x10;
        resp[2]  = id;
        resp[3]  = 0x01;
        resp[4]  = 0x00;
        resp[5]  = 0x00;
        resp[6]  = 0x00;
        resp[7]  = 0x00;
        resp[8]  = 0x00;
        resp[9]  = 0x00;
        resp[10] = bridge_checksum(resp + 1, 9);
        resp[11] = 0x03;
        bridge_send_response(resp, 12, cmd, id);
        return;
    }
    case 0x15: {
        /* Pwrdist status (follow-up after 0x10). Handler at file 0x9160a0
         * requires exactly 1 payload byte; OR/XORs it into state word at
         * data_490e5b0+128. First guess: 0x00 = "all clear, no faults". */
        resp[0] = 0x02;
        resp[1] = 0x15;
        resp[2] = id;
        resp[3] = 0x00;
        resp[4] = bridge_checksum(resp + 1, 3);
        resp[5] = 0x03;
        bridge_send_response(resp, 6, cmd, id);
        return;
    }
    case 0x18: {
        /* Pwrdist followup #2. Handler at file 0x91607c records min(existing,
         * payload_len) into a halfword table at data_490e5b0+0x88[id]; the
         * value isn't read. Send minimal 1-byte payload with value 0x00. */
        resp[0] = 0x02;
        resp[1] = 0x18;
        resp[2] = id;
        resp[3] = 0x00;
        resp[4] = bridge_checksum(resp + 1, 3);
        resp[5] = 0x03;
        bridge_send_response(resp, 6, cmd, id);
        return;
    }
    case 0x1c: {
        /* Pwrdist firmware flash chunk write. Jump table routes cmd 0x1c to
         * the generic "silent no-op + ACK" handler at file 0x915d28 (shared
         * with cmds 0x11/0x13/0x16/0x17/0x19/0x21/0x23–0x25/0x30/0x31). A
         * zero-payload frame takes the cbnz-w27 path and signals the waiting
         * channel, which is what send_bridge_message_with_response wants to
         * unblock. Game treats that as "chunk write acknowledged." */
        resp[0] = 0x02;
        resp[1] = 0x1c;
        resp[2] = id;
        resp[3] = bridge_checksum(resp + 1, 2);
        resp[4] = 0x03;
        bridge_send_response(resp, 5, cmd, id);
        return;
    }
    case 0x11: {
        /* Jump table routes cmd 0x11 to the generic silent-ACK fallback at
         * 0x915d28 (same family as 0x1c). Zero-payload frame unblocks the
         * channel; no state is written. */
        resp[0] = 0x02;
        resp[1] = 0x11;
        resp[2] = id;
        resp[3] = bridge_checksum(resp + 1, 2);
        resp[4] = 0x03;
        bridge_send_response(resp, 5, cmd, id);
        return;
    }
    case 0x16: {
        /* Cmd 0x16 routes to the shared handler path at 0x915d28. That path
         * has two branches:
         *   - payload_len == 0 AND id == 0: pure ACK, no buffer write.
         *   - payload_len > 0: falls to 0x915e8c which memcpys the payload
         *     into the channel's rx_buf registered at
         *     data_490e5b0+0xa0+id*8.
         * The caller at 0x675c20 (wrapper sub_917b60) sends cmd 0x16 with a
         * 1-byte tx payload (0x00) and expects a 4-byte response. It then
         * polls up to 1000×1ms waiting for:
         *   return value (rx[0]) > 0  AND  rx[2] == 0x02
         * Values for rx[1] and rx[3] are stored to caller out-ptrs but not
         * tested. Send `01 00 02 00` so the poll exits immediately. */
        resp[0] = 0x02;
        resp[1] = 0x16;
        resp[2] = id;
        resp[3] = 0x01;
        resp[4] = 0x00;
        resp[5] = 0x02;
        resp[6] = 0x00;
        resp[7] = bridge_checksum(resp + 1, 6);
        resp[8] = 0x03;
        bridge_send_response(resp, 9, cmd, id);
        return;
    }
    case 0x1d: {
        /* Dedicated handler at 0x916028: `cmp w27,#4 / b.eq 0x915e88`. If
         * payload_len == 4 it memcpys into the per-id buffer at
         * data_490e5b0+0xa0+id*8; otherwise it falls through to the silent-
         * ACK fallback at 0x915d28. Sending 0-byte payload takes the else
         * path — unblocks the waiting channel without writing stale data
         * into a buffer whose semantics we haven't yet verified (plausibly
         * pwrdist status bits + rail state + ADC reading, per FotE §4.16). */
        resp[0] = 0x02;
        resp[1] = 0x1d;
        resp[2] = id;
        resp[3] = bridge_checksum(resp + 1, 2);
        resp[4] = 0x03;
        bridge_send_response(resp, 5, cmd, id);
        return;
    }
    case 0x27: {
        /* Four game-side callsites polling a 1-byte response byte:
         *   0x916828 one-shot getter (stores resp[0] to caller out-ptr)
         *   0x916fe0 loop: spin until bit 2 == 1
         *   0x917020 loop: spin until bit 2 == 0  (paired with 916fe0)
         *   0x917264 loop: spin (100ms × ≤100) until (resp & 3) == 2
         * The paired loop at 916fe0/917020 requires bit 2 to toggle 1→0
         * within the same function call, so a static response stalls one
         * of the two halves. Alternate odd/even calls:
         *   odd  -> 0x06 (bits 1,2 set; resp&3 == 2)
         *   even -> 0x02 (bit 1 set only; resp&3 == 2)
         * Both values satisfy 917264; alternation resolves the pair. */
        static unsigned int cmd27_count = 0;
        cmd27_count++;
        uint8_t status = (cmd27_count & 1) ? 0x06 : 0x02;
        resp[0] = 0x02;
        resp[1] = 0x27;
        resp[2] = id;
        resp[3] = status;
        resp[4] = bridge_checksum(resp + 1, 3);
        resp[5] = 0x03;
        bridge_send_response(resp, 6, cmd, id);
        SHIM_LOG("bridge: cmd 0x27 #%u -> status 0x%02x (id=0x%02x)",
                 cmd27_count, status, id);
        return;
    }
    default:
        SHIM_LOG("bridge: no synthetic response for cmd 0x%02x", cmd);
        return;
    }
}

ssize_t write(int fd, const void *buf, size_t count) {
    resolve_all();

    if (is_uart_fd(fd)) {
        uart_write_count++;
        /* Log every bridge frame. Unlike SPI these are low-volume (~a few
         * hundred per run during init), and knowing each request payload
         * matters for diagnosing polling loops on cmd 0x16 / 0x27 / etc. */
        const uint8_t *data = (const uint8_t *)buf;
        fprintf(stderr, "[shim] UART write #%u (%zu bytes):", uart_write_count, count);
        for (size_t i = 0; i < count && i < 32; i++)
            fprintf(stderr, " %02x", data[i]);
        if (count > 32) fprintf(stderr, " ...");
        fprintf(stderr, "\n");
        ssize_t ret = real_write(fd, buf, count);
        /* Save/restore errno around response synthesis so our extra
         * real_write() into uart_shim_fd can't pollute the game's view of
         * the primary write(). (The previous drain-via-real_read also had
         * this problem because non-blocking read on an empty socket sets
         * errno=EAGAIN, which leaked out as "failed to initialize bridge:
         * Resource temporarily unavailable".) */
        int saved_errno = errno;
        synthesize_bridge_response((const uint8_t *)buf, count);
        errno = saved_errno;
        return ret;
    }

    if (fd == FAKE_FD_SPI && spi_active) {
        /* SPI shouldn't use write() directly (uses ioctl), but just in case */
        return (ssize_t)count;
    }

    return real_write(fd, buf, count);
}

/* ---- Hooked: ioctl ---- */

int ioctl(int fd, unsigned long request, ...) {
    resolve_all();

    va_list args;
    va_start(args, request);
    void *arg = va_arg(args, void *);
    va_end(args);

    /* ---- UART (real socketpair fd) — intercept the termios ioctls that
     *      glibc tcXXX functions issue, plus FIONREAD/TIOCOUTQ which the
     *      game calls directly; pass anything else through to the real fd. */
    if (is_uart_fd(fd)) {
        switch (request) {
        case TCGETS: {
            struct termios *t = (struct termios *)arg;
            if (t) memset(t, 0, sizeof(*t));
            SHIM_LOG("UART ioctl TCGETS");
            return 0;
        }
        case TCSETS:
        case TCSETSW:
        case TCSETSF:
            SHIM_LOG("UART ioctl TCSETS/W/F (configure serial)");
            return 0;
        case TIOCMGET: {
            int *bits = (int *)arg;
            if (bits) *bits = 0x60;  /* TIOCM_CAR | TIOCM_CTS */
            SHIM_LOG("UART ioctl TIOCMGET -> 0x60");
            return 0;
        }
        case TIOCMSET:
            SHIM_LOG("UART ioctl TIOCMSET");
            return 0;
        case 0x541e: { /* FIONREAD */
            int *n = (int *)arg;
            if (n) *n = 0;
            SHIM_LOG("UART ioctl FIONREAD -> 0");
            return 0;
        }
        case 0x541f: { /* TIOCOUTQ */
            int *n = (int *)arg;
            if (n) *n = 0;
            SHIM_LOG("UART ioctl TIOCOUTQ -> 0");
            return 0;
        }
        default:
            SHIM_LOG("UART ioctl 0x%lx (passthrough)", request);
            return real_ioctl(fd, request, arg);
        }
    }

    /* ---- Pass through non-fake fds ---- */
    if (!is_fake_fd(fd)) {
        return real_ioctl(fd, request, arg);
    }

    /* ---- SPI ioctls ---- */
    if (fd == FAKE_FD_SPI) {
        switch (request) {
        case SPI_IOC_WR_MAX_SPEED_HZ: {
            uint32_t speed = *(uint32_t *)arg;
            SHIM_LOG("SPI ioctl WR_MAX_SPEED_HZ = %u Hz", speed);
            return 0;
        }
        case SPI_IOC_WR_MODE: {
            uint8_t mode = *(uint8_t *)arg;
            SHIM_LOG("SPI ioctl WR_MODE = %u", mode);
            return 0;
        }
        case SPI_IOC_MESSAGE_1: {
            struct spi_ioc_transfer *xfer = (struct spi_ioc_transfer *)arg;
            uint8_t *tx = (uint8_t *)(uintptr_t)xfer->tx_buf;
            uint8_t *rx = (uint8_t *)(uintptr_t)xfer->rx_buf;
            uint32_t len = xfer->len;

            spi_transfer_count++;

            /* Log TX data (first 20, then every 1000th) */
            if (spi_transfer_count <= 20 || (spi_transfer_count % 1000) == 0) {
                fprintf(stderr, "[shim] SPI #%u TX:", spi_transfer_count);
                for (uint32_t i = 0; i < len && i < 16; i++)
                    fprintf(stderr, " %02x", tx[i]);
                fprintf(stderr, "\n");
            }

            /* Craft RX response */
            if (rx && len > 0) {
                memset(rx, 0, len);
                if (len > 2) {
                    rx[2] = 0x04;  /* bit 2 set — advances state machine */
                }
            }
            return 0;
        }
        default:
            SHIM_LOG("SPI ioctl unknown 0x%lx", request);
            return 0;
        }
    }

    /* ---- DRM ioctls ---- */
    if (fd == FAKE_FD_DRM) {
        switch (request) {

        case DRM_IOCTL_VERSION: {
            struct drm_version *v = (struct drm_version *)arg;
            v->version_major = 1;
            v->version_minor = 6;
            v->version_patchlevel = 0;
            /* Caller passes buffers and length; on the two-call pattern we
             * write the string up to the provided length and report the
             * actual lengths. Use harmless fake names. */
            const char *name = "stub", *date = "20260418", *desc = "shim-drm";
            if (v->name && v->name_len >= strlen(name) + 1)
                strcpy(v->name, name);
            v->name_len = strlen(name);
            if (v->date && v->date_len >= strlen(date) + 1)
                strcpy(v->date, date);
            v->date_len = strlen(date);
            if (v->desc && v->desc_len >= strlen(desc) + 1)
                strcpy(v->desc, desc);
            v->desc_len = strlen(desc);
            SHIM_LOG("DRM ioctl VERSION -> 1.6.0 stub");
            return 0;
        }

        case DRM_IOCTL_SET_CLIENT_CAP: {
            struct drm_set_client_cap *c = (struct drm_set_client_cap *)arg;
            SHIM_LOG("DRM ioctl SET_CLIENT_CAP cap=%llu val=%llu",
                     (unsigned long long)c->capability,
                     (unsigned long long)c->value);
            return 0;
        }

        case DRM_IOCTL_GET_CAP: {
            struct drm_get_cap *c = (struct drm_get_cap *)arg;
            /* Claim support for dumb buffers + addfb2 modifiers; deny the
             * rest (prime / vblank / cursor). Game uses gbm_bo_create which
             * in dumb-fallback mode needs DRM_CAP_DUMB_BUFFER. */
            switch (c->capability) {
            case DRM_CAP_DUMB_BUFFER:          c->value = 1; break;
            case DRM_CAP_ADDFB2_MODIFIERS:     c->value = 1; break;
            case DRM_CAP_DUMB_PREFERRED_DEPTH: c->value = 24; break;
            case DRM_CAP_DUMB_PREFER_SHADOW:   c->value = 0; break;
            default:                           c->value = 0; break;
            }
            SHIM_LOG("DRM ioctl GET_CAP cap=%llu -> %llu",
                     (unsigned long long)c->capability,
                     (unsigned long long)c->value);
            return 0;
        }

        case DRM_IOCTL_MODE_GETRESOURCES: {
            struct drm_mode_card_res *r = (struct drm_mode_card_res *)arg;
            /* Two-call pattern: first call passes zero counts to get sizes,
             * second call passes allocated arrays to populate. Always tell
             * the caller what the real counts are. If arrays are big
             * enough we populate them. */
            uint32_t want_crtc = 1, want_conn = 2, want_enc = 1, want_fb = 0;
            if (r->crtc_id_ptr && r->count_crtcs >= want_crtc) {
                uint32_t *ids = (uint32_t *)(uintptr_t)r->crtc_id_ptr;
                ids[0] = FAKE_CRTC_ID;
            }
            if (r->connector_id_ptr && r->count_connectors >= want_conn) {
                uint32_t *ids = (uint32_t *)(uintptr_t)r->connector_id_ptr;
                ids[0] = FAKE_CONNECTOR_A;
                ids[1] = FAKE_CONNECTOR_B;
            }
            if (r->encoder_id_ptr && r->count_encoders >= want_enc) {
                uint32_t *ids = (uint32_t *)(uintptr_t)r->encoder_id_ptr;
                ids[0] = FAKE_ENCODER_ID;
            }
            r->count_fbs        = want_fb;
            r->count_crtcs      = want_crtc;
            r->count_connectors = want_conn;
            r->count_encoders   = want_enc;
            r->min_width = 320;  r->max_width = 3840;
            r->min_height = 200; r->max_height = 2160;
            SHIM_LOG("DRM ioctl MODE_GETRESOURCES -> crtc=%u conn=%u enc=%u",
                     want_crtc, want_conn, want_enc);
            return 0;
        }

        case DRM_IOCTL_MODE_GETCONNECTOR: {
            struct drm_mode_get_connector *c = (struct drm_mode_get_connector *)arg;
            /* Both connectors present the same HDMI-A 1920x1080@60 mode.
             * We don't know which connector the game will iterate to first;
             * returning "connected" on both simplifies acceptance. */
            c->encoder_id = FAKE_ENCODER_ID;
            c->connector_type = 11;         /* DRM_MODE_CONNECTOR_HDMIA */
            c->connector_type_id = (c->connector_id == FAKE_CONNECTOR_A) ? 1 : 2;
            c->connection = 1;              /* 1=connected */
            c->mm_width = 477;
            c->mm_height = 268;
            c->subpixel = 1;                /* SubPixelHorizontalRGB */

            uint32_t want_modes = 1;
            uint32_t want_props = 0;
            uint32_t want_encoders = 1;

            if (c->modes_ptr && c->count_modes >= want_modes) {
                struct drm_mode_modeinfo *modes =
                    (struct drm_mode_modeinfo *)(uintptr_t)c->modes_ptr;
                memset(&modes[0], 0, sizeof(modes[0]));
                modes[0].clock = 148500;    /* kHz, 1080p60 */
                modes[0].hdisplay    = FAKE_MODE_HDISPLAY;
                modes[0].hsync_start = FAKE_MODE_HDISPLAY + 88;
                modes[0].hsync_end   = FAKE_MODE_HDISPLAY + 88 + 44;
                modes[0].htotal      = FAKE_MODE_HDISPLAY + 280;
                modes[0].vdisplay    = FAKE_MODE_VDISPLAY;
                modes[0].vsync_start = FAKE_MODE_VDISPLAY + 4;
                modes[0].vsync_end   = FAKE_MODE_VDISPLAY + 4 + 5;
                modes[0].vtotal      = FAKE_MODE_VDISPLAY + 45;
                modes[0].vrefresh    = 60;
                modes[0].flags       = 0x5; /* PHSYNC|PVSYNC */
                modes[0].type        = 0x48; /* PREFERRED | DRIVER */
                strncpy(modes[0].name, "1920x1080", sizeof(modes[0].name) - 1);
            }
            if (c->encoders_ptr && c->count_encoders >= want_encoders) {
                uint32_t *e = (uint32_t *)(uintptr_t)c->encoders_ptr;
                e[0] = FAKE_ENCODER_ID;
            }

            c->count_modes    = want_modes;
            c->count_props    = want_props;
            c->count_encoders = want_encoders;
            SHIM_LOG("DRM ioctl MODE_GETCONNECTOR id=%u -> connected, %u mode(s)",
                     c->connector_id, want_modes);
            return 0;
        }

        case DRM_IOCTL_MODE_GETENCODER: {
            struct drm_mode_get_encoder *e = (struct drm_mode_get_encoder *)arg;
            e->encoder_type     = 2;   /* DRM_MODE_ENCODER_TMDS */
            e->crtc_id          = FAKE_CRTC_ID;
            e->possible_crtcs   = 1;   /* only CRTC 0 */
            e->possible_clones  = 0;
            SHIM_LOG("DRM ioctl MODE_GETENCODER id=%u -> crtc=%u",
                     e->encoder_id, FAKE_CRTC_ID);
            return 0;
        }

        case DRM_IOCTL_MODE_GETCRTC: {
            struct drm_mode_crtc *c = (struct drm_mode_crtc *)arg;
            c->x = 0; c->y = 0;
            c->fb_id = 0;
            c->gamma_size = 256;
            c->mode_valid = 0;
            memset(&c->mode, 0, sizeof(c->mode));
            SHIM_LOG("DRM ioctl MODE_GETCRTC id=%u", c->crtc_id);
            return 0;
        }

        case DRM_IOCTL_MODE_SETCRTC: {
            struct drm_mode_crtc *c = (struct drm_mode_crtc *)arg;
            SHIM_LOG("DRM ioctl MODE_SETCRTC crtc=%u fb=%u mode=%s",
                     c->crtc_id, c->fb_id,
                     c->mode_valid ? c->mode.name : "(none)");
            return 0;
        }

        case DRM_IOCTL_MODE_CREATE_DUMB: {
            struct drm_mode_create_dumb *d = (struct drm_mode_create_dumb *)arg;
            d->handle = drm_next_dumb_handle++;
            d->pitch  = d->width * ((d->bpp + 7) / 8);
            d->size   = (uint64_t)d->pitch * d->height;
            SHIM_LOG("DRM ioctl MODE_CREATE_DUMB %ux%u bpp=%u -> handle=%u size=%llu",
                     d->width, d->height, d->bpp, d->handle,
                     (unsigned long long)d->size);
            return 0;
        }

        case DRM_IOCTL_MODE_DESTROY_DUMB: {
            struct drm_mode_destroy_dumb *d = (struct drm_mode_destroy_dumb *)arg;
            SHIM_LOG("DRM ioctl MODE_DESTROY_DUMB handle=%u", d->handle);
            return 0;
        }

        case DRM_IOCTL_MODE_MAP_DUMB: {
            /* Not implementable under QEMU without a backing mappable fd.
             * Returning ENOTSUP causes GBM to fall back; if the game
             * actually depends on mmap() of the DRM fd, we'll see a crash
             * later and revisit. */
            struct drm_mode_map_dumb *d = (struct drm_mode_map_dumb *)arg;
            d->offset = 0;
            SHIM_LOG("DRM ioctl MODE_MAP_DUMB handle=%u -> ENOTSUP", d->handle);
            errno = ENOTSUP;
            return -1;
        }

        case DRM_IOCTL_MODE_ADDFB:
        case DRM_IOCTL_MODE_ADDFB2: {
            /* drm_mode_fb_cmd and drm_mode_fb_cmd2 both start with fb_id. */
            uint32_t *fb_id = (uint32_t *)arg;
            *fb_id = drm_next_fb_id++;
            SHIM_LOG("DRM ioctl MODE_ADDFB%s -> fb_id=%u",
                     request == DRM_IOCTL_MODE_ADDFB2 ? "2" : "", *fb_id);
            return 0;
        }

        case DRM_IOCTL_MODE_RMFB: {
            uint32_t *fb_id = (uint32_t *)arg;
            SHIM_LOG("DRM ioctl MODE_RMFB fb_id=%u", *fb_id);
            return 0;
        }

        case DRM_IOCTL_MODE_PAGE_FLIP: {
            struct drm_mode_crtc_page_flip *p =
                (struct drm_mode_crtc_page_flip *)arg;
            SHIM_LOG("DRM ioctl MODE_PAGE_FLIP crtc=%u fb=%u",
                     p->crtc_id, p->fb_id);
            return 0;
        }

        case DRM_IOCTL_GEM_CLOSE: {
            struct drm_gem_close *g = (struct drm_gem_close *)arg;
            SHIM_LOG("DRM ioctl GEM_CLOSE handle=%u", g->handle);
            return 0;
        }

        default:
            SHIM_LOG("DRM ioctl 0x%lx (unknown, returning 0)", request);
            return 0;
        }
    }

    /* ---- I2C ioctls ---- */
    if (fd == FAKE_FD_I2C) {
        i2c_ioctl_count++;
        switch (request) {
        case I2C_SLAVE:
        case I2C_SLAVE_FORCE: {
            unsigned long addr = (unsigned long)(uintptr_t)arg;
            SHIM_LOG("I2C ioctl SET_SLAVE addr=0x%02lx", addr);
            return 0;
        }
        case I2C_RDWR:
            if (i2c_ioctl_count <= 10) {
                SHIM_LOG("I2C ioctl RDWR (transfer #%u)", i2c_ioctl_count);
            }
            return 0;
        case I2C_SMBUS:
            if (i2c_ioctl_count <= 10) {
                SHIM_LOG("I2C ioctl SMBUS (transfer #%u)", i2c_ioctl_count);
            }
            return 0;
        default:
            SHIM_LOG("I2C ioctl 0x%lx (#%u)", request, i2c_ioctl_count);
            return 0;
        }
    }

    return 0;
}
