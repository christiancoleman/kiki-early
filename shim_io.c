/*
 * shim_io.c — byte-level I/O hooks on the shim's fake fds.
 *
 * Dispatch order mirrors the intercept_device() contract:
 *   - GPIO fake fds (pool defined in shim_fs.c)
 *   - SPI / I2C / V4L2 fake fds (pure-fake fds, no backing socketpair)
 *   - UART socketpair fd (real fd; bridge-response synthesis lives in
 *     shim_nodebus.c and is called from write() when the game sends a frame)
 *   - DRM socketpair fd (real fd; shim_ioctl.c pushes synthesized page-flip
 *     events via real_write() which unblocks read() here)
 *   - everything else falls through to real libc
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
    if (fd == FAKE_FD_BOOT_DISPLAY && boot_display_active) {
        SHIM_LOG("CLOSE boot_display (fd %d)", fd);
        boot_display_active = 0;
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

ssize_t read(int fd, void *buf, size_t count) {
    resolve_all();

    if (is_gpio_fake_fd(fd)) {
        int idx = fd - FAKE_FD_GPIO_BASE;
        if (gpio_fake_read_done[idx]) return 0;
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

ssize_t write(int fd, const void *buf, size_t count) {
    resolve_all();

    if (is_gpio_fake_fd(fd)) {
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
        return (ssize_t)count;
    }

    if (is_boot_display_fd(fd)) {
        /* Swallow the write, but log size + cumulative volume. The traffic
         * shape (small text-y writes vs huge fragmented frame writes) tells
         * us whether /dev/boot_display is a command channel or a render
         * surface — see _emulation runs/latest/boot_display.log analysis. */
        static uint32_t _bd_write_count = 0;
        static uint64_t _bd_write_bytes = 0;
        uint32_t n = ++_bd_write_count;
        _bd_write_bytes += count;
        if (n <= 8 || (n % 100) == 0) {
            SHIM_LOG("[boot_display] write #%u: %zu bytes (cumulative=%lu)",
                     n, count, (unsigned long)_bd_write_bytes);
        }
        return (ssize_t)count;
    }

    return real_write(fd, buf, count);
}

/* On 64-bit aarch64 the LFS entry points (lseek64 / pread64 / pwrite64) are
 * functionally identical to their plain counterparts (off_t is already
 * 64-bit), but PLT symbols are separate so callers using the 64-bit variant
 * bypass the plain-variant hook. Hook both. */

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
        return (ssize_t)count;
    }
    static real_pwrite64_t real_fn = NULL;
    if (!real_fn) real_fn = (real_pwrite64_t)dlsym(RTLD_NEXT, "pwrite64");
    return real_fn(fd, buf, count, offset);
}

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

#include <sys/mman.h>

typedef void * (*real_mmap_t)(void *, size_t, int, int, int, off_t);
typedef void * (*real_mmap64_t)(void *, size_t, int, int, int, off64_t);
typedef int    (*real_munmap_t)(void *, size_t);

static const char *mmap_flag_summary(int flags) {
    if (flags & MAP_ANONYMOUS) return "ANON";
    if ((flags & MAP_PRIVATE) && (flags & MAP_FIXED)) return "PRIV|FIXED";
    if (flags & MAP_PRIVATE) return "PRIV";
    if (flags & MAP_SHARED)  return "SHARED";
    return "other";
}

/* Resolve a live file descriptor to its path via /proc/self/fd/<N>.
 * Returns the input `out` buffer on success, "?" on failure. Always
 * NUL-terminates. Used by the mmap hooks so the log line tells us
 * *what file* was mapped instead of just an opaque fd number. */
static const char *mmap_fd_to_path(int fd, char *out, size_t outsz) {
    if (outsz == 0) return "?";
    char link[64];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, out, outsz - 1);
    if (n <= 0) { out[0] = '?'; out[1] = 0; return out; }
    out[n] = 0;
    return out;
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
            char path[512];
            mmap_fd_to_path(fd, path, sizeof(path));
            SHIM_LOG("[mm] mmap #%u: fd=%d(%s) len=%zu prot=0x%x flags=%s(0x%x) off=%lld -> %p",
                     n, fd, path, length, prot, mmap_flag_summary(flags), flags,
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
            char path[512];
            mmap_fd_to_path(fd, path, sizeof(path));
            SHIM_LOG("[mm] mmap64 #%u: fd=%d(%s) len=%zu prot=0x%x flags=%s(0x%x) off=%lld -> %p",
                     n, fd, path, length, prot, mmap_flag_summary(flags), flags,
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

