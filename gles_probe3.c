/*
 * gles_probe3.c — minimal aarch64 EGL+GLES probe that links DIRECTLY against
 * libEGL/libGLESv2 (no dlopen). Built for testing the shim → gl_daemon
 * end-to-end path under LD_PRELOAD=spi_shim.so + GL_DAEMON=1.
 *
 * Sequence: get display → initialize → bind GLES → choose config → create
 * context + pbuffer → make current → clear to red → readback → verify.
 */
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    if (glGetError() != GL_NO_ERROR) return fail("post-readPixels glGetError");

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
