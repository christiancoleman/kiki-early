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
#define FAKE_MODE_HDISPLAY 1920
#define FAKE_MODE_VDISPLAY 1080

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

static int uart_is_active(void) { return uart_game_fd >= 0; }
static int is_drm_fd(int fd) { return drm_game_fd >= 0 && fd == drm_game_fd; }

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

/* ---- Capture pipeline (Step 23.3) ----
 *
 * Writes DMD frames, shader source, and texture bytes to _emulation/{dmd,
 * shaders,textures}/ as the game runs. Hooks that feed these live further
 * down in the file (gbm_bo_map/unmap, glShaderSource, glTex*Image2D,
 * glCompressedTexSubImage2D). The constructor creates the dirs up front.
 */

__attribute__((constructor))
static void shim_init_capture_dirs(void) {
    /* Paths are relative to the game's cwd, which is games/pokemon_le/ per
     * the documented run command (see Step 23.3). mkdir returns EEXIST when
     * rerunning, which we ignore. */
    mkdir("_emulation",           0755);
    mkdir("_emulation/dmd",       0755);
    mkdir("_emulation/shaders",   0755);
    mkdir("_emulation/textures",  0755);
    mkdir("_emulation/audio",     0755);
    /* Step 45 — Godot tries to cache compiled shaders here and logs an
     * ERROR when it can't create /tmp/cache. Host-visible path (/tmp is
     * excluded from remap_path), so this mkdir works directly. */
    mkdir("/tmp/cache",           0755);
}

/* ---- SIGSEGV/SIGBUS backtrace handler ----
 *
 * QEMU user-mode currently reports `uncaught target signal 11` for every
 * crash in the guest binary. Installing a guest-side sigaction lets us
 * catch the fault inside the guest process (the shim is loaded into the
 * guest's address space via LD_PRELOAD) and dump a backtrace before
 * chaining to the default handler for the core dump.
 *
 * Frame addresses are runtime addresses in the guest's address space.
 * `dladdr()` resolves each frame to module + offset, so we can translate
 * to file offsets by subtracting the module base and map into Binary
 * Ninja via `BN_VA = file_offset + 0x400000` (PROTOCOL.md).
 *
 * Async-signal safety: snprintf() isn't strictly async-signal-safe, but
 * in practice it doesn't take locks or allocate, and we only care about
 * getting output out before the inevitable re-crash.
 */

/* Read `size` bytes from guest VA `addr` into `dst`. Under QEMU user-mode
 * the shim runs as guest code, so a direct memcpy hits QEMU's address
 * translation and reaches guest memory (unlike /proc/self/mem, which is
 * host-view). If `addr` points to an unmapped guest page the memcpy
 * faults — if we're already inside a SIGSEGV handler, the second fault
 * will terminate the process. Caller should write diagnostic context
 * BEFORE calling this so at least the header makes it to stderr. */
static ssize_t shim_safe_read(void *dst, uintptr_t addr, size_t size) {
    memcpy(dst, (const void *)addr, size);
    return (ssize_t)size;
}

/* Hexdump `size` bytes starting at guest VA `addr` to stderr in
 * `offset: hh hh hh ... |ascii|` format. Uses shim_safe_read so
 * unmapped addresses report an error rather than crashing. */
static void shim_hexdump_stderr(const char *label, uintptr_t addr, size_t size) {
    char hdr[256];
    int hn = snprintf(hdr, sizeof(hdr),
        "\n--- DUMP %s @ 0x%lx (%zu bytes) ---\n", label, addr, size);
    ssize_t u = write(STDERR_FILENO, hdr, (size_t)hn);
    (void)u;

    unsigned char buf[4096];
    size_t off = 0;
    while (off < size) {
        size_t chunk = size - off;
        if (chunk > sizeof(buf)) chunk = sizeof(buf);
        ssize_t got = shim_safe_read(buf, addr + off, chunk);
        if (got <= 0) {
            char err[128];
            int en = snprintf(err, sizeof(err),
                "  (unreadable at 0x%lx: pread=%zd errno=%d)\n",
                addr + off, got, errno);
            u = write(STDERR_FILENO, err, (size_t)en);
            break;
        }
        for (ssize_t i = 0; i < got; i += 16) {
            char line[128];
            int ln = snprintf(line, sizeof(line), "%08lx: ", addr + off + (size_t)i);
            ssize_t j;
            for (j = 0; j < 16 && (i + j) < got; j++)
                ln += snprintf(line + ln, sizeof(line) - (size_t)ln,
                               "%02x ", buf[i + j]);
            while (ln < 60) line[ln++] = ' ';
            line[ln++] = '|';
            for (j = 0; j < 16 && (i + j) < got; j++) {
                unsigned char c = buf[i + j];
                line[ln++] = (c >= 32 && c < 127) ? (char)c : '.';
            }
            line[ln++] = '|';
            line[ln++] = '\n';
            u = write(STDERR_FILENO, line, (size_t)ln);
        }
        off += (size_t)got;
    }
}

/* One-shot diagnostic: dump /proc/self/maps grep'd for the game binary
 * and the two sound-system globals. Called from SIGSEGV handler. */
static void shim_dump_sound_globals(void) {
    /* Print the shim's own handler address so we know what VA range
     * our code landed at under QEMU user-mode. If this is in the
     * 0x7xxx... range, the shim is in high-memory mmap and we need
     * to rebase guest addresses accordingly. */
    char addr_line[256];
    int aln = snprintf(addr_line, sizeof(addr_line),
        "\n--- shim handler addresses (for computing QEMU guest base) ---\n"
        "&shim_dump_sound_globals = %p\n"
        "(shim .so was compiled with default aarch64 Linux ABI)\n",
        (void*)shim_dump_sound_globals);
    ssize_t u = write(STDERR_FILENO, addr_line, (size_t)aln);
    (void)u;

    /* Print the FULL /proc/self/maps so we can see file-backed
     * mappings for the game binary. */
    int mf = open("/proc/self/maps", O_RDONLY);
    if (mf >= 0) {
        char buf[32768];
        ssize_t total = 0;
        ssize_t n;
        while (total < (ssize_t)sizeof(buf) - 1 &&
               (n = read(mf, buf + total, sizeof(buf) - 1 - (size_t)total)) > 0) {
            total += n;
        }
        close(mf);
        if (total > 0) {
            u = write(STDERR_FILENO,
                "\n--- /proc/self/maps (full) ---\n", 33);
            (void)u;
            u = write(STDERR_FILENO, buf, (size_t)total);
            (void)u;
            u = write(STDERR_FILENO, "\n--- end /proc/self/maps ---\n", 30);
            (void)u;
        }
    }

    /* Find the game binary's load base at runtime by walking backtrace
     * frames for one inside ./game and subtracting its file offset.
     * QEMU user-mode + ASLR puts the game at a randomized base
     * (~0x7ab...) rather than at ELF VA 0x400000. Filter out .so
     * frames — the shim path contains the substring "games/" which
     * would falsely match a bare "/game" strstr. */
    uintptr_t game_base = 0;
    void *bt[64];
    int nb = backtrace(bt, 64);
    for (int i = 0; i < nb; i++) {
        Dl_info bi;
        if (dladdr(bt[i], &bi) && bi.dli_fname &&
            strstr(bi.dli_fname, "/game") &&
            !strstr(bi.dli_fname, ".so")) {
            game_base = (uintptr_t)bi.dli_fbase;
            break;
        }
    }

    char base_line[256];
    int bln;
    if (game_base == 0) {
        bln = snprintf(base_line, sizeof(base_line),
            "\n--- could NOT resolve game binary load base via dladdr; aborting dump ---\n");
        u = write(STDERR_FILENO, base_line, (size_t)bln);
        return;
    }
    bln = snprintf(base_line, sizeof(base_line),
        "\n--- resolved game load base = 0x%lx ---\n"
        "(hashmap = base + 0x44c56d0; registry = base + 0x44ca4d0)\n",
        game_base);
    u = write(STDERR_FILENO, base_line, (size_t)bln);

    /* Self-probe: read 32 bytes at our own function's address. If
     * THIS fails, memcpy can't read anything — something is wrong
     * with how the shim executes in its own memory (unlikely). */
    shim_hexdump_stderr("self-probe (&shim_dump_sound_globals)",
                        (uintptr_t)&shim_dump_sound_globals, 0x20);

    /* Frame-probe: read at the actual backtrace frame we found in
     * the game binary. We know this address IS valid because the
     * game is executing there right now. */
    if (nb > 0) {
        /* Find first frame in ./game and probe there directly. */
        for (int i = 0; i < nb; i++) {
            Dl_info bi;
            if (dladdr(bt[i], &bi) && bi.dli_fname && strstr(bi.dli_fname, "/game")) {
                shim_hexdump_stderr("frame-probe (live game PC from backtrace)",
                                    (uintptr_t)bt[i], 0x20);
                break;
            }
        }
    }

    /* Sanity probe: dump 64 bytes at the known rodata location of
     * SE_PK_FX_ATTACK_TYPE_BUG. If this doesn't print an ASCII run
     * starting with "SE_PK_FX_ATTACK_TYPE_BUG", game_base resolution
     * is wrong and all subsequent dumps are suspect. */
    shim_hexdump_stderr("rodata probe (SE_PK_FX_ATTACK_TYPE_BUG)",
                        game_base + 0x3607900, 0x40);

    /* Parse the g_asset_registry _Hashtable header and walk into
     * the first few nodes. BN VA 0x48ca4d0 → ELF VA 0x44ca4d0. */
    uintptr_t reg_addr = game_base + 0x44ca4d0;
    uint64_t buckets_ptr = 0, bucket_count = 0, first_node = 0;
    uint64_t element_count = 0, next_resize = 0;
    uint32_t max_lf_bits = 0;
    memcpy(&buckets_ptr,  (void*)(reg_addr + 0x00), 8);
    memcpy(&bucket_count, (void*)(reg_addr + 0x08), 8);
    memcpy(&first_node,   (void*)(reg_addr + 0x10), 8);
    memcpy(&element_count,(void*)(reg_addr + 0x18), 8);
    memcpy(&max_lf_bits,  (void*)(reg_addr + 0x20), 4);
    memcpy(&next_resize,  (void*)(reg_addr + 0x28), 8);
    float max_lf;
    memcpy(&max_lf, &max_lf_bits, 4);

    char summary[512];
    int sn = snprintf(summary, sizeof(summary),
        "\n=== g_asset_registry _Hashtable @ 0x%lx ===\n"
        "  _M_buckets       = 0x%lx\n"
        "  _M_bucket_count  = %lu\n"
        "  _M_first_node    = 0x%lx\n"
        "  _M_element_count = %lu\n"
        "  _M_max_load_factor = %.2f\n"
        "  _M_next_resize   = %lu\n",
        reg_addr, buckets_ptr, bucket_count, first_node,
        element_count, max_lf, next_resize);
    u = write(STDERR_FILENO, summary, (size_t)sn);

    /* Walk the full _M_nxt chain and dump each node's raw bytes to
     * _emulation/asset_registry.bin for the standalone parser. Each
     * node is assumed ≤128 bytes (libstdc++ _Hash_node<pair<K,V>,true>
     * with cached hash; larger than typical but conservative). */
    const size_t NODE_SIZE = 128;
    uint64_t node_count = 0;
    int fd = open("_emulation/asset_registry.bin",
                  O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        const char *msg = "\n[shim] failed to open _emulation/asset_registry.bin\n";
        u = write(STDERR_FILENO, msg, strlen(msg));
    } else {
        /* File layout:
         *   [0x00..0x40)   64B file header (magic, game_base, registry_va, counts)
         *   [0x40..0x80)   64B raw copy of the _Hashtable struct
         *   [0x80..end)    node_count × NODE_SIZE bytes, each node raw */
        struct {
            char     magic[8];         // "SPK3ASR\0"
            uint64_t game_base;
            uint64_t registry_va;
            uint64_t hashtable_bytes;  // 64
            uint64_t node_size;        // NODE_SIZE
            uint64_t node_count;       // filled after walk
            uint64_t bucket_count;
            uint64_t element_count;
        } hdr = {{0}};
        memcpy(hdr.magic, "SPK3ASR", 8);
        hdr.game_base = game_base;
        hdr.registry_va = reg_addr;
        hdr.hashtable_bytes = 64;
        hdr.node_size = NODE_SIZE;
        hdr.bucket_count = bucket_count;
        hdr.element_count = element_count;
        /* Placeholder — rewritten after walk. */
        u = (ssize_t)write(fd, &hdr, sizeof(hdr));
        /* Pad to 64 bytes. */
        char pad[64];
        memset(pad, 0, sizeof(pad));
        u = (ssize_t)write(fd, pad, 64 - sizeof(hdr));

        /* Dump the _Hashtable struct verbatim (64 bytes covers it). */
        u = (ssize_t)write(fd, (void*)reg_addr, 64);

        /* Walk the chain. */
        char node_buf[128];
        uintptr_t node = (uintptr_t)first_node;
        while (node != 0 && node_count < 3000) {
            memcpy(node_buf, (void*)node, NODE_SIZE);
            u = (ssize_t)write(fd, node_buf, NODE_SIZE);
            /* _M_nxt is at +0x00 of every node. */
            uint64_t nxt;
            memcpy(&nxt, &node_buf[0], 8);
            node = (uintptr_t)nxt;
            node_count++;
        }

        /* Rewrite header with final node_count. */
        hdr.node_count = node_count;
        lseek(fd, 0, SEEK_SET);
        u = (ssize_t)write(fd, &hdr, sizeof(hdr));
        close(fd);

        char msg[256];
        int ml = snprintf(msg, sizeof(msg),
            "\n[shim] asset_registry dumped: %lu nodes × %zu bytes → "
            "_emulation/asset_registry.bin (%lu bytes total, elem_count=%lu)\n",
            node_count, NODE_SIZE, 64 + 64 + node_count * NODE_SIZE, element_count);
        u = write(STDERR_FILENO, msg, (size_t)ml);
    }

    /* Keep a small in-band hexdump so the user can eyeball structure
     * without opening the binary file. 0x200 bytes = ~4 nodes. */
    if (first_node) {
        shim_hexdump_stderr("asset_registry first nodes (eyeball sample)",
                            first_node, 0x200);
    }
    (void)node_count;
}

/* ---- cmode dump (Step 50) ----
 *
 * _INIT_74 registers 42 crule::cmode_* objects at fixed BN VA addresses.
 * Struct layout (verified from sub_90d840 decomp):
 *   +0x00  vtable ptr
 *   +0x08  mode_id          (uint32)
 *   +0x10  arg5 behaviour   (uint32)
 *   +0x18  const 0x700
 *   +0x30  type_flags       (uint32)
 *   +0x58  scene_data ptr   (from get_scene_data_by_id(scene_id))
 *   +0xb0  timer_obj ptr
 *   +0x308 init_flag        (int — changes from 0 when mode activates)
 *
 * BN VA → runtime address = game_base + (bn_va - 0x400000).
 *
 * For 42 slots, dump (mode_id, type_flags, scene_ptr, init_flag) to give
 * us a live snapshot of the game's mode state. If static init ran
 * successfully these will all have the expected mode_id / type_flags
 * values matching MODE_TABLE.md. If any init_flag is non-zero, that mode
 * has transitioned from static-registration to "activated" state.
 */

struct shim_cmode_entry {
    uintptr_t bn_va;        /* BN-reported .bss address */
    uint32_t  expected_mode_id;
    uint32_t  expected_type_flags;
    const char *name_hint;
};

static const struct shim_cmode_entry shim_cmode_table[] = {
    /* Simple/base modes (type_flags=0x01) — 20 entries */
    { 0x4856ae0, 0x00, 0x01, "base_00" },
    { 0x4856be0, 0x01, 0x01, "base_01" },
    { 0x4856ce0, 0x02, 0x01, "base_02" },
    { 0x4856df0, 0x03, 0x01, "base_03" },
    { 0x4856f38, 0x04, 0x01, "base_04" },
    { 0x4856fb8, 0x05, 0x01, "base_05" },
    { 0x4857020, 0x06, 0x01, "base_06" },
    { 0x48570c0, 0x08, 0x01, "base_08" },
    { 0x48571a0, 0x07, 0x01, "base_07" },
    { 0x4857288, 0x0b, 0x01, "base_0b" },
    { 0x4857320, 0x0a, 0x01, "base_0a" },
    { 0x48573d0, 0x0c, 0x01, "base_0c" },
    { 0x4857448, 0x1f, 0x01, "base_1f" },
    { 0x48574b8, 0x0d, 0x01, "base_0d" },
    { 0x4857548, 0x0e, 0x01, "base_0e" },
    { 0x4857608, 0x10, 0x01, "base_10" },
    { 0x48576f8, 0x0f, 0x01, "base_0f" },
    { 0x4857760, 0x11, 0x01, "base_11" },
    { 0x48577d0, 0x1e, 0x01, "base_1e" },
    { 0x4857950, 0x12, 0x01, "base_12" },
    /* type_flags=0x08 utility modes */
    { 0x48579d0, 0x25, 0x08, "util_25" },
    { 0x4858360, 0x24, 0x08, "util_24" },
    { 0x4859288, 0x1c, 0x08, "util_1c" },
    /* Battle environment modes (type_flags=0x48) */
    { 0x4857aa0, 0x20, 0x48, "battle_mountain" },
    { 0x4857cd0, 0x21, 0x48, "battle_forest" },
    { 0x4857f00, 0x22, 0x48, "battle_desert" },
    { 0x4858130, 0x23, 0x48, "battle_coast" },
    /* Pokemon story modes (type_flags=0x28) */
    { 0x4858480, 0x13, 0x28, "story_bulbasaur_1" },
    { 0x48585e0, 0x14, 0x28, "story_bulbasaur_2" },
    { 0x48586b8, 0x15, 0x28, "story_charmander_1" },  /* CONFIRMED */
    { 0x48587f8, 0x16, 0x28, "story_charmander_2" },
    { 0x4858900, 0x19, 0x28, "story_squirtle_1" },
    { 0x4858a50, 0x1a, 0x28, "story_squirtle_2" },
    /* Pikachu modes (type_flags=0x24) */
    { 0x4858df0, 0x17, 0x24, "pikachu_1" },
    { 0x4858f90, 0x18, 0x24, "pikachu_2" },
    /* Multiball family (type_flags=0x04) */
    { 0x4858b28, 0x09, 0x04, "mb_09" },
    { 0x4858c80, 0x26, 0x04, "mb_26" },
    { 0x4858d38, 0x27, 0x04, "mb_27" },
    /* Multiball / wizard (type_flags=0x84) */
    { 0x48590a0, 0x28, 0x84, "rocket_mb_1" },
    { 0x4859160, 0x1b, 0x84, "rocket_mb_2" },
    { 0x4859368, 0x1d, 0x84, "rocket_mb_3" },
    { 0x4859438, 0x29, 0x84, "rocket_wizard" },
};

/* Resolve the game binary's load base. Caches on first success — needed
 * because some shim code paths (e.g. the Step 52 sweep timer thread) run
 * from threads that have no game frames on their stack, making backtrace
 * + dladdr return nothing. Any shim hook called from a game thread will
 * populate the cache via this function before the timer thread needs it. */
static uintptr_t shim_cached_game_base = 0;
static uintptr_t shim_resolve_game_base(void) {
    uintptr_t cached = __atomic_load_n(&shim_cached_game_base, __ATOMIC_ACQUIRE);
    if (cached) return cached;

    void *bt[64];
    int nb = backtrace(bt, 64);
    for (int i = 0; i < nb; i++) {
        Dl_info bi;
        if (dladdr(bt[i], &bi) && bi.dli_fname &&
            strstr(bi.dli_fname, "/game") &&
            !strstr(bi.dli_fname, ".so")) {
            uintptr_t base = (uintptr_t)bi.dli_fbase;
            __atomic_store_n(&shim_cached_game_base, base, __ATOMIC_RELEASE);
            return base;
        }
    }
    return 0;
}

static void shim_dump_cmodes(void) {
    uintptr_t game_base = shim_resolve_game_base();
    if (game_base == 0) {
        const char *m = "\n--- cmode dump: game_base not resolved, skipping ---\n";
        ssize_t u = write(STDERR_FILENO, m, strlen(m));
        (void)u;
        return;
    }

    const char *hdr = "\n--- cmode state dump (42 entries from _INIT_74) ---\n"
                      "slot  bn_va        rt_va            mode_id  type_flags  "
                      "scene_ptr        init_flag  name\n";
    ssize_t u = write(STDERR_FILENO, hdr, strlen(hdr));
    (void)u;

    size_t N = sizeof(shim_cmode_table) / sizeof(shim_cmode_table[0]);
    int ok_count = 0, mismatch_count = 0, activated_count = 0;

    for (size_t i = 0; i < N; i++) {
        const struct shim_cmode_entry *e = &shim_cmode_table[i];
        uintptr_t rt_va = game_base + (e->bn_va - 0x400000);

        uint32_t mode_id = 0, type_flags = 0, init_flag = 0;
        uintptr_t scene_ptr = 0;

        /* Direct memcpy — under QEMU user-mode the shim runs as guest code,
         * so accessing guest VAs works (same as asset_registry dump). */
        memcpy(&mode_id,     (void*)(rt_va + 0x08), 4);
        memcpy(&type_flags,  (void*)(rt_va + 0x0c), 4);   /* CORRECTED from +0x30 */
        memcpy(&scene_ptr,   (void*)(rt_va + 0x58), 8);
        memcpy(&init_flag,   (void*)(rt_va + 0x308), 4);

        int ok = (mode_id == e->expected_mode_id &&
                  type_flags == e->expected_type_flags);
        if (ok) ok_count++; else mismatch_count++;
        if (init_flag != 0) activated_count++;

        char line[256];
        int ln = snprintf(line, sizeof(line),
            "%4zu  0x%08lx   0x%012lx   0x%02x      0x%02x        "
            "0x%012lx   0x%08x  %s%s\n",
            i, e->bn_va, (unsigned long)rt_va,
            mode_id, type_flags,
            (unsigned long)scene_ptr, init_flag,
            e->name_hint,
            ok ? "" : "  [MISMATCH]");
        u = write(STDERR_FILENO, line, (size_t)ln);
        (void)u;
    }

    char tail[256];
    int tl = snprintf(tail, sizeof(tail),
        "\n--- cmode dump summary: %d/%zu mode_id+type_flags matched, "
        "%d activated (init_flag != 0), %d mismatches ---\n"
        "(type_flags mismatches are expected — we haven't pinned the offset yet; "
        "see hexdump below)\n",
        ok_count, N, activated_count, mismatch_count);
    u = write(STDERR_FILENO, tail, (size_t)tl);
    (void)u;

    /* Dereference scene_data_ptr for a few representative cmodes and
     * hexdump the target memory. If scene_data contains a SHA-1 (Radium
     * scene identifier) or a pointer to one, we'll see it as ASCII hex
     * text in the dump. Three cmodes chosen:
     *   - base_00 (mode 0x00, scene_id 0x29) — shared attract scene
     *   - story_charmander_1 (mode 0x15, scene_id 0x1a) — known Radium
     *     scene SHA-1 "7949bb14a9a21abf5e2885a01eb5ed4f9c3a8eca"
     *   - battle_mountain (mode 0x20, scene_id 0x14) — distinct */
    uintptr_t scene_ptrs[3];
    memcpy(&scene_ptrs[0], (void*)(game_base + (0x4856ae0 - 0x400000) + 0x58), 8);
    memcpy(&scene_ptrs[1], (void*)(game_base + (0x48586b8 - 0x400000) + 0x58), 8);
    memcpy(&scene_ptrs[2], (void*)(game_base + (0x4857aa0 - 0x400000) + 0x58), 8);

    if (scene_ptrs[0]) shim_hexdump_stderr(
        "scene_data for base_00 (scene_id 0x29, shared attract)",
        scene_ptrs[0], 0x80);
    if (scene_ptrs[1]) shim_hexdump_stderr(
        "scene_data for story_charmander_1 (scene_id 0x1a, SHA-1 7949bb14...)",
        scene_ptrs[1], 0x80);
    if (scene_ptrs[2]) shim_hexdump_stderr(
        "scene_data for battle_mountain (scene_id 0x14)",
        scene_ptrs[2], 0x80);
}

/* ---- Step 51: dispatch table dump ----
 *
 * From BN analysis (user-provided):
 *   data_4767b30 = cmode pointer registry (42 entries × 8 bytes = 336 bytes)
 *     Iterated by sub_950d60 (event handler) + sub_94f140 (update callback)
 *   data_485ea80 = event dispatch linked-list heads (207 slots × 8 bytes = 1656 bytes)
 *     206 event types, slot [event_id] -> head of callback linked list
 *   data_48c52c0 = update-type linked-list heads (5 slots × 8 bytes = 40 bytes)
 *   data_480eab0 = scene_data lookup table (40 entries × 0x1a0 = 16640 bytes)
 *
 * Dump: (a) the 42 cmode registry pointers and verify they match the .bss
 * addresses we already expect. (b) which event slots have handlers — a
 * histogram of "# events with ≥1 handler registered".
 */
static void shim_dump_dispatch_tables(void) {
    uintptr_t game_base = shim_resolve_game_base();
    if (!game_base) {
        const char *m = "\n--- dispatch tables: game_base unresolved, skipping ---\n";
        ssize_t u = write(STDERR_FILENO, m, strlen(m)); (void)u;
        return;
    }

    /* --- cmode registry at data_4767b30 --- */
    uintptr_t reg_rt = game_base + (0x4767b30 - 0x400000);
    uintptr_t reg_end_rt = game_base + (0x4767c80 - 0x400000);
    size_t reg_entries = (reg_end_rt - reg_rt) / 8;

    char hdr[256];
    int hn = snprintf(hdr, sizeof(hdr),
        "\n--- cmode registry @ data_4767b30 (runtime 0x%lx, %zu entries) ---\n"
        " idx  registry_ptr_value   expected_bss     match?\n",
        reg_rt, reg_entries);
    ssize_t u = write(STDERR_FILENO, hdr, (size_t)hn); (void)u;

    /* Expected cmode BSS addresses from _INIT_74, in the ORDER we think
     * they're registered into data_4767b30. This is a guess — the actual
     * order may differ. If the guess is wrong, the "match?" column flags
     * it and we learn what the real order is from the registry itself. */
    static const uintptr_t expected_order[] = {
        0x4856ae0, 0x4856be0, 0x4856ce0, 0x4856df0, 0x4856f38, 0x4856fb8,
        0x4857020, 0x48571a0, 0x48570c0, 0x4857288, 0x4857320, 0x48573d0,
        0x4857448, 0x48574b8, 0x4857548, 0x4857608, 0x48576f8, 0x4857760,
        0x48577d0, 0x4857950, 0x48579d0, 0x4857aa0, 0x4857cd0, 0x4857f00,
        0x4858130, 0x4858360, 0x4858480, 0x48585e0, 0x48586b8, 0x48587f8,
        0x4858900, 0x4858a50, 0x4858b28, 0x4858c80, 0x4858d38, 0x4858df0,
        0x4858f90, 0x48590a0, 0x4859160, 0x4859288, 0x4859368, 0x4859438,
    };
    size_t N_expected = sizeof(expected_order) / sizeof(expected_order[0]);

    int matched = 0;
    for (size_t i = 0; i < reg_entries && i < 64; i++) {
        uintptr_t entry = 0;
        memcpy(&entry, (void*)(reg_rt + i * 8), 8);
        /* Unmap runtime→BN VA: subtract game_base, add 0x400000 */
        uintptr_t entry_bn = entry - game_base + 0x400000;
        uintptr_t expected_bn = (i < N_expected) ? expected_order[i] : 0;
        int mismatch = (expected_bn == 0 || entry_bn != expected_bn);
        if (!mismatch) matched++;

        char line[200];
        int ln = snprintf(line, sizeof(line),
            " %3zu  0x%014lx  0x%08lx  %s\n",
            i, (unsigned long)entry, (unsigned long)expected_bn,
            mismatch ? "[differs]" : "ok");
        u = write(STDERR_FILENO, line, (size_t)ln); (void)u;
    }
    char sum[128];
    int sl = snprintf(sum, sizeof(sum),
        "  cmode registry: %d/%zu matched expected .bss ordering\n",
        matched, reg_entries);
    u = write(STDERR_FILENO, sum, (size_t)sl); (void)u;

    /* --- event handler histogram at data_485ea80 (207 slots) --- */
    uintptr_t evt_base = game_base + (0x485ea80 - 0x400000);
    int populated = 0;
    int total_handlers = 0;
    char evt_detail[16384] = "\n--- event dispatch @ data_485ea80 (207 slots; non-empty shown) ---\n";
    size_t evt_off = strlen(evt_detail);
    for (int ev = 0; ev <= 0xce; ev++) {
        uintptr_t head = 0;
        memcpy(&head, (void*)(evt_base + ev * 8), 8);
        if (!head) continue;
        populated++;
        /* Count chain length by following a 'next' pointer at offset +0x10
         * (guessed from sub_9c11d0's linked-list insertion pattern). */
        int chain = 0;
        uintptr_t cur = head;
        for (int step = 0; step < 32 && cur; step++) {
            chain++;
            total_handlers++;
            uintptr_t next = 0;
            memcpy(&next, (void*)(cur + 0x10), 8);
            cur = next;
        }
        size_t space_left = sizeof(evt_detail) - evt_off;
        if (space_left > 128) {
            int added = snprintf(evt_detail + evt_off, space_left,
                "  event 0x%02x: head=0x%lx  chain_len=%d\n",
                ev, (unsigned long)head, chain);
            evt_off += (size_t)added;
        }
    }
    u = write(STDERR_FILENO, evt_detail, evt_off); (void)u;
    char esum[128];
    int esl = snprintf(esum, sizeof(esum),
        "  event registry summary: %d/207 slots populated, %d total handlers\n",
        populated, total_handlers);
    u = write(STDERR_FILENO, esum, (size_t)esl); (void)u;

    /* --- update-type dispatch at data_48c52c0 (5 slots) --- */
    uintptr_t upd_base = game_base + (0x48c52c0 - 0x400000);
    char upd_hdr[128];
    int uhl = snprintf(upd_hdr, sizeof(upd_hdr),
        "\n--- update-type dispatch @ data_48c52c0 (5 slots) ---\n");
    u = write(STDERR_FILENO, upd_hdr, (size_t)uhl); (void)u;
    for (int ut = 0; ut < 5; ut++) {
        uintptr_t head = 0;
        memcpy(&head, (void*)(upd_base + ut * 8), 8);
        char line[128];
        int lnn = snprintf(line, sizeof(line),
            "  update_type %d: head=0x%lx\n", ut, (unsigned long)head);
        u = write(STDERR_FILENO, line, (size_t)lnn); (void)u;
    }
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

static sigjmp_buf shim_event_sweep_jmp;
static volatile sig_atomic_t shim_event_fire_in_progress = 0;
static volatile int shim_current_sweep_event_id = -1;

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
    size_t N = sizeof(shim_cmode_table) / sizeof(shim_cmode_table[0]);
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
    size_t N = sizeof(shim_cmode_table) / sizeof(shim_cmode_table[0]);
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

static void shim_event_sweep(void) {
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

    size_t N = sizeof(shim_cmode_table) / sizeof(shim_cmode_table[0]);
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

static void shim_sigsegv_handler(int sig, siginfo_t *info, void *ucontext) {
    (void)ucontext;

    /* Event sweep recovery path: if a SEGV fires while we're inside
     * shim_fire_event(), log the failure and longjmp back to the sweep
     * loop so we can continue with the next event instead of aborting. */
    if (shim_event_fire_in_progress) {
        char msg[192];
        int mn = snprintf(msg, sizeof(msg),
            "\n  [sweep] event 0x%02x CRASHED sig=%d fault=%p — recovering\n",
            shim_current_sweep_event_id, sig, info->si_addr);
        ssize_t u2 = write(STDERR_FILENO, msg, (size_t)mn); (void)u2;
        shim_event_fire_in_progress = 0;
        siglongjmp(shim_event_sweep_jmp, 1);
    }

    char hdr[256];
    int hn = snprintf(hdr, sizeof(hdr),
        "\n=== SHIM SIGSEGV: sig=%d si_code=%d fault_addr=%p ===\n",
        sig, info->si_code, info->si_addr);
    ssize_t u = write(STDERR_FILENO, hdr, (size_t)hn);
    (void)u;

    void *frames[64];
    int n = backtrace(frames, 64);
    for (int i = 0; i < n; i++) {
        Dl_info di;
        char line[512];
        int ln;
        if (dladdr(frames[i], &di) && di.dli_fname) {
            unsigned long off =
                (unsigned long)((uintptr_t)frames[i] - (uintptr_t)di.dli_fbase);
            ln = snprintf(line, sizeof(line),
                "[%02d] %p  %s+0x%lx  %s\n",
                i, frames[i], di.dli_fname, off,
                di.dli_sname ? di.dli_sname : "");
        } else {
            ln = snprintf(line, sizeof(line),
                "[%02d] %p  (no dladdr)\n", i, frames[i]);
        }
        u = write(STDERR_FILENO, line, (size_t)ln);
    }

    /* Dump sound-system globals for offline inspection (Step 42). */
    shim_dump_sound_globals();

    /* Dump the 42 cmode objects from _INIT_74 (Step 50). */
    shim_dump_cmodes();

    /* Dump dispatch tables: cmode registry, event handlers, update types (Step 51). */
    shim_dump_dispatch_tables();

    /* Chain to default handler so QEMU's "uncaught target signal 11"
     * + core dump still happen (unchanged external behaviour). */
    signal(sig, SIG_DFL);
    raise(sig);
}

/* SIGTERM / SIGUSR1 / SIGUSR2 handler:
 *   SIGTERM/SIGINT — dump state and exit cleanly.
 *   SIGUSR1        — dump state and keep running.
 *   SIGUSR2        — run event sweep (fire events 0x00..0xce) and keep running.
 *
 * Lets us extract memory state when `timeout` kills QEMU (SIGTERM)
 * or when the user triggers a manual dump via `kill -USR1 <pid>` /
 * a sweep via `kill -USR2 <pid>`. */
static void shim_term_dump_handler(int sig) {
    if (sig == SIGUSR2) {
        const char *m = "\n=== SHIM SIGUSR2: RUNNING EVENT SWEEP ===\n";
        ssize_t u = write(STDERR_FILENO, m, strlen(m)); (void)u;
        shim_event_sweep();
        return;
    }

    char hdr[128];
    int hn = snprintf(hdr, sizeof(hdr),
        "\n=== SHIM SIGTERM/USR1 DUMP (sig=%d) ===\n", sig);
    ssize_t u = write(STDERR_FILENO, hdr, (size_t)hn);
    (void)u;
    shim_dump_sound_globals();
    shim_dump_cmodes();
    shim_dump_dispatch_tables();
    /* For SIGTERM, exit cleanly; for SIGUSR1, keep running. */
    if (sig == SIGTERM || sig == SIGINT) {
        _exit(0);
    }
}
































#include <sys/un.h>
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

/* Write a JSON event to the subscriber if connected. Graceful on disconnect. */
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
        struct sockaddr_un addr;
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

        /* Send a hello so the subscriber knows we're live. */
        char hello[256];
        int hn = snprintf(hello, sizeof(hello),
            "{\"t\":\"hello\",\"shim\":\"pokemon_le\",\"cmodes\":42,\"ts_ms\":%lu}\n",
            (unsigned long)(shim_now_ms() - shim_bridge_start_ms));
        (void)send(fd, hello, (size_t)hn, MSG_NOSIGNAL);
    }
    return NULL;
}

/* Polling thread — samples 42 cmodes' +0x308 at 10 Hz, emits on deltas. */
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

    size_t N = sizeof(shim_cmode_table) / sizeof(shim_cmode_table[0]);
    uint64_t last[42];
    /* Prime baseline */
    for (size_t i = 0; i < N; i++) {
        uintptr_t rt = game_base + (shim_cmode_table[i].bn_va - 0x400000);
        memcpy(&last[i], (void*)(rt + 0x308), 8);
    }

    SHIM_LOG("[bridge] poll thread live (game_base=0x%lx, polling %zu cmodes @ 10 Hz)",
             game_base, N);

    for (;;) {
        /* 100 ms between polls */
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 100 * 1000 * 1000 };
        nanosleep(&ts, NULL);

        for (size_t i = 0; i < N; i++) {
            uintptr_t rt = game_base + (shim_cmode_table[i].bn_va - 0x400000);
            uint64_t now_val;
            memcpy(&now_val, (void*)(rt + 0x308), 8);
            if (now_val != last[i]) {
                char line[320];
                int ln = snprintf(line, sizeof(line),
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
    size_t N = sizeof(shim_cmode_table) / sizeof(shim_cmode_table[0]);
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

/* Start the bridge: create listener socket, spawn accept + poll threads.
 * Called from the constructor; no-op unless SPIKE3_BRIDGE=1. */
static void shim_bridge_start(void) {
    const char *enabled = getenv("SPIKE3_BRIDGE");
    if (!enabled || enabled[0] != '1') return;

    const char *sock_path = getenv("SPIKE3_BRIDGE_SOCKET");
    if (!sock_path || !sock_path[0]) sock_path = "/tmp/spike3_bridge.sock";

    /* Unlink any stale socket. */
    unlink(sock_path);

    shim_bridge_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (shim_bridge_listen_fd < 0) {
        SHIM_LOG("[bridge] socket() failed: %s", strerror(errno));
        return;
    }

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    if (bind(shim_bridge_listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        SHIM_LOG("[bridge] bind(%s) failed: %s", sock_path, strerror(errno));
        close(shim_bridge_listen_fd);
        shim_bridge_listen_fd = -1;
        return;
    }

    if (listen(shim_bridge_listen_fd, 1) < 0) {
        SHIM_LOG("[bridge] listen() failed: %s", strerror(errno));
        close(shim_bridge_listen_fd);
        shim_bridge_listen_fd = -1;
        return;
    }

    shim_bridge_start_ms = shim_now_ms();

    pthread_t t_accept, t_poll;
    if (pthread_create(&t_accept, NULL, shim_bridge_accept_thread, NULL) == 0) {
        pthread_detach(t_accept);
    }
    if (pthread_create(&t_poll, NULL, shim_bridge_poll_thread, NULL) == 0) {
        pthread_detach(t_poll);
    }

    SHIM_LOG("[bridge] listening on %s (AF_UNIX)", sock_path);
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
    (void)device;
    return gbm_drm_fd;
}

const char *gbm_device_get_backend_name(void *device) {
    (void)device;
    return "stub";
}

int gbm_device_is_format_supported(void *device, uint32_t format, uint32_t usage) {
    (void)device; (void)format; (void)usage;
    return 1;
}

void *gbm_surface_create(void *gbm, uint32_t width, uint32_t height,
                         uint32_t format, uint32_t flags) {
    if (!fake_gbm_surface_mem) fake_gbm_surface_mem = alloc_fake_gbm_object();
    SHIM_LOG("gbm_surface_create(%ux%u, fmt=0x%x, flags=0x%x) -> %p",
             width, height, format, flags, fake_gbm_surface_mem);
    (void)gbm;
    return fake_gbm_surface_mem;
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

/* Each BO needs its own backing so the game can distinguish them. Track
 * the most-recent one in a small LRU so gbm_surface_lock_front_buffer
 * can return a valid BO. */
static void *gbm_bo_latest = NULL;

/* ---- Per-BO side table ----
 *
 * Needed so gbm_bo_map can hand back the BO's real width/stride/backing
 * and so gbm_bo_unmap can flush the DMD backing to disk as a BGRA frame.
 * The game's `DMDEmulationScene` uses a 1024×256 ABGR8888 BO (see BN
 * analysis in _emulation/ASSETS.md); that pair of dimensions + format
 * uniquely identifies the DMD among the few BOs the game allocates. */

#define GBM_BO_TABLE_SIZE   32
#define GBM_FORMAT_ABGR8888 0x34324241u   /* 'AB24' — DMD format */

struct gbm_bo_entry {
    void    *bo;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t flags;
    uint32_t stride;
    void    *mapped;        /* allocated in gbm_bo_map, freed in gbm_bo_unmap */
    int      is_dmd;
};

static struct gbm_bo_entry gbm_bo_table[GBM_BO_TABLE_SIZE];
static pthread_mutex_t gbm_bo_table_lock = PTHREAD_MUTEX_INITIALIZER;

static struct gbm_bo_entry *gbm_bo_register(void *bo, uint32_t w, uint32_t h,
                                            uint32_t fmt, uint32_t flags) {
    pthread_mutex_lock(&gbm_bo_table_lock);
    struct gbm_bo_entry *out = NULL;
    for (int i = 0; i < GBM_BO_TABLE_SIZE; i++) {
        if (!gbm_bo_table[i].bo) {
            gbm_bo_table[i].bo      = bo;
            gbm_bo_table[i].width   = w;
            gbm_bo_table[i].height  = h;
            gbm_bo_table[i].format  = fmt;
            gbm_bo_table[i].flags   = flags;
            gbm_bo_table[i].stride  = w * 4;   /* 32-bpp assumed */
            gbm_bo_table[i].mapped  = NULL;
            gbm_bo_table[i].is_dmd  = (w == 1024 && h == 256
                                        && fmt == GBM_FORMAT_ABGR8888);
            out = &gbm_bo_table[i];
            break;
        }
    }
    pthread_mutex_unlock(&gbm_bo_table_lock);
    return out;
}

static struct gbm_bo_entry *gbm_bo_lookup(void *bo) {
    pthread_mutex_lock(&gbm_bo_table_lock);
    struct gbm_bo_entry *out = NULL;
    for (int i = 0; i < GBM_BO_TABLE_SIZE; i++) {
        if (gbm_bo_table[i].bo == bo) { out = &gbm_bo_table[i]; break; }
    }
    pthread_mutex_unlock(&gbm_bo_table_lock);
    return out;
}

static void gbm_bo_unregister(void *bo) {
    pthread_mutex_lock(&gbm_bo_table_lock);
    for (int i = 0; i < GBM_BO_TABLE_SIZE; i++) {
        if (gbm_bo_table[i].bo == bo) {
            if (gbm_bo_table[i].mapped) free(gbm_bo_table[i].mapped);
            memset(&gbm_bo_table[i], 0, sizeof(gbm_bo_table[i]));
            break;
        }
    }
    pthread_mutex_unlock(&gbm_bo_table_lock);
}

void *gbm_bo_create(void *gbm, uint32_t width, uint32_t height,
                    uint32_t format, uint32_t flags) {
    void *bo = alloc_fake_gbm_object();
    gbm_bo_latest = bo;
    struct gbm_bo_entry *e = gbm_bo_register(bo, width, height, format, flags);
    SHIM_LOG("gbm_bo_create(%ux%u, fmt=0x%x, flags=0x%x) -> %p%s",
             width, height, format, flags, bo,
             (e && e->is_dmd) ? " [DMD]" : "");
    (void)gbm;
    return bo;
}

void *gbm_bo_create_with_modifiers(void *gbm, uint32_t width, uint32_t height,
                                   uint32_t format, const uint64_t *modifiers,
                                   unsigned int count) {
    void *bo = alloc_fake_gbm_object();
    gbm_bo_latest = bo;
    struct gbm_bo_entry *e = gbm_bo_register(bo, width, height, format, 0);
    SHIM_LOG("gbm_bo_create_with_modifiers(%ux%u) -> %p%s",
             width, height, bo, (e && e->is_dmd) ? " [DMD]" : "");
    (void)gbm; (void)modifiers; (void)count;
    return bo;
}

void gbm_bo_destroy(void *bo) { gbm_bo_unregister(bo); free(bo); }

/* Per-BO getters: consult the side table so each BO reports its real
 * dimensions/format (the DMD is 1024×256 ABGR8888, not the screen size).
 * Unknown BOs fall back to the screen defaults. */
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
uint64_t gbm_bo_get_modifier(void *bo) { (void)bo; return 0; /* LINEAR */ }
int gbm_bo_get_fd(void *bo) { (void)bo; return -1; }
uint32_t gbm_bo_get_handle_u(void *bo) { (void)bo; return 1; }

/* ---- gbm_bo_map / gbm_bo_unmap ----
 *
 * The game's DMD upload path (create_gbm_bo_and_upload, BN VA 0xcffe80)
 * calls gbm_bo_map to get a writable pointer + stride, memcpys pixel rows
 * into it, then calls gbm_bo_unmap. We allocate `stride*height` zero-
 * initialized backing on first map and hand back (backing, stride). On
 * unmap of a DMD-sized BO we write the backing to _emulation/dmd/ and
 * free it.
 *
 * Libgbm signature:
 *   void *gbm_bo_map(bo, x, y, w, h, flags, *stride_out, **map_data_out)
 *   void  gbm_bo_unmap(bo, map_data)
 */

static uint32_t dmd_frame_counter = 0;

void *gbm_bo_map(void *bo, uint32_t x, uint32_t y,
                 uint32_t w, uint32_t h, uint32_t flags,
                 uint32_t *stride_out, void **map_data_out) {
    (void)x; (void)y;
    struct gbm_bo_entry *e = gbm_bo_lookup(bo);
    if (!e) {
        /* Unknown BO (created through a path we didn't track). Still hand
         * back scratch of the asked-for region so the caller doesn't SEGV. */
        size_t sz = (size_t)w * (size_t)h * 4;
        void *scratch = calloc(1, sz ? sz : 16);
        if (stride_out) *stride_out = w * 4;
        if (map_data_out) *map_data_out = scratch;
        SHIM_LOG("gbm_bo_map(%p, unknown BO, %ux%u) -> %p (scratch)",
                 bo, w, h, scratch);
        return scratch;
    }
    pthread_mutex_lock(&gbm_bo_table_lock);
    if (!e->mapped) {
        size_t sz = (size_t)e->stride * (size_t)e->height;
        e->mapped = calloc(1, sz);
    }
    void *backing = e->mapped;
    uint32_t stride = e->stride;
    int is_dmd = e->is_dmd;
    pthread_mutex_unlock(&gbm_bo_table_lock);
    if (stride_out)   *stride_out = stride;
    if (map_data_out) *map_data_out = backing;
    static int map_log_budget = 6;
    if (map_log_budget > 0) {
        map_log_budget--;
        SHIM_LOG("gbm_bo_map(%p, %ux%u, flags=0x%x) -> %p stride=%u%s",
                 bo, w, h, flags, backing, stride, is_dmd ? " [DMD]" : "");
    }
    return backing;
}

void gbm_bo_unmap(void *bo, void *map_data) {
    struct gbm_bo_entry *e = gbm_bo_lookup(bo);
    if (!e) {
        /* Scratch path: just free and return. */
        free(map_data);
        return;
    }
    /* Dump DMD frames before freeing. */
    if (e->is_dmd && e->mapped) {
        uint32_t n = __atomic_fetch_add(&dmd_frame_counter, 1, __ATOMIC_SEQ_CST);
        char path[128];
        snprintf(path, sizeof(path), "_emulation/dmd/frame_%06u.bgra", n);
        FILE *f = fopen(path, "wb");
        if (f) {
            size_t sz = (size_t)e->stride * (size_t)e->height;
            fwrite(e->mapped, 1, sz, f);
            fclose(f);
            if (n < 8 || (n % 60) == 0) {
                SHIM_LOG("DMD frame %u -> %s (%zu bytes)", n, path, sz);
            }
        } else {
            SHIM_LOG("DMD frame %u: fopen %s failed: %s",
                     n, path, strerror(errno));
        }
    }
    pthread_mutex_lock(&gbm_bo_table_lock);
    if (e->mapped) {
        free(e->mapped);
        e->mapped = NULL;
    }
    pthread_mutex_unlock(&gbm_bo_table_lock);
    (void)map_data;
}

/* gbm_bo_get_handle returns a union; declaring it as returning uint32_t
 * by value works because union gbm_bo_handle is layout-compatible with a
 * single 64-bit integer. */
uint64_t gbm_bo_get_handle(void *bo) { (void)bo; return 1; }

void *gbm_surface_lock_front_buffer(void *surface) {
    (void)surface;
    /* Return the most recent BO; if none yet, allocate one. */
    if (!gbm_bo_latest) gbm_bo_latest = alloc_fake_gbm_object();
    return gbm_bo_latest;
}
void gbm_surface_release_buffer(void *surface, void *bo) {
    (void)surface; (void)bo;
}
int gbm_surface_has_free_buffers(void *surface) { (void)surface; return 1; }

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

void *eglGetDisplay(void *native_display) {
    SHIM_LOG("eglGetDisplay(%p) -> fake display", native_display);
    return FAKE_EGL_DISPLAY;
}

void *eglGetPlatformDisplay(unsigned platform, void *native_display, const intptr_t *attrib_list) {
    SHIM_LOG("eglGetPlatformDisplay(platform=0x%x, dpy=%p) -> fake", platform, native_display);
    (void)attrib_list; return FAKE_EGL_DISPLAY;
}

void *eglGetPlatformDisplayEXT(unsigned platform, void *native_display, const int *attrib_list) {
    SHIM_LOG("eglGetPlatformDisplayEXT(platform=0x%x, dpy=%p) -> fake", platform, native_display);
    (void)attrib_list; return FAKE_EGL_DISPLAY;
}

unsigned eglInitialize(void *display, int *major, int *minor) {
    if (major) *major = 1;
    if (minor) *minor = 5;
    SHIM_LOG("eglInitialize(%p) -> 1.5", display);
    return EGL_TRUE;
}

unsigned eglTerminate(void *display) { (void)display; return EGL_TRUE; }

const char *eglQueryString(void *display, int name) {
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
    SHIM_LOG("eglBindAPI(0x%x)", api);
    return EGL_TRUE;
}

unsigned eglGetConfigs(void *display, void **configs, int config_size, int *num_config) {
    (void)display;
    if (num_config) *num_config = 1;
    if (configs && config_size >= 1) configs[0] = FAKE_EGL_CONFIG;
    return EGL_TRUE;
}

unsigned eglChooseConfig(void *display, const int *attrib_list,
                         void **configs, int config_size, int *num_config) {
    (void)display; (void)attrib_list;
    if (num_config) *num_config = 1;
    if (configs && config_size >= 1) configs[0] = FAKE_EGL_CONFIG;
    return EGL_TRUE;
}

unsigned eglGetConfigAttrib(void *display, void *config, int attribute, int *value) {
    (void)display; (void)config;
    if (!value) return EGL_FALSE;
    /* EGL attribute constants from <EGL/egl.h>; my earlier guesses had
     * RED_SIZE and NATIVE_VISUAL_ID on the wrong numbers, which made the
     * game log "failed to find egl config for gbm format" because the
     * returned visual id didn't match XR24. */
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
    SHIM_LOG("eglCreateContext -> fake context");
    (void)display; (void)config; (void)share_context; (void)attrib_list;
    return FAKE_EGL_CONTEXT;
}

unsigned eglDestroyContext(void *display, void *context) {
    (void)display; (void)context; return EGL_TRUE;
}

void *eglCreateWindowSurface(void *display, void *config, void *native_window,
                             const int *attrib_list) {
    SHIM_LOG("eglCreateWindowSurface(native=%p) -> fake surface", native_window);
    (void)display; (void)config; (void)attrib_list;
    return FAKE_EGL_SURFACE;
}

void *eglCreatePbufferSurface(void *display, void *config, const int *attrib_list) {
    (void)display; (void)config; (void)attrib_list;
    return FAKE_EGL_SURFACE;
}

unsigned eglDestroySurface(void *display, void *surface) {
    (void)display; (void)surface; return EGL_TRUE;
}

unsigned eglMakeCurrent(void *display, void *draw, void *read, void *context) {
    SHIM_LOG("eglMakeCurrent(dpy=%p, draw=%p, read=%p, ctx=%p)",
             display, draw, read, context);
    return EGL_TRUE;
}

unsigned eglSwapBuffers(void *display, void *surface) {
    (void)display; (void)surface; return EGL_TRUE;
}

unsigned eglSwapInterval(void *display, int interval) {
    (void)display; (void)interval; return EGL_TRUE;
}

unsigned eglSurfaceAttrib(void *display, void *surface, int attribute, int value) {
    (void)display; (void)surface; (void)attribute; (void)value; return EGL_TRUE;
}

unsigned eglQuerySurface(void *display, void *surface, int attribute, int *value) {
    (void)display; (void)surface;
    if (!value) return EGL_FALSE;
    switch (attribute) {
    case 0x3057: *value = FAKE_MODE_HDISPLAY; break; /* EGL_WIDTH */
    case 0x3056: *value = FAKE_MODE_VDISPLAY; break; /* EGL_HEIGHT */
    default:     *value = 0; break;
    }
    return EGL_TRUE;
}

int eglGetError(void) { return EGL_SUCCESS; }

/* Stubs for extensions resolved via eglGetProcAddress. The game queries
 * these after eglCreateContext (per BN analysis of sub_d01d10). Each
 * returns a fake sentinel so the game's subsequent calls unblock. */

static void *stub_eglCreateImageKHR(void *display, void *ctx, unsigned target,
                                    void *buffer, const intptr_t *attrib_list) {
    (void)display; (void)ctx; (void)target; (void)buffer; (void)attrib_list;
    return FAKE_EGL_IMAGE;
}
static unsigned stub_eglDestroyImageKHR(void *display, void *image) {
    (void)display; (void)image; return EGL_TRUE;
}
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
static void stub_glEGLImageTargetTexture2DOES(unsigned target, void *image) {
    (void)target; (void)image;
}

/* ---- Hooked: GLES 3.x ----
 * Pure no-op stubs to satisfy the game's rendering calls without a real
 * context. Returns are designed to be plausible enough for the game to
 * believe rendering succeeded:
 *   - glGen*  returns an incrementing handle per object
 *   - glGetShaderiv/glGetProgramiv return GL_TRUE for COMPILE_STATUS /
 *     LINK_STATUS so the Radium shader pipeline doesn't bail on "failed
 *     to compile" branches
 *   - glCheckFramebufferStatus returns GL_FRAMEBUFFER_COMPLETE
 *   - glGetError returns GL_NO_ERROR
 *   - glGetString advertises GLES 3.1 + the OES/EXT extensions the game's
 *     Texture2dDirect checks for
 *
 * When we pivot to real host GL (Step 21.8 option B), each stub body
 * gets replaced with a forwarded call; signatures stay the same.
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
static int gl_log_budget_uniform       = 20;
static int gl_log_budget_state         = 40;
#define GLLOG(kind, fmt, ...) do { \
    if (gl_log_budget_##kind > 0) { \
        gl_log_budget_##kind--; \
        SHIM_LOG("GL " fmt, ##__VA_ARGS__); \
    } \
} while (0)

/* --- Query / error --- */

unsigned glGetError(void) { return GL_NO_ERROR; }

const unsigned char *glGetString(unsigned name) {
    switch (name) {
    case GL_VERSION:                  return (const unsigned char *)"OpenGL ES 3.1 (stub)";
    case GL_VENDOR:                   return (const unsigned char *)"shim";
    case GL_RENDERER:                 return (const unsigned char *)"shim GLES3";
    case GL_SHADING_LANGUAGE_VERSION: return (const unsigned char *)"OpenGL ES GLSL ES 3.10";
    case GL_EXTENSIONS:
        /* Advertise the OES/EXT extensions Texture2dDirect / Radium check. */
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
    (void)name; (void)index; return (const unsigned char *)"";
}

/* Track which pnames glGetIntegerv has been asked about, so we can log each
 * unknown pname exactly once without swamping the log. 1 KB bitmap indexed
 * by the low bits of the pname (sufficient since GL enum low bits rarely
 * collide within a session). */
static unsigned char gl_getint_seen[4096];
static void gl_getint_log_unknown(unsigned pname) {
    unsigned slot = pname & 0xFFF;
    if (gl_getint_seen[slot]) return;
    gl_getint_seen[slot] = 1;
    SHIM_LOG("GL glGetIntegerv(pname=0x%x) -> 0 (unknown, defaulting)", pname);
}

void glGetIntegerv(unsigned pname, int *params) {
    if (!params) return;
    switch (pname) {
    /* Sizes / dims */
    case GL_MAX_TEXTURE_SIZE:                    *params = 4096; break;
    case GL_MAX_VIEWPORT_DIMS:                   params[0] = 4096; params[1] = 4096; break;
    case 0x851C: /* GL_MAX_CUBE_MAP_TEXTURE_SIZE */ *params = 2048; break;
    case 0x84E8: /* GL_MAX_RENDERBUFFER_SIZE */  *params = 4096; break;
    case 0x88FF: /* GL_MAX_3D_TEXTURE_SIZE */    *params = 2048; break;
    case 0x88FE: /* GL_MAX_ARRAY_TEXTURE_LAYERS */ *params = 256; break;

    /* Texture units */
    case GL_MAX_TEXTURE_IMAGE_UNITS:             *params = 16; break;
    case 0x8B4C: /* GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS */ *params = 16; break;
    case 0x8B4D: /* GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS */ *params = 32; break;

    /* Vertex pipeline */
    case GL_MAX_VERTEX_ATTRIBS:                  *params = 16; break;
    case 0x8DFB: /* GL_MAX_VERTEX_UNIFORM_VECTORS */   *params = 256; break;
    case 0x8A2B: /* GL_MAX_VERTEX_UNIFORM_COMPONENTS */ *params = 1024; break;
    case 0x8B4A: /* GL_MAX_VERTEX_UNIFORM_BLOCKS */    *params = 12; break;
    case 0x8B4B: /* GL_MAX_VARYING_COMPONENTS */ *params = 60; break;
    case 0x8DFC: /* GL_MAX_VARYING_VECTORS */    *params = 15; break;
    case 0x9122: /* GL_MAX_VERTEX_OUTPUT_COMPONENTS — ES 3.x per-stage */ *params = 64; break;

    /* Fragment pipeline */
    case 0x8DFD: /* GL_MAX_FRAGMENT_UNIFORM_VECTORS */ *params = 224; break;
    case 0x8B49: /* GL_MAX_FRAGMENT_UNIFORM_COMPONENTS */ *params = 896; break;
    case 0x8A2D: /* GL_MAX_FRAGMENT_UNIFORM_BLOCKS */ *params = 12; break;
    case 0x9125: /* GL_MAX_FRAGMENT_INPUT_COMPONENTS — ES 3.x per-stage */ *params = 60; break;

    /* Framebuffer */
    case 0x8CDF: /* GL_MAX_COLOR_ATTACHMENTS */  *params = 4; break;
    case 0x8824: /* GL_MAX_DRAW_BUFFERS */       *params = 4; break;
    case 0x8D57: /* GL_MAX_SAMPLES */            *params = 4; break;

    /* Uniform buffers */
    case 0x8A2E: /* GL_MAX_COMBINED_UNIFORM_BLOCKS */ *params = 24; break;
    case 0x8A2F: /* GL_MAX_UNIFORM_BUFFER_BINDINGS */ *params = 24; break;
    case 0x8A30: /* GL_MAX_UNIFORM_BLOCK_SIZE */ *params = 16384; break;
    case 0x8A34: /* GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT */ *params = 256; break;

    /* Program binary formats (count only; we claim zero since none supported) */
    case 0x87FE: /* GL_NUM_PROGRAM_BINARY_FORMATS */ *params = 0; break;

    default:
        *params = 0;
        gl_getint_log_unknown(pname);
        break;
    }
}
void glGetFloatv(unsigned pname, float *params) {
    if (params) *params = 0.0f;
    (void)pname;
}
void glGetBooleanv(unsigned pname, unsigned char *params) {
    if (params) *params = GL_FALSE;
    (void)pname;
}

/* --- Object creation --- */

void glGenTextures(int n, unsigned *out) {
    for (int i = 0; i < n; i++) out[i] = gl_next_tex_id++;
    GLLOG(object_create, "glGenTextures(%d) -> [%u..]", n, n ? out[0] : 0);
}
void glDeleteTextures(int n, const unsigned *ids) { (void)n; (void)ids; }

void glGenBuffers(int n, unsigned *out) {
    for (int i = 0; i < n; i++) out[i] = gl_next_buf_id++;
    GLLOG(object_create, "glGenBuffers(%d) -> [%u..]", n, n ? out[0] : 0);
}
void glDeleteBuffers(int n, const unsigned *ids) { (void)n; (void)ids; }

void glGenFramebuffers(int n, unsigned *out) {
    for (int i = 0; i < n; i++) out[i] = gl_next_fb_id++;
    GLLOG(object_create, "glGenFramebuffers(%d) -> [%u..]", n, n ? out[0] : 0);
}
void glDeleteFramebuffers(int n, const unsigned *ids) { (void)n; (void)ids; }

void glGenRenderbuffers(int n, unsigned *out) {
    for (int i = 0; i < n; i++) out[i] = gl_next_rb_id++;
    GLLOG(object_create, "glGenRenderbuffers(%d) -> [%u..]", n, n ? out[0] : 0);
}
void glDeleteRenderbuffers(int n, const unsigned *ids) { (void)n; (void)ids; }

void glGenVertexArrays(int n, unsigned *out) {
    for (int i = 0; i < n; i++) out[i] = gl_next_vao_id++;
    GLLOG(object_create, "glGenVertexArrays(%d) -> [%u..]", n, n ? out[0] : 0);
}
void glDeleteVertexArrays(int n, const unsigned *ids) { (void)n; (void)ids; }

/* Per-shader-id type tracking so glShaderSource captures can name the
 * right file suffix (vertex=0x8B31, fragment=0x8B30). Indexed by shader id. */
#define GL_SHADER_TYPE_MAX 1024
static uint32_t gl_shader_type[GL_SHADER_TYPE_MAX];

unsigned glCreateShader(unsigned type) {
    unsigned id = gl_next_shader_id++;
    if (id < GL_SHADER_TYPE_MAX) gl_shader_type[id] = type;
    GLLOG(object_create, "glCreateShader(type=0x%x) -> %u", type, id);
    return id;
}
void glDeleteShader(unsigned shader) { (void)shader; }

unsigned glCreateProgram(void) {
    unsigned id = gl_next_program_id++;
    GLLOG(object_create, "glCreateProgram() -> %u", id);
    return id;
}
void glDeleteProgram(unsigned program) { (void)program; }

/* --- Binding --- */

void glBindTexture(unsigned target, unsigned tex)       { (void)target; (void)tex; }
void glBindBuffer(unsigned target, unsigned buf)        { (void)target; (void)buf; }
void glBindFramebuffer(unsigned target, unsigned fb)    { (void)target; (void)fb; }
void glBindRenderbuffer(unsigned target, unsigned rb)   { (void)target; (void)rb; }
void glBindVertexArray(unsigned array)                   { (void)array; }
void glActiveTexture(unsigned tex)                       { (void)tex; }

/* --- Texture data / params ---
 *
 * Capture policy (Step 23.3): when `data` is non-NULL, dump the payload to
 * _emulation/textures/. StreamingImage_Bind (BN VA 0xca7210) drives most
 * of these calls with pixel data sourced from decoded image.bin entries;
 * capturing them gives us the game's runtime texture stream. */

static uint32_t gl_tex_capture_count = 0;

static int gl_bytes_per_pixel(unsigned format, unsigned type) {
    /* Packed types carry their own total width; return it directly. */
    switch (type) {
    case 0x8363: return 2;  /* UNSIGNED_SHORT_5_6_5 */
    case 0x8033: return 2;  /* UNSIGNED_SHORT_4_4_4_4 */
    case 0x8034: return 2;  /* UNSIGNED_SHORT_5_5_5_1 */
    case 0x8368: return 4;  /* UNSIGNED_INT_2_10_10_10_REV */
    case 0x8DAD: return 4;  /* UNSIGNED_INT_24_8 (depth-stencil) */
    default: break;
    }
    int comp;
    switch (format) {
    case 0x1906: comp = 1; break;   /* ALPHA */
    case 0x1909: comp = 1; break;   /* LUMINANCE */
    case 0x190A: comp = 2; break;   /* LUMINANCE_ALPHA */
    case 0x1903: comp = 1; break;   /* RED */
    case 0x8227: comp = 2; break;   /* RG */
    case 0x1907: comp = 3; break;   /* RGB */
    case 0x1908: comp = 4; break;   /* RGBA */
    case 0x80E0: comp = 3; break;   /* BGR */
    case 0x80E1: comp = 4; break;   /* BGRA (EXT_texture_format_BGRA8888) */
    case 0x1902: comp = 1; break;   /* DEPTH_COMPONENT */
    default:     comp = 4; break;
    }
    int byte_per_comp;
    switch (type) {
    case 0x1400: case 0x1401: byte_per_comp = 1; break; /* [U]BYTE */
    case 0x1402: case 0x1403: byte_per_comp = 2; break; /* [U]SHORT */
    case 0x1404: case 0x1405: byte_per_comp = 4; break; /* [U]INT */
    case 0x1406: byte_per_comp = 4; break;              /* FLOAT */
    case 0x140B: byte_per_comp = 2; break;              /* HALF_FLOAT */
    default:     byte_per_comp = 1; break;
    }
    return comp * byte_per_comp;
}

static void gl_capture_tex(const char *call, int w, int h,
                           unsigned format, unsigned type, const void *data) {
    if (!data || w <= 0 || h <= 0) return;
    int bpp = gl_bytes_per_pixel(format, type);
    size_t sz = (size_t)w * (size_t)h * (size_t)bpp;
    uint32_t seq = __atomic_fetch_add(&gl_tex_capture_count, 1, __ATOMIC_SEQ_CST);
    char path[160];
    snprintf(path, sizeof(path),
             "_emulation/textures/tex_%06u_%dx%d_fmt%04x_type%04x.raw",
             seq, w, h, format, type);
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(data, 1, sz, f);
        fclose(f);
        if (seq < 12 || (seq % 100) == 0) {
            SHIM_LOG("%s capture %u: %dx%d fmt=0x%x type=0x%x -> %s (%zu B)",
                     call, seq, w, h, format, type, path, sz);
        }
    }
}

void glTexImage2D(unsigned target, int level, int internalfmt,
                  int w, int h, int border, unsigned format, unsigned type,
                  const void *data) {
    GLLOG(object_create, "glTexImage2D %dx%d internal=0x%x fmt=0x%x type=0x%x data=%s",
          w, h, internalfmt, format, type, data ? "<ptr>" : "NULL");
    gl_capture_tex("glTexImage2D", w, h, format, type, data);
    (void)target; (void)level; (void)border;
}
void glTexSubImage2D(unsigned target, int level, int xoff, int yoff,
                     int w, int h, unsigned format, unsigned type,
                     const void *data) {
    gl_capture_tex("glTexSubImage2D", w, h, format, type, data);
    (void)target; (void)level; (void)xoff; (void)yoff;
}

static uint32_t gl_ctex_capture_count = 0;

static void gl_capture_ctex(const char *call, int w, int h,
                            unsigned format, int size, const void *data) {
    if (!data || size <= 0) return;
    uint32_t seq = __atomic_fetch_add(&gl_ctex_capture_count, 1, __ATOMIC_SEQ_CST);
    char path[160];
    snprintf(path, sizeof(path),
             "_emulation/textures/tex_compressed_%06u_%dx%d_fmt%04x.bin",
             seq, w, h, format);
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(data, 1, (size_t)size, f);
        fclose(f);
        if (seq < 12 || (seq % 100) == 0) {
            SHIM_LOG("%s capture %u: %dx%d fmt=0x%x size=%d -> %s",
                     call, seq, w, h, format, size, path);
        }
    }
}

void glCompressedTexImage2D(unsigned target, int level, unsigned internalfmt,
                            int w, int h, int border, int size, const void *data) {
    gl_capture_ctex("glCompressedTexImage2D", w, h, internalfmt, size, data);
    (void)target; (void)level; (void)border;
}
void glCompressedTexSubImage2D(unsigned target, int level, int xoff, int yoff,
                               int w, int h, unsigned format, int size,
                               const void *data) {
    gl_capture_ctex("glCompressedTexSubImage2D", w, h, format, size, data);
    (void)target; (void)level; (void)xoff; (void)yoff;
}
void glTexParameteri(unsigned target, unsigned pname, int param) {
    (void)target; (void)pname; (void)param;
}
void glTexParameterf(unsigned target, unsigned pname, float param) {
    (void)target; (void)pname; (void)param;
}
void glPixelStorei(unsigned pname, int param) { (void)pname; (void)param; }
void glGenerateMipmap(unsigned target) { (void)target; }

/* --- Buffer data --- */

void glBufferData(unsigned target, intptr_t size, const void *data, unsigned usage) {
    (void)target; (void)size; (void)data; (void)usage;
}
void glBufferSubData(unsigned target, intptr_t offset, intptr_t size, const void *data) {
    (void)target; (void)offset; (void)size; (void)data;
}
void *glMapBufferRange(unsigned target, intptr_t offset, intptr_t len, unsigned access) {
    (void)target; (void)offset; (void)len; (void)access; return NULL;
}
unsigned char glUnmapBuffer(unsigned target) { (void)target; return GL_TRUE; }

/* --- Shader --- */

static uint32_t gl_shader_capture_count = 0;

void glShaderSource(unsigned shader, int count, const char *const *string, const int *length) {
    if (count <= 0 || !string) return;
    size_t total = 0;
    for (int i = 0; i < count; i++) {
        size_t n = (length && length[i] >= 0) ? (size_t)length[i]
                                               : (string[i] ? strlen(string[i]) : 0);
        total += n;
    }
    char *buf = (char *)malloc(total + 1);
    if (!buf) return;
    size_t off = 0;
    for (int i = 0; i < count; i++) {
        size_t n = (length && length[i] >= 0) ? (size_t)length[i]
                                               : (string[i] ? strlen(string[i]) : 0);
        if (n && string[i]) memcpy(buf + off, string[i], n);
        off += n;
    }
    buf[total] = 0;
    uint32_t seq = __atomic_fetch_add(&gl_shader_capture_count, 1, __ATOMIC_SEQ_CST);
    uint32_t type = (shader < GL_SHADER_TYPE_MAX) ? gl_shader_type[shader] : 0;
    char path[160];
    snprintf(path, sizeof(path),
             "_emulation/shaders/shader_%03u_id%u_type%04x.glsl",
             seq, shader, type);
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(buf, 1, total, f);
        fclose(f);
        SHIM_LOG("shader capture %u: id=%u type=0x%x -> %s (%zu B)",
                 seq, shader, type, path, total);
    }
    free(buf);
}
void glCompileShader(unsigned shader) { (void)shader; }
void glAttachShader(unsigned program, unsigned shader) { (void)program; (void)shader; }
void glDetachShader(unsigned program, unsigned shader) { (void)program; (void)shader; }
void glLinkProgram(unsigned program) { (void)program; }
void glValidateProgram(unsigned program) { (void)program; }
void glUseProgram(unsigned program) { (void)program; }

void glGetShaderiv(unsigned shader, unsigned pname, int *params) {
    (void)shader;
    if (!params) return;
    /* Claim success for every status query — keeps Radium from bailing. */
    switch (pname) {
    case GL_COMPILE_STATUS:  *params = GL_TRUE; break;
    case GL_INFO_LOG_LENGTH: *params = 0; break;
    default:                 *params = 0; break;
    }
}
void glGetProgramiv(unsigned program, unsigned pname, int *params) {
    (void)program;
    if (!params) return;
    switch (pname) {
    case GL_LINK_STATUS:      *params = GL_TRUE; break;
    case GL_ACTIVE_UNIFORMS:  *params = 0; break;
    case GL_ACTIVE_ATTRIBUTES:*params = 0; break;
    case GL_INFO_LOG_LENGTH:  *params = 0; break;
    default:                  *params = 0; break;
    }
}
void glGetShaderInfoLog(unsigned shader, int max, int *len, char *log) {
    (void)shader; (void)max;
    if (len) *len = 0;
    if (log && max > 0) log[0] = 0;
}
void glGetProgramInfoLog(unsigned program, int max, int *len, char *log) {
    (void)program; (void)max;
    if (len) *len = 0;
    if (log && max > 0) log[0] = 0;
}

int glGetUniformLocation(unsigned program, const char *name) {
    (void)program; (void)name; return 0;
}
int glGetAttribLocation(unsigned program, const char *name) {
    (void)program; (void)name; return 0;
}
void glBindAttribLocation(unsigned program, unsigned index, const char *name) {
    (void)program; (void)index; (void)name;
}

/* Uniforms — many variants, all no-op */
#define GL_UNIFORM_1(n, t) void glUniform1##n(int loc, t v0) { \
    (void)loc; (void)v0; GLLOG(uniform, "glUniform1" #n); }
#define GL_UNIFORM_2(n, t) void glUniform2##n(int loc, t v0, t v1) { \
    (void)loc; (void)v0; (void)v1; GLLOG(uniform, "glUniform2" #n); }
#define GL_UNIFORM_3(n, t) void glUniform3##n(int loc, t v0, t v1, t v2) { \
    (void)loc; (void)v0; (void)v1; (void)v2; GLLOG(uniform, "glUniform3" #n); }
#define GL_UNIFORM_4(n, t) void glUniform4##n(int loc, t v0, t v1, t v2, t v3) { \
    (void)loc; (void)v0; (void)v1; (void)v2; (void)v3; GLLOG(uniform, "glUniform4" #n); }
GL_UNIFORM_1(i, int) GL_UNIFORM_2(i, int) GL_UNIFORM_3(i, int) GL_UNIFORM_4(i, int)
GL_UNIFORM_1(f, float) GL_UNIFORM_2(f, float) GL_UNIFORM_3(f, float) GL_UNIFORM_4(f, float)
void glUniform1iv(int loc, int count, const int *v)   { (void)loc; (void)count; (void)v; }
void glUniform1fv(int loc, int count, const float *v) { (void)loc; (void)count; (void)v; }
void glUniform2fv(int loc, int count, const float *v) { (void)loc; (void)count; (void)v; }
void glUniform3fv(int loc, int count, const float *v) { (void)loc; (void)count; (void)v; }
void glUniform4fv(int loc, int count, const float *v) { (void)loc; (void)count; (void)v; }
void glUniformMatrix3fv(int loc, int count, unsigned char trans, const float *v) {
    (void)loc; (void)count; (void)trans; (void)v;
}
void glUniformMatrix4fv(int loc, int count, unsigned char trans, const float *v) {
    (void)loc; (void)count; (void)trans; (void)v;
}

/* Vertex attribs */
void glVertexAttribPointer(unsigned idx, int size, unsigned type, unsigned char norm,
                           int stride, const void *ptr) {
    (void)idx; (void)size; (void)type; (void)norm; (void)stride; (void)ptr;
}
void glEnableVertexAttribArray(unsigned idx)  { (void)idx; }
void glDisableVertexAttribArray(unsigned idx) { (void)idx; }

/* --- Framebuffer --- */

void glFramebufferTexture2D(unsigned target, unsigned att, unsigned ttarget,
                            unsigned tex, int level) {
    (void)target; (void)att; (void)ttarget; (void)tex; (void)level;
}
void glFramebufferRenderbuffer(unsigned target, unsigned att, unsigned rbtarget, unsigned rb) {
    (void)target; (void)att; (void)rbtarget; (void)rb;
}
void glRenderbufferStorage(unsigned target, unsigned internalfmt, int w, int h) {
    (void)target; (void)internalfmt; (void)w; (void)h;
}
unsigned glCheckFramebufferStatus(unsigned target) {
    (void)target; return GL_FRAMEBUFFER_COMPLETE;
}

/* --- State --- */

void glEnable(unsigned cap)  { GLLOG(state, "glEnable(0x%x)",  cap); }
void glDisable(unsigned cap) { GLLOG(state, "glDisable(0x%x)", cap); }
unsigned char glIsEnabled(unsigned cap) { (void)cap; return GL_FALSE; }
void glBlendFunc(unsigned src, unsigned dst) { (void)src; (void)dst; }
void glBlendFuncSeparate(unsigned srcRGB, unsigned dstRGB, unsigned srcA, unsigned dstA) {
    (void)srcRGB; (void)dstRGB; (void)srcA; (void)dstA;
}
void glBlendEquation(unsigned mode) { (void)mode; }
void glBlendEquationSeparate(unsigned rgb, unsigned a) { (void)rgb; (void)a; }
void glBlendColor(float r, float g, float b, float a) {
    (void)r; (void)g; (void)b; (void)a;
}
void glViewport(int x, int y, int w, int h) {
    (void)x; (void)y; (void)w; (void)h;
}
void glScissor(int x, int y, int w, int h) {
    (void)x; (void)y; (void)w; (void)h;
}
void glDepthFunc(unsigned func) { (void)func; }
void glDepthMask(unsigned char mask) { (void)mask; }
void glDepthRangef(float n, float f) { (void)n; (void)f; }
void glCullFace(unsigned mode) { (void)mode; }
void glFrontFace(unsigned mode) { (void)mode; }
void glStencilFunc(unsigned func, int ref, unsigned mask) {
    (void)func; (void)ref; (void)mask;
}
void glStencilMask(unsigned mask) { (void)mask; }
void glStencilOp(unsigned f, unsigned zf, unsigned zp) { (void)f; (void)zf; (void)zp; }
void glColorMask(unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
    (void)r; (void)g; (void)b; (void)a;
}
void glLineWidth(float w) { (void)w; }
void glPolygonOffset(float f, float u) { (void)f; (void)u; }
void glClearColor(float r, float g, float b, float a) {
    (void)r; (void)g; (void)b; (void)a;
}
void glClearDepthf(float d) { (void)d; }
void glClearStencil(int s)  { (void)s; }
void glClear(unsigned mask) { (void)mask; }

/* --- Draw --- */

void glDrawArrays(unsigned mode, int first, int count) {
    (void)mode; (void)first;
    GLLOG(draw, "glDrawArrays(count=%d)", count);
}
void glDrawElements(unsigned mode, int count, unsigned type, const void *idx) {
    (void)mode; (void)type; (void)idx;
    GLLOG(draw, "glDrawElements(count=%d)", count);
}
void glDrawArraysInstanced(unsigned mode, int first, int count, int inst) {
    (void)mode; (void)first; (void)count; (void)inst;
}
void glDrawElementsInstanced(unsigned mode, int count, unsigned type, const void *idx, int inst) {
    (void)mode; (void)count; (void)type; (void)idx; (void)inst;
}
void glVertexAttribDivisor(unsigned idx, unsigned divisor) { (void)idx; (void)divisor; }

/* --- Sync --- */

void glFinish(void) {}
void glFlush(void) {}
void glReadPixels(int x, int y, int w, int h, unsigned fmt, unsigned type, void *data) {
    (void)x; (void)y; (void)w; (void)h; (void)fmt; (void)type;
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


void (*eglGetProcAddress(const char *name))(void) {
    /* Funnel every game-queried extension through our stubs. These match
     * the names BN showed the game resolving inside glWindow::setGlContext
     * (KHR image/sync + GL_OES_EGL_image) plus eglGetPlatformDisplayEXT
     * itself (the game queries it before calling it, and falls back if
     * it's NULL). */
    typedef void (*pfn_t)(void);
    struct { const char *name; pfn_t fn; } table[] = {
        { "eglGetPlatformDisplayEXT",         (pfn_t)eglGetPlatformDisplayEXT },
        { "eglGetPlatformDisplay",            (pfn_t)eglGetPlatformDisplay },
        { "eglCreateImageKHR",                (pfn_t)stub_eglCreateImageKHR },
        { "eglDestroyImageKHR",               (pfn_t)stub_eglDestroyImageKHR },
        { "eglCreateSyncKHR",                 (pfn_t)stub_eglCreateSyncKHR },
        { "eglDestroySyncKHR",                (pfn_t)stub_eglDestroySyncKHR },
        { "eglWaitSyncKHR",                   (pfn_t)stub_eglWaitSyncKHR },
        { "eglClientWaitSyncKHR",             (pfn_t)stub_eglClientWaitSyncKHR },
        { "glEGLImageTargetTexture2DOES",     (pfn_t)stub_glEGLImageTargetTexture2DOES },
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
     * populated very early — before the sweep timer thread needs it. */
    if (!__atomic_load_n(&shim_cached_game_base, __ATOMIC_ACQUIRE)) {
        (void)shim_resolve_game_base();
    }

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
    /* Passthrough — do NOT fallback to SW. The game hardcodes HW decode and
     * asserts(hw_decoder) at SpiVideoStreamDecoderBase.cpp:1830 if we
     * return NULL. Let the real HW decoder through; we'll intervene later
     * in the pipeline. */
    const void *ret = real_avcodec_find_decoder_by_name(name);
    SHIM_LOG("  -> %p (name='%s')", ret, name ? name : "(null)");
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

