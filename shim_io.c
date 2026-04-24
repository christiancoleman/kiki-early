/*
 * shim_io.c — byte-level I/O hooks on the shim's fake fds.
 *
 * Phase 7 of the refactor. Owns: close / read / lseek / write.
 *
 * Dispatch order mirrors the intercept_device() contract:
 *   - GPIO fake fds (pool defined in shim_fs.c)
 *   - SPI / I2C / V4L2 fake fds (pure-fake fds, no backing socketpair)
 *   - UART socketpair fd (real fd; bridge-response synthesis lives in
 *     shim_nodebus.c and is called from write() when the game sends a frame)
 *   - DRM socketpair fd (real fd; shim_ioctl.c pushes synthesized page-flip
 *     events via real_write() which unblocks read() here)
 *   - everything else falls through to real libc
 *
 * Cross-TU dependencies resolved via spi_shim.h:
 *   - Device-state externs (spi_active, i2c_active, etc.)
 *   - fd predicates (is_fake_fd, is_uart_fd, is_drm_fd)
 *   - FAKE_FD_* constants
 *   - real_close / real_read / real_write function pointers
 *   - GPIO fake-fd pool (gpio_fake_paths, gpio_fake_read_done, FAKE_FD_GPIO_BASE,
 *     is_gpio_fake_fd, free_gpio_fake_fd)
 *   - synthesize_bridge_response (from shim_nodebus.c; called from write())
 *   - resolve_all (ensures real_* pointers are bound on first entry)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <dlfcn.h>
#include <sys/types.h>

#include "spi_shim.h"

/* ========== moved from spi_shim.c (1743-1913): I/O hooks: close / read / lseek / write ========== */
/* ---- Hooked: close ---- */

int close(int fd) {
    resolve_all();
    if (is_gpio_fake_fd(fd)) {
        free_gpio_fake_fd(fd);
        return 0;
    }
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
    if (fd == FAKE_FD_V4L2_MEDIA && v4l2_media_active) {
        SHIM_LOG("CLOSE V4L2 media (fd %d) after %u ioctls", fd, v4l2_ioctl_count);
        v4l2_media_active = 0;
        return 0;
    }
    if (fd == FAKE_FD_V4L2_VIDEO && v4l2_video_active) {
        SHIM_LOG("CLOSE V4L2 video (fd %d)", fd);
        v4l2_video_active = 0;
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
    if (is_drm_fd(fd)) {
        SHIM_LOG("CLOSE DRM (game=%d shim=%d)", drm_game_fd, drm_shim_fd);
        int shim_fd = drm_shim_fd;
        int game_fd = drm_game_fd;
        drm_game_fd = -1;
        drm_shim_fd = -1;
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

    if (is_gpio_fake_fd(fd)) {
        int idx = fd - FAKE_FD_GPIO_BASE;
        if (gpio_fake_read_done[idx]) return 0;  /* EOF on 2nd read (real sysfs behavior) */
        const char *p = gpio_fake_paths[idx];
        const char *resp = "";
        if (strstr(p, "/value"))          resp = "0\n";
        else if (strstr(p, "/direction")) resp = "out\n";
        else if (strstr(p, "/edge"))      resp = "none\n";
        else if (strstr(p, "/active_low")) resp = "0\n";
        size_t resp_len = strlen(resp);
        size_t n = count < resp_len ? count : resp_len;
        if (n) memcpy(buf, resp, n);
        gpio_fake_read_done[idx] = 1;
        return (ssize_t)n;
    }

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

    if (is_drm_fd(fd)) {
        /* drmHandleEvent reads here after select() wakes — it will get the
         * synthesized drm_event_vblank we pushed from PAGE_FLIP. */
        ssize_t n = real_read(fd, buf, count);
        static uint32_t drm_read_count = 0;
        drm_read_count++;
        if (n > 0 && drm_read_count <= 8) {
            SHIM_LOG("DRM read #%u got %zd bytes", drm_read_count, n);
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

/* ---- Hooked: lseek (GPIO polling only) ----
 * Some GPIO pollers keep one fd open and lseek(0) between reads. Clear the
 * once-read flag so the next read() re-emits the value. All other fds fall
 * through to libc. */

typedef off_t (*real_lseek_t)(int, off_t, int);
off_t lseek(int fd, off_t offset, int whence) {
    if (is_gpio_fake_fd(fd)) {
        if (whence == SEEK_SET && offset == 0) {
            gpio_fake_read_done[fd - FAKE_FD_GPIO_BASE] = 0;
        }
        return 0;
    }
    static real_lseek_t real_fn = NULL;
    if (!real_fn) real_fn = (real_lseek_t)dlsym(RTLD_NEXT, "lseek");
    return real_fn(fd, offset, whence);
}

/* ---- Hooked: write ---- */


ssize_t write(int fd, const void *buf, size_t count) {
    resolve_all();

    if (is_gpio_fake_fd(fd)) {
        /* Swallow writes to export / direction / value / unexport. */
        return (ssize_t)count;
    }

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

/* ---- LFS variants: lseek64 / pread64 / pwrite64 (defensive coverage) ----
 *
 * glibc's LFS (Large File Support) entry points are distinct PLT symbols from
 * their plain counterparts. On 64-bit aarch64 Linux they're functionally
 * identical (off_t is already 64-bit, so off64_t is the same width), but
 * SYMBOL resolution is separate, so code linked to the 64-bit variant bypasses
 * our plain-variant hooks entirely.
 *
 * ---- The pattern: "when you hook one variant, hook both" ----
 *
 * Any libc function with a 32/64 pair (`*64` suffix) can silently leak traffic
 * through the non-hooked variant. Before Phase 7.post we hooked only plain
 * `lseek`; `lseek64` / `pread64` / `pwrite64` were all invisible. Adding the
 * 64-bit hooks with a FIRST-CALL banner immediately caught `pread64` in use
 * (we'd been missing whatever file reads it was doing for the whole refactor).
 *
 * Remaining asymmetries worth closing later (listed in README "Known gaps"):
 *   - `pread`   — paired with `pread64` (hooked here); plain variant not hooked
 *   - `pwrite`  — paired with `pwrite64` (hooked here); plain variant not hooked
 *   - `fopen`   — paired with `fopen64` (hooked in shim_fs.c); plain variant
 *                 not hooked. This one's a path-taking function so the
 *                 asymmetry can leak un-remapped paths — highest priority of
 *                 the three to fix.
 *
 * When closing an asymmetry, keep the defensive first-call banner for the
 * newly-added variant for at least one smoke run so we observe whether it
 * fires. If it doesn't, the banner cost is essentially zero.
 */

typedef off_t  (*real_lseek64_t)(int, off_t, int);
typedef ssize_t (*real_pread64_t)(int, void *, size_t, off_t);
typedef ssize_t (*real_pwrite64_t)(int, const void *, size_t, off_t);

/* First-call banner + counting helper — shared across the three hooks.
 * `_count` is per-callsite (static inside the macro expansion). */
#define SHIM_LFS_FIRST_CALL(NAME) do {                                      \
    static uint32_t _count = 0;                                             \
    uint32_t _n = __atomic_add_fetch(&_count, 1, __ATOMIC_RELAXED);         \
    if (_n == 1) {                                                          \
        SHIM_LOG("[lfs] FIRST CALL: " NAME                                  \
                 " — hook now wired (was previously untracked)");           \
    } else if ((_n % 1000) == 0) {                                          \
        SHIM_LOG("[lfs] " NAME " #%u", _n);                                 \
    }                                                                       \
} while (0)

off_t lseek64(int fd, off_t offset, int whence) {
    SHIM_LFS_FIRST_CALL("lseek64");

    if (is_gpio_fake_fd(fd)) {
        if (whence == SEEK_SET && offset == 0) {
            gpio_fake_read_done[fd - FAKE_FD_GPIO_BASE] = 0;
        }
        return 0;
    }
    static real_lseek64_t real_fn = NULL;
    if (!real_fn) real_fn = (real_lseek64_t)dlsym(RTLD_NEXT, "lseek64");
    return real_fn(fd, offset, whence);
}

ssize_t pread64(int fd, void *buf, size_t count, off_t offset) {
    /* pread64 DID fire under Pokémon LE (surfaced by the first-call banner
     * the first time this hook was wired). We don't yet know which fd(s) or
     * what the game is reading — emit detailed fd/offset/count for the first
     * 8 calls, plus every 500th after that, to map the usage pattern. Follow
     * up separately when someone has cycles to investigate the fd provenance. */
    static uint32_t _pread_count = 0;
    uint32_t _n = __atomic_add_fetch(&_pread_count, 1, __ATOMIC_RELAXED);
    if (_n <= 8 || (_n % 500) == 0) {
        SHIM_LOG("[lfs] pread64 #%u: fd=%d offset=%lld count=%zu",
                 _n, fd, (long long)offset, count);
    }

    /* GPIO sysfs files are ordinary 1-byte-ish reads; a pread(offset=0) on
     * one should behave like read(). For any non-zero offset we return 0
     * (EOF) — real sysfs value/direction files don't support random access. */
    if (is_gpio_fake_fd(fd)) {
        if (offset != 0) return 0;
        /* Temporarily clear the read-done latch so the regular read() path
         * re-emits the value. Restore-on-return not needed since pread is
         * conceptually atomic per-call. */
        gpio_fake_read_done[fd - FAKE_FD_GPIO_BASE] = 0;
        return read(fd, buf, count);
    }
    static real_pread64_t real_fn = NULL;
    if (!real_fn) real_fn = (real_pread64_t)dlsym(RTLD_NEXT, "pread64");
    return real_fn(fd, buf, count, offset);
}

ssize_t pwrite64(int fd, const void *buf, size_t count, off_t offset) {
    SHIM_LFS_FIRST_CALL("pwrite64");
    (void)offset;  /* GPIO sysfs writes ignore position */

    if (is_gpio_fake_fd(fd)) {
        /* Same swallow semantics as write() on GPIO fake fds. */
        return (ssize_t)count;
    }
    static real_pwrite64_t real_fn = NULL;
    if (!real_fn) real_fn = (real_pwrite64_t)dlsym(RTLD_NEXT, "pwrite64");
    return real_fn(fd, buf, count, offset);
}

/* ---- Hooked: pread / pwrite (plain — Phase-7 asymmetries closed 2026-04-24) ----
 *
 * On 64-bit aarch64 these are ABI-identical to pread64/pwrite64 (off_t is
 * already 64-bit), but PLT symbols are distinct so a caller using the
 * plain variant would bypass our pread64/pwrite64 hook. Mirror the
 * 64-bit bodies exactly so both paths land the same. */
typedef ssize_t (*real_pread_t)(int, void *, size_t, off_t);
typedef ssize_t (*real_pwrite_t)(int, const void *, size_t, off_t);

ssize_t pread(int fd, void *buf, size_t count, off_t offset) {
    static uint32_t _pread_count = 0;
    uint32_t _n = __atomic_add_fetch(&_pread_count, 1, __ATOMIC_RELAXED);
    if (_n == 1) {
        SHIM_LOG("[lfs] FIRST CALL: pread — hook now wired "
                 "(was previously untracked)");
    }
    if (_n <= 8 || (_n % 500) == 0) {
        SHIM_LOG("[lfs] pread #%u: fd=%d offset=%lld count=%zu",
                 _n, fd, (long long)offset, count);
    }
    if (is_gpio_fake_fd(fd)) {
        if (offset != 0) return 0;
        gpio_fake_read_done[fd - FAKE_FD_GPIO_BASE] = 0;
        return read(fd, buf, count);
    }
    static real_pread_t real_fn = NULL;
    if (!real_fn) real_fn = (real_pread_t)dlsym(RTLD_NEXT, "pread");
    return real_fn(fd, buf, count, offset);
}

ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset) {
    SHIM_LFS_FIRST_CALL("pwrite");
    (void)offset;
    if (is_gpio_fake_fd(fd)) return (ssize_t)count;
    static real_pwrite_t real_fn = NULL;
    if (!real_fn) real_fn = (real_pwrite_t)dlsym(RTLD_NEXT, "pwrite");
    return real_fn(fd, buf, count, offset);
}

/* ---- Observation hooks: mmap / mmap64 / munmap (2026-04-24) ----
 *
 * The GAME BINARY doesn't import mmap in its dynsym — its code (if it uses
 * mmap at all) resolves the syscall inside glibc via INLINE_SYSCALL and
 * bypasses LD_PRELOAD. But other libraries in the process (Mesa's shader
 * cache, libavcodec, libcurl, dlopen's .so-loader) import mmap normally
 * and their calls DO go through the PLT → our override.
 *
 * So this hook's diagnostic value is: "what libs are doing mmap at
 * runtime," not "what the game is doing with main.pck." We filter out
 * anonymous mappings (malloc arenas, stack growth — pure noise) and log
 * only file-backed mappings (fd != -1) where the interesting activity
 * lives. First 8 + every 500th sampled, same pattern as pread64.
 *
 * All three functions pass through to real libc; we never intercept the
 * actual mapping. */

#include <sys/mman.h>  /* MAP_ANONYMOUS, off64_t-via-_GNU_SOURCE */

typedef void * (*real_mmap_t)(void *, size_t, int, int, int, off_t);
typedef void * (*real_mmap64_t)(void *, size_t, int, int, int, off64_t);
typedef int    (*real_munmap_t)(void *, size_t);

static const char *mmap_flag_summary(int flags) {
    /* Return a short, non-allocating label for the most common cases. */
    if (flags & MAP_ANONYMOUS) return "ANON";
    if ((flags & MAP_PRIVATE) && (flags & MAP_FIXED)) return "PRIV|FIXED";
    if (flags & MAP_PRIVATE) return "PRIV";
    if (flags & MAP_SHARED)  return "SHARED";
    return "other";
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    static real_mmap_t real_fn = NULL;
    if (!real_fn) real_fn = (real_mmap_t)dlsym(RTLD_NEXT, "mmap");
    void *ret = real_fn(addr, length, prot, flags, fd, offset);

    /* Skip anonymous mappings (malloc / stack) — pure noise. Only observe
     * file-backed mappings, which are where the interesting activity lives. */
    if (!(flags & MAP_ANONYMOUS) && fd >= 0) {
        static uint32_t _n = 0;
        uint32_t n = __atomic_add_fetch(&_n, 1, __ATOMIC_RELAXED);
        if (n == 1) {
            SHIM_LOG("[mm] FIRST CALL: mmap (file-backed) — hook wired");
        }
        if (n <= 8 || (n % 500) == 0) {
            SHIM_LOG("[mm] mmap #%u: fd=%d len=%zu prot=0x%x flags=%s(0x%x) off=%lld -> %p",
                     n, fd, length, prot, mmap_flag_summary(flags), flags,
                     (long long)offset, ret);
        }
    }
    return ret;
}

void *mmap64(void *addr, size_t length, int prot, int flags, int fd, off64_t offset) {
    static real_mmap64_t real_fn = NULL;
    if (!real_fn) real_fn = (real_mmap64_t)dlsym(RTLD_NEXT, "mmap64");
    void *ret = real_fn(addr, length, prot, flags, fd, offset);
    if (!(flags & MAP_ANONYMOUS) && fd >= 0) {
        static uint32_t _n = 0;
        uint32_t n = __atomic_add_fetch(&_n, 1, __ATOMIC_RELAXED);
        if (n == 1) {
            SHIM_LOG("[mm] FIRST CALL: mmap64 (file-backed) — hook wired");
        }
        if (n <= 8 || (n % 500) == 0) {
            SHIM_LOG("[mm] mmap64 #%u: fd=%d len=%zu prot=0x%x flags=%s(0x%x) off=%lld -> %p",
                     n, fd, length, prot, mmap_flag_summary(flags), flags,
                     (long long)offset, ret);
        }
    }
    return ret;
}

int munmap(void *addr, size_t length) {
    static real_munmap_t real_fn = NULL;
    if (!real_fn) real_fn = (real_munmap_t)dlsym(RTLD_NEXT, "munmap");
    int ret = real_fn(addr, length);
    /* munmap doesn't carry an fd, so we can't filter anon vs file-backed.
     * Log sparsely — first 8 + every 1000th. If it proves noisy, tighten. */
    static uint32_t _n = 0;
    uint32_t n = __atomic_add_fetch(&_n, 1, __ATOMIC_RELAXED);
    if (n == 1) {
        SHIM_LOG("[mm] FIRST CALL: munmap — hook wired");
    }
    if (n <= 8 || (n % 1000) == 0) {
        SHIM_LOG("[mm] munmap #%u: addr=%p len=%zu -> %d", n, addr, length, ret);
    }
    return ret;
}

