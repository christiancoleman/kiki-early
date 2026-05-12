/*
 * gles_probe3.c — minimal aarch64 EGL+GLES probe linked DIRECTLY against
 * libEGL/libGLESv2 (no dlopen). Built for testing the shim → gl_daemon
 * end-to-end path under LD_PRELOAD=spi_shim.so + GL_DAEMON=1. EGL/GLES
 * decls inlined to avoid header dependency on the cross-compiler.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef void *EGLDisplay, *EGLConfig, *EGLContext, *EGLSurface;
typedef int   EGLint;
typedef unsigned EGLBoolean;
typedef unsigned EGLenum;
typedef unsigned GLbitfield, GLenum, GLuint;
typedef int      GLint, GLsizei;
typedef float    GLfloat;
typedef unsigned char GLboolean;

#define EGL_DEFAULT_DISPLAY ((void*)0)
#define EGL_NO_CONTEXT      ((void*)0)
#define EGL_NO_DISPLAY      ((void*)0)
#define EGL_NO_SURFACE      ((void*)0)
#define EGL_TRUE  1
#define EGL_NONE                       0x3038
#define EGL_SURFACE_TYPE               0x3033
#define EGL_PBUFFER_BIT                0x0001
#define EGL_RED_SIZE                   0x3024
#define EGL_GREEN_SIZE                 0x3023
#define EGL_BLUE_SIZE                  0x3022
#define EGL_ALPHA_SIZE                 0x3021
#define EGL_RENDERABLE_TYPE            0x3040
#define EGL_OPENGL_ES2_BIT             0x0004
#define EGL_OPENGL_ES_API              0x30A0
#define EGL_CONTEXT_CLIENT_VERSION     0x3098
#define EGL_WIDTH                      0x3057
#define EGL_HEIGHT                     0x3056

#define GL_NO_ERROR          0
#define GL_COLOR_BUFFER_BIT  0x4000
#define GL_RGBA              0x1908
#define GL_UNSIGNED_BYTE     0x1401

extern EGLDisplay eglGetDisplay(void *native);
extern EGLBoolean eglInitialize(EGLDisplay, EGLint *maj, EGLint *min);
extern EGLBoolean eglBindAPI(EGLenum api);
extern EGLBoolean eglChooseConfig(EGLDisplay, const EGLint *attribs,
                                   EGLConfig *configs, EGLint cfg_size, EGLint *n);
extern EGLContext eglCreateContext(EGLDisplay, EGLConfig, EGLContext share,
                                    const EGLint *attribs);
extern EGLSurface eglCreatePbufferSurface(EGLDisplay, EGLConfig,
                                           const EGLint *attribs);
extern EGLBoolean eglMakeCurrent(EGLDisplay, EGLSurface draw, EGLSurface read,
                                  EGLContext);
extern EGLint     eglGetError(void);

extern void  glViewport(GLint, GLint, GLsizei, GLsizei);
extern void  glClearColor(GLfloat, GLfloat, GLfloat, GLfloat);
extern void  glClear(GLbitfield);
extern void  glFinish(void);
extern void  glReadPixels(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
extern GLenum glGetError(void);

#define W 64
#define H 32

static int fail(const char *step) {
    fprintf(stderr, "FAIL at %s (egl=0x%x gl=0x%x)\n", step, eglGetError(), glGetError());
    return 1;
}

int main(void) {
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY) return fail("eglGetDisplay");
    EGLint maj, min;
    if (!eglInitialize(dpy, &maj, &min)) return fail("eglInitialize");
    fprintf(stderr, "EGL %d.%d\n", maj, min);

    if (!eglBindAPI(EGL_OPENGL_ES_API)) return fail("eglBindAPI");

    EGLint cfg_attrs[] = {
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE,
    };
    EGLConfig cfg;
    EGLint n;
    if (!eglChooseConfig(dpy, cfg_attrs, &cfg, 1, &n) || n < 1) return fail("eglChooseConfig");

    EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
    if (ctx == EGL_NO_CONTEXT) return fail("eglCreateContext");

    EGLint pb_attrs[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE };
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pb_attrs);
    if (surf == EGL_NO_SURFACE) return fail("eglCreatePbufferSurface");

    if (!eglMakeCurrent(dpy, surf, surf, ctx)) return fail("eglMakeCurrent");

    glViewport(0, 0, W, H);
    glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    if (glGetError() != GL_NO_ERROR) return fail("post-clear glGetError");

    unsigned char pixels[W * H * 4];
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    int red_count = 0;
    for (int i = 0; i < W * H * 4; i += 4) {
        if (pixels[i] == 255 && pixels[i+1] == 0 && pixels[i+2] == 0 && pixels[i+3] == 255)
            red_count++;
    }
    fprintf(stderr, "first pixel: (%u,%u,%u,%u)\n",
            pixels[0], pixels[1], pixels[2], pixels[3]);
    fprintf(stderr, "red pixels: %d/%d\n", red_count, W * H);
    if (red_count == W * H) {
        fprintf(stderr, "VERDICT: ALL PASS — shim → gl_daemon end-to-end works\n");
        return 0;
    }
    return fail("pixel verification");
}
