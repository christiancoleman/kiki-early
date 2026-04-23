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

        qemu-aarch64 -L /mnt/c/Workspace/pokemon-le-0.81.0 \
            -E LD_PRELOAD=$(pwd)/_emulation/spi_shim.so \
            -E SPIKE3_SYSROOT=/mnt/c/Workspace/pokemon-le-0.81.0 \
            -E SPIKE3_BRIDGE=1 \
            -E "SPIKE3_POKE_CMODE=story_charmander_1:0x700:35" \
            ./game

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
/* DRM is a real socketpair (same pattern as UART) so the game's select() +
 * drmHandleEvent loop can drive forward on synthesized page-flip events. */

/* ---- V4L2 stub fds (Step 46) ----
 * libavcodec's v4l2_request HEVC decoder respects LIBVA_V4L2_REQUEST_MEDIA_PATH
 * and LIBVA_V4L2_REQUEST_VIDEO_PATH env vars. We set both at run time to
 * these sentinel paths and intercept the open — no /dev/media0 on the host
 * means we must provide our own fake device.
 */
#define FAKE_V4L2_MEDIA_PATH  "/dev/shim-v4l2-media"
#define FAKE_V4L2_VIDEO_PATH  "/dev/shim-v4l2-video"
#define FAKE_FD_V4L2_MEDIA    1021
#define FAKE_FD_V4L2_VIDEO    1020
static int v4l2_media_active = 0;
static int v4l2_video_active = 0;
static uint32_t v4l2_ioctl_count = 0;

/* ---- GPIO sysfs stubs (Step 63) ----
 * The game uses legacy /sys/class/gpio/{export,gpio25/direction,gpio25/value}
 * to bit-bang chip-select (or an interrupt line) for a SPI peripheral. Under
 * qemu-user the host's sysfs either has no gpio class or is not writable,
 * so the game sees EACCES on export and spins in a retry loop (687 failures
 * in a 185s run). We present each sysfs path as a small fake fd that swallows
 * writes and feeds back plausible reads ("0\n" for value, "out\n" for
 * direction). One fd per open() — the game re-opens on every poll. */
#define FAKE_FD_GPIO_BASE   1000
#define FAKE_FD_GPIO_COUNT  16
static char     *gpio_fake_paths[FAKE_FD_GPIO_COUNT];
static uint8_t   gpio_fake_read_done[FAKE_FD_GPIO_COUNT];
static pthread_mutex_t gpio_fake_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---- Device state ---- */

static int spi_active = 0;
static int i2c_active = 0;
static int uart_game_fd = -1;   /* returned from open(); real socketpair end */
static int uart_shim_fd = -1;   /* held by the shim for pushing responses */
static int drm_game_fd  = -1;   /* returned from open(/dev/dri/card1) */
static int drm_shim_fd  = -1;   /* shim end; we write synthetic flip events here */
static uint32_t spi_transfer_count = 0;
static uint32_t i2c_ioctl_count = 0;
static uint32_t uart_read_count = 0;
static uint32_t uart_write_count = 0;

/* uart_is_active() was here — never called; removed in Phase 3. */
static int is_drm_fd(int fd) { return drm_game_fd >= 0 && fd == drm_game_fd; }

/* SHIM_LOG moved up to just after the macros — see the early definition. */

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



/* ---- Step 58.2: frame capture tracking ----
 *
 * The game presents via DRM MODE_PAGE_FLIP, not eglSwapBuffers, so the
 * capture trigger sits in the DRM ioctl path below. The target is whichever
 * GL texture the game's create_egl_framebuffer bound as an FBO color
 * attachment via glEGLImageTargetTexture2DOES — our override already runs
 * glTexImage2D(w, h, RGBA8) to give it Mesa-allocated storage. We track
 * every texture we touched that way; at page-flip we read from the largest
 * one (assumed to be the main render target). */

/* Forward decl — defined alongside the glBindFramebuffer hook. */
extern volatile unsigned shim_last_draw_fbo;

#define SHIM_FRAMETEX_MAX 32
struct shim_frametex {
    unsigned tex_id;
    unsigned target;  /* GL_TEXTURE_2D or GL_TEXTURE_EXTERNAL_OES */
    int w, h;
};
static struct shim_frametex shim_frametex[SHIM_FRAMETEX_MAX];
static int shim_frametex_count = 0;
static pthread_mutex_t shim_frametex_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned shim_capture_fbo = 0;  /* lazily created at first capture */
/* shim_capture_flip_counter moved to shim_bridges.c alongside the capture
 * function that owns it; shim_capture_fbo stays (was unused, harmless). */

/* Upsert {tex_id, target, w, h} into the tracked list. Called from
 * glEGLImageTargetTexture2DOES right after we glTexImage2D'd the storage. */
static void shim_frametex_record(unsigned target, int w, int h) {
    static void (*real_glGetIntegerv)(unsigned, int*) = NULL;
    if (!real_glGetIntegerv) real_glGetIntegerv = (void(*)(unsigned,int*))
                                                   dlsym(RTLD_NEXT, "glGetIntegerv");
    if (!real_glGetIntegerv) return;

    int bind_param;
    switch (target) {
        case 0x0DE1: bind_param = 0x8069; break;  /* GL_TEXTURE_BINDING_2D */
        case 0x8D65: bind_param = 0x8D67; break;  /* GL_TEXTURE_BINDING_EXTERNAL_OES */
        default: return;
    }
    int tex_id = 0;
    real_glGetIntegerv((unsigned)bind_param, &tex_id);
    if (tex_id <= 0) return;

    pthread_mutex_lock(&shim_frametex_lock);
    for (int i = 0; i < shim_frametex_count; i++) {
        if (shim_frametex[i].tex_id == (unsigned)tex_id) {
            shim_frametex[i].w = w;
            shim_frametex[i].h = h;
            shim_frametex[i].target = target;
            pthread_mutex_unlock(&shim_frametex_lock);
            return;
        }
    }
    if (shim_frametex_count < SHIM_FRAMETEX_MAX) {
        shim_frametex[shim_frametex_count++] = (struct shim_frametex){
            .tex_id = (unsigned)tex_id, .target = target, .w = w, .h = h,
        };
        SHIM_LOG("[frame_cap] tracking tex=%u target=0x%x %dx%d (%d tracked)",
                 (unsigned)tex_id, target, w, h, shim_frametex_count);
    }
    pthread_mutex_unlock(&shim_frametex_lock);
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

static void resolve_all(void) {
    if (!real_open)  real_open  = (real_open_t)dlsym(RTLD_NEXT, "open");
    if (!real_close) real_close = (real_close_t)dlsym(RTLD_NEXT, "close");
    if (!real_ioctl) real_ioctl = (real_ioctl_t)dlsym(RTLD_NEXT, "ioctl");
    if (!real_read)  real_read  = (real_read_t)dlsym(RTLD_NEXT, "read");
    if (!real_write) real_write = (real_write_t)dlsym(RTLD_NEXT, "write");
}

/* ---- GPIO sysfs fake-fd pool ---- */

static int is_gpio_sysfs_path(const char *p) {
    return p && strncmp(p, "/sys/class/gpio/", 16) == 0;
}

static int alloc_gpio_fake_fd(const char *path) {
    pthread_mutex_lock(&gpio_fake_lock);
    for (int i = 0; i < FAKE_FD_GPIO_COUNT; i++) {
        if (!gpio_fake_paths[i]) {
            gpio_fake_paths[i] = strdup(path);
            gpio_fake_read_done[i] = 0;
            pthread_mutex_unlock(&gpio_fake_lock);
            return FAKE_FD_GPIO_BASE + i;
        }
    }
    pthread_mutex_unlock(&gpio_fake_lock);
    return 0;
}

static int is_gpio_fake_fd(int fd) {
    if (fd < FAKE_FD_GPIO_BASE || fd >= FAKE_FD_GPIO_BASE + FAKE_FD_GPIO_COUNT)
        return 0;
    return gpio_fake_paths[fd - FAKE_FD_GPIO_BASE] != NULL;
}

static void free_gpio_fake_fd(int fd) {
    int idx = fd - FAKE_FD_GPIO_BASE;
    pthread_mutex_lock(&gpio_fake_lock);
    free(gpio_fake_paths[idx]);
    gpio_fake_paths[idx] = NULL;
    gpio_fake_read_done[idx] = 0;
    pthread_mutex_unlock(&gpio_fake_lock);
}

/* ---- Check if path is a device we intercept, return fake fd or 0 ---- */

static int intercept_device(const char *pathname) {
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

static int is_fake_fd(int fd) {
    /* UART and DRM are NOT fake — both are real socketpair fds; callers
     * that need to check for "any intercepted device" should call both
     * this and uart_game_fd / drm_game_fd explicitly. */
    return (fd == FAKE_FD_SPI && spi_active) ||
           (fd == FAKE_FD_I2C && i2c_active) ||
           (fd == FAKE_FD_V4L2_MEDIA && v4l2_media_active) ||
           (fd == FAKE_FD_V4L2_VIDEO && v4l2_video_active);
}

static int is_v4l2_fd(int fd) {
    return (fd == FAKE_FD_V4L2_MEDIA && v4l2_media_active) ||
           (fd == FAKE_FD_V4L2_VIDEO && v4l2_video_active);
}

static int is_uart_fd(int fd) {
    return uart_game_fd >= 0 && fd == uart_game_fd;
}

/* ---- Hooked: open / openat ---- */

/* Log interesting opens: device nodes, sysfs/procfs, and anything under
 * /games/ that isn't plain-file library loading. Ignore noisy common paths
 * (shared libraries, locale, tzdata) and already-covered devices. */
static int should_log_open(const char *pathname) {
    if (!pathname) return 0;
    if (strncmp(pathname, "/dev/", 5) == 0) return 1;
    if (strncmp(pathname, "/sys/", 5) == 0) return 1;
    if (strncmp(pathname, "/proc/", 6) == 0) return 1;
    if (strncmp(pathname, "/games/", 7) == 0) return 1;
    if (strncmp(pathname, "/run/", 5) == 0) return 1;
    if (strncmp(pathname, "/tmp/", 5) == 0) return 1;
    if (strncmp(pathname, "/var/", 5) == 0) return 1;
    if (strncmp(pathname, "/etc/", 5) == 0) {
        /* Skip /etc/ld.so.*, /etc/nsswitch.conf, /etc/localtime — these are
         * libc bootstrap and don't tell us anything about game state. */
        if (strstr(pathname, "ld.so")) return 0;
        if (strstr(pathname, "nsswitch")) return 0;
        if (strstr(pathname, "localtime")) return 0;
        return 1;
    }
    /* Step 62.b: catch Godot-resource opens regardless of prefix. Relative
     * paths (./assets/...) and absolute /mnt/c/... paths fall through all
     * the /dev/games/etc checks above. These tell us whether the game
     * actually finds main.pck, .tscn/.scn resources, .gd scripts, etc. */
    if (strstr(pathname, ".pck")) return 1;
    if (strstr(pathname, ".tscn")) return 1;
    if (strstr(pathname, ".scn"))  return 1;
    if (strstr(pathname, ".remap")) return 1;
    if (strstr(pathname, ".import")) return 1;
    if (strstr(pathname, "godot"))  return 1;
    return 0;
}

int open(const char *pathname, int flags, ...) {
    resolve_all();
    int fake = intercept_device(pathname);
    if (fake) return fake;

    const char *actual = remap_path(pathname);

    int ret;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        int mode = va_arg(args, int);
        va_end(args);
        ret = real_open(actual, flags, mode);
    } else {
        ret = real_open(actual, flags);
    }

    if (should_log_open(pathname)) {
        if (ret < 0) {
            SHIM_LOG("OPEN FAIL: %s (flags=0x%x, errno=%d %s)",
                     pathname, flags, errno, strerror(errno));
        } else {
            SHIM_LOG("OPEN OK:   %s -> fd %d (flags=0x%x)",
                     pathname, ret, flags);
        }
    }
    return ret;
}

typedef int (*real_openat_t)(int, const char *, int, ...);
static real_openat_t real_openat = NULL;

int openat(int dirfd, const char *pathname, int flags, ...) {
    if (!real_openat) real_openat = (real_openat_t)dlsym(RTLD_NEXT, "openat");

    int fake = intercept_device(pathname);
    if (fake) return fake;

    const char *actual = remap_path(pathname);

    int ret;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        int mode = va_arg(args, int);
        va_end(args);
        ret = real_openat(dirfd, actual, flags, mode);
    } else {
        ret = real_openat(dirfd, actual, flags);
    }

    if (should_log_open(pathname)) {
        if (ret < 0) {
            SHIM_LOG("OPENAT FAIL: %s (dirfd=%d flags=0x%x, errno=%d %s)",
                     pathname, dirfd, flags, errno, strerror(errno));
        } else {
            SHIM_LOG("OPENAT OK:   %s -> fd %d (dirfd=%d flags=0x%x)",
                     pathname, ret, dirfd, flags);
        }
    }
    return ret;
}

/* ---- Step 62.b: LFS variants (open64/openat64/fopen64) ----
 * The Pokemon LE binary imports open64@GLIBC_2.17 and fopen64@GLIBC_2.17
 * (per readelf --dyn-syms). Our open() hook only catches calls to the
 * 32-bit-off_t variant; the game's Godot resource loader actually uses
 * open64 (the LFS family), so main.pck access was invisible to us. Mirror
 * the existing open/openat semantics here so path-remap + logging cover
 * those calls too. On 64-bit aarch64 Linux, open64 and open are functionally
 * identical (off_t is already 64-bit), but the SYMBOL resolution is distinct. */

typedef int (*real_open64_t)(const char *, int, ...);
static real_open64_t real_open64 = NULL;

int open64(const char *pathname, int flags, ...) {
    if (!real_open64) real_open64 = (real_open64_t)dlsym(RTLD_NEXT, "open64");
    int fake = intercept_device(pathname);
    if (fake) return fake;
    const char *actual = remap_path(pathname);
    int ret;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        int mode = va_arg(args, int);
        va_end(args);
        ret = real_open64(actual, flags, mode);
    } else {
        ret = real_open64(actual, flags);
    }
    if (should_log_open(pathname)) {
        if (ret < 0) {
            SHIM_LOG("OPEN64 FAIL: %s (flags=0x%x, errno=%d %s)",
                     pathname, flags, errno, strerror(errno));
        } else {
            SHIM_LOG("OPEN64 OK:   %s -> fd %d (flags=0x%x)",
                     pathname, ret, flags);
        }
    }
    return ret;
}

typedef int (*real_openat64_t)(int, const char *, int, ...);
static real_openat64_t real_openat64 = NULL;

int openat64(int dirfd, const char *pathname, int flags, ...) {
    if (!real_openat64) real_openat64 = (real_openat64_t)dlsym(RTLD_NEXT, "openat64");
    int fake = intercept_device(pathname);
    if (fake) return fake;
    const char *actual = remap_path(pathname);
    int ret;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        int mode = va_arg(args, int);
        va_end(args);
        ret = real_openat64(dirfd, actual, flags, mode);
    } else {
        ret = real_openat64(dirfd, actual, flags);
    }
    if (should_log_open(pathname)) {
        if (ret < 0) {
            SHIM_LOG("OPENAT64 FAIL: %s (dirfd=%d flags=0x%x, errno=%d %s)",
                     pathname, dirfd, flags, errno, strerror(errno));
        } else {
            SHIM_LOG("OPENAT64 OK:   %s -> fd %d (dirfd=%d flags=0x%x)",
                     pathname, ret, dirfd, flags);
        }
    }
    return ret;
}

typedef FILE* (*real_fopen64_t)(const char *, const char *);
static real_fopen64_t real_fopen64 = NULL;

FILE *fopen64(const char *pathname, const char *mode) {
    if (!real_fopen64) real_fopen64 = (real_fopen64_t)dlsym(RTLD_NEXT, "fopen64");
    /* fopen64 returns FILE* not an fd — device-interception path doesn't fit.
     * Path-remap still applies. */
    const char *actual = remap_path(pathname);
    FILE *ret = real_fopen64(actual, mode);
    if (should_log_open(pathname)) {
        if (!ret) {
            SHIM_LOG("FOPEN64 FAIL: %s (mode=%s, errno=%d %s)",
                     pathname, mode, errno, strerror(errno));
        } else {
            SHIM_LOG("FOPEN64 OK:   %s -> FILE* %p (mode=%s)",
                     pathname, (void*)ret, mode);
        }
    }
    return ret;
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

    /* GPIO sysfs: pretend the pin directory + attribute files always exist
     * (mode ignored — the game uses F_OK to check export state). */
    if (is_gpio_sysfs_path(pathname)) {
        (void)mode;
        return 0;
    }

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

/* ---- Hooked: GBM library functions ----
 * libgbm's real gbm_create_device inspects the DRM fd's driver identity via
 * /sys and DRM_IOCTL_VERSION-derived driver-name matching. Our fake DRM fd
 * has no backing /sys entry, so libgbm rejects it with EINVAL. We short-
 * circuit with fake handles so the game's "does GBM work" check succeeds.
 * Pointers returned are just magic sentinels — the real gbm library never
 * sees them because we also intercept every subsequent gbm_* call. */

/* gbm_device/bo/surface pointers need to be dereferenceable.
 * The game's own buffer-management code (or mesa internals that our
 * LD_PRELOAD misses) dereferences the pointers we return, bypassing our
 * getter stubs. Originally we used sentinel constants like 0x61626d44
 * ("abmD") and saw SEGV at address 0x61626d60 — fake-pointer + ~30 bytes.
 * Allocate zero-filled scratch buffers instead so direct-deref reads hit
 * valid memory. Values read back are zero, which the game's gbm_bo_*
 * getters override via our hooks anyway. */

#define FAKE_GBM_BACKING_SIZE 256

static int gbm_drm_fd = -1;
static void *fake_gbm_device_mem  = NULL;
static void *fake_gbm_surface_mem = NULL;

static void *alloc_fake_gbm_object(void) {
    void *p = calloc(1, FAKE_GBM_BACKING_SIZE);
    return p;
}

/* Minimal per-BO side table: track dimensions so gbm_bo_get_width/height/
 * stride/format return what the caller originally requested at create-time.
 * DMD-specific machinery (is_dmd, mapped tracking, frame dump) removed in
 * Step 56 — modern Spike 3 has no physical DMD. */
#define GBM_BO_TABLE_SIZE   32

struct gbm_bo_entry {
    void    *bo;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t stride;
};

static struct gbm_bo_entry gbm_bo_table[GBM_BO_TABLE_SIZE];
static pthread_mutex_t gbm_bo_table_lock = PTHREAD_MUTEX_INITIALIZER;
static void *gbm_bo_latest = NULL;

static void gbm_bo_register(void *bo, uint32_t w, uint32_t h, uint32_t fmt) {
    pthread_mutex_lock(&gbm_bo_table_lock);
    for (int i = 0; i < GBM_BO_TABLE_SIZE; i++) {
        if (!gbm_bo_table[i].bo) {
            gbm_bo_table[i].bo     = bo;
            gbm_bo_table[i].width  = w;
            gbm_bo_table[i].height = h;
            gbm_bo_table[i].format = fmt;
            gbm_bo_table[i].stride = w * 4;   /* 32-bpp assumed */
            break;
        }
    }
    pthread_mutex_unlock(&gbm_bo_table_lock);
}

static struct gbm_bo_entry *gbm_bo_lookup(void *bo) {
    /* No lock needed for read-only lookup; entries are write-once post-create. */
    for (int i = 0; i < GBM_BO_TABLE_SIZE; i++)
        if (gbm_bo_table[i].bo == bo) return &gbm_bo_table[i];
    return NULL;
}

static void gbm_bo_unregister(void *bo) {
    pthread_mutex_lock(&gbm_bo_table_lock);
    for (int i = 0; i < GBM_BO_TABLE_SIZE; i++)
        if (gbm_bo_table[i].bo == bo) { memset(&gbm_bo_table[i], 0, sizeof(gbm_bo_table[i])); break; }
    pthread_mutex_unlock(&gbm_bo_table_lock);
}

void *gbm_create_device(int fd) {
    if (!fake_gbm_device_mem) fake_gbm_device_mem = alloc_fake_gbm_object();
    SHIM_LOG("gbm_create_device(fd=%d) -> %p (%d-byte backing)",
             fd, fake_gbm_device_mem, FAKE_GBM_BACKING_SIZE);
    gbm_drm_fd = fd;
    return fake_gbm_device_mem;
}

void gbm_device_destroy(void *device) {
    SHIM_LOG("gbm_device_destroy");
    (void)device;
}

int gbm_device_get_fd(void *device) {
    (void)device; return gbm_drm_fd;
}

const char *gbm_device_get_backend_name(void *device) {
    (void)device; return "stub";
}

int gbm_device_is_format_supported(void *device, uint32_t format, uint32_t usage) {
    (void)device; (void)format; (void)usage; return 1;
}

void *gbm_surface_create(void *gbm, uint32_t width, uint32_t height,
                         uint32_t format, uint32_t flags) {
    if (!fake_gbm_surface_mem) fake_gbm_surface_mem = alloc_fake_gbm_object();
    SHIM_LOG("gbm_surface_create(%ux%u, fmt=0x%x, flags=0x%x) -> %p",
             width, height, format, flags, fake_gbm_surface_mem);
    (void)gbm; return fake_gbm_surface_mem;
}

void *gbm_surface_create_with_modifiers(void *gbm, uint32_t width, uint32_t height,
                                        uint32_t format, const uint64_t *modifiers,
                                        unsigned int count) {
    if (!fake_gbm_surface_mem) fake_gbm_surface_mem = alloc_fake_gbm_object();
    SHIM_LOG("gbm_surface_create_with_modifiers(%ux%u) -> %p", width, height,
             fake_gbm_surface_mem);
    (void)gbm; (void)format; (void)modifiers; (void)count;
    return fake_gbm_surface_mem;
}

void gbm_surface_destroy(void *surface) {
    (void)surface;
}

void *gbm_bo_create(void *gbm, uint32_t width, uint32_t height,
                    uint32_t format, uint32_t flags) {
    void *bo = alloc_fake_gbm_object();
    gbm_bo_latest = bo;
    gbm_bo_register(bo, width, height, format);
    SHIM_LOG("gbm_bo_create(%ux%u, fmt=0x%x, flags=0x%x) -> %p",
             width, height, format, flags, bo);
    (void)gbm;
    return bo;
}

void *gbm_bo_create_with_modifiers(void *gbm, uint32_t width, uint32_t height,
                                   uint32_t format, const uint64_t *modifiers,
                                   unsigned int count) {
    void *bo = alloc_fake_gbm_object();
    gbm_bo_latest = bo;
    gbm_bo_register(bo, width, height, format);
    SHIM_LOG("gbm_bo_create_with_modifiers(%ux%u) -> %p", width, height, bo);
    (void)gbm; (void)modifiers; (void)count;
    return bo;
}

void gbm_bo_destroy(void *bo) {
    gbm_bo_unregister(bo); free(bo);
}

uint32_t gbm_bo_get_width(void *bo) {
    struct gbm_bo_entry *e = gbm_bo_lookup(bo);
    return e ? e->width : FAKE_MODE_HDISPLAY;
}
uint32_t gbm_bo_get_height(void *bo) {
    struct gbm_bo_entry *e = gbm_bo_lookup(bo);
    return e ? e->height : FAKE_MODE_VDISPLAY;
}
uint32_t gbm_bo_get_stride(void *bo) {
    struct gbm_bo_entry *e = gbm_bo_lookup(bo);
    return e ? e->stride : (FAKE_MODE_HDISPLAY * 4);
}
uint32_t gbm_bo_get_format(void *bo) {
    struct gbm_bo_entry *e = gbm_bo_lookup(bo);
    return e ? e->format : 0x34325258; /* XR24 */
}
uint64_t gbm_bo_get_modifier(void *bo) {
    (void)bo; return 0; /* LINEAR */
}
int gbm_bo_get_fd(void *bo) {
    (void)bo; return -1;
}
uint32_t gbm_bo_get_handle_u(void *bo) {
    (void)bo; return 1;
}

/* gbm_bo_map / gbm_bo_unmap: stub path hands back a per-call scratch buffer
 * sized from the caller's requested (w,h). The caller memcpys into it and
 * unmaps — we free and return. No state kept. */
void *gbm_bo_map(void *bo, uint32_t x, uint32_t y,
                 uint32_t w, uint32_t h, uint32_t flags,
                 uint32_t *stride_out, void **map_data_out) {
    (void)x; (void)y; (void)flags;
    size_t sz = (size_t)w * (size_t)h * 4;
    void *scratch = calloc(1, sz ? sz : 16);
    if (stride_out)   *stride_out = w * 4;
    if (map_data_out) *map_data_out = scratch;
    (void)bo;
    return scratch;
}

void gbm_bo_unmap(void *bo, void *map_data) {
    (void)bo;
    free(map_data);
}

uint64_t gbm_bo_get_handle(void *bo) {
    (void)bo; return 1;
}

void *gbm_surface_lock_front_buffer(void *surface) {
    (void)surface;
    if (!gbm_bo_latest) gbm_bo_latest = alloc_fake_gbm_object();
    return gbm_bo_latest;
}
void gbm_surface_release_buffer(void *surface, void *bo) {
    (void)surface; (void)bo;
}
int gbm_surface_has_free_buffers(void *surface) {
    (void)surface; return 1;
}

/* ---- Hooked: EGL ----
 * Once gbm_create_device returns our fake handle, mesa's libEGL would
 * dereference it — crash. We intercept the public EGL entry points the
 * game uses (per BN analysis of glWindow::setGlContext) so no mesa code
 * runs against our fake GBM handle. Returns are fake but stable.
 */

#define FAKE_EGL_DISPLAY   ((void *)0x65676c44)  /* "eglD" */
#define FAKE_EGL_CONFIG    ((void *)0x65676c43)  /* "eglC" */
#define FAKE_EGL_CONTEXT   ((void *)0x65676c58)  /* "eglX" */
#define FAKE_EGL_SURFACE   ((void *)0x65676c53)  /* "eglS" */
#define FAKE_EGL_IMAGE     ((void *)0x65676c49)  /* "eglI" */
#define FAKE_EGL_SYNC      ((void *)0x65676c59)  /* "eglY" */

/* EGL constants used in our stubs (from <EGL/egl.h>). Declared locally so
 * the shim doesn't need EGL headers installed. */
#define EGL_SUCCESS          0x3000
#define EGL_TRUE             1
#define EGL_FALSE            0
#define EGL_NO_DISPLAY       ((void *)0)
#define EGL_NO_CONTEXT       ((void *)0)
#define EGL_NO_SURFACE       ((void *)0)
#define EGL_OPENGL_ES_API    0x30A0
#define EGL_PLATFORM_GBM_KHR         0x31D7
#define EGL_PLATFORM_WAYLAND_KHR     0x31D8
#define EGL_PLATFORM_X11_KHR         0x31D5
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#define EGL_DEFAULT_DISPLAY          ((void *)0)

/* Under SPIKE3_REAL_GL, translate the game's display query into a form Mesa
 * can actually satisfy on our overlay. Mesa 24.0.9 in surfaceless mode only
 * accepts EGL_PLATFORM_SURFACELESS_MESA (0x31DD) with a NULL native handle.
 * The game asks for EGL_PLATFORM_GBM_KHR (0x31D7) with a fake GBM device.
 * Translation: drop the game's platform/handle, ask Mesa for surfaceless.
 *
 * Fallback chain for resolving the real function:
 *   1. dlsym("eglGetPlatformDisplay[EXT]") — usually works for non-EXT,
 *      returns NULL for EXT (Mesa exports the EXT variant only via
 *      eglGetProcAddress, not as a direct dynamic symbol).
 *   2. dlsym("eglGetPlatformDisplay") — EGL 1.5 core, always exported.
 *   3. dlsym("eglGetDisplay") — EGL 1.0 legacy, last-ditch.
 * If none resolve, fall through to the stub body returning FAKE_EGL_DISPLAY. */

/* Forward decls — defined alongside eglMakeCurrent further down. Captured
 * here so the fallback-pbuffer path at makeCurrent time has something to
 * pass to Mesa's eglCreatePbufferSurface(display, config, ...). */
extern void *shim_last_real_display;
extern void *shim_last_real_config;
extern void *shim_fallback_pbuf;

static void *shim_real_platform_display(unsigned /*unused game-requested*/ platform) {
    (void)platform;
    static void *(*real_1_5)(unsigned, void*, const intptr_t*) = NULL;
    if (!real_1_5) real_1_5 = (void *(*)(unsigned, void*, const intptr_t*))
                              dlsym(RTLD_NEXT, "eglGetPlatformDisplay");
    if (real_1_5) {
        void *d = real_1_5(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
        if (d) {
            SHIM_LOG("  → Mesa real display %p via eglGetPlatformDisplay(SURFACELESS)", d);
            shim_last_real_display = d;  /* capture for later fallback-pbuffer creation */
            return d;
        }
    }
    static void *(*real_legacy)(void*) = NULL;
    if (!real_legacy) real_legacy = (void *(*)(void*))dlsym(RTLD_NEXT, "eglGetDisplay");
    if (real_legacy) {
        void *d = real_legacy(EGL_DEFAULT_DISPLAY);
        if (d) {
            SHIM_LOG("  → Mesa real display %p via legacy eglGetDisplay", d);
            shim_last_real_display = d;
            return d;
        }
    }
    return NULL;
}

void *eglGetDisplay(void *native_display) {
    if (is_real_gl()) {
        SHIM_LOG("FWD eglGetDisplay(%p) — translating to SURFACELESS_MESA", native_display);
        void *d = shim_real_platform_display(0);
        if (d) return d;
        SHIM_LOG("  no real Mesa display found; falling back to stub");
    }
    SHIM_LOG("eglGetDisplay(%p) -> fake display", native_display);
    return FAKE_EGL_DISPLAY;
}

void *eglGetPlatformDisplay(unsigned platform, void *native_display, const intptr_t *attrib_list) {
    if (is_real_gl()) {
        SHIM_LOG("FWD eglGetPlatformDisplay(platform=0x%x, dpy=%p) — translating to SURFACELESS_MESA",
                 platform, native_display);
        void *d = shim_real_platform_display(platform);
        if (d) { (void)attrib_list; return d; }
        SHIM_LOG("  no real Mesa display found; falling back to stub");
    }
    SHIM_LOG("eglGetPlatformDisplay(platform=0x%x, dpy=%p) -> fake", platform, native_display);
    (void)attrib_list; return FAKE_EGL_DISPLAY;
}

void *eglGetPlatformDisplayEXT(unsigned platform, void *native_display, const int *attrib_list) {
    if (is_real_gl()) {
        /* Mesa may or may not export the EXT symbol as a direct dynamic symbol.
         * Try it first; if NULL, fall back to the non-EXT EGL 1.5 path. */
        static void *(*real_ext)(unsigned, void*, const int*) = NULL;
        static int real_ext_tried = 0;
        if (!real_ext_tried) {
            real_ext = (void *(*)(unsigned, void*, const int*))
                       dlsym(RTLD_NEXT, "eglGetPlatformDisplayEXT");
            real_ext_tried = 1;
        }
        if (real_ext) {
            SHIM_LOG("FWD eglGetPlatformDisplayEXT(platform=0x%x) — direct forward", platform);
            return real_ext(platform, native_display, attrib_list);
        }
        SHIM_LOG("FWD eglGetPlatformDisplayEXT(platform=0x%x) — EXT symbol not exported, translating to SURFACELESS_MESA",
                 platform);
        void *d = shim_real_platform_display(platform);
        if (d) { (void)attrib_list; return d; }
        SHIM_LOG("  no real Mesa display found; falling back to stub");
    }
    SHIM_LOG("eglGetPlatformDisplayEXT(platform=0x%x, dpy=%p) -> fake", platform, native_display);
    (void)attrib_list; return FAKE_EGL_DISPLAY;
}

unsigned eglInitialize(void *display, int *major, int *minor) {
    FWD_RET_V(unsigned, eglInitialize, (void*, int*, int*), (display, major, minor));
    if (major) *major = 1;
    if (minor) *minor = 5;
    SHIM_LOG("eglInitialize(%p) -> 1.5", display);
    return EGL_TRUE;
}

unsigned eglTerminate(void *display) {
    FWD_RET_V(unsigned, eglTerminate, (void*), (display));
    (void)display; return EGL_TRUE;
}

const char *eglQueryString(void *display, int name) {
    FWD_RET_V(const char*, eglQueryString, (void*, int), (display, name));
    (void)display;
    switch (name) {
    case 0x3053: return "1.5";                       /* EGL_VERSION */
    case 0x3054: return "Stub EGL";                  /* EGL_VENDOR */
    case 0x3055: return "";                          /* EGL_EXTENSIONS */
    case 0x308D: return "OpenGL_ES";                 /* EGL_CLIENT_APIS */
    default: return "";
    }
}

unsigned eglBindAPI(unsigned api) {
    FWD_RET_V(unsigned, eglBindAPI, (unsigned), (api));
    SHIM_LOG("eglBindAPI(0x%x)", api);
    return EGL_TRUE;
}

unsigned eglGetConfigs(void *display, void **configs, int config_size, int *num_config) {
    FWD_RET_V(unsigned, eglGetConfigs, (void*, void**, int, int*),
            (display, configs, config_size, num_config));
    (void)display;
    if (num_config) *num_config = 1;
    if (configs && config_size >= 1) configs[0] = FAKE_EGL_CONFIG;
    return EGL_TRUE;
}

/* Map EGL attribute enum to a short readable name for the diagnostic log. */
static const char *shim_egl_attr_name(int a) {
    switch (a) {
        case 0x3020: return "BUFFER_SIZE";
        case 0x3021: return "ALPHA_SIZE";
        case 0x3022: return "BLUE_SIZE";
        case 0x3023: return "GREEN_SIZE";
        case 0x3024: return "RED_SIZE";
        case 0x3025: return "DEPTH_SIZE";
        case 0x3026: return "STENCIL_SIZE";
        case 0x3027: return "CONFIG_CAVEAT";
        case 0x3028: return "CONFIG_ID";
        case 0x302D: return "NATIVE_RENDERABLE";
        case 0x302E: return "NATIVE_VISUAL_ID";
        case 0x302F: return "NATIVE_VISUAL_TYPE";
        case 0x3031: return "SAMPLES";
        case 0x3032: return "SAMPLE_BUFFERS";
        case 0x3033: return "SURFACE_TYPE";
        case 0x303F: return "COLOR_BUFFER_TYPE";
        case 0x3040: return "RENDERABLE_TYPE";
        case 0x3042: return "CONFORMANT";
        case 0x3038: return "NONE";
        default:     return "?";
    }
}

unsigned eglChooseConfig(void *display, const int *attrib_list,
                         void **configs, int config_size, int *num_config) {
    if (__builtin_expect(is_real_gl(), 0)) {
        /* Copy the game's attrib list into a local scratch so we can patch
         * EGL_SURFACE_TYPE = EGL_WINDOW_BIT (0x4) → EGL_PBUFFER_BIT (0x1).
         * Mesa surfaceless only advertises PBUFFER configs, so a WINDOW_BIT
         * ask returns zero matches. Our eglCreateWindowSurface interceptor
         * already redirects to eglCreatePbufferSurface (Step 56.4), so this
         * translation is end-to-end consistent — the game sees a "window"
         * surface at its API but gets a pbuffer backing it. */
        int patched[128];
        int plen = 0;
        int patched_surface = 0;
        if (attrib_list) {
            for (int i = 0; attrib_list[i] != 0x3038 && i < 60; i += 2) {
                int a = attrib_list[i], v = attrib_list[i+1];
                if (a == 0x3033 /* EGL_SURFACE_TYPE */ && (v & 0x4) /* WINDOW_BIT set */) {
                    int v2 = (v & ~0x4) | 0x1;   /* drop WINDOW, add PBUFFER */
                    SHIM_LOG("  patching SURFACE_TYPE: 0x%x -> 0x%x (WINDOW→PBUFFER)", v, v2);
                    v = v2;
                    patched_surface = 1;
                }
                patched[plen++] = a;
                patched[plen++] = v;
            }
        }
        patched[plen++] = 0x3038; /* EGL_NONE terminator */

        SHIM_LOG("FWD eglChooseConfig(dpy=%p, config_size=%d)%s:",
                 display, config_size, patched_surface ? " [patched]" : "");
        for (int i = 0; i < plen - 1; i += 2) {
            SHIM_LOG("    attrib[%d] = 0x%x (%s)  value = 0x%x (%d)",
                     i, patched[i], shim_egl_attr_name(patched[i]),
                     patched[i+1], patched[i+1]);
        }

        static unsigned (*_real)(void*, const int*, void**, int, int*) = NULL;
        if (!_real) _real = (unsigned(*)(void*, const int*, void**, int, int*))
                            dlsym(RTLD_NEXT, "eglChooseConfig");
        if (_real) {
            unsigned r = _real(display, attrib_list ? patched : NULL,
                               configs, config_size, num_config);
            SHIM_LOG("  Mesa eglChooseConfig -> ret=%u, *num_config=%d",
                     r, num_config ? *num_config : -1);
            /* Capture the first config for the fallback-pbuffer path in
             * eglMakeCurrent (game may call eglMakeCurrent with no surface
             * so we need a config to fabricate one). */
            if (r && configs && num_config && *num_config > 0 && !shim_last_real_config) {
                shim_last_real_config = configs[0];
                SHIM_LOG("  captured EGLConfig %p for fallback-pbuffer use", configs[0]);
            }
            return r;
        }
        SHIM_LOG("  no real eglChooseConfig; falling back to stub");
    }
    (void)display; (void)attrib_list;
    if (num_config) *num_config = 1;
    if (configs && config_size >= 1) configs[0] = FAKE_EGL_CONFIG;
    return EGL_TRUE;
}

unsigned eglGetConfigAttrib(void *display, void *config, int attribute, int *value) {
    if (__builtin_expect(is_real_gl(), 0)) {
        static unsigned (*_real)(void*, void*, int, int*) = NULL;
        if (!_real) _real = (unsigned(*)(void*, void*, int, int*))
                            dlsym(RTLD_NEXT, "eglGetConfigAttrib");
        if (_real) {
            unsigned r = _real(display, config, attribute, value);
            /* Override EGL_NATIVE_VISUAL_ID to the GBM XR24 fourcc the game
             * expects — Mesa's surfaceless configs have no native-visual
             * tie, so this attribute returns 0 by default and the game's
             * per-config GBM-format filter fails. */
            if (attribute == 0x302E /* EGL_NATIVE_VISUAL_ID */ && value) {
                *value = 0x34325258; /* XR24 — matches gbm_bo_get_format stub */
            }
            return r;
        }
        /* dlsym miss — fall through to stub body below */
    }
    (void)display; (void)config;
    if (!value) return EGL_FALSE;
    /* EGL attribute constants from <EGL/egl.h>. */
    switch (attribute) {
    case 0x3020: *value = 32; break;                 /* EGL_BUFFER_SIZE */
    case 0x3021: *value = 8;  break;                 /* EGL_ALPHA_SIZE */
    case 0x3022: *value = 8;  break;                 /* EGL_BLUE_SIZE */
    case 0x3023: *value = 8;  break;                 /* EGL_GREEN_SIZE */
    case 0x3024: *value = 8;  break;                 /* EGL_RED_SIZE */
    case 0x3025: *value = 0;  break;                 /* EGL_DEPTH_SIZE */
    case 0x3026: *value = 0;  break;                 /* EGL_STENCIL_SIZE */
    case 0x3027: *value = 0x3038; break;             /* EGL_CONFIG_CAVEAT = NONE */
    case 0x3028: *value = 1;  break;                 /* EGL_CONFIG_ID */
    case 0x302E: *value = 0x34325258; break;         /* EGL_NATIVE_VISUAL_ID = XR24 (matches GBM) */
    case 0x302F: *value = 0; break;                  /* EGL_NATIVE_VISUAL_TYPE */
    case 0x3033: *value = 0x0004; break;             /* EGL_SURFACE_TYPE = WINDOW_BIT */
    case 0x3040: *value = 0x0040; break;             /* EGL_RENDERABLE_TYPE = OPENGL_ES2_BIT */
    case 0x3050: *value = 0x0040; break;             /* EGL_CONFORMANT */
    default:     *value = 0; break;
    }
    return EGL_TRUE;
}

void *eglCreateContext(void *display, void *config, void *share_context,
                       const int *attrib_list) {
    FWD_RET_V(void*, eglCreateContext, (void*, void*, void*, const int*),
            (display, config, share_context, attrib_list));
    SHIM_LOG("eglCreateContext -> fake context");
    (void)display; (void)config; (void)share_context; (void)attrib_list;
    return FAKE_EGL_CONTEXT;
}

unsigned eglDestroyContext(void *display, void *context) {
    FWD_RET_V(unsigned, eglDestroyContext, (void*, void*), (display, context));
    (void)display; (void)context; return EGL_TRUE;
}

void *eglCreateWindowSurface(void *display, void *config, void *native_window,
                             const int *attrib_list) {
    /* Step 56.2 — in real-GL mode, window surfaces don't work because our
     * "native_window" is a fake GBM surface that Mesa's GBM platform cannot
     * consume (no real DRM device). Redirect to a surfaceless pbuffer of
     * screen size. Game sees an EGL surface and doesn't know the difference;
     * Mesa gets something it can bind. */
    if (is_real_gl()) {
        static void *(*real_pbuf)(void*, void*, const int*) = NULL;
        if (!real_pbuf) real_pbuf = (void*(*)(void*, void*, const int*))
                                    dlsym(RTLD_NEXT, "eglCreatePbufferSurface");
        if (real_pbuf) {
            int pb_attrs[] = {
                0x3057, FAKE_MODE_HDISPLAY,   /* EGL_WIDTH  */
                0x3056, FAKE_MODE_VDISPLAY,   /* EGL_HEIGHT */
                0x3038                         /* EGL_NONE   */
            };
            void *p = real_pbuf(display, config, pb_attrs);
            SHIM_LOG("eglCreateWindowSurface(native=%p) -> pbuffer %p (redirected %dx%d)",
                     native_window, p, FAKE_MODE_HDISPLAY, FAKE_MODE_VDISPLAY);
            (void)attrib_list;
            return p;
        }
        SHIM_LOG("eglCreateWindowSurface: no RTLD_NEXT eglCreatePbufferSurface; falling back to stub");
    }
    SHIM_LOG("eglCreateWindowSurface(native=%p) -> fake surface", native_window);
    (void)display; (void)config; (void)attrib_list;
    return FAKE_EGL_SURFACE;
}

void *eglCreatePbufferSurface(void *display, void *config, const int *attrib_list) {
    FWD_RET_V(void*, eglCreatePbufferSurface, (void*, void*, const int*),
            (display, config, attrib_list));
    (void)display; (void)config; (void)attrib_list;
    return FAKE_EGL_SURFACE;
}

unsigned eglDestroySurface(void *display, void *surface) {
    FWD_RET_V(unsigned, eglDestroySurface, (void*, void*), (display, surface));
    (void)display; (void)surface; return EGL_TRUE;
}

/* Tracked across EGL calls so we can lazily create a fallback pbuffer when
 * the game does surfaceless eglMakeCurrent(draw=NULL). Captured in
 * shim_real_platform_display (display) and eglChooseConfig (config).
 * Non-static so the forward decls upstream can find them. */
void *shim_last_real_display = NULL;
void *shim_last_real_config  = NULL;
void *shim_fallback_pbuf     = NULL;

unsigned eglMakeCurrent(void *display, void *draw, void *read, void *context) {
    if (__builtin_expect(is_real_gl(), 0)) {
        /* The game calls eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)
         * — a surfaceless context binding. On real hardware its eglCreateWindowSurface
         * path produces a GBM-backed window surface and `draw` wouldn't be NULL, but
         * in our run the game never calls eglCreateWindowSurface at all. Without a
         * drawing surface the default framebuffer reports GL_FRAMEBUFFER_UNDEFINED
         * (0x8219) and every glDrawArrays returns GL_INVALID_FRAMEBUFFER_OPERATION
         * (0x506) — exactly what we captured at SpriteRenderer.cpp:143.
         *
         * Fix: when the game passes NULL, substitute a pbuffer we create ourselves
         * from the first known-good EGLConfig. Gives the default framebuffer a real
         * backing so draws succeed. The game doesn't care what surface it drew to
         * because it never reads back — the present path is separate (DRM page flip
         * / EGLImage-sampled FBOs). */
        if (draw == NULL || read == NULL) {
            if (!shim_fallback_pbuf && display && shim_last_real_config) {
                static void *(*real_cps)(void*, void*, const int*) = NULL;
                if (!real_cps) real_cps = (void*(*)(void*, void*, const int*))
                                          dlsym(RTLD_NEXT, "eglCreatePbufferSurface");
                if (real_cps) {
                    int pb_attrs[] = {
                        0x3057, FAKE_MODE_HDISPLAY,
                        0x3056, FAKE_MODE_VDISPLAY,
                        0x3038,
                    };
                    shim_fallback_pbuf = real_cps(display, shim_last_real_config, pb_attrs);
                    SHIM_LOG("[mkcur] created fallback pbuffer %p (%dx%d) for surfaceless makeCurrent",
                             shim_fallback_pbuf, FAKE_MODE_HDISPLAY, FAKE_MODE_VDISPLAY);
                }
            }
            if (shim_fallback_pbuf) {
                if (draw == NULL) draw = shim_fallback_pbuf;
                if (read == NULL) read = shim_fallback_pbuf;
            }
        }

        static unsigned (*real)(void*, void*, void*, void*) = NULL;
        if (!real) real = (unsigned(*)(void*, void*, void*, void*))
                          dlsym(RTLD_NEXT, "eglMakeCurrent");
        SHIM_LOG("FWD eglMakeCurrent(dpy=%p, draw=%p, read=%p, ctx=%p)",
                 display, draw, read, context);
        unsigned r = real ? real(display, draw, read, context) : EGL_TRUE;
        if (!r) {
            SHIM_LOG("  → makeCurrent FAILED (returned 0)");
            return r;
        }

        /* Diagnostic — only if draw isn't NULL, otherwise we'd set EGL_BAD_SURFACE
         * and the game's next eglGetError would assert. */
        static unsigned (*real_gerr)(void)              = NULL;
        static unsigned (*real_cfb)(unsigned)           = NULL;
        static const unsigned char *(*real_gstr)(unsigned) = NULL;
        if (!real_gerr) real_gerr = (unsigned(*)(void))dlsym(RTLD_NEXT, "glGetError");
        if (!real_cfb)  real_cfb  = (unsigned(*)(unsigned))dlsym(RTLD_NEXT, "glCheckFramebufferStatus");
        if (!real_gstr) real_gstr = (const unsigned char*(*)(unsigned))dlsym(RTLD_NEXT, "glGetString");
        if (real_gerr) (void)real_gerr();
        unsigned fbstatus = real_cfb ? real_cfb(0x8D40 /*GL_FRAMEBUFFER*/) : 0;
        unsigned err_after = real_gerr ? real_gerr() : 0;
        SHIM_LOG("  glCheckFramebufferStatus(DEFAULT) = 0x%x (COMPLETE=0x8CD5), glGetError=0x%x",
                 fbstatus, err_after);
        if (real_gstr) {
            const unsigned char *ver = real_gstr(0x1F02);
            const unsigned char *rend = real_gstr(0x1F01);
            SHIM_LOG("  GL_VERSION='%s' GL_RENDERER='%s'",
                     ver ? (const char*)ver : "(null)",
                     rend ? (const char*)rend : "(null)");
        }
        return r;
    }
    SHIM_LOG("eglMakeCurrent(dpy=%p, draw=%p, read=%p, ctx=%p) -> EGL_TRUE (stub)",
             display, draw, read, context);
    return EGL_TRUE;
}

unsigned eglSwapBuffers(void *display, void *surface) {
    /* Plain forward — capture trigger moved to MODE_PAGE_FLIP (Step 58.2)
     * because this game uses DRM page-flip for presentation and never
     * actually calls eglSwapBuffers in practice. Left in place as a pure
     * forward so any future code path that DOES call it still works. */
    FWD_RET_V(unsigned, eglSwapBuffers, (void*, void*), (display, surface));
    (void)display; (void)surface; return EGL_TRUE;
}

unsigned eglSwapInterval(void *display, int interval) {
    FWD_RET_V(unsigned, eglSwapInterval, (void*, int), (display, interval));
    (void)display; (void)interval; return EGL_TRUE;
}

unsigned eglSurfaceAttrib(void *display, void *surface, int attribute, int value) {
    FWD_RET_V(unsigned, eglSurfaceAttrib, (void*, void*, int, int),
            (display, surface, attribute, value));
    (void)display; (void)surface; (void)attribute; (void)value; return EGL_TRUE;
}

unsigned eglQuerySurface(void *display, void *surface, int attribute, int *value) {
    FWD_RET_V(unsigned, eglQuerySurface, (void*, void*, int, int*),
            (display, surface, attribute, value));
    (void)display; (void)surface;
    if (!value) return EGL_FALSE;
    switch (attribute) {
    case 0x3057: *value = FAKE_MODE_HDISPLAY; break; /* EGL_WIDTH */
    case 0x3056: *value = FAKE_MODE_VDISPLAY; break; /* EGL_HEIGHT */
    default:     *value = 0; break;
    }
    return EGL_TRUE;
}

int eglGetError(void) {
    FWD_RET_V(int, eglGetError, (void), ());
    return EGL_SUCCESS;
}

/* Extension stubs resolved via eglGetProcAddress AND exported top-level so
 * direct PLT calls from the game also land here. The KHR image/sync family
 * plus glEGLImageTargetTexture2DOES are never forwarded to Mesa (Step 57's
 * invariant): Mesa's real impls would dereference fake GBM handles.
 *
 * eglCreateImageKHR in particular has a real job under SPIKE3_REAL_GL: the
 * game's create_egl_framebuffer (BN 0xd00800) builds a DMA-BUF attribute
 * list and expects the returned EGLImage to back an FBO color attachment.
 * Our GBM stubs don't produce real DMA-BUFs (gbm_bo_get_fd returns -1), so
 * we fake the zero-copy path with a regular GL texture: parse the attribute
 * list, return a unique sentinel, map {sentinel → {w,h,fourcc}}, and when
 * glEGLImageTargetTexture2DOES is called we look up and glTexImage2D(NULL)
 * the currently-bound texture to give it real storage. */

#define SHIM_EGL_IMAGE_MAP_SIZE 128
static struct shim_egl_image_entry {
    void    *egl_image;  /* sentinel returned by eglCreateImageKHR */
    int      width;
    int      height;
    unsigned fourcc;     /* DRM FOURCC from EGL_LINUX_DRM_FOURCC_EXT */
} shim_egl_image_map[SHIM_EGL_IMAGE_MAP_SIZE];
static int shim_egl_image_map_count = 0;
static pthread_mutex_t shim_egl_image_map_lock = PTHREAD_MUTEX_INITIALIZER;

/* EGLImage attribute decoder. Called with the int32 attribute array the
 * game hands to eglCreateImageKHR for a EGL_LINUX_DMA_BUF_EXT import.
 * Attribute enums: EGL_WIDTH=0x3057, EGL_HEIGHT=0x3056,
 * EGL_LINUX_DRM_FOURCC_EXT=0x3271, terminator EGL_NONE=0x3038. */
static void shim_egl_image_parse_attribs(const int *attrib_list,
                                         int *w, int *h, unsigned *fmt) {
    if (!attrib_list) return;
    for (int i = 0; attrib_list[i] != 0x3038 && i < 32; i += 2) {
        switch (attrib_list[i]) {
            case 0x3057: *w   = attrib_list[i+1]; break;
            case 0x3056: *h   = attrib_list[i+1]; break;
            case 0x3271: *fmt = (unsigned)attrib_list[i+1]; break;
        }
    }
}

static void *shim_egl_image_register(int w, int h, unsigned fmt) {
    void *sentinel = malloc(1);  /* unique non-null pointer — game treats as opaque */
    if (!sentinel) return FAKE_EGL_IMAGE;
    pthread_mutex_lock(&shim_egl_image_map_lock);
    if (shim_egl_image_map_count >= SHIM_EGL_IMAGE_MAP_SIZE) {
        pthread_mutex_unlock(&shim_egl_image_map_lock);
        SHIM_LOG("[egl_image] map full (%d); returning FAKE_EGL_IMAGE",
                 SHIM_EGL_IMAGE_MAP_SIZE);
        free(sentinel);
        return FAKE_EGL_IMAGE;
    }
    shim_egl_image_map[shim_egl_image_map_count++] = (struct shim_egl_image_entry){
        .egl_image = sentinel, .width = w, .height = h, .fourcc = fmt,
    };
    pthread_mutex_unlock(&shim_egl_image_map_lock);
    return sentinel;
}

static int shim_egl_image_lookup(void *image, int *w, int *h, unsigned *fmt) {
    pthread_mutex_lock(&shim_egl_image_map_lock);
    for (int i = 0; i < shim_egl_image_map_count; i++) {
        if (shim_egl_image_map[i].egl_image == image) {
            *w = shim_egl_image_map[i].width;
            *h = shim_egl_image_map[i].height;
            *fmt = shim_egl_image_map[i].fourcc;
            pthread_mutex_unlock(&shim_egl_image_map_lock);
            return 1;
        }
    }
    pthread_mutex_unlock(&shim_egl_image_map_lock);
    return 0;
}

/* Top-level exports — reachable both via eglGetProcAddress table AND via
 * direct PLT-linked calls from the game. Note the int-based attrib_list
 * signature (KHR extension uses EGLint, not EGLAttrib intptr_t). */
void *eglCreateImageKHR(void *display, void *ctx, unsigned target,
                        void *buffer, const int *attrib_list) {
    int      w = 0, h = 0;
    unsigned fmt = 0x34325258;  /* XR24 — matches GBM default */
    shim_egl_image_parse_attribs(attrib_list, &w, &h, &fmt);
    /* Sanity-clamp to our render mode if attribs were malformed. */
    if (w <= 0 || w > 8192)  w = FAKE_MODE_HDISPLAY;
    if (h <= 0 || h > 8192)  h = FAKE_MODE_VDISPLAY;
    void *img = shim_egl_image_register(w, h, fmt);
    SHIM_LOG("eglCreateImageKHR(dpy=%p, target=0x%x, buf=%p) "
             "[w=%d h=%d fourcc=0x%x] -> %p",
             display, target, buffer, w, h, fmt, img);
    (void)ctx;
    return img;
}

unsigned eglDestroyImageKHR(void *display, void *image) {
    /* Free the sentinel and remove from map. We don't glDeleteTextures the
     * backing — the game's glFramebufferTexture2D has already stored a ref,
     * and we don't track texture lifetimes. Bounded leak by map size. */
    pthread_mutex_lock(&shim_egl_image_map_lock);
    for (int i = 0; i < shim_egl_image_map_count; i++) {
        if (shim_egl_image_map[i].egl_image == image) {
            free(shim_egl_image_map[i].egl_image);
            shim_egl_image_map[i] = shim_egl_image_map[--shim_egl_image_map_count];
            break;
        }
    }
    pthread_mutex_unlock(&shim_egl_image_map_lock);
    SHIM_LOG("eglDestroyImageKHR(%p)", image);
    (void)display;
    return EGL_TRUE;
}

void glEGLImageTargetTexture2DOES(unsigned target, void *image) {
    int w = 0, h = 0; unsigned fmt = 0;
    if (!shim_egl_image_lookup(image, &w, &h, &fmt)) {
        SHIM_LOG("glEGLImageTargetTexture2DOES: unknown image %p, no-op", image);
        return;
    }
    /* Redefine the currently-bound texture's storage via Mesa's glTexImage2D.
     * This is the non-zero-copy substitute for a DMA-BUF-backed EGLImage.
     * The currently-bound texture (on 'target') becomes a w×h RGBA8 texture;
     * the game's subsequent glFramebufferTexture2D attaches it to the FBO. */
    static void (*real_glTexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void*) = NULL;
    if (!real_glTexImage2D) real_glTexImage2D = (void (*)(unsigned, int, int, int, int, int, unsigned, unsigned, const void*))
                                                 dlsym(RTLD_NEXT, "glTexImage2D");
    if (!real_glTexImage2D) {
        SHIM_LOG("glEGLImageTargetTexture2DOES: glTexImage2D unresolved; no-op");
        return;
    }
    /* GL_RGBA8 = 0x8058 internal, GL_RGBA = 0x1908 format, GL_UNSIGNED_BYTE = 0x1401. */
    real_glTexImage2D(target, 0, 0x8058, w, h, 0, 0x1908, 0x1401, NULL);
    SHIM_LOG("glEGLImageTargetTexture2DOES(target=0x%x, image=%p) -> glTexImage2D %dx%d RGBA8",
             target, image, w, h);
    /* Register for frame capture at MODE_PAGE_FLIP. */
    shim_frametex_record(target, w, h);
}

/* Sync extensions — still pure stubs; no real work needed. */
static void *stub_eglCreateSyncKHR(void *display, unsigned type, const intptr_t *attrib_list) {
    (void)display; (void)type; (void)attrib_list; return FAKE_EGL_SYNC;
}
static unsigned stub_eglDestroySyncKHR(void *display, void *sync) {
    (void)display; (void)sync; return EGL_TRUE;
}
static int stub_eglWaitSyncKHR(void *display, void *sync, int flags) {
    (void)display; (void)sync; (void)flags; return EGL_TRUE;
}
static int stub_eglClientWaitSyncKHR(void *display, void *sync, int flags, uint64_t timeout) {
    (void)display; (void)sync; (void)flags; (void)timeout;
    return 0x30F6;  /* EGL_CONDITION_SATISFIED_KHR */
}

/* ---- Hooked: GLES 3.x ----
 *
 * Two modes, selected by SPIKE3_REAL_GL env var (Step 56):
 *
 * 1. Unset (default): pure no-op stubs. Game's rendering calls all succeed
 *    with plausible values, no actual rendering happens. Used for all of
 *    Steps 1-55 — reaching steady state without needing a real GL stack.
 *    - glGen* returns an incrementing handle
 *    - glGetShaderiv/glGetProgramiv return GL_TRUE for COMPILE_STATUS /
 *      LINK_STATUS
 *    - glCheckFramebufferStatus returns GL_FRAMEBUFFER_COMPLETE
 *    - glGetError returns GL_NO_ERROR
 *    - glGetString advertises GLES 3.1 + common OES/EXT extensions
 *
 * 2. Set: each stub forwards to the real Mesa implementation via
 *    dlsym(RTLD_NEXT, ...) (cached per-function). Expects the Mesa overlay
 *    at mesa_sw_overlay/pkg/usr/lib/aarch64-linux-gnu/ to be on
 *    LD_LIBRARY_PATH. See _emulation/SETUP.md §3b.
 */

#define GL_NO_ERROR              0
#define GL_TRUE                  1
#define GL_FALSE                 0
#define GL_FRAMEBUFFER_COMPLETE  0x8CD5
#define GL_VENDOR                0x1F00
#define GL_RENDERER              0x1F01
#define GL_VERSION               0x1F02
#define GL_EXTENSIONS            0x1F03
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#define GL_COMPILE_STATUS        0x8B81
#define GL_LINK_STATUS           0x8B82
#define GL_INFO_LOG_LENGTH       0x8B84
#define GL_ACTIVE_UNIFORMS       0x8B86
#define GL_ACTIVE_ATTRIBUTES     0x8B89
#define GL_MAX_TEXTURE_SIZE      0x0D33
#define GL_MAX_VIEWPORT_DIMS     0x0D3A
#define GL_MAX_TEXTURE_IMAGE_UNITS 0x8872
#define GL_MAX_VERTEX_ATTRIBS    0x8869

static uint32_t gl_next_tex_id     = 1;
static uint32_t gl_next_buf_id     = 1;
static uint32_t gl_next_fb_id      = 1;
static uint32_t gl_next_rb_id      = 1;
static uint32_t gl_next_vao_id     = 1;
static uint32_t gl_next_shader_id  = 1;
static uint32_t gl_next_program_id = 1;

/* Logging is selective — GL calls can be extremely high-volume during
 * draw. Only the first N of each kind, and first-time calls for creation
 * functions. */
static int gl_log_budget_object_create = 40;
static int gl_log_budget_draw          = 20;
static int gl_log_budget_state         = 40;
#define GLLOG(kind, fmt, ...) do { \
    if (gl_log_budget_##kind > 0) { \
        gl_log_budget_##kind--; \
        SHIM_LOG("GL " fmt, ##__VA_ARGS__); \
    } \
} while (0)

/* --- Query / error --- */

unsigned glGetError(void) {
    FWD_RET(unsigned, glGetError, (void), ());
    return GL_NO_ERROR;
}

const unsigned char *glGetString(unsigned name) {
    FWD_RET(const unsigned char*, glGetString, (unsigned), (name));
    switch (name) {
    case GL_VERSION:                  return (const unsigned char *)"OpenGL ES 3.1 (stub)";
    case GL_VENDOR:                   return (const unsigned char *)"shim";
    case GL_RENDERER:                 return (const unsigned char *)"shim GLES3";
    case GL_SHADING_LANGUAGE_VERSION: return (const unsigned char *)"OpenGL ES GLSL ES 3.10";
    case GL_EXTENSIONS:
        return (const unsigned char *)
            "GL_OES_EGL_image GL_OES_EGL_image_external "
            "GL_OES_texture_float GL_OES_texture_half_float "
            "GL_OES_vertex_array_object GL_OES_mapbuffer "
            "GL_EXT_texture_format_BGRA8888 GL_EXT_discard_framebuffer "
            "GL_EXT_color_buffer_float GL_EXT_blend_minmax";
    default: return (const unsigned char *)"";
    }
}

const unsigned char *glGetStringi(unsigned name, unsigned index) {
    FWD_RET(const unsigned char*, glGetStringi, (unsigned, unsigned), (name, index));
    (void)name; (void)index; return (const unsigned char *)"";
}

/* Log each unknown glGetIntegerv pname exactly once via a 4KB bitmap keyed
 * on the low 12 bits of the pname. Only used by the stub path. */
static unsigned char gl_getint_seen[4096];
static void gl_getint_log_unknown(unsigned pname) {
    unsigned slot = pname & 0xFFF;
    if (gl_getint_seen[slot]) return;
    gl_getint_seen[slot] = 1;
    SHIM_LOG("GL glGetIntegerv(pname=0x%x) -> 0 (unknown, defaulting)", pname);
}

void glGetIntegerv(unsigned pname, int *params) {
    FWD_VOID(glGetIntegerv, (unsigned, int*), (pname, params));
    if (!params) return;
    switch (pname) {
    case GL_MAX_TEXTURE_SIZE:                    *params = 4096; break;
    case GL_MAX_VIEWPORT_DIMS:                   params[0] = 4096; params[1] = 4096; break;
    case 0x851C: *params = 2048; break;    /* GL_MAX_CUBE_MAP_TEXTURE_SIZE */
    case 0x84E8: *params = 4096; break;    /* GL_MAX_RENDERBUFFER_SIZE */
    case 0x88FF: *params = 2048; break;    /* GL_MAX_3D_TEXTURE_SIZE */
    case 0x88FE: *params = 256;  break;    /* GL_MAX_ARRAY_TEXTURE_LAYERS */
    case GL_MAX_TEXTURE_IMAGE_UNITS:             *params = 16; break;
    case 0x8B4C: *params = 16; break;      /* GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS */
    case 0x8B4D: *params = 32; break;      /* GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS */
    case GL_MAX_VERTEX_ATTRIBS:                  *params = 16; break;
    case 0x8DFB: *params = 256;  break;    /* GL_MAX_VERTEX_UNIFORM_VECTORS */
    case 0x8A2B: *params = 1024; break;    /* GL_MAX_VERTEX_UNIFORM_COMPONENTS */
    case 0x8B4A: *params = 12;   break;    /* GL_MAX_VERTEX_UNIFORM_BLOCKS */
    case 0x8B4B: *params = 60;   break;    /* GL_MAX_VARYING_COMPONENTS */
    case 0x8DFC: *params = 15;   break;    /* GL_MAX_VARYING_VECTORS */
    case 0x9122: *params = 64;   break;    /* GL_MAX_VERTEX_OUTPUT_COMPONENTS */
    case 0x8DFD: *params = 224;  break;    /* GL_MAX_FRAGMENT_UNIFORM_VECTORS */
    case 0x8B49: *params = 896;  break;    /* GL_MAX_FRAGMENT_UNIFORM_COMPONENTS */
    case 0x8A2D: *params = 12;   break;    /* GL_MAX_FRAGMENT_UNIFORM_BLOCKS */
    case 0x9125: *params = 60;   break;    /* GL_MAX_FRAGMENT_INPUT_COMPONENTS */
    case 0x8CDF: *params = 4;    break;    /* GL_MAX_COLOR_ATTACHMENTS */
    case 0x8824: *params = 4;    break;    /* GL_MAX_DRAW_BUFFERS */
    case 0x8D57: *params = 4;    break;    /* GL_MAX_SAMPLES */
    case 0x8A2E: *params = 24;   break;    /* GL_MAX_COMBINED_UNIFORM_BLOCKS */
    case 0x8A2F: *params = 24;   break;    /* GL_MAX_UNIFORM_BUFFER_BINDINGS */
    case 0x8A30: *params = 16384;break;    /* GL_MAX_UNIFORM_BLOCK_SIZE */
    case 0x8A34: *params = 256;  break;    /* GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT */
    case 0x87FE: *params = 0;    break;    /* GL_NUM_PROGRAM_BINARY_FORMATS */
    default: *params = 0; gl_getint_log_unknown(pname); break;
    }
}
void glGetFloatv(unsigned pname, float *params) {
    FWD_VOID(glGetFloatv, (unsigned, float*), (pname, params));
    if (params) *params = 0.0f;
    (void)pname;
}
void glGetBooleanv(unsigned pname, unsigned char *params) {
    FWD_VOID(glGetBooleanv, (unsigned, unsigned char*), (pname, params));
    if (params) *params = GL_FALSE;
    (void)pname;
}

/* --- Object creation --- */

void glGenTextures(int n, unsigned *out) {
    FWD_VOID(glGenTextures, (int, unsigned*), (n, out));
    for (int i = 0; i < n; i++) out[i] = gl_next_tex_id++;
    GLLOG(object_create, "glGenTextures(%d) -> [%u..]", n, n ? out[0] : 0);
}
void glDeleteTextures(int n, const unsigned *ids) {
    FWD_VOID(glDeleteTextures, (int, const unsigned*), (n, ids));
    (void)n; (void)ids;
}
void glGenBuffers(int n, unsigned *out) {
    FWD_VOID(glGenBuffers, (int, unsigned*), (n, out));
    for (int i = 0; i < n; i++) out[i] = gl_next_buf_id++;
    GLLOG(object_create, "glGenBuffers(%d) -> [%u..]", n, n ? out[0] : 0);
}
void glDeleteBuffers(int n, const unsigned *ids) {
    FWD_VOID(glDeleteBuffers, (int, const unsigned*), (n, ids));
    (void)n; (void)ids;
}
/* Step 58 debug: prove which FBO/texture paths the game actually exercises.
 * Each of these hooks logs its first few invocations then falls silent. */
#define SHIM_GL_DBG_HOOK(NAME) \
    do { static uint64_t _c = 0; \
         if (__builtin_expect(is_real_gl(), 0) && ++_c <= 4) \
             SHIM_LOG("[gl] " #NAME " #%lu entered", (unsigned long)_c); \
    } while (0)

void glGenFramebuffers(int n, unsigned *out) {
    SHIM_GL_DBG_HOOK(glGenFramebuffers);
    FWD_VOID(glGenFramebuffers, (int, unsigned*), (n, out));
    for (int i = 0; i < n; i++) out[i] = gl_next_fb_id++;
    GLLOG(object_create, "glGenFramebuffers(%d) -> [%u..]", n, n ? out[0] : 0);
}
void glDeleteFramebuffers(int n, const unsigned *ids) {
    FWD_VOID(glDeleteFramebuffers, (int, const unsigned*), (n, ids));
    (void)n; (void)ids;
}
void glGenRenderbuffers(int n, unsigned *out) {
    FWD_VOID(glGenRenderbuffers, (int, unsigned*), (n, out));
    for (int i = 0; i < n; i++) out[i] = gl_next_rb_id++;
    GLLOG(object_create, "glGenRenderbuffers(%d) -> [%u..]", n, n ? out[0] : 0);
}
void glDeleteRenderbuffers(int n, const unsigned *ids) {
    FWD_VOID(glDeleteRenderbuffers, (int, const unsigned*), (n, ids));
    (void)n; (void)ids;
}
void glGenVertexArrays(int n, unsigned *out) {
    FWD_VOID(glGenVertexArrays, (int, unsigned*), (n, out));
    for (int i = 0; i < n; i++) out[i] = gl_next_vao_id++;
    GLLOG(object_create, "glGenVertexArrays(%d) -> [%u..]", n, n ? out[0] : 0);
}
void glDeleteVertexArrays(int n, const unsigned *ids) {
    FWD_VOID(glDeleteVertexArrays, (int, const unsigned*), (n, ids));
    (void)n; (void)ids;
}

unsigned glCreateShader(unsigned type) {
    FWD_RET(unsigned, glCreateShader, (unsigned), (type));
    unsigned id = gl_next_shader_id++;
    GLLOG(object_create, "glCreateShader(type=0x%x) -> %u", type, id);
    return id;
}
void glDeleteShader(unsigned shader) {
    FWD_VOID(glDeleteShader, (unsigned), (shader));
    (void)shader;
}
unsigned glCreateProgram(void) {
    FWD_RET(unsigned, glCreateProgram, (void), ());
    unsigned id = gl_next_program_id++;
    GLLOG(object_create, "glCreateProgram() -> %u", id);
    return id;
}
void glDeleteProgram(unsigned program) {
    FWD_VOID(glDeleteProgram, (unsigned), (program));
    (void)program;
}

/* --- Binding --- */

void glBindTexture(unsigned target, unsigned tex) {
    FWD_VOID(glBindTexture, (unsigned, unsigned), (target, tex));
    (void)target; (void)tex;
}
void glBindBuffer(unsigned target, unsigned buf) {
    FWD_VOID(glBindBuffer, (unsigned, unsigned), (target, buf));
    (void)target; (void)buf;
}
/* Track the game's most recent draw FBO so the MODE_PAGE_FLIP capture hook
 * has something to read from even after the game unbinds (our pbuffer read
 * at fb=0 is empty because the game isn't compositing into it). Ignore
 * binds to 0 (unbind) and to our own capture FBO. Thread-safe via atomic.
 * Forward-declared as extern alongside shim_frame_capture_at_page_flip. */
volatile unsigned shim_last_draw_fbo = 0;

void glBindFramebuffer(unsigned target, unsigned fb) {
    if (__builtin_expect(is_real_gl(), 0)) {
        /* Count + occasional log — diagnostic for whether this hook fires at all. */
        static uint64_t bind_count = 0;
        uint64_t c = __atomic_add_fetch(&bind_count, 1, __ATOMIC_RELAXED);
        if (c <= 20 || (c & 1023) == 0)
            SHIM_LOG("[gl] glBindFramebuffer #%lu target=0x%x fb=%u", (unsigned long)c, target, fb);
        if ((target == 0x8D40 /* GL_FRAMEBUFFER */ ||
             target == 0x8CA9 /* GL_DRAW_FRAMEBUFFER */) &&
            fb != 0 && fb != shim_capture_fbo) {
            __atomic_store_n(&shim_last_draw_fbo, fb, __ATOMIC_RELAXED);
        }
    }
    FWD_VOID(glBindFramebuffer, (unsigned, unsigned), (target, fb));
    (void)target; (void)fb;
}
void glBindRenderbuffer(unsigned target, unsigned rb) {
    FWD_VOID(glBindRenderbuffer, (unsigned, unsigned), (target, rb));
    (void)target; (void)rb;
}
void glBindVertexArray(unsigned array) {
    FWD_VOID(glBindVertexArray, (unsigned), (array));
    (void)array;
}
void glActiveTexture(unsigned tex) {
    FWD_VOID(glActiveTexture, (unsigned), (tex));
    (void)tex;
}

/* --- Texture data / params --- */

void glTexImage2D(unsigned target, int level, int internalfmt,
                  int w, int h, int border, unsigned format, unsigned type,
                  const void *data) {
    FWD_VOID(glTexImage2D,
             (unsigned, int, int, int, int, int, unsigned, unsigned, const void*),
             (target, level, internalfmt, w, h, border, format, type, data));
    GLLOG(object_create, "glTexImage2D %dx%d internal=0x%x fmt=0x%x type=0x%x data=%s",
          w, h, internalfmt, format, type, data ? "<ptr>" : "NULL");
    (void)target; (void)level; (void)border;
}
void glTexSubImage2D(unsigned target, int level, int xoff, int yoff,
                     int w, int h, unsigned format, unsigned type,
                     const void *data) {
    FWD_VOID(glTexSubImage2D,
             (unsigned, int, int, int, int, int, unsigned, unsigned, const void*),
             (target, level, xoff, yoff, w, h, format, type, data));
    (void)target; (void)level; (void)xoff; (void)yoff;
    (void)w; (void)h; (void)format; (void)type; (void)data;
}
void glCompressedTexImage2D(unsigned target, int level, unsigned internalfmt,
                            int w, int h, int border, int size, const void *data) {
    FWD_VOID(glCompressedTexImage2D,
             (unsigned, int, unsigned, int, int, int, int, const void*),
             (target, level, internalfmt, w, h, border, size, data));
    (void)target; (void)level; (void)internalfmt;
    (void)w; (void)h; (void)border; (void)size; (void)data;
}
void glCompressedTexSubImage2D(unsigned target, int level, int xoff, int yoff,
                               int w, int h, unsigned format, int size,
                               const void *data) {
    FWD_VOID(glCompressedTexSubImage2D,
             (unsigned, int, int, int, int, int, unsigned, int, const void*),
             (target, level, xoff, yoff, w, h, format, size, data));
    (void)target; (void)level; (void)xoff; (void)yoff;
    (void)w; (void)h; (void)format; (void)size; (void)data;
}
void glTexParameteri(unsigned target, unsigned pname, int param) {
    FWD_VOID(glTexParameteri, (unsigned, unsigned, int), (target, pname, param));
    (void)target; (void)pname; (void)param;
}
void glTexParameterf(unsigned target, unsigned pname, float param) {
    FWD_VOID(glTexParameterf, (unsigned, unsigned, float), (target, pname, param));
    (void)target; (void)pname; (void)param;
}
void glPixelStorei(unsigned pname, int param) {
    FWD_VOID(glPixelStorei, (unsigned, int), (pname, param));
    (void)pname; (void)param;
}
void glGenerateMipmap(unsigned target) {
    FWD_VOID(glGenerateMipmap, (unsigned), (target));
    (void)target;
}

/* --- Buffer data --- */

void glBufferData(unsigned target, intptr_t size, const void *data, unsigned usage) {
    FWD_VOID(glBufferData, (unsigned, intptr_t, const void*, unsigned),
             (target, size, data, usage));
    (void)target; (void)size; (void)data; (void)usage;
}
void glBufferSubData(unsigned target, intptr_t offset, intptr_t size, const void *data) {
    FWD_VOID(glBufferSubData, (unsigned, intptr_t, intptr_t, const void*),
             (target, offset, size, data));
    (void)target; (void)offset; (void)size; (void)data;
}
void *glMapBufferRange(unsigned target, intptr_t offset, intptr_t len, unsigned access) {
    FWD_RET(void*, glMapBufferRange, (unsigned, intptr_t, intptr_t, unsigned),
            (target, offset, len, access));
    (void)target; (void)offset; (void)len; (void)access; return NULL;
}
unsigned char glUnmapBuffer(unsigned target) {
    FWD_RET(unsigned char, glUnmapBuffer, (unsigned), (target));
    (void)target; return GL_TRUE;
}

/* --- Shader --- */

void glShaderSource(unsigned shader, int count, const char *const *string, const int *length) {
    FWD_VOID(glShaderSource, (unsigned, int, const char* const*, const int*),
             (shader, count, string, length));
    (void)shader; (void)count; (void)string; (void)length;
}
void glCompileShader(unsigned shader) {
    FWD_VOID(glCompileShader, (unsigned), (shader));
    (void)shader;
}
void glAttachShader(unsigned program, unsigned shader) {
    FWD_VOID(glAttachShader, (unsigned, unsigned), (program, shader));
    (void)program; (void)shader;
}
void glDetachShader(unsigned program, unsigned shader) {
    FWD_VOID(glDetachShader, (unsigned, unsigned), (program, shader));
    (void)program; (void)shader;
}
void glLinkProgram(unsigned program) {
    FWD_VOID(glLinkProgram, (unsigned), (program));
    (void)program;
}
void glValidateProgram(unsigned program) {
    FWD_VOID(glValidateProgram, (unsigned), (program));
    (void)program;
}
void glUseProgram(unsigned program) {
    SHIM_GL_DBG_HOOK(glUseProgram);
    FWD_VOID(glUseProgram, (unsigned), (program));
    (void)program;
}

void glGetShaderiv(unsigned shader, unsigned pname, int *params) {
    FWD_VOID(glGetShaderiv, (unsigned, unsigned, int*), (shader, pname, params));
    (void)shader;
    if (!params) return;
    switch (pname) {
    case GL_COMPILE_STATUS:  *params = GL_TRUE; break;
    case GL_INFO_LOG_LENGTH: *params = 0; break;
    default:                 *params = 0; break;
    }
}
void glGetProgramiv(unsigned program, unsigned pname, int *params) {
    FWD_VOID(glGetProgramiv, (unsigned, unsigned, int*), (program, pname, params));
    (void)program;
    if (!params) return;
    switch (pname) {
    case GL_LINK_STATUS:       *params = GL_TRUE; break;
    case GL_ACTIVE_UNIFORMS:   *params = 0; break;
    case GL_ACTIVE_ATTRIBUTES: *params = 0; break;
    case GL_INFO_LOG_LENGTH:   *params = 0; break;
    default:                   *params = 0; break;
    }
}
void glGetShaderInfoLog(unsigned shader, int max, int *len, char *log) {
    FWD_VOID(glGetShaderInfoLog, (unsigned, int, int*, char*),
             (shader, max, len, log));
    (void)shader; (void)max;
    if (len) *len = 0;
    if (log && max > 0) log[0] = 0;
}
void glGetProgramInfoLog(unsigned program, int max, int *len, char *log) {
    FWD_VOID(glGetProgramInfoLog, (unsigned, int, int*, char*),
             (program, max, len, log));
    (void)program; (void)max;
    if (len) *len = 0;
    if (log && max > 0) log[0] = 0;
}

int glGetUniformLocation(unsigned program, const char *name) {
    FWD_RET(int, glGetUniformLocation, (unsigned, const char*), (program, name));
    (void)program; (void)name; return 0;
}
int glGetAttribLocation(unsigned program, const char *name) {
    FWD_RET(int, glGetAttribLocation, (unsigned, const char*), (program, name));
    (void)program; (void)name; return 0;
}
void glBindAttribLocation(unsigned program, unsigned index, const char *name) {
    FWD_VOID(glBindAttribLocation, (unsigned, unsigned, const char*),
             (program, index, name));
    (void)program; (void)index; (void)name;
}

/* Uniforms — all no-op in stub mode, forwarded in real mode. */
void glUniform1i(int loc, int v0) {
    FWD_VOID(glUniform1i, (int, int), (loc, v0));
    (void)loc; (void)v0;
}
void glUniform2i(int loc, int v0, int v1) {
    FWD_VOID(glUniform2i, (int, int, int), (loc, v0, v1));
    (void)loc; (void)v0; (void)v1;
}
void glUniform3i(int loc, int v0, int v1, int v2) {
    FWD_VOID(glUniform3i, (int, int, int, int), (loc, v0, v1, v2));
    (void)loc; (void)v0; (void)v1; (void)v2;
}
void glUniform4i(int loc, int v0, int v1, int v2, int v3) {
    FWD_VOID(glUniform4i, (int, int, int, int, int), (loc, v0, v1, v2, v3));
    (void)loc; (void)v0; (void)v1; (void)v2; (void)v3;
}
void glUniform1f(int loc, float v0) {
    FWD_VOID(glUniform1f, (int, float), (loc, v0));
    (void)loc; (void)v0;
}
void glUniform2f(int loc, float v0, float v1) {
    FWD_VOID(glUniform2f, (int, float, float), (loc, v0, v1));
    (void)loc; (void)v0; (void)v1;
}
void glUniform3f(int loc, float v0, float v1, float v2) {
    FWD_VOID(glUniform3f, (int, float, float, float), (loc, v0, v1, v2));
    (void)loc; (void)v0; (void)v1; (void)v2;
}
void glUniform4f(int loc, float v0, float v1, float v2, float v3) {
    FWD_VOID(glUniform4f, (int, float, float, float, float), (loc, v0, v1, v2, v3));
    (void)loc; (void)v0; (void)v1; (void)v2; (void)v3;
}
void glUniform1iv(int loc, int count, const int *v) {
    FWD_VOID(glUniform1iv, (int, int, const int*), (loc, count, v));
    (void)loc; (void)count; (void)v;
}
void glUniform1fv(int loc, int count, const float *v) {
    FWD_VOID(glUniform1fv, (int, int, const float*), (loc, count, v));
    (void)loc; (void)count; (void)v;
}
void glUniform2fv(int loc, int count, const float *v) {
    FWD_VOID(glUniform2fv, (int, int, const float*), (loc, count, v));
    (void)loc; (void)count; (void)v;
}
void glUniform3fv(int loc, int count, const float *v) {
    FWD_VOID(glUniform3fv, (int, int, const float*), (loc, count, v));
    (void)loc; (void)count; (void)v;
}
void glUniform4fv(int loc, int count, const float *v) {
    FWD_VOID(glUniform4fv, (int, int, const float*), (loc, count, v));
    (void)loc; (void)count; (void)v;
}
void glUniformMatrix3fv(int loc, int count, unsigned char trans, const float *v) {
    FWD_VOID(glUniformMatrix3fv, (int, int, unsigned char, const float*),
             (loc, count, trans, v));
    (void)loc; (void)count; (void)trans; (void)v;
}
void glUniformMatrix4fv(int loc, int count, unsigned char trans, const float *v) {
    FWD_VOID(glUniformMatrix4fv, (int, int, unsigned char, const float*),
             (loc, count, trans, v));
    (void)loc; (void)count; (void)trans; (void)v;
}

/* Vertex attribs */
void glVertexAttribPointer(unsigned idx, int size, unsigned type, unsigned char norm,
                           int stride, const void *ptr) {
    FWD_VOID(glVertexAttribPointer,
             (unsigned, int, unsigned, unsigned char, int, const void*),
             (idx, size, type, norm, stride, ptr));
    (void)idx; (void)size; (void)type; (void)norm; (void)stride; (void)ptr;
}
void glEnableVertexAttribArray(unsigned idx) {
    FWD_VOID(glEnableVertexAttribArray, (unsigned), (idx));
    (void)idx;
}
void glDisableVertexAttribArray(unsigned idx) {
    FWD_VOID(glDisableVertexAttribArray, (unsigned), (idx));
    (void)idx;
}

/* --- Framebuffer --- */

void glFramebufferTexture2D(unsigned target, unsigned att, unsigned ttarget,
                            unsigned tex, int level) {
    if (__builtin_expect(is_real_gl(), 0)) {
        static uint64_t c = 0;
        if (++c <= 6) SHIM_LOG("[gl] glFramebufferTexture2D #%lu target=0x%x att=0x%x ttarget=0x%x tex=%u",
                               (unsigned long)c, target, att, ttarget, tex);
        if (ttarget == 0x8D65) ttarget = 0x0DE1;  /* EXTERNAL_OES → TEXTURE_2D */
    }
    FWD_VOID(glFramebufferTexture2D, (unsigned, unsigned, unsigned, unsigned, int),
             (target, att, ttarget, tex, level));
    (void)target; (void)att; (void)ttarget; (void)tex; (void)level;
}
/* GLES3 FBO attach variant — same EXTERNAL_OES→TEXTURE_2D swap rationale.
 * Game binary imports this symbol (verified via readelf --dyn-syms). Exported
 * so both PLT path and our eglGetProcAddress table route here. */
void glFramebufferTextureLayer(unsigned target, unsigned att, unsigned tex,
                               int level, int layer) {
    if (__builtin_expect(is_real_gl(), 0)) {
        static uint64_t c = 0;
        if (++c <= 6) SHIM_LOG("[gl] glFramebufferTextureLayer #%lu target=0x%x att=0x%x tex=%u layer=%d",
                               (unsigned long)c, target, att, tex, layer);
    }
    FWD_VOID(glFramebufferTextureLayer, (unsigned, unsigned, unsigned, int, int),
             (target, att, tex, level, layer));
    (void)target; (void)att; (void)tex; (void)level; (void)layer;
}
void glFramebufferRenderbuffer(unsigned target, unsigned att, unsigned rbtarget, unsigned rb) {
    SHIM_GL_DBG_HOOK(glFramebufferRenderbuffer);
    FWD_VOID(glFramebufferRenderbuffer, (unsigned, unsigned, unsigned, unsigned),
             (target, att, rbtarget, rb));
    (void)target; (void)att; (void)rbtarget; (void)rb;
}
void glRenderbufferStorage(unsigned target, unsigned internalfmt, int w, int h) {
    FWD_VOID(glRenderbufferStorage, (unsigned, unsigned, int, int),
             (target, internalfmt, w, h));
    (void)target; (void)internalfmt; (void)w; (void)h;
}
unsigned glCheckFramebufferStatus(unsigned target) {
    FWD_RET(unsigned, glCheckFramebufferStatus, (unsigned), (target));
    (void)target; return GL_FRAMEBUFFER_COMPLETE;
}

/* --- State --- */

void glEnable(unsigned cap) {
    FWD_VOID(glEnable, (unsigned), (cap));
    GLLOG(state, "glEnable(0x%x)", cap);
}
void glDisable(unsigned cap) {
    FWD_VOID(glDisable, (unsigned), (cap));
    GLLOG(state, "glDisable(0x%x)", cap);
}
unsigned char glIsEnabled(unsigned cap) {
    FWD_RET(unsigned char, glIsEnabled, (unsigned), (cap));
    (void)cap; return GL_FALSE;
}
void glBlendFunc(unsigned src, unsigned dst) {
    FWD_VOID(glBlendFunc, (unsigned, unsigned), (src, dst));
    (void)src; (void)dst;
}
void glBlendFuncSeparate(unsigned srcRGB, unsigned dstRGB, unsigned srcA, unsigned dstA) {
    FWD_VOID(glBlendFuncSeparate, (unsigned, unsigned, unsigned, unsigned),
             (srcRGB, dstRGB, srcA, dstA));
    (void)srcRGB; (void)dstRGB; (void)srcA; (void)dstA;
}
void glBlendEquation(unsigned mode) {
    FWD_VOID(glBlendEquation, (unsigned), (mode));
    (void)mode;
}
void glBlendEquationSeparate(unsigned rgb, unsigned a) {
    FWD_VOID(glBlendEquationSeparate, (unsigned, unsigned), (rgb, a));
    (void)rgb; (void)a;
}
void glBlendColor(float r, float g, float b, float a) {
    FWD_VOID(glBlendColor, (float, float, float, float), (r, g, b, a));
    (void)r; (void)g; (void)b; (void)a;
}
void glViewport(int x, int y, int w, int h) {
    FWD_VOID(glViewport, (int, int, int, int), (x, y, w, h));
    (void)x; (void)y; (void)w; (void)h;
}
void glScissor(int x, int y, int w, int h) {
    FWD_VOID(glScissor, (int, int, int, int), (x, y, w, h));
    (void)x; (void)y; (void)w; (void)h;
}
void glDepthFunc(unsigned func) {
    FWD_VOID(glDepthFunc, (unsigned), (func));
    (void)func;
}
void glDepthMask(unsigned char mask) {
    FWD_VOID(glDepthMask, (unsigned char), (mask));
    (void)mask;
}
void glDepthRangef(float n, float f) {
    FWD_VOID(glDepthRangef, (float, float), (n, f));
    (void)n; (void)f;
}
void glCullFace(unsigned mode) {
    FWD_VOID(glCullFace, (unsigned), (mode));
    (void)mode;
}
void glFrontFace(unsigned mode) {
    FWD_VOID(glFrontFace, (unsigned), (mode));
    (void)mode;
}
void glStencilFunc(unsigned func, int ref, unsigned mask) {
    FWD_VOID(glStencilFunc, (unsigned, int, unsigned), (func, ref, mask));
    (void)func; (void)ref; (void)mask;
}
void glStencilMask(unsigned mask) {
    FWD_VOID(glStencilMask, (unsigned), (mask));
    (void)mask;
}
void glStencilOp(unsigned f, unsigned zf, unsigned zp) {
    FWD_VOID(glStencilOp, (unsigned, unsigned, unsigned), (f, zf, zp));
    (void)f; (void)zf; (void)zp;
}
void glColorMask(unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
    FWD_VOID(glColorMask, (unsigned char, unsigned char, unsigned char, unsigned char),
             (r, g, b, a));
    (void)r; (void)g; (void)b; (void)a;
}
void glLineWidth(float w) {
    FWD_VOID(glLineWidth, (float), (w));
    (void)w;
}
void glPolygonOffset(float f, float u) {
    FWD_VOID(glPolygonOffset, (float, float), (f, u));
    (void)f; (void)u;
}
void glClearColor(float r, float g, float b, float a) {
    FWD_VOID(glClearColor, (float, float, float, float), (r, g, b, a));
    (void)r; (void)g; (void)b; (void)a;
}
void glClearDepthf(float d) {
    FWD_VOID(glClearDepthf, (float), (d));
    (void)d;
}
void glClearStencil(int s) {
    FWD_VOID(glClearStencil, (int), (s));
    (void)s;
}
void glClear(unsigned mask) {
    FWD_VOID(glClear, (unsigned), (mask));
    (void)mask;
}

/* --- Draw --- */

void glDrawArrays(unsigned mode, int first, int count) {
    SHIM_GL_DBG_HOOK(glDrawArrays);
    FWD_VOID(glDrawArrays, (unsigned, int, int), (mode, first, count));
    (void)mode; (void)first;
    GLLOG(draw, "glDrawArrays(count=%d)", count);
}
void glDrawElements(unsigned mode, int count, unsigned type, const void *idx) {
    FWD_VOID(glDrawElements, (unsigned, int, unsigned, const void*),
             (mode, count, type, idx));
    (void)mode; (void)type; (void)idx;
    GLLOG(draw, "glDrawElements(count=%d)", count);
}
void glDrawArraysInstanced(unsigned mode, int first, int count, int inst) {
    FWD_VOID(glDrawArraysInstanced, (unsigned, int, int, int),
             (mode, first, count, inst));
    (void)mode; (void)first; (void)count; (void)inst;
}
void glDrawElementsInstanced(unsigned mode, int count, unsigned type, const void *idx, int inst) {
    FWD_VOID(glDrawElementsInstanced, (unsigned, int, unsigned, const void*, int),
             (mode, count, type, idx, inst));
    (void)mode; (void)count; (void)type; (void)idx; (void)inst;
}
void glVertexAttribDivisor(unsigned idx, unsigned divisor) {
    FWD_VOID(glVertexAttribDivisor, (unsigned, unsigned), (idx, divisor));
    (void)idx; (void)divisor;
}

/* --- Sync --- */

void glFinish(void) {
    FWD_VOID(glFinish, (void), ());
}
void glFlush(void) {
    FWD_VOID(glFlush, (void), ());
}
void glReadPixels(int x, int y, int w, int h, unsigned fmt, unsigned type, void *data) {
    FWD_VOID(glReadPixels, (int, int, int, int, unsigned, unsigned, void*),
             (x, y, w, h, fmt, type, data));
    (void)x; (void)y; (void)fmt; (void)type;
    if (data) memset(data, 0, (size_t)w * (size_t)h * 4);
}

/* --- end GL stubs --- */

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


typedef void (*pfn_t)(void);

void (*eglGetProcAddress(const char *name))(void) {
    /* Never forwarded to Mesa, even under SPIKE3_REAL_GL. Mesa's real
     * eglCreateImageKHR / eglCreateSyncKHR / glEGLImageTargetTexture2DOES
     * would be handed our fake GBM BOs and fake sync handles and SEGV on
     * the first deref (Step 56.6 diagnosis). The KHR stubs below accept
     * those handles silently. eglGetPlatformDisplay[EXT] returns our own
     * trampoline; forwarding happens *inside* its body via FWD_RET_V,
     * where the fake display the game stores matches our other interceptors.
     * Unknown names return NULL rather than falling through to
     * dlsym(RTLD_NEXT, name) — every unstubbed extension is one we have
     * not proven fake-handle-safe. Names match BN's reading of
     * glWindow::setGlContext (KHR image/sync + GL_OES_EGL_image). */
    /* Forward decls so we can put core-GL symbols in the table. Bodies are
     * exported top-level functions elsewhere in this file. */
    extern void glFramebufferTexture2D(unsigned, unsigned, unsigned, unsigned, int);
    extern void glFramebufferTextureLayer(unsigned, unsigned, unsigned, int, int);
    extern void glFramebufferRenderbuffer(unsigned, unsigned, unsigned, unsigned);
    struct { const char *name; pfn_t fn; } table[] = {
        { "eglGetPlatformDisplayEXT",         (pfn_t)eglGetPlatformDisplayEXT },
        { "eglGetPlatformDisplay",            (pfn_t)eglGetPlatformDisplay },
        { "eglCreateImageKHR",                (pfn_t)eglCreateImageKHR },
        { "eglDestroyImageKHR",               (pfn_t)eglDestroyImageKHR },
        { "eglCreateSyncKHR",                 (pfn_t)stub_eglCreateSyncKHR },
        { "eglDestroySyncKHR",                (pfn_t)stub_eglDestroySyncKHR },
        { "eglWaitSyncKHR",                   (pfn_t)stub_eglWaitSyncKHR },
        { "eglClientWaitSyncKHR",             (pfn_t)stub_eglClientWaitSyncKHR },
        { "glEGLImageTargetTexture2DOES",     (pfn_t)glEGLImageTargetTexture2DOES },
        /* FBO attach functions — Step 58.post.3 pivot. Route these here so
         * the EXTERNAL_OES→TEXTURE_2D swap inside our wrapper applies even
         * if the game resolves these via eglGetProcAddress instead of PLT. */
        { "glFramebufferTexture2D",           (pfn_t)glFramebufferTexture2D },
        { "glFramebufferTextureLayer",        (pfn_t)glFramebufferTextureLayer },
        { "glFramebufferRenderbuffer",        (pfn_t)glFramebufferRenderbuffer },
        { NULL, NULL },
    };
    for (int i = 0; table[i].name; i++) {
        if (strcmp(table[i].name, name) == 0) {
            SHIM_LOG("eglGetProcAddress(\"%s\") -> stub %p", name, (void *)table[i].fn);
            return table[i].fn;
        }
    }
    SHIM_LOG("eglGetProcAddress(\"%s\") -> NULL (no stub)", name);
    return NULL;
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

/* ---- Bridge frame escape encoding ----
 *
 * The Spike 3 bridge protocol reserves three byte values for framing:
 *   0x02 = STX (start of frame)
 *   0x03 = ETX (end of frame)
 *   0x1b = ESC (next data byte has bit 7 cleared)
 *
 * Any CMD/ID/PAYLOAD/CKSUM byte that equals one of these three values is
 * transmitted as `0x1b (byte | 0x80)`. The receiver's byte-by-byte parser
 * in sub_d15aa8 (see file 0x915c38 — the `cmp w0, #0x1b` and the
 * `bic w0, w0, w26` at 0x915c10) un-escapes by AND-ing with ~0x80.
 *
 * We discovered this via cmd 0x18 responses where id=0x02 (channel 2) was
 * sent by the game as `02 18 1b 82 cksum 03` (escape-encoded). Our naive
 * req[2] extraction read 0x1b as the channel and sent back a malformed
 * frame, which the receiver then rejected with "bridge rx checksum error".
 */

/* Decode an incoming escape-encoded frame into its logical bytes. Returns
 * decoded length including STX/ETX, or -1 on malformed input. */
static int bridge_decode_escape(const uint8_t *raw, size_t raw_len,
                                uint8_t *out, size_t out_cap) {
    if (raw_len < 5 || raw[0] != 0x02 || raw[raw_len - 1] != 0x03) return -1;
    if (out_cap < 2) return -1;
    out[0] = 0x02;
    size_t o = 1;
    for (size_t i = 1; i < raw_len - 1; i++) {
        if (o >= out_cap - 1) return -1;
        if (raw[i] == 0x1b && i + 1 < raw_len - 1) {
            out[o++] = raw[i + 1] & 0x7f;
            i++;
        } else {
            out[o++] = raw[i];
        }
    }
    out[o++] = 0x03;
    return (int)o;
}

/* Encode a logical frame into its on-wire (escape-encoded) form. Returns
 * encoded length. STX (byte 0) and ETX (last byte) are emitted literally;
 * any 0x02/0x03/0x1b byte between them is prefixed with 0x1b. */
static int bridge_encode_escape(const uint8_t *logical, size_t logical_len,
                                uint8_t *out, size_t out_cap) {
    if (logical_len < 2 || logical[0] != 0x02 ||
        logical[logical_len - 1] != 0x03 || out_cap < 1) return -1;
    out[0] = 0x02;
    size_t o = 1;
    for (size_t i = 1; i < logical_len - 1; i++) {
        uint8_t b = logical[i];
        if (b == 0x02 || b == 0x03 || b == 0x1b) {
            if (o + 2 >= out_cap) return -1;
            out[o++] = 0x1b;
            out[o++] = b | 0x80;
        } else {
            if (o + 1 >= out_cap) return -1;
            out[o++] = b;
        }
    }
    if (o + 1 > out_cap) return -1;
    out[o++] = 0x03;
    return (int)o;
}

static void bridge_send_response(const uint8_t *resp, size_t resp_len,
                                 uint8_t req_cmd, uint8_t id) {
    if (uart_shim_fd < 0) return;
    fprintf(stderr, "[shim] bridge RESP to cmd=0x%02x id=0x%02x (%zu logical bytes):",
            req_cmd, id, resp_len);
    for (size_t i = 0; i < resp_len; i++) fprintf(stderr, " %02x", resp[i]);
    fprintf(stderr, "\n");
    uint8_t wire[64];
    int wire_len = bridge_encode_escape(resp, resp_len, wire, sizeof(wire));
    if (wire_len < 0) {
        SHIM_LOG("bridge: encode_escape failed");
        return;
    }
    if ((size_t)wire_len != resp_len) {
        fprintf(stderr, "[shim]   (escape-encoded to %d bytes:", wire_len);
        for (int i = 0; i < wire_len; i++) fprintf(stderr, " %02x", wire[i]);
        fprintf(stderr, ")\n");
    }
    ssize_t n = real_write(uart_shim_fd, wire, wire_len);
    if (n < 0) SHIM_LOG("bridge: real_write response failed: %s", strerror(errno));
}

static void synthesize_bridge_response(const uint8_t *raw_req, size_t raw_len) {
    uint8_t req[64];
    int req_len = bridge_decode_escape(raw_req, raw_len, req, sizeof(req));
    if (req_len < 5) return;
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
    case 0x15: {
        /* Pwrdist status (follow-up after 0x10). Handler at file 0x9160a0
         * requires exactly 1 payload byte; OR/XORs it into state word at
         * data_490e5b0+128. First guess: 0x00 = "all clear, no faults". */
        resp[0] = 0x02;
        resp[1] = 0x15;
        resp[2] = id;
        resp[3] = 0x00;
        resp[4] = bridge_checksum(resp + 1, 3);
        resp[5] = 0x03;
        bridge_send_response(resp, 6, cmd, id);
        return;
    }
    case 0x18: {
        /* Pwrdist followup #2. Handler at file 0x91607c records min(existing,
         * payload_len) into a halfword table at data_490e5b0+0x88[id]; the
         * value isn't read. Send minimal 1-byte payload with value 0x00. */
        resp[0] = 0x02;
        resp[1] = 0x18;
        resp[2] = id;
        resp[3] = 0x00;
        resp[4] = bridge_checksum(resp + 1, 3);
        resp[5] = 0x03;
        bridge_send_response(resp, 6, cmd, id);
        return;
    }
    case 0x1c: {
        /* Pwrdist firmware flash chunk write. Jump table routes cmd 0x1c to
         * the generic "silent no-op + ACK" handler at file 0x915d28 (shared
         * with cmds 0x11/0x13/0x16/0x17/0x19/0x21/0x23–0x25/0x30/0x31). A
         * zero-payload frame takes the cbnz-w27 path and signals the waiting
         * channel, which is what send_bridge_message_with_response wants to
         * unblock. Game treats that as "chunk write acknowledged." */
        resp[0] = 0x02;
        resp[1] = 0x1c;
        resp[2] = id;
        resp[3] = bridge_checksum(resp + 1, 2);
        resp[4] = 0x03;
        bridge_send_response(resp, 5, cmd, id);
        return;
    }
    case 0x11: {
        /* Jump table routes cmd 0x11 to the generic silent-ACK fallback at
         * 0x915d28 (same family as 0x1c). Zero-payload frame unblocks the
         * channel; no state is written. */
        resp[0] = 0x02;
        resp[1] = 0x11;
        resp[2] = id;
        resp[3] = bridge_checksum(resp + 1, 2);
        resp[4] = 0x03;
        bridge_send_response(resp, 5, cmd, id);
        return;
    }
    case 0x16: {
        /* Cmd 0x16 routes to the shared handler path at 0x915d28. That path
         * has two branches:
         *   - payload_len == 0 AND id == 0: pure ACK, no buffer write.
         *   - payload_len > 0: falls to 0x915e8c which memcpys the payload
         *     into the channel's rx_buf registered at
         *     data_490e5b0+0xa0+id*8.
         * The caller at 0x675c20 (wrapper sub_917b60) sends cmd 0x16 with a
         * 1-byte tx payload (0x00) and expects a 4-byte response. It then
         * polls up to 1000×1ms waiting for:
         *   return value (rx[0]) > 0  AND  rx[2] == 0x02
         * Values for rx[1] and rx[3] are stored to caller out-ptrs but not
         * tested. Send `01 00 02 00` so the poll exits immediately. */
        resp[0] = 0x02;
        resp[1] = 0x16;
        resp[2] = id;
        resp[3] = 0x01;
        resp[4] = 0x00;
        resp[5] = 0x02;
        resp[6] = 0x00;
        resp[7] = bridge_checksum(resp + 1, 6);
        resp[8] = 0x03;
        bridge_send_response(resp, 9, cmd, id);
        return;
    }
    case 0x1d: {
        /* Dedicated handler at 0x916028: `cmp w27,#4 / b.eq 0x915e88`. If
         * payload_len == 4 it memcpys into the per-id buffer at
         * data_490e5b0+0xa0+id*8; otherwise it falls through to the silent-
         * ACK fallback at 0x915d28. Sending 0-byte payload takes the else
         * path — unblocks the waiting channel without writing stale data
         * into a buffer whose semantics we haven't yet verified (plausibly
         * pwrdist status bits + rail state + ADC reading, per FotE §4.16). */
        resp[0] = 0x02;
        resp[1] = 0x1d;
        resp[2] = id;
        resp[3] = bridge_checksum(resp + 1, 2);
        resp[4] = 0x03;
        bridge_send_response(resp, 5, cmd, id);
        return;
    }
    case 0x27: {
        /* Four game-side callsites polling a 1-byte response byte:
         *   0x916828 one-shot getter (stores resp[0] to caller out-ptr)
         *   0x916fe0 loop: spin until bit 2 == 1
         *   0x917020 loop: spin until bit 2 == 0  (paired with 916fe0)
         *   0x917264 loop: spin (100ms × ≤100) until (resp & 3) == 2
         * The paired loop at 916fe0/917020 requires bit 2 to toggle 1→0
         * within the same function call, so a static response stalls one
         * of the two halves. Alternate odd/even calls:
         *   odd  -> 0x06 (bits 1,2 set; resp&3 == 2)
         *   even -> 0x02 (bit 1 set only; resp&3 == 2)
         * Both values satisfy 917264; alternation resolves the pair. */
        static unsigned int cmd27_count = 0;
        cmd27_count++;
        uint8_t status = (cmd27_count & 1) ? 0x06 : 0x02;
        resp[0] = 0x02;
        resp[1] = 0x27;
        resp[2] = id;
        resp[3] = status;
        resp[4] = bridge_checksum(resp + 1, 3);
        resp[5] = 0x03;
        bridge_send_response(resp, 6, cmd, id);
        SHIM_LOG("bridge: cmd 0x27 #%u -> status 0x%02x (id=0x%02x)",
                 cmd27_count, status, id);
        return;
    }
    /* Step 45: cmd 0x25 / 0x41 silent-ACK reverted.
     *
     * ATTEMPTED: zero-payload ACK for both. Unblocked the send-side channel
     * wait, but progress REGRESSED — the crashing run reached Godot init +
     * Radium + pokemon_battle_scene + texture 400 before SEGV. With ACKs
     * enabled, the game never reached Godot at all in 90 s (stdout stopped
     * at the /data/ mkdir errors; no Godot Engine banner; texture/shader
     * counts unchanged from pre-init steady state).
     *
     * Interpretation: the previous timeout path WAS the progress path. Game
     * sends cmd 0x41, waits 1000 ms, times out, logs "Connection timed out",
     * falls back to default/null state, proceeds to Godot. Our fast zero-
     * payload ACK arrives before the timeout fires; game interprets "ACK
     * without the expected status payload" as "keep polling", and spins.
     *
     * cmd 0x41 dispatches to dedicated handler 0x915f88 (PROTOCOL.md) with
     * payload contract unknown. Resolving it properly needs a BN dig we
     * don't want to spend budget on today. For now: let the timeout fire,
     * memcpy NULL-guard catches the downstream PopupMenu SEGV.
     */
    default:
        SHIM_LOG("bridge: no synthetic response for cmd 0x%02x", cmd);
        return;
    }
}

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

