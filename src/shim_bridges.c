/*
 * shim_bridges.c — state bridge (JSON cmode deltas) + frame bridge (RGBA
 * page-flip capture). Phase 2 of the refactor: carved out of spi_shim.c.
 *
 * Both bridges are AF_UNIX SOCK_STREAM listeners, off by default, toggled
 * via SPIKE3_BRIDGE=1 / SPIKE3_FRAME_BRIDGE=1. Each owns its own listener
 * socket, client fd, mutex, and worker thread(s).
 *
 * Dependencies resolved via spi_shim.h:
 *   - struct shim_cmode_entry + shim_cmode_table[] + shim_cmode_table_count
 *     (defined in spi_shim.c; used by the state bridge's poll thread)
 *   - shim_resolve_game_base() (defined in spi_shim.c; used by poll thread)
 *   - FAKE_MODE_HDISPLAY / FAKE_MODE_VDISPLAY (used by frame capture)
 *   - SHIM_LOG
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <sys/time.h>

#include "spi_shim.h"
#include "gl_proto.h"

/* Common listener creator for the three bridges. Creates an abstract
 * AF_UNIX listener at `abstract_name`. `tag` is used only in log messages.
 * Returns the listening fd, or -1 on error. */
static int shim_bridge_create_listener(const char *abstract_name,
                                       const char *tag) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        SHIM_LOG("[%s] socket(AF_UNIX) failed: %s", tag, strerror(errno));
        return -1;
    }
    struct sockaddr_un addr;
    socklen_t addrlen = shim_unix_abstract_addr(&addr, abstract_name);
    if (bind(fd, (struct sockaddr*)&addr, addrlen) < 0 ||
        listen(fd, 1) < 0) {
        SHIM_LOG("[%s] bind/listen(@%s) failed: %s",
                 tag, abstract_name, strerror(errno));
        close(fd);
        return -1;
    }
    SHIM_LOG("[%s] listening on @%s (abstract AF_UNIX)", tag, abstract_name);
    return fd;
}

/* shim_last_draw_fbo is set by the glBindFramebuffer hook in spi_shim.c; the
 * capture function reads it atomically. Keep as extern to avoid dragging
 * the GL state into this TU. */
extern volatile unsigned shim_last_draw_fbo;

/* shim_unix_abstract_addr is defined as static inline in spi_shim.h —
 * the canonical AF_UNIX addressing strategy: abstract namespace, no fs
 * touch. WSL/9P can't bind AF_UNIX on /mnt/c, so this is the only way
 * to keep both endpoints inside the project (helper Python uses the
 * same abstract name). */
#define ABSTRACT_BRIDGE_NAME  "pokemon_spike3_bridge"
#define ABSTRACT_FRAME_NAME   "pokemon_spike3_frame"
#define ABSTRACT_AUDIO_NAME   "pokemon_spike3_audio"

/* State bridge: subscriber connects to abstract AF_UNIX SOCK_STREAM; shim
 * emits newline-delimited JSON events when cmode state changes.
 *
 * Emitted event format (one line, no pretty-printing):
 *   {"t":"cmode","id":21,"name":"story_charmander_1","flags":40,
 *    "init":0,"init_prev":1792,"ts_ms":12345}
 *
 * Where:
 *   t         — event type ("cmode" for cmode state delta)
 *   id        — mode_id (uint32)
 *   name      — human-readable name from shim_cmode_table
 *   flags     — type_flags (0x28 = Pokemon story, 0x48 = battle, etc.)
 *   init      — current value of field at +0x308 (post-change)
 *   init_prev — previous value (pre-change)
 *   ts_ms     — monotonic ms since shim constructor ran
 *
 *   - Non-blocking subscriber socket — if no one's listening, we just log
 *     less; shim doesn't stall the game.
 *   - Listener socket lives for process lifetime; multiple connects OK
 *     but we only accept ONE subscriber at a time for simplicity.
 *   - Polls cmode state at 10 Hz. Enough for interactive feel, doesn't
 *     burden the guest.
 *   - Enabled when SPIKE3_BRIDGE=1 to keep bridge off by default.
 */

#include <sys/un.h>
#include <poll.h>
#include <sys/time.h>

static int shim_bridge_listen_fd = -1;
static int shim_bridge_client_fd = -1;
static pthread_mutex_t shim_bridge_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t shim_bridge_start_ms = 0;

static uint64_t shim_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void shim_bridge_emit(const char *json_line) {
    if (!json_line) return;
    size_t len = strlen(json_line);
    pthread_mutex_lock(&shim_bridge_lock);
    if (shim_bridge_client_fd >= 0) {
        ssize_t w = send(shim_bridge_client_fd, json_line, len, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (w < 0 && (errno == EPIPE || errno == ECONNRESET || errno == EBADF)) {
            close(shim_bridge_client_fd);
            shim_bridge_client_fd = -1;
            SHIM_LOG("[bridge] subscriber disconnected");
        }
    }
    pthread_mutex_unlock(&shim_bridge_lock);
}

/* Accept loop — runs in its own thread. Blocks on accept(); whenever a
 * subscriber connects we stash the fd. If a previous subscriber is still
 * connected we disconnect them first (one consumer at a time). */
static void *shim_bridge_accept_thread(void *arg) {
    (void)arg;
    for (;;) {
        struct sockaddr_storage addr;
        socklen_t addrlen = sizeof(addr);
        int fd = accept(shim_bridge_listen_fd, (struct sockaddr*)&addr, &addrlen);
        if (fd < 0) {
            if (errno == EINTR) continue;
            SHIM_LOG("[bridge] accept() failed: %s — listener terminating", strerror(errno));
            return NULL;
        }

        pthread_mutex_lock(&shim_bridge_lock);
        if (shim_bridge_client_fd >= 0) {
            close(shim_bridge_client_fd);
            SHIM_LOG("[bridge] evicted prior subscriber (new one connected)");
        }
        shim_bridge_client_fd = fd;
        pthread_mutex_unlock(&shim_bridge_lock);

        SHIM_LOG("[bridge] subscriber connected on fd %d", fd);

        char hello[256];
        int hn = snprintf(hello, sizeof(hello),
            "{\"t\":\"hello\",\"shim\":\"pokemon_le\",\"cmodes\":42,\"ts_ms\":%lu}\n",
            (unsigned long)(shim_now_ms() - shim_bridge_start_ms));
        (void)send(fd, hello, (size_t)hn, MSG_NOSIGNAL);
    }
    return NULL;
}

static void *shim_bridge_poll_thread(void *arg) {
    (void)arg;

    /* Wait for game_base to resolve (populated by ioctl hook or similar). */
    uintptr_t game_base = 0;
    for (int attempt = 0; attempt < 60; attempt++) {
        game_base = shim_resolve_game_base();
        if (game_base) break;
        sleep(1);
    }
    if (!game_base) {
        SHIM_LOG("[bridge] poll thread: game_base never resolved; giving up");
        return NULL;
    }

    size_t N = shim_cmode_table_count;
    uint64_t last[42];
    for (size_t i = 0; i < N; i++) {
        uintptr_t rt = game_base + shim_cmode_table[i].va;
        memcpy(&last[i], (void*)(rt + 0x308), 8);
    }

    SHIM_LOG("[bridge] poll thread live (game_base=0x%lx, polling %zu cmodes @ 10 Hz)",
             game_base, N);

    for (;;) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 100 * 1000 * 1000 };
        nanosleep(&ts, NULL);

        for (size_t i = 0; i < N; i++) {
            uintptr_t rt = game_base + shim_cmode_table[i].va;
            uint64_t now_val;
            memcpy(&now_val, (void*)(rt + 0x308), 8);
            if (now_val != last[i]) {
                char line[320];
                snprintf(line, sizeof(line),
                    "{\"t\":\"cmode\",\"id\":%u,\"name\":\"%s\","
                    "\"flags\":%u,\"init\":%lu,\"init_prev\":%lu,"
                    "\"ts_ms\":%lu}\n",
                    shim_cmode_table[i].expected_mode_id,
                    shim_cmode_table[i].name_hint,
                    shim_cmode_table[i].expected_type_flags,
                    (unsigned long)now_val,
                    (unsigned long)last[i],
                    (unsigned long)(shim_now_ms() - shim_bridge_start_ms));
                shim_bridge_emit(line);
                SHIM_LOG("[bridge] cmode delta: %s init %lx -> %lx",
                         shim_cmode_table[i].name_hint,
                         (unsigned long)last[i], (unsigned long)now_val);
                last[i] = now_val;
            }
        }
    }
    return NULL;
}

/* Start the bridge: create listener socket, spawn accept + poll threads.
 * Called from the constructor; no-op unless SPIKE3_BRIDGE=1. */
void shim_bridge_start(void) {
    const char *enabled = getenv("SPIKE3_BRIDGE");
    if (!enabled || enabled[0] != '1') return;

    shim_bridge_listen_fd = shim_bridge_create_listener(
        ABSTRACT_BRIDGE_NAME, "bridge");
    if (shim_bridge_listen_fd < 0) return;

    shim_bridge_start_ms = shim_now_ms();

    pthread_t t_accept, t_poll;
    if (pthread_create(&t_accept, NULL, shim_bridge_accept_thread, NULL) == 0) {
        pthread_detach(t_accept);
    }
    if (pthread_create(&t_poll, NULL, shim_bridge_poll_thread, NULL) == 0) {
        pthread_detach(t_poll);
    }

    SHIM_LOG("[bridge] threads ready");
}

/* Frame bridge: glReadPixels → AF_UNIX stream → frame viewer.
 *
 * Wire protocol (little-endian, 16-byte header + W*H*4 payload):
 *     u32 magic       — 0xF00FBABA
 *     u32 seq         — monotonic sequence number
 *     u16 width       — pixel width (typically FAKE_MODE_HDISPLAY = 1920)
 *     u16 height      — pixel height (typically FAKE_MODE_VDISPLAY = 1080)
 *     u8  format      — 0 = RGBA8, 1 = BGRA8 (reserved for future use)
 *     u8  flags       — bit 0: y_flipped (GL bottom-up origin)
 *     u16 reserved    — zero
 *
 * Header + payload are sent as a single atomic "message" from the
 * subscriber's perspective (we hold the socket lock across both).
 * If the subscriber can't keep up (kernel send buffer full), we drop
 * the subscriber — partial-frame recovery isn't worth the complexity.
 *
 * Gated by SPIKE3_FRAME_BRIDGE=1 (independent from SPIKE3_BRIDGE).
 * Hook fires inside eglSwapBuffers (real-GL mode only) BEFORE the swap;
 * EGL spec leaves back-buffer contents undefined post-swap.
 * ------------------------------------------------------------------------ */

static int  shim_frame_listen_fd = -1;
static int  shim_frame_client_fd = -1;
static pthread_mutex_t shim_frame_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t shim_frame_seq = 0;
static uint8_t *shim_frame_buf = NULL;
static size_t   shim_frame_buf_size = 0;

static void *shim_frame_accept_thread(void *arg) {
    (void)arg;
    for (;;) {
        struct sockaddr_storage addr;
        socklen_t addrlen = sizeof(addr);
        int fd = accept(shim_frame_listen_fd, (struct sockaddr*)&addr, &addrlen);
        if (fd < 0) {
            if (errno == EINTR) continue;
            SHIM_LOG("[frame] accept() failed: %s — listener terminating", strerror(errno));
            return NULL;
        }
        pthread_mutex_lock(&shim_frame_lock);
        if (shim_frame_client_fd >= 0) {
            close(shim_frame_client_fd);
            SHIM_LOG("[frame] evicted prior subscriber");
        }
        shim_frame_client_fd = fd;
        pthread_mutex_unlock(&shim_frame_lock);
        SHIM_LOG("[frame] subscriber connected on fd %d", fd);
    }
    return NULL;
}

static void shim_frame_drop_client_locked(void) {
    if (shim_frame_client_fd >= 0) {
        close(shim_frame_client_fd);
        shim_frame_client_fd = -1;
    }
}

/* glReadPixels the current GL surface and ship the RGBA to the subscriber.
 * Called from eglSwapBuffers in real-GL mode. No-op if no subscriber, if
 * SPIKE3_FRAME_BRIDGE is off, or if Mesa's glReadPixels can't be resolved. */
/* Blocking full-send. Linux's default net.core.wmem_max is ~200 KB while our
 * frame payload is ~8 MB, so a non-blocking path gives us at best 1/40 of a
 * frame per send — the subscriber would get dropped on literally every frame.
 * Instead, use a blocking send and let the kernel handle back-pressure. The
 * caller (MODE_PAGE_FLIP ioctl handler) runs in the game's render thread;
 * ioctl latency is already measured in milliseconds so brief waits are fine.
 * Returns 0 on full send, -1 on hard socket error. */
static int shim_frame_send_all(int fd, const uint8_t *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

/* Ship a raw RGBA buffer over the frame bridge socket. Atomic header +
 * payload under the socket lock; uses poll-retry to tolerate the small
 * kernel send buffer (200 KB vs ~8 MB frames). Drops the subscriber only
 * after the retry timeout expires or on hard socket error. */
static void shim_frame_send(int w, int h, const uint8_t *rgba) {
    if (shim_frame_client_fd < 0 || !rgba) return;
    const size_t payload_sz = (size_t)w * (size_t)h * 4u;

    uint8_t hdr[16];
    uint32_t magic = 0xF00FBABA;
    uint32_t seq   = __atomic_add_fetch(&shim_frame_seq, 1, __ATOMIC_SEQ_CST);
    memcpy(hdr + 0, &magic, 4);
    memcpy(hdr + 4, &seq,   4);
    uint16_t w16 = (uint16_t)w, h16 = (uint16_t)h;
    memcpy(hdr + 8,  &w16, 2);
    memcpy(hdr + 10, &h16, 2);
    hdr[12] = 0;
    hdr[13] = 0;      /* flags: 0 — Pokémon LE / Radium renders top-down into
                       * the FBO we read (confirmed 2026-04-24 by empirical
                       * orientation match). If a future build truly needs
                       * bottom-up handling, set bit 0 and the subscriber will
                       * Y-flip. */
    memset(hdr + 14, 0, 2);

    pthread_mutex_lock(&shim_frame_lock);
    if (shim_frame_client_fd < 0) { pthread_mutex_unlock(&shim_frame_lock); return; }
    int fd = shim_frame_client_fd;

    if (shim_frame_send_all(fd, hdr, 16) != 0 ||
        shim_frame_send_all(fd, rgba, payload_sz) != 0) {
        shim_frame_drop_client_locked();
        SHIM_LOG("[frame] subscriber disconnected (seq=%u): %s",
                 seq, strerror(errno));
    }
    pthread_mutex_unlock(&shim_frame_lock);
}

/* Capture the current read-framebuffer to the frame bridge at page-flip.
 *
 * Evolution of this function has proven that the "pick the biggest tracked
 * EGLImage texture and read from it" strategy doesn't work: the only
 * EGLImage the game creates in this boot path is a 1024×256 sampler tied
 * to GL_TEXTURE_EXTERNAL_OES, which cannot be attached as an FBO color
 * attachment (glCheckFramebufferStatus returns GL_FRAMEBUFFER_INCOMPLETE_
 * MISSING_ATTACHMENT = 0x8CD7). That texture is a font/UI atlas, not the
 * main render target — the game renders through a different path we haven't
 * traced yet.
 *
 * Pragmatic fallback: read from whatever READ_FRAMEBUFFER is currently
 * bound when MODE_PAGE_FLIP fires. If the game unbinds its render FBO
 * before page-flipping, we read the default framebuffer (the pbuffer we
 * gave it in eglCreateWindowSurface); if the render FBO is still bound,
 * we read that. Either way we don't need texture tracking or a capture
 * FBO — just glReadPixels at the game's display resolution.
 *
 * Runs in the game thread (DRM ioctl handler), so the game's EGL context
 * is current. No context stealing, no race with the game's GL state. */
static uint32_t shim_capture_flip_counter = 0;

void shim_frame_capture_at_page_flip(void) {
    if (shim_frame_client_fd < 0) return;
    if ((++shim_capture_flip_counter) & 1) return;

    int verbose_d = (shim_capture_flip_counter < 8) ||
                    ((shim_capture_flip_counter & 63) == 0);

    if (shim_gl_marshal_active()) {
        int w = FAKE_MODE_HDISPLAY, h = FAKE_MODE_VDISPLAY;
        size_t need = (size_t)w * h * 4;
        if (!shim_frame_buf || shim_frame_buf_size < need) {
            free(shim_frame_buf);
            shim_frame_buf = malloc(need);
            shim_frame_buf_size = shim_frame_buf ? need : 0;
        }
        if (!shim_frame_buf) return;

        unsigned game_fbo = __atomic_load_n(&shim_last_draw_fbo, __ATOMIC_RELAXED);
        if (game_fbo != 0) {
            struct gl_payload_bind bind_to = { 0x8CA8 /* GL_READ_FRAMEBUFFER */, game_fbo };
            shim_gl_marshal_send_async(GL_OP_GL_BIND_FRAMEBUFFER, &bind_to, sizeof(bind_to));
        }

        struct gl_payload_read_pixels req = {
            0, 0, w, h, 0x1908 /* GL_RGBA */, 0x1401 /* GL_UNSIGNED_BYTE */ };
        size_t reply_size = sizeof(struct gl_payload_rep_pixels) + need;
        unsigned char *reply = (unsigned char*)malloc(reply_size);
        int sent = 0;
        if (reply) {
            uint32_t reply_len = 0;
            int rep_op = shim_gl_marshal_send_sync(GL_OP_GL_READ_PIXELS,
                                                   &req, sizeof(req),
                                                   reply, (uint32_t)reply_size,
                                                   &reply_len);
            if (rep_op == GL_OP_REP_PIXELS &&
                reply_len >= sizeof(struct gl_payload_rep_pixels)) {
                size_t got = reply_len - sizeof(struct gl_payload_rep_pixels);
                if (got > need) got = need;
                memcpy(shim_frame_buf, reply + sizeof(struct gl_payload_rep_pixels), got);
                if (got < need) memset(shim_frame_buf + got, 0, need - got);
                shim_frame_send(w, h, shim_frame_buf);
                sent = 1;
            }
            free(reply);
        }

        if (game_fbo != 0) {
            struct gl_payload_bind bind_back = { 0x8CA8, 0 };
            shim_gl_marshal_send_async(GL_OP_GL_BIND_FRAMEBUFFER, &bind_back, sizeof(bind_back));
        }

        if (verbose_d) {
            /* JOURNAL Step 97 — near_black sniff */
            int near_black = 0;
            int sampled = 0;
            if (sent && shim_frame_buf) {
                size_t stride = ((size_t)w * h) / 256;
                if (stride < 1) stride = 1;
                for (size_t i = 0; i < (size_t)w * h && sampled < 256; i += stride, sampled++) {
                    uint8_t r = shim_frame_buf[i*4 + 0];
                    uint8_t g = shim_frame_buf[i*4 + 1];
                    uint8_t b = shim_frame_buf[i*4 + 2];
                    if (r < 8 && g < 8 && b < 8) near_black++;
                }
            }
            SHIM_LOG("[frame_cap] %s %dx%d (flip #%u, daemon, from_fbo=%u, near_black=%d/%d)",
                     sent ? "sent" : "FAILED", w, h, shim_capture_flip_counter, game_fbo,
                     near_black, sampled);
        }
        return;
    }

    static void (*real_glReadPixels)(int, int, int, int, unsigned, unsigned, void*) = NULL;
    static void (*real_glGetIntegerv)(unsigned, int*) = NULL;
    static void (*real_glBindFramebuffer)(unsigned, unsigned) = NULL;
    if (!real_glReadPixels)
        real_glReadPixels  = (void(*)(int,int,int,int,unsigned,unsigned,void*))
                             dlsym(RTLD_NEXT, "glReadPixels");
    if (!real_glGetIntegerv)
        real_glGetIntegerv = (void(*)(unsigned,int*))dlsym(RTLD_NEXT, "glGetIntegerv");
    if (!real_glBindFramebuffer)
        real_glBindFramebuffer = (void(*)(unsigned,unsigned))dlsym(RTLD_NEXT, "glBindFramebuffer");
    if (!real_glReadPixels || !real_glGetIntegerv || !real_glBindFramebuffer) return;

    int verbose = (shim_capture_flip_counter < 8) ||
                  ((shim_capture_flip_counter & 63) == 0);

    /* Save current read-FBO; bind the game's most recent draw FBO (which it
     * likely unbound before calling page-flip, hence read_fbo=0 earlier).
     * If we don't know a game draw FBO yet, read whatever is current. */
    int prev_read = 0;
    real_glGetIntegerv(0x8CAA /* GL_READ_FRAMEBUFFER_BINDING */, &prev_read);
    unsigned game_fbo = __atomic_load_n(&shim_last_draw_fbo, __ATOMIC_RELAXED);
    if (game_fbo != 0) {
        real_glBindFramebuffer(0x8CA8 /* GL_READ_FRAMEBUFFER */, game_fbo);
    }

    int w = FAKE_MODE_HDISPLAY, h = FAKE_MODE_VDISPLAY;
    size_t need = (size_t)w * h * 4;
    if (!shim_frame_buf || shim_frame_buf_size < need) {
        free(shim_frame_buf);
        shim_frame_buf = malloc(need);
        shim_frame_buf_size = shim_frame_buf ? need : 0;
    }
    if (!shim_frame_buf) goto restore;

    real_glReadPixels(0, 0, w, h, 0x1908 /* GL_RGBA */, 0x1401 /* GL_UNSIGNED_BYTE */,
                      shim_frame_buf);
    shim_frame_send(w, h, shim_frame_buf);
    if (verbose)
        SHIM_LOG("[frame_cap] sent %dx%d (flip #%u, from_fbo=%u prev_read=%d)",
                 w, h, shim_capture_flip_counter, game_fbo, prev_read);

restore:
    if (game_fbo != 0)
        real_glBindFramebuffer(0x8CA8, (unsigned)prev_read);
}

void shim_frame_start(void) {
    const char *enabled = getenv("SPIKE3_FRAME_BRIDGE");
    if (!enabled || enabled[0] != '1') return;
    shim_frame_listen_fd = shim_bridge_create_listener(
        ABSTRACT_FRAME_NAME, "frame");
    if (shim_frame_listen_fd < 0) return;
    pthread_t t;
    if (pthread_create(&t, NULL, shim_frame_accept_thread, NULL) == 0) pthread_detach(t);
}

/* Audio bridge: AF_UNIX SOCK_STREAM listener, one subscriber at
 * a time, blocking send-all with drop-on-hard-error. Subscriber receives
 * raw S16_LE interleaved stereo @ 44.1 kHz — no headers, no sequence
 * numbers. Format is fixed by the ALSA stub's configuration, so there's
 * nothing dynamic to communicate.
 *
 * shim_audio_emit is called from snd_pcm_writei in spi_shim.c once per
 * ALSA buffer (~46 ms at 44.1 kHz / 2048 frames). End-to-end latency is
 * buffer-granular (~46 ms) plus aplay's internal buffering (~100 ms).
 *
 * Gated by SPIKE3_AUDIO_BRIDGE=1. Typical invocation from a second terminal:
 *     python3 audio_subscriber.py | aplay -f S16_LE -c 2 -r 44100 -t raw -
 */

static int  shim_audio_listen_fd = -1;
static int  shim_audio_client_fd = -1;
static pthread_mutex_t shim_audio_lock = PTHREAD_MUTEX_INITIALIZER;

static void *shim_audio_accept_thread(void *arg) {
    (void)arg;
    for (;;) {
        struct sockaddr_storage addr;
        socklen_t addrlen = sizeof(addr);
        int fd = accept(shim_audio_listen_fd, (struct sockaddr*)&addr, &addrlen);
        if (fd < 0) {
            if (errno == EINTR) continue;
            SHIM_LOG("[audio] accept() failed: %s — listener terminating", strerror(errno));
            return NULL;
        }
        pthread_mutex_lock(&shim_audio_lock);
        if (shim_audio_client_fd >= 0) {
            close(shim_audio_client_fd);
            SHIM_LOG("[audio] evicted prior subscriber");
        }
        shim_audio_client_fd = fd;
        pthread_mutex_unlock(&shim_audio_lock);
        SHIM_LOG("[audio] subscriber connected on fd %d", fd);
    }
    return NULL;
}

/* Blocking full-send; same rationale as shim_frame_send_all (kernel
 * wmem_max is small, subscriber may briefly stall). Returns 0 on full
 * send, -1 on hard socket error. */
static int shim_audio_send_all(int fd, const uint8_t *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

void shim_audio_emit(const uint8_t *buf, size_t bytes) {
    if (!buf || bytes == 0) return;
    pthread_mutex_lock(&shim_audio_lock);
    if (shim_audio_client_fd < 0) { pthread_mutex_unlock(&shim_audio_lock); return; }
    int fd = shim_audio_client_fd;
    if (shim_audio_send_all(fd, buf, bytes) != 0) {
        close(shim_audio_client_fd);
        shim_audio_client_fd = -1;
        SHIM_LOG("[audio] subscriber disconnected: %s", strerror(errno));
    }
    pthread_mutex_unlock(&shim_audio_lock);
}

void shim_audio_start(void) {
    const char *enabled = getenv("SPIKE3_AUDIO_BRIDGE");
    if (!enabled || enabled[0] != '1') return;
    shim_audio_listen_fd = shim_bridge_create_listener(
        ABSTRACT_AUDIO_NAME, "audio");
    if (shim_audio_listen_fd < 0) return;
    pthread_t t;
    if (pthread_create(&t, NULL, shim_audio_accept_thread, NULL) == 0) pthread_detach(t);
    SHIM_LOG("[audio] wire format: S16_LE 2ch 44.1 kHz raw");
}
