/*
 * shim_gl.c — OpenGL ES + EGL + GBM surface unified in one source file.
 *
 * Combines three subsystems into a single translation unit because they
 * share state:
 *
 *   - GBM: fake gbm_device / gbm_bo / gbm_surface objects (so mesa's libgbm
 *     doesnt dereference our handles). Cheap stubs with a tiny per-BO side
 *     table for gbm_bo_get_width/height/format/stride.
 *
 *   - EGL: SURFACELESS_MESA display translation, Mesa config capture,
 *     eglChooseConfig WINDOW→PBUFFER surface-type patching, fallback-pbuffer
 *     injection at eglMakeCurrent(draw=NULL). Plus the KHR image extension
 *     (we fake DMA-BUF import with a regular GL texture).
 *
 *   - GLES 3.x: the big pile of stubs. In stub mode (SPIKE3_REAL_GL=0) each
 *     returns a plausible value. In real-GL mode the FWD_* macros short-
 *     circuit to dlsym(RTLD_NEXT, ...) forwarding to Mesa.
 *
 *   - shim_frametex texture tracker — per-texture {tex_id, target, w, h} map
 *     populated from glEGLImageTargetTexture2DOES so the page-flip frame
 *     capture in shim_bridges.c can later pick the main render target.
 *
 *   - eglGetProcAddress — routing table so the game's extension lookups land
 *     on our EGL overrides even when it bypasses the dynamic linker.
 *
 * Cross-TU boundary is narrow:
 *   - shim_last_draw_fbo (exported) — set by glBindFramebuffer, read by
 *     shim_frame_capture_at_page_flip in shim_bridges.c.
 *   - shim_frame_capture_at_page_flip() — called by glEGLImageTargetTexture2DOES
 *     via shim_frametex_record (itself a no-op outside real-GL mode).
 *
 * All other state (config capture globals, KHR image map, frametex pool,
 * GBM BO table, display/config captures) is internal to this file.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <stdarg.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/syscall.h>

#include "spi_shim.h"
#include "gl_proto.h"

/* Exported so shim_bridges.c's frame capture can read the game's most recent
 * draw-FBO binding. Definition still lives with glBindFramebuffer inside
 * this file; extern decls in the other TUs pick it up. */
extern volatile unsigned shim_last_draw_fbo;

/* Forward decls for the three functions referenced by the
 * eglGetProcAddress table. Bodies live below in this same TU but the table
 * is authored toward the end of the file — forward-decls keep GCC happy
 * regardless of ordering. */
void glFramebufferTexture2D(unsigned, unsigned, unsigned, unsigned, int);

static int  gl_marshal_gen_ids(uint16_t op, int n, unsigned *out);
static void gl_marshal_delete_ids(uint16_t op, int n, const unsigned *ids);
void glFramebufferTextureLayer(unsigned, unsigned, unsigned, int, int);
void glFramebufferRenderbuffer(unsigned, unsigned, unsigned, unsigned);

void *eglGetPlatformDisplayEXT(unsigned, void *, const int *);
void *eglGetPlatformDisplay(unsigned, void *, const intptr_t *);
void *eglCreateImageKHR(void *, void *, unsigned, void *, const int *);
unsigned eglDestroyImageKHR(void *, void *);
void glEGLImageTargetTexture2DOES(unsigned, void *);

static void shim_frametex_record(unsigned target, int w, int h);


/* The game presents via DRM MODE_PAGE_FLIP, not eglSwapBuffers, so the
 * capture trigger sits in the DRM ioctl path below. The target is whichever
 * GL texture the game's create_egl_framebuffer bound as an FBO color
 * attachment via glEGLImageTargetTexture2DOES — our override already runs
 * glTexImage2D(w, h, RGBA8) to give it Mesa-allocated storage. We track
 * every texture we touched that way; at page-flip we read from the largest
 * one (assumed to be the main render target). */

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
static unsigned shim_capture_fbo = 0;

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

/* libgbm's real gbm_create_device inspects the DRM fd's driver identity via
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
    (void)bo;
    /* Game's create_egl_framebuffer flow:
     *   fd = gbm_bo_get_fd(bo);          <-- here
     *   if (fd < 0) error("failed to get fd for bo");
     *   image = eglCreateImageKHR(... fd ...);
     *   close(fd);
     *   ... build GL texture/framebuffer from image ...
     *
     * Our eglCreateImageKHR override doesn't dereference the fd — it parses
     * the EGL attribute list (width/height/fourcc), allocates a sentinel,
     * and the eventual glEGLImageTargetTexture2DOES gives the texture real
     * storage via glTexImage2D. So the fd only has to satisfy "(fd >= 0)
     * and close(fd) succeeds". A memfd is the simplest valid fd that
     * survives close() cleanly under qemu-user. */
    int fd = (int)syscall(SYS_memfd_create, "shim_dma_buf_stub", 1u /* MFD_CLOEXEC */);
    if (fd < 0) {
        SHIM_LOG("gbm_bo_get_fd: memfd_create failed (%s); returning -1",
                 strerror(errno));
    }
    return fd;
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

/* Once gbm_create_device returns our fake handle, mesa's libEGL would
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

/* Forward decls — defined alongside eglMakeCurrent further down. Captured
 * here so the fallback-pbuffer path at makeCurrent time has something to
 * pass to Mesa's eglCreatePbufferSurface(display, config, ...). */
extern void *shim_last_real_display;
extern void *shim_last_real_config;
extern void *shim_fallback_pbuf;

static void *shim_real_platform_display(unsigned platform) {
    (void)platform;
    static void *(*real_1_5)(unsigned, void*, const intptr_t*) = NULL;
    if (!real_1_5) real_1_5 = (void *(*)(unsigned, void*, const intptr_t*))
                              dlsym(RTLD_NEXT, "eglGetPlatformDisplay");
    if (real_1_5) {
        void *d = real_1_5(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
        if (d) {
            SHIM_LOG("  → Mesa real display %p via eglGetPlatformDisplay(SURFACELESS)", d);
            shim_last_real_display = d;
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
    if (shim_gl_marshal_active()) {
        SHIM_LOG("eglGetDisplay(%p) -> fake display [daemon]", native_display);
        return FAKE_EGL_DISPLAY;
    }
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
    if (shim_gl_marshal_active()) {
        SHIM_LOG("eglGetPlatformDisplay(platform=0x%x, dpy=%p) -> fake [daemon]",
                 platform, native_display);
        (void)attrib_list; return FAKE_EGL_DISPLAY;
    }
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
    if (shim_gl_marshal_active()) {
        SHIM_LOG("eglGetPlatformDisplayEXT(platform=0x%x, dpy=%p) -> fake [daemon]",
                 platform, native_display);
        (void)attrib_list; return FAKE_EGL_DISPLAY;
    }
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
    if (shim_gl_marshal_active()) {
        if (major) *major = 1;
        if (minor) *minor = 5;
        SHIM_LOG("eglInitialize(%p) -> 1.5 [daemon]", display);
        return EGL_TRUE;
    }
    FWD_RET_V(unsigned, eglInitialize, (void*, int*, int*), (display, major, minor));
    if (major) *major = 1;
    if (minor) *minor = 5;
    SHIM_LOG("eglInitialize(%p) -> 1.5", display);
    return EGL_TRUE;
}

unsigned eglTerminate(void *display) {
    if (shim_gl_marshal_active()) { (void)display; return EGL_TRUE; }
    FWD_RET_V(unsigned, eglTerminate, (void*), (display));
    (void)display; return EGL_TRUE;
}

const char *eglQueryString(void *display, int name) {
    if (!shim_gl_marshal_active()) {
        FWD_RET_V(const char*, eglQueryString, (void*, int), (display, name));
    }
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
    if (shim_gl_marshal_active()) {
        SHIM_LOG("eglBindAPI(0x%x) [daemon]", api);
        return EGL_TRUE;
    }
    FWD_RET_V(unsigned, eglBindAPI, (unsigned), (api));
    SHIM_LOG("eglBindAPI(0x%x)", api);
    return EGL_TRUE;
}

unsigned eglGetConfigs(void *display, void **configs, int config_size, int *num_config) {
    if (!shim_gl_marshal_active()) {
        FWD_RET_V(unsigned, eglGetConfigs, (void*, void**, int, int*),
                (display, configs, config_size, num_config));
    }
    (void)display;
    if (num_config) *num_config = 1;
    if (configs && config_size >= 1) configs[0] = FAKE_EGL_CONFIG;
    return EGL_TRUE;
}

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
    if (shim_gl_marshal_active()) {
        (void)display; (void)attrib_list;
        if (num_config) *num_config = 1;
        if (configs && config_size >= 1) configs[0] = FAKE_EGL_CONFIG;
        return EGL_TRUE;
    }
    if (__builtin_expect(is_real_gl(), 0)) {
        /* Copy the game's attrib list into a local scratch so we can patch
         * EGL_SURFACE_TYPE = EGL_WINDOW_BIT (0x4) → EGL_PBUFFER_BIT (0x1).
         * Mesa surfaceless only advertises PBUFFER configs, so a WINDOW_BIT
         * ask returns zero matches. Our eglCreateWindowSurface interceptor
         * already redirects to eglCreatePbufferSurface (Step 55.4), so this
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
    if (!shim_gl_marshal_active() && __builtin_expect(is_real_gl(), 0)) {
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
    if (shim_gl_marshal_active()) {
        (void)display; (void)config; (void)share_context; (void)attrib_list;
        return FAKE_EGL_CONTEXT;
    }
    FWD_RET_V(void*, eglCreateContext, (void*, void*, void*, const int*),
            (display, config, share_context, attrib_list));
    SHIM_LOG("eglCreateContext -> fake context");
    (void)display; (void)config; (void)share_context; (void)attrib_list;
    return FAKE_EGL_CONTEXT;
}

unsigned eglDestroyContext(void *display, void *context) {
    if (shim_gl_marshal_active()) { (void)display; (void)context; return EGL_TRUE; }
    FWD_RET_V(unsigned, eglDestroyContext, (void*, void*), (display, context));
    (void)display; (void)context; return EGL_TRUE;
}

void *eglCreateWindowSurface(void *display, void *config, void *native_window,
                             const int *attrib_list) {
    if (shim_gl_marshal_active()) {
        SHIM_LOG("eglCreateWindowSurface(native=%p) -> fake surface [daemon]", native_window);
        (void)display; (void)config; (void)attrib_list;
        return FAKE_EGL_SURFACE;
    }
    /* Step 55.2 — in real-GL mode, window surfaces don't work because our
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
    if (shim_gl_marshal_active()) {
        (void)display; (void)config; (void)attrib_list;
        return FAKE_EGL_SURFACE;
    }
    FWD_RET_V(void*, eglCreatePbufferSurface, (void*, void*, const int*),
            (display, config, attrib_list));
    (void)display; (void)config; (void)attrib_list;
    return FAKE_EGL_SURFACE;
}

unsigned eglDestroySurface(void *display, void *surface) {
    if (shim_gl_marshal_active()) { (void)display; (void)surface; return EGL_TRUE; }
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
    if (shim_gl_marshal_active()) {
        SHIM_LOG("eglMakeCurrent(dpy=%p, draw=%p, read=%p, ctx=%p) [daemon]",
                 display, draw, read, context);
        return EGL_TRUE;
    }
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
    if (shim_gl_marshal_active()) { (void)display; (void)surface; return EGL_TRUE; }
    /* Plain forward — capture trigger moved to MODE_PAGE_FLIP (Step 57.2)
     * because this game uses DRM page-flip for presentation and never
     * actually calls eglSwapBuffers in practice. Left in place as a pure
     * forward so any future code path that DOES call it still works. */
    FWD_RET_V(unsigned, eglSwapBuffers, (void*, void*), (display, surface));
    (void)display; (void)surface; return EGL_TRUE;
}

unsigned eglSwapInterval(void *display, int interval) {
    if (shim_gl_marshal_active()) { (void)display; (void)interval; return EGL_TRUE; }
    FWD_RET_V(unsigned, eglSwapInterval, (void*, int), (display, interval));
    (void)display; (void)interval; return EGL_TRUE;
}

unsigned eglSurfaceAttrib(void *display, void *surface, int attribute, int value) {
    if (shim_gl_marshal_active()) {
        (void)display; (void)surface; (void)attribute; (void)value; return EGL_TRUE;
    }
    FWD_RET_V(unsigned, eglSurfaceAttrib, (void*, void*, int, int),
            (display, surface, attribute, value));
    (void)display; (void)surface; (void)attribute; (void)value; return EGL_TRUE;
}

unsigned eglQuerySurface(void *display, void *surface, int attribute, int *value) {
    if (!shim_gl_marshal_active()) {
        FWD_RET_V(unsigned, eglQuerySurface, (void*, void*, int, int*),
                (display, surface, attribute, value));
    }
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
    if (shim_gl_marshal_active()) return EGL_SUCCESS;
    FWD_RET_V(int, eglGetError, (void), ());
    return EGL_SUCCESS;
}

/* Extension stubs resolved via eglGetProcAddress AND exported top-level so
 * direct PLT calls from the game also land here. The KHR image/sync family
 * plus glEGLImageTargetTexture2DOES are never forwarded to Mesa (Step 56's
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
    void    *egl_image;
    int      width;
    int      height;
    unsigned fourcc;     /* DRM FOURCC from EGL_LINUX_DRM_FOURCC_EXT */
    /* When the game's eglCreateImageKHR carried an avforward sentinel fd in
     * EGL_DMA_BUF_PLANE0_FD_EXT, we cache the live AVFrame RGBA buffer
     * pointer here so glEGLImageTargetTexture2DOES can upload directly via
     * glTexImage2D. NULL for non-video FBO-storage images. */
    const uint8_t *rgba_ptr;
    int      stride;
} shim_egl_image_map[SHIM_EGL_IMAGE_MAP_SIZE];
static int shim_egl_image_map_count = 0;
static pthread_mutex_t shim_egl_image_map_lock = PTHREAD_MUTEX_INITIALIZER;

/* Mesa's swrast llvmpipe rejects GL_TEXTURE_EXTERNAL_OES (0x8D65) on
 * glTexImage2D / glTexSubImage2D / glTexParameter* with GL_INVALID_ENUM —
 * per the GL_OES_EGL_image_external spec, only glEGLImageTargetTexture2DOES
 * may set EXTERNAL_OES texture state. To get pixels into video textures we
 * demote EXTERNAL_OES to GL_TEXTURE_2D consistently across the texture-state
 * hooks, so the texture object lives as a 2D RGBA8 throughout. Mesa's GLSL
 * accepts samplerExternalOES sampling a 2D RGBA texture with sampler2D
 * semantics — the OES extension's distinguishing behavior is YUV→RGB which
 * is irrelevant here since we hand it RGBA bytes. */
static inline unsigned shim_demote_external_oes(unsigned target) {
    return (target == 0x8D65) ? 0x0DE1 : target;
}

/* Counter-based EGLImage sentinel. Earlier versions used malloc(1) which
 * caused address reuse after eglDestroyImageKHR's free() — the game's
 * FUN_01df8b40 cache check (`if (cached == new) return;`) then false-hit
 * and skipped re-binding. A monotonic counter cast to a pointer stays
 * unique for the run; the game treats EGLImage as opaque. */
static void *shim_egl_image_alloc_sentinel(void) {
    static uintptr_t seq = 0xdeadbeef0000ull;
    return (void *)__atomic_add_fetch(&seq, 8, __ATOMIC_RELAXED);
}

/* EGLImage attribute decoder. Called with the int32 attribute array the
 * game hands to eglCreateImageKHR for a EGL_LINUX_DMA_BUF_EXT import.
 * Attribute enums: EGL_WIDTH=0x3057, EGL_HEIGHT=0x3056,
 * EGL_LINUX_DRM_FOURCC_EXT=0x3271, EGL_DMA_BUF_PLANE0_FD_EXT=0x3272,
 * terminator EGL_NONE=0x3038. Loop bound 64 holds 4 planes' worth of
 * key/value/modifier ints with headroom. */
static void shim_egl_image_parse_attribs(const int *attrib_list,
                                         int *w, int *h, unsigned *fmt,
                                         int *plane0_fd) {
    if (!attrib_list) return;
    for (int i = 0; attrib_list[i] != 0x3038 && i < 64; i += 2) {
        switch (attrib_list[i]) {
            case 0x3057: *w         = attrib_list[i+1]; break;
            case 0x3056: *h         = attrib_list[i+1]; break;
            case 0x3271: *fmt       = (unsigned)attrib_list[i+1]; break;
            case 0x3272: *plane0_fd = attrib_list[i+1]; break;
        }
    }
}

static void *shim_egl_image_register(int w, int h, unsigned fmt,
                                     const uint8_t *rgba_ptr, int stride) {
    void *sentinel = shim_egl_image_alloc_sentinel();
    pthread_mutex_lock(&shim_egl_image_map_lock);
    if (shim_egl_image_map_count >= SHIM_EGL_IMAGE_MAP_SIZE) {
        pthread_mutex_unlock(&shim_egl_image_map_lock);
        SHIM_LOG("[egl_image] map full (%d); returning FAKE_EGL_IMAGE",
                 SHIM_EGL_IMAGE_MAP_SIZE);
        return FAKE_EGL_IMAGE;
    }
    shim_egl_image_map[shim_egl_image_map_count++] = (struct shim_egl_image_entry){
        .egl_image = sentinel, .width = w, .height = h, .fourcc = fmt,
        .rgba_ptr = rgba_ptr, .stride = stride,
    };
    pthread_mutex_unlock(&shim_egl_image_map_lock);
    return sentinel;
}

static int shim_egl_image_lookup(void *image, int *w, int *h, unsigned *fmt,
                                 const uint8_t **rgba_ptr, int *stride) {
    pthread_mutex_lock(&shim_egl_image_map_lock);
    for (int i = 0; i < shim_egl_image_map_count; i++) {
        if (shim_egl_image_map[i].egl_image == image) {
            if (w)        *w        = shim_egl_image_map[i].width;
            if (h)        *h        = shim_egl_image_map[i].height;
            if (fmt)      *fmt      = shim_egl_image_map[i].fourcc;
            if (rgba_ptr) *rgba_ptr = shim_egl_image_map[i].rgba_ptr;
            if (stride)   *stride   = shim_egl_image_map[i].stride;
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
    int      plane0_fd = 0;
    shim_egl_image_parse_attribs(attrib_list, &w, &h, &fmt, &plane0_fd);
    if (w <= 0 || w > 8192)  w = FAKE_MODE_HDISPLAY;
    if (h <= 0 || h > 8192)  h = FAKE_MODE_VDISPLAY;

    /* If plane0_fd matches an avforward sentinel, pull the live RGBA buffer
     * pointer out of avforward's side-table and stash it in our entry;
     * glEGLImageTargetTexture2DOES below uploads it directly via
     * glTexImage2D. Pointer stays valid until the next av_frame_unref of
     * the same frame — the game runs FUN_00e3e150's eglCreateImageKHR +
     * glEGLImageTargetTexture2DOES sequence synchronously. */
    const uint8_t *rgba_ptr = NULL;
    int stride = 0;
    if (plane0_fd > 0) {
        shim_avforward_pixel_lookup(plane0_fd, &rgba_ptr, NULL, NULL, &stride);
    }

    void *img = shim_egl_image_register(w, h, fmt, rgba_ptr, stride);
    SHIM_LOG("eglCreateImageKHR(dpy=%p, target=0x%x, buf=%p) "
             "[w=%d h=%d fourcc=0x%x fd=%d %s] -> %p",
             display, target, buffer, w, h, fmt, plane0_fd,
             rgba_ptr ? "VIDEO" : "non-video", img);
    (void)ctx;
    return img;
}

unsigned eglDestroyImageKHR(void *display, void *image) {
    /* Sentinels are counter-cast pointers, not malloc'd — just remove from
     * the map. The backing GL texture's lifetime is the game's concern (we
     * never free state the game still holds via glFramebufferTexture2D). */
    pthread_mutex_lock(&shim_egl_image_map_lock);
    for (int i = 0; i < shim_egl_image_map_count; i++) {
        if (shim_egl_image_map[i].egl_image == image) {
            shim_egl_image_map[i] = shim_egl_image_map[--shim_egl_image_map_count];
            break;
        }
    }
    pthread_mutex_unlock(&shim_egl_image_map_lock);
    (void)display;
    return EGL_TRUE;
}

void glEGLImageTargetTexture2DOES(unsigned target, void *image) {
    /* Demote EXTERNAL_OES so the upcoming real_glTexImage2D succeeds (Mesa
     * rejects EXTERNAL_OES on glTexImage2D with GL_INVALID_ENUM). The paired
     * glBindTexture demotion keeps the texture object's target coherent. */
    target = shim_demote_external_oes(target);

    int w = 0, h = 0; unsigned fmt = 0;
    const uint8_t *rgba_ptr = NULL;
    int stride = 0;
    if (!shim_egl_image_lookup(image, &w, &h, &fmt, &rgba_ptr, &stride)) {
        SHIM_LOG("glEGLImageTargetTexture2DOES: unknown image %p, no-op", image);
        return;
    }

    if (shim_gl_marshal_active()) {
        /* Daemon path: ship the image's RGBA payload (or empty allocation
         * for non-video FBO storage) as TEX_IMAGE_2D against whatever 2D
         * texture the game already bound through our marshalled
         * glBindTexture. We always use TEX_IMAGE_2D rather than SUB — without
         * a sync GETINTEGERV opcode we can't query daemon-side bound id to
         * gate on size match. */
        size_t dsz = rgba_ptr ? (size_t)w * (size_t)h * 4 : 0;
        size_t plen = sizeof(struct gl_payload_tex_image_2d) + dsz;
        unsigned char *buf = (unsigned char*)malloc(plen);
        if (buf) {
            struct gl_payload_tex_image_2d *r = (struct gl_payload_tex_image_2d*)buf;
            r->target = target; r->level = 0;
            r->internal_format = 0x8058; r->width = w; r->height = h;
            r->border = 0; r->format = 0x1908; r->type = 0x1401;
            r->data_size = (uint32_t)dsz;
            if (rgba_ptr && dsz) memcpy(buf + sizeof(*r), rgba_ptr, dsz);
            shim_gl_marshal_send_async(GL_OP_GL_TEX_IMAGE_2D, buf, (uint32_t)plen);
            free(buf);
        }
        shim_frametex_record(target, w, h);
        return;
    }

    static void (*real_glTexImage2D)(unsigned, int, int, int, int, int,
                                     unsigned, unsigned, const void *) = NULL;
    static void (*real_glTexSubImage2D)(unsigned, int, int, int, int, int,
                                        unsigned, unsigned, const void *) = NULL;
    static void (*real_glGetIntegerv)(unsigned, int *) = NULL;
    if (!real_glTexImage2D)    real_glTexImage2D    = (void (*)(unsigned, int, int, int, int, int, unsigned, unsigned, const void*))
                                                       dlsym(RTLD_NEXT, "glTexImage2D");
    if (!real_glTexSubImage2D) real_glTexSubImage2D = (void (*)(unsigned, int, int, int, int, int, unsigned, unsigned, const void*))
                                                       dlsym(RTLD_NEXT, "glTexSubImage2D");
    if (!real_glGetIntegerv)   real_glGetIntegerv   = (void (*)(unsigned, int *))
                                                       dlsym(RTLD_NEXT, "glGetIntegerv");
    if (!real_glTexImage2D) return;

    /* Reuse texture storage across frames — repeated glTexImage2D forces
     * Mesa to reallocate the backing every call, which under software
     * rasterization is far slower than a glTexSubImage2D into existing
     * storage. Track per-texture {tex_id, w, h}; on a match, sub-image
     * upload; otherwise re-allocate via glTexImage2D and update tracking. */
    static struct { unsigned tex_id; int w, h; } tex_alloc[128];
    static int tex_alloc_count = 0;
    static pthread_mutex_t tex_alloc_lock = PTHREAD_MUTEX_INITIALIZER;

    int tex_id = 0;
    if (real_glGetIntegerv) real_glGetIntegerv(0x8069 /* TEXTURE_BINDING_2D */, &tex_id);

    int needs_realloc = 1;
    if (tex_id > 0) {
        pthread_mutex_lock(&tex_alloc_lock);
        for (int i = 0; i < tex_alloc_count; i++) {
            if (tex_alloc[i].tex_id == (unsigned)tex_id) {
                if (tex_alloc[i].w == w && tex_alloc[i].h == h) {
                    needs_realloc = 0;
                } else {
                    tex_alloc[i].w = w;
                    tex_alloc[i].h = h;
                }
                break;
            }
        }
        if (needs_realloc && tex_alloc_count < 128) {
            int found = 0;
            for (int i = 0; i < tex_alloc_count; i++) {
                if (tex_alloc[i].tex_id == (unsigned)tex_id) { found = 1; break; }
            }
            if (!found) {
                tex_alloc[tex_alloc_count].tex_id = (unsigned)tex_id;
                tex_alloc[tex_alloc_count].w      = w;
                tex_alloc[tex_alloc_count].h      = h;
                tex_alloc_count++;
            }
        }
        pthread_mutex_unlock(&tex_alloc_lock);
    }

    /* GL_RGBA8 = 0x8058 internal, GL_RGBA = 0x1908 format, GL_UNSIGNED_BYTE = 0x1401. */
    if (rgba_ptr) {
        if (!needs_realloc && real_glTexSubImage2D) {
            real_glTexSubImage2D(target, 0, 0, 0, w, h, 0x1908, 0x1401, rgba_ptr);
        } else {
            real_glTexImage2D(target, 0, 0x8058, w, h, 0, 0x1908, 0x1401, rgba_ptr);
        }
        shim_frametex_record(target, w, h);
        return;
    }
    /* Non-video path: storage-only allocation for FBO color attachment. */
    real_glTexImage2D(target, 0, 0x8058, w, h, 0, 0x1908, 0x1401, NULL);
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

/* Two modes, selected by SPIKE3_REAL_GL env var:
 *
 * 1. Unset (default): pure no-op stubs. Game's rendering calls all succeed
 *    with plausible values, no actual rendering happens — reaching steady
 *    state without needing a real GL stack.
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
 *    LD_LIBRARY_PATH.
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

unsigned glGetError(void) {
    if (shim_gl_marshal_active()) {
        shim_gl_marshal_send_async(GL_OP_GL_GET_ERROR, NULL, 0);
    }
    FWD_RET(unsigned, glGetError, (void), ());
    return GL_NO_ERROR;
}

const unsigned char *glGetString(unsigned name) {
    if (!shim_gl_marshal_active()) {
        FWD_RET(const unsigned char*, glGetString, (unsigned), (name));
    }
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
    if (!shim_gl_marshal_active()) {
        FWD_RET(const unsigned char*, glGetStringi, (unsigned, unsigned), (name, index));
    }
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
    if (!shim_gl_marshal_active()) {
        FWD_VOID(glGetIntegerv, (unsigned, int*), (pname, params));
    }
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
    if (!shim_gl_marshal_active()) {
        FWD_VOID(glGetFloatv, (unsigned, float*), (pname, params));
    }
    if (params) *params = 0.0f;
    (void)pname;
}
void glGetBooleanv(unsigned pname, unsigned char *params) {
    if (!shim_gl_marshal_active()) {
        FWD_VOID(glGetBooleanv, (unsigned, unsigned char*), (pname, params));
    }
    if (params) *params = GL_FALSE;
    (void)pname;
}

void glGenTextures(int n, unsigned *out) {
    if (shim_gl_marshal_active()) {
        gl_marshal_gen_ids(GL_OP_GL_GEN_TEXTURES, n, out);
        return;
    }
    FWD_VOID(glGenTextures, (int, unsigned*), (n, out));
    for (int i = 0; i < n; i++) out[i] = gl_next_tex_id++;
    GLLOG(object_create, "glGenTextures(%d) -> [%u..]", n, n ? out[0] : 0);
}
void glDeleteTextures(int n, const unsigned *ids) {
    if (shim_gl_marshal_active()) {
        gl_marshal_delete_ids(GL_OP_GL_DELETE_TEXTURES, n, ids);
        return;
    }
    FWD_VOID(glDeleteTextures, (int, const unsigned*), (n, ids));
    (void)n; (void)ids;
}
static int gl_marshal_gen_ids(uint16_t op, int n, unsigned *out) {
    if (n <= 0 || !out) return -1;
    struct gl_payload_count req = { (uint32_t)n };
    size_t reply_max = sizeof(struct gl_payload_rep_ids) + (size_t)n * 4;
    unsigned char *reply = (unsigned char*)malloc(reply_max);
    if (!reply) return -1;
    uint32_t reply_len = 0;
    int rep_op = shim_gl_marshal_send_sync(op, &req, sizeof(req),
                                            reply, (uint32_t)reply_max, &reply_len);
    int rc = -1;
    if (rep_op == GL_OP_REP_IDS && reply_len >= sizeof(struct gl_payload_rep_ids)) {
        struct gl_payload_rep_ids *r = (struct gl_payload_rep_ids*)reply;
        uint32_t got = r->count > (uint32_t)n ? (uint32_t)n : r->count;
        memcpy(out, reply + sizeof(*r), got * 4);
        for (uint32_t i = got; i < (uint32_t)n; i++) out[i] = 0;
        rc = 0;
    }
    free(reply);
    return rc;
}

static void gl_marshal_delete_ids(uint16_t op, int n, const unsigned *ids) {
    if (n <= 0 || !ids) return;
    size_t plen = 4 + (size_t)n * 4;
    unsigned char *buf = (unsigned char*)malloc(plen);
    if (!buf) return;
    uint32_t cnt = (uint32_t)n;
    memcpy(buf, &cnt, 4);
    memcpy(buf + 4, ids, (size_t)n * 4);
    shim_gl_marshal_send_async(op, buf, (uint32_t)plen);
    free(buf);
}

void glGenBuffers(int n, unsigned *out) {
    if (shim_gl_marshal_active()) {
        gl_marshal_gen_ids(GL_OP_GL_GEN_BUFFERS, n, out);
        return;
    }
    FWD_VOID(glGenBuffers, (int, unsigned*), (n, out));
    for (int i = 0; i < n; i++) out[i] = gl_next_buf_id++;
    GLLOG(object_create, "glGenBuffers(%d) -> [%u..]", n, n ? out[0] : 0);
}
void glDeleteBuffers(int n, const unsigned *ids) {
    if (shim_gl_marshal_active()) {
        gl_marshal_delete_ids(GL_OP_GL_DELETE_BUFFERS, n, ids);
        return;
    }
    FWD_VOID(glDeleteBuffers, (int, const unsigned*), (n, ids));
    (void)n; (void)ids;
}
#define SHIM_GL_DBG_HOOK(NAME) \
    do { static uint64_t _c = 0; \
         if (__builtin_expect(is_real_gl(), 0) && ++_c <= 4) \
             SHIM_LOG("[gl] " #NAME " #%lu entered", (unsigned long)_c); \
    } while (0)

void glGenFramebuffers(int n, unsigned *out) {
    SHIM_GL_DBG_HOOK(glGenFramebuffers);
    if (shim_gl_marshal_active()) {
        gl_marshal_gen_ids(GL_OP_GL_GEN_FRAMEBUFFERS, n, out);
        return;
    }
    FWD_VOID(glGenFramebuffers, (int, unsigned*), (n, out));
    for (int i = 0; i < n; i++) out[i] = gl_next_fb_id++;
    GLLOG(object_create, "glGenFramebuffers(%d) -> [%u..]", n, n ? out[0] : 0);
}
void glDeleteFramebuffers(int n, const unsigned *ids) {
    if (shim_gl_marshal_active()) {
        gl_marshal_delete_ids(GL_OP_GL_DELETE_FRAMEBUFFERS, n, ids);
        return;
    }
    FWD_VOID(glDeleteFramebuffers, (int, const unsigned*), (n, ids));
    (void)n; (void)ids;
}
void glGenRenderbuffers(int n, unsigned *out) {
    if (shim_gl_marshal_active()) {
        gl_marshal_gen_ids(GL_OP_GL_GEN_RENDERBUFFERS, n, out);
        return;
    }
    FWD_VOID(glGenRenderbuffers, (int, unsigned*), (n, out));
    for (int i = 0; i < n; i++) out[i] = gl_next_rb_id++;
    GLLOG(object_create, "glGenRenderbuffers(%d) -> [%u..]", n, n ? out[0] : 0);
}
void glDeleteRenderbuffers(int n, const unsigned *ids) {
    if (shim_gl_marshal_active()) {
        gl_marshal_delete_ids(GL_OP_GL_DELETE_RENDERBUFFERS, n, ids);
        return;
    }
    FWD_VOID(glDeleteRenderbuffers, (int, const unsigned*), (n, ids));
    (void)n; (void)ids;
}
void glGenVertexArrays(int n, unsigned *out) {
    if (shim_gl_marshal_active()) {
        gl_marshal_gen_ids(GL_OP_GL_GEN_VERTEX_ARRAYS, n, out);
        return;
    }
    FWD_VOID(glGenVertexArrays, (int, unsigned*), (n, out));
    for (int i = 0; i < n; i++) out[i] = gl_next_vao_id++;
    GLLOG(object_create, "glGenVertexArrays(%d) -> [%u..]", n, n ? out[0] : 0);
}
void glDeleteVertexArrays(int n, const unsigned *ids) {
    if (shim_gl_marshal_active()) {
        gl_marshal_delete_ids(GL_OP_GL_DELETE_VERTEX_ARRAYS, n, ids);
        return;
    }
    FWD_VOID(glDeleteVertexArrays, (int, const unsigned*), (n, ids));
    (void)n; (void)ids;
}

unsigned glCreateShader(unsigned type) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_create_shader req = { type };
        struct gl_payload_rep_value reply = {0};
        uint32_t rl = 0;
        int rep_op = shim_gl_marshal_send_sync(GL_OP_GL_CREATE_SHADER, &req, sizeof(req),
                                                &reply, sizeof(reply), &rl);
        return (rep_op == GL_OP_REP_VALUE) ? reply.value : 0;
    }
    FWD_RET(unsigned, glCreateShader, (unsigned), (type));
    unsigned id = gl_next_shader_id++;
    GLLOG(object_create, "glCreateShader(type=0x%x) -> %u", type, id);
    return id;
}
void glDeleteShader(unsigned shader) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_id_op req = { shader };
        shim_gl_marshal_send_async(GL_OP_GL_DELETE_SHADER, &req, sizeof(req));
        return;
    }
    FWD_VOID(glDeleteShader, (unsigned), (shader));
    (void)shader;
}
unsigned glCreateProgram(void) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_rep_value reply = {0};
        uint32_t rl = 0;
        int rep_op = shim_gl_marshal_send_sync(GL_OP_GL_CREATE_PROGRAM, NULL, 0,
                                                &reply, sizeof(reply), &rl);
        return (rep_op == GL_OP_REP_VALUE) ? reply.value : 0;
    }
    FWD_RET(unsigned, glCreateProgram, (void), ());
    unsigned id = gl_next_program_id++;
    GLLOG(object_create, "glCreateProgram() -> %u", id);
    return id;
}
void glDeleteProgram(unsigned program) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_id_op req = { program };
        shim_gl_marshal_send_async(GL_OP_GL_DELETE_PROGRAM, &req, sizeof(req));
        return;
    }
    FWD_VOID(glDeleteProgram, (unsigned), (program));
    (void)program;
}

void glBindTexture(unsigned target, unsigned tex) {
    target = shim_demote_external_oes(target);
    if (shim_gl_marshal_active()) {
        struct gl_payload_bind req = { target, tex };
        shim_gl_marshal_send_async(GL_OP_GL_BIND_TEXTURE, &req, sizeof(req));
        return;
    }
    FWD_VOID(glBindTexture, (unsigned, unsigned), (target, tex));
    (void)target; (void)tex;
}
void glBindBuffer(unsigned target, unsigned buf) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_bind req = { target, buf };
        shim_gl_marshal_send_async(GL_OP_GL_BIND_BUFFER, &req, sizeof(req));
        return;
    }
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
    if (shim_gl_marshal_active()) {
        struct gl_payload_bind req = { target, fb };
        shim_gl_marshal_send_async(GL_OP_GL_BIND_FRAMEBUFFER, &req, sizeof(req));
        if ((target == 0x8D40 || target == 0x8CA9) && fb != 0 && fb != shim_capture_fbo)
            __atomic_store_n(&shim_last_draw_fbo, fb, __ATOMIC_RELAXED);
        return;
    }
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
    if (shim_gl_marshal_active()) {
        struct gl_payload_bind req = { target, rb };
        shim_gl_marshal_send_async(GL_OP_GL_BIND_RENDERBUFFER, &req, sizeof(req));
        return;
    }
    FWD_VOID(glBindRenderbuffer, (unsigned, unsigned), (target, rb));
    (void)target; (void)rb;
}
void glBindVertexArray(unsigned array) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_id_op req = { array };
        shim_gl_marshal_send_async(GL_OP_GL_BIND_VERTEX_ARRAY, &req, sizeof(req));
        return;
    }
    FWD_VOID(glBindVertexArray, (unsigned), (array));
    (void)array;
}
void glActiveTexture(unsigned tex) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_active_texture req = { tex };
        shim_gl_marshal_send_async(GL_OP_GL_ACTIVE_TEXTURE, &req, sizeof(req));
        return;
    }
    FWD_VOID(glActiveTexture, (unsigned), (tex));
    (void)tex;
}

static size_t gl_pixel_size(unsigned format, unsigned type) {
    int channels = 4;
    switch (format) {
        case 0x1908: channels = 4; break; /* GL_RGBA */
        case 0x1907: channels = 3; break; /* GL_RGB */
        case 0x1906: channels = 1; break; /* GL_ALPHA */
        case 0x1909: channels = 1; break; /* GL_LUMINANCE */
        case 0x190A: channels = 2; break; /* GL_LUMINANCE_ALPHA */
        default:     channels = 4; break;
    }
    int bytes_per_channel = 1;
    switch (type) {
        case 0x1401: bytes_per_channel = 1; break; /* UNSIGNED_BYTE */
        case 0x1403: bytes_per_channel = 2; break; /* UNSIGNED_SHORT */
        case 0x8363: case 0x8033: case 0x8034: bytes_per_channel = 2; channels = 1; break; /* packed */
        case 0x1406: bytes_per_channel = 4; break; /* FLOAT */
        default:     bytes_per_channel = 1; break;
    }
    return (size_t)channels * (size_t)bytes_per_channel;
}

void glTexImage2D(unsigned target, int level, int internalfmt,
                  int w, int h, int border, unsigned format, unsigned type,
                  const void *data) {
    target = shim_demote_external_oes(target);
    if (shim_gl_marshal_active()) {
        size_t dsz = data ? (size_t)w * (size_t)h * gl_pixel_size(format, type) : 0;
        size_t plen = sizeof(struct gl_payload_tex_image_2d) + dsz;
        unsigned char *buf = (unsigned char*)malloc(plen);
        if (buf) {
            struct gl_payload_tex_image_2d *r = (struct gl_payload_tex_image_2d*)buf;
            r->target = target; r->level = level; r->internal_format = internalfmt;
            r->width = w; r->height = h; r->border = border;
            r->format = format; r->type = type; r->data_size = (uint32_t)dsz;
            if (data && dsz) memcpy(buf + sizeof(*r), data, dsz);
            shim_gl_marshal_send_async(GL_OP_GL_TEX_IMAGE_2D, buf, (uint32_t)plen);
            free(buf);
        }
        return;
    }
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
    target = shim_demote_external_oes(target);
    if (shim_gl_marshal_active()) {
        size_t dsz = data ? (size_t)w * (size_t)h * gl_pixel_size(format, type) : 0;
        size_t plen = sizeof(struct gl_payload_tex_sub_image_2d) + dsz;
        unsigned char *buf = (unsigned char*)malloc(plen);
        if (buf) {
            struct gl_payload_tex_sub_image_2d *r = (struct gl_payload_tex_sub_image_2d*)buf;
            r->target = target; r->level = level;
            r->xoffset = xoff; r->yoffset = yoff;
            r->width = w; r->height = h;
            r->format = format; r->type = type; r->data_size = (uint32_t)dsz;
            if (data && dsz) memcpy(buf + sizeof(*r), data, dsz);
            shim_gl_marshal_send_async(GL_OP_GL_TEX_SUB_IMAGE_2D, buf, (uint32_t)plen);
            free(buf);
        }
        return;
    }
    FWD_VOID(glTexSubImage2D,
             (unsigned, int, int, int, int, int, unsigned, unsigned, const void*),
             (target, level, xoff, yoff, w, h, format, type, data));
    (void)target; (void)level; (void)xoff; (void)yoff;
    (void)w; (void)h; (void)format; (void)type; (void)data;
}
void glCompressedTexImage2D(unsigned target, int level, unsigned internalfmt,
                            int w, int h, int border, int size, const void *data) {
    target = shim_demote_external_oes(target);
    if (shim_gl_marshal_active()) {
        size_t isz = (data && size > 0) ? (size_t)size : 0;
        size_t plen = sizeof(struct gl_payload_compressed_tex_image_2d) + isz;
        unsigned char *buf = (unsigned char*)malloc(plen);
        if (buf) {
            struct gl_payload_compressed_tex_image_2d *r = (struct gl_payload_compressed_tex_image_2d*)buf;
            r->target = target; r->level = level; r->internal_format = internalfmt;
            r->width = w; r->height = h; r->border = border; r->image_size = (uint32_t)isz;
            if (isz) memcpy(buf + sizeof(*r), data, isz);
            shim_gl_marshal_send_async(GL_OP_GL_COMPRESSED_TEX_IMAGE_2D, buf, (uint32_t)plen);
            free(buf);
        }
        return;
    }
    FWD_VOID(glCompressedTexImage2D,
             (unsigned, int, unsigned, int, int, int, int, const void*),
             (target, level, internalfmt, w, h, border, size, data));
    (void)target; (void)level; (void)internalfmt;
    (void)w; (void)h; (void)border; (void)size; (void)data;
}
void glCompressedTexSubImage2D(unsigned target, int level, int xoff, int yoff,
                               int w, int h, unsigned format, int size,
                               const void *data) {
    target = shim_demote_external_oes(target);
    if (shim_gl_marshal_active()) {
        size_t isz = (data && size > 0) ? (size_t)size : 0;
        size_t plen = sizeof(struct gl_payload_compressed_tex_sub_image_2d) + isz;
        unsigned char *buf = (unsigned char*)malloc(plen);
        if (buf) {
            struct gl_payload_compressed_tex_sub_image_2d *r = (struct gl_payload_compressed_tex_sub_image_2d*)buf;
            r->target = target; r->level = level;
            r->xoffset = xoff; r->yoffset = yoff;
            r->width = w; r->height = h;
            r->format = format; r->image_size = (uint32_t)isz;
            if (isz) memcpy(buf + sizeof(*r), data, isz);
            shim_gl_marshal_send_async(GL_OP_GL_COMPRESSED_TEX_SUB_IMAGE_2D, buf, (uint32_t)plen);
            free(buf);
        }
        return;
    }
    FWD_VOID(glCompressedTexSubImage2D,
             (unsigned, int, int, int, int, int, unsigned, int, const void*),
             (target, level, xoff, yoff, w, h, format, size, data));
    (void)target; (void)level; (void)xoff; (void)yoff;
    (void)w; (void)h; (void)format; (void)size; (void)data;
}
void glTexParameteri(unsigned target, unsigned pname, int param) {
    target = shim_demote_external_oes(target);
    if (shim_gl_marshal_active()) {
        struct gl_payload_tex_parameter_i req = { target, pname, param };
        shim_gl_marshal_send_async(GL_OP_GL_TEX_PARAMETER_I, &req, sizeof(req));
        return;
    }
    FWD_VOID(glTexParameteri, (unsigned, unsigned, int), (target, pname, param));
    (void)target; (void)pname; (void)param;
}
void glTexParameterf(unsigned target, unsigned pname, float param) {
    target = shim_demote_external_oes(target);
    if (shim_gl_marshal_active()) {
        struct gl_payload_tex_param_f req = { target, pname, param };
        shim_gl_marshal_send_async(GL_OP_GL_TEX_PARAMETER_F, &req, sizeof(req));
        return;
    }
    FWD_VOID(glTexParameterf, (unsigned, unsigned, float), (target, pname, param));
    (void)target; (void)pname; (void)param;
}
void glPixelStorei(unsigned pname, int param) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_pixel_store_i req = { pname, param };
        shim_gl_marshal_send_async(GL_OP_GL_PIXEL_STORE_I, &req, sizeof(req));
        return;
    }
    FWD_VOID(glPixelStorei, (unsigned, int), (pname, param));
    (void)pname; (void)param;
}
void glGenerateMipmap(unsigned target) {
    target = shim_demote_external_oes(target);
    if (shim_gl_marshal_active()) {
        struct gl_payload_1u req = { target };
        shim_gl_marshal_send_async(GL_OP_GL_GENERATE_MIPMAP, &req, sizeof(req));
        return;
    }
    FWD_VOID(glGenerateMipmap, (unsigned), (target));
    (void)target;
}

void glBufferData(unsigned target, intptr_t size, const void *data, unsigned usage) {
    if (shim_gl_marshal_active()) {
        size_t plen = sizeof(struct gl_payload_buffer_data) + (data ? (size_t)size : 0);
        unsigned char *buf = (unsigned char*)malloc(plen);
        if (buf) {
            struct gl_payload_buffer_data *r = (struct gl_payload_buffer_data*)buf;
            r->target = target; r->size = (uint32_t)size; r->usage = usage;
            if (data && size > 0) memcpy(buf + sizeof(*r), data, (size_t)size);
            shim_gl_marshal_send_async(GL_OP_GL_BUFFER_DATA, buf, (uint32_t)plen);
            free(buf);
        }
        return;
    }
    FWD_VOID(glBufferData, (unsigned, intptr_t, const void*, unsigned),
             (target, size, data, usage));
    (void)target; (void)size; (void)data; (void)usage;
}
void glBufferSubData(unsigned target, intptr_t offset, intptr_t size, const void *data) {
    if (shim_gl_marshal_active()) {
        size_t dsz = (data && size > 0) ? (size_t)size : 0;
        size_t plen = sizeof(struct gl_payload_buffer_sub_data) + dsz;
        unsigned char *buf = (unsigned char*)malloc(plen);
        if (buf) {
            struct gl_payload_buffer_sub_data *r = (struct gl_payload_buffer_sub_data*)buf;
            r->target = target; r->offset = (int64_t)offset; r->size = (uint32_t)dsz;
            if (dsz) memcpy(buf + sizeof(*r), data, dsz);
            shim_gl_marshal_send_async(GL_OP_GL_BUFFER_SUB_DATA, buf, (uint32_t)plen);
            free(buf);
        }
        return;
    }
    FWD_VOID(glBufferSubData, (unsigned, intptr_t, intptr_t, const void*),
             (target, offset, size, data));
    (void)target; (void)offset; (void)size; (void)data;
}
/* Per-target buffer-map shadow table (max 8 simultaneous mappings; GL spec
 * permits one mapping per target).
 *
 * Daemon mode requirement: glMapBufferRange must return writable host-process
 * memory the game can deref. The daemon's GL state is in another process —
 * its mapped pointer is unreachable here. We allocate scratch sized to the
 * mapping, hand it to the game, and on glUnmapBuffer ship the bytes via
 * glBufferSubData(target, offset, len, scratch). MAP_READ_BIT is best-effort:
 * we don't fetch from daemon first, so a read-only map sees zeros. The game's
 * actual usage (glMap+memcpy+glUnmap) is the WRITE pattern so this is fine. */
struct shim_gl_map_entry {
    unsigned target;
    void    *ptr;
    intptr_t offset;
    intptr_t len;
    unsigned access;
};
static struct shim_gl_map_entry shim_gl_map_table[8];
static pthread_mutex_t shim_gl_map_lock = PTHREAD_MUTEX_INITIALIZER;

void *glMapBufferRange(unsigned target, intptr_t offset, intptr_t len, unsigned access) {
    if (shim_gl_marshal_active()) {
        if (len <= 0) return NULL;
        void *ptr = malloc((size_t)len);
        if (!ptr) return NULL;
        memset(ptr, 0, (size_t)len);
        pthread_mutex_lock(&shim_gl_map_lock);
        int slot = -1;
        for (int i = 0; i < 8; i++) {
            if (!shim_gl_map_table[i].ptr) { slot = i; break; }
        }
        if (slot >= 0) {
            shim_gl_map_table[slot].target = target;
            shim_gl_map_table[slot].ptr    = ptr;
            shim_gl_map_table[slot].offset = offset;
            shim_gl_map_table[slot].len    = len;
            shim_gl_map_table[slot].access = access;
        }
        pthread_mutex_unlock(&shim_gl_map_lock);
        if (slot < 0) {
            SHIM_LOG("[gl_map] no free slot for map(target=0x%x len=%ld); leaking", target, (long)len);
            free(ptr);
            return NULL;
        }
        return ptr;
    }
    FWD_RET(void*, glMapBufferRange, (unsigned, intptr_t, intptr_t, unsigned),
            (target, offset, len, access));
    (void)target; (void)offset; (void)len; (void)access; return NULL;
}
unsigned char glUnmapBuffer(unsigned target) {
    if (shim_gl_marshal_active()) {
        void *ptr = NULL;
        intptr_t offset = 0, len = 0;
        unsigned access = 0;
        pthread_mutex_lock(&shim_gl_map_lock);
        for (int i = 0; i < 8; i++) {
            if (shim_gl_map_table[i].target == target && shim_gl_map_table[i].ptr) {
                ptr     = shim_gl_map_table[i].ptr;
                offset  = shim_gl_map_table[i].offset;
                len     = shim_gl_map_table[i].len;
                access  = shim_gl_map_table[i].access;
                shim_gl_map_table[i].ptr = NULL;
                break;
            }
        }
        pthread_mutex_unlock(&shim_gl_map_lock);
        if (!ptr) return GL_TRUE;
        /* GL_MAP_WRITE_BIT = 0x2; ship contents back if the game might have
         * written. UNSYNCHRONIZED/INVALIDATE bits don't change correctness here. */
        if ((access & 0x2) && len > 0) {
            size_t plen = sizeof(struct gl_payload_buffer_sub_data) + (size_t)len;
            unsigned char *buf = (unsigned char*)malloc(plen);
            if (buf) {
                struct gl_payload_buffer_sub_data *r = (struct gl_payload_buffer_sub_data*)buf;
                r->target = target; r->offset = (int64_t)offset; r->size = (uint32_t)len;
                memcpy(buf + sizeof(*r), ptr, (size_t)len);
                shim_gl_marshal_send_async(GL_OP_GL_BUFFER_SUB_DATA, buf, (uint32_t)plen);
                free(buf);
            }
        }
        free(ptr);
        return GL_TRUE;
    }
    FWD_RET(unsigned char, glUnmapBuffer, (unsigned), (target));
    (void)target; return GL_TRUE;
}

void glShaderSource(unsigned shader, int count, const char *const *string, const int *length) {
    int active = shim_gl_marshal_active();
    if (!active && !is_real_gl()) {
        (void)shader; (void)count; (void)string; (void)length;
        return;
    }
    static void (*real_fn)(unsigned, int, const char *const *, const int *) = NULL;
    if (!active && !real_fn)
        real_fn = (void(*)(unsigned, int, const char *const *, const int*))
                  dlsym(RTLD_NEXT, "glShaderSource");
    if (count <= 0 || !string) {
        if (!active && real_fn) real_fn(shader, count, string, length);
        return;
    }

    /* Concatenate input strings into a single buffer so we can scan for
     * samplerExternalOES and inject a sampler2D #define. */
    size_t total = 1;  /* +1 for null terminator */
    for (int i = 0; i < count; i++) {
        if (!string[i]) continue;
        size_t li = (length && length[i] >= 0) ? (size_t)length[i] : strlen(string[i]);
        total += li;
    }
    char *buf = (char *)malloc(total);
    if (!buf) { real_fn(shader, count, string, length); return; }
    size_t off = 0;
    for (int i = 0; i < count; i++) {
        if (!string[i]) continue;
        size_t li = (length && length[i] >= 0) ? (size_t)length[i] : strlen(string[i]);
        memcpy(buf + off, string[i], li);
        off += li;
    }
    buf[off] = '\0';

    /* Stern's video fragment shader uses `samplerExternalOES textureSampler`
     * unconditionally, expecting the OES extension's YUV-aware sampler.
     * Under Mesa swrast llvmpipe a samplerExternalOES sampling a 2D-bound
     * texture returns zero; the OES extension is supported but only for
     * EXTERNAL_OES-typed textures (which require real DMA-BUF import).
     *
     * Inject `#define samplerExternalOES sampler2D` right after the relevant
     * #extension line so the GLSL preprocessor textually substitutes the
     * type to sampler2D. The bound texture (demoted to GL_TEXTURE_2D in
     * glBindTexture) then matches the sampler type and our RGBA pixels reach
     * the fragment shader. The game's own Godot shaders use this same trick
     * already — we just extend it to the Stern Radium video shaders that
     * lack the conditional fallback. */
    if (strstr(buf, "samplerExternalOES")) {
        const char *inject = "#define samplerExternalOES sampler2D\n";
        size_t inject_len = strlen(inject);

        char *insert_at = buf;
        char *ext = strstr(buf, "#extension GL_OES_EGL_image_external");
        char *ver = strstr(buf, "#version");
        if (ext) {
            char *eol = strchr(ext, '\n');
            if (eol) insert_at = eol + 1;
        } else if (ver) {
            char *eol = strchr(ver, '\n');
            if (eol) insert_at = eol + 1;
        }

        size_t prefix_len = (size_t)(insert_at - buf);
        size_t tail_len = off - prefix_len;
        char *new_buf = (char *)malloc(off + inject_len + 1);
        if (new_buf) {
            memcpy(new_buf, buf, prefix_len);
            memcpy(new_buf + prefix_len, inject, inject_len);
            memcpy(new_buf + prefix_len + inject_len, insert_at, tail_len);
            new_buf[prefix_len + inject_len + tail_len] = '\0';
            free(buf);
            buf = new_buf;
            static int log_count = 0;
            if (log_count < 4) {
                SHIM_LOG("[shader_patch] injected sampler2D define for shader=%u (rewrite #%d)",
                         shader, ++log_count);
            }
        }
    }

    if (shim_gl_marshal_active()) {
        size_t slen = strlen(buf);
        size_t plen = sizeof(struct gl_payload_shader_source) + slen;
        unsigned char *msg = (unsigned char*)malloc(plen);
        if (msg) {
            struct gl_payload_shader_source *r = (struct gl_payload_shader_source*)msg;
            r->shader_id = shader;
            r->source_len = (uint32_t)slen;
            memcpy(msg + sizeof(*r), buf, slen);
            shim_gl_marshal_send_async(GL_OP_GL_SHADER_SOURCE, msg, (uint32_t)plen);
            free(msg);
        }
        free(buf);
        return;
    }

    const char *src = buf;
    real_fn(shader, 1, &src, NULL);
    free(buf);
}
void glCompileShader(unsigned shader) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_id_op req = { shader };
        shim_gl_marshal_send_async(GL_OP_GL_COMPILE_SHADER, &req, sizeof(req));
        return;
    }
    FWD_VOID(glCompileShader, (unsigned), (shader));
    (void)shader;
}
void glAttachShader(unsigned program, unsigned shader) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_two_id req = { program, shader };
        shim_gl_marshal_send_async(GL_OP_GL_ATTACH_SHADER, &req, sizeof(req));
        return;
    }
    FWD_VOID(glAttachShader, (unsigned, unsigned), (program, shader));
    (void)program; (void)shader;
}
void glDetachShader(unsigned program, unsigned shader) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_two_id req = { program, shader };
        shim_gl_marshal_send_async(GL_OP_GL_DETACH_SHADER, &req, sizeof(req));
        return;
    }
    FWD_VOID(glDetachShader, (unsigned, unsigned), (program, shader));
    (void)program; (void)shader;
}
void glLinkProgram(unsigned program) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_id_op req = { program };
        shim_gl_marshal_send_async(GL_OP_GL_LINK_PROGRAM, &req, sizeof(req));
        return;
    }
    FWD_VOID(glLinkProgram, (unsigned), (program));
    (void)program;
}
void glValidateProgram(unsigned program) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_id_op req = { program };
        shim_gl_marshal_send_async(GL_OP_GL_VALIDATE_PROGRAM, &req, sizeof(req));
        return;
    }
    FWD_VOID(glValidateProgram, (unsigned), (program));
    (void)program;
}
void glUseProgram(unsigned program) {
    SHIM_GL_DBG_HOOK(glUseProgram);
    if (shim_gl_marshal_active()) {
        struct gl_payload_id_op req = { program };
        shim_gl_marshal_send_async(GL_OP_GL_USE_PROGRAM, &req, sizeof(req));
        return;
    }
    FWD_VOID(glUseProgram, (unsigned), (program));
    (void)program;
}

void glGetShaderiv(unsigned shader, unsigned pname, int *params) {
    if (shim_gl_marshal_active() && params) {
        struct gl_payload_get_iv req = { shader, pname };
        struct gl_payload_rep_value reply = {0};
        uint32_t rl = 0;
        int rep_op = shim_gl_marshal_send_sync(GL_OP_GL_GET_SHADER_IV, &req, sizeof(req),
                                                &reply, sizeof(reply), &rl);
        *params = (rep_op == GL_OP_REP_VALUE) ? (int)reply.value : 0;
        return;
    }
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
    if (shim_gl_marshal_active() && params) {
        struct gl_payload_get_iv req = { program, pname };
        struct gl_payload_rep_value reply = {0};
        uint32_t rl = 0;
        int rep_op = shim_gl_marshal_send_sync(GL_OP_GL_GET_PROGRAM_IV, &req, sizeof(req),
                                                &reply, sizeof(reply), &rl);
        *params = (rep_op == GL_OP_REP_VALUE) ? (int)reply.value : 0;
        return;
    }
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
    if (!shim_gl_marshal_active()) {
        FWD_VOID(glGetShaderInfoLog, (unsigned, int, int*, char*),
                 (shader, max, len, log));
    }
    (void)shader; (void)max;
    if (len) *len = 0;
    if (log && max > 0) log[0] = 0;
}
void glGetProgramInfoLog(unsigned program, int max, int *len, char *log) {
    if (!shim_gl_marshal_active()) {
        FWD_VOID(glGetProgramInfoLog, (unsigned, int, int*, char*),
                 (program, max, len, log));
    }
    (void)program; (void)max;
    if (len) *len = 0;
    if (log && max > 0) log[0] = 0;
}

static int gl_marshal_get_location(uint16_t op, unsigned program, const char *name) {
    if (!name) return -1;
    size_t nlen = strlen(name);
    size_t plen = sizeof(struct gl_payload_get_location) + nlen;
    unsigned char *buf = (unsigned char*)malloc(plen);
    if (!buf) return -1;
    struct gl_payload_get_location *r = (struct gl_payload_get_location*)buf;
    r->program = program;
    r->name_len = (uint32_t)nlen;
    memcpy(buf + sizeof(*r), name, nlen);
    struct gl_payload_rep_loc reply = { -1 };
    uint32_t rl = 0;
    int rep_op = shim_gl_marshal_send_sync(op, buf, (uint32_t)plen,
                                            &reply, sizeof(reply), &rl);
    free(buf);
    return (rep_op == GL_OP_REP_LOC) ? reply.location : -1;
}

int glGetUniformLocation(unsigned program, const char *name) {
    if (shim_gl_marshal_active())
        return gl_marshal_get_location(GL_OP_GL_GET_UNIFORM_LOCATION, program, name);
    FWD_RET(int, glGetUniformLocation, (unsigned, const char*), (program, name));
    (void)program; (void)name; return 0;
}
int glGetAttribLocation(unsigned program, const char *name) {
    if (shim_gl_marshal_active())
        return gl_marshal_get_location(GL_OP_GL_GET_ATTRIB_LOCATION, program, name);
    FWD_RET(int, glGetAttribLocation, (unsigned, const char*), (program, name));
    (void)program; (void)name; return 0;
}
void glBindAttribLocation(unsigned program, unsigned index, const char *name) {
    if (shim_gl_marshal_active() && name) {
        size_t nlen = strlen(name);
        size_t plen = sizeof(struct gl_payload_bind_attrib_loc) + nlen;
        unsigned char *buf = (unsigned char*)malloc(plen);
        if (buf) {
            struct gl_payload_bind_attrib_loc *r = (struct gl_payload_bind_attrib_loc*)buf;
            r->program = program; r->index = index; r->name_len = (uint32_t)nlen;
            memcpy(buf + sizeof(*r), name, nlen);
            shim_gl_marshal_send_async(GL_OP_GL_BIND_ATTRIB_LOC, buf, (uint32_t)plen);
            free(buf);
        }
        return;
    }
    FWD_VOID(glBindAttribLocation, (unsigned, unsigned, const char*),
             (program, index, name));
    (void)program; (void)index; (void)name;
}

/* Uniforms — all no-op in stub mode, forwarded in real mode. */
void glUniform1i(int loc, int v0) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_uniform_1i req = { loc, v0 };
        shim_gl_marshal_send_async(GL_OP_GL_UNIFORM_1I, &req, sizeof(req));
        return;
    }
    FWD_VOID(glUniform1i, (int, int), (loc, v0));
    (void)loc; (void)v0;
}
void glUniform2i(int loc, int v0, int v1) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_uniform_2i req = { loc, v0, v1 };
        shim_gl_marshal_send_async(GL_OP_GL_UNIFORM_2I, &req, sizeof(req));
        return;
    }
    FWD_VOID(glUniform2i, (int, int, int), (loc, v0, v1));
    (void)loc; (void)v0; (void)v1;
}
void glUniform3i(int loc, int v0, int v1, int v2) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_uniform_3i req = { loc, v0, v1, v2 };
        shim_gl_marshal_send_async(GL_OP_GL_UNIFORM_3I, &req, sizeof(req));
        return;
    }
    FWD_VOID(glUniform3i, (int, int, int, int), (loc, v0, v1, v2));
    (void)loc; (void)v0; (void)v1; (void)v2;
}
void glUniform4i(int loc, int v0, int v1, int v2, int v3) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_uniform_4i req = { loc, v0, v1, v2, v3 };
        shim_gl_marshal_send_async(GL_OP_GL_UNIFORM_4I, &req, sizeof(req));
        return;
    }
    FWD_VOID(glUniform4i, (int, int, int, int, int), (loc, v0, v1, v2, v3));
    (void)loc; (void)v0; (void)v1; (void)v2; (void)v3;
}
void glUniform1f(int loc, float v0) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_uniform_1f req = { loc, v0 };
        shim_gl_marshal_send_async(GL_OP_GL_UNIFORM_1F, &req, sizeof(req));
        return;
    }
    FWD_VOID(glUniform1f, (int, float), (loc, v0));
    (void)loc; (void)v0;
}
void glUniform2f(int loc, float v0, float v1) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_uniform_2f req = { loc, v0, v1 };
        shim_gl_marshal_send_async(GL_OP_GL_UNIFORM_2F, &req, sizeof(req));
        return;
    }
    FWD_VOID(glUniform2f, (int, float, float), (loc, v0, v1));
    (void)loc; (void)v0; (void)v1;
}
void glUniform3f(int loc, float v0, float v1, float v2) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_uniform_3f req = { loc, v0, v1, v2 };
        shim_gl_marshal_send_async(GL_OP_GL_UNIFORM_3F, &req, sizeof(req));
        return;
    }
    FWD_VOID(glUniform3f, (int, float, float, float), (loc, v0, v1, v2));
    (void)loc; (void)v0; (void)v1; (void)v2;
}
void glUniform4f(int loc, float v0, float v1, float v2, float v3) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_uniform_4f req = { loc, v0, v1, v2, v3 };
        shim_gl_marshal_send_async(GL_OP_GL_UNIFORM_4F, &req, sizeof(req));
        return;
    }
    FWD_VOID(glUniform4f, (int, float, float, float, float), (loc, v0, v1, v2, v3));
    (void)loc; (void)v0; (void)v1; (void)v2; (void)v3;
}
/* Helper: marshal a uniform-array op (1iv/1fv/2fv/3fv/4fv) with header + data. */
static void gl_marshal_uniform_xv(uint16_t op, int loc, int count,
                                   const void *data, size_t bytes_per_elem) {
    if (count <= 0 || !data) return;
    size_t data_bytes = (size_t)count * bytes_per_elem;
    size_t plen = sizeof(struct gl_payload_uniform_xfv) + data_bytes;
    unsigned char *buf = (unsigned char*)malloc(plen);
    if (!buf) return;
    struct gl_payload_uniform_xfv *r = (struct gl_payload_uniform_xfv*)buf;
    r->location = loc; r->count = count;
    memcpy(buf + sizeof(*r), data, data_bytes);
    shim_gl_marshal_send_async(op, buf, (uint32_t)plen);
    free(buf);
}
void glUniform1iv(int loc, int count, const int *v) {
    if (shim_gl_marshal_active()) {
        gl_marshal_uniform_xv(GL_OP_GL_UNIFORM_1IV, loc, count, v, 1 * sizeof(int));
        return;
    }
    FWD_VOID(glUniform1iv, (int, int, const int*), (loc, count, v));
    (void)loc; (void)count; (void)v;
}
void glUniform1fv(int loc, int count, const float *v) {
    if (shim_gl_marshal_active()) {
        gl_marshal_uniform_xv(GL_OP_GL_UNIFORM_1FV, loc, count, v, 1 * sizeof(float));
        return;
    }
    FWD_VOID(glUniform1fv, (int, int, const float*), (loc, count, v));
    (void)loc; (void)count; (void)v;
}
void glUniform2fv(int loc, int count, const float *v) {
    if (shim_gl_marshal_active()) {
        gl_marshal_uniform_xv(GL_OP_GL_UNIFORM_2FV, loc, count, v, 2 * sizeof(float));
        return;
    }
    FWD_VOID(glUniform2fv, (int, int, const float*), (loc, count, v));
    (void)loc; (void)count; (void)v;
}
void glUniform3fv(int loc, int count, const float *v) {
    if (shim_gl_marshal_active()) {
        gl_marshal_uniform_xv(GL_OP_GL_UNIFORM_3FV, loc, count, v, 3 * sizeof(float));
        return;
    }
    FWD_VOID(glUniform3fv, (int, int, const float*), (loc, count, v));
    (void)loc; (void)count; (void)v;
}
void glUniform4fv(int loc, int count, const float *v) {
    if (shim_gl_marshal_active()) {
        gl_marshal_uniform_xv(GL_OP_GL_UNIFORM_4FV, loc, count, v, 4 * sizeof(float));
        return;
    }
    FWD_VOID(glUniform4fv, (int, int, const float*), (loc, count, v));
    (void)loc; (void)count; (void)v;
}
void glUniformMatrix3fv(int loc, int count, unsigned char trans, const float *v) {
    if (shim_gl_marshal_active() && count > 0 && v) {
        size_t data_bytes = (size_t)count * 9 * sizeof(float);
        size_t plen = sizeof(struct gl_payload_uniform_matrix_3fv) + data_bytes;
        unsigned char *buf = (unsigned char*)malloc(plen);
        if (buf) {
            struct gl_payload_uniform_matrix_3fv *r = (struct gl_payload_uniform_matrix_3fv*)buf;
            r->location = loc; r->count = count; r->transpose = trans;
            memcpy(buf + sizeof(*r), v, data_bytes);
            shim_gl_marshal_send_async(GL_OP_GL_UNIFORM_MATRIX_3FV, buf, (uint32_t)plen);
            free(buf);
        }
        return;
    }
    FWD_VOID(glUniformMatrix3fv, (int, int, unsigned char, const float*),
             (loc, count, trans, v));
    (void)loc; (void)count; (void)trans; (void)v;
}
void glUniformMatrix4fv(int loc, int count, unsigned char trans, const float *v) {
    if (shim_gl_marshal_active() && count > 0 && v) {
        size_t data_bytes = (size_t)count * 16 * sizeof(float);
        size_t plen = sizeof(struct gl_payload_uniform_matrix4fv) + data_bytes;
        unsigned char *buf = (unsigned char*)malloc(plen);
        if (buf) {
            struct gl_payload_uniform_matrix4fv *r = (struct gl_payload_uniform_matrix4fv*)buf;
            r->location = loc; r->count = count; r->transpose = trans;
            memcpy(buf + sizeof(*r), v, data_bytes);
            shim_gl_marshal_send_async(GL_OP_GL_UNIFORM_MATRIX4FV, buf, (uint32_t)plen);
            free(buf);
        }
        return;
    }
    FWD_VOID(glUniformMatrix4fv, (int, int, unsigned char, const float*),
             (loc, count, trans, v));
    (void)loc; (void)count; (void)trans; (void)v;
}

void glVertexAttribPointer(unsigned idx, int size, unsigned type, unsigned char norm,
                           int stride, const void *ptr) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_vap req = { idx, size, type, norm, {0,0,0}, stride, (uint64_t)(uintptr_t)ptr };
        shim_gl_marshal_send_async(GL_OP_GL_VERTEX_ATTRIB_PTR, &req, sizeof(req));
        return;
    }
    FWD_VOID(glVertexAttribPointer,
             (unsigned, int, unsigned, unsigned char, int, const void*),
             (idx, size, type, norm, stride, ptr));
    (void)idx; (void)size; (void)type; (void)norm; (void)stride; (void)ptr;
}
void glEnableVertexAttribArray(unsigned idx) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_id_op req = { idx };
        shim_gl_marshal_send_async(GL_OP_GL_ENABLE_VERTEX_ATTRIB, &req, sizeof(req));
        return;
    }
    FWD_VOID(glEnableVertexAttribArray, (unsigned), (idx));
    (void)idx;
}
void glDisableVertexAttribArray(unsigned idx) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_1u req = { idx };
        shim_gl_marshal_send_async(GL_OP_GL_DISABLE_VERTEX_ATTRIB, &req, sizeof(req));
        return;
    }
    FWD_VOID(glDisableVertexAttribArray, (unsigned), (idx));
    (void)idx;
}

void glFramebufferTexture2D(unsigned target, unsigned att, unsigned ttarget,
                            unsigned tex, int level) {
    if (__builtin_expect(is_real_gl(), 0)) {
        static uint64_t c = 0;
        if (++c <= 6) SHIM_LOG("[gl] glFramebufferTexture2D #%lu target=0x%x att=0x%x ttarget=0x%x tex=%u",
                               (unsigned long)c, target, att, ttarget, tex);
        if (ttarget == 0x8D65) ttarget = 0x0DE1;  /* EXTERNAL_OES → TEXTURE_2D */
    }
    if (shim_gl_marshal_active()) {
        if (ttarget == 0x8D65) ttarget = 0x0DE1;
        struct gl_payload_framebuffer_tex_2d req = { target, att, ttarget, tex, level };
        shim_gl_marshal_send_async(GL_OP_GL_FRAMEBUFFER_TEX_2D, &req, sizeof(req));
        return;
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
    if (shim_gl_marshal_active()) {
        struct gl_payload_framebuffer_rb req = { target, att, rbtarget, rb };
        shim_gl_marshal_send_async(GL_OP_GL_FRAMEBUFFER_RB, &req, sizeof(req));
        return;
    }
    FWD_VOID(glFramebufferRenderbuffer, (unsigned, unsigned, unsigned, unsigned),
             (target, att, rbtarget, rb));
    (void)target; (void)att; (void)rbtarget; (void)rb;
}
void glRenderbufferStorage(unsigned target, unsigned internalfmt, int w, int h) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_renderbuffer_storage req = { target, internalfmt, w, h };
        shim_gl_marshal_send_async(GL_OP_GL_RENDERBUFFER_STORAGE, &req, sizeof(req));
        return;
    }
    FWD_VOID(glRenderbufferStorage, (unsigned, unsigned, int, int),
             (target, internalfmt, w, h));
    (void)target; (void)internalfmt; (void)w; (void)h;
}
unsigned glCheckFramebufferStatus(unsigned target) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_enable req = { target };
        struct gl_payload_rep_value reply = {0};
        uint32_t rl = 0;
        int rep = shim_gl_marshal_send_sync(GL_OP_GL_CHECK_FRAMEBUFFER, &req, sizeof(req),
                                            &reply, sizeof(reply), &rl);
        return (rep == GL_OP_REP_VALUE) ? reply.value : GL_FRAMEBUFFER_COMPLETE;
    }
    FWD_RET(unsigned, glCheckFramebufferStatus, (unsigned), (target));
    (void)target; return GL_FRAMEBUFFER_COMPLETE;
}

/* SCISSOR_TEST tracker: log every transition so we can correlate viewport
 * clipping with what's on screen (PROTOCOL §10 — Service Menu appears
 * clipped to 800x480 because the boot-splash Radium scene set scissor
 * there and never reset it). */
#ifndef GL_SCISSOR_TEST
#define GL_SCISSOR_TEST 0x0C11
#endif
static int shim_scissor_test_state = -1;  /* -1 = unknown, 0 = off, 1 = on */

void glEnable(unsigned cap) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_enable req = { cap };
        shim_gl_marshal_send_async(GL_OP_GL_ENABLE, &req, sizeof(req));
        return;
    }
    FWD_VOID(glEnable, (unsigned), (cap));
    GLLOG(state, "glEnable(0x%x)", cap);
    if (cap == GL_SCISSOR_TEST && shim_scissor_test_state != 1) {
        SHIM_LOG("[scissor_test] ENABLED (0x%x)", cap);
        shim_scissor_test_state = 1;
    }
}
void glDisable(unsigned cap) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_enable req = { cap };
        shim_gl_marshal_send_async(GL_OP_GL_DISABLE, &req, sizeof(req));
        return;
    }
    FWD_VOID(glDisable, (unsigned), (cap));
    GLLOG(state, "glDisable(0x%x)", cap);
    if (cap == GL_SCISSOR_TEST && shim_scissor_test_state != 0) {
        SHIM_LOG("[scissor_test] DISABLED (0x%x)", cap);
        shim_scissor_test_state = 0;
    }
}
unsigned char glIsEnabled(unsigned cap) {
    FWD_RET(unsigned char, glIsEnabled, (unsigned), (cap));
    (void)cap; return GL_FALSE;
}
void glBlendFunc(unsigned src, unsigned dst) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_blend_func req = { src, dst };
        shim_gl_marshal_send_async(GL_OP_GL_BLEND_FUNC, &req, sizeof(req));
        return;
    }
    FWD_VOID(glBlendFunc, (unsigned, unsigned), (src, dst));
    (void)src; (void)dst;
}
void glBlendFuncSeparate(unsigned srcRGB, unsigned dstRGB, unsigned srcA, unsigned dstA) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_blend_func_separate req = { srcRGB, dstRGB, srcA, dstA };
        shim_gl_marshal_send_async(GL_OP_GL_BLEND_FUNC_SEPARATE, &req, sizeof(req));
        return;
    }
    FWD_VOID(glBlendFuncSeparate, (unsigned, unsigned, unsigned, unsigned),
             (srcRGB, dstRGB, srcA, dstA));
    (void)srcRGB; (void)dstRGB; (void)srcA; (void)dstA;
}
void glBlendEquation(unsigned mode) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_1u req = { mode };
        shim_gl_marshal_send_async(GL_OP_GL_BLEND_EQUATION, &req, sizeof(req));
        return;
    }
    FWD_VOID(glBlendEquation, (unsigned), (mode));
    (void)mode;
}
void glBlendEquationSeparate(unsigned rgb, unsigned a) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_blend_eq_separate req = { rgb, a };
        shim_gl_marshal_send_async(GL_OP_GL_BLEND_EQUATION_SEPARATE, &req, sizeof(req));
        return;
    }
    FWD_VOID(glBlendEquationSeparate, (unsigned, unsigned), (rgb, a));
    (void)rgb; (void)a;
}
void glBlendColor(float r, float g, float b, float a) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_4f req = { r, g, b, a };
        shim_gl_marshal_send_async(GL_OP_GL_BLEND_COLOR, &req, sizeof(req));
        return;
    }
    FWD_VOID(glBlendColor, (float, float, float, float), (r, g, b, a));
    (void)r; (void)g; (void)b; (void)a;
}
void glViewport(int x, int y, int w, int h) {
    static int last_x = -1, last_y = -1, last_w = -1, last_h = -1;
    if (x != last_x || y != last_y || w != last_w || h != last_h) {
        SHIM_LOG("[viewport] glViewport(%d, %d, %d, %d)", x, y, w, h);
        last_x = x; last_y = y; last_w = w; last_h = h;
    }
    if (shim_gl_marshal_active()) {
        struct gl_payload_viewport pl = { x, y, w, h };
        shim_gl_marshal_send_async(GL_OP_GL_VIEWPORT, &pl, sizeof(pl));
        return;
    }
    FWD_VOID(glViewport, (int, int, int, int), (x, y, w, h));
}
void glScissor(int x, int y, int w, int h) {
    static int last_x = -1, last_y = -1, last_w = -1, last_h = -1;
    if (x != last_x || y != last_y || w != last_w || h != last_h) {
        SHIM_LOG("[scissor] glScissor(%d, %d, %d, %d)", x, y, w, h);
        last_x = x; last_y = y; last_w = w; last_h = h;
    }
    if (shim_gl_marshal_active()) {
        struct gl_payload_rect req = { x, y, w, h };
        shim_gl_marshal_send_async(GL_OP_GL_SCISSOR, &req, sizeof(req));
        return;
    }
    FWD_VOID(glScissor, (int, int, int, int), (x, y, w, h));
}
void glDepthFunc(unsigned func) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_depth_func req = { func };
        shim_gl_marshal_send_async(GL_OP_GL_DEPTH_FUNC, &req, sizeof(req));
        return;
    }
    FWD_VOID(glDepthFunc, (unsigned), (func));
    (void)func;
}
void glDepthMask(unsigned char mask) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_byte_param req = { mask, {0,0,0} };
        shim_gl_marshal_send_async(GL_OP_GL_DEPTH_MASK, &req, sizeof(req));
        return;
    }
    FWD_VOID(glDepthMask, (unsigned char), (mask));
    (void)mask;
}
void glDepthRangef(float n, float f) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_2f req = { n, f };
        shim_gl_marshal_send_async(GL_OP_GL_DEPTH_RANGEF, &req, sizeof(req));
        return;
    }
    FWD_VOID(glDepthRangef, (float, float), (n, f));
    (void)n; (void)f;
}
void glCullFace(unsigned mode) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_enable req = { mode };
        shim_gl_marshal_send_async(GL_OP_GL_CULL_FACE, &req, sizeof(req));
        return;
    }
    FWD_VOID(glCullFace, (unsigned), (mode));
    (void)mode;
}
void glFrontFace(unsigned mode) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_1u req = { mode };
        shim_gl_marshal_send_async(GL_OP_GL_FRONT_FACE, &req, sizeof(req));
        return;
    }
    FWD_VOID(glFrontFace, (unsigned), (mode));
    (void)mode;
}
void glStencilFunc(unsigned func, int ref, unsigned mask) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_3u req = { func, (uint32_t)ref, mask };
        shim_gl_marshal_send_async(GL_OP_GL_STENCIL_FUNC, &req, sizeof(req));
        return;
    }
    FWD_VOID(glStencilFunc, (unsigned, int, unsigned), (func, ref, mask));
    (void)func; (void)ref; (void)mask;
}
void glStencilMask(unsigned mask) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_1u req = { mask };
        shim_gl_marshal_send_async(GL_OP_GL_STENCIL_MASK, &req, sizeof(req));
        return;
    }
    FWD_VOID(glStencilMask, (unsigned), (mask));
    (void)mask;
}
void glStencilOp(unsigned f, unsigned zf, unsigned zp) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_3u req = { f, zf, zp };
        shim_gl_marshal_send_async(GL_OP_GL_STENCIL_OP, &req, sizeof(req));
        return;
    }
    FWD_VOID(glStencilOp, (unsigned, unsigned, unsigned), (f, zf, zp));
    (void)f; (void)zf; (void)zp;
}
void glColorMask(unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_color_mask req = { r, g, b, a };
        shim_gl_marshal_send_async(GL_OP_GL_COLOR_MASK, &req, sizeof(req));
        return;
    }
    FWD_VOID(glColorMask, (unsigned char, unsigned char, unsigned char, unsigned char),
             (r, g, b, a));
    (void)r; (void)g; (void)b; (void)a;
}
void glLineWidth(float w) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_1f req = { w };
        shim_gl_marshal_send_async(GL_OP_GL_LINE_WIDTH, &req, sizeof(req));
        return;
    }
    FWD_VOID(glLineWidth, (float), (w));
    (void)w;
}
void glPolygonOffset(float f, float u) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_2f req = { f, u };
        shim_gl_marshal_send_async(GL_OP_GL_POLYGON_OFFSET, &req, sizeof(req));
        return;
    }
    FWD_VOID(glPolygonOffset, (float, float), (f, u));
    (void)f; (void)u;
}
void glClearColor(float r, float g, float b, float a) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_clear_color pl = { r, g, b, a };
        shim_gl_marshal_send_async(GL_OP_GL_CLEAR_COLOR, &pl, sizeof(pl));
        return;
    }
    FWD_VOID(glClearColor, (float, float, float, float), (r, g, b, a));
    (void)r; (void)g; (void)b; (void)a;
}
void glClearDepthf(float d) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_1f req = { d };
        shim_gl_marshal_send_async(GL_OP_GL_CLEAR_DEPTHF, &req, sizeof(req));
        return;
    }
    FWD_VOID(glClearDepthf, (float), (d));
    (void)d;
}
void glClearStencil(int s) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_1i req = { s };
        shim_gl_marshal_send_async(GL_OP_GL_CLEAR_STENCIL, &req, sizeof(req));
        return;
    }
    FWD_VOID(glClearStencil, (int), (s));
    (void)s;
}
void glClear(unsigned mask) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_clear pl = { .mask = mask };
        shim_gl_marshal_send_async(GL_OP_GL_CLEAR, &pl, sizeof(pl));
        return;
    }
    FWD_VOID(glClear, (unsigned), (mask));
    (void)mask;
}

void glDrawArrays(unsigned mode, int first, int count) {
    SHIM_GL_DBG_HOOK(glDrawArrays);
    if (shim_gl_marshal_active()) {
        struct gl_payload_draw_arrays req = { mode, first, count };
        shim_gl_marshal_send_async(GL_OP_GL_DRAW_ARRAYS, &req, sizeof(req));
        return;
    }
    FWD_VOID(glDrawArrays, (unsigned, int, int), (mode, first, count));
    (void)mode; (void)first;
    GLLOG(draw, "glDrawArrays(count=%d)", count);
}
void glDrawElements(unsigned mode, int count, unsigned type, const void *idx) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_draw_elements req = { mode, count, type, (uint64_t)(uintptr_t)idx };
        shim_gl_marshal_send_async(GL_OP_GL_DRAW_ELEMENTS, &req, sizeof(req));
        return;
    }
    FWD_VOID(glDrawElements, (unsigned, int, unsigned, const void*),
             (mode, count, type, idx));
    (void)mode; (void)type; (void)idx;
    GLLOG(draw, "glDrawElements(count=%d)", count);
}
void glDrawArraysInstanced(unsigned mode, int first, int count, int inst) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_draw_arrays_instanced req = { mode, first, count, inst };
        shim_gl_marshal_send_async(GL_OP_GL_DRAW_ARRAYS_INSTANCED, &req, sizeof(req));
        return;
    }
    FWD_VOID(glDrawArraysInstanced, (unsigned, int, int, int),
             (mode, first, count, inst));
    (void)mode; (void)first; (void)count; (void)inst;
}
void glDrawElementsInstanced(unsigned mode, int count, unsigned type, const void *idx, int inst) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_draw_elements_instanced req = {
            mode, count, type, (uint64_t)(uintptr_t)idx, inst };
        shim_gl_marshal_send_async(GL_OP_GL_DRAW_ELEMENTS_INSTANCED, &req, sizeof(req));
        return;
    }
    FWD_VOID(glDrawElementsInstanced, (unsigned, int, unsigned, const void*, int),
             (mode, count, type, idx, inst));
    (void)mode; (void)count; (void)type; (void)idx; (void)inst;
}
void glVertexAttribDivisor(unsigned idx, unsigned divisor) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_2u req = { idx, divisor };
        shim_gl_marshal_send_async(GL_OP_GL_VERTEX_ATTRIB_DIVISOR, &req, sizeof(req));
        return;
    }
    FWD_VOID(glVertexAttribDivisor, (unsigned, unsigned), (idx, divisor));
    (void)idx; (void)divisor;
}

void glFinish(void) {
    if (shim_gl_marshal_active()) {
        uint32_t reply_len = 0;
        (void)shim_gl_marshal_send_sync(GL_OP_GL_FINISH, NULL, 0,
                                        NULL, 0, &reply_len);
        return;
    }
    FWD_VOID(glFinish, (void), ());
}
void glFlush(void) {
    if (shim_gl_marshal_active()) {
        shim_gl_marshal_send_async(GL_OP_GL_FLUSH, NULL, 0);
        return;
    }
    FWD_VOID(glFlush, (void), ());
}
void glReadPixels(int x, int y, int w, int h, unsigned fmt, unsigned type, void *data) {
    if (shim_gl_marshal_active() && data) {
        struct gl_payload_read_pixels req = { x, y, w, h, fmt, type };
        size_t bytes = (size_t)w * (size_t)h * 4;
        size_t reply_size = sizeof(struct gl_payload_rep_pixels) + bytes;
        unsigned char *reply = (unsigned char*)malloc(reply_size);
        if (reply) {
            uint32_t reply_len = 0;
            int rep_op = shim_gl_marshal_send_sync(GL_OP_GL_READ_PIXELS,
                                                   &req, sizeof(req),
                                                   reply, (uint32_t)reply_size,
                                                   &reply_len);
            if (rep_op == GL_OP_REP_PIXELS &&
                reply_len >= sizeof(struct gl_payload_rep_pixels)) {
                size_t got = reply_len - sizeof(struct gl_payload_rep_pixels);
                if (got > bytes) got = bytes;
                memcpy(data, reply + sizeof(struct gl_payload_rep_pixels), got);
                if (got < bytes) memset((unsigned char*)data + got, 0, bytes - got);
            } else {
                memset(data, 0, bytes);
            }
            free(reply);
        } else {
            memset(data, 0, bytes);
        }
        return;
    }
    FWD_VOID(glReadPixels, (int, int, int, int, unsigned, unsigned, void*),
             (x, y, w, h, fmt, type, data));
    (void)x; (void)y; (void)fmt; (void)type;
    if (data) memset(data, 0, (size_t)w * (size_t)h * 4);
}

/* ---------------------------------------------------------------------------
 * ES3 functions referenced by game.elf that the shim previously didn't export.
 * Without these the dynamic linker resolves them in Mesa overlay → uninitialised
 * Mesa state → silent corruption. All marshal to the daemon when active; when
 * not active they fall through to FWD_* / canned stub values exactly as the
 * other GL hooks. */

void glBindBufferBase(unsigned target, unsigned index, unsigned buffer) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_bind_buffer_base req = { target, index, buffer };
        shim_gl_marshal_send_async(GL_OP_GL_BIND_BUFFER_BASE, &req, sizeof(req));
        return;
    }
    FWD_VOID(glBindBufferBase, (unsigned, unsigned, unsigned), (target, index, buffer));
    (void)target; (void)index; (void)buffer;
}
void glBindBufferRange(unsigned target, unsigned index, unsigned buffer,
                       intptr_t offset, intptr_t size) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_bind_buffer_range req = {
            target, index, buffer, (int64_t)offset, (int64_t)size };
        shim_gl_marshal_send_async(GL_OP_GL_BIND_BUFFER_RANGE, &req, sizeof(req));
        return;
    }
    FWD_VOID(glBindBufferRange, (unsigned, unsigned, unsigned, intptr_t, intptr_t),
             (target, index, buffer, offset, size));
    (void)target; (void)index; (void)buffer; (void)offset; (void)size;
}
void glUniformBlockBinding(unsigned program, unsigned block_index, unsigned binding) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_uniform_block_binding req = { program, block_index, binding };
        shim_gl_marshal_send_async(GL_OP_GL_UNIFORM_BLOCK_BINDING, &req, sizeof(req));
        return;
    }
    FWD_VOID(glUniformBlockBinding, (unsigned, unsigned, unsigned),
             (program, block_index, binding));
    (void)program; (void)block_index; (void)binding;
}
unsigned glGetUniformBlockIndex(unsigned program, const char *name) {
    if (shim_gl_marshal_active() && name) {
        size_t nlen = strlen(name);
        size_t plen = sizeof(struct gl_payload_get_ublock_index) + nlen;
        unsigned char *buf = (unsigned char*)malloc(plen);
        unsigned ret = 0xFFFFFFFFu; /* GL_INVALID_INDEX */
        if (buf) {
            struct gl_payload_get_ublock_index *r = (struct gl_payload_get_ublock_index*)buf;
            r->program = program; r->name_len = (uint32_t)nlen;
            memcpy(buf + sizeof(*r), name, nlen);
            struct gl_payload_rep_value reply = {0};
            uint32_t rl = 0;
            int rep_op = shim_gl_marshal_send_sync(GL_OP_GL_GET_UBLOCK_INDEX,
                                                    buf, (uint32_t)plen,
                                                    &reply, sizeof(reply), &rl);
            if (rep_op == GL_OP_REP_VALUE) ret = reply.value;
            free(buf);
        }
        return ret;
    }
    FWD_RET(unsigned, glGetUniformBlockIndex, (unsigned, const char*), (program, name));
    (void)program; (void)name; return 0xFFFFFFFFu;
}
void glBlitFramebuffer(int sx0, int sy0, int sx1, int sy1,
                       int dx0, int dy0, int dx1, int dy1,
                       unsigned mask, unsigned filt) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_blit_framebuffer req = {
            sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filt };
        shim_gl_marshal_send_async(GL_OP_GL_BLIT_FRAMEBUFFER, &req, sizeof(req));
        return;
    }
    FWD_VOID(glBlitFramebuffer,
             (int, int, int, int, int, int, int, int, unsigned, unsigned),
             (sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filt));
    (void)sx0; (void)sy0; (void)sx1; (void)sy1;
    (void)dx0; (void)dy0; (void)dx1; (void)dy1;
    (void)mask; (void)filt;
}
void glDrawBuffers(int n, const unsigned *bufs) {
    if (shim_gl_marshal_active() && n > 0 && bufs) {
        size_t plen = sizeof(struct gl_payload_draw_buffers) + (size_t)n * 4;
        unsigned char *buf = (unsigned char*)malloc(plen);
        if (buf) {
            struct gl_payload_draw_buffers *r = (struct gl_payload_draw_buffers*)buf;
            r->count = (uint32_t)n;
            memcpy(buf + sizeof(*r), bufs, (size_t)n * 4);
            shim_gl_marshal_send_async(GL_OP_GL_DRAW_BUFFERS, buf, (uint32_t)plen);
            free(buf);
        }
        return;
    }
    FWD_VOID(glDrawBuffers, (int, const unsigned*), (n, bufs));
    (void)n; (void)bufs;
}
void glReadBuffer(unsigned src) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_1u req = { src };
        shim_gl_marshal_send_async(GL_OP_GL_READ_BUFFER, &req, sizeof(req));
        return;
    }
    FWD_VOID(glReadBuffer, (unsigned), (src));
    (void)src;
}
void glClearBufferfv(unsigned buffer, int drawbuffer, const float *value) {
    if (shim_gl_marshal_active() && value) {
        size_t plen = sizeof(struct gl_payload_clear_buffer_fv) + 16;
        unsigned char *buf = (unsigned char*)malloc(plen);
        if (buf) {
            struct gl_payload_clear_buffer_fv *r = (struct gl_payload_clear_buffer_fv*)buf;
            r->buffer = buffer; r->drawbuffer = drawbuffer;
            memcpy(buf + sizeof(*r), value, 16);
            shim_gl_marshal_send_async(GL_OP_GL_CLEAR_BUFFER_FV, buf, (uint32_t)plen);
            free(buf);
        }
        return;
    }
    FWD_VOID(glClearBufferfv, (unsigned, int, const float*), (buffer, drawbuffer, value));
    (void)buffer; (void)drawbuffer; (void)value;
}
void glInvalidateFramebuffer(unsigned target, int n, const unsigned *atts) {
    if (shim_gl_marshal_active() && n >= 0) {
        size_t plen = sizeof(struct gl_payload_invalidate_fb) + (size_t)n * 4;
        unsigned char *buf = (unsigned char*)malloc(plen);
        if (buf) {
            struct gl_payload_invalidate_fb *r = (struct gl_payload_invalidate_fb*)buf;
            r->target = target; r->count = (uint32_t)n;
            if (n > 0 && atts) memcpy(buf + sizeof(*r), atts, (size_t)n * 4);
            shim_gl_marshal_send_async(GL_OP_GL_INVALIDATE_FB, buf, (uint32_t)plen);
            free(buf);
        }
        return;
    }
    FWD_VOID(glInvalidateFramebuffer, (unsigned, int, const unsigned*),
             (target, n, atts));
    (void)target; (void)n; (void)atts;
}
void glVertexAttribIPointer(unsigned idx, int size, unsigned type,
                             int stride, const void *ptr) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_vap_i req = { idx, size, type, stride, (uint64_t)(uintptr_t)ptr };
        shim_gl_marshal_send_async(GL_OP_GL_VERTEX_ATTRIB_I_PTR, &req, sizeof(req));
        return;
    }
    FWD_VOID(glVertexAttribIPointer,
             (unsigned, int, unsigned, int, const void*),
             (idx, size, type, stride, ptr));
    (void)idx; (void)size; (void)type; (void)stride; (void)ptr;
}
void glDrawRangeElements(unsigned mode, unsigned start, unsigned end,
                         int count, unsigned type, const void *idx) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_draw_range_elements req = {
            mode, start, end, count, type, (uint64_t)(uintptr_t)idx };
        shim_gl_marshal_send_async(GL_OP_GL_DRAW_RANGE_ELEMENTS, &req, sizeof(req));
        return;
    }
    FWD_VOID(glDrawRangeElements,
             (unsigned, unsigned, unsigned, int, unsigned, const void*),
             (mode, start, end, count, type, idx));
    (void)mode; (void)start; (void)end; (void)count; (void)type; (void)idx;
}
void glCopyBufferSubData(unsigned rt, unsigned wt, intptr_t ro, intptr_t wo, intptr_t sz) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_copy_buffer_sub_data req = {
            rt, wt, (int64_t)ro, (int64_t)wo, (int64_t)sz };
        shim_gl_marshal_send_async(GL_OP_GL_COPY_BUFFER_SUB_DATA, &req, sizeof(req));
        return;
    }
    FWD_VOID(glCopyBufferSubData,
             (unsigned, unsigned, intptr_t, intptr_t, intptr_t),
             (rt, wt, ro, wo, sz));
    (void)rt; (void)wt; (void)ro; (void)wo; (void)sz;
}
void glTexImage3D(unsigned target, int level, int internal, int w, int h, int d,
                  int border, unsigned format, unsigned type, const void *data) {
    if (shim_gl_marshal_active()) {
        size_t dsz = data ? (size_t)w * (size_t)h * (size_t)d * gl_pixel_size(format, type) : 0;
        size_t plen = sizeof(struct gl_payload_tex_image_3d) + dsz;
        unsigned char *buf = (unsigned char*)malloc(plen);
        if (buf) {
            struct gl_payload_tex_image_3d *r = (struct gl_payload_tex_image_3d*)buf;
            r->target = target; r->level = level; r->internal_format = internal;
            r->width = w; r->height = h; r->depth = d; r->border = border;
            r->format = format; r->type = type; r->data_size = (uint32_t)dsz;
            if (dsz) memcpy(buf + sizeof(*r), data, dsz);
            shim_gl_marshal_send_async(GL_OP_GL_TEX_IMAGE_3D, buf, (uint32_t)plen);
            free(buf);
        }
        return;
    }
    FWD_VOID(glTexImage3D,
             (unsigned, int, int, int, int, int, int, unsigned, unsigned, const void*),
             (target, level, internal, w, h, d, border, format, type, data));
    (void)target; (void)level; (void)internal; (void)w; (void)h; (void)d;
    (void)border; (void)format; (void)type; (void)data;
}
void glTexSubImage3D(unsigned target, int level, int xo, int yo, int zo,
                     int w, int h, int d, unsigned format, unsigned type,
                     const void *data) {
    if (shim_gl_marshal_active()) {
        size_t dsz = data ? (size_t)w * (size_t)h * (size_t)d * gl_pixel_size(format, type) : 0;
        size_t plen = sizeof(struct gl_payload_tex_sub_image_3d) + dsz;
        unsigned char *buf = (unsigned char*)malloc(plen);
        if (buf) {
            struct gl_payload_tex_sub_image_3d *r = (struct gl_payload_tex_sub_image_3d*)buf;
            r->target = target; r->level = level;
            r->xoffset = xo; r->yoffset = yo; r->zoffset = zo;
            r->width = w; r->height = h; r->depth = d;
            r->format = format; r->type = type; r->data_size = (uint32_t)dsz;
            if (dsz) memcpy(buf + sizeof(*r), data, dsz);
            shim_gl_marshal_send_async(GL_OP_GL_TEX_SUB_IMAGE_3D, buf, (uint32_t)plen);
            free(buf);
        }
        return;
    }
    FWD_VOID(glTexSubImage3D,
             (unsigned, int, int, int, int, int, int, int, unsigned, unsigned, const void*),
             (target, level, xo, yo, zo, w, h, d, format, type, data));
    (void)target; (void)level; (void)xo; (void)yo; (void)zo;
    (void)w; (void)h; (void)d; (void)format; (void)type; (void)data;
}
void glTexStorage2D(unsigned target, int levels, unsigned internal, int w, int h) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_tex_storage_2d req = { target, levels, internal, w, h };
        shim_gl_marshal_send_async(GL_OP_GL_TEX_STORAGE_2D, &req, sizeof(req));
        return;
    }
    FWD_VOID(glTexStorage2D, (unsigned, int, unsigned, int, int),
             (target, levels, internal, w, h));
    (void)target; (void)levels; (void)internal; (void)w; (void)h;
}
void glTexStorage3D(unsigned target, int levels, unsigned internal, int w, int h, int d) {
    if (shim_gl_marshal_active()) {
        struct gl_payload_tex_storage_3d req = { target, levels, internal, w, h, d };
        shim_gl_marshal_send_async(GL_OP_GL_TEX_STORAGE_3D, &req, sizeof(req));
        return;
    }
    FWD_VOID(glTexStorage3D, (unsigned, int, unsigned, int, int, int),
             (target, levels, internal, w, h, d));
    (void)target; (void)levels; (void)internal; (void)w; (void)h; (void)d;
}

/* Functions safe to no-op when daemon active: debug callbacks, transform
 * feedback (game uses for HEVC out-path which we replace via shim_avforward),
 * occlusion queries, OVR multiview, sync objects (we synthesise sentinels),
 * program-binary cache (game falls back to source-compile on no-format). */

void glDebugMessageCallback(void *cb, const void *userParam) {
    (void)cb; (void)userParam;
}
void glDebugMessageCallbackKHR(void *cb, const void *userParam) {
    (void)cb; (void)userParam;
}
void glBeginTransformFeedback(unsigned mode) { (void)mode; }
void glEndTransformFeedback(void) { }
void glTransformFeedbackVaryings(unsigned program, int count,
                                 const char *const *varyings, unsigned mode) {
    (void)program; (void)count; (void)varyings; (void)mode;
}
void glGenQueries(int n, unsigned *out) {
    static uint32_t qid = 1;
    if (out) for (int i = 0; i < n; i++) out[i] = qid++;
}
void glDeleteQueries(int n, const unsigned *ids) { (void)n; (void)ids; }
void glFramebufferTextureMultiviewOVR(unsigned target, unsigned att, unsigned tex,
                                       int level, int baseViewIndex, int numViews) {
    (void)target; (void)att; (void)tex; (void)level; (void)baseViewIndex; (void)numViews;
}
void glFramebufferTextureMultisampleMultiviewOVR(unsigned target, unsigned att, unsigned tex,
                                                  int level, int samples,
                                                  int baseViewIndex, int numViews) {
    (void)target; (void)att; (void)tex; (void)level;
    (void)samples; (void)baseViewIndex; (void)numViews;
}
/* Sync objects: return monotonic sentinels. ClientWaitSync always reports
 * "already signaled" so the game's wait-loop terminates. */
void *glFenceSync(unsigned condition, unsigned flags) {
    static uintptr_t sync_seq = 0xfeed0000ull;
    (void)condition; (void)flags;
    return (void *)__atomic_add_fetch(&sync_seq, 1, __ATOMIC_RELAXED);
}
void glDeleteSync(void *sync) { (void)sync; }
unsigned glClientWaitSync(void *sync, unsigned flags, uint64_t timeout) {
    (void)sync; (void)flags; (void)timeout;
    return 0x911A;  /* GL_ALREADY_SIGNALED */
}
void glWaitSync(void *sync, unsigned flags, uint64_t timeout) {
    (void)sync; (void)flags; (void)timeout;
}
void glGetSynciv(void *sync, unsigned pname, int bufSize, int *length, int *values) {
    (void)sync;
    if (length) *length = 1;
    if (values && bufSize >= 1) {
        if (pname == 0x9114 /* SYNC_STATUS */)        values[0] = 0x9117 /* SIGNALED */;
        else if (pname == 0x9112 /* OBJECT_TYPE */)   values[0] = 0x9116 /* SYNC_FENCE */;
        else if (pname == 0x9113 /* SYNC_CONDITION */) values[0] = 0x9117;
        else                                           values[0] = 0;
    }
}
/* Program binary: report no formats; game falls back to source compile. */
void glGetProgramBinary(unsigned program, int bufSize, int *length,
                        unsigned *binaryFormat, void *binary) {
    (void)program; (void)bufSize; (void)binary;
    if (length) *length = 0;
    if (binaryFormat) *binaryFormat = 0;
}
void glProgramBinary(unsigned program, unsigned binaryFormat,
                     const void *binary, int length) {
    (void)program; (void)binaryFormat; (void)binary; (void)length;
}

typedef void (*pfn_t)(void);

void (*eglGetProcAddress(const char *name))(void) {
    /* Never forwarded to Mesa, even under SPIKE3_REAL_GL. Mesa's real
     * eglCreateImageKHR / eglCreateSyncKHR / glEGLImageTargetTexture2DOES
     * would be handed our fake GBM BOs and fake sync handles and SEGV on
     * the first deref (Step 55.6 diagnosis). The KHR stubs below accept
     * those handles silently. eglGetPlatformDisplay[EXT] returns our own
     * trampoline; forwarding happens *inside* its body via FWD_RET_V,
     * where the fake display the game stores matches our other interceptors.
     * Unknown names return NULL rather than falling through to
     * dlsym(RTLD_NEXT, name) — every unstubbed extension is one we have
     * not proven fake-handle-safe. Names match BN's reading of
     * glWindow::setGlContext (KHR image/sync + GL_OES_EGL_image). */
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
        /* FBO attach functions — Step 62 pivot. Route these here so
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

