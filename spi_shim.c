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

/* ---- Device state ---- */

static int spi_active = 0;
static int i2c_active = 0;
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
           (fd == FAKE_FD_I2C && i2c_active);
}

static int is_uart_fd(int fd) {
    return uart_game_fd >= 0 && fd == uart_game_fd;
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

static void bridge_send_response(const uint8_t *resp, size_t resp_len,
                                 uint8_t req_cmd, uint8_t id) {
    if (uart_shim_fd < 0) return;
    fprintf(stderr, "[shim] bridge RESP to cmd=0x%02x id=0x%02x (%zu bytes):",
            req_cmd, id, resp_len);
    for (size_t i = 0; i < resp_len; i++) fprintf(stderr, " %02x", resp[i]);
    fprintf(stderr, "\n");
    ssize_t n = real_write(uart_shim_fd, resp, resp_len);
    if (n < 0) SHIM_LOG("bridge: real_write response failed: %s", strerror(errno));
}

static void synthesize_bridge_response(const uint8_t *req, size_t req_len) {
    if (req_len < 5 || req[0] != 0x02 || req[req_len - 1] != 0x03) return;
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
    default:
        SHIM_LOG("bridge: no synthetic response for cmd 0x%02x", cmd);
        return;
    }
}

ssize_t write(int fd, const void *buf, size_t count) {
    resolve_all();

    if (is_uart_fd(fd)) {
        uart_write_count++;
        if (uart_write_count <= 10 || (uart_write_count % 1000) == 0) {
            const uint8_t *data = (const uint8_t *)buf;
            fprintf(stderr, "[shim] UART write #%u (%zu bytes):", uart_write_count, count);
            for (size_t i = 0; i < count && i < 32; i++)
                fprintf(stderr, " %02x", data[i]);
            if (count > 32) fprintf(stderr, " ...");
            fprintf(stderr, "\n");
        }
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
