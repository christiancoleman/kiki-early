/* End-to-end aarch64 GLES3 render probe: surfaceless EGL → pbuffer → GLES3 context
 * → glClear(1.0, 0.5, 0.25, 1.0) → glReadPixels → check pixel matches clear color.
 * If the read-back pixel matches, llvmpipe is actually rasterizing.
 * No EGL/GLES headers needed — dlsym'd with explicit typedefs. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef void *EGLDisplay;
typedef void *EGLConfig;
typedef void *EGLContext;
typedef void *EGLSurface;
typedef unsigned int EGLBoolean;
typedef int EGLint;
typedef unsigned int GLenum;
typedef unsigned int GLbitfield;
typedef int GLint;
typedef int GLsizei;
typedef float GLfloat;
typedef unsigned char GLubyte;

#define EGL_DEFAULT_DISPLAY          ((void*)0)
#define EGL_NO_CONTEXT               ((EGLContext)0)
#define EGL_NONE                     0x3038
#define EGL_RED_SIZE                 0x3024
#define EGL_GREEN_SIZE               0x3023
#define EGL_BLUE_SIZE                0x3022
#define EGL_ALPHA_SIZE               0x3021
#define EGL_DEPTH_SIZE               0x3025
#define EGL_SURFACE_TYPE             0x3033
#define EGL_RENDERABLE_TYPE          0x3040
#define EGL_PBUFFER_BIT              0x0001
#define EGL_OPENGL_ES2_BIT           0x0004
#define EGL_OPENGL_ES3_BIT           0x0040
#define EGL_OPENGL_ES_API            0x30A0
#define EGL_WIDTH                    0x3057
#define EGL_HEIGHT                   0x3056
#define EGL_CONTEXT_CLIENT_VERSION   0x3098
#define EGL_VENDOR                   0x3053

#define GL_COLOR_BUFFER_BIT          0x4000
#define GL_RGBA                      0x1908
#define GL_UNSIGNED_BYTE             0x1401
#define GL_VERSION                   0x1F02
#define GL_RENDERER                  0x1F01
#define GL_VENDOR                    0x1F00
#define GL_SHADING_LANGUAGE_VERSION  0x8B8C

typedef EGLDisplay  (*PFN_eglGetDisplay)(void*);
typedef EGLBoolean  (*PFN_eglInitialize)(EGLDisplay, EGLint*, EGLint*);
typedef EGLBoolean  (*PFN_eglBindAPI)(EGLint);
typedef EGLBoolean  (*PFN_eglChooseConfig)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*);
typedef EGLSurface  (*PFN_eglCreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint*);
typedef EGLContext  (*PFN_eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint*);
typedef EGLBoolean  (*PFN_eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
typedef EGLint      (*PFN_eglGetError)(void);
typedef const char *(*PFN_eglQueryString)(EGLDisplay, EGLint);

typedef const GLubyte *(*PFN_glGetString)(GLenum);
typedef void           (*PFN_glClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void           (*PFN_glClear)(GLbitfield);
typedef void           (*PFN_glFinish)(void);
typedef void           (*PFN_glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
typedef GLenum         (*PFN_glGetError)(void);

#define LOAD(VAR, H, NAME) do {                                  \
    VAR = (void*)dlsym((H), (NAME));                             \
    if (!VAR) { fprintf(stderr, "dlsym %s fail: %s\n",           \
                        (NAME), dlerror()); return 9; }          \
} while(0)

int main(void) {
    void *egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!egl) { fprintf(stderr, "dlopen libEGL: %s\n", dlerror()); return 1; }

    PFN_eglGetDisplay           pf_eglGetDisplay;
    PFN_eglInitialize           pf_eglInitialize;
    PFN_eglBindAPI              pf_eglBindAPI;
    PFN_eglChooseConfig         pf_eglChooseConfig;
    PFN_eglCreatePbufferSurface pf_eglCreatePbufferSurface;
    PFN_eglCreateContext        pf_eglCreateContext;
    PFN_eglMakeCurrent          pf_eglMakeCurrent;
    PFN_eglGetError             pf_eglGetError;
    PFN_eglQueryString          pf_eglQueryString;
    LOAD(pf_eglGetDisplay,           egl, "eglGetDisplay");
    LOAD(pf_eglInitialize,           egl, "eglInitialize");
    LOAD(pf_eglBindAPI,              egl, "eglBindAPI");
    LOAD(pf_eglChooseConfig,         egl, "eglChooseConfig");
    LOAD(pf_eglCreatePbufferSurface, egl, "eglCreatePbufferSurface");
    LOAD(pf_eglCreateContext,        egl, "eglCreateContext");
    LOAD(pf_eglMakeCurrent,          egl, "eglMakeCurrent");
    LOAD(pf_eglGetError,             egl, "eglGetError");
    LOAD(pf_eglQueryString,          egl, "eglQueryString");

    EGLDisplay d = pf_eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!d) { fprintf(stderr, "eglGetDisplay NO_DISPLAY err=0x%x\n", pf_eglGetError()); return 2; }
    EGLint maj = 0, min = 0;
    if (!pf_eglInitialize(d, &maj, &min)) { fprintf(stderr, "eglInitialize err=0x%x\n", pf_eglGetError()); return 3; }
    fprintf(stderr, "EGL %d.%d  vendor='%s'\n", maj, min, pf_eglQueryString(d, EGL_VENDOR));

    if (!pf_eglBindAPI(EGL_OPENGL_ES_API)) { fprintf(stderr, "eglBindAPI err=0x%x\n", pf_eglGetError()); return 4; }

    EGLint cfg_attrs[] = {
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8,
        EGL_DEPTH_SIZE,      24,
        EGL_NONE
    };
    EGLConfig cfg; EGLint n_cfg = 0;
    if (!pf_eglChooseConfig(d, cfg_attrs, &cfg, 1, &n_cfg) || n_cfg == 0) {
        fprintf(stderr, "eglChooseConfig err=0x%x n_cfg=%d\n", pf_eglGetError(), n_cfg); return 5;
    }
    fprintf(stderr, "eglChooseConfig: picked %d config(s)\n", n_cfg);

    EGLint pb_attrs[] = { EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE };
    EGLSurface surf = pf_eglCreatePbufferSurface(d, cfg, pb_attrs);
    if (!surf) { fprintf(stderr, "eglCreatePbufferSurface err=0x%x\n", pf_eglGetError()); return 6; }

    EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext ctx = pf_eglCreateContext(d, cfg, EGL_NO_CONTEXT, ctx_attrs);
    if (!ctx) { fprintf(stderr, "eglCreateContext err=0x%x\n", pf_eglGetError()); return 7; }
    if (!pf_eglMakeCurrent(d, surf, surf, ctx)) { fprintf(stderr, "eglMakeCurrent err=0x%x\n", pf_eglGetError()); return 8; }
    fprintf(stderr, "EGL context bound to pbuffer 64x64\n");

    void *gl = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
    if (!gl) { fprintf(stderr, "dlopen libGLESv2: %s\n", dlerror()); return 10; }
    PFN_glGetString  pf_glGetString;
    PFN_glClearColor pf_glClearColor;
    PFN_glClear      pf_glClear;
    PFN_glFinish     pf_glFinish;
    PFN_glReadPixels pf_glReadPixels;
    PFN_glGetError   pf_glGetError;
    LOAD(pf_glGetString,  gl, "glGetString");
    LOAD(pf_glClearColor, gl, "glClearColor");
    LOAD(pf_glClear,      gl, "glClear");
    LOAD(pf_glFinish,     gl, "glFinish");
    LOAD(pf_glReadPixels, gl, "glReadPixels");
    LOAD(pf_glGetError,   gl, "glGetError");

    printf("GL_VENDOR:   %s\n", (const char*)pf_glGetString(GL_VENDOR));
    printf("GL_RENDERER: %s\n", (const char*)pf_glGetString(GL_RENDERER));
    printf("GL_VERSION:  %s\n", (const char*)pf_glGetString(GL_VERSION));
    printf("GL_GLSL_VER: %s\n", (const char*)pf_glGetString(GL_SHADING_LANGUAGE_VERSION));

    pf_glClearColor(1.0f, 0.5f, 0.25f, 1.0f);   /* RGBA target ≈ 0xFF 0x80 0x40 0xFF */
    pf_glClear(GL_COLOR_BUFFER_BIT);
    pf_glFinish();
    GLenum e = pf_glGetError();
    if (e) { fprintf(stderr, "glClear path err=0x%x\n", e); return 11; }

    unsigned char pix[4] = {0};
    pf_glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pix);
    e = pf_glGetError();
    if (e) { fprintf(stderr, "glReadPixels err=0x%x\n", e); return 12; }
    printf("pixel(32,32) = %02x %02x %02x %02x\n", pix[0], pix[1], pix[2], pix[3]);

    int ok = (pix[0] >= 0xfc &&
              pix[1] >= 0x7a && pix[1] <= 0x86 &&
              pix[2] >= 0x3a && pix[2] <= 0x46 &&
              pix[3] >= 0xfc);
    printf("RASTER: %s\n", ok ? "PASS" : "MISMATCH");
    return ok ? 0 : 13;
}
