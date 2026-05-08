/* shim_avforward.c — forward libavcodec packet/frame traffic to the host
 * decode_daemon.py over AF_UNIX. Engaged when SPIKE3_FORWARD_DECODE=1.
 *
 * Why: the game's V4L2-mem2mem hwaccel decoders don't work under QEMU
 * user-mode (no /dev/video* m2m device, no DRM_PRIME). Plain SW HEVC works
 * but adds CPU load on the QEMU side. The host has an RTX 4090 with NVDEC;
 * forwarding decode there moves the load off the emulator and matches the
 * "hardware-decoded" semantics of real Spike 3 hardware.
 *
 * Architecture:
 *   game (aarch64) ─► sysroot libavcodec.so.59 (hooks here)
 *                        │
 *                        ▼ AF_UNIX @spike3_decode (length-prefixed msgs)
 *                     decode_daemon.py (host x86_64) ─► hevc_cuvid → NVDEC
 *
 * Hooked entry points (all dispatched from shim_patches.c when this gate
 * is on):
 *   - avcodec_send_packet    → ship encoded packet bytes to daemon
 *   - avcodec_receive_frame  → pull decoded NV12 frame, populate AVFrame
 *
 * Session opens lazily on the first send_packet for a given AVCodecContext;
 * codec_id + extradata are extracted via avcodec_parameters_from_context
 * (stable since libavcodec 57). Closed on free_context (best-effort — the
 * underlying AF_UNIX socket also closes if the game exits).
 *
 * Not yet hooked: avcodec_open2 (we lazy-open on first packet instead);
 * avcodec_free_context (sessions reaped on socket close anyway). Adequate
 * for the MVP — extend if leak rate becomes an issue.
 *
 * Wire protocol mirror — see decode_daemon.py docstring for the canonical
 * spec. All integers little-endian.
 */

#define _GNU_SOURCE
#include "spi_shim.h"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define REQ_OPEN      0x01u
#define REQ_PACKET    0x02u
#define REQ_RECEIVE   0x03u
#define REQ_CLOSE     0x04u
#define REP_OPEN_OK   0x81u
#define REP_PACKET_OK 0x82u
#define REP_FRAME     0x83u
#define REP_OK        0x84u
#define REP_ERR       0xF0u
#define REP_EAGAIN    0xF1u
#define REP_EOF       0xF2u

/* AVCodecParameters offsets — stable since libavcodec 57. */
#define AVCP_OFF_CODEC_ID         4
#define AVCP_OFF_EXTRADATA       16
#define AVCP_OFF_EXTRADATA_SIZE  24

/* AVPacket offsets — stable. */
#define AVPKT_OFF_PTS    8
#define AVPKT_OFF_DTS   16
#define AVPKT_OFF_DATA  24
#define AVPKT_OFF_SIZE  32
#define AVPKT_OFF_FLAGS 40

/* AVFrame offsets — established by Step 45b dummy-frame code. */
#define AVFRAME_OFF_DATA0      0     /* uint8_t *data[8] */
#define AVFRAME_OFF_LINESIZE0 64     /* int linesize[8] */
#define AVFRAME_OFF_WIDTH    104
#define AVFRAME_OFF_HEIGHT   108
#define AVFRAME_OFF_FORMAT   116
#define AVFRAME_OFF_PTS      136
#define AVFRAME_OFF_PKTDTS   144
#define AV_PIX_FMT_NV12       23

/* AVERROR_EOF — FFmpeg-specific. */
#define AVERROR_EOF_VAL ((int)0xdfb9b0bbu)

/* Zero-initialized empty AVDRMFrameDescriptor used to satisfy the game's
 * post-decode descriptor consumer (see receive-frame end for full rationale).
 * Read-only after init; safe to share across all decoded frames and threads.
 * Sized 1024 for paranoia — the real struct is 528 bytes on aarch64. */
static uint8_t shim_avforward_empty_drm_descriptor[1024] = {0};

extern void *shim_resolve_libav(const char *sym, const char *version);

typedef void *(*real_avparams_alloc_t)(void);
typedef void  (*real_avparams_free_t)(void **);
typedef int   (*real_avparams_from_context_t)(void *par, const void *avctx);
typedef int   (*real_av_frame_get_buffer_t)(void *frame, int align);
typedef void  (*real_av_frame_unref_t)(void *frame);

typedef struct {
    void *avctx;
    int   sockfd;
    int   opened;
    pthread_mutex_t mu;
} forward_sess_t;

#define FORWARD_MAX_SESSIONS 8
static forward_sess_t g_sessions[FORWARD_MAX_SESSIONS];
static pthread_mutex_t g_sessions_mu = PTHREAD_MUTEX_INITIALIZER;

int shim_avforward_enabled(void) {
    static int gate = -1;
    if (gate < 0) {
        const char *e = getenv("SPIKE3_FORWARD_DECODE");
        gate = (e && e[0] == '1') ? 1 : 0;
    }
    return gate;
}

static int write_all(int fd, const void *buf, size_t n) {
    const char *p = (const char *)buf;
    while (n) {
        ssize_t r = write(fd, p, n);
        if (r <= 0) return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static int read_all(int fd, void *buf, size_t n) {
    char *p = (char *)buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r <= 0) return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static int connect_daemon(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    /* Abstract namespace: leading NUL, then name. */
    static const char NAME[] = "spike3_decode";
    addr.sun_path[0] = '\0';
    memcpy(addr.sun_path + 1, NAME, sizeof(NAME) - 1);
    socklen_t addrlen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
                                    1 + (sizeof(NAME) - 1));
    if (connect(fd, (struct sockaddr *)&addr, addrlen) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

/* Send [type][len][payload] in one or two writes. */
static int send_msg(int fd, uint32_t type, const void *payload, uint32_t len) {
    uint32_t hdr[2] = { type, len };
    if (write_all(fd, hdr, 8) != 0) return -1;
    if (len && write_all(fd, payload, len) != 0) return -1;
    return 0;
}

static void drain_bytes(int fd, uint32_t remaining) {
    char buf[4096];
    while (remaining) {
        uint32_t n = remaining > sizeof(buf) ? (uint32_t)sizeof(buf) : remaining;
        if (read_all(fd, buf, n) != 0) return;
        remaining -= n;
    }
}

static forward_sess_t *find_session_locked(void *avctx) {
    for (int i = 0; i < FORWARD_MAX_SESSIONS; i++) {
        if (g_sessions[i].avctx == avctx) return &g_sessions[i];
    }
    return NULL;
}

static forward_sess_t *get_or_alloc_session(void *avctx) {
    pthread_mutex_lock(&g_sessions_mu);
    forward_sess_t *s = find_session_locked(avctx);
    if (!s) {
        for (int i = 0; i < FORWARD_MAX_SESSIONS; i++) {
            if (g_sessions[i].avctx == NULL) {
                memset(&g_sessions[i], 0, sizeof(g_sessions[i]));
                g_sessions[i].avctx = avctx;
                g_sessions[i].sockfd = -1;
                pthread_mutex_init(&g_sessions[i].mu, NULL);
                s = &g_sessions[i];
                break;
            }
        }
    }
    pthread_mutex_unlock(&g_sessions_mu);
    return s;
}

/* Open the daemon-side session: dial AF_UNIX, send OPEN with codec_id +
 * extradata pulled out of avctx via avcodec_parameters_from_context, await
 * OPEN_OK. On any failure, leave session unopened so the caller can fall
 * back to passthrough. */
static int session_open_locked(forward_sess_t *s, void *avctx) {
    real_avparams_alloc_t        alloc =
        (real_avparams_alloc_t)shim_resolve_libav(
            "avcodec_parameters_alloc", "LIBAVCODEC_59");
    real_avparams_free_t         freep =
        (real_avparams_free_t)shim_resolve_libav(
            "avcodec_parameters_free", "LIBAVCODEC_59");
    real_avparams_from_context_t from_ctx =
        (real_avparams_from_context_t)shim_resolve_libav(
            "avcodec_parameters_from_context", "LIBAVCODEC_59");
    if (!alloc || !freep || !from_ctx) {
        SHIM_LOG("[avforward] cannot resolve avcodec_parameters_* helpers");
        return -1;
    }

    void *par = alloc();
    if (!par) {
        SHIM_LOG("[avforward] avcodec_parameters_alloc returned NULL");
        return -1;
    }
    int rc = from_ctx(par, avctx);
    if (rc < 0) {
        SHIM_LOG("[avforward] avcodec_parameters_from_context -> %d", rc);
        freep(&par);
        return -1;
    }
    int codec_id        = *(int *)((char *)par + AVCP_OFF_CODEC_ID);
    uint8_t *extradata  = *(uint8_t **)((char *)par + AVCP_OFF_EXTRADATA);
    int extradata_size  = *(int *)((char *)par + AVCP_OFF_EXTRADATA_SIZE);
    if (extradata_size < 0) extradata_size = 0;

    int fd = connect_daemon();
    if (fd < 0) {
        SHIM_LOG("[avforward] connect to @spike3_decode failed: %s",
                 strerror(errno));
        freep(&par);
        return -1;
    }

    /* OPEN payload: [u32 codec_id][u32 extradata_len][extradata bytes] */
    uint32_t open_hdr[2] = { (uint32_t)codec_id, (uint32_t)extradata_size };
    uint32_t mhdr[2] = { REQ_OPEN, (uint32_t)(8 + extradata_size) };
    if (write_all(fd, mhdr, 8) != 0 ||
        write_all(fd, open_hdr, 8) != 0 ||
        (extradata_size && write_all(fd, extradata, (size_t)extradata_size) != 0)) {
        SHIM_LOG("[avforward] OPEN write failed");
        close(fd);
        freep(&par);
        return -1;
    }
    uint32_t rhdr[2];
    if (read_all(fd, rhdr, 8) != 0) {
        SHIM_LOG("[avforward] OPEN reply read failed");
        close(fd);
        freep(&par);
        return -1;
    }
    if (rhdr[0] != REP_OPEN_OK) {
        if (rhdr[1]) drain_bytes(fd, rhdr[1]);
        SHIM_LOG("[avforward] OPEN rejected: type=0x%x", rhdr[0]);
        close(fd);
        freep(&par);
        return -1;
    }
    freep(&par);
    s->sockfd = fd;
    s->opened = 1;
    SHIM_LOG("[avforward] session OPEN ok: avctx=%p codec_id=%d extradata=%dB",
             avctx, codec_id, extradata_size);
    return 0;
}

/* Returns:
 *   1 = forward path handled it; *out_ret holds the libav return code
 *   0 = forward not engaged (caller should fall through to passthrough)
 * Errors during forwarding still return 1 with an appropriate negative
 * libav code in *out_ret — we'd rather report the error than silently
 * passthrough mid-stream and confuse the caller's state. */
int shim_avforward_send_packet(void *avctx, const void *avpkt, int *out_ret) {
    if (!shim_avforward_enabled()) return 0;

    forward_sess_t *s = get_or_alloc_session(avctx);
    if (!s) {
        SHIM_LOG("[avforward] all session slots in use; falling through");
        return 0;
    }
    pthread_mutex_lock(&s->mu);
    if (!s->opened) {
        if (session_open_locked(s, avctx) != 0) {
            pthread_mutex_unlock(&s->mu);
            return 0;
        }
    }

    /* Read AVPacket fields. avpkt may be NULL → flush packet. */
    int64_t pts = 0, dts = 0;
    uint8_t *data = NULL;
    int size = 0, flags = 0;
    if (avpkt) {
        pts   = *(int64_t *)((const char *)avpkt + AVPKT_OFF_PTS);
        dts   = *(int64_t *)((const char *)avpkt + AVPKT_OFF_DTS);
        data  = *(uint8_t **)((const char *)avpkt + AVPKT_OFF_DATA);
        size  = *(int *)((const char *)avpkt + AVPKT_OFF_SIZE);
        flags = *(int *)((const char *)avpkt + AVPKT_OFF_FLAGS);
        if (size < 0) size = 0;
    }

    /* PACKET payload: [i64 pts][i64 dts][u32 flags][u32 size][bytes] */
    uint8_t pkt_hdr[24];
    memcpy(pkt_hdr + 0, &pts, 8);
    memcpy(pkt_hdr + 8, &dts, 8);
    *(uint32_t *)(pkt_hdr + 16) = (uint32_t)flags;
    *(uint32_t *)(pkt_hdr + 20) = (uint32_t)size;
    uint32_t mhdr[2] = { REQ_PACKET, (uint32_t)(24 + size) };
    if (write_all(s->sockfd, mhdr, 8) != 0 ||
        write_all(s->sockfd, pkt_hdr, 24) != 0 ||
        (size && write_all(s->sockfd, data, (size_t)size) != 0)) {
        SHIM_LOG("[avforward] PACKET write failed");
        pthread_mutex_unlock(&s->mu);
        *out_ret = -EIO;
        return 1;
    }
    uint32_t rhdr[2];
    if (read_all(s->sockfd, rhdr, 8) != 0) {
        SHIM_LOG("[avforward] PACKET reply read failed");
        pthread_mutex_unlock(&s->mu);
        *out_ret = -EIO;
        return 1;
    }
    if (rhdr[0] != REP_PACKET_OK) {
        if (rhdr[1]) drain_bytes(s->sockfd, rhdr[1]);
        SHIM_LOG("[avforward] PACKET rejected type=0x%x", rhdr[0]);
        pthread_mutex_unlock(&s->mu);
        *out_ret = -EINVAL;
        return 1;
    }
    pthread_mutex_unlock(&s->mu);
    *out_ret = 0;
    return 1;
}

int shim_avforward_receive_frame(void *avctx, void *frame, int *out_ret) {
    if (!shim_avforward_enabled()) return 0;
    pthread_mutex_lock(&g_sessions_mu);
    forward_sess_t *s = find_session_locked(avctx);
    pthread_mutex_unlock(&g_sessions_mu);
    if (!s || !s->opened) return 0;

    pthread_mutex_lock(&s->mu);
    if (send_msg(s->sockfd, REQ_RECEIVE, NULL, 0) != 0) {
        pthread_mutex_unlock(&s->mu);
        *out_ret = -EIO;
        return 1;
    }
    uint32_t rhdr[2];
    if (read_all(s->sockfd, rhdr, 8) != 0) {
        pthread_mutex_unlock(&s->mu);
        *out_ret = -EIO;
        return 1;
    }
    if (rhdr[0] == REP_EAGAIN) {
        pthread_mutex_unlock(&s->mu);
        *out_ret = -EAGAIN;
        return 1;
    }
    if (rhdr[0] == REP_EOF) {
        pthread_mutex_unlock(&s->mu);
        *out_ret = AVERROR_EOF_VAL;
        return 1;
    }
    if (rhdr[0] != REP_FRAME) {
        if (rhdr[1]) drain_bytes(s->sockfd, rhdr[1]);
        SHIM_LOG("[avforward] RECEIVE got type=0x%x", rhdr[0]);
        pthread_mutex_unlock(&s->mu);
        *out_ret = -EINVAL;
        return 1;
    }

    uint32_t remaining = rhdr[1];
    if (remaining < 24) {
        if (remaining) drain_bytes(s->sockfd, remaining);
        pthread_mutex_unlock(&s->mu);
        *out_ret = -EINVAL;
        return 1;
    }
    uint8_t fhdr[24];
    if (read_all(s->sockfd, fhdr, 24) != 0) {
        pthread_mutex_unlock(&s->mu);
        *out_ret = -EIO;
        return 1;
    }
    remaining -= 24;
    int      w        = *(int *)(fhdr + 0);
    int      h        = *(int *)(fhdr + 4);
    int      fmt      = *(int *)(fhdr + 8);
    int64_t  fpts     = *(int64_t *)(fhdr + 12);
    uint32_t n_planes = *(uint32_t *)(fhdr + 20);

    real_av_frame_unref_t      unref =
        (real_av_frame_unref_t)shim_resolve_libav(
            "av_frame_unref", "LIBAVUTIL_57");
    real_av_frame_get_buffer_t getbuf =
        (real_av_frame_get_buffer_t)shim_resolve_libav(
            "av_frame_get_buffer", "LIBAVUTIL_57");
    if (!unref || !getbuf) {
        drain_bytes(s->sockfd, remaining);
        pthread_mutex_unlock(&s->mu);
        *out_ret = -EINVAL;
        return 1;
    }

    /* Allocate buffers in the caller's frame: unref any prior, set
     * width/height/format, ask libavutil to attach a fresh buffer. */
    unref(frame);
    *(int *)((char *)frame + AVFRAME_OFF_WIDTH)  = w;
    *(int *)((char *)frame + AVFRAME_OFF_HEIGHT) = h;
    *(int *)((char *)frame + AVFRAME_OFF_FORMAT) = fmt;
    int gb = getbuf(frame, 32);
    if (gb < 0) {
        SHIM_LOG("[avforward] av_frame_get_buffer failed: %d", gb);
        drain_bytes(s->sockfd, remaining);
        pthread_mutex_unlock(&s->mu);
        *out_ret = -ENOMEM;
        return 1;
    }
    uint8_t **fdata     = (uint8_t **)((char *)frame + AVFRAME_OFF_DATA0);
    int      *flinesize = (int *)((char *)frame + AVFRAME_OFF_LINESIZE0);

    for (uint32_t p = 0; p < n_planes; p++) {
        if (remaining < 8) goto fail;
        uint32_t pmeta[2];
        if (read_all(s->sockfd, pmeta, 8) != 0) goto fail;
        remaining -= 8;
        uint32_t src_stride = pmeta[0];
        uint32_t plane_size = pmeta[1];
        if (plane_size > remaining) goto fail;

        /* Plane heights for NV12: Y = h, UV = h/2. For other formats we
         * use the same heuristic (chroma half-height) as a safe default —
         * the daemon currently only emits NV12 so this is what matters. */
        int plane_h = (p == 0) ? h : (h / 2);
        if (p >= 8 || !fdata[p] || src_stride == 0 || plane_h <= 0) {
            drain_bytes(s->sockfd, plane_size);
            remaining -= plane_size;
            continue;
        }

        /* If the daemon's stride matches the destination linesize exactly,
         * one read straight into the frame buffer. Otherwise stream into
         * a temp and memcpy row-by-row. */
        int dst_stride = flinesize[p];
        if ((uint32_t)dst_stride == src_stride &&
            (uint32_t)dst_stride * (uint32_t)plane_h == plane_size) {
            if (read_all(s->sockfd, fdata[p], plane_size) != 0) goto fail;
            remaining -= plane_size;
        } else {
            uint8_t *tmp = (uint8_t *)malloc(plane_size);
            if (!tmp) goto fail;
            if (read_all(s->sockfd, tmp, plane_size) != 0) {
                free(tmp);
                goto fail;
            }
            remaining -= plane_size;
            int rows = (int)(plane_size / src_stride);
            if (rows > plane_h) rows = plane_h;
            for (int row = 0; row < rows; row++) {
                memcpy(fdata[p] + (size_t)row * (size_t)dst_stride,
                       tmp + (size_t)row * (size_t)src_stride,
                       (size_t)src_stride);
            }
            free(tmp);
        }
    }
    if (remaining) drain_bytes(s->sockfd, remaining);

    *(int64_t *)((char *)frame + AVFRAME_OFF_PTS)    = fpts;
    *(int64_t *)((char *)frame + AVFRAME_OFF_PKTDTS) = fpts;

    /* DRM-prime consumer compatibility shim. JOURNAL Step 45 / Step 87:
     * Step 87's NOP at game VA 0x920cf0 made get_format accept non-DRM-PRIME
     * pix_fmts (so avcodec_open2 succeeds with the SW decoder), but a
     * downstream consumer at game+0xe3e15c still walks frame->data[0] as
     * AVDRMFrameDescriptor* — reading nb_layers at offset 104, then iterating
     * planes[i].object_index*24 into objects[].fd. With our NV12 pixel bytes
     * in data[0] the consumer faults on `ldr w9, [x6, x2]` at +0xe3e250.
     *
     * Step 45 §47.5/§47.8 ruled out the proper fix (real DMA-BUF + GPU
     * import) as out-of-scope under MESA_LOADER_DRIVER_OVERRIDE=swrast.
     *
     * Compat patch: redirect frame->data[0] to a zero-initialized buffer
     * large enough for an AVDRMFrameDescriptor (528 bytes; we use 1024 for
     * paranoia). nb_layers reads as 0 → consumer's `cmp w0, #0; b.le` at
     * game+0xe3e204 branches to the early-out → no crash. Pixel data still
     * lives in frame->buf[0] / data[1] (chroma); buf[0]'s AVBufferRef owns
     * the original allocation and av_frame_unref frees it correctly even
     * though data[0] points elsewhere. */
    *(uint8_t **)((char *)frame + AVFRAME_OFF_DATA0) =
        shim_avforward_empty_drm_descriptor;

    pthread_mutex_unlock(&s->mu);
    *out_ret = 0;
    return 1;

fail:
    drain_bytes(s->sockfd, remaining);
    pthread_mutex_unlock(&s->mu);
    *out_ret = -EIO;
    return 1;
}
