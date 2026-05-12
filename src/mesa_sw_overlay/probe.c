/* Gate-check: dlopen the megadriver and look up swrast's DRI extensions symbol.
 * If this succeeds end-to-end, libgallium + LLVM + xcb/wayland/etc all loaded. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *path = argv[1] ? argv[1] : "/swrast_dri.so";
    fprintf(stderr, "[probe] dlopen(\"%s\") ...\n", path);
    void *h = dlopen(path, RTLD_LAZY | RTLD_LOCAL);
    if (!h) {
        fprintf(stderr, "[probe] FAIL: %s\n", dlerror());
        return 1;
    }
    fprintf(stderr, "[probe] dlopen OK: %p\n", h);

    const char *sym = "__driDriverGetExtensions_swrast";
    fprintf(stderr, "[probe] dlsym(\"%s\") ...\n", sym);
    void *fn = dlsym(h, sym);
    if (!fn) {
        fprintf(stderr, "[probe] dlsym FAIL: %s\n", dlerror());
        return 2;
    }
    fprintf(stderr, "[probe] dlsym OK: %p\n", fn);
    return 0;
}
