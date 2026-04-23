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
 * Audio capture (_emulation/audio/stream.s16le) is the only data-capture
 * pipeline still in use — texture/shader/DMD capture was removed in Step 56
 * (duplicative of _extraction/ asset extraction, and DMD doesn't exist on
 * modern Spike 3 hardware).
 *
 * /tmp/cache exists because Godot logs an ERROR when it can't create it
 * (Step 45); the mkdir here pre-empts that.
 */

__attribute__((constructor))
static void shim_init_dirs(void) {
    mkdir("_emulation",           0755);
    mkdir("_emulation/audio",     0755);
    mkdir("/tmp/cache",           0755);
}


/* ---- Step 52: Pokémon LE switch manifest ------------------------------
 *
 * Transcribed from the Pokémon LE / Premium manual (500-55AJ-01) switch
 * reference table. Only switches actually present in this machine are
 * listed — the manual allocates ID space with gaps (3, 4, 13, 14, 24, 26,
 * 32, 38, 39, 42, 43, 47, 48, 62, 65, 68, 69, 75, 77, 81-89, 91, 92, 95)
 * that correspond to unused slots. Do not fire those IDs; they have no
 * meaning on this machine.
 *
 * The `id` field is the manual's switch number (1..105). `node_addr` is
 * the node-local address (e.g. "8-SW-17") which maps to the RS485 node
 * bus frame the netbridge builds — if we ever need to emulate the wire
 * protocol more faithfully, that column is the key.
 *
 * `issues` is a free-form comment documenting known problems when firing
 * this switch from the shim (filled in as we discover them).
 * ------------------------------------------------------------------------ */

struct shim_switch_entry {
    int         id;           /* manual ID (1..105, plus C10/C11 as 200+10, 200+11) */
    const char *name;         /* human-readable */
    const char *location;     /* Playfield / Cabinet / CoinDoor */
    const char *type;         /* Leaf / Opto / Rollover / Target / Micro / MagSw / Plumb */
    const char *node_addr;    /* e.g. "8-SW-17" */
    const char *issues;       /* NULL if none known, otherwise terse comment */
};

static const struct shim_switch_entry shim_switch_manifest[] = {
    /* Playfield rollovers / ramps / lanes — Node 8 */
    {  1, "Left Outlane",              "Playfield", "Rollover", "8-SW-17", NULL },
    {  2, "Left Return Lane",          "Playfield", "Rollover", "8-SW-18", NULL },
    {  5, "Right Return Lane",         "Playfield", "Rollover", "8-SW-19", NULL },
    {  6, "Right Outlane",             "Playfield", "Rollover", "8-SW-20", NULL },
    /* Flippers / slingshots */
    {  7, "Left Slingshot",            "Playfield", "Leaf",     "8-SW-30", NULL },
    {  8, "Right Slingshot",           "Playfield", "Leaf",     "8-SW-29", NULL },
    {  9, "Left Flipper Button",       "Cabinet",   "Leaf",     "8-SW-25", NULL },
    { 10, "Right Flipper Button",      "Cabinet",   "Leaf",     "8-SW-24", NULL },
    { 11, "Left Flipper EOS",          "Playfield", "Leaf",     "8-SW-16", NULL },
    { 12, "Right Flipper EOS",         "Playfield", "Leaf",     "8-SW-31", NULL },
    /* Trough — Node 8 extension 8a (opto array) */
    { 15, "Trough 6",                  "Playfield", "Opto",     "8-SW-32", NULL },
    { 16, "Trough 5",                  "Playfield", "Opto",     "8-SW-33", NULL },
    { 17, "Trough 4",                  "Playfield", "Opto",     "8-SW-34", NULL },
    { 18, "Trough 3",                  "Playfield", "Opto",     "8-SW-35", NULL },
    { 19, "Trough 2",                  "Playfield", "Opto",     "8-SW-36", NULL },
    { 20, "Trough 1",                  "Playfield", "Opto",     "8-SW-37", NULL },
    { 21, "Trough Jam",                "Playfield", "Opto",     "8-SW-38", NULL },
    /* Shooter lane + bowl */
    { 22, "Shooter Lane",              "Playfield", "Micro",    "8-SW-28", NULL },
    { 25, "Bowl Opto",                 "Playfield", "Opto",     "8-SW-27", NULL },
    /* Berry targets + left ramp cluster */
    { 27, "Berry Target Left",         "Playfield", "Target",   "8-SW-9",  NULL },
    { 28, "Berry Target Center",       "Playfield", "Target",   "8-SW-10", NULL },
    { 29, "Left Ramp Enter Opto",      "Playfield", "Opto",     "8-SW-11", NULL },
    { 30, "Left Ramp Made 2 Gate",     "Playfield", "Micro",    "8-SW-12", NULL },
    { 31, "Subway Enter",              "Playfield", "Opto",     "8-SW-8",  NULL },
    /* Lower standup target bank */
    { 33, "Lower Left Target-Top",     "Playfield", "Target",   "8-SW-0",  NULL },
    { 34, "Lower Left Target-Mid",     "Playfield", "Target",   "8-SW-1",  NULL },
    { 35, "Lower Left Target-Bot",     "Playfield", "Target",   "8-SW-2",  NULL },
    { 36, "Lower Right Target-Top",    "Playfield", "Target",   "8-SW-3",  NULL },
    { 37, "Lower Right Target-Bot",    "Playfield", "Target",   "8-SW-4",  NULL },
    /* Ejects */
    { 40, "Left Eject Opto",           "Playfield", "Opto",     "8-SW-5",  NULL },
    { 41, "Right Eject",               "Playfield", "Micro",    "9-SW-1",  NULL },
    /* Node 9 — orbits / ramps / arena / BATTLE / Team Rocket */
    { 44, "Left Orbit",                "Playfield", "Rollover", "9-SW-8",  NULL },
    { 45, "Left Ramp Made 1 Opto",     "Playfield", "Opto",     "9-SW-9",  NULL },
    { 46, "Rear Ramp Made",            "Playfield", "Micro",    "9-SW-29", NULL },
    { 49, "Arena Left 10 Pt",          "Playfield", "Leaf",     "9-SW-13", NULL },
    { 50, "Arena Right 10 Pt",         "Playfield", "Leaf",     "9-SW-14", NULL },
    { 51, "(B)ATTLE Target",           "Playfield", "Target",   "9-SW-26", NULL },
    { 52, "B(A)TTLE Target",           "Playfield", "Target",   "9-SW-27", NULL },
    { 53, "BA(T)TLE Target",           "Playfield", "Target",   "9-SW-17", NULL },
    { 54, "BAT(T)LE Target",           "Playfield", "Target",   "9-SW-18", NULL },
    { 55, "BATT(L)E Target",           "Playfield", "Target",   "9-SW-19", NULL },
    { 56, "BATTL(E) Target",           "Playfield", "Target",   "9-SW-20", NULL },
    { 57, "Team Rocket Target Left",   "Playfield", "Target",   "9-SW-21", NULL },
    { 58, "Team Rocket Target Right",  "Playfield", "Target",   "9-SW-22", NULL },
    { 59, "Meowth Hit Opto",           "Playfield", "Opto",     "9-SW-23", NULL },
    { 60, "Meowth Up",                 "Playfield", "MagSw",    "9-SW-24", NULL },
    { 61, "Meowth Down",               "Playfield", "MagSw",    "9-SW-25", NULL },
    { 63, "Top Pop Bumper",            "Playfield", "Leaf",     "9-SW-28", NULL },
    { 64, "Bot Pop Bumper",            "Playfield", "Leaf",     "9-SW-30", NULL },
    { 66, "Top Lane Left",             "Playfield", "Rollover", "9-SW-10", NULL },
    { 67, "Top Lane Right",            "Playfield", "Rollover", "9-SW-11", NULL },
    /* Berry right target + captive ball + spinner + right ramp */
    { 70, "Berry Target Right",        "Playfield", "Target",   "9-SW-0",  NULL },
    { 71, "Captive Ball-Lower",        "Playfield", "Rollover", "9-SW-4",  NULL },
    { 72, "Captive Ball-Upper",        "Playfield", "Rollover", "9-SW-5",  NULL },
    { 73, "Captive Ball-Target",       "Playfield", "Target",   "9-SW-6",  NULL },
    { 74, "Right Ramp Hold",           "Playfield", "Micro",    "9-SW-15", NULL },
    { 76, "Right Ramp Made Opto",      "Playfield", "Opto",     "9-SW-31", NULL },
    { 78, "Spinner Opto",              "Playfield", "Opto",     "9-SW-7",  NULL },
    { 79, "Team Rocket Enter",         "Playfield", "Rollover", "9-SW-2",  NULL },
    { 80, "Right Orbit",               "Playfield", "Rollover", "9-SW-3",  NULL },
    /* Cabinet / coin door — Node 1 */
    { 90, "Action Button",             "Cabinet",   "Leaf",     "1-SW-2",  NULL },
    { 93, "Start Button",              "Cabinet",   "Micro",    "1-SW-11", NULL },
    { 94, "Tournament Start Button",   "Cabinet",   "Micro",    "1-SW-12", NULL },
    { 96, "Left Coin",                 "CoinDoor",  "Micro",    "1-SW-16", NULL },
    { 97, "Right Coin",                "CoinDoor",  "Micro",    "1-SW-18", NULL },
    { 98, "Center Coin",               "CoinDoor",  "Micro",    "1-SW-17", NULL },
    { 99, "Fourth Coin",               "CoinDoor",  "Micro",    "1-SW-19", NULL },
    {100, "Fifth Coin",                "CoinDoor",  "Micro",    "1-SW-20", NULL },
    {101, "Tilt Pendulum",             "Cabinet",   "Plumb",    "1-SW-14", NULL },
    {102, "Sixth Coin",                "Cabinet",   "Micro",    "1-SW-21", NULL },
    {103, "Ticket Notch",              "Cabinet",   "Micro",    "1-SW-8",  NULL },
    {104, "Slam Tilt",                 "Cabinet",   "Micro",    "1-SW-22", NULL },
    {105, "Ticket Low",                "Cabinet",   "Micro",    "1-SW-7",  NULL },
    /* Coin-door service switches (ID prefixed with "C" in the manual) */
    {210, "Service Plus",              "CoinDoor",  "Micro",    "0-SW-9",  NULL },
    /* Note: the manual's C11 entry is truncated in the source we have;
     * when we have the full row, add {211, "Service", ...}. */
};

/* Lookup helper. Returns NULL if switch id isn't in the manifest. */
static const struct shim_switch_entry *shim_find_switch(int id) {
    size_t N = sizeof(shim_switch_manifest) / sizeof(shim_switch_manifest[0]);
    for (size_t i = 0; i < N; i++) {
        if (shim_switch_manifest[i].id == id) return &shim_switch_manifest[i];
    }
    return NULL;
}

/* ---- Step 52: fire_event trampoline + event sweep ---------------------
 *
 * BN Step 52 revealed:
 *   fire_event @ 0x9c12a0: uint64_t(int32_t event_id, int64_t event_data)
 *     - bounds-checks event_id <= 0xce
 *     - walks linked list at data_485ea80[event_id*8]
 *     - each node: {callback_fn_ptr @ +0, priority @ +0x08, next @ +0x10}
 *     - calls callback(event_data); short-circuits on !(ret & 1)
 *
 * Trampoline pattern:
 *   fire_event_addr = game_base + (0x9c12a0 - 0x400000)
 *
 * Event sweep fires every event_id 0..0xce with event_data=0 and diffs
 * cmode state before/after. Events whose handlers dereference event_data
 * as a pointer will SEGV with fault_addr=0; we catch and recover.
 *
 * Triggered by SIGUSR2 (kept out of the main dump path so we can still
 * use SIGUSR1 for state snapshots without firing events).
 * ------------------------------------------------------------------------ */

/* Sweep globals — read by shim_sigsegv_handler (in shim_diagnostics.c) to
 * longjmp back out of a faulting sweep iteration. Externed in spi_shim.h. */
sigjmp_buf shim_event_sweep_jmp;
volatile sig_atomic_t shim_event_fire_in_progress = 0;
volatile int shim_current_sweep_event_id = -1;

static uint64_t shim_fire_event(int32_t event_id, int64_t event_data) {
    uintptr_t game_base = shim_resolve_game_base();
    if (!game_base) return (uint64_t)-1;
    typedef uint64_t (*fire_event_t)(int32_t, int64_t);
    fire_event_t fn = (fire_event_t)(game_base + (0x9c12a0 - 0x400000));
    return fn(event_id, event_data);
}

/* Minimal cmode state snapshot. For each of the 42 cmodes captures
 * 2 u64 words: +0x308 (the field most likely to change on activation,
 * though it's subclass-reinterpreted) and +0x60 (zero at static init,
 * non-zero post-activation in some cmodes). 42×16 = 672 bytes. */
static void shim_capture_cmode_snap(uintptr_t game_base, uint64_t *out) {
    size_t N = shim_cmode_table_count;
    for (size_t i = 0; i < N; i++) {
        uintptr_t rt = game_base + (shim_cmode_table[i].bn_va - 0x400000);
        memcpy(&out[i*2 + 0], (void*)(rt + 0x308), 8);
        memcpy(&out[i*2 + 1], (void*)(rt + 0x60),  8);
    }
}

/* ---- Step 52: switch-closure API -------------------------------------
 *
 * shim_fire_switch(switch_id) simulates a physical switch closure at the
 * game's in-memory switch state array (data_4880220, per PROTOCOL.md).
 *
 * Current behaviour (Plan-C pending):
 *   1. Look up the switch in the manifest; refuse if not listed (manual
 *      enumerates all real switches; unlisted IDs are unused slots).
 *   2. Snapshot cmode state (+0x308/+0x60 across 42 cmodes).
 *   3. Write 1 to data_4880220 + id * 0x30 + 0x28.
 *   4. Re-snapshot; log deltas with switch name.
 *   5. After a short dwell, write 0 back (simulating release).
 *
 * What's missing until Plan C returns: the "switch decoder" — the
 * function that consumes UART cmd 0x27 bytes, writes into data_4880220,
 * AND calls fire_event(derived_event_id, struct_ptr). Without it, the
 * game sees the state byte flip but no event fires, so it may not react
 * until its own poll cycle notices. Once we find the decoder we can
 * either call it from shim_fire_switch or fire the right event ourselves.
 * ------------------------------------------------------------------------ */

#define SHIM_SWITCH_STATE_BASE 0x4880220   /* BN VA; stride 0x30, state @ +0x28 */
#define SHIM_SWITCH_COUNT_ADDR 0x48cbaa0   /* BN VA; dynamic switch count */

static int shim_fire_switch(int switch_id) {
    const struct shim_switch_entry *sw = shim_find_switch(switch_id);
    if (!sw) {
        char msg[128];
        int mn = snprintf(msg, sizeof(msg),
            "\n--- shim_fire_switch: id %d not in manifest (unused slot?) ---\n",
            switch_id);
        ssize_t u = write(STDERR_FILENO, msg, (size_t)mn); (void)u;
        return -1;
    }

    uintptr_t game_base = shim_resolve_game_base();
    if (!game_base) {
        const char *m = "\n--- shim_fire_switch: game_base unresolved ---\n";
        ssize_t u = write(STDERR_FILENO, m, strlen(m)); (void)u;
        return -1;
    }

    /* Bounds-check against the game's runtime switch count. */
    uint32_t sw_count = 0;
    memcpy(&sw_count,
           (void*)(game_base + (SHIM_SWITCH_COUNT_ADDR - 0x400000)),
           4);
    if (sw_count > 0 && (uint32_t)switch_id >= sw_count) {
        char msg[192];
        int mn = snprintf(msg, sizeof(msg),
            "\n--- shim_fire_switch: id %d (%s) out of range — runtime "
            "count is %u ---\n",
            switch_id, sw->name, sw_count);
        ssize_t u = write(STDERR_FILENO, msg, (size_t)mn); (void)u;
        return -1;
    }

    uintptr_t state_addr = game_base +
        (SHIM_SWITCH_STATE_BASE - 0x400000) + (uintptr_t)switch_id * 0x30 + 0x28;

    char hdr[256];
    int hn = snprintf(hdr, sizeof(hdr),
        "\n--- shim_fire_switch: %d '%s' (%s, %s, %s) state@0x%lx ---\n",
        switch_id, sw->name, sw->location, sw->type, sw->node_addr,
        (unsigned long)state_addr);
    ssize_t u = write(STDERR_FILENO, hdr, (size_t)hn); (void)u;

    uint64_t snap_before[42 * 2];
    uint64_t snap_after[42 * 2];
    shim_capture_cmode_snap(game_base, snap_before);

    uint8_t before_state = 0;
    memcpy(&before_state, (void*)state_addr, 1);

    /* Close the switch. */
    uint8_t closed = 1;
    memcpy((void*)state_addr, &closed, 1);

    /* Give the game a tick to notice. In practice the game's switch
     * poll runs off netbridge cmd 0x27 responses — so the shim-side
     * flip may go unnoticed until Plan C's translator hook is added.
     * We still sleep briefly in case any polling thread samples the
     * state array directly. */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 20 * 1000 * 1000 }; /* 20 ms */
    nanosleep(&ts, NULL);

    shim_capture_cmode_snap(game_base, snap_after);

    /* Release the switch. */
    memcpy((void*)state_addr, &before_state, 1);

    /* Report deltas. */
    size_t N = shim_cmode_table_count;
    char delta_buf[1800];
    size_t delta_off = 0;
    int delta_count = 0;
    for (size_t i = 0; i < N; i++) {
        if (snap_after[i*2 + 0] != snap_before[i*2 + 0] ||
            snap_after[i*2 + 1] != snap_before[i*2 + 1]) {
            delta_count++;
            if (delta_off < sizeof(delta_buf) - 128) {
                int added = snprintf(
                    delta_buf + delta_off, sizeof(delta_buf) - delta_off,
                    "  %s(+0x308:%lx->%lx)",
                    shim_cmode_table[i].name_hint,
                    (unsigned long)snap_before[i*2 + 0],
                    (unsigned long)snap_after[i*2 + 0]);
                delta_off += (size_t)added;
            }
        }
    }

    char tail[2048];
    int tn = snprintf(tail, sizeof(tail),
        "  switch %d '%s': before_state=%u Δ=%d%s\n",
        switch_id, sw->name, before_state, delta_count,
        delta_count > 0 ? delta_buf : " (no cmode state change — translator "
                                      "not yet hooked, see PROTOCOL.md)");
    u = write(STDERR_FILENO, tail, (size_t)tn); (void)u;

    return delta_count;
}

void shim_event_sweep(void) {
    uintptr_t game_base = shim_resolve_game_base();
    if (!game_base) {
        const char *m = "\n--- event sweep: game_base not resolved ---\n";
        ssize_t u = write(STDERR_FILENO, m, strlen(m)); (void)u;
        return;
    }

    const char *hdr =
        "\n--- event sweep: firing events 0x00..0xce with event_data=0 ---\n"
        "format:  event <id>  ret=<ret>  Δ=<#changed>[  <cmode>(+0x308: <before>-><after>)]...\n";
    ssize_t u = write(STDERR_FILENO, hdr, strlen(hdr)); (void)u;

    size_t N = shim_cmode_table_count;
    uint64_t baseline[42 * 2];
    uint64_t post[42 * 2];
    shim_capture_cmode_snap(game_base, baseline);

    int crashed_events = 0;
    int reactive_events = 0;
    int fired_ok = 0;

    int timed_out_events = 0;
    for (int ev = 0; ev <= 0xce; ev++) {
        shim_current_sweep_event_id = ev;
        shim_event_fire_in_progress = 1;

        uint64_t ret = (uint64_t)-1;
        int did_fire = 0;
        int setjmp_result;

        /* alarm(1) delivers SIGALRM to this thread (blocked elsewhere by the
         * sigmask set on the timer thread). SIGALRM handler recognises
         * in_progress and siglongjmps back with value 2. */
        alarm(1);
        setjmp_result = sigsetjmp(shim_event_sweep_jmp, 1);
        if (setjmp_result == 0) {
            ret = shim_fire_event(ev, 0);
            alarm(0);
            did_fire = 1;
            fired_ok++;
        } else if (setjmp_result == 2) {
            alarm(0);
            timed_out_events++;
        } else {
            /* setjmp_result == 1 — came from SEGV */
            alarm(0);
            crashed_events++;
        }
        shim_event_fire_in_progress = 0;

        if (did_fire) {
            shim_capture_cmode_snap(game_base, post);

            char delta_buf[1800];
            size_t delta_off = 0;
            int delta_count = 0;
            for (size_t i = 0; i < N; i++) {
                if (post[i*2 + 0] != baseline[i*2 + 0] ||
                    post[i*2 + 1] != baseline[i*2 + 1]) {
                    delta_count++;
                    if (delta_off < sizeof(delta_buf) - 128) {
                        int added = snprintf(
                            delta_buf + delta_off,
                            sizeof(delta_buf) - delta_off,
                            "  %s(+0x308:%lx->%lx)",
                            shim_cmode_table[i].name_hint,
                            (unsigned long)baseline[i*2 + 0],
                            (unsigned long)post[i*2 + 0]);
                        delta_off += (size_t)added;
                    }
                }
            }

            char line[2048];
            int ln;
            if (delta_count > 0) {
                reactive_events++;
                ln = snprintf(line, sizeof(line),
                    "  event 0x%02x ret=%lu Δ=%d %s\n",
                    ev, (unsigned long)ret, delta_count, delta_buf);
                /* Use post as new baseline for cumulative tracking */
                memcpy(baseline, post, sizeof(baseline));
            } else {
                ln = snprintf(line, sizeof(line),
                    "  event 0x%02x ret=%lu Δ=0\n",
                    ev, (unsigned long)ret);
            }
            u = write(STDERR_FILENO, line, (size_t)ln); (void)u;
        }
    }

    char sum[220];
    int sl = snprintf(sum, sizeof(sum),
        "\n--- sweep complete: fired=%d reactive=%d crashed=%d timeouts=%d (of 207) ---\n",
        fired_ok, reactive_events, crashed_events, timed_out_events);
    u = write(STDERR_FILENO, sum, (size_t)sl); (void)u;
}



/* SPIKE3_POKE_CMODE=<name>:<hex_value>:<delay_sec> — force a cmode's +0x308
 * field to <hex_value> after <delay_sec> seconds, then reset to 0 after
 * another 3 s. Lets us simulate mode activations without needing the
 * game's Stage-2 event logic to agree. Name matches shim_cmode_table.name_hint
 * (e.g. "story_charmander_1", "battle_coast"). */
static void *shim_poke_cmode_thread(void *arg) {
    char *spec = (char*)arg;
    if (!spec) return NULL;

    char *colon1 = strchr(spec, ':');
    if (!colon1) { free(spec); return NULL; }
    *colon1 = 0;
    char *name = spec;
    char *rest = colon1 + 1;
    char *colon2 = strchr(rest, ':');
    uint64_t value = 0;
    long delay = 35;
    if (colon2) {
        *colon2 = 0;
        value = strtoull(rest, NULL, 0);
        delay = atol(colon2 + 1);
    } else {
        value = strtoull(rest, NULL, 0);
    }

    /* Look up cmode. */
    size_t N = shim_cmode_table_count;
    const struct shim_cmode_entry *entry = NULL;
    for (size_t i = 0; i < N; i++) {
        if (strcmp(shim_cmode_table[i].name_hint, name) == 0) {
            entry = &shim_cmode_table[i];
            break;
        }
    }
    if (!entry) {
        SHIM_LOG("[poke_cmode] unknown cmode name: %s", name);
        free(spec);
        return NULL;
    }

    SHIM_LOG("[poke_cmode] will poke %s +0x308 = 0x%lx after %ld s",
             name, (unsigned long)value, delay);

    sleep((unsigned int)delay);

    uintptr_t game_base = shim_resolve_game_base();
    if (!game_base) {
        SHIM_LOG("[poke_cmode] game_base unresolved; aborting");
        free(spec);
        return NULL;
    }

    uintptr_t addr = game_base + (entry->bn_va - 0x400000) + 0x308;
    SHIM_LOG("[poke_cmode] writing 0x%lx to %s @ rt 0x%lx (activation)",
             (unsigned long)value, name, (unsigned long)addr);
    memcpy((void*)addr, &value, 8);

    /* Hold for 3 seconds so the bridge has time to emit + subscriber reacts */
    sleep(3);

    uint64_t zero = 0;
    SHIM_LOG("[poke_cmode] writing 0 to %s @ rt 0x%lx (deactivation)",
             name, (unsigned long)addr);
    memcpy((void*)addr, &zero, 8);

    free(spec);
    return NULL;
}

/* Step 60: direct poke of system_state_flags bit 0x10.
 *
 * BN entry [85] established that the main loop at 0xb2aba0 gates downstream
 * progression on `system_state_flags bit 0x10` being set AND elapsed time in
 * [599 ms, 3599 ms]. The two legitimate writers (sub_a61380 / sub_a61040)
 * only set the bit on fire_event(0x46|0x4d) success, which means firing those
 * events via our shim_fire_event trampoline won't set the bit — only the game
 * internally calling sub_a61380 can do that.
 *
 * This thread brute-forces the experiment: after a configurable delay into
 * the timing window, atomically OR the bit in directly. Three outcomes per
 * the journal Step-60 plan:
 *   - boot-splash advances  → bit was the gate, find the legitimate trigger
 *   - no change             → bit is part of conjunction with other state
 *   - crash                 → downstream code needs more setup than just the bit
 *
 * Spec: SPIKE3_POKE_STATE_FLAG=<hex_or_dec_bit_mask>:<delay_ms>
 *   default mask  = 0x10
 *   default delay = 1500 ms (solidly inside the [599, 3599] ms window).
 * Example: SPIKE3_POKE_STATE_FLAG=0x10:1500 */
#define SYSTEM_STATE_FLAGS_BN_VA 0x4882388

static void *shim_poke_state_flag_thread(void *arg) {
    char *spec = (char*)arg;
    unsigned mask = 0x10;
    long delay_ms = 1500;
    if (spec && *spec) {
        char *copy = spec;
        char *colon = strchr(copy, ':');
        if (colon) {
            *colon = 0;
            if (copy[0]) mask = (unsigned)strtoul(copy, NULL, 0);
            delay_ms = atol(colon + 1);
        } else {
            /* bare value = mask only, keep default delay */
            mask = (unsigned)strtoul(copy, NULL, 0);
        }
    }
    SHIM_LOG("[poke_state_flag] will OR 0x%x into system_state_flags @ 0x%x after %ld ms",
             mask, SYSTEM_STATE_FLAGS_BN_VA, delay_ms);

    if (delay_ms > 0) {
        struct timespec ts = {
            .tv_sec  = delay_ms / 1000,
            .tv_nsec = (delay_ms % 1000) * 1000000L,
        };
        nanosleep(&ts, NULL);
    }

    uintptr_t game_base = shim_resolve_game_base();
    if (!game_base) {
        SHIM_LOG("[poke_state_flag] game_base unresolved; aborting");
        free(spec);
        return NULL;
    }
    volatile uint16_t *flag_ptr =
        (volatile uint16_t *)(game_base + (SYSTEM_STATE_FLAGS_BN_VA - 0x400000));
    uint16_t before = *flag_ptr;
    __atomic_or_fetch(flag_ptr, (uint16_t)mask, __ATOMIC_SEQ_CST);
    uint16_t after = *flag_ptr;
    SHIM_LOG("[poke_state_flag] @ rt %p: 0x%04x → 0x%04x (mask 0x%x applied)",
             (void*)flag_ptr, before, after, mask);

    free(spec);
    return NULL;
}

/* ---- Step 62: SpiVideoStreamDecoderBase::Play vtable patch ----
 *
 * BN analysis: vtable at 0x46cc9c8 (22 slots, 384 bytes), slot +0x28 is Play
 * at sub_d24360. Play polls *(this+8) [update_thread_running flag] every 1ms
 * for up to 250 iterations. The flag is set by DecodeFrame (sub_d227d0) only
 * after avcodec_receive_frame successfully decodes a frame. In our shim the
 * V4L2 HEVC hwaccel is unavailable so decoder always returns EAGAIN/EOF →
 * flag never set → Play's 250ms timeout fires, prints the ****** banner,
 * returns 1 (success) anyway. Queue at *(this+0xa0) stays empty. Downstream
 * consumer gets nothing, state wedges, eventual pure-virtual-method abort
 * during the VideoPlayerSpi destructor chain.
 *
 * Fix: vtable-patch slot +0x28 to a function that immediately sets
 * update_thread_running=1 and state=PLAYING, then returns 1. Skips the
 * FFmpeg wait entirely. Gated by SPIKE3_PATCH_SPI_VIDEO=1. Three expected
 * outcomes per HANDOFF.md: (a) game advances to a new specific failure,
 * (b) game reaches attract with missing video (still useful), (c) same
 * abort — meaning it's a dtor-order bug not play-timeout.
 *
 * Object layout for Play's caller args (see reference_video_player_spi.md):
 *   *(self+0x08)  u8   update_thread_running (the flag)
 *   *(self+0x179) u8   loop_flag
 *   *(self+0x17c) u32  start_frame (-1 = from beginning)
 *   *(self+0x180) u64  seek_pending
 *   *(self+0x18c) u32  decoder state (2=PLAYING)
 */
#define SPI_DECODER_VTABLE_BN_VA   0x46cc9c8
#define SPI_DECODER_PLAY_SLOT      0x28

static int64_t shim_fake_spi_play(void *self, char loop_flag, int32_t start_frame) {
    /* Preserve real Play's state-guard: if (state & 0xfffffffd) != 1 it returns
     * 0 unchanged. Keeps us from unblocking a genuinely-errored Play path. */
    int32_t state = *(uint32_t*)((char*)self + 0x18c);
    if ((state & 0xfffffffd) != 1) return 0;

    *(uint8_t  *)((char*)self + 0x179) = loop_flag;
    if (start_frame != -1)
        *(uint32_t*)((char*)self + 0x17c) = start_frame;
    *(uint64_t *)((char*)self + 0x180) = 0;      /* clear seek_pending */
    *(uint32_t *)((char*)self + 0x18c) = 2;      /* PLAYING */
    __atomic_store_n((uint8_t*)((char*)self + 8), 1, __ATOMIC_SEQ_CST);
    SHIM_LOG("[vp] fake_spi_play(self=%p, loop=%d, start=%d) -> 1 [skipped FFmpeg]",
             self, (int)loop_flag, start_frame);
    return 1;
}

static void shim_patch_spi_video_vtable(void) {
    if (!is_real_gl()) return;
    const char *en = getenv("SPIKE3_PATCH_SPI_VIDEO");
    if (!en || en[0] != '1') return;

    uintptr_t game_base = shim_resolve_game_base();
    if (!game_base) {
        SHIM_LOG("[vp] game_base unresolved; patch aborted");
        return;
    }

    void **slot = (void**)(game_base +
        (SPI_DECODER_VTABLE_BN_VA - 0x400000) + SPI_DECODER_PLAY_SLOT);
    uintptr_t page = (uintptr_t)slot & ~(uintptr_t)4095;
    /* Spans two pages in case the slot is near a page boundary. */
    if (mprotect((void*)page, 8192, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        SHIM_LOG("[vp] mprotect RWX failed: %s", strerror(errno));
        return;
    }
    void *old = *slot;
    *slot = (void*)shim_fake_spi_play;
    SHIM_LOG("[vp] patched SpiDecoder::Play vtable slot @ %p: %p -> %p",
             (void*)slot, old, (void*)shim_fake_spi_play);
}

/* ---- Step 64: software HEVC path ----
 *
 * Two pieces: (1) swap the decoder name "hevc_v4l2request" → "hevc" at
 * avcodec_find_decoder_by_name; handled in that hook's body. (2) NOP the
 * b.ne in the game's get_format callback so it accepts the first pix_fmt
 * libavcodec offers, instead of insisting on DRM_PRIME (0xb3).
 *
 * Callback layout at BN VA 0x920cd0 (extracted via objdump):
 *   920cd0  ldr  w0, [x1]          ; fmt = list[0]
 *   920cd4  cmn  w0, #1             ; AV_PIX_FMT_NONE?
 *   920cd8  b.ne 0x920cec           ; not sentinel → compare
 *   920cdc  b    0x920cf8           ; empty list → error
 *   920ce0  ldr  w0, [x1, #4]!      ; LOOP: advance, load
 *   920ce4  cmn  w0, #1
 *   920ce8  b.eq 0x920cf8           ; end → error
 *   920cec  cmp  w0, #0xb3          ; DRM_PRIME?
 *   920cf0  b.ne 0x920ce0           ; no → loop
 *   920cf4  ret                     ; yes → return 0xb3
 *
 * Patching 920cf0 to NOP makes the callback always fall through to `ret`
 * with w0 holding whatever format was just loaded — i.e. the first
 * non-sentinel entry. For software hevc that's YUV420P (0) or NV12 (23).
 *
 * Gated by SPIKE3_SW_HEVC=1. When paired with the name-swap above it should
 * take software decode end-to-end until Radium's downstream code (which
 * expects DRM_PRIME descriptors in AVFrame->data[0]) hits a new wall.
 */
/* BN VA = ELF VA + 0x400000; objdump showed the b.ne at ELF VA 0x920cf0,
 * so BN VA = 0xd20cf0. The shim's patch formula subtracts 0x400000 to
 * recover ELF VA before adding game_base (same convention as Step 62's
 * vtable patch at 0x46cc9c8). */
#define GET_FORMAT_BNE_BN_VA   0xd20cf0
#define AARCH64_NOP            0xd503201fu

static int shim_sw_hevc_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *s = getenv("SPIKE3_SW_HEVC");
        cached = (s && s[0] == '1') ? 1 : 0;
    }
    return cached;
}

static void shim_patch_get_format_callback(void) {
    if (!shim_sw_hevc_enabled()) return;

    uintptr_t game_base = shim_resolve_game_base();
    if (!game_base) {
        SHIM_LOG("[sw_hevc] game_base unresolved; get_format patch aborted");
        return;
    }

    uint32_t *instr = (uint32_t *)(game_base +
        (GET_FORMAT_BNE_BN_VA - 0x400000));
    uintptr_t page = (uintptr_t)instr & ~(uintptr_t)4095;
    if (mprotect((void *)page, 8192,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        SHIM_LOG("[sw_hevc] mprotect RWX @ %p failed: %s",
                 (void *)page, strerror(errno));
        return;
    }

    uint32_t old = *instr;
    /* Safety: expect `b.ne 0x920ce0` (backward 3 instructions) encoded as
     * 0x54ffff81. Refuse to patch if we see anything else — prior bug had
     * us subtracting 0x400000 twice (objdump VAs ARE BN VAs) and writing
     * a NOP over an innocent STP in an unrelated function. */
    if (old != 0x54ffff81u) {
        SHIM_LOG("[sw_hevc] REFUSED to patch @ %p: expected 0x54ffff81 (b.ne), got 0x%08x",
                 (void *)instr, old);
        return;
    }
    *instr = AARCH64_NOP;
    __builtin___clear_cache((char *)instr, (char *)instr + 4);
    SHIM_LOG("[sw_hevc] patched get_format b.ne @ %p: 0x%08x -> 0x%08x (NOP)",
             (void *)instr, old, (unsigned)AARCH64_NOP);
}

static void *shim_vp_patch_thread(void *arg) {
    (void)arg;
    /* Wait for game's C++ static initializers to complete before patching.
     * 2 seconds is generous; could shrink later once we know it's stable. */
    sleep(2);
    shim_patch_spi_video_vtable();
    shim_patch_get_format_callback();
    return NULL;
}




/* Step 52: SIGALRM handler used to break out of hung event handlers during
 * the sweep. Installed in the constructor; blocked on all threads except
 * the sweep timer thread so alarm() delivers to the right place. */
static void shim_event_sweep_alarm_handler(int sig, siginfo_t *info, void *uc) {
    (void)info; (void)uc;
    if (shim_event_fire_in_progress) {
        char msg[160];
        int n = snprintf(msg, sizeof(msg),
            "\n  [sweep] event 0x%02x TIMEOUT (sig=%d) — recovering\n",
            shim_current_sweep_event_id, sig);
        ssize_t u = write(STDERR_FILENO, msg, (size_t)n); (void)u;
        shim_event_fire_in_progress = 0;
        siglongjmp(shim_event_sweep_jmp, 2);
    }
    /* else: unexpected SIGALRM outside the sweep — just ignore */
}

/* Step 52: SIGUSR2 gets intercepted by QEMU user-mode itself, so we can't
 * rely on external signals to trigger action from outside. Instead we use
 * env-var-driven timer threads:
 *
 *   SPIKE3_SWEEP_AFTER=<sec>      → run shim_event_sweep() after N seconds
 *                                    (requires SPIKE3_SWEEP_EVENTS=1 opt-in)
 *   SPIKE3_FIRE_SWITCH=<id[,id]>  → run shim_fire_switch() on each id after
 *                                    <SPIKE3_SWITCH_DELAY or 30> seconds
 *   SPIKE3_SWITCH_DELAY=<sec>     → delay before fire_switch calls (default 30)
 */
static void *shim_sweep_timer_thread(void *arg) {
    long seconds = (long)arg;
    if (seconds <= 0) return NULL;

    /* Unblock SIGALRM on this thread only — the constructor blocked it on
     * the main thread (so all threads created by the game inherit that
     * mask), leaving this thread as the sole recipient of alarm() signals.
     * Keeps siglongjmp-on-timeout well-defined (same thread as sigsetjmp
     * in shim_event_sweep). */
    sigset_t unblock;
    sigemptyset(&unblock);
    sigaddset(&unblock, SIGALRM);
    pthread_sigmask(SIG_UNBLOCK, &unblock, NULL);

    char msg[128];
    int mn = snprintf(msg, sizeof(msg),
        "\n=== shim_sweep_timer: sleeping %ld seconds then firing sweep ===\n",
        seconds);
    ssize_t u = write(STDERR_FILENO, msg, (size_t)mn); (void)u;

    sleep((unsigned int)seconds);

    const char *m = "\n=== shim_sweep_timer: firing sweep NOW ===\n";
    u = write(STDERR_FILENO, m, strlen(m)); (void)u;

    shim_event_sweep();
    return NULL;
}

/* SPIKE3_FORCE_GSTREAMER=<seconds> — after N seconds, trampoline into the
 * game's gstreamer_init function at BN VA 0xcc1ec0. Diagnostic: tells us
 * whether gstreamer is reachable + functional when explicitly called.
 * If our gst_init_check hook fires afterwards, gstreamer is callable and
 * the dormancy we observed is purely "the game never chooses to call it
 * under current conditions" (hardware-gated, config-gated, etc.). */
static void *shim_force_gstreamer_thread(void *arg) {
    long seconds = (long)arg;
    if (seconds <= 0) return NULL;

    char msg[160];
    int mn = snprintf(msg, sizeof(msg),
        "\n=== shim_force_gstreamer: sleeping %ld s then calling game's gstreamer_init @ 0xcc1ec0 ===\n",
        seconds);
    ssize_t u = write(STDERR_FILENO, msg, (size_t)mn); (void)u;

    sleep((unsigned int)seconds);

    uintptr_t game_base = shim_resolve_game_base();
    if (!game_base) {
        const char *m = "[shim] force_gstreamer: game_base unresolved; aborting\n";
        u = write(STDERR_FILENO, m, strlen(m)); (void)u;
        return NULL;
    }

    typedef void (*gstreamer_init_t)(void);
    uintptr_t addr = game_base + (0xcc1ec0 - 0x400000);
    gstreamer_init_t gstreamer_init_fn = (gstreamer_init_t)addr;

    char hdr[200];
    int hn = snprintf(hdr, sizeof(hdr),
        "[shim] force_gstreamer: calling gstreamer_init via trampoline @ %p  (game_base=0x%lx)\n",
        (void*)addr, game_base);
    u = write(STDERR_FILENO, hdr, (size_t)hn); (void)u;

    gstreamer_init_fn();

    const char *done = "[shim] force_gstreamer: gstreamer_init returned\n";
    u = write(STDERR_FILENO, done, strlen(done)); (void)u;
    return NULL;
}

/* SPIKE3_FIRE_SWITCH=<id1:id2:...> — fire one or more switches after
 * SPIKE3_SWITCH_DELAY seconds (default 30).
 *
 * NOTE: colon-delimited, not comma — QEMU's `-E var=value` uses comma as
 * the var separator (discovered via Step 46 V4L2 probe). If you invoke
 * the shim directly (not via QEMU), commas also work.
 *
 * Each ID is spaced by ~200 ms to let the game's poll cycle breathe. */
static void *shim_fire_switch_thread(void *arg) {
    char *list_copy = (char*)arg;
    if (!list_copy || !list_copy[0]) { free(list_copy); return NULL; }

    const char *delay_s = getenv("SPIKE3_SWITCH_DELAY");
    long delay = delay_s && delay_s[0] ? atol(delay_s) : 30;
    if (delay < 0) delay = 0;
    if (delay > 3600) delay = 3600;

    char msg[200];
    int mn = snprintf(msg, sizeof(msg),
        "\n=== shim_fire_switch_thread: sleeping %ld s then firing switches [%s] ===\n",
        delay, list_copy);
    ssize_t u = write(STDERR_FILENO, msg, (size_t)mn); (void)u;

    sleep((unsigned int)delay);

    /* Parse colon-or-comma-separated IDs (":," covers both for safety). */
    char *save = NULL;
    char *tok = strtok_r(list_copy, ":,", &save);
    while (tok) {
        while (*tok == ' ') tok++;
        int id = atoi(tok);
        if (id > 0) {
            shim_fire_switch(id);
            /* short gap between fires so the game's poll can observe
             * one state transition at a time */
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 200*1000*1000 };
            nanosleep(&ts, NULL);
        }
        tok = strtok_r(NULL, ":,", &save);
    }

    free(list_copy);
    return NULL;
}

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

    /* Step 52: SIGALRM handler for per-event timeout during sweep. */
    struct sigaction sa3;
    memset(&sa3, 0, sizeof(sa3));
    sa3.sa_sigaction = shim_event_sweep_alarm_handler;
    sa3.sa_flags = SA_SIGINFO;
    sigemptyset(&sa3.sa_mask);
    sigaction(SIGALRM, &sa3, NULL);

    /* Step 52: block SIGALRM on this (main) thread. Threads created later
     * by the game inherit this mask, so SIGALRM won't disturb anything
     * other than the sweep timer thread (which unblocks SIGALRM on itself
     * before running the sweep). */
    sigset_t block_alarm;
    sigemptyset(&block_alarm);
    sigaddset(&block_alarm, SIGALRM);
    pthread_sigmask(SIG_BLOCK, &block_alarm, NULL);

    /* Step 52: env-var-triggered delayed sweep.
     * Opt-in — only runs when SPIKE3_SWEEP_EVENTS=1 is also set, to keep
     * default runs on the switch-first path (see EVENT_SWEEP_STEP52.md
     * for why the blind sweep is low-signal). */
    const char *sweep_enabled = getenv("SPIKE3_SWEEP_EVENTS");
    if (sweep_enabled && sweep_enabled[0] == '1') {
        const char *s = getenv("SPIKE3_SWEEP_AFTER");
        if (s && s[0]) {
            long sec = atol(s);
            if (sec > 0 && sec < 3600) {
                pthread_t t;
                if (pthread_create(&t, NULL, shim_sweep_timer_thread,
                                   (void*)(intptr_t)sec) == 0) {
                    pthread_detach(t);
                }
            }
        }
    }

    /* Step 52: env-var-triggered targeted switch firing. */
    const char *fs = getenv("SPIKE3_FIRE_SWITCH");
    if (fs && fs[0]) {
        char *copy = strdup(fs);
        if (copy) {
            pthread_t t;
            if (pthread_create(&t, NULL, shim_fire_switch_thread, copy) == 0) {
                pthread_detach(t);
            } else {
                free(copy);
            }
        }
    }

    /* Step 53: env-var-triggered gstreamer init force-call (diagnostic). */
    const char *fg = getenv("SPIKE3_FORCE_GSTREAMER");
    if (fg && fg[0]) {
        long sec = atol(fg);
        if (sec > 0 && sec < 3600) {
            pthread_t t;
            if (pthread_create(&t, NULL, shim_force_gstreamer_thread,
                               (void*)(intptr_t)sec) == 0) {
                pthread_detach(t);
            }
        }
    }

    /* Step 54: optional state bridge over Unix socket. */
    shim_bridge_start();

    /* Step 58: optional frame bridge (glReadPixels → AF_UNIX stream). */
    shim_frame_start();

    /* Step 54: optional cmode poke (force activation for testing). */
    const char *pc = getenv("SPIKE3_POKE_CMODE");
    if (pc && pc[0]) {
        char *copy = strdup(pc);
        if (copy) {
            pthread_t t;
            if (pthread_create(&t, NULL, shim_poke_cmode_thread, copy) == 0) {
                pthread_detach(t);
            } else {
                free(copy);
            }
        }
    }

    /* Step 60: optional system_state_flags bit poke (force boot-splash gate). */
    const char *psf = getenv("SPIKE3_POKE_STATE_FLAG");
    if (psf && psf[0]) {
        /* Accept "1" as shorthand for "use defaults (bit 0x10, 1500ms)". */
        char *copy = strdup(psf[0] == '1' && psf[1] == 0 ? "" : psf);
        if (copy) {
            pthread_t t;
            if (pthread_create(&t, NULL, shim_poke_state_flag_thread, copy) == 0) {
                pthread_detach(t);
            } else {
                free(copy);
            }
        }
    }

    /* Step 62: optional SpiDecoder::Play vtable patch — bypasses FFmpeg wait. */
    {
        const char *pv = getenv("SPIKE3_PATCH_SPI_VIDEO");
        if (pv && pv[0] == '1') {
            pthread_t t;
            if (pthread_create(&t, NULL, shim_vp_patch_thread, NULL) == 0)
                pthread_detach(t);
        }
    }
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
 * `_emulation/audio/stream.s16le`. Playback / conversion:
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
            alsa_capture_file = fopen("_emulation/audio/stream.s16le", "ab");
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



/* ---- memcpy NULL-guard (Step 45) ----
 *
 * Godot's error-logger path on the custom Stern/Radium DisplayRenderer
 * thread hits vformat()/memcpy(src=NULL, …) because Godot TLS singletons
 * (StringName pool, memory arena, OS::get_singleton) are unregistered on
 * that thread. The resulting NULL dereference inside aarch64 __memcpy_aarch64
 * (cpyfp) has been the Step-31 blocker since 2026-04-19.
 *
 * This override: when either pointer is NULL, silently no-op instead of
 * faulting. Real memcpy goes through dlsym(RTLD_NEXT, "memcpy") so hot-path
 * copies retain libc's SIMD-optimised implementation.
 *
 * Safety:
 *   - Thread-local `in_memcpy` guard breaks any recursion (e.g. if SHIM_LOG's
 *     internal printf happens to call back into memcpy).
 *   - Byte-copy fallback if dlsym hasn't resolved yet (early ctor) or fails.
 *   - SHIM_LOG is called inside the non-null path; that's fine because we've
 *     already guarded the in_memcpy recursion flag.
 *   - Counts are atomics so concurrent threads don't corrupt the tally.
 */
static volatile unsigned long shim_memcpy_null_count = 0;
static void *(*real_memcpy)(void *, const void *, size_t) = NULL;

void *memcpy(void *dst, const void *src, size_t n) {
    static __thread int in_memcpy = 0;

    if (!dst || !src) {
        unsigned long n_hit = __atomic_add_fetch(&shim_memcpy_null_count, 1,
                                                  __ATOMIC_RELAXED);
        /* Log first 8, then every 1000th — avoids flooding if this fires
         * per-frame. Log OUTSIDE the in_memcpy recursion guard is fine
         * because we're returning before any real work. */
        if (n_hit <= 8 || (n_hit % 1000) == 0) {
            if (!in_memcpy) {
                in_memcpy = 1;
                SHIM_LOG("memcpy NULL-guard #%lu: dst=%p src=%p n=%zu",
                         n_hit, dst, src, n);
                in_memcpy = 0;
            }
        }
        return dst;
    }

    if (in_memcpy) {
        /* Recursion break — copy byte-by-byte. Used if dlsym indirectly
         * re-enters memcpy during its own resolution path. */
        unsigned char *d = dst;
        const unsigned char *s = src;
        for (size_t i = 0; i < n; i++) d[i] = s[i];
        return dst;
    }

    in_memcpy = 1;
    if (!real_memcpy) {
        real_memcpy = dlsym(RTLD_NEXT, "memcpy");
    }
    void *ret;
    if (real_memcpy) {
        ret = real_memcpy(dst, src, n);
    } else {
        unsigned char *d = dst;
        const unsigned char *s = src;
        for (size_t i = 0; i < n; i++) d[i] = s[i];
        ret = dst;
    }
    in_memcpy = 0;
    return ret;
}

/* ---- libavcodec HW-accel bypass (Step 47) ----
 *
 * The game's spi_video_player uses libavcodec to decode HEVC. Its init
 * sequence (per `nm -D --undefined-only game`) calls:
 *   avcodec_find_decoder_by_name  → looks up the decoder
 *   avcodec_get_hw_config         → probes HW configs
 *   av_hwdevice_ctx_create        → creates a DRM/V4L2 device context
 *   avcodec_open2                 → opens the codec
 *
 * On Spike 3 hardware this resolves to "hevc_v4l2request" + drm_prime.
 * Under QEMU user-mode we have no real V4L2 HEVC decoder, so hwaccel init
 * fails (Step 46) and `Error parsing NAL unit #0` follows — the game's
 * render path doesn't get usable frames. Rather than emulate V4L2 fully,
 * we intercept here and force the plain software HEVC decoder:
 *
 *   - avcodec_find_decoder_by_name("hevc_v4l2request") → delegate to "hevc"
 *   - avcodec_get_hw_config(codec, idx) → always NULL  (no HW configs)
 *   - av_hwdevice_ctx_create(..., DRM, …) → AVERROR(ENOSYS)
 *
 * If the game's video module has a SW fallback path, it'll now take it and
 * libavcodec's software HEVC decoder produces frames into an AVFrame the
 * game uploads as a GL texture. No dummy frames needed — real decode.
 *
 * Risk: if the game hard-codes hwaccel (no fallback) these hooks don't
 * produce video, but they at least make the failure deterministic. We'll
 * see in the next run.
 */

typedef const void *(*real_avcodec_find_decoder_by_name_t)(const char *name);
typedef const void *(*real_avcodec_get_hw_config_t)(const void *codec, int index);
typedef int (*real_av_hwdevice_ctx_create_t)(void **device_ctx, int type,
                                              const char *device, void *opts, int flags);
typedef int (*real_avcodec_send_packet_t)(void *avctx, const void *pkt);
typedef int (*real_avcodec_receive_frame_t)(void *avctx, void *frame);

static real_avcodec_find_decoder_by_name_t real_avcodec_find_decoder_by_name = NULL;
static real_avcodec_get_hw_config_t real_avcodec_get_hw_config = NULL;
static real_av_hwdevice_ctx_create_t real_av_hwdevice_ctx_create = NULL;
static real_avcodec_send_packet_t real_avcodec_send_packet = NULL;
static real_avcodec_receive_frame_t real_avcodec_receive_frame = NULL;

static unsigned int av_send_count = 0;
static unsigned int av_recv_count = 0;
static unsigned int av_recv_err_count = 0;
static unsigned int av_recv_ok_count = 0;

/* Helper — resolve via dlvsym so versioned symbols work.
 * The game imports avcodec_find_decoder_by_name@LIBAVCODEC_59 etc.; plain
 * dlsym(RTLD_NEXT, ...) can miss versioned symbols in some ld configs, so we
 * try both. Also try dlopen(libavcodec.so.59) as a last resort. */
static void *shim_resolve_libav(const char *sym, const char *version) {
    void *p = dlvsym(RTLD_NEXT, sym, version);
    if (p) return p;
    p = dlsym(RTLD_NEXT, sym);
    if (p) return p;
    /* Try explicit lib handle */
    static void *h_avcodec = NULL, *h_avutil = NULL;
    void **h = NULL;
    if (strstr(sym, "avcodec_")) h = &h_avcodec;
    else if (strstr(sym, "av_hwdevice_") || strstr(sym, "av_frame_")) h = &h_avutil;
    if (h) {
        if (!*h) {
            *h = dlopen(strstr(sym, "avcodec_") ? "libavcodec.so.59" : "libavutil.so.57",
                        RTLD_LAZY | RTLD_NOLOAD);
        }
        if (*h) p = dlsym(*h, sym);
    }
    return p;
}

const void *avcodec_find_decoder_by_name(const char *name) {
    SHIM_LOG("avcodec_find_decoder_by_name HOOK ENTERED: name='%s'",
             name ? name : "(null)");
    if (!real_avcodec_find_decoder_by_name) {
        real_avcodec_find_decoder_by_name =
            (real_avcodec_find_decoder_by_name_t)shim_resolve_libav(
                "avcodec_find_decoder_by_name", "LIBAVCODEC_59");
        SHIM_LOG("  dlvsym resolved real_avcodec_find_decoder_by_name = %p",
                 (void *)real_avcodec_find_decoder_by_name);
    }
    if (!real_avcodec_find_decoder_by_name) return NULL;

    /* Step 64 (SPIKE3_SW_HEVC=1): redirect hardware HEVC decoder names to
     * libavcodec's software "hevc". Paired with the get_format-callback
     * NOP patch in shim_patch_get_format_callback. */
    const char *lookup = name;
    if (shim_sw_hevc_enabled() && name &&
        (strcmp(name, "hevc_v4l2request") == 0 ||
         strcmp(name, "hevc_v4l2m2m")    == 0)) {
        SHIM_LOG("[sw_hevc] remapping decoder name '%s' -> 'hevc'", name);
        lookup = "hevc";
    }

    const void *ret = real_avcodec_find_decoder_by_name(lookup);
    SHIM_LOG("  -> %p (name='%s', lookup='%s')", ret,
             name ? name : "(null)", lookup);
    return ret;
}

/* Get_hw_config, hwdevice_ctx_create, send_packet, receive_frame — all
 * REVERTED to passthrough for now. First goal is to confirm the hook itself
 * fires and we can see the call pattern. Once we see avcodec_find_decoder's
 * name, avcodec_open2's return, and the codec ptr flow, we can design the
 * intervention. */
const void *avcodec_get_hw_config(const void *codec, int index) {
    if (!real_avcodec_get_hw_config) {
        real_avcodec_get_hw_config =
            (real_avcodec_get_hw_config_t)shim_resolve_libav(
                "avcodec_get_hw_config", "LIBAVCODEC_59");
    }
    if (!real_avcodec_get_hw_config) return NULL;
    const void *ret = real_avcodec_get_hw_config(codec, index);
    if (index < 4) {
        SHIM_LOG("avcodec_get_hw_config(codec=%p, index=%d) -> %p (passthrough)",
                 codec, index, ret);
    }
    return ret;
}

int av_hwdevice_ctx_create(void **device_ctx, int type, const char *device,
                            void *opts, int flags) {
    if (!real_av_hwdevice_ctx_create) {
        real_av_hwdevice_ctx_create =
            (real_av_hwdevice_ctx_create_t)shim_resolve_libav(
                "av_hwdevice_ctx_create", "LIBAVUTIL_57");
    }
    if (!real_av_hwdevice_ctx_create) {
        SHIM_LOG("av_hwdevice_ctx_create: no real symbol, returning ENOSYS");
        if (device_ctx) *device_ctx = NULL;
        return -38;
    }
    int ret = real_av_hwdevice_ctx_create(device_ctx, type, device, opts, flags);
    SHIM_LOG("av_hwdevice_ctx_create(type=%d, device=%s, flags=%d) -> %d (passthrough)",
             type, device ? device : "(null)", flags, ret);
    return ret;
}

/* ---- Dummy-frame injection (Step 47b) ----
 *
 * DRM-based HW decode is being set up successfully (av_hwdevice_ctx_create
 * returned 0 for AV_HWDEVICE_TYPE_DRM=8), packets are being sent to the
 * codec, but receive_frame always returns EAGAIN (-11) — no real frames
 * because our DRM socketpair doesn't actually decode. Inject a dummy
 * NV12 1920×1080 all-black frame so the game's video renderer gets SOMETHING.
 *
 * AVFrame field offsets (libavutil 57 / ffmpeg 5.1, stable):
 *   width:   104   int
 *   height:  108   int
 *   format:  116   int  (AV_PIX_FMT_NV12 = 23)
 *
 * We let libavutil helpers (av_frame_alloc, av_frame_get_buffer, av_frame_ref,
 * av_frame_unref) do allocation. The only direct field pokes are the three
 * ints above.
 */
#define DUMMY_FRAME_W      1920
#define DUMMY_FRAME_H      1080
#define AV_PIX_FMT_NV12    23
#define AVFRAME_OFF_WIDTH  104
#define AVFRAME_OFF_HEIGHT 108
#define AVFRAME_OFF_FORMAT 116
#define AVFRAME_OFF_PTS    136   /* int64_t  — ffmpeg 5.1 stable layout */
#define AVFRAME_OFF_PKTDTS 144   /* int64_t  — ffmpeg 5.1 stable layout */

typedef void *(*real_av_frame_alloc_t)(void);
typedef int  (*real_av_frame_get_buffer_t)(void *frame, int align);
typedef int  (*real_av_frame_ref_t)(void *dst, const void *src);
typedef void (*real_av_frame_unref_t)(void *frame);

static void *shim_dummy_frame = NULL;
static int shim_dummy_frame_inited = 0;

static void shim_init_dummy_frame(void) {
    if (shim_dummy_frame_inited) return;
    shim_dummy_frame_inited = 1;  /* flag now, regardless of success */

    real_av_frame_alloc_t      alloc    = shim_resolve_libav("av_frame_alloc", "LIBAVUTIL_57");
    real_av_frame_get_buffer_t getbuf   = shim_resolve_libav("av_frame_get_buffer", "LIBAVUTIL_57");

    if (!alloc || !getbuf) {
        SHIM_LOG("shim_init_dummy_frame: couldn't resolve libavutil helpers (alloc=%p getbuf=%p)",
                 (void *)alloc, (void *)getbuf);
        return;
    }

    void *f = alloc();
    if (!f) {
        SHIM_LOG("shim_init_dummy_frame: av_frame_alloc returned NULL");
        return;
    }

    *(int *)((char *)f + AVFRAME_OFF_WIDTH)  = DUMMY_FRAME_W;
    *(int *)((char *)f + AVFRAME_OFF_HEIGHT) = DUMMY_FRAME_H;
    *(int *)((char *)f + AVFRAME_OFF_FORMAT) = AV_PIX_FMT_NV12;

    int gb_ret = getbuf(f, 32);
    if (gb_ret < 0) {
        SHIM_LOG("shim_init_dummy_frame: av_frame_get_buffer returned %d", gb_ret);
        return;
    }

    /* Zero-fill Y plane to 0x10 (studio black luma), UV plane to 0x80
     * (neutral chroma). data/linesize are at known offsets 0/64. */
    uint8_t **data = (uint8_t **)f;
    int *linesize  = (int *)((char *)f + 64);
    if (data[0]) memset(data[0], 0x10, (size_t)linesize[0] * DUMMY_FRAME_H);
    if (data[1]) memset(data[1], 0x80, (size_t)linesize[1] * (DUMMY_FRAME_H / 2));

    shim_dummy_frame = f;
    SHIM_LOG("shim_init_dummy_frame OK: frame=%p data[0]=%p linesize[0]=%d",
             f, data[0], linesize[0]);
}

/* State: number of pending frames to deliver. send_packet accepts → +1. */
static int shim_pending_dummies = 0;

int avcodec_send_packet(void *avctx, const void *pkt) {
    if (!real_avcodec_send_packet)
        real_avcodec_send_packet =
            (real_avcodec_send_packet_t)shim_resolve_libav("avcodec_send_packet", "LIBAVCODEC_59");
    if (!real_avcodec_send_packet) return -38;

    int ret = real_avcodec_send_packet(avctx, pkt);
    av_send_count++;
    if (ret == 0) shim_pending_dummies++;   /* packet accepted → frame coming */
    if (av_send_count <= 8 || (av_send_count % 500) == 0) {
        SHIM_LOG("avcodec_send_packet #%u (ctx=%p, pkt=%p) -> %d  pending=%d",
                 av_send_count, avctx, pkt, ret, shim_pending_dummies);
    }
    return ret;
}

int avcodec_receive_frame(void *avctx, void *frame) {
    if (!real_avcodec_receive_frame)
        real_avcodec_receive_frame =
            (real_avcodec_receive_frame_t)shim_resolve_libav("avcodec_receive_frame", "LIBAVCODEC_59");
    if (!real_avcodec_receive_frame) return -38;

    int ret = real_avcodec_receive_frame(avctx, frame);
    av_recv_count++;
    if (ret == 0) av_recv_ok_count++; else av_recv_err_count++;

    /* Real decode returned EAGAIN AND we have a pending packet — inject
     * a dummy frame instead of propagating EAGAIN.
     * Step 52: gated behind SPIKE3_INJECT_VIDEO=1 so event-sweep runs default
     * to the Step-45 stable-shutdown baseline (no injection → no DRM-prime SEGV). */
    static int inject_enabled = -1;
    if (inject_enabled < 0) {
        const char *s = getenv("SPIKE3_INJECT_VIDEO");
        inject_enabled = (s && s[0] == '1') ? 1 : 0;
    }
    if (inject_enabled && ret == -11 /* EAGAIN */ && shim_pending_dummies > 0) {
        shim_init_dummy_frame();
        if (shim_dummy_frame) {
            real_av_frame_ref_t   aref   = shim_resolve_libav("av_frame_ref",   "LIBAVUTIL_57");
            real_av_frame_unref_t aunref = shim_resolve_libav("av_frame_unref", "LIBAVUTIL_57");
            if (aref && aunref) {
                aunref(frame);
                int rref = aref(frame, shim_dummy_frame);
                if (rref == 0) {
                    /* Stamp a valid monotonic PTS. The game asserts
                     * `frame->pts != AV_NOPTS_VALUE` at
                     * SpiVideoStreamDecoderBase.cpp:1490. A counter in the
                     * codec's native timebase would be ideal; we don't know
                     * it, so we just increment by 3000 per frame (=1/30 s in
                     * a 1/90000 timebase, the H.264/H.265 default). */
                    static int64_t next_pts = 0;
                    int64_t pts_val = next_pts;
                    next_pts += 3000;
                    *(int64_t *)((char *)frame + AVFRAME_OFF_PTS)    = pts_val;
                    *(int64_t *)((char *)frame + AVFRAME_OFF_PKTDTS) = pts_val;
                    shim_pending_dummies--;
                    av_recv_ok_count++;
                    av_recv_err_count--;
                    if (av_recv_count <= 8 || (av_recv_count % 500) == 0) {
                        SHIM_LOG("avcodec_receive_frame #%u -> INJECTED dummy NV12 %dx%d pts=%lld (remaining=%d)",
                                 av_recv_count, DUMMY_FRAME_W, DUMMY_FRAME_H,
                                 (long long)pts_val, shim_pending_dummies);
                    }
                    return 0;
                }
            }
        }
    }

    if (av_recv_count <= 8 || (av_recv_count % 500) == 0) {
        SHIM_LOG("avcodec_receive_frame #%u (ctx=%p, frame=%p) -> %d  (ok=%u err=%u)",
                 av_recv_count, avctx, frame, ret, av_recv_ok_count, av_recv_err_count);
    }
    return ret;
}

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

