/*
 * shim_ioctl.c — ioctl dispatch + DRM page-flip event synthesis.
 *
 * Dispatch structure:
 *   UART fd   → termios ioctls (TCGETS/TCSETS/TIOCMGET/...), FIONREAD/TIOCOUTQ,
 *               rest passthrough
 *   non-fake, non-DRM → passthrough to real libc
 *   FAKE_FD_SPI → SPI ioctl suite (WR_MODE / WR_MAX_SPEED_HZ / MESSAGE_1)
 *   DRM       → DRM_IOCTL_VERSION, MODE_GET*, CREATE_DUMB, ADDFB, PAGE_FLIP, ...
 *   V4L2 fds  → first-pass probe (log request, return ENOTTY)
 *   FAKE_FD_I2C → SET_SLAVE, RDWR, SMBUS
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>
#include <dlfcn.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>

#include "spi_shim.h"

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

#define I2C_SLAVE       0x0703
#define I2C_SLAVE_FORCE 0x0706
#define I2C_RDWR        0x0707
#define I2C_SMBUS       0x0720

#define I2C_M_RD        0x0001

/* Match kernel uapi struct layout — natural alignment, 2 bytes pad after len
 * to align buf on 8 bytes; total 24 bytes per msg on aarch64. */
struct shim_i2c_msg {
    uint16_t addr;
    uint16_t flags;
    uint16_t len;
    uint8_t  *buf;
};

struct shim_i2c_rdwr_ioctl_data {
    struct shim_i2c_msg *msgs;
    uint32_t             nmsgs;
};

#define I2C_SMBUS_READ              1
#define I2C_SMBUS_WRITE             0
#define I2C_SMBUS_QUICK             0
#define I2C_SMBUS_BYTE              1
#define I2C_SMBUS_BYTE_DATA         2
#define I2C_SMBUS_WORD_DATA         3
#define I2C_SMBUS_BLOCK_DATA        5
#define I2C_SMBUS_I2C_BLOCK_BROKEN  6
#define I2C_SMBUS_I2C_BLOCK_DATA    8
#define I2C_SMBUS_BLOCK_MAX         32

union shim_i2c_smbus_data {
    uint8_t  byte;
    uint16_t word;
    uint8_t  block[I2C_SMBUS_BLOCK_MAX + 2];
};

struct shim_i2c_smbus_ioctl_data {
    uint8_t                     read_write;
    uint8_t                     command;
    uint32_t                    size;
    union shim_i2c_smbus_data  *data;
};

/* Per-chip post-write transforms. Some registers are status registers whose
 * read value differs from what was written: a write triggers a hardware
 * sequence and the chip then reports the result via a status bit. The plain
 * regfile would return the original written value forever, so the driver's
 * polling loop times out. Each known (slave, register) gets a small case
 * here that mutates regfile[slave][reg] right after the write.
 *
 * Add cases as new "polling never succeeds" symptoms appear. */
static void shim_i2c_chip_post_write(uint8_t slave, uint8_t reg,
                                     uint8_t /*unused*/ written) {
    (void)written;
    /* TAS5711 / TAS5755M digital amp at slave 0x1b.
     *
     * Register 0x1b is the OSC_TRIM register. Per TI SLAS502/SLAS589, writing
     * any value triggers an oscillator self-calibration sequence that takes
     * ~50–500 ms; the chip reports completion by setting bit 6
     * (OSC_TRIM_DONE) read-back. The driver sleeps 50 ms then polls bit 6
     * up to 10× at 10 ms intervals; without the bit set it logs
     * "oscillator trim failed to lock" and falls through.
     *
     * On real hardware the bit is not instantaneous but for emulation we
     * report "done" on the very next read — the driver's first poll after
     * its initial usleep(50000) sees it, and forward progress is identical. */
    if (slave == 0x1b && reg == 0x1b) {
        i2c_regfile[slave][reg] = 0x40;
    }
}

void shim_i2c_handle_write(uint8_t slave, const uint8_t *buf, size_t len) {
    slave &= 0x7f;
    if (len == 0 || !buf) return;
    uint8_t reg = buf[0];
    i2c_reg_pointer[slave] = reg;
    /* buf[1..len-1] are payload bytes written at reg, reg+1, ... with the
     * pointer advancing. Clamp at 256 to avoid wrapping unintentionally. */
    for (size_t i = 1; i < len; i++) {
        if (reg + (i - 1) >= 256) break;
        uint8_t target_reg = (uint8_t)(reg + i - 1);
        i2c_regfile[slave][target_reg] = buf[i];
        shim_i2c_chip_post_write(slave, target_reg, buf[i]);
    }
    if (len > 1) {
        size_t advance = len - 1;
        if ((size_t)reg + advance > 0xff) advance = 0xff - reg;
        i2c_reg_pointer[slave] = (uint8_t)(reg + advance);
    }
}

size_t shim_i2c_handle_read(uint8_t slave, uint8_t *buf, size_t len) {
    slave &= 0x7f;
    if (len == 0 || !buf) return 0;
    uint8_t reg = i2c_reg_pointer[slave];
    size_t n = len;
    if ((size_t)reg + n > 256) n = 256 - reg;
    for (size_t i = 0; i < n; i++) buf[i] = i2c_regfile[slave][reg + i];
    if ((size_t)reg + n <= 0xff)
        i2c_reg_pointer[slave] = (uint8_t)(reg + n);
    else
        i2c_reg_pointer[slave] = 0xff;
    return n;
}

/* DRM fake resource IDs: 1 CRTC, 1 encoder, 2 connectors. First connector
 * is connected with one mode; second is disconnected (satisfies the >=2
 * connector check before falling back to card0). FAKE_MODE_HDISPLAY /
 * FAKE_MODE_VDISPLAY are in spi_shim.h. */

#define FAKE_CRTC_ID       1
#define FAKE_ENCODER_ID    20
#define FAKE_CONNECTOR_A   10
#define FAKE_CONNECTOR_B   11

static uint32_t drm_next_dumb_handle = 1;
static uint32_t drm_next_fb_id = 100;

volatile uint8_t shim_spi_rx_extra[8] = {0};

static int shim_spi_fill_channel0_idle_rx(uint8_t *rx, uint32_t len) {
    uintptr_t game_base = shim_resolve_game_base();
    if (!game_base || !rx || len == 0) return 0;

    uint8_t idle[8];
    memcpy(idle, (const void *)(uintptr_t)(game_base + 0x44804d0ULL), 8);
    int nonzero = 0;
    for (int i = 0; i < 8; i++) nonzero |= idle[i];
    if (!nonzero) return 0;

    /* sub_8fa820 stores DAT_0450d9b0 byte N from SPI rx[N-1].
     * DAT byte 0 is a separate zero-initialized stack byte, so rx[7] is
     * ignored for channel-0 switch translation. */
    for (uint32_t dat_i = 1; dat_i < 8 && dat_i - 1 < len; dat_i++)
        rx[dat_i - 1] = idle[dat_i];

    static uint8_t last_logged[8];
    static int logged = 0;
    if (!logged || memcmp(last_logged, idle, sizeof(idle)) != 0) {
        memcpy(last_logged, idle, sizeof(idle));
        logged = 1;
        SHIM_LOG("[spi] channel0 idle RX source DAT=%02x %02x %02x %02x %02x %02x %02x %02x -> rx=%02x %02x %02x %02x %02x %02x %02x %02x",
                 idle[0], idle[1], idle[2], idle[3],
                 idle[4], idle[5], idle[6], idle[7],
                 len > 0 ? rx[0] : 0, len > 1 ? rx[1] : 0,
                 len > 2 ? rx[2] : 0, len > 3 ? rx[3] : 0,
                 len > 4 ? rx[4] : 0, len > 5 ? rx[5] : 0,
                 len > 6 ? rx[6] : 0, len > 7 ? rx[7] : 0);
    }
    return 1;
}

/* Synthesize a page-flip completion event on the shim side of the DRM
 * socketpair. Format matches `struct drm_event_vblank` from <drm/drm.h>
 * (32 bytes; libdrm reads the length field to know how much to consume,
 * so forward-compat across libdrm versions). DRM_EVENT_FLIP_COMPLETE = 2. */
struct shim_drm_event_vblank {
    uint32_t type;
    uint32_t length;
    uint64_t user_data;
    uint32_t tv_sec;
    uint32_t tv_usec;
    uint32_t sequence;
    uint32_t crtc_id;
};

static uint32_t drm_flip_sequence = 0;

static void drm_synthesize_flip_event(uint32_t crtc_id, uint64_t user_data) {
    if (drm_shim_fd < 0) return;
    struct shim_drm_event_vblank ev;
    ev.type = 2;                    /* DRM_EVENT_FLIP_COMPLETE */
    ev.length = sizeof(ev);
    ev.user_data = user_data;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ev.tv_sec = (uint32_t)ts.tv_sec;
    ev.tv_usec = (uint32_t)(ts.tv_nsec / 1000);
    ev.sequence = __atomic_fetch_add(&drm_flip_sequence, 1, __ATOMIC_SEQ_CST);
    ev.crtc_id = crtc_id;
    /* ioctl() calls resolve_all() first, so real_write is set by the time
     * this helper runs. Fall back to libc write() just in case. */
    ssize_t n = real_write ? real_write(drm_shim_fd, &ev, sizeof(ev))
                           : write(drm_shim_fd, &ev, sizeof(ev));
    if (n != (ssize_t)sizeof(ev)) {
        SHIM_LOG("DRM flip-event write short: n=%zd err=%s",
                 n, strerror(errno));
    }
}

int ioctl(int fd, unsigned long request, ...) {
    resolve_all();
    (void)shim_resolve_game_base();

    va_list args;
    va_start(args, request);
    void *arg = va_arg(args, void *);
    va_end(args);

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

    if (!is_fake_fd(fd) && !is_drm_fd(fd)) {
        return real_ioctl(fd, request, arg);
    }

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
            /* Per-command RX response table + env-var overrides (2026-04-24).
             *
             * The game's nodebus SPI loop sends 8-byte packets whose last byte
             * is a command/node-address code (empirically 0x00, 0x2c, 0x40,
             * 0x6c dominate in captured runs). Historically the shim replied
             * identically to every command (rx[2]=0x04). That's almost
             * certainly why the game's state machine never advances — it
             * expects different RX bytes per command.
             *
             * New behavior:
             *   • Lazy-init a per-byte override table from env vars
             *     SPIKE3_SPI_RX_XX=<hex-bytes> where XX is 00..FF.
             *     Example: SPIKE3_SPI_RX_6C=0000110000000000 to try rx[2]=0x11
             *              for the 0x6c query.
             *     Separators in the value (colons/dashes/spaces) are stripped.
             *   • Per-command counter so summary logs show the distribution.
             *   • Log line now shows BOTH TX and RX plus the command tag so
             *     reverse-engineering the protocol is observable live.
             *   • Defaults preserved: no override → rx[2]=0x04 (old behavior).
             *   • Banner prints once per command byte the first time we see
             *     it, independent of the sampled-log cadence. */
            static uint8_t  spi_rx_override[256][16];
            static uint8_t  spi_rx_override_len[256];  /* 0 = no override */
            static uint32_t spi_cmd_count[256];
            static int      spi_override_loaded = 0;

            if (!spi_override_loaded) {
                spi_override_loaded = 1;
                for (int c = 0; c < 256; c++) {
                    char name[32];
                    snprintf(name, sizeof(name), "SPIKE3_SPI_RX_%02X", c);
                    const char *v = getenv(name);
                    if (!v || !v[0]) continue;
                    int nb = 0;
                    const char *p = v;
                    while (*p && nb < 16) {
                        while (*p && !((*p >= '0' && *p <= '9') ||
                                       (*p >= 'a' && *p <= 'f') ||
                                       (*p >= 'A' && *p <= 'F'))) p++;
                        if (!*p) break;
                        unsigned b = 0;
                        if (sscanf(p, "%2x", &b) != 1) break;
                        spi_rx_override[c][nb++] = (uint8_t)b;
                        p += 2;
                    }
                    if (nb > 0) {
                        spi_rx_override_len[c] = (uint8_t)nb;
                        SHIM_LOG("[spi] override for cmd 0x%02x loaded: %d bytes", c, nb);
                    }
                }
            }

            struct spi_ioc_transfer *xfer = (struct spi_ioc_transfer *)arg;
            uint8_t *tx = (uint8_t *)(uintptr_t)xfer->tx_buf;
            uint8_t *rx = (uint8_t *)(uintptr_t)xfer->rx_buf;
            uint32_t len = xfer->len;

            spi_transfer_count++;
            uint8_t cmd = (tx && len > 0) ? tx[len - 1] : 0;
            uint32_t cmd_seen = ++spi_cmd_count[cmd];

            {
                static int delay_us_cached = -1;
                if (__builtin_expect(delay_us_cached < 0, 0)) {
                    const char *d = getenv("SPIKE3_SPI_DELAY_US");
                    delay_us_cached = (d && d[0]) ? atoi(d) : 0;
                    if (delay_us_cached < 0) delay_us_cached = 0;
                    if (delay_us_cached > 100000) delay_us_cached = 100000;
                }
                if (delay_us_cached > 0) usleep((useconds_t)delay_us_cached);
            }

            /* Craft RX response BEFORE logging so the log reflects what we
             * actually returned. */
            if (rx && len > 0) {
                memset(rx, 0, len);
                if (spi_rx_override_len[cmd] > 0) {
                    uint32_t n = spi_rx_override_len[cmd];
                    if (n > len) n = len;
                    memcpy(rx, spi_rx_override[cmd], n);
                } else if (len > 2) {
                    static int quiet_cached = -1;
                    if (__builtin_expect(quiet_cached < 0, 0)) {
                        const char *q = getenv("SPIKE3_SPI_QUIET");
                        quiet_cached = (q && q[0] == '1') ? 1 : 0;
                    }
                    if (!quiet_cached && !shim_spi_fill_channel0_idle_rx(rx, len))
                        rx[2] = 0x04;
                }
                for (uint32_t i = 0; i < len && i < 8; i++)
                    rx[i] |= shim_spi_rx_extra[i];
            }

            /* Log cadence: first 20 transactions overall (boot visibility) +
             * every 1000th + FIRST sighting of each distinct command byte
             * (so we never silently miss a new nodebus query in long runs). */
            int is_first_of_cmd = (cmd_seen == 1);
            int is_sample = (spi_transfer_count <= 20) ||
                            (spi_transfer_count % 1000 == 0);
            if (is_first_of_cmd || is_sample) {
                /* Build the whole line in one buffer + single write() so
                 * concurrent threads (e.g. I2C ioctls firing from another
                 * thread) can't splice into the middle of an SPI log. */
                char line[256];
                int n = snprintf(line, sizeof(line),
                                 "[shim] SPI #%u cmd=0x%02x%s  TX:",
                                 spi_transfer_count, cmd,
                                 spi_rx_override_len[cmd] > 0 ? " (override)" : "");
                for (uint32_t i = 0; i < len && i < 16 && n < (int)sizeof(line); i++)
                    n += snprintf(line + n, sizeof(line) - n, " %02x", tx[i]);
                if (n < (int)sizeof(line))
                    n += snprintf(line + n, sizeof(line) - n, "  RX:");
                for (uint32_t i = 0; i < len && i < 16 && n < (int)sizeof(line); i++)
                    n += snprintf(line + n, sizeof(line) - n, " %02x", rx ? rx[i] : 0);
                if (is_first_of_cmd && n < (int)sizeof(line))
                    n += snprintf(line + n, sizeof(line) - n, "  [FIRST]");
                if (n < (int)sizeof(line))
                    n += snprintf(line + n, sizeof(line) - n, "\n");
                if (n > (int)sizeof(line)) n = (int)sizeof(line);
                (void)write(STDERR_FILENO, line, (size_t)n);
            }
            return 0;
        }
        default:
            SHIM_LOG("SPI ioctl unknown 0x%lx", request);
            return 0;
        }
    }

    if (is_drm_fd(fd)) {
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
            /* If the caller requested an event (DRM_MODE_PAGE_FLIP_EVENT = 1),
             * synthesize a drm_event_vblank on the shim side of the socketpair.
             * The game's select() on drm_fd wakes, drmHandleEvent reads the
             * event, and the page-flip callback fires — driving the next
             * render-loop iteration (see _emulation/JOURNAL.md Step 23.3). */
            if (p->flags & 1 /* DRM_MODE_PAGE_FLIP_EVENT */) {
                drm_synthesize_flip_event(p->crtc_id, p->user_data);
            }
            static uint32_t flip_count = 0;
            flip_count++;
            if (flip_count <= 4 || (flip_count % 60) == 0) {
                SHIM_LOG("DRM ioctl MODE_PAGE_FLIP #%u crtc=%u fb=%u flags=0x%x",
                         flip_count, p->crtc_id, p->fb_id, p->flags);
            }
            shim_frame_capture_at_page_flip();
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

    /* ---- V4L2 media controller + video device ioctls (Step 44 probe) ----
     *
     * First-pass probe: log every ioctl request number with arg pointer,
     * return -1 with errno=ENOTTY. This tells libavcodec "this device does
     * not support this ioctl" (the canonical response for an unsupported
     * command) and should produce a deterministic error trail we can follow.
     *
     * ioctl request numbers follow the standard _IOR/_IOWR encoding:
     *   bits [0..7]   = command nr
     *   bits [8..15]  = magic letter ('V' = 0x56 for V4L2, '|' = 0x7c for MC)
     *   bits [16..29] = arg size in bytes
     *   bits [30..31] = direction (_IOC_READ=2, _IOC_WRITE=1)
     * We print all four fields so we can decode without a header. */
    if (is_v4l2_fd(fd)) {
        v4l2_ioctl_count++;
        unsigned long type = (request >> 8) & 0xff;
        unsigned long nr   = request & 0xff;
        unsigned long size = (request >> 16) & 0x3fff;
        unsigned long dir  = (request >> 30) & 0x3;
        const char *dev = (fd == FAKE_FD_V4L2_MEDIA) ? "media" : "video";
        SHIM_LOG("V4L2 %s ioctl #%u: req=0x%lx dir=%lu type='%c' nr=0x%02lx size=%lu arg=%p  -> ENOTTY",
                 dev, v4l2_ioctl_count, request, dir, (char)type, nr, size, arg);
        errno = 25;  /* ENOTTY = inappropriate ioctl for device */
        return -1;
    }

    /* ---- boot_display ioctls ---- */
    /* Inert stub: log every ioctl with full request decode, return 0 (success).
     * Unlike the V4L2 probe which deliberately returns ENOTTY, boot_display
     * may be issuing real config ioctls it expects to succeed; returning 0
     * is the safer first cut. If the log shows DRM-style mode-set ioctls
     * (large request numbers in the 0x40xx_xxxx range with struct-shaped
     * sizes) it suggests /dev/boot_display IS a render surface and we
     * should escalate the stub to track surface state. */
    if (is_boot_display_fd(fd)) {
        static uint32_t _bd_ioctl_count = 0;
        unsigned long type = (request >> 8) & 0xff;
        unsigned long nr   = request & 0xff;
        unsigned long size = (request >> 16) & 0x3fff;
        unsigned long dir  = (request >> 30) & 0x3;
        _bd_ioctl_count++;
        SHIM_LOG("boot_display ioctl #%u: req=0x%lx dir=%lu type='%c' nr=0x%02lx size=%lu arg=%p  -> 0 (fake OK)",
                 _bd_ioctl_count, request, dir, (char)type, nr, size, arg);
        return 0;
    }

    /* ---- I2C ioctls ----
     * Modeled, not just accepted: the audio driver does write-then-readback
     * verify on every register write, so the read side has to return what
     * was just written. RDWR returns nmsgs (the kernel ABI), not 0 — the
     * driver's verify path treats <=0 as failure. */
    if (fd == FAKE_FD_I2C) {
        i2c_ioctl_count++;
        switch (request) {
        case I2C_SLAVE:
        case I2C_SLAVE_FORCE: {
            unsigned long addr = (unsigned long)(uintptr_t)arg;
            i2c_current_slave = (uint8_t)(addr & 0x7f);
            SHIM_LOG("I2C ioctl SET_SLAVE addr=0x%02lx", addr);
            return 0;
        }
        case I2C_RDWR: {
            struct shim_i2c_rdwr_ioctl_data *r =
                (struct shim_i2c_rdwr_ioctl_data *)arg;
            if (!r || (!r->msgs && r->nmsgs > 0)) {
                errno = EFAULT;
                return -1;
            }
            for (uint32_t i = 0; i < r->nmsgs; i++) {
                struct shim_i2c_msg *m = &r->msgs[i];
                uint8_t slave = (uint8_t)(m->addr & 0x7f);
                if (m->flags & I2C_M_RD) {
                    shim_i2c_handle_read(slave, m->buf, m->len);
                } else {
                    shim_i2c_handle_write(slave, m->buf, m->len);
                }
            }
            if (i2c_ioctl_count <= 10) {
                SHIM_LOG("I2C ioctl RDWR (transfer #%u, %u msgs)",
                         i2c_ioctl_count, r->nmsgs);
            }
            return (int)r->nmsgs;
        }
        case I2C_SMBUS: {
            struct shim_i2c_smbus_ioctl_data *s =
                (struct shim_i2c_smbus_ioctl_data *)arg;
            if (!s) { errno = EFAULT; return -1; }
            uint8_t slave = i2c_current_slave;
            int is_read = (s->read_write == I2C_SMBUS_READ);
            switch (s->size) {
            case I2C_SMBUS_QUICK:
                break;
            case I2C_SMBUS_BYTE:
                if (!s->data) break;
                if (is_read) {
                    shim_i2c_handle_read(slave, &s->data->byte, 1);
                } else {
                    /* For SMBUS_BYTE write, the byte is in command. */
                    uint8_t b = s->command;
                    shim_i2c_handle_write(slave, &b, 1);
                }
                break;
            case I2C_SMBUS_BYTE_DATA:
                if (!s->data) break;
                if (is_read) {
                    uint8_t reg = s->command;
                    shim_i2c_handle_write(slave, &reg, 1);
                    shim_i2c_handle_read(slave, &s->data->byte, 1);
                } else {
                    uint8_t buf[2] = { s->command, s->data->byte };
                    shim_i2c_handle_write(slave, buf, 2);
                }
                break;
            case I2C_SMBUS_WORD_DATA:
                if (!s->data) break;
                if (is_read) {
                    uint8_t reg = s->command;
                    uint8_t tmp[2] = {0, 0};
                    shim_i2c_handle_write(slave, &reg, 1);
                    shim_i2c_handle_read(slave, tmp, 2);
                    s->data->word = (uint16_t)tmp[0] |
                                    ((uint16_t)tmp[1] << 8);
                } else {
                    uint8_t buf[3] = {
                        s->command,
                        (uint8_t)(s->data->word & 0xff),
                        (uint8_t)((s->data->word >> 8) & 0xff)
                    };
                    shim_i2c_handle_write(slave, buf, 3);
                }
                break;
            case I2C_SMBUS_BLOCK_DATA:
            case I2C_SMBUS_I2C_BLOCK_BROKEN:
            case I2C_SMBUS_I2C_BLOCK_DATA:
                if (!s->data) break;
                if (is_read) {
                    uint8_t reg = s->command;
                    uint8_t blen = s->data->block[0];
                    if (blen > I2C_SMBUS_BLOCK_MAX) blen = I2C_SMBUS_BLOCK_MAX;
                    shim_i2c_handle_write(slave, &reg, 1);
                    shim_i2c_handle_read(slave, &s->data->block[1], blen);
                } else {
                    uint8_t blen = s->data->block[0];
                    if (blen > I2C_SMBUS_BLOCK_MAX) blen = I2C_SMBUS_BLOCK_MAX;
                    uint8_t tmp[1 + I2C_SMBUS_BLOCK_MAX];
                    tmp[0] = s->command;
                    for (uint8_t i = 0; i < blen; i++)
                        tmp[1 + i] = s->data->block[1 + i];
                    shim_i2c_handle_write(slave, tmp, 1 + blen);
                }
                break;
            default:
                break;
            }
            if (i2c_ioctl_count <= 10) {
                SHIM_LOG("I2C ioctl SMBUS (transfer #%u, %s size=%u cmd=0x%02x)",
                         i2c_ioctl_count, is_read ? "R" : "W",
                         s->size, s->command);
            }
            return 0;
        }
        default:
            SHIM_LOG("I2C ioctl 0x%lx (#%u)", request, i2c_ioctl_count);
            return 0;
        }
    }

    return 0;
}
