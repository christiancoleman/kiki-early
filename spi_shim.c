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
#include <termios.h>
#include <stdlib.h>

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

/* ---- Fake fd assignments ---- */

#define FAKE_FD_SPI   1023
#define FAKE_FD_I2C   1022
#define FAKE_FD_UART  1021

/* ---- Device state ---- */

static int spi_active = 0;
static int i2c_active = 0;
static int uart_active = 0;
static uint32_t spi_transfer_count = 0;
static uint32_t i2c_ioctl_count = 0;
static uint32_t uart_read_count = 0;
static uint32_t uart_write_count = 0;

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
    if (strcmp(pathname, "/dev/ttyAMA5") == 0) {
        uart_active = 1;
        uart_read_count = 0;
        uart_write_count = 0;
        SHIM_LOG("OPEN %s -> fake fd %d (UART bridge)", pathname, FAKE_FD_UART);
        return FAKE_FD_UART;
    }
    return 0;
}

static int is_fake_fd(int fd) {
    return (fd == FAKE_FD_SPI  && spi_active) ||
           (fd == FAKE_FD_I2C  && i2c_active) ||
           (fd == FAKE_FD_UART && uart_active);
}

/* ---- Hooked: open / openat ---- */

int open(const char *pathname, int flags, ...) {
    resolve_all();
    int fake = intercept_device(pathname);
    if (fake) return fake;

    const char *actual = remap_path(pathname);

    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        int mode = va_arg(args, int);
        va_end(args);
        return real_open(actual, flags, mode);
    }
    return real_open(actual, flags);
}

typedef int (*real_openat_t)(int, const char *, int, ...);
static real_openat_t real_openat = NULL;

int openat(int dirfd, const char *pathname, int flags, ...) {
    if (!real_openat) real_openat = (real_openat_t)dlsym(RTLD_NEXT, "openat");

    int fake = intercept_device(pathname);
    if (fake) return fake;

    const char *actual = remap_path(pathname);

    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        int mode = va_arg(args, int);
        va_end(args);
        return real_openat(dirfd, actual, flags, mode);
    }
    return real_openat(dirfd, actual, flags);
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
static real_fstatat_t real_fstatat = NULL;

int __fxstatat(int ver, int dirfd, const char *pathname, void *statbuf, int flags) {
    /* glibc compat — may not be needed */
    if (!real_fstatat) real_fstatat = (real_fstatat_t)dlsym(RTLD_NEXT, "__fxstatat");
    const char *actual = remap_path(pathname);
    return real_fstatat(ver, actual, statbuf, flags);
}

int fstatat(int dirfd, const char *pathname, void *statbuf, int flags) {
    if (!real_fstatat) real_fstatat = (real_fstatat_t)dlsym(RTLD_NEXT, "fstatat");
    const char *actual = remap_path(pathname);
    return real_fstatat(dirfd, actual, statbuf, flags);
}

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
    if (fd == FAKE_FD_UART && uart_active) {
        SHIM_LOG("CLOSE UART (fd %d) after %u reads, %u writes", fd, uart_read_count, uart_write_count);
        uart_active = 0;
        return 0;
    }
    return real_close(fd);
}

/* ---- Hooked: read ---- */

ssize_t read(int fd, void *buf, size_t count) {
    resolve_all();

    if (fd == FAKE_FD_UART && uart_active) {
        uart_read_count++;
        if (uart_read_count <= 10) {
            SHIM_LOG("UART read(%zu bytes) — returning EAGAIN", count);
        }
        /* Return "no data available" — the bridge isn't sending us anything yet */
        errno = EAGAIN;
        return -1;
    }

    if (is_fake_fd(fd)) {
        /* Other fake devices shouldn't be read() directly */
        SHIM_LOG("read(fd=%d, %zu bytes) on fake device — returning 0", fd, count);
        return 0;
    }

    return real_read(fd, buf, count);
}

/* ---- Hooked: write ---- */

ssize_t write(int fd, const void *buf, size_t count) {
    resolve_all();

    if (fd == FAKE_FD_UART && uart_active) {
        uart_write_count++;
        if (uart_write_count <= 10) {
            const uint8_t *data = (const uint8_t *)buf;
            fprintf(stderr, "[shim] UART write(%zu bytes):", count);
            for (size_t i = 0; i < count && i < 32; i++)
                fprintf(stderr, " %02x", data[i]);
            if (count > 32) fprintf(stderr, " ...");
            fprintf(stderr, "\n");
        } else if ((uart_write_count % 1000) == 0) {
            SHIM_LOG("UART write #%u (%zu bytes)", uart_write_count, count);
        }
        return (ssize_t)count;  /* pretend we wrote it all */
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

    /* ---- UART ioctls (termios, modem control, etc.) ---- */
    if (fd == FAKE_FD_UART) {
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
        case 0x541e: { /* FIONREAD — bytes available to read */
            int *n = (int *)arg;
            if (n) *n = 0;  /* no data available */
            SHIM_LOG("UART ioctl FIONREAD -> 0");
            return 0;
        }
        case 0x541f: { /* TIOCOUTQ — bytes in output queue */
            int *n = (int *)arg;
            if (n) *n = 0;  /* output queue empty */
            SHIM_LOG("UART ioctl TIOCOUTQ -> 0");
            return 0;
        }
        default:
            SHIM_LOG("UART ioctl 0x%lx", request);
            return 0;
        }
    }

    return 0;
}
