/*
 * spi_shim.c — LD_PRELOAD shim for Spike 3 hardware emulation.
 *
 * This is the main source file of the shim. Subsystem-specific code lives in
 * sibling files (shim_bridges.c, shim_diagnostics.c, shim_nodebus.c, ...);
 * cross-file declarations are in spi_shim.h. All source files compile into
 * a single spi_shim.so via ./build.sh.
 *
 * See _emulation/README.md for the module guide and this file's role, and
 * _emulation/SETUP.md for environment setup.
 *
 * Build:  ./build.sh
 * Run:    ./run_game.sh    (auto-detects sysroot from script location)
 *
 * Intercepts include /dev/spidev4.0 (SPI node bus), /dev/i2c-10 (I2C bus),
 * /dev/ttyAMA5 (UART netbridge), /dev/dri/card1 (DRM), plus the game's
 * EGL/GLES/GBM/libavcodec/ALSA/GPIO-sysfs surface. See README.md for the
 * full env-var list that gates individual patches.
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
#include <pthread.h>
#include <sys/mman.h>
#include <time.h>
#include <signal.h>
#include <setjmp.h>
#include <execinfo.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>

/* Not including <sys/stat.h> because it conflicts with the shim's
 * loose-typed stat/lstat/fstatat wrappers below. Just forward-declare
 * the one thing we need from it. */
extern int mkdir(const char *, unsigned int);

#include "spi_shim.h"

/* ---- Path remapping: moved to shim_fs.c in Phase 5. ---- */

/* ---- SPIKE3_REAL_GL env gate (Step 56) ----
 *
 * When set (any non-empty value), the shim's EGL/GBM/GLES interceptors
 * forward to the real library via dlsym(RTLD_NEXT, ...) instead of returning
 * fake-success stubs. Let Mesa's real pipeline do the rendering.
 *
 * Unset (default): preserves the Step 54 behavior — every interceptor is a
 * stub that returns plausible values, no Mesa ever runs. The bridge, switch,
 * UART, DRM-socketpair, ALSA, pthread_create, memcpy NULL-guard, system()
 * openocd block, /tmp/cache mkdir, shim_dump_* diagnostics all stay on
 * regardless of this flag — they are orthogonal to rendering.
 */
static int spike3_real_gl_cached = -1;
int is_real_gl(void) {
    if (__builtin_expect(spike3_real_gl_cached == -1, 0)) {
        const char *v = getenv("SPIKE3_REAL_GL");
        spike3_real_gl_cached = (v && v[0]) ? 1 : 0;
    }
    return spike3_real_gl_cached;
}

/* SHIM_LOG and FWD_{RET,VOID}{,_V} macros moved to spi_shim.h — they're
 * used at 200+ sites across what will become multiple translation units. */

/* ---- Step 59: dlopen/dlsym interception for dynamically-loaded libEGL ----
 *
 * The game resolves key EGL/GLES symbols (eglCreateImageKHR,
 * glEGLImageTargetTexture2DOES, eglCreateContext) by dlopen'ing
 * libEGL.so.1 / libGLESv2.so.2 / libgbm.so.1 and dlsym'ing from the
 * returned handles. LD_PRELOAD doesn't intercept those handle-specific
 * lookups — only calls routed through the global PLT/GOT chain hit our
 * shim. Step 58's FWD trace was silent on eglCreateContext /
 * eglCreateImageKHR for exactly this reason.
 *
 * We intercept dlopen to record handles for the three libraries of
 * interest, and intercept dlsym so that lookups against those handles
 * substitute our overrides for specific symbols we need to re-route
 * (EGLImage creation and DMA-BUF-to-texture binding). Every other
 * symbol — including 99% of EGL core and all of GLES — falls through
 * to the real library unchanged.
 *
 * Bootstrap: dlsym is itself overridden, so we cache the real dlsym/dlopen
 * once on first call via dlvsym. glibc on this sysroot exports both
 * GLIBC_2.34 and GLIBC_2.17 versions; we prefer 2.34 and fall back.
 * dlvsym is not overridden by us, so it always resolves to the real one. */

static void *shim_tracked_dl_handles[16];
static int   shim_tracked_dl_handles_count = 0;
static pthread_mutex_t shim_tracked_dl_lock = PTHREAD_MUTEX_INITIALIZER;

static int shim_dl_handle_is_tracked(void *h) {
    pthread_mutex_lock(&shim_tracked_dl_lock);
    for (int i = 0; i < shim_tracked_dl_handles_count; i++) {
        if (shim_tracked_dl_handles[i] == h) {
            pthread_mutex_unlock(&shim_tracked_dl_lock);
            return 1;
        }
    }
    pthread_mutex_unlock(&shim_tracked_dl_lock);
    return 0;
}

static void shim_dl_handle_track(void *h) {
    pthread_mutex_lock(&shim_tracked_dl_lock);
    if (shim_tracked_dl_handles_count < 16) {
        shim_tracked_dl_handles[shim_tracked_dl_handles_count++] = h;
    }
    pthread_mutex_unlock(&shim_tracked_dl_lock);
}

static void *shim_real_dlopen_p = NULL;
static void *shim_real_dlsym_p  = NULL;

static void shim_resolve_real_dl(void) {
    if (!shim_real_dlopen_p) {
        shim_real_dlopen_p = dlvsym(RTLD_NEXT, "dlopen", "GLIBC_2.34");
        if (!shim_real_dlopen_p)
            shim_real_dlopen_p = dlvsym(RTLD_NEXT, "dlopen", "GLIBC_2.17");
    }
    if (!shim_real_dlsym_p) {
        shim_real_dlsym_p = dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34");
        if (!shim_real_dlsym_p)
            shim_real_dlsym_p = dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.17");
    }
}

void *dlopen(const char *filename, int flags) {
    shim_resolve_real_dl();
    void *(*real)(const char*, int) = (void *(*)(const char*, int))shim_real_dlopen_p;
    if (!real) return NULL;
    void *h = real(filename, flags);
    /* Log every dlopen to confirm our override is actually being hit. */
    SHIM_LOG("[dlopen] \"%s\" flags=0x%x -> handle=%p", filename ? filename : "(NULL)", flags, h);
    if (h && filename) {
        if (strstr(filename, "libEGL")  ||
            strstr(filename, "libGLES") ||
            strstr(filename, "libgbm")) {
            SHIM_LOG("[dlopen] tracking %s handle=%p for dlsym intercept", filename, h);
            shim_dl_handle_track(h);
        }
    }
    return h;
}

void *dlsym(void *handle, const char *symbol) {
    shim_resolve_real_dl();
    void *(*real)(void*, const char*) = (void *(*)(void*, const char*))shim_real_dlsym_p;
    if (!real) return NULL;

    /* Substitute only for handles we tracked (EGL/GLES/GBM). RTLD_NEXT /
     * RTLD_DEFAULT / unknown handles fall through untouched. */
    if (handle != RTLD_DEFAULT && handle != RTLD_NEXT && symbol &&
        shim_dl_handle_is_tracked(handle)) {
        if (strcmp(symbol, "eglCreateImageKHR") == 0) {
            extern void *eglCreateImageKHR(void*, void*, unsigned, void*, const int*);
            SHIM_LOG("[dlsym intercept] eglCreateImageKHR -> shim override");
            return (void*)eglCreateImageKHR;
        }
        if (strcmp(symbol, "eglDestroyImageKHR") == 0) {
            extern unsigned eglDestroyImageKHR(void*, void*);
            SHIM_LOG("[dlsym intercept] eglDestroyImageKHR -> shim override");
            return (void*)eglDestroyImageKHR;
        }
        if (strcmp(symbol, "glEGLImageTargetTexture2DOES") == 0) {
            extern void glEGLImageTargetTexture2DOES(unsigned, void*);
            SHIM_LOG("[dlsym intercept] glEGLImageTargetTexture2DOES -> shim override");
            return (void*)glEGLImageTargetTexture2DOES;
        }
        /* Log once per miss so we know what else the game is asking for. */
        SHIM_LOG("[dlsym] tracked handle %p wants %s (passthrough)", handle, symbol);
    }
    return real(handle, symbol);
}




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

/* FAKE_FD_SPI / FAKE_FD_I2C / FAKE_FD_V4L2_MEDIA / FAKE_FD_V4L2_VIDEO are in
 * spi_shim.h (so shim_io.c + shim_ioctl.c can range-check fds). DRM and UART
 * use real socketpair fds; is_drm_fd() / is_uart_fd() predicates check them. */

/* ---- V4L2 stub fds (Step 46) ----
 * libavcodec's v4l2_request HEVC decoder respects LIBVA_V4L2_REQUEST_MEDIA_PATH
 * and LIBVA_V4L2_REQUEST_VIDEO_PATH env vars. We set both at run time to
 * these sentinel paths and intercept the open — no /dev/media0 on the host
 * means we must provide our own fake device. Path sentinels stay here
 * (only intercept_device uses them). FAKE_FD_V4L2_* constants are in spi_shim.h. */
#define FAKE_V4L2_MEDIA_PATH  "/dev/shim-v4l2-media"
#define FAKE_V4L2_VIDEO_PATH  "/dev/shim-v4l2-video"
int      v4l2_media_active = 0;
int      v4l2_video_active = 0;
uint32_t v4l2_ioctl_count = 0;

/* ---- GPIO sysfs stubs (Step 63) ----
 * The game uses legacy /sys/class/gpio/{export,gpio25/direction,gpio25/value}
 * to bit-bang chip-select (or an interrupt line) for a SPI peripheral. Under
 * qemu-user the host's sysfs either has no gpio class or is not writable,
 * so the game sees EACCES on export and spins in a retry loop (687 failures
 * in a 185s run). We present each sysfs path as a small fake fd that swallows
 * writes and feeds back plausible reads ("0\n" for value, "out\n" for
 * direction). One fd per open() — the game re-opens on every poll. */

/* ---- Device state ---- */

int      spi_active = 0;
int      i2c_active = 0;
int      uart_game_fd = -1;      /* returned from open(); real socketpair end */
int      uart_shim_fd = -1;      /* held by the shim for pushing responses;
                                    read by shim_nodebus.c via extern */
int      drm_game_fd  = -1;      /* returned from open(/dev/dri/card1) */
int      drm_shim_fd  = -1;      /* shim end; we write synthetic flip events here */
uint32_t spi_transfer_count = 0;
uint32_t i2c_ioctl_count = 0;
uint32_t uart_read_count = 0;
uint32_t uart_write_count = 0;

/* Predicates exported via spi_shim.h — called from shim_io.c, shim_ioctl.c,
 * and the termios hooks still in this file. */
int is_drm_fd(int fd) { return drm_game_fd >= 0 && fd == drm_game_fd; }

/* SHIM_LOG moved up to just after the macros — see the early definition. */

/* ---- Real function pointers ---- */

typedef int (*real_open_t)(const char *, int, ...);
typedef int (*real_close_t)(int);
typedef int (*real_ioctl_t)(int, unsigned long, ...);
typedef ssize_t (*real_read_t)(int, void *, size_t);
typedef ssize_t (*real_write_t)(int, const void *, size_t);

real_open_t  real_open  = NULL;   /* used by open() hook in shim_fs.c */
real_close_t real_close = NULL;   /* used by close() hook in shim_io.c */
real_ioctl_t real_ioctl = NULL;   /* used by ioctl() hook in shim_ioctl.c */
real_read_t  real_read  = NULL;   /* used by read() hook in shim_io.c */
real_write_t real_write = NULL;   /* used by shim_nodebus.c + shim_io.c write() + shim_ioctl.c flip event */

/* ---- Shim directory init ----
 *
 * Audio capture (_emulation/captures/audio/stream.s16le) is the only
 * data-capture pipeline still in use — texture/shader/DMD capture was
 * removed in Step 56 (capture_hooks_note in README.md documents how to
 * restore them if ever needed)
 * (duplicative of _extraction/ asset extraction, and DMD doesn't exist on
 * modern Spike 3 hardware).
 *
 * /tmp/cache exists because Godot logs an ERROR when it can't create it
 * (Step 45); the mkdir here pre-empts that.
 */

__attribute__((constructor))
static void shim_init_dirs(void) {
    mkdir("_emulation",                0755);
    mkdir("_emulation/captures",       0755);
    mkdir("_emulation/captures/audio", 0755);
    mkdir("/tmp/cache",                0755);
}


/* ---- Pokémon LE switch manifest (Phase 9) moved to shim_inject.c ---- */

/* ---- Event-sweep machinery (Phase 9) moved to shim_inject.c ----
 * Includes the sigjmp_buf / shim_event_fire_in_progress /
 * shim_current_sweep_event_id globals whose externs are declared
 * in spi_shim.h. */



/* ---- Poke threads (Phase 9) moved to shim_inject.c ---- */

/* ---- Patches (Phase 8) + injection threads (Phase 9) carved out ----
 * shim_patch_spi_video_vtable / shim_patch_get_format_callback live in
 * shim_patches.c. The vp_patch thread that applies them, all other
 * env-gated worker threads, the event-sweep machinery, and the
 * SIGALRM handler live in shim_inject.c. See spi_shim.h for the
 * two public entry points (shim_event_sweep, shim_inject_start). */

__attribute__((constructor))
static void shim_install_crash_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = shim_sigsegv_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);

    struct sigaction sa2;
    memset(&sa2, 0, sizeof(sa2));
    sa2.sa_handler = shim_term_dump_handler;
    sigemptyset(&sa2.sa_mask);
    sigaction(SIGTERM, &sa2, NULL);
    sigaction(SIGINT,  &sa2, NULL);
    sigaction(SIGUSR1, &sa2, NULL);
    sigaction(SIGUSR2, &sa2, NULL);  /* Step 52: event sweep (signal path, usually swallowed by qemu) */

    /* Optional state bridge over Unix socket (SPIKE3_BRIDGE=1). */
    shim_bridge_start();

    /* Optional frame bridge — glReadPixels → AF_UNIX stream (SPIKE3_FRAME_BRIDGE=1). */
    shim_frame_start();

    /* SIGALRM handler, main-thread SIGALRM block, and the six env-gated
     * worker threads (sweep timer, switch firing, gstreamer force-init,
     * cmode poke, state-flag poke, vp-patch) — all live in shim_inject.c. */
    shim_inject_start();
}


void resolve_all(void) {
    if (!real_open)  real_open  = (real_open_t)dlsym(RTLD_NEXT, "open");
    if (!real_close) real_close = (real_close_t)dlsym(RTLD_NEXT, "close");
    if (!real_ioctl) real_ioctl = (real_ioctl_t)dlsym(RTLD_NEXT, "ioctl");
    if (!real_read)  real_read  = (real_read_t)dlsym(RTLD_NEXT, "read");
    if (!real_write) real_write = (real_write_t)dlsym(RTLD_NEXT, "write");
}


/* ---- Check if path is a device we intercept, return fake fd or 0 ---- */

int intercept_device(const char *pathname) {
    if (!pathname) return 0;

    if (is_gpio_sysfs_path(pathname)) {
        int fd = alloc_gpio_fake_fd(pathname);
        if (fd) {
            SHIM_LOG("OPEN %s -> fake GPIO fd %d", pathname, fd);
            return fd;
        }
        /* pool exhausted — leak to real open which will fail; not fatal */
        SHIM_LOG("GPIO fake-fd pool exhausted for %s", pathname);
    }

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
    if (strcmp(pathname, "/dev/dri/card1") == 0) {
        if (drm_game_fd < 0) {
            int sv[2];
            if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
                SHIM_LOG("socketpair() failed for %s: %s", pathname, strerror(errno));
                return 0;
            }
            drm_game_fd = sv[0];
            drm_shim_fd = sv[1];
            int flags = fcntl(drm_shim_fd, F_GETFL, 0);
            if (flags >= 0) fcntl(drm_shim_fd, F_SETFL, flags | O_NONBLOCK);
        }
        SHIM_LOG("OPEN %s -> real socketpair game=%d shim=%d (DRM)",
                 pathname, drm_game_fd, drm_shim_fd);
        return drm_game_fd;
    }
    if (strcmp(pathname, FAKE_V4L2_MEDIA_PATH) == 0) {
        v4l2_media_active = 1;
        SHIM_LOG("OPEN %s -> fake fd %d (V4L2 media controller — probe logging)",
                 pathname, FAKE_FD_V4L2_MEDIA);
        return FAKE_FD_V4L2_MEDIA;
    }
    if (strcmp(pathname, FAKE_V4L2_VIDEO_PATH) == 0) {
        v4l2_video_active = 1;
        SHIM_LOG("OPEN %s -> fake fd %d (V4L2 video device — probe logging)",
                 pathname, FAKE_FD_V4L2_VIDEO);
        return FAKE_FD_V4L2_VIDEO;
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

int is_fake_fd(int fd) {
    /* UART and DRM are NOT fake — both are real socketpair fds; callers
     * that need to check for "any intercepted device" should call both
     * this and uart_game_fd / drm_game_fd explicitly. */
    return (fd == FAKE_FD_SPI && spi_active) ||
           (fd == FAKE_FD_I2C && i2c_active) ||
           (fd == FAKE_FD_V4L2_MEDIA && v4l2_media_active) ||
           (fd == FAKE_FD_V4L2_VIDEO && v4l2_video_active);
}

int is_v4l2_fd(int fd) {
    return (fd == FAKE_FD_V4L2_MEDIA && v4l2_media_active) ||
           (fd == FAKE_FD_V4L2_VIDEO && v4l2_video_active);
}

int is_uart_fd(int fd) {
    return uart_game_fd >= 0 && fd == uart_game_fd;
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

/* ---- Hooked: pthread_create (force inherit-sched) ----
 * The game spawns at least one worker thread with explicit SCHED_RR
 * attributes (seen in strace: `sched_setscheduler(3614, 2, ...) = EPERM`).
 * Under QEMU user-mode running as a regular user we lack CAP_SYS_NICE AND
 * ulimit -r is 0 (RT priority disabled at the kernel level). glibc's
 * pthread_create, when its internal setup-handshake call to
 * sched_setscheduler fails, signals the suspended child to exit WITHOUT
 * running its entry function. That thread was the dispatch-subsystem
 * worker whose code path ultimately sets `data_4882108` to 1 — the flag
 * polled by the orchestrator `sub_a78bc0` (file 0x678bc0) in a 480-iter
 * × 250 ms loop. Flag never gets set → orchestrator loops forever.
 *
 * glibc uses INTERNAL_SYSCALL_CALL for sched_setscheduler inside
 * pthread_create, which bypasses LD_PRELOAD on the public wrapper (same
 * INLINE_SYSCALL pattern as access/stat). So we hook pthread_create itself:
 * if the caller passed an attr with PTHREAD_EXPLICIT_SCHED, copy it,
 * force PTHREAD_INHERIT_SCHED on the copy, and pass the copy through.
 * The child then inherits the parent's scheduling (SCHED_OTHER) — no
 * privileged syscall involved. Priority differences don't matter under
 * QEMU user-mode emulation anyway.
 */

typedef int (*real_pthread_create_t)(pthread_t *, const pthread_attr_t *,
                                     void *(*)(void *), void *);

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg) {
    static real_pthread_create_t real_fn = NULL;
    if (!real_fn) real_fn = (real_pthread_create_t)dlsym(RTLD_NEXT, "pthread_create");

    if (attr != NULL) {
        int inherit = 0;
        if (pthread_attr_getinheritsched(attr, &inherit) == 0
            && inherit == PTHREAD_EXPLICIT_SCHED) {
            pthread_attr_t copy;
            memcpy(&copy, attr, sizeof(pthread_attr_t));
            pthread_attr_setinheritsched(&copy, PTHREAD_INHERIT_SCHED);
            SHIM_LOG("pthread_create: forcing PTHREAD_INHERIT_SCHED (was EXPLICIT)");
            return real_fn(thread, &copy, start_routine, arg);
        }
    }
    return real_fn(thread, attr, start_routine, arg);
}




/* ---- Hooked: libasound (ALSA) ----
 *
 * Game has no fallback — under QEMU user-mode with no /dev/snd devices, the
 * real `snd_pcm_open` fails, and although the game *does* check the
 * return code (BN: `alsa_pcm_init` at `0xce61e0` tests bit 31 at
 * `0xce6270`, jumps to error-7 path), it still crashes later when the
 * audio subsystem calls `snd_pcm_prepare(NULL)`. Our fix: fake-succeed
 * every ALSA call so the audio thread runs, and capture the PCM
 * stream via `snd_pcm_writei`.
 *
 * Audio format (BN confirmed Step 25.6 research):
 *   44100 Hz, 2 channels, S16_LE, interleaved. Default buffer = 2048
 *   frames = 8192 bytes = 46.4 ms. The audio thread is a tight loop
 *   around `snd_pcm_writei` with no sleep — if our stub returns
 *   instantly it will spin at 100% CPU. We pace with `usleep(size *
 *   1000000 / 44100)` inside writei to match real ALSA timing.
 *
 * Capture: every `snd_pcm_writei` buffer is appended to
 * `_emulation/captures/audio/stream.s16le`. Playback / conversion:
 *   ffplay -f s16le -ar 44100 -ac 2 stream.s16le
 *   ffmpeg -f s16le -ar 44100 -ac 2 -i stream.s16le stream.wav
 */

#define ALSA_FAKE_PARAMS_SIZE 1024  /* upper bound; caller alloc stack-safe */
#define ALSA_DEFAULT_RATE     44100
#define ALSA_DEFAULT_CHANNELS 2

static void *fake_pcm_handle     = NULL;
static FILE *alsa_capture_file   = NULL;
static pthread_mutex_t alsa_capture_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t alsa_writei_count = 0;

int snd_pcm_open(void **pcm, const char *name, int stream, int mode) {
    if (!fake_pcm_handle) fake_pcm_handle = calloc(1, 256);
    if (pcm) *pcm = fake_pcm_handle;
    SHIM_LOG("snd_pcm_open(name=%s, stream=%d, mode=%d) -> %p (fake)",
             name ? name : "(null)", stream, mode, fake_pcm_handle);
    return 0;
}
int snd_pcm_close(void *pcm)                      { (void)pcm; return 0; }
int snd_pcm_prepare(void *pcm)                    { (void)pcm; return 0; }
int snd_pcm_drain(void *pcm)                      { (void)pcm; return 0; }
int snd_pcm_nonblock(void *pcm, int nonblock)     { (void)pcm; (void)nonblock; return 0; }

size_t snd_pcm_hw_params_sizeof(void) { return ALSA_FAKE_PARAMS_SIZE; }

int snd_pcm_hw_params(void *pcm, void *params)          { (void)pcm; (void)params; return 0; }
int snd_pcm_hw_params_any(void *pcm, void *params)      { (void)pcm; (void)params; return 0; }
int snd_pcm_hw_params_set_access(void *pcm, void *params, int access) {
    (void)pcm; (void)params; (void)access; return 0;
}
int snd_pcm_hw_params_set_format(void *pcm, void *params, int format) {
    (void)pcm; (void)params; (void)format; return 0;
}
int snd_pcm_hw_params_set_channels(void *pcm, void *params, unsigned channels) {
    (void)pcm; (void)params; (void)channels; return 0;
}
int snd_pcm_hw_params_set_rate(void *pcm, void *params, unsigned rate, int dir) {
    (void)pcm; (void)params; (void)rate; (void)dir; return 0;
}
int snd_pcm_hw_params_set_rate_near(void *pcm, void *params, unsigned *rate, int *dir) {
    (void)pcm; (void)params;
    if (rate) *rate = ALSA_DEFAULT_RATE;
    if (dir)  *dir  = 0;
    return 0;
}
int snd_pcm_hw_params_set_buffer_size(void *pcm, void *params, unsigned long size) {
    (void)pcm; (void)params; (void)size; return 0;
}
int snd_pcm_hw_params_set_period_size(void *pcm, void *params, unsigned long size, int dir) {
    (void)pcm; (void)params; (void)size; (void)dir; return 0;
}
int snd_pcm_hw_params_get_channels(const void *params, unsigned *val) {
    (void)params;
    if (val) *val = ALSA_DEFAULT_CHANNELS;
    return 0;
}
int snd_pcm_hw_params_get_rate(const void *params, unsigned *val, int *dir) {
    (void)params;
    if (val) *val = ALSA_DEFAULT_RATE;
    if (dir) *dir = 0;
    return 0;
}

/* Return type is snd_pcm_sframes_t (signed long) — see alsa-lib. */
long snd_pcm_writei(void *pcm, const void *buffer, unsigned long size) {
    (void)pcm;
    /* Append the raw S16_LE interleaved buffer to the session stream. */
    if (buffer && size > 0) {
        size_t bytes = (size_t)size * 4; /* stereo × 2 bytes = 4 B/frame */
        pthread_mutex_lock(&alsa_capture_lock);
        if (!alsa_capture_file) {
            /* "wb" truncates on the first open of each run so stream.s16le
             * starts fresh per game invocation. Within a single run the shim
             * holds the FILE* and appends naturally via subsequent fwrites. */
            alsa_capture_file = fopen("_emulation/captures/audio/stream.s16le", "wb");
        }
        if (alsa_capture_file) {
            fwrite(buffer, 1, bytes, alsa_capture_file);
            /* Flush occasionally so cold-pickup gets partial data if we
             * get killed — every 64 writes ≈ 3 s at 46 ms/buffer. */
            if ((alsa_writei_count & 63) == 0) fflush(alsa_capture_file);
        }
        uint32_t n = ++alsa_writei_count;
        pthread_mutex_unlock(&alsa_capture_lock);
        if (n <= 4 || (n % 500) == 0) {
            SHIM_LOG("snd_pcm_writei #%u frames=%lu bytes=%zu",
                     n, size, bytes);
        }
    }
    /* Pace the audio thread to real-time rate. Without this the game's
     * tight writei loop would spin at 100% CPU (BN: no usleep between
     * generate-samples → writei → loop). */
    unsigned long delay_us = (size * 1000000UL) / ALSA_DEFAULT_RATE;
    if (delay_us) usleep((unsigned)delay_us);
    return (long)size;
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



/* ---- Runtime patches moved to shim_patches.c (Phase 8) ----
 * memcpy NULL-guard, libavcodec HW-accel bypass, dummy-frame injection.
 * See shim_patches.c's header comment for the full rationale. */

/* ---- Step 53: GStreamer interception (passthrough + logging) ----------
 *
 * Goal: observe the game's GStreamer pipeline construction so we can
 * later redirect the sink element to appsink and route frames to a host-
 * renderable surface. This pass is OBSERVATION ONLY — every call falls
 * through to the real libgstreamer-1.0 after logging.
 *
 * Element creation:
 *   gst_element_factory_make(factory, name)
 *   gst_parse_launch(description, err)
 *
 * Pipeline assembly:
 *   gst_bin_add, gst_bin_add_many
 *   gst_element_link, gst_element_link_many
 *
 * Lifecycle:
 *   gst_element_set_state
 *
 * We also hook gst_init_check so we can confirm gstreamer initialises
 * cleanly under QEMU+sysroot (the gstreamer plugins live at
 * <sysroot>/usr/lib64/gstreamer-1.0/ — 36 plugins inc. libgstkms.so,
 * libgstapp.so, libgstisomp4.so, libgstvideoconvertscale.so).
 *
 * GStreamer opaque types are referenced as void* to avoid needing gst
 * headers in the shim build. The ABI is stable — enums are ints,
 * handles are pointers.
 * ------------------------------------------------------------------------ */

typedef void (*real_gst_element_factory_make_t)(void);  /* placeholder; see below */

/* Real function pointers resolved lazily via dlsym(RTLD_NEXT). */
static void *(*real_gst_element_factory_make)(const char *, const char *) = NULL;
static void *(*real_gst_parse_launch)(const char *, void *) = NULL;
static int   (*real_gst_bin_add)(void *, void *) = NULL;
static int   (*real_gst_element_link)(void *, void *) = NULL;
static int   (*real_gst_element_set_state)(void *, int) = NULL;
static int   (*real_gst_init_check)(int *, char ***, void *) = NULL;

static void *shim_resolve_gst(const char *sym) {
    void *r = dlsym(RTLD_NEXT, sym);
    if (!r) {
        /* Fallback: open libgstreamer-1.0 directly (RTLD_NOLOAD in case
         * it's already loaded; otherwise RTLD_LAZY). */
        void *h = dlopen("libgstreamer-1.0.so.0", RTLD_LAZY | RTLD_NOLOAD);
        if (!h) h = dlopen("libgstreamer-1.0.so.0", RTLD_LAZY);
        if (h) r = dlsym(h, sym);
    }
    return r;
}

static unsigned gst_factory_make_count    = 0;
static unsigned gst_parse_launch_count    = 0;
static unsigned gst_bin_add_count         = 0;
static unsigned gst_element_link_count    = 0;
static unsigned gst_set_state_count       = 0;

static const char *gst_state_name(int state) {
    switch (state) {
        case 0: return "VOID_PENDING";
        case 1: return "NULL";
        case 2: return "READY";
        case 3: return "PAUSED";
        case 4: return "PLAYING";
        default: return "UNKNOWN";
    }
}

int gst_init_check(int *argc, char ***argv, void *err) {
    if (!real_gst_init_check)
        real_gst_init_check = shim_resolve_gst("gst_init_check");
    if (!real_gst_init_check) {
        SHIM_LOG("[gst] gst_init_check: FAILED TO RESOLVE real symbol");
        return 0;
    }
    int r = real_gst_init_check(argc, argv, err);
    const char *pp = getenv("GST_PLUGIN_PATH");
    SHIM_LOG("[gst] gst_init_check() -> %d  (GST_PLUGIN_PATH=%s)", r,
             pp ? pp : "(unset)");
    return r;
}

static void (*real_gst_deinit)(void) = NULL;
void gst_deinit(void) {
    if (!real_gst_deinit)
        real_gst_deinit = shim_resolve_gst("gst_deinit");
    SHIM_LOG("[gst] gst_deinit() called  (factory_make=%u parse=%u bin_add=%u link=%u setstate=%u)",
             gst_factory_make_count, gst_parse_launch_count,
             gst_bin_add_count, gst_element_link_count, gst_set_state_count);
    if (real_gst_deinit) real_gst_deinit();
}

void *gst_element_factory_make(const char *factory, const char *name) {
    if (!real_gst_element_factory_make)
        real_gst_element_factory_make = shim_resolve_gst("gst_element_factory_make");
    if (!real_gst_element_factory_make) return NULL;

    void *r = real_gst_element_factory_make(factory, name);
    gst_factory_make_count++;

    /* Flag sinks and decoders — we care most about those. */
    int is_sink = factory && strstr(factory, "sink") != NULL;
    int is_dec  = factory && (strstr(factory, "dec") != NULL ||
                              strstr(factory, "parse") != NULL);
    const char *tag = is_sink ? " [SINK]" : (is_dec ? " [DECODE]" : "");

    SHIM_LOG("[gst] #%u factory_make(factory=\"%s\", name=\"%s\") -> %p%s",
             gst_factory_make_count,
             factory ? factory : "(null)",
             name ? name : "(null)",
             r, tag);
    return r;
}

void *gst_parse_launch(const char *description, void *err) {
    if (!real_gst_parse_launch)
        real_gst_parse_launch = shim_resolve_gst("gst_parse_launch");
    if (!real_gst_parse_launch) return NULL;

    void *r = real_gst_parse_launch(description, err);
    gst_parse_launch_count++;
    SHIM_LOG("[gst] #%u parse_launch(\"%s\") -> %p",
             gst_parse_launch_count,
             description ? description : "(null)", r);
    return r;
}

int gst_bin_add(void *bin, void *element) {
    if (!real_gst_bin_add)
        real_gst_bin_add = shim_resolve_gst("gst_bin_add");
    if (!real_gst_bin_add) return 0;

    int r = real_gst_bin_add(bin, element);
    gst_bin_add_count++;
    /* First 16 and every 10th after that — keeps the log from drowning
     * in identical bin_add lines during heavy pipeline construction. */
    if (gst_bin_add_count <= 16 || (gst_bin_add_count % 10) == 0) {
        SHIM_LOG("[gst] #%u bin_add(bin=%p, el=%p) -> %d",
                 gst_bin_add_count, bin, element, r);
    }
    return r;
}

/* Variadic bin_add_many — libgstreamer may inline-call its own
 * gst_bin_add (bypassing our LD_PRELOAD), so we reimplement in terms of
 * our hooked gst_bin_add to guarantee we see every element. */
int gst_bin_add_many(void *bin, void *first, ...) {
    if (!first) return 1;  /* no elements — "success" */
    int all_ok = gst_bin_add(bin, first);
    va_list ap;
    va_start(ap, first);
    for (;;) {
        void *e = va_arg(ap, void *);
        if (!e) break;
        if (!gst_bin_add(bin, e)) all_ok = 0;
    }
    va_end(ap);
    return all_ok;
}

int gst_element_link(void *src, void *dest) {
    if (!real_gst_element_link)
        real_gst_element_link = shim_resolve_gst("gst_element_link");
    if (!real_gst_element_link) return 0;

    int r = real_gst_element_link(src, dest);
    gst_element_link_count++;
    if (gst_element_link_count <= 16 || (gst_element_link_count % 10) == 0) {
        SHIM_LOG("[gst] #%u element_link(src=%p, dest=%p) -> %d",
                 gst_element_link_count, src, dest, r);
    }
    return r;
}

/* Variadic link_many — chain-links each neighbour pair. */
int gst_element_link_many(void *first, void *second, ...) {
    if (!first || !second) return 0;
    if (!gst_element_link(first, second)) return 0;
    va_list ap;
    va_start(ap, second);
    void *prev = second;
    int ok = 1;
    for (;;) {
        void *next = va_arg(ap, void *);
        if (!next) break;
        if (!gst_element_link(prev, next)) { ok = 0; break; }
        prev = next;
    }
    va_end(ap);
    return ok;
}

int gst_element_set_state(void *element, int state) {
    if (!real_gst_element_set_state)
        real_gst_element_set_state = shim_resolve_gst("gst_element_set_state");
    if (!real_gst_element_set_state) return 0;

    int r = real_gst_element_set_state(element, state);
    gst_set_state_count++;
    /* Always log PLAYING/NULL transitions (pipeline start/stop milestones);
     * sample-log the rest to keep noise down. */
    int is_milestone = (state == 4 /*PLAYING*/) || (state == 1 /*NULL*/);
    if (is_milestone || gst_set_state_count <= 24 ||
        (gst_set_state_count % 20) == 0) {
        SHIM_LOG("[gst] #%u element_set_state(el=%p, state=%s) -> %d%s",
                 gst_set_state_count, element, gst_state_name(state), r,
                 is_milestone ? "  [MILESTONE]" : "");
    }
    return r;
}

