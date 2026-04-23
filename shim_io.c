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

