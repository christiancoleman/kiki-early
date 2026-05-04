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
#include <dirent.h>

#include "spi_shim.h"
#include "shim_discovery.h"

/* glibc's <sys/stat.h> fights our loose-typed stat wrappers (same as in
 * spi_shim.c). Forward-declare the one thing we need from it. */
extern int mkdir(const char *, unsigned int);

/* Mirror selected fs activity to discovery.log so the diagnostic is
 * visible regardless of whether the run captures qemu's STDERR
 * (game.log can be skipped if the user invokes qemu outside run_game.sh).
 * SHIM_LOG continues to fire too, for the normal-path log. */
#define FS_DIAG_LOG(fmt, ...) do {                              \
    SHIM_LOG("[fs_diag] " fmt, ##__VA_ARGS__);                  \
    discovery_log("[fs_diag] " fmt, ##__VA_ARGS__);             \
} while (0)

/* Two diagnostic modes:
 *
 *   default               — match `/tmp/<X>` only. Low volume; catches the
 *                           sysroot-remap traffic that's our normal area
 *                           of interest. Safe to leave on every run.
 *
 *   SPIKE3_FS_DIAG_VERBOSE=1
 *                         — also match any path containing "cache" or
 *                           "shader" anywhere. Caught Mesa's 144-entry
 *                           shader-hash directory tree during the Godot
 *                           shader-cache investigation; useful when you
 *                           want maximum visibility ("everything") and
 *                           are willing to pay the log-volume cost.
 *
 * The env-var read is cached on first call so the per-syscall hot path
 * is one branch + one strncmp.
 */
static int fs_diag_path_match(const char *p) {
    if (!p) return 0;
    /* Recursion guard: never log the log file itself, or any log under
     * runs/latest/. Without this, FS_DIAG_ALL=1 turns every fs op into a
     * log call → log file open → fs op → log call → ... stack-overflow
     * SEGV before init even gets going. */
    if (strstr(p, "discovery.log") != NULL) return 0;
    if (strstr(p, "runs/latest")   != NULL) return 0;

    static int diag_all = -1;
    if (__builtin_expect(diag_all < 0, 0)) {
        const char *e = getenv("SPIKE3_FS_DIAG_ALL");
        diag_all = (e && e[0] == '1') ? 1 : 0;
    }
    if (diag_all) return 1;

    if (strncmp(p, "/tmp/", 5) == 0) return 1;
    if (strstr(p, "lcd")           != NULL) return 1;
    if (strstr(p, "nuk")           != NULL) return 1;
    if (strstr(p, "assets")        != NULL) return 1;
    if (strstr(p, "auto_loaded")   != NULL) return 1;
    if (strstr(p, "demand_loaded") != NULL) return 1;
    if (strstr(p, ".radium")       != NULL) return 1;

    static int verbose = -1;
    if (__builtin_expect(verbose < 0, 0)) {
        const char *e = getenv("SPIKE3_FS_DIAG_VERBOSE");
        verbose = (e && e[0] == '1') ? 1 : 0;
    }
    if (verbose) {
        if (strstr(p, "cache")  != NULL) return 1;
        if (strstr(p, "shader") != NULL) return 1;
    }
    return 0;
}
#define FS_DIAG_TMP_PREFIX(p)  fs_diag_path_match(p)

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
    /* Only absolute paths participate in remapping. Any path starting at /
     * is either a Spike 3 path (we want it under $SYSROOT) or a host path
     * (we want it left alone). The skip list captures the latter. */
    if (path[0] != '/') return path;
    /* Already-remapped paths: if the caller hands us a path that already
     * starts with $SYSROOT/, don't double-prefix. Hits when Mesa's shader
     * cache is given MESA_SHADER_CACHE_DIR=$SYSROOT/tmp/cache/mesa and then
     * mkdirs subdirectories of that absolute path — without this guard
     * each subdir mkdir would land at $SYSROOT/$SYSROOT/tmp/cache/...
     * Also catches any other code path that pre-resolves sysroot before
     * calling our hooks. The trailing '/' or '\0' check avoids matching
     * a sibling path that happens to share the prefix (e.g. sysroot=/foo
     * accidentally matching /foobar). */
    if (strncmp(path, sysroot, sysroot_len) == 0 &&
        (path[sysroot_len] == '/' || path[sysroot_len] == '\0'))
        return path;
    /* --- Live kernel + IPC interfaces. Never remap; the host kernel owns
     *     these and a sysroot overlay would either be wrong or empty. --- */
    if (strncmp(path, "/dev/", 5) == 0) return path;
    if (strncmp(path, "/proc/", 6) == 0) return path;
    // if (strncmp(path, "/tmp/", 5) == 0) {
    //     /* Specific carve-outs that should land in the sysroot's /tmp, not the
    //      * host /tmp (keeps WSL's filesystem clean). The game opens these via
    //      * libc fopen which goes through this hook:
    //      *   - /tmp/godot_logs[<TS>]  — Godot engine's log file
    //      *   - /tmp/cache/...         — Mesa shader cache (also reinforced by
    //      *                              MESA_SHADER_CACHE_DIR env var passed
    //      *                              from run_game.sh)
    //      * Other /tmp/ paths (shim's own AF_UNIX sockets, system-level tmp)
    //      * keep falling through to the host. */
    //     if (strncmp(path, "/tmp/godot_logs", 15) == 0 ||
    //         strncmp(path, "/tmp/cache/",     11) == 0 ||
    //         strcmp (path, "/tmp/cache")           == 0) {
    //         goto do_remap;
    //     }
    //     return path;
    // }
    if (strncmp(path, "/run/", 5) == 0) return path;
    /* Carve-out: /sys/firmware/ IS remapped even though /sys/ generally isn't.
     * Kernel /sys is a live sysfs interface we shouldn't rewrite, but
     * /sys/firmware/ is firmware identity (devicetree, DMI) — plausible for
     * a sysroot overlay to provide. Lets us fake machine-id / serial-number
     * by dropping a file at $SYSROOT/sys/firmware/... */
    if (strncmp(path, "/sys/firmware/", 14) == 0) goto do_remap;
    if (strncmp(path, "/sys/", 5) == 0) return path;
    /* Font config redirects: libfontconfig probes the four per-user config
     * paths below under the host's /home/. Without this carve-out those
     * probes fall through the /home/ skip and hit the WSL-native filesystem
     * (where they don't exist). Redirect each onto the sysroot /etc/fonts/
     * equivalent so fontconfig stays inside the project sysroot. The
     * pokemon-side stub at $SYSROOT/etc/fonts/local.conf was created with
     * Step 70 specifically to be the landing point. */
    if (strcmp(path, "/home/c/.fonts.conf") == 0 ||
        strcmp(path, "/home/c/.config/fontconfig/fonts.conf") == 0) {
        snprintf(remapped_buf, sizeof(remapped_buf), "%s/etc/fonts/local.conf", sysroot);
        return remapped_buf;
    }
    if (strcmp(path, "/home/c/.fonts.conf.d") == 0 ||
        strcmp(path, "/home/c/.config/fontconfig/conf.d") == 0) {
        snprintf(remapped_buf, sizeof(remapped_buf), "%s/etc/fonts/conf.d", sysroot);
        return remapped_buf;
    }
    /* --- Host user/system state. The aarch64 process inherits HOME and
     *     other env from the host launcher, so glibc/fontconfig/etc. probe
     *     these paths. We must NOT remap them — leave them on the host so
     *     fontconfig finds the host's font cache, $HOME points at a real
     *     dir, and so on. (Earlier omission caused fontconfig to fail
     *     silently and the game to fall back to bundled defaults.) --- */
    if (strncmp(path, "/home/", 6) == 0) return path;
    if (strncmp(path, "/root/", 6) == 0) return path;
    if (strncmp(path, "/var/", 5) == 0) return path;
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
    int diag = FS_DIAG_TMP_PREFIX(pathname);

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
    if (diag) FS_DIAG_LOG("open(%s, flags=0x%x) -> %s ret=%d errno=%d",
                          pathname, flags, actual, ret,
                          (ret < 0) ? errno : 0);
    return ret;
}

typedef int (*real_openat_t)(int, const char *, int, ...);
static real_openat_t real_openat = NULL;

int openat(int dirfd, const char *pathname, int flags, ...) {
    if (!real_openat) real_openat = (real_openat_t)dlsym(RTLD_NEXT, "openat");

    int fake = intercept_device(pathname);
    if (fake) return fake;

    const char *actual = remap_path(pathname);
    int diag = FS_DIAG_TMP_PREFIX(pathname);

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

    if (diag) FS_DIAG_LOG("openat(dirfd=%d, %s, flags=0x%x) -> %s ret=%d errno=%d",
                          dirfd, pathname, flags, actual, ret,
                          (ret < 0) ? errno : 0);
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
    int diag = FS_DIAG_TMP_PREFIX(pathname);
    if (actual != pathname) {
        SHIM_LOG("access REMAP: %s -> %s", pathname, actual);
    }
    if (diag) FS_DIAG_LOG("access(%s, mode=0x%x) -> %s",
                          pathname, mode, actual);
    int ret = real_fn(actual, mode);
    if (ret != 0 && pathname[0] == '/') {
        SHIM_LOG("access FAILED: %s (errno=%d)", actual, errno);
    }
    if (diag) FS_DIAG_LOG("access(%s) ret=%d errno=%d",
                          pathname, ret, ret ? errno : 0);
    return ret;
}

/* ---- Hooked: mkdir / mkdirat ----
 * Godot's DirAccess::make_dir() and the game's NV-layer init both call
 * mkdir() / mkdirat() directly via libc. Without our hook these miss the
 * sysroot remap entirely and try to create directories on the host root
 * (always fails for /connectivity/, /data/nv/pokemon_le/<store>/, etc.). The
 * wrapper remaps through the sysroot and treats EEXIST as success
 * (mkdir-p semantics) — Godot DirAccess and Stern's NV layer both treat
 * EEXIST as a hard failure even for already-existing directories.
 *
 * mode_t is `unsigned int` per the existing extern declaration at the top
 * of this file (see comment about <sys/stat.h> conflict with the loose-typed
 * stat wrappers). */

typedef int (*real_mkdir_t)(const char *, unsigned int);

int mkdir(const char *pathname, unsigned int mode) {
    static real_mkdir_t real_fn = NULL;
    if (!real_fn) real_fn = (real_mkdir_t)dlsym(RTLD_NEXT, "mkdir");
    const char *actual = remap_path(pathname);
    int diag = FS_DIAG_TMP_PREFIX(pathname);
    if (actual != pathname) {
        SHIM_LOG("mkdir REMAP: %s -> %s", pathname, actual);
    }
    if (diag) FS_DIAG_LOG("mkdir(%s, mode=0%o) -> %s",
                          pathname, mode, actual);
    int ret = real_fn(actual, mode);
    if (ret == 0) {
        if (diag) FS_DIAG_LOG("mkdir(%s) ret=0 (created)", pathname);
        return 0;
    }
    if (errno == EEXIST) {
        /* mkdir -p: pre-existing target is success. The risk that `actual`
         * is a regular file with the same name (also yields EEXIST) is low
         * — these paths are dirs in our sysroot. */
        if (diag) FS_DIAG_LOG("mkdir(%s) ret=0 (EEXIST → ok)", pathname);
        return 0;
    }
    if (pathname[0] == '/') {
        SHIM_LOG("mkdir FAILED: %s (errno=%d)", actual, errno);
    }
    if (diag) FS_DIAG_LOG("mkdir(%s) FAILED ret=%d errno=%d",
                          pathname, ret, errno);
    return ret;
}

typedef int (*real_mkdirat_t)(int, const char *, unsigned int);

int mkdirat(int dirfd, const char *pathname, unsigned int mode) {
    static real_mkdirat_t real_fn = NULL;
    if (!real_fn) real_fn = (real_mkdirat_t)dlsym(RTLD_NEXT, "mkdirat");
    /* Only absolute paths participate in remapping; relative paths use
     * dirfd as the resolution anchor and shouldn't be rewritten. */
    const char *actual = (pathname && pathname[0] == '/')
                             ? remap_path(pathname)
                             : pathname;
    if (actual != pathname) {
        SHIM_LOG("mkdirat REMAP: %s -> %s", pathname, actual);
    }
    int ret = real_fn(dirfd, actual, mode);
    if (ret == 0) return 0;
    if (errno == EEXIST) {
        return 0;
    }
    if (pathname && pathname[0] == '/') {
        SHIM_LOG("mkdirat FAILED: %s (errno=%d)", actual, errno);
    }
    return ret;
}

/* ---- Hooked: stat / lstat / stat64 / lstat64 (+ legacy __xstat compat) ----
 *
 * GLIBC 2.33 retired the __xstat/__xstat64 indirection: stat/lstat/stat64/
 * lstat64/fstat64 became direct exports versioned at GLIBC_2.33. The Pokemon
 * LE game.elf is built against a glibc-2.33+ toolchain and imports
 * `stat@GLIBC_2.33`, `lstat@GLIBC_2.33`, `stat64@GLIBC_2.33`,
 * `lstat64@GLIBC_2.33`, `fstat64@GLIBC_2.33` (verified by
 * `objdump -R game | grep stat`).
 *
 * Two consequences for this hook:
 *   1. We need stat64/lstat64 path-rewriting hooks (we already had only
 *      __xstat64/__lxstat64; the new direct names had no hook at all,
 *      so /tmp/<X> paths went straight to libc unrewritten).
 *   2. Each hook needs an explicit `.symver foo, foo@GLIBC_2.33` so the
 *      game's PLT relocation for `foo@GLIBC_2.33` resolves to *our*
 *      definition. An unversioned hook satisfies an unversioned reference,
 *      but the dynamic linker prefers an exact-version match — and libc
 *      has the exact-version definition. Adding the `.symver` directive
 *      makes our hook ALSO export the GLIBC_2.33-versioned alias so we
 *      win the resolution.
 *
 * fstat64 takes an fd, not a path → no remap needed, no hook needed.
 *
 * Diagnostic logging: every stat-family call against `/tmp/<X>` is logged
 * (path, remap target, ret/errno) so we can verify the hook actually
 * fires for paths like `/tmp/cache` (the Godot shader-cache complaint
 * traces back to this — host /tmp/cache doesn't exist, so any stat that
 * sneaks past us hits ENOENT). Filtered to the /tmp/ prefix to keep log
 * volume sane; widen the filter if a different path proves problematic.
 */

/* real_stat_t / real_xstat_t typedefs already declared at file top. */

int stat(const char *pathname, void *statbuf) {
    static real_stat_t real_fn = NULL;
    if (!real_fn) {
        real_fn = (real_stat_t)dlvsym(RTLD_NEXT, "stat", "GLIBC_2.33");
        if (!real_fn) real_fn = (real_stat_t)dlsym(RTLD_NEXT, "stat");
    }
    const char *actual = remap_path(pathname);
    int diag = FS_DIAG_TMP_PREFIX(pathname);
    if (diag) FS_DIAG_LOG("stat(%s) -> %s", pathname, actual);
    int ret = real_fn(actual, statbuf);
    if (diag) FS_DIAG_LOG("stat(%s) ret=%d errno=%d",
                          pathname, ret, ret ? errno : 0);
    return ret;
}
__asm__(".symver stat, stat@GLIBC_2.33");

int lstat(const char *pathname, void *statbuf) {
    static real_stat_t real_fn = NULL;
    if (!real_fn) {
        real_fn = (real_stat_t)dlvsym(RTLD_NEXT, "lstat", "GLIBC_2.33");
        if (!real_fn) real_fn = (real_stat_t)dlsym(RTLD_NEXT, "lstat");
    }
    const char *actual = remap_path(pathname);
    int diag = FS_DIAG_TMP_PREFIX(pathname);
    if (diag) FS_DIAG_LOG("lstat(%s) -> %s", pathname, actual);
    int ret = real_fn(actual, statbuf);
    if (diag) FS_DIAG_LOG("lstat(%s) ret=%d errno=%d",
                          pathname, ret, ret ? errno : 0);
    return ret;
}
__asm__(".symver lstat, lstat@GLIBC_2.33");

int stat64(const char *pathname, void *statbuf) {
    static real_stat_t real_fn = NULL;
    if (!real_fn) {
        real_fn = (real_stat_t)dlvsym(RTLD_NEXT, "stat64", "GLIBC_2.33");
        if (!real_fn) real_fn = (real_stat_t)dlsym(RTLD_NEXT, "stat64");
    }
    const char *actual = remap_path(pathname);
    int diag = FS_DIAG_TMP_PREFIX(pathname);
    if (diag) FS_DIAG_LOG("stat64(%s) -> %s", pathname, actual);
    int ret = real_fn(actual, statbuf);
    if (diag) FS_DIAG_LOG("stat64(%s) ret=%d errno=%d",
                          pathname, ret, ret ? errno : 0);
    return ret;
}
__asm__(".symver stat64, stat64@GLIBC_2.33");

int lstat64(const char *pathname, void *statbuf) {
    static real_stat_t real_fn = NULL;
    if (!real_fn) {
        real_fn = (real_stat_t)dlvsym(RTLD_NEXT, "lstat64", "GLIBC_2.33");
        if (!real_fn) real_fn = (real_stat_t)dlsym(RTLD_NEXT, "lstat64");
    }
    const char *actual = remap_path(pathname);
    int diag = FS_DIAG_TMP_PREFIX(pathname);
    if (diag) FS_DIAG_LOG("lstat64(%s) -> %s", pathname, actual);
    int ret = real_fn(actual, statbuf);
    if (diag) FS_DIAG_LOG("lstat64(%s) ret=%d errno=%d",
                          pathname, ret, ret ? errno : 0);
    return ret;
}
__asm__(".symver lstat64, lstat64@GLIBC_2.33");

/* chdir hook — remap path before dispatch to libc.
 *
 * Why this matters: Godot's DirAccessUnix::change_dir uses chdir() to
 * verify a directory is reachable. RasterizerGLES3::initialize calls
 * DirAccess::open(<cache_path>) → change_dir(<cache_path>) → chdir().
 * The cache path resolves to "/tmp/cache" (Godot OS::get_*_path member).
 * Without remapping, chdir("/tmp/cache") goes straight to libc unmapped,
 * hits the host filesystem where /tmp/cache doesn't exist, returns -1,
 * change_dir returns ERR_INVALID_PARAMETER, DirAccess::open returns
 * NULL, and the rasterizer prints "Can't create shader cache folder, no
 * shader caching will happen: /tmp/cache" at line 335. The dir DOES
 * exist in our sysroot — we just need to route the chdir through
 * remap_path so it lands at $SYSROOT/tmp/cache.
 *
 * Sub-tle: DirAccessUnix::change_dir saves prev_dir via getcwd() and
 * chdirs back at the end. Both the save and the restore work fine —
 * getcwd reports the real (host-side) cwd which run_game.sh has set to
 * $GAME_DIR (real dir, no remap needed); the restore chdir is to that
 * same real path. So we only need to remap the FORWARD chdir.
 *
 * Diagnostic: log every chdir against /tmp/<X> (and any cache/shader
 * path when SPIKE3_FS_DIAG_VERBOSE=1) so failures here surface
 * immediately in discovery.log on the next regression. */
typedef int (*real_chdir_t)(const char *);
int chdir(const char *path) {
    static real_chdir_t real_fn = NULL;
    if (!real_fn) {
        real_fn = (real_chdir_t)dlvsym(RTLD_NEXT, "chdir", "GLIBC_2.17");
        if (!real_fn) real_fn = (real_chdir_t)dlsym(RTLD_NEXT, "chdir");
    }
    const char *actual = path ? remap_path(path) : path;
    int diag = FS_DIAG_TMP_PREFIX(path);
    if (diag) FS_DIAG_LOG("chdir(%s) -> %s", path ? path : "(null)",
                          actual ? actual : "(null)");
    int ret = real_fn(actual);
    if (diag) FS_DIAG_LOG("chdir(%s) ret=%d errno=%d",
                          path ? path : "(null)", ret, ret ? errno : 0);
    return ret;
}

/* Legacy __xstat / __xstat64 — kept for older builds that still go
 * through the indirection. Not the path the 0.81 game.elf takes. */
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

/* opendir hook — Godot's DirAccess::change_dir / dir_exists ultimately
 * goes through DIR* opendir(). We DO hook openat (which opendir calls
 * under the hood in modern glibc), but if Godot's libstdc++/Godot wraps
 * opendir with internal syscalls or the openat is taking a different
 * code path we're missing, we'd never see the call. Hooking opendir
 * itself catches it definitively. */
typedef DIR *(*real_opendir_t)(const char *);
DIR *opendir(const char *name) {
    static real_opendir_t real_fn = NULL;
    if (!real_fn) {
        real_fn = (real_opendir_t)dlvsym(RTLD_NEXT, "opendir", "GLIBC_2.17");
        if (!real_fn) real_fn = (real_opendir_t)dlsym(RTLD_NEXT, "opendir");
    }
    const char *actual = name ? remap_path(name) : name;
    int diag = FS_DIAG_TMP_PREFIX(name) ||
               (name && strstr(name, "cache") != NULL);
    if (diag) FS_DIAG_LOG("opendir(%s) -> %s", name ? name : "(null)",
                          actual ? actual : "(null)");
    DIR *ret = real_fn(actual);
    if (diag) FS_DIAG_LOG("opendir(%s) ret=%p errno=%d",
                          name ? name : "(null)", (void *)ret, ret ? 0 : errno);
    return ret;
}

int __lxstat64(int ver, const char *pathname, void *statbuf) {
    static real_xstat_t real_fn = NULL;
    if (!real_fn) real_fn = (real_xstat_t)dlsym(RTLD_NEXT, "__lxstat64");
    const char *actual = remap_path(pathname);
    return real_fn(ver, actual, statbuf);
}

/* ========== Audit-pass hooks (2026-04-30) ==========
 *
 * After we shipped the chdir() hook, an audit of the game's libc imports
 * (objdump -R game | grep @GLIBC | filter on fs-related names) found
 * eight more path-taking calls that were going straight to libc unmapped:
 *
 *   unlink, rename, readlink, statfs, statvfs64, chmod, glob64, rmdir
 *
 * Same failure mode as chdir: if any of these hits a sysroot-only path
 * (e.g. an NV-write cleanup `unlink("/data/nv/...")`, an atomic
 * `rename(tmp, dst)` for save files, a `readlink` on a sysroot symlink,
 * a `statfs` for free-space check), libc would dispatch to the host
 * filesystem where the path doesn't exist, return -1, and the caller
 * would silently mishandle. Each hook here calls remap_path() and emits
 * fs_diag for /tmp/<X> + cache/shader paths (when SPIKE3_FS_DIAG_VERBOSE).
 *
 * Two-path hooks (rename, symlink) remap BOTH operands. */

typedef int (*real_unlink_t)(const char *);
int unlink(const char *pathname) {
    static real_unlink_t real_fn = NULL;
    if (!real_fn) real_fn = (real_unlink_t)dlsym(RTLD_NEXT, "unlink");
    const char *actual = pathname ? remap_path(pathname) : pathname;
    int diag = FS_DIAG_TMP_PREFIX(pathname);
    if (diag) FS_DIAG_LOG("unlink(%s) -> %s", pathname ? pathname : "(null)",
                          actual ? actual : "(null)");
    int ret = real_fn(actual);
    if (diag) FS_DIAG_LOG("unlink(%s) ret=%d errno=%d",
                          pathname ? pathname : "(null)", ret, ret ? errno : 0);
    return ret;
}

typedef int (*real_rename_t)(const char *, const char *);
int rename(const char *oldpath, const char *newpath) {
    static real_rename_t real_fn = NULL;
    if (!real_fn) real_fn = (real_rename_t)dlsym(RTLD_NEXT, "rename");
    const char *old_a = oldpath ? remap_path(oldpath) : oldpath;
    const char *new_a = newpath ? remap_path(newpath) : newpath;
    int diag = FS_DIAG_TMP_PREFIX(oldpath) || FS_DIAG_TMP_PREFIX(newpath);
    if (diag) FS_DIAG_LOG("rename(%s, %s) -> (%s, %s)",
                          oldpath ? oldpath : "(null)",
                          newpath ? newpath : "(null)",
                          old_a ? old_a : "(null)",
                          new_a ? new_a : "(null)");
    int ret = real_fn(old_a, new_a);
    if (diag) FS_DIAG_LOG("rename(%s, %s) ret=%d errno=%d",
                          oldpath ? oldpath : "(null)",
                          newpath ? newpath : "(null)",
                          ret, ret ? errno : 0);
    return ret;
}

typedef int (*real_rmdir_t)(const char *);
int rmdir(const char *pathname) {
    static real_rmdir_t real_fn = NULL;
    if (!real_fn) real_fn = (real_rmdir_t)dlsym(RTLD_NEXT, "rmdir");
    const char *actual = pathname ? remap_path(pathname) : pathname;
    int diag = FS_DIAG_TMP_PREFIX(pathname);
    if (diag) FS_DIAG_LOG("rmdir(%s) -> %s", pathname ? pathname : "(null)",
                          actual ? actual : "(null)");
    int ret = real_fn(actual);
    if (diag) FS_DIAG_LOG("rmdir(%s) ret=%d errno=%d",
                          pathname ? pathname : "(null)", ret, ret ? errno : 0);
    return ret;
}

typedef ssize_t (*real_readlink_t)(const char *, char *, size_t);
ssize_t readlink(const char *pathname, char *buf, size_t bufsiz) {
    static real_readlink_t real_fn = NULL;
    if (!real_fn) real_fn = (real_readlink_t)dlsym(RTLD_NEXT, "readlink");
    const char *actual = pathname ? remap_path(pathname) : pathname;
    int diag = FS_DIAG_TMP_PREFIX(pathname);
    if (diag) FS_DIAG_LOG("readlink(%s) -> %s", pathname ? pathname : "(null)",
                          actual ? actual : "(null)");
    ssize_t ret = real_fn(actual, buf, bufsiz);
    if (diag) FS_DIAG_LOG("readlink(%s) ret=%zd errno=%d",
                          pathname ? pathname : "(null)", ret,
                          (ret < 0) ? errno : 0);
    return ret;
}

typedef int (*real_chmod_t)(const char *, unsigned int);
int chmod(const char *pathname, unsigned int mode) {
    static real_chmod_t real_fn = NULL;
    if (!real_fn) real_fn = (real_chmod_t)dlsym(RTLD_NEXT, "chmod");
    const char *actual = pathname ? remap_path(pathname) : pathname;
    int diag = FS_DIAG_TMP_PREFIX(pathname);
    if (diag) FS_DIAG_LOG("chmod(%s, mode=0%o) -> %s",
                          pathname ? pathname : "(null)", mode,
                          actual ? actual : "(null)");
    int ret = real_fn(actual, mode);
    if (diag) FS_DIAG_LOG("chmod(%s) ret=%d errno=%d",
                          pathname ? pathname : "(null)", ret, ret ? errno : 0);
    return ret;
}

typedef int (*real_statfs_t)(const char *, void *);
int statfs(const char *pathname, void *buf) {
    static real_statfs_t real_fn = NULL;
    if (!real_fn) real_fn = (real_statfs_t)dlsym(RTLD_NEXT, "statfs");
    const char *actual = pathname ? remap_path(pathname) : pathname;
    int diag = FS_DIAG_TMP_PREFIX(pathname);
    if (diag) FS_DIAG_LOG("statfs(%s) -> %s", pathname ? pathname : "(null)",
                          actual ? actual : "(null)");
    int ret = real_fn(actual, buf);
    if (diag) FS_DIAG_LOG("statfs(%s) ret=%d errno=%d",
                          pathname ? pathname : "(null)", ret, ret ? errno : 0);
    return ret;
}

typedef int (*real_statvfs64_t)(const char *, void *);
int statvfs64(const char *pathname, void *buf) {
    static real_statvfs64_t real_fn = NULL;
    if (!real_fn) real_fn = (real_statvfs64_t)dlsym(RTLD_NEXT, "statvfs64");
    const char *actual = pathname ? remap_path(pathname) : pathname;
    int diag = FS_DIAG_TMP_PREFIX(pathname);
    if (diag) FS_DIAG_LOG("statvfs64(%s) -> %s", pathname ? pathname : "(null)",
                          actual ? actual : "(null)");
    int ret = real_fn(actual, buf);
    if (diag) FS_DIAG_LOG("statvfs64(%s) ret=%d errno=%d",
                          pathname ? pathname : "(null)", ret, ret ? errno : 0);
    return ret;
}

/* glob64 — pattern expansion. We remap the pattern itself (so a pattern
 * like "/tmp/cache/*" lands at $SYSROOT/tmp/cache/*) and forward.
 * The result paths in the glob_t come back as the resolved sysroot paths
 * — that's a leak (callers may compare them against the original
 * pattern's prefix) but is the same behavior remap_path produces for
 * other hooks. Acceptable for now; revisit if a glob caller depends on
 * "/tmp/" prefix in returned paths. */
struct shim_glob_t_opaque;  /* forward decl */
typedef int (*real_glob64_t)(const char *, int,
                              int (*errfunc)(const char *, int),
                              struct shim_glob_t_opaque *);
int glob64(const char *pattern, int flags,
           int (*errfunc)(const char *, int),
           struct shim_glob_t_opaque *pglob) {
    static real_glob64_t real_fn = NULL;
    if (!real_fn) real_fn = (real_glob64_t)dlsym(RTLD_NEXT, "glob64");
    const char *actual = pattern ? remap_path(pattern) : pattern;
    int diag = FS_DIAG_TMP_PREFIX(pattern);
    if (diag) FS_DIAG_LOG("glob64(%s) -> %s", pattern ? pattern : "(null)",
                          actual ? actual : "(null)");
    int ret = real_fn(actual, flags, errfunc, pglob);
    if (diag) FS_DIAG_LOG("glob64(%s) ret=%d", pattern ? pattern : "(null)", ret);
    return ret;
}

/* ========== Comprehensive fs hook coverage (2026-05-04) ==========
 * Comprehensive instrumentation for the SceneCache investigation: every
 * fs-related libc function the game imports gets a hook. Path-bearing
 * hooks remap. fd-only hooks log when SPIKE3_FS_DIAG_ALL=1 (or when the
 * fs_diag filter matches). Goal is black-box visibility — assume hidden
 * call paths and instrument exhaustively. */

/* readdir/readdir64 — log entry name through the proper struct dirent.
 * d_name access via the real header avoids any layout guesswork. */
typedef struct dirent *(*real_readdir_t)(DIR *dirp);
struct dirent *readdir(DIR *dirp) {
    static real_readdir_t real_fn = NULL;
    if (!real_fn) real_fn = (real_readdir_t)dlsym(RTLD_NEXT, "readdir");
    struct dirent *ret = real_fn(dirp);
    if (ret && FS_DIAG_TMP_PREFIX(ret->d_name)) {
        FS_DIAG_LOG("readdir(dir=%p) -> %s", (void *)dirp, ret->d_name);
    }
    return ret;
}

typedef struct dirent64 *(*real_readdir64_t)(DIR *dirp);
struct dirent64 *readdir64(DIR *dirp) {
    static real_readdir64_t real_fn = NULL;
    if (!real_fn) real_fn = (real_readdir64_t)dlsym(RTLD_NEXT, "readdir64");
    struct dirent64 *ret = real_fn(dirp);
    if (ret && FS_DIAG_TMP_PREFIX(ret->d_name)) {
        FS_DIAG_LOG("readdir64(dir=%p) -> %s", (void *)dirp, ret->d_name);
    }
    return ret;
}

typedef int (*real_closedir_t)(DIR *dirp);
int closedir(DIR *dirp) {
    static real_closedir_t real_fn = NULL;
    if (!real_fn) real_fn = (real_closedir_t)dlsym(RTLD_NEXT, "closedir");
    int ret = real_fn(dirp);
    if (FS_DIAG_TMP_PREFIX("")) {
        FS_DIAG_LOG("closedir(dir=%p) ret=%d", (void *)dirp, ret);
    }
    return ret;
}

typedef void (*real_rewinddir_t)(DIR *dirp);
void rewinddir(DIR *dirp) {
    static real_rewinddir_t real_fn = NULL;
    if (!real_fn) real_fn = (real_rewinddir_t)dlsym(RTLD_NEXT, "rewinddir");
    if (FS_DIAG_TMP_PREFIX("")) {
        FS_DIAG_LOG("rewinddir(dir=%p)", (void *)dirp);
    }
    real_fn(dirp);
}

/* getcwd — the game queries CWD here. Log unconditionally so we can
 * correlate scene-cache enumeration with the running CWD. */
typedef char *(*real_getcwd_t)(char *buf, size_t size);
char *getcwd(char *buf, size_t size) {
    static real_getcwd_t real_fn = NULL;
    if (!real_fn) real_fn = (real_getcwd_t)dlsym(RTLD_NEXT, "getcwd");
    char *ret = real_fn(buf, size);
    static int logged = 0;
    /* Log first 16 calls + every 1000th to keep volume manageable. */
    int count = __atomic_add_fetch(&logged, 1, __ATOMIC_RELAXED);
    if (count <= 16 || (count % 1000) == 0) {
        FS_DIAG_LOG("getcwd #%d -> %s", count, ret ? ret : "(null)");
    }
    return ret;
}

/* fdopen — fd → FILE* conversion. No path; log fd. */
typedef FILE *(*real_fdopen_t)(int fd, const char *mode);
FILE *fdopen(int fd, const char *mode) {
    static real_fdopen_t real_fn = NULL;
    if (!real_fn) real_fn = (real_fdopen_t)dlsym(RTLD_NEXT, "fdopen");
    FILE *ret = real_fn(fd, mode);
    if (FS_DIAG_TMP_PREFIX("")) {
        FS_DIAG_LOG("fdopen(fd=%d, mode=%s) -> %p", fd, mode ? mode : "(null)", (void *)ret);
    }
    return ret;
}

/* fstat64 — fd-based stat. No path. */
typedef int (*real_fstat64_t)(int fd, void *statbuf);
int fstat64(int fd, void *statbuf) {
    static real_fstat64_t real_fn = NULL;
    if (!real_fn) real_fn = (real_fstat64_t)dlsym(RTLD_NEXT, "fstat64");
    int ret = real_fn(fd, statbuf);
    if (FS_DIAG_TMP_PREFIX("")) {
        FS_DIAG_LOG("fstat64(fd=%d) ret=%d errno=%d", fd, ret, ret ? errno : 0);
    }
    return ret;
}

/* ftruncate / ftruncate64 — fd-based truncate. */
typedef int (*real_ftruncate_t)(int fd, off_t length);
int ftruncate(int fd, off_t length) {
    static real_ftruncate_t real_fn = NULL;
    if (!real_fn) real_fn = (real_ftruncate_t)dlsym(RTLD_NEXT, "ftruncate");
    int ret = real_fn(fd, length);
    if (FS_DIAG_TMP_PREFIX("")) {
        FS_DIAG_LOG("ftruncate(fd=%d, length=%ld) ret=%d", fd, (long)length, ret);
    }
    return ret;
}

typedef int (*real_ftruncate64_t)(int fd, off_t length);
int ftruncate64(int fd, off_t length) {
    static real_ftruncate64_t real_fn = NULL;
    if (!real_fn) real_fn = (real_ftruncate64_t)dlsym(RTLD_NEXT, "ftruncate64");
    int ret = real_fn(fd, length);
    if (FS_DIAG_TMP_PREFIX("")) {
        FS_DIAG_LOG("ftruncate64(fd=%d, length=%ld) ret=%d", fd, (long)length, ret);
    }
    return ret;
}

/* popen — runs a shell command, returns FILE*. The command is parsed by
 * /bin/sh; we don't try to parse out paths within. Log the full command. */
typedef FILE *(*real_popen_t)(const char *command, const char *type);
FILE *popen(const char *command, const char *type) {
    static real_popen_t real_fn = NULL;
    if (!real_fn) real_fn = (real_popen_t)dlsym(RTLD_NEXT, "popen");
    SHIM_LOG("popen(\"%s\", \"%s\")", command ? command : "(null)",
             type ? type : "(null)");
    return real_fn(command, type);
}

/* system() is hooked in spi_shim.c (path-rewrite logic for /connectivity/...).
 * Don't redefine here. */

/* execvp — execute a command with PATH search. Log program + first arg. */
typedef int (*real_execvp_t)(const char *file, char *const argv[]);
int execvp(const char *file, char *const argv[]) {
    static real_execvp_t real_fn = NULL;
    if (!real_fn) real_fn = (real_execvp_t)dlsym(RTLD_NEXT, "execvp");
    SHIM_LOG("execvp(file=%s, argv[1]=%s)",
             file ? file : "(null)",
             (argv && argv[0] && argv[1]) ? argv[1] : "(none)");
    return real_fn(file, argv);
}

/* dup2 — fd duplication. */
typedef int (*real_dup2_t)(int oldfd, int newfd);
int dup2(int oldfd, int newfd) {
    static real_dup2_t real_fn = NULL;
    if (!real_fn) real_fn = (real_dup2_t)dlsym(RTLD_NEXT, "dup2");
    int ret = real_fn(oldfd, newfd);
    if (FS_DIAG_TMP_PREFIX("")) {
        FS_DIAG_LOG("dup2(old=%d, new=%d) ret=%d", oldfd, newfd, ret);
    }
    return ret;
}

/* fchmod — fd-based chmod. */
typedef int (*real_fchmod_t)(int fd, unsigned int mode);
int fchmod(int fd, unsigned int mode) {
    static real_fchmod_t real_fn = NULL;
    if (!real_fn) real_fn = (real_fchmod_t)dlsym(RTLD_NEXT, "fchmod");
    int ret = real_fn(fd, mode);
    if (FS_DIAG_TMP_PREFIX("")) {
        FS_DIAG_LOG("fchmod(fd=%d, mode=0%o) ret=%d", fd, mode, ret);
    }
    return ret;
}

/* fcntl64 intentionally NOT hooked: variadic third arg + cmd-dependent
 * type means safely intercepting it would require a cmd-by-cmd switch.
 * Skip until we have evidence a specific cmd matters. */

/* __openat64_2 / __open_2 / __open64_2 / __openat_2 — FORTIFY_SOURCE
 * hardened variants of the open family. Libstdc++ imports __openat64_2
 * (verified via nm -D on libstdc++.so.6.0.32), so std::basic_filebuf::open
 * → __openat64_2 → glibc syscall — bypasses our public-name hooks on
 * openat/openat64. Without these hooks, every .radium open via ifstream
 * is invisible to the shim. Same remap_path treatment as the non-_2
 * variants. */
typedef int (*real_openat64_2_t)(int dirfd, const char *pathname, int flags);
int __openat64_2(int dirfd, const char *pathname, int flags) {
    static real_openat64_2_t real_fn = NULL;
    if (!real_fn) real_fn = (real_openat64_2_t)dlsym(RTLD_NEXT, "__openat64_2");
    int fake = pathname ? intercept_device(pathname) : 0;
    if (fake) return fake;
    const char *actual = pathname ? remap_path(pathname) : pathname;
    int diag = FS_DIAG_TMP_PREFIX(pathname);
    if (diag) FS_DIAG_LOG("__openat64_2(dirfd=%d, %s, flags=0x%x) -> %s",
                          dirfd, pathname ? pathname : "(null)", flags,
                          actual ? actual : "(null)");
    int ret = real_fn ? real_fn(dirfd, actual, flags) : -1;
    if (diag) FS_DIAG_LOG("__openat64_2(%s) ret=%d errno=%d",
                          pathname ? pathname : "(null)", ret,
                          (ret < 0) ? errno : 0);
    return ret;
}

typedef int (*real_open_2_t)(const char *pathname, int flags);
int __open_2(const char *pathname, int flags) {
    static real_open_2_t real_fn = NULL;
    if (!real_fn) real_fn = (real_open_2_t)dlsym(RTLD_NEXT, "__open_2");
    int fake = pathname ? intercept_device(pathname) : 0;
    if (fake) return fake;
    const char *actual = pathname ? remap_path(pathname) : pathname;
    int diag = FS_DIAG_TMP_PREFIX(pathname);
    if (diag) FS_DIAG_LOG("__open_2(%s, flags=0x%x) -> %s",
                          pathname ? pathname : "(null)", flags,
                          actual ? actual : "(null)");
    int ret = real_fn ? real_fn(actual, flags) : -1;
    if (diag) FS_DIAG_LOG("__open_2(%s) ret=%d errno=%d",
                          pathname ? pathname : "(null)", ret,
                          (ret < 0) ? errno : 0);
    return ret;
}

int __open64_2(const char *pathname, int flags) {
    static real_open_2_t real_fn = NULL;
    if (!real_fn) real_fn = (real_open_2_t)dlsym(RTLD_NEXT, "__open64_2");
    int fake = pathname ? intercept_device(pathname) : 0;
    if (fake) return fake;
    const char *actual = pathname ? remap_path(pathname) : pathname;
    int diag = FS_DIAG_TMP_PREFIX(pathname);
    if (diag) FS_DIAG_LOG("__open64_2(%s, flags=0x%x) -> %s",
                          pathname ? pathname : "(null)", flags,
                          actual ? actual : "(null)");
    int ret = real_fn ? real_fn(actual, flags) : -1;
    if (diag) FS_DIAG_LOG("__open64_2(%s) ret=%d errno=%d",
                          pathname ? pathname : "(null)", ret,
                          (ret < 0) ? errno : 0);
    return ret;
}

int __openat_2(int dirfd, const char *pathname, int flags) {
    static real_openat64_2_t real_fn = NULL;
    if (!real_fn) real_fn = (real_openat64_2_t)dlsym(RTLD_NEXT, "__openat_2");
    int fake = pathname ? intercept_device(pathname) : 0;
    if (fake) return fake;
    const char *actual = pathname ? remap_path(pathname) : pathname;
    int diag = FS_DIAG_TMP_PREFIX(pathname);
    if (diag) FS_DIAG_LOG("__openat_2(dirfd=%d, %s, flags=0x%x) -> %s",
                          dirfd, pathname ? pathname : "(null)", flags,
                          actual ? actual : "(null)");
    int ret = real_fn ? real_fn(dirfd, actual, flags) : -1;
    if (diag) FS_DIAG_LOG("__openat_2(%s) ret=%d errno=%d",
                          pathname ? pathname : "(null)", ret,
                          (ret < 0) ? errno : 0);
    return ret;
}

/* Internal-name aliases of the open family. glibc exports both the public
 * (open/openat/open64/openat64) AND the underscored internal (__open etc.)
 * names. libstdc++ MAY call the underscored ones to bypass FORTIFY checks
 * or for other internal reasons. LD_PRELOAD wraps by symbol name, so
 * hooking the public name doesn't catch calls to the underscored alias. */
typedef int (*real_open_t)(const char *, int, ...);
int __open(const char *pathname, int flags, ...) {
    static real_open_t real_fn = NULL;
    if (!real_fn) real_fn = (real_open_t)dlsym(RTLD_NEXT, "__open");
    int fake = pathname ? intercept_device(pathname) : 0;
    if (fake) return fake;
    const char *actual = pathname ? remap_path(pathname) : pathname;
    int diag = FS_DIAG_TMP_PREFIX(pathname);
    int ret;
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags);
        int mode = va_arg(ap, int);
        va_end(ap);
        ret = real_fn ? real_fn(actual, flags, mode) : -1;
    } else {
        ret = real_fn ? real_fn(actual, flags) : -1;
    }
    if (diag) FS_DIAG_LOG("__open(%s, flags=0x%x) -> %s ret=%d errno=%d",
                          pathname ? pathname : "(null)", flags,
                          actual ? actual : "(null)", ret,
                          (ret < 0) ? errno : 0);
    return ret;
}

int __open64(const char *pathname, int flags, ...) {
    static real_open_t real_fn = NULL;
    if (!real_fn) real_fn = (real_open_t)dlsym(RTLD_NEXT, "__open64");
    int fake = pathname ? intercept_device(pathname) : 0;
    if (fake) return fake;
    const char *actual = pathname ? remap_path(pathname) : pathname;
    int diag = FS_DIAG_TMP_PREFIX(pathname);
    int ret;
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags);
        int mode = va_arg(ap, int);
        va_end(ap);
        ret = real_fn ? real_fn(actual, flags, mode) : -1;
    } else {
        ret = real_fn ? real_fn(actual, flags) : -1;
    }
    if (diag) FS_DIAG_LOG("__open64(%s, flags=0x%x) -> %s ret=%d errno=%d",
                          pathname ? pathname : "(null)", flags,
                          actual ? actual : "(null)", ret,
                          (ret < 0) ? errno : 0);
    return ret;
}

typedef int (*real_openat_int_t)(int, const char *, int, ...);
int __openat(int dirfd, const char *pathname, int flags, ...) {
    static real_openat_int_t real_fn = NULL;
    if (!real_fn) real_fn = (real_openat_int_t)dlsym(RTLD_NEXT, "__openat");
    int fake = pathname ? intercept_device(pathname) : 0;
    if (fake) return fake;
    const char *actual = pathname ? remap_path(pathname) : pathname;
    int diag = FS_DIAG_TMP_PREFIX(pathname);
    int ret;
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags);
        int mode = va_arg(ap, int);
        va_end(ap);
        ret = real_fn ? real_fn(dirfd, actual, flags, mode) : -1;
    } else {
        ret = real_fn ? real_fn(dirfd, actual, flags) : -1;
    }
    if (diag) FS_DIAG_LOG("__openat(dirfd=%d, %s, flags=0x%x) -> %s ret=%d errno=%d",
                          dirfd, pathname ? pathname : "(null)", flags,
                          actual ? actual : "(null)", ret,
                          (ret < 0) ? errno : 0);
    return ret;
}

int __openat64(int dirfd, const char *pathname, int flags, ...) {
    static real_openat_int_t real_fn = NULL;
    if (!real_fn) real_fn = (real_openat_int_t)dlsym(RTLD_NEXT, "__openat64");
    int fake = pathname ? intercept_device(pathname) : 0;
    if (fake) return fake;
    const char *actual = pathname ? remap_path(pathname) : pathname;
    int diag = FS_DIAG_TMP_PREFIX(pathname);
    int ret;
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags);
        int mode = va_arg(ap, int);
        va_end(ap);
        ret = real_fn ? real_fn(dirfd, actual, flags, mode) : -1;
    } else {
        ret = real_fn ? real_fn(dirfd, actual, flags) : -1;
    }
    if (diag) FS_DIAG_LOG("__openat64(dirfd=%d, %s, flags=0x%x) -> %s ret=%d errno=%d",
                          dirfd, pathname ? pathname : "(null)", flags,
                          actual ? actual : "(null)", ret,
                          (ret < 0) ? errno : 0);
    return ret;
}

/* fork — child process creation. Log the call; child's pid is logged on
 * return. The child inherits our shim via LD_PRELOAD environment. */
typedef pid_t (*real_fork_t)(void);
pid_t fork(void) {
    static real_fork_t real_fn = NULL;
    if (!real_fn) real_fn = (real_fork_t)dlsym(RTLD_NEXT, "fork");
    pid_t ret = real_fn();
    SHIM_LOG("fork() ret=%d (parent_view)", (int)ret);
    return ret;
}

/* mount / umount2 — fs-mount manipulation. Log path operands but DO NOT
 * remap (these are kernel-level ops that the game shouldn't really be
 * using; logging serves as a tripwire). */
typedef int (*real_mount_t)(const char *src, const char *target,
                             const char *fstype, unsigned long flags,
                             const void *data);
int mount(const char *src, const char *target, const char *fstype,
          unsigned long flags, const void *data) {
    static real_mount_t real_fn = NULL;
    if (!real_fn) real_fn = (real_mount_t)dlsym(RTLD_NEXT, "mount");
    SHIM_LOG("mount(src=%s, target=%s, fstype=%s, flags=0x%lx)",
             src ? src : "(null)", target ? target : "(null)",
             fstype ? fstype : "(null)", flags);
    return real_fn(src, target, fstype, flags, data);
}

typedef int (*real_umount2_t)(const char *target, int flags);
int umount2(const char *target, int flags) {
    static real_umount2_t real_fn = NULL;
    if (!real_fn) real_fn = (real_umount2_t)dlsym(RTLD_NEXT, "umount2");
    SHIM_LOG("umount2(target=%s, flags=0x%x)",
             target ? target : "(null)", flags);
    return real_fn(target, flags);
}

/* sync / syncfs — global / fd-targeted fs sync. */
typedef void (*real_sync_t)(void);
void sync(void) {
    static real_sync_t real_fn = NULL;
    if (!real_fn) real_fn = (real_sync_t)dlsym(RTLD_NEXT, "sync");
    if (FS_DIAG_TMP_PREFIX("")) FS_DIAG_LOG("sync()");
    real_fn();
}

typedef int (*real_syncfs_t)(int fd);
int syncfs(int fd) {
    static real_syncfs_t real_fn = NULL;
    if (!real_fn) real_fn = (real_syncfs_t)dlsym(RTLD_NEXT, "syncfs");
    int ret = real_fn(fd);
    if (FS_DIAG_TMP_PREFIX("")) {
        FS_DIAG_LOG("syncfs(fd=%d) ret=%d", fd, ret);
    }
    return ret;
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

