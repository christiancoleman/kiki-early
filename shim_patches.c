/*
 * shim_patches.c — runtime patches the shim applies to the game's behavior.
 *
 * Phase 8 of the refactor. Owns:
 *   - memcpy NULL-guard (Step 45): makes Godot's error-logger path on the
 *     DisplayRenderer thread non-fatal.
 *   - Step 62 SpiDecoder::Play vtable patch: replaces the real Play slot
 *     with a shim that sets update_thread_running=1 and returns 1 without
 *     waiting on FFmpeg. Gated by SPIKE3_PATCH_SPI_VIDEO=1.
 *   - Step 64 get_format callback NOP patch: NOPs the b.ne that insists on
 *     DRM_PRIME, so the callback accepts the first pix_fmt libavcodec
 *     offers. Gated by SPIKE3_SW_HEVC=1.
 *   - libavcodec HW-accel bypass (Step 47): redirects
 *     avcodec_find_decoder_by_name(hevc_v4l2request) → hevc and logs the
 *     other decoder entry points (get_hw_config / hwdevice_ctx_create /
 *     send_packet / receive_frame) as passthrough.
 *   - Dummy-frame injection (Step 47b): when SPIKE3_INJECT_VIDEO=1 and the
 *     real decoder returns EAGAIN, avcodec_receive_frame fakes a valid
 *     AVFrame so the game's render path gets SOMETHING. Shares the
 *     libavcodec machinery above.
 *
 * What's NOT here (intentionally):
 *   - shim_vp_patch_thread — the thread that applies the two patches after
 *     a 2-second settle. Stays in spi_shim.c through Phase 8, moves to
 *     shim_inject.c in Phase 9 along with the other timer threads.
 *
 * Cross-TU dependencies resolved via spi_shim.h:
 *   - SHIM_LOG macro
 *   - shim_resolve_game_base() — used by both patch functions to translate
 *     BN VAs to live runtime addresses.
 *
 * Cross-TU APIs exposed via spi_shim.h:
 *   - shim_patch_spi_video_vtable() — called from shim_vp_patch_thread.
 *   - shim_patch_get_format_callback() — called from shim_vp_patch_thread.
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

/* ========== moved from spi_shim.c (1662-1729): memcpy NULL-guard ========== */
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

/* ========== moved from spi_shim.c (819-888): Step 62 SpiDecoder::Play vtable patch ========== */
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
#define SPI_DECODER_VTABLE_VA   0x42cc9c8
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

void shim_patch_spi_video_vtable(void) {
    if (!is_real_gl()) return;
    const char *en = getenv("SPIKE3_PATCH_SPI_VIDEO");
    if (!en || en[0] != '1') return;

    uintptr_t game_base = shim_resolve_game_base();
    if (!game_base) {
        SHIM_LOG("[vp] game_base unresolved; patch aborted");
        return;
    }

    void **slot = (void**)(game_base +
        SPI_DECODER_VTABLE_VA + SPI_DECODER_PLAY_SLOT);
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

/* ========== Step 65 (2026-04-26): AudioDriver abstract vtable slot 7 patch ========== */
/* ---- Audio mixer thread pure-virtual abort fix ----
 *
 * The audio mixer thread (BN: sub_25609b4 = AudioDriverDummy::thread_func)
 * calls a virtual via `*(*arg1 + 0x38)` (slot 7) on its own this-pointer
 * once per mix cycle. The concrete AudioDriverDummy vtable has sub_2560ac0
 * (a no-op) at slot 7 and the calling code has a safety check
 * `if (slot7 != sub_2560ac0)` to skip the call entirely. Normal mixing
 * runs fine — the safety check is hit and the thread proceeds to the real
 * mix call (sub_2372390).
 *
 * During engine teardown, the destructor chain resets the vptr to the
 * abstract `AudioDriver` vtable BEFORE the audio thread has noticed the
 * exit_thread flag. The abstract vtable (at BN VA 0x4324a28 post-rebase)
 * has slot 7 = __cxa_pure_virtual (via .got.plt entry at 0x46ab5c0). The
 * safety check passes (slot7 != sub_2560ac0 IS true), the call proceeds,
 * we get "pure virtual method called" → terminate → SIGABRT.
 *
 * Discovery path: Step 65 SIGABRT handler captured backtrace; frame [09]
 * was inside sub_25609b4 at 0x2560ab4 calling vtable slot 7. AudioDriver
 * typeinfo at 0x43247c0; xref → vtable at 0x4324a28; slot 7 at +0x40.
 *
 * Fix: at startup, overwrite slot 7 of the abstract AudioDriver vtable
 * with sub_2560ac0. Then during teardown, when vptr resets to the abstract
 * vtable, the audio thread still sees sub_2560ac0, the safety check skips
 * the call, and the thread cleanly observes exit_thread and terminates.
 *
 * The author of Step 62 hypothesized this exact abort but blamed
 * VideoPlayerSpi destruction. Their patch defused a video-side symptom
 * (Play timeout) but the actual abort was always in audio.
 *
 * Gated by SPIKE3_PATCH_AUDIO_ABRT=1 so we can A/B compare. Safe to leave
 * on long-term once verified — the patched value matches the AudioDriver-
 * Dummy concrete vtable's slot 7 exactly.
 *
 * Address convention: constants below are ELF VAs (== current BN VAs,
 * since the BN database is rebased to base 0x0). Add to game_base for
 * runtime addresses.
 */
#define AUDIO_DRIVER_VTABLE_VA       0x4324a28   /* AudioDriver vtable */
#define AUDIO_DRIVER_VPTR_OFF        0x08        /* skip typeinfo to first vfunc */
#define AUDIO_DRIVER_SLOT7_NOOP_VA   0x2560ac0   /* sub_2560ac0 — AudioDriverDummy slot 7 default */
#define AUDIO_DRIVER_RET_ZERO_VA     0x23725d0   /* sub_23725d0 — `return 0` (AudioServer slot 5 default) */

/* Slots in the AudioDriver abstract vtable that are __cxa_pure_virtual at
 * static-link time but get resolved to libstdc++ __cxa_pure_virtual at runtime,
 * causing aborts whenever the audio thread or AudioServer dispatches through
 * a partly-constructed/destructed driver instance.
 *
 * Slots 0 & 1 are virtual destructors (D1, D0 in Itanium ABI) — leaving those
 * alone; replacing them would skip member cleanup. The remaining pure slots
 * we replace with safe defaults: slot 7 with sub_2560ac0 (matches
 * AudioDriverDummy's value, hits the existing safety check elsewhere); the
 * rest with sub_23725d0 (literal `return 0`) which produces a sane default
 * for any int-returning method (channel count, sample rate, latency, etc.).
 *
 * Discovered via Step 65 SIGABRT dump showing *data_451ac48's vtable was the
 * AudioDriver abstract vtable. After patching slot 7, slot 5 was the next to
 * fire. Pre-emptively covering 2, 3, 5, 8 in one pass to avoid iterating.
 */
struct shim_audio_slot_patch { uint32_t off; uintptr_t target_va; const char *desc; };
static const struct shim_audio_slot_patch shim_audio_slots[] = {
    /* offsets are from the vtable base; vfunc[N] is at base + 8 + N*8. */
    { 0x08 + 2 * 8, 0x23725d0, "slot 2 -> return 0" },
    { 0x08 + 3 * 8, 0x23725d0, "slot 3 -> return 0" },
    { 0x08 + 5 * 8, 0x23725d0, "slot 5 -> return 0" },
    { 0x08 + 7 * 8, 0x2560ac0, "slot 7 -> sub_2560ac0 (AudioDriverDummy default)" },
    { 0x08 + 8 * 8, 0x23725d0, "slot 8 -> return 0" },
};

void shim_patch_audio_driver_vtable(void) {
    if (!is_real_gl()) return;
    const char *en = getenv("SPIKE3_PATCH_AUDIO_ABRT");
    if (!en || en[0] != '1') return;

    uintptr_t game_base = shim_resolve_game_base();
    if (!game_base) {
        SHIM_LOG("[audio] game_base unresolved; AudioDriver vtable patch aborted");
        return;
    }

    /* Open the page once for the whole vtable region. */
    void **first = (void**)(game_base + AUDIO_DRIVER_VTABLE_VA);
    uintptr_t page = (uintptr_t)first & ~(uintptr_t)4095;
    if (mprotect((void*)page, 8192,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        SHIM_LOG("[audio] mprotect RWX @ %p failed: %s",
                 (void*)page, strerror(errno));
        return;
    }

    size_t n = sizeof(shim_audio_slots) / sizeof(shim_audio_slots[0]);
    for (size_t i = 0; i < n; i++) {
        const struct shim_audio_slot_patch *p = &shim_audio_slots[i];
        void **slot = (void**)(game_base + AUDIO_DRIVER_VTABLE_VA + p->off);
        void *old = *slot;
        void *new_target = (void*)(game_base + p->target_va);
        *slot = new_target;
        SHIM_LOG("[audio] AudioDriver vtable %s @ %p: %p -> %p",
                 p->desc, (void*)slot, old, new_target);
    }
}

/* ========== moved from spi_shim.c (890-966): Step 64 get_format NOP patch ========== */
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
/* objdump shows the b.ne at ELF VA 0x920cf0; we patch game_base + that. */
#define GET_FORMAT_BNE_VA   0x920cf0
#define AARCH64_NOP            0xd503201fu

static int shim_sw_hevc_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *s = getenv("SPIKE3_SW_HEVC");
        cached = (s && s[0] == '1') ? 1 : 0;
    }
    return cached;
}

void shim_patch_get_format_callback(void) {
    if (!shim_sw_hevc_enabled()) return;

    uintptr_t game_base = shim_resolve_game_base();
    if (!game_base) {
        SHIM_LOG("[sw_hevc] game_base unresolved; get_format patch aborted");
        return;
    }

    uint32_t *instr = (uint32_t *)(game_base +
        GET_FORMAT_BNE_VA);
    uintptr_t page = (uintptr_t)instr & ~(uintptr_t)4095;
    if (mprotect((void *)page, 8192,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        SHIM_LOG("[sw_hevc] mprotect RWX @ %p failed: %s",
                 (void *)page, strerror(errno));
        return;
    }

    uint32_t old = *instr;
    /* Safety: expect `b.ne 0x920ce0` (backward 3 instructions) encoded as
     * 0x54ffff81. Refuse to patch if we see anything else. */
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

/* ========== moved from spi_shim.c (1731-1866): libavcodec HW-accel bypass ========== */
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

/* Step 72i tried to force these to NULL/ENOSYS when SPIKE3_SW_HEVC=1.
 * RESULT: SIGABRT — Stern's video init at SpiVideoStreamDecoderBase.cpp:1830
 * has `assert(hw_decoder)` that requires get_hw_config + hwdevice_ctx_create
 * to BOTH succeed. There is no SW fallback at this level. Reverted to
 * passthrough. The visible "frozen attract" is NOT caused by decoder EAGAIN
 * (pre-fix: 50% receive_frame ok is normal SW HEVC latency, game tolerates
 * it) — it's caused by VideoPlayerSpi not being in the Godot scene tree
 * (can_process returns false → no _process() ticks → frames decoded but
 * never displayed). See JOURNAL Step 71 + Step 72j.
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

/* ========== moved from spi_shim.c (1868-2020): Dummy-frame injection ========== */
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
    /* SPIKE3_FORWARD_DECODE=1: route to host decode_daemon over AF_UNIX. */
    {
        int fwd_ret = 0;
        if (shim_avforward_send_packet(avctx, pkt, &fwd_ret)) {
            av_send_count++;
            if (fwd_ret == 0) shim_pending_dummies++;
            if (av_send_count <= 8 || (av_send_count % 500) == 0) {
                SHIM_LOG("avcodec_send_packet #%u (ctx=%p, pkt=%p) -> %d  [forward]",
                         av_send_count, avctx, pkt, fwd_ret);
            }
            return fwd_ret;
        }
    }

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
    /* SPIKE3_FORWARD_DECODE=1: pull frame from host decode_daemon. */
    {
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
    }

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



































/* ========== Step 71: VideoPlayerSpi !is_inside_tree() assertion bypass ==========
 *
 * VideoPlayerSpi::play() at sub_e3819c contains a tbz at BN VA 0xe381b4 that
 * branches to an _err_print_error path when bit 22 of the Node base flag at
 * +0x2c0 (NODE_FLAG_IN_TREE) is clear. Per the Step-71 BN investigation, the
 * GDScript caller (bkgnd_video_player_manager.gd:86 — extracted scratch HINT
 * only) fires play() inside an AsyncLoadUtilV2.load_resource() callback,
 * which can resolve before the player is added to the parent scene tree.
 *
 * NOPing the tbz lets play() fall through to the playback object load
 * (this+0x988 → VideoStreamPlaybackSpi). If the playback object is NULL the
 * existing cbz at 0xe381bc skips the cast and play() exits cleanly. If
 * non-NULL, vtable+0x110 is invoked and Radium's playback engages — which
 * is the breakthrough we want.
 *
 * Always-on (no env gate) per feedback_stay_in_shim_and_sysroot. Safe even
 * if the player IS in the tree: the NOP is functionally identical to the
 * tbz when bit 22 is set (tbz falls through, NOP falls through).
 */
#define VIDEO_PLAYER_TBZ_VA  0xe381b4
#define EXPECTED_TBZ_BYTES   0x36b005c0u  /* tbz w0, #22, +0xb8 */

void shim_patch_video_player_assertion(void) {
    uintptr_t game_base = shim_resolve_game_base();
    if (!game_base) {
        SHIM_LOG("[vp_assert] game_base unresolved; assertion patch aborted");
        return;
    }

    uint32_t *instr = (uint32_t *)(game_base + VIDEO_PLAYER_TBZ_VA);
    uintptr_t page = (uintptr_t)instr & ~(uintptr_t)4095;
    if (mprotect((void *)page, 8192,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        SHIM_LOG("[vp_assert] mprotect RWX @ %p failed: %s",
                 (void *)page, strerror(errno));
        return;
    }

    uint32_t old = *instr;
    if (old != EXPECTED_TBZ_BYTES) {
        SHIM_LOG("[vp_assert] REFUSED to patch @ %p: expected 0x%08x (tbz w0,#22), got 0x%08x",
                 (void *)instr, EXPECTED_TBZ_BYTES, old);
        return;
    }
    *instr = AARCH64_NOP;
    __builtin___clear_cache((char *)instr, (char *)instr + 4);
    SHIM_LOG("[vp_assert] patched VideoPlayerSpi !is_inside_tree() tbz @ %p: 0x%08x -> 0x%08x (NOP)",
             (void *)instr, old, (unsigned)AARCH64_NOP);
}

/* ========== Step 71: VideoPlayerSpi::play() entry probe via brk ==========
 *
 * Patches the NOP at sub_e3819c (function entry; BN VA 0xe3819c) with
 * `brk #0`. When play() is called, qemu delivers SIGTRAP to the guest;
 * shim_sigtrap_handler in spi_shim.c reads x0 (=this), peeks at this+0x2c0
 * (the in-tree flag) and this+0x988 (the Radium playback object pointer),
 * logs them, advances PC past the brk, and returns. The skipped instruction
 * is a NOP, so no behavior change.
 *
 * This is the diagnostic the user requested under "trampoline log" in
 * Step 71. SIGTRAP is functionally a software-defined breakpoint at the
 * shim level — no nearby trampoline page allocation needed.
 */
#define VIDEO_PLAYER_PLAY_ENTRY_VA  0xe3819c
#define EXPECTED_PLAY_ENTRY_BYTES   0xd503201fu  /* NOP — confirmed by Step 71 dump */
#define AARCH64_BRK_0               0xd4200000u

void shim_patch_video_player_play_entry_brk(void) {
    uintptr_t game_base = shim_resolve_game_base();
    if (!game_base) {
        SHIM_LOG("[vp_brk] game_base unresolved; play()-entry probe aborted");
        return;
    }

    uint32_t *instr = (uint32_t *)(game_base + VIDEO_PLAYER_PLAY_ENTRY_VA);
    uintptr_t page = (uintptr_t)instr & ~(uintptr_t)4095;
    if (mprotect((void *)page, 8192,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        SHIM_LOG("[vp_brk] mprotect RWX @ %p failed: %s",
                 (void *)page, strerror(errno));
        return;
    }

    uint32_t old = *instr;
    if (old != EXPECTED_PLAY_ENTRY_BYTES) {
        SHIM_LOG("[vp_brk] REFUSED to patch @ %p: expected NOP 0x%08x, got 0x%08x",
                 (void *)instr, EXPECTED_PLAY_ENTRY_BYTES, old);
        return;
    }
    /* Set the runtime addr BEFORE writing the brk so the SIGTRAP handler
     * recognizes the trap site even if the first trap fires immediately. */
    shim_vp_play_entry_runtime = (uintptr_t)instr;
    __sync_synchronize();
    *instr = AARCH64_BRK_0;
    __builtin___clear_cache((char *)instr, (char *)instr + 4);
    SHIM_LOG("[vp_brk] patched play() entry @ %p: 0x%08x -> 0x%08x (brk #0)",
             (void *)instr, old, (unsigned)AARCH64_BRK_0);
}

/* ========== Step 72: sub_8edfe0 BRK-trap synthesizer ==========
 *
 * Replaces the in-binary nodebus-read shim with a shim-side synthesizer
 * that lets us inject switch closures into the game's translator chain
 * (sub_8edfe0 → sub_7b4d00 → sub_651610 → pending-list drainer).
 *
 * Verified bytes at game_base + 0x8edfe0 (Step 72 BN session 2026-04-30):
 *   0x8edfe0  02 00 80 d2     mov   x2, #0          ; arg3 = NULL
 *   0x8edfe4  b7 ff ff 17     b     sub_8edec0      ; tail call (-292 bytes)
 *
 * Function is exactly 8 bytes. We patch the first instruction with `brk #0`.
 * The SIGTRAP handler (shim_sigtrap_handler in spi_shim.c) reads x0
 * (channel) + x1 (out_buf), populates the 8-byte buffer per the arm-state,
 * sets x0 = 1 (success), and sets pc = lr — which returns to the caller
 * (sub_674b90 at 0x674d24) without executing the rest of sub_8edfe0.
 *
 * Side-effect review (per BN agent): sub_8edec0/sub_8edfe0 has no global
 * counters; it just XOR-decrypts a network response into arg2. The caller
 * gates `data_4482150[arg1] = 1` on success — our return-1 keeps that
 * bookkeeping intact. Skipping the function is safe.
 *
 * Bypassing the real wire path means the channel buffers our shim gives
 * back are fully synthetic. That's fine for the wedge-breaking experiment
 * because shim_nodebus.c already synthesizes empty/dummy bridge responses
 * — there is no real wire to read from. The real cabinet/playfield switch
 * data was always going to be zero with the current netbridge stub.
 *
 * Always-on (no env gate) once enabled, because shim_fire_switch needs
 * the trap site to be patched before it can arm a synthesis window.
 * Self-protective: refuses to patch if the bytes don't match the expected
 * `mov x2, #0` (so a future game build with a different sub_8edfe0 layout
 * fails loudly rather than silently corrupting random instructions).
 */
#define SUB_8EDFE0_VA              0x8edfe0
#define EXPECTED_SUB_8EDFE0_BYTES  0xd2800002u  /* mov x2, #0 — verified Step 72 */

/* ========== Step 72g: fire_event selective-return BRK ==========
 *
 * Forces fire_event(event_id, ...) to return 1 for event_ids in
 * {0x80, 0x81, 0x82, 0x83} — the four orchestrator gates per the
 * Step 72c BN trace of mcp_nodebus_init_orchestrator and sub_675d10.
 * For all other event_ids the trap handler emulates the function's
 * first instruction in C (`stp x19, x20, [sp, #-32]!`) and advances
 * pc by 4 so the rest of fire_event runs normally.
 *
 * Why selective: a blanket return-1 patch would corrupt all events,
 * including ones that route to legitimate game-state handlers. We
 * only want to force the orchestrator-gating events.
 *
 * Verified bytes at game_base + 0x5c12a0 (Step 72g BN session 2026-04-30):
 *   0x5c12a0  f3 53 be a9    stp x19, x20, [sp, #-32]!  ; frame setup
 *
 * Self-gated on SPIKE3_FORCE_NODEBUS_GATES=1.
 */
#define FIRE_EVENT_VA              0x5c12a0
#define EXPECTED_FIRE_EVENT_BYTES  0xa9be53f3u  /* stp x19, x20, [sp, #-32]! — verified Step 72g */

void shim_patch_fire_event_brk(void) {
    const char *en = getenv("SPIKE3_FORCE_NODEBUS_GATES");
    if (!en || en[0] != '1') return;

    uintptr_t game_base = shim_resolve_game_base();
    if (!game_base) {
        SHIM_LOG("[fe_brk] game_base unresolved; fire_event BRK aborted");
        return;
    }

    uint32_t *instr = (uint32_t *)(game_base + FIRE_EVENT_VA);
    uintptr_t page = (uintptr_t)instr & ~(uintptr_t)4095;
    if (mprotect((void *)page, 8192,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        SHIM_LOG("[fe_brk] mprotect RWX @ %p failed: %s",
                 (void *)page, strerror(errno));
        return;
    }

    uint32_t old = *instr;
    if (old == AARCH64_BRK_0) {
        SHIM_LOG("[fe_brk] fire_event already patched @ %p; skipping",
                 (void *)instr);
        shim_fire_event_runtime = (uintptr_t)instr;
        return;
    }
    if (old != EXPECTED_FIRE_EVENT_BYTES) {
        SHIM_LOG("[fe_brk] REFUSED to patch fire_event @ %p: expected 0x%08x (stp x19,x20,[sp,#-32]!), got 0x%08x",
                 (void *)instr, (unsigned)EXPECTED_FIRE_EVENT_BYTES, old);
        return;
    }

    shim_fire_event_runtime = (uintptr_t)instr;
    __sync_synchronize();
    *instr = AARCH64_BRK_0;
    __builtin___clear_cache((char *)instr, (char *)instr + 4);
    SHIM_LOG("[fe_brk] patched fire_event @ %p: 0x%08x -> 0x%08x (brk #0)",
             (void *)instr, old, (unsigned)AARCH64_BRK_0);
}

void shim_patch_sub_8edfe0_brk(void) {
    /* Self-gate: install when explicitly requested (SPIKE3_PATCH_SUB_8EDFE0=1)
     * OR implicitly when SPIKE3_FIRE_SWITCH is set (since shim_fire_switch
     * needs the trap to drive switch 93's wire path). Skipping otherwise
     * preserves the real wire-read behavior for runs that don't want
     * synthesized channel state. */
    {
        const char *p_sw = getenv("SPIKE3_PATCH_SUB_8EDFE0");
        const char *fs   = getenv("SPIKE3_FIRE_SWITCH");
        int want = (p_sw && p_sw[0] == '1') || (fs && fs[0]);
        if (!want) return;
    }

    uintptr_t game_base = shim_resolve_game_base();
    if (!game_base) {
        SHIM_LOG("[sw_brk] game_base unresolved; sub_8edfe0 BRK aborted");
        return;
    }

    uint32_t *instr = (uint32_t *)(game_base + SUB_8EDFE0_VA);
    uintptr_t page = (uintptr_t)instr & ~(uintptr_t)4095;
    if (mprotect((void *)page, 8192,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        SHIM_LOG("[sw_brk] mprotect RWX @ %p failed: %s",
                 (void *)page, strerror(errno));
        return;
    }

    uint32_t old = *instr;
    if (old == AARCH64_BRK_0) {
        SHIM_LOG("[sw_brk] sub_8edfe0 already patched @ %p; skipping",
                 (void *)instr);
        shim_sub_8edfe0_runtime = (uintptr_t)instr;
        return;
    }
    if (old != EXPECTED_SUB_8EDFE0_BYTES) {
        SHIM_LOG("[sw_brk] REFUSED to patch sub_8edfe0 @ %p: expected 0x%08x (mov x2,#0), got 0x%08x",
                 (void *)instr, (unsigned)EXPECTED_SUB_8EDFE0_BYTES, old);
        return;
    }
    /* Publish the runtime address BEFORE writing brk so the SIGTRAP handler
     * recognizes the trap site even if the first trap fires immediately. */
    shim_sub_8edfe0_runtime = (uintptr_t)instr;
    __sync_synchronize();
    *instr = AARCH64_BRK_0;
    __builtin___clear_cache((char *)instr, (char *)instr + 4);
    SHIM_LOG("[sw_brk] patched sub_8edfe0 @ %p: 0x%08x -> 0x%08x (brk #0)",
             (void *)instr, old, (unsigned)AARCH64_BRK_0);
}
