/*
 * shim_patches.c — runtime patches and libavcodec entry-point hooks.
 *
 * Active content:
 *   - memcpy NULL-guard (Step 45): silently no-ops a memcpy() with src or
 *     dst NULL. Fixes the Godot error-logger path on the Stern/Radium
 *     DisplayRenderer thread, where Godot's TLS singletons are unregistered
 *     and vformat()/memcpy(NULL,…) would otherwise SEGV. Always-on.
 *   - Decoder compatibility patches (renamed from SPIKE3_SW_HEVC):
 *     decoder-name remap (hevc_v4l2request → hevc) + get_format callback
 *     b.ne NOP. Default-on; gate via SPIKE3_DECODER_PATCH=0 for debugging.
 *     These don't do software decoding — they enable the game to accept
 *     ANY non-V4L2-stateless decoder, which is required for forward-decode.
 *   - libavcodec entry-point hooks: avcodec_find_decoder_by_name,
 *     avcodec_get_hw_config, av_hwdevice_ctx_create, avcodec_send_packet,
 *     avcodec_receive_frame. send_packet / receive_frame dispatch to
 *     shim_avforward.c when SPIKE3_FORWARD_DECODE=1 (default-on); on
 *     bridge-unavailable they HARD-FAIL with -EIO. There is no SW fallback.
 *
 * Cross-TU deps (via spi_shim.h): SHIM_LOG macro.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/types.h>

#include "spi_shim.h"

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

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnonnull-compare"
    if (!dst || !src) {
#pragma GCC diagnostic pop
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

/* ========== Decoder compatibility patches (Step 64 origin, restored 2026-05-06) ========== */
/* Two pieces that together let the game's video pipeline accept any
 * non-V4L2-stateless HEVC decoder — whether plain software hevc as a
 * diagnostic, or NVDEC frames returned through the shim_avforward.c host
 * bridge in normal operation:
 *
 *   (1) avcodec_find_decoder_by_name remap: "hevc_v4l2request" → "hevc",
 *       so the game's codec init succeeds even when no V4L2 m2m HEVC slice
 *       device exists in our env. Handled inline in the hook below.
 *
 *   (2) get_format-callback NOP: NOPs the b.ne in the game's get_format
 *       callback so it accepts the first pix_fmt libavcodec offers (NV12
 *       in our flow), instead of insisting on DRM_PRIME (0xb3) which only
 *       the real V4L2 stateless stack on production CM4 hardware can deliver.
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
 * Patching 920cf0 to NOP makes the callback fall through to `ret` with w0
 * holding whatever format was just loaded — the first non-sentinel entry.
 *
 * Default-on. The previous SPIKE3_SW_HEVC env name was misleading — these
 * patches don't decode anything, they just enable a non-DRM_PRIME decoder
 * to function. SPIKE3_DECODER_PATCH=0 disables for debugging only —
 * without these, the game cannot allocate a working AVCodecContext in our
 * env, and forward-decode is therefore also broken.
 */
#define GET_FORMAT_BNE_VA   0x920cf0
#define AARCH64_NOP         0xd503201fu

static int shim_decoder_patch_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *s = getenv("SPIKE3_DECODER_PATCH");
        /* Default ENABLED. SPIKE3_DECODER_PATCH=0 to disable for debugging. */
        cached = !(s && s[0] == '0');
    }
    return cached;
}

void shim_patch_get_format_callback(void) {
    if (!shim_decoder_patch_enabled()) return;

    uintptr_t game_base = shim_resolve_game_base();
    if (!game_base) {
        SHIM_LOG("[decoder_patch] game_base unresolved; get_format patch aborted");
        return;
    }

    uint32_t *instr = (uint32_t *)(game_base + GET_FORMAT_BNE_VA);
    uintptr_t page = (uintptr_t)instr & ~(uintptr_t)4095;
    if (mprotect((void *)page, 8192,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        SHIM_LOG("[decoder_patch] mprotect RWX @ %p failed: %s",
                 (void *)page, strerror(errno));
        return;
    }

    uint32_t old = *instr;
    /* Safety: expect `b.ne 0x920ce0` (backward 3 instructions) encoded as
     * 0x54ffff81. Refuse to patch if we see anything else — guards against
     * binary drift. */
    if (old != 0x54ffff81u) {
        SHIM_LOG("[decoder_patch] REFUSED to patch @ %p: expected 0x54ffff81 (b.ne), got 0x%08x",
                 (void *)instr, old);
        return;
    }
    *instr = AARCH64_NOP;
    __builtin___clear_cache((char *)instr, (char *)instr + 4);
    SHIM_LOG("[decoder_patch] patched get_format b.ne @ %p: 0x%08x -> 0x%08x (NOP)",
             (void *)instr, old, (unsigned)AARCH64_NOP);
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

static real_avcodec_find_decoder_by_name_t real_avcodec_find_decoder_by_name = NULL;
static real_avcodec_get_hw_config_t real_avcodec_get_hw_config = NULL;
static real_av_hwdevice_ctx_create_t real_av_hwdevice_ctx_create = NULL;

static unsigned int av_send_count = 0;
static unsigned int av_recv_count = 0;
static unsigned int av_recv_err_count = 0;
static unsigned int av_recv_ok_count = 0;

/* Helper — resolve via dlvsym so versioned symbols work.
 * The game imports avcodec_find_decoder_by_name@LIBAVCODEC_59 etc.; plain
 * dlsym(RTLD_NEXT, ...) can miss versioned symbols in some ld configs, so we
 * try both. Also try dlopen(libavcodec.so.59) as a last resort. */
void *shim_resolve_libav(const char *sym, const char *version) {
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

    /* SPIKE3_DECODER_PATCH (default-on): redirect hardware HEVC decoder
     * names to libavcodec's plain "hevc". Paired with the get_format
     * NOP patch in shim_patch_get_format_callback. Required for both
     * forward-decode and any diagnostic SW path to function — the V4L2
     * stateless m2m HEVC slice device the game asks for doesn't exist
     * in our env. */
    const char *lookup = name;
    if (shim_decoder_patch_enabled() && name &&
        (strcmp(name, "hevc_v4l2request") == 0 ||
         strcmp(name, "hevc_v4l2m2m")    == 0)) {
        SHIM_LOG("[decoder_patch] remapping decoder name '%s' -> 'hevc'", name);
        lookup = "hevc";
    }

    const void *ret = real_avcodec_find_decoder_by_name(lookup);
    SHIM_LOG("  -> %p (name='%s', lookup='%s')", ret,
             name ? name : "(null)", lookup);
    return ret;
}

/* Earlier work tried to force these to NULL/ENOSYS to prevent the game
 * from picking the DRM hwaccel path. RESULT: SIGABRT — Stern's video
 * init at SpiVideoStreamDecoderBase.cpp:1830 has `assert(hw_decoder)`
 * that requires get_hw_config + hwdevice_ctx_create to BOTH succeed.
 * There's no in-game SW fallback at this level. Kept as pure passthrough
 * — the actual decoder-substitution work happens in
 * shim_patch_get_format_callback (b.ne NOP) + the avcodec_find_decoder_by_name
 * remap above, which together let the game accept NV12 frames from
 * forward-decode (or local SW hevc as a diagnostic).
 */
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

/* (The Step 47b dummy-frame injection block lived here until 2026-05-06.
 * It allocated an NV12 1920×1080 black AVFrame and injected it on
 * avcodec_receive_frame EAGAIN — useful when the only "decode" we had was a
 * DRM socketpair that produced no real frames. Now that shim_avforward.c
 * routes packets to host NVDEC, real frames arrive and dummies are
 * unnecessary. AVFrame/AVPacket field offsets that lived here are now
 * duplicated in shim_avforward.c at AVFRAME_OFF_* / AVPKT_OFF_* — see that
 * file for the canonical copy.) */

int avcodec_send_packet(void *avctx, const void *pkt) {
    /* SPIKE3_FORWARD_DECODE is the only decode path. Forward to host
     * decode_daemon over AF_UNIX. If the bridge can't handle it (daemon
     * down, all session slots in use, OPEN rejected), HARD-FAIL with -EIO.
     * We deliberately do NOT fall back to local libavcodec SW HEVC — running
     * a pinball video game's clips on the in-VM CPU is unviable, and we
     * want bridge issues to surface loudly rather than be masked by silent
     * 5-fps SW frames that look like a game bug. */
    int fwd_ret = 0;
    if (shim_avforward_send_packet(avctx, pkt, &fwd_ret)) {
        av_send_count++;
        if (av_send_count <= 8 || (av_send_count % 500) == 0) {
            SHIM_LOG("avcodec_send_packet #%u (ctx=%p, pkt=%p) -> %d  [forward]",
                     av_send_count, avctx, pkt, fwd_ret);
        }
        return fwd_ret;
    }
    av_send_count++;
    if (av_send_count <= 8 || (av_send_count % 500) == 0) {
        SHIM_LOG("avcodec_send_packet #%u (ctx=%p, pkt=%p) -> -EIO  "
                 "[forward unreachable; SW fallback removed]",
                 av_send_count, avctx, pkt);
    }
    return -5;  /* -EIO */
}

int avcodec_receive_frame(void *avctx, void *frame) {
    /* SPIKE3_FORWARD_DECODE is the only decode path. See avcodec_send_packet
     * above for the rationale on no-SW-fallback. */
    int fwd_ret = 0;
    if (shim_avforward_receive_frame(avctx, frame, &fwd_ret)) {
        av_recv_count++;
        if (fwd_ret == 0) av_recv_ok_count++; else av_recv_err_count++;
        if (av_recv_count <= 8 || (av_recv_count % 500) == 0) {
            SHIM_LOG("avcodec_receive_frame #%u (ctx=%p, frame=%p) -> %d  [forward] (ok=%u err=%u)",
                     av_recv_count, avctx, frame, fwd_ret,
                     av_recv_ok_count, av_recv_err_count);
        }
        return fwd_ret;
    }
    av_recv_count++;
    av_recv_err_count++;
    if (av_recv_count <= 8 || (av_recv_count % 500) == 0) {
        SHIM_LOG("avcodec_receive_frame #%u (ctx=%p, frame=%p) -> -EIO  "
                 "[forward unreachable; SW fallback removed]",
                 av_recv_count, avctx, frame);
    }
    return -5;  /* -EIO */
}

