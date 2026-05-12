/* aarch64 EGL init probe.
 * Dynamically resolves EGL symbols from the sysroot libEGL.so.1 (no EGL headers needed),
 * tries eglGetDisplay + eglInitialize + eglQueryString. Prints vendor / version /
 * extensions so we can see what driver Mesa picked. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void *EGLDisplay;
typedef unsigned int EGLBoolean;
typedef int EGLint;

#define EGL_DEFAULT_DISPLAY      ((void*)0)
#define EGL_NO_DISPLAY           ((EGLDisplay)0)
#define EGL_VENDOR               0x3053
#define EGL_VERSION              0x3054
#define EGL_EXTENSIONS           0x3055
#define EGL_CLIENT_APIS          0x308D

int main(void) {
    void *h = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!h) { fprintf(stderr, "dlopen libEGL.so.1: %s\n", dlerror()); return 1; }
    fprintf(stderr, "libEGL.so.1 loaded: %p\n", h);

    EGLDisplay (*eglGetDisplay)(void*) = dlsym(h, "eglGetDisplay");
    EGLBoolean (*eglInitialize)(EGLDisplay, EGLint*, EGLint*) = dlsym(h, "eglInitialize");
    const char *(*eglQueryString)(EGLDisplay, EGLint) = dlsym(h, "eglQueryString");
    EGLint (*eglGetError)(void) = dlsym(h, "eglGetError");
    if (!eglGetDisplay || !eglInitialize || !eglQueryString || !eglGetError) {
        fprintf(stderr, "dlsym failed: %s\n", dlerror());
        return 2;
    }

    fprintf(stderr, "eglGetDisplay(EGL_DEFAULT_DISPLAY)...\n");
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (d == EGL_NO_DISPLAY) {
        fprintf(stderr, "  returned EGL_NO_DISPLAY; err=0x%x\n", eglGetError());
        return 3;
    }
    fprintf(stderr, "  got display %p\n", d);

    EGLint maj = 0, min = 0;
    fprintf(stderr, "eglInitialize(d)...\n");
    if (!eglInitialize(d, &maj, &min)) {
        fprintf(stderr, "  FAIL err=0x%x\n", eglGetError());
        return 4;
    }
    printf("EGL %d.%d\n", maj, min);

    const char *v = eglQueryString(d, EGL_VENDOR);
    const char *ver = eglQueryString(d, EGL_VERSION);
    const char *apis = eglQueryString(d, EGL_CLIENT_APIS);
    const char *exts = eglQueryString(d, EGL_EXTENSIONS);
    printf("  vendor:     %s\n", v ? v : "(null)");
    printf("  version:    %s\n", ver ? ver : "(null)");
    printf("  client apis: %s\n", apis ? apis : "(null)");
    printf("  extensions (first 400c): %.400s%s\n",
           exts ? exts : "(null)",
           (exts && strlen(exts) > 400) ? "..." : "");
    return 0;
}
