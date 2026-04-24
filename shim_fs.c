/*
 * shim_fs.c — filesystem intercept layer.
 *
 * Phase 5 of the refactor. Owns every intercept on the path namespace:
 *   - sysroot-aware path remapping (remap_path)
 *   - open / openat / open64 / openat64 / fopen64
 *   - access / faccessat
 *   - stat / lstat / __xstat / __lxstat / __xstat64 / __lxstat64 / fstatat /
 *     __fxstatat / __fxstatat64
 *   - GPIO sysfs fake-fd pool (allocation + lookup + free)
 *   - should_log_open (diagnostic filter — what opens are worth logging)
 *
 * The open family calls intercept_device() (defined in spi_shim.c) to check
 * for device-node matches before falling through to real open(). Fake-fd
 * lifecycle for GPIO lives here; lifecycle for SPI/I2C/DRM/UART still lives
 * with the device-layer state in spi_shim.c.
 *
 * All cross-TU declarations are in spi_shim.h.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>
#include <pthread.h>
#include <dlfcn.h>
#include <sys/types.h>

#include "spi_shim.h"

/* glibc's <sys/stat.h> fights our loose-typed stat wrappers (same as in
 * spi_shim.c). Forward-declare the one thing we need from it. */
extern int mkdir(const char *, unsigned int);

/* Forward decls for libc symbols our wrappers chain to via dlsym(RTLD_NEXT).
 * They're also typedef'd in spi_shim.c; duplicating here keeps this TU
 * independent of internal typedefs.  */
typedef int    (*real_open_t)(const char *, int, ...);
typedef int    (*real_openat_t)(int, const char *, int, ...);
typedef int    (*real_open64_t)(const char *, int, ...);
typedef int    (*real_openat64_t)(int, const char *, int, ...);
typedef FILE * (*real_fopen64_t)(const char *, const char *);
typedef int    (*real_access_t)(const char *, int);
typedef int    (*real_faccessat_t)(int, const char *, int, int);
typedef int    (*real_stat_t)(const char *, void *);
typedef int    (*real_xstat_t)(int, const char *, void *);
typedef int    (*real_fstatat_t)(int, const char *, void *, int);
typedef int    (*real_fxstatat_t)(int, int, const char *, void *, int);

/* open() is resolved by spi_shim.c's resolve_all() and stored in real_open;
 * re-extern here so our open() hook can use the already-resolved pointer
 * without another dlsym round. openat64/fopen64/etc. resolve themselves
 * lazily on first call (they keep their own static pointers inside the
 * wrappers — the existing behavior carried over verbatim). */
extern real_open_t real_open;

/* ========== moved from spi_shim.c: sysroot + remap_path (path remap helper) ========== */
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
    /* Carve-out: /sys/firmware/* IS remapped even though /sys/ generally isn't.
     * Kernel /sys is a live sysfs interface we shouldn't rewrite, but
     * /sys/firmware/ is firmware identity (devicetree, DMI) — plausible for
     * a sysroot overlay to provide. Lets us fake machine-id / serial-number
     * by dropping a file at $SYSROOT/sys/firmware/... */
    if (strncmp(path, "/sys/firmware/", 14) == 0) goto do_remap;
    if (strncmp(path, "/sys/", 5) == 0) return path;
    if (strncmp(path, "/tmp/", 5) == 0) return path;
do_remap:
    snprintf(remapped_buf, sizeof(remapped_buf), "%s%s", sysroot, path);
    return remapped_buf;
}

/* ========== moved from spi_shim.c: GPIO fake-fd pool state ==========
 * FAKE_FD_GPIO_BASE / FAKE_FD_GPIO_COUNT live in spi_shim.h so the I/O
 * hooks (still in spi_shim.c) can range-check fds against this pool.
 * The arrays are externed in the header — non-static here. */
char     *gpio_fake_paths[FAKE_FD_GPIO_COUNT];
uint8_t   gpio_fake_read_done[FAKE_FD_GPIO_COUNT];
static pthread_mutex_t gpio_fake_lock = PTHREAD_MUTEX_INITIALIZER;

/* ========== moved from spi_shim.c (1441-1474): GPIO sysfs helpers (is_gpio_sysfs_path / alloc / is_gpio_fake / free) ========== */
/* ---- GPIO sysfs fake-fd pool ---- */

int is_gpio_sysfs_path(const char *p) {
    return p && strncmp(p, "/sys/class/gpio/", 16) == 0;
}

int alloc_gpio_fake_fd(const char *path) {
    pthread_mutex_lock(&gpio_fake_lock);
    for (int i = 0; i < FAKE_FD_GPIO_COUNT; i++) {
        if (!gpio_fake_paths[i]) {
            gpio_fake_paths[i] = strdup(path);
            gpio_fake_read_done[i] = 0;
            pthread_mutex_unlock(&gpio_fake_lock);
            return FAKE_FD_GPIO_BASE + i;
        }
    }
    pthread_mutex_unlock(&gpio_fake_lock);
    return 0;
}

int is_gpio_fake_fd(int fd) {
    if (fd < FAKE_FD_GPIO_BASE || fd >= FAKE_FD_GPIO_BASE + FAKE_FD_GPIO_COUNT)
        return 0;
    return gpio_fake_paths[fd - FAKE_FD_GPIO_BASE] != NULL;
}

void free_gpio_fake_fd(int fd) {
    int idx = fd - FAKE_FD_GPIO_BASE;
    pthread_mutex_lock(&gpio_fake_lock);
    free(gpio_fake_paths[idx]);
    gpio_fake_paths[idx] = NULL;
    gpio_fake_read_done[idx] = 0;
    pthread_mutex_unlock(&gpio_fake_lock);
}

/* ========== moved from spi_shim.c (1574-1882): Filesystem hooks: should_log_open + open family + stat family + access/faccessat ========== */
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
    /* Step 62.b: catch Godot-resource opens regardless of prefix. Relative
     * paths (./assets/...) and absolute /mnt/c/... paths fall through all
     * the /dev/games/etc checks above. These tell us whether the game
     * actually finds main.pck, .tscn/.scn resources, .gd scripts, etc. */
    if (strstr(pathname, ".pck")) return 1;
    if (strstr(pathname, ".tscn")) return 1;
    if (strstr(pathname, ".scn"))  return 1;
    if (strstr(pathname, ".remap")) return 1;
    if (strstr(pathname, ".import")) return 1;
    if (strstr(pathname, "godot"))  return 1;
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

/* ---- Step 62.b: LFS variants (open64/openat64/fopen64) ----
 * The Pokemon LE binary imports open64@GLIBC_2.17 and fopen64@GLIBC_2.17
 * (per readelf --dyn-syms). Our open() hook only catches calls to the
 * 32-bit-off_t variant; the game's Godot resource loader actually uses
 * open64 (the LFS family), so main.pck access was invisible to us. Mirror
 * the existing open/openat semantics here so path-remap + logging cover
 * those calls too. On 64-bit aarch64 Linux, open64 and open are functionally
 * identical (off_t is already 64-bit), but the SYMBOL resolution is distinct. */

typedef int (*real_open64_t)(const char *, int, ...);
static real_open64_t real_open64 = NULL;

int open64(const char *pathname, int flags, ...) {
    if (!real_open64) real_open64 = (real_open64_t)dlsym(RTLD_NEXT, "open64");
    int fake = intercept_device(pathname);
    if (fake) return fake;
    const char *actual = remap_path(pathname);
    int ret;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        int mode = va_arg(args, int);
        va_end(args);
        ret = real_open64(actual, flags, mode);
    } else {
        ret = real_open64(actual, flags);
    }
    if (should_log_open(pathname)) {
        if (ret < 0) {
            SHIM_LOG("OPEN64 FAIL: %s (flags=0x%x, errno=%d %s)",
                     pathname, flags, errno, strerror(errno));
        } else {
            SHIM_LOG("OPEN64 OK:   %s -> fd %d (flags=0x%x)",
                     pathname, ret, flags);
        }
    }
    return ret;
}

typedef int (*real_openat64_t)(int, const char *, int, ...);
static real_openat64_t real_openat64 = NULL;

int openat64(int dirfd, const char *pathname, int flags, ...) {
    if (!real_openat64) real_openat64 = (real_openat64_t)dlsym(RTLD_NEXT, "openat64");
    int fake = intercept_device(pathname);
    if (fake) return fake;
    const char *actual = remap_path(pathname);
    int ret;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        int mode = va_arg(args, int);
        va_end(args);
        ret = real_openat64(dirfd, actual, flags, mode);
    } else {
        ret = real_openat64(dirfd, actual, flags);
    }
    if (should_log_open(pathname)) {
        if (ret < 0) {
            SHIM_LOG("OPENAT64 FAIL: %s (dirfd=%d flags=0x%x, errno=%d %s)",
                     pathname, dirfd, flags, errno, strerror(errno));
        } else {
            SHIM_LOG("OPENAT64 OK:   %s -> fd %d (dirfd=%d flags=0x%x)",
                     pathname, ret, dirfd, flags);
        }
    }
    return ret;
}

typedef FILE* (*real_fopen64_t)(const char *, const char *);
static real_fopen64_t real_fopen64 = NULL;

FILE *fopen64(const char *pathname, const char *mode) {
    if (!real_fopen64) real_fopen64 = (real_fopen64_t)dlsym(RTLD_NEXT, "fopen64");
    /* fopen64 returns FILE* not an fd — device-interception path doesn't fit.
     * Path-remap still applies. */
    const char *actual = remap_path(pathname);
    FILE *ret = real_fopen64(actual, mode);
    if (should_log_open(pathname)) {
        if (!ret) {
            SHIM_LOG("FOPEN64 FAIL: %s (mode=%s, errno=%d %s)",
                     pathname, mode, errno, strerror(errno));
        } else {
            SHIM_LOG("FOPEN64 OK:   %s -> FILE* %p (mode=%s)",
                     pathname, (void*)ret, mode);
        }
    }
    return ret;
}

/* ---- Hooked: fopen (plain — Phase-7 asymmetry closed 2026-04-24) ----
 *
 * The game binary imports `fopen@GLIBC_2.17` directly (verified via readelf
 * --dyn-syms). If Godot was compiled with `_FILE_OFFSET_BITS=64` the
 * compiler probably redirects some calls to `fopen64`, but any remaining
 * plain-fopen call would bypass our path-remap + logging entirely.
 * Semantics match fopen64 exactly on 64-bit aarch64 (off_t is already
 * 64-bit). */
typedef FILE* (*real_fopen_t)(const char *, const char *);
static real_fopen_t real_fopen = NULL;

FILE *fopen(const char *pathname, const char *mode) {
    if (!real_fopen) real_fopen = (real_fopen_t)dlsym(RTLD_NEXT, "fopen");

    /* FIRST CALL banner so we notice when a plain-fopen path first fires. */
    static uint32_t _fopen_count = 0;
    uint32_t _n = __atomic_add_fetch(&_fopen_count, 1, __ATOMIC_RELAXED);
    if (_n == 1) {
        SHIM_LOG("[lfs] FIRST CALL: fopen — hook now wired "
                 "(was previously falling through to libc untracked)");
    }

    const char *actual = remap_path(pathname);
    FILE *ret = real_fopen(actual, mode);
    if (should_log_open(pathname)) {
        if (!ret) {
            SHIM_LOG("FOPEN FAIL:   %s (mode=%s, errno=%d %s)",
                     pathname, mode, errno, strerror(errno));
        } else {
            SHIM_LOG("FOPEN OK:     %s -> FILE* %p (mode=%s)",
                     pathname, (void*)ret, mode);
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

    /* GPIO sysfs: pretend the pin directory + attribute files always exist
     * (mode ignored — the game uses F_OK to check export state). */
    if (is_gpio_sysfs_path(pathname)) {
        (void)mode;
        return 0;
    }

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

/* ========== libgpiod chardev stubs (2026-04-24) ==========
 * The game imports six symbols from libgpiod (the modern /dev/gpiochipN
 * character-device API). Known-gap: our shim only faked the legacy
 * /sys/class/gpio/ sysfs interface, so calls through libgpiod silently
 * fell through to real libgpiod, which then opened /dev/gpiochip* — that
 * doesn't exist on this host, so the opens errored and the game's
 * chip-select / interrupt-line logic never got its handshake.
 *
 * Stubs return opaque non-NULL handles (dynamically allocated tracking
 * structs) so pointer identity between open and close works naturally.
 * The game uses GPIO as output-only (only `gpiod_line_request_output` is
 * imported — no `_request_input` / `_get_value`) so we log value writes
 * but don't have to invent a read semantic.
 *
 * The opaque `struct gpiod_chip` and `struct gpiod_line` in libgpiod's
 * public headers are ABI-compatible with our plain pointer return types.
 */

struct shim_gpiod_chip { char name[32]; };
struct shim_gpiod_line { struct shim_gpiod_chip *chip; unsigned offset;
                         int direction; int value; };

void *gpiod_chip_open_by_name(const char *name) {
    struct shim_gpiod_chip *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    if (name) {
        strncpy(c->name, name, sizeof(c->name) - 1);
        c->name[sizeof(c->name) - 1] = 0;
    } else {
        strcpy(c->name, "?");
    }
    SHIM_LOG("[gpiod] chip_open_by_name(\"%s\") -> %p (fake)", c->name, (void*)c);
    return c;
}

void *gpiod_chip_get_line(void *chip, unsigned offset) {
    if (!chip) return NULL;
    struct shim_gpiod_chip *c = chip;
    struct shim_gpiod_line *l = calloc(1, sizeof(*l));
    if (!l) return NULL;
    l->chip = c;
    l->offset = offset;
    SHIM_LOG("[gpiod] chip_get_line(%s, %u) -> %p (fake)", c->name, offset, (void*)l);
    return l;
}

int gpiod_line_request_output(void *line, const char *consumer, int default_val) {
    if (!line) return -1;
    struct shim_gpiod_line *l = line;
    l->direction = 1;
    l->value = default_val;
    SHIM_LOG("[gpiod] line_request_output(%s:%u, consumer=\"%s\", default=%d) -> 0 (fake)",
             l->chip ? l->chip->name : "?", l->offset,
             consumer ? consumer : "?", default_val);
    return 0;
}

int gpiod_line_set_value(void *line, int value) {
    if (!line) return -1;
    struct shim_gpiod_line *l = line;
    l->value = value;
    static uint32_t _n = 0;
    uint32_t n = __atomic_add_fetch(&_n, 1, __ATOMIC_RELAXED);
    /* Toggle-heavy bit-bang could overwhelm the log; sample like other hooks. */
    if (n <= 8 || (n % 500) == 0) {
        SHIM_LOG("[gpiod] line_set_value #%u (%s:%u) = %d",
                 n, l->chip ? l->chip->name : "?", l->offset, value);
    }
    return 0;
}

void gpiod_line_release(void *line) {
    if (!line) return;
    struct shim_gpiod_line *l = line;
    SHIM_LOG("[gpiod] line_release(%s:%u)", l->chip ? l->chip->name : "?", l->offset);
    free(l);
}

void gpiod_chip_close(void *chip) {
    if (!chip) return;
    struct shim_gpiod_chip *c = chip;
    SHIM_LOG("[gpiod] chip_close(%s)", c->name);
    free(c);
}

