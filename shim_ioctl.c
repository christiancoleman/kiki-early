/*
 * shim_ioctl.c — ioctl dispatch + DRM page-flip event synthesis.
 *
 * Phase 7 of the refactor. Owns ioctl() for every fake/real socketpair fd
 * the shim exposes, plus the drm_synthesize_flip_event helper that pushes
 * DRM_EVENT_FLIP_COMPLETE events onto the shim side of the DRM socketpair
 * whenever MODE_PAGE_FLIP fires.
 *
 * Dispatch structure:
 *   UART fd   → termios ioctls (TCGETS/TCSETS/TIOCMGET/...), FIONREAD/TIOCOUTQ,
 *               rest passthrough
 *   non-fake, non-DRM → passthrough to real libc
 *   FAKE_FD_SPI → SPI ioctl suite (WR_MODE / WR_MAX_SPEED_HZ / MESSAGE_1)
 *   DRM       → DRM_IOCTL_VERSION, MODE_GET*, CREATE_DUMB, ADDFB, PAGE_FLIP, ...
 *   V4L2 fds  → first-pass probe (log request, return ENOTTY)
 *   FAKE_FD_I2C → SET_SLAVE, RDWR, SMBUS
 *
 * Cross-TU dependencies resolved via spi_shim.h:
 *   - Device-state externs + fd predicates
 *   - real_ioctl / real_write pointers
 *   - FAKE_FD_* constants
 *   - FAKE_MODE_HDISPLAY / FAKE_MODE_VDISPLAY (DRM mode info)
 *   - shim_frame_capture_at_page_flip (called from MODE_PAGE_FLIP)
 *   - shim_resolve_game_base (called at ioctl entry to prime the cache)
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

/* ========== moved from spi_shim.c (187-205): SPI ioctl defines + spi_ioc_transfer struct ========== */
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

/* ========== moved from spi_shim.c (207-212): I2C ioctl defines ========== */
/* ---- I2C ioctl definitions ---- */

#define I2C_SLAVE       0x0703
#define I2C_SLAVE_FORCE 0x0706
#define I2C_RDWR        0x0707
#define I2C_SMBUS       0x0720

/* ========== moved from spi_shim.c (214-230): DRM fake resource IDs + drm_next_dumb_handle / drm_next_fb_id ========== */
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
/* FAKE_MODE_HDISPLAY / FAKE_MODE_VDISPLAY in spi_shim.h */

static uint32_t drm_next_dumb_handle = 1;
static uint32_t drm_next_fb_id = 100;

/* ========== moved from spi_shim.c (1298-1334): drm_synthesize_flip_event + shim_drm_event_vblank struct + drm_flip_sequence ========== */
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

/* ========== moved from spi_shim.c (1915-2318): ioctl() dispatch: UART termios, SPI, DRM, V4L2, I2C ========== */
/* ---- Hooked: ioctl ---- */

int ioctl(int fd, unsigned long request, ...) {
    resolve_all();

    /* Step 52: prime the cached game_base from a guest-thread context.
     * This hook is called constantly (SPI poll loop) so the cache gets
     * populated very early — before the sweep timer thread needs it.
     * shim_resolve_game_base() itself early-returns if already cached. */
    (void)shim_resolve_game_base();

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

    /* ---- Pass through non-fake, non-DRM fds ---- */
    if (!is_fake_fd(fd) && !is_drm_fd(fd)) {
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
            /* Step 58.2: capture frame to the frame bridge at the present
             * boundary. Runs in the game's thread, so the GL context the
             * game made current is live — we can glReadPixels directly. */
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

    /* ---- V4L2 media controller + video device ioctls (Step 46 probe) ----
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

