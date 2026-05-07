/*
 * shim_fs.c — open-family device-intercept wrappers + GPIO sysfs fake-fd
 *             pool + libgpiod chardev stubs.
 *
 * Hardware-related paths the game opens that don't exist on the host:
 *
 *   - open / openat / open64 / openat64 — call `intercept_device(path)`
 *     for absolute paths; if it returns a non-zero fd, that fd is the
 *     answer (fake fd for SPI / I2C / V4L2 / GPIO, real socketpair fd for
 *     DRM and UART). Otherwise fall through to real libc.
 *   - __open_2 / __openat_2 / __open64_2 / __openat64_2 — FORTIFY_SOURCE
 *     variants. Same intercept-then-fallthrough.
 *   - GPIO sysfs fake-fd pool (alloc / is_fake / free) — backs the
 *     synthetic /sys/class/gpio/* opens that shim_io.c serves.
 *   - libgpiod chardev stubs — fake the modern /dev/gpiochip* API the
 *     game imports (output-only — request_input / get_value not used).
 *
 * Cross-TU declarations are in spi_shim.h.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdarg.h>
#include <pthread.h>
#include <dlfcn.h>
#include <sys/types.h>

#include "spi_shim.h"

int open(const char *pathname, int flags, ...) {
    if (!real_open) real_open = (real_open_t)dlsym(RTLD_NEXT, "open");
    int fake = intercept_device(pathname);
    if (fake) return fake;
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    return real_open(pathname, flags, mode);
}

typedef int (*real_openat_t)(int, const char *, int, ...);
static real_openat_t real_openat = NULL;
int openat(int dirfd, const char *pathname, int flags, ...) {
    if (!real_openat) real_openat = (real_openat_t)dlsym(RTLD_NEXT, "openat");
    if (pathname && pathname[0] == '/') {
        int fake = intercept_device(pathname);
        if (fake) return fake;
    }
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    return real_openat(dirfd, pathname, flags, mode);
}

typedef int (*real_open64_t)(const char *, int, ...);
static real_open64_t real_open64 = NULL;
int open64(const char *pathname, int flags, ...) {
    if (!real_open64) real_open64 = (real_open64_t)dlsym(RTLD_NEXT, "open64");
    int fake = intercept_device(pathname);
    if (fake) return fake;
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    return real_open64(pathname, flags, mode);
}

typedef int (*real_openat64_t)(int, const char *, int, ...);
static real_openat64_t real_openat64 = NULL;
int openat64(int dirfd, const char *pathname, int flags, ...) {
    if (!real_openat64) real_openat64 = (real_openat64_t)dlsym(RTLD_NEXT, "openat64");
    if (pathname && pathname[0] == '/') {
        int fake = intercept_device(pathname);
        if (fake) return fake;
    }
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    return real_openat64(dirfd, pathname, flags, mode);
}

/* FORTIFY_SOURCE variants — glibc's INLINE_SYSCALL paths bypass the
 * public symbols above when the caller compiled with _FORTIFY_SOURCE>=2,
 * so we hook these too. Two-arg form (no mode) — FORTIFY refuses
 * O_CREAT-without-mode and routes to the public symbol instead. */
int __open_2(const char *pathname, int flags) {
    int fake = intercept_device(pathname);
    if (fake) return fake;
    static int (*real)(const char *, int) = NULL;
    if (!real) real = dlsym(RTLD_NEXT, "__open_2");
    return real(pathname, flags);
}
int __open64_2(const char *pathname, int flags) {
    int fake = intercept_device(pathname);
    if (fake) return fake;
    static int (*real)(const char *, int) = NULL;
    if (!real) real = dlsym(RTLD_NEXT, "__open64_2");
    return real(pathname, flags);
}
int __openat_2(int dirfd, const char *pathname, int flags) {
    if (pathname && pathname[0] == '/') {
        int fake = intercept_device(pathname);
        if (fake) return fake;
    }
    static int (*real)(int, const char *, int) = NULL;
    if (!real) real = dlsym(RTLD_NEXT, "__openat_2");
    return real(dirfd, pathname, flags);
}
int __openat64_2(int dirfd, const char *pathname, int flags) {
    if (pathname && pathname[0] == '/') {
        int fake = intercept_device(pathname);
        if (fake) return fake;
    }
    static int (*real)(int, const char *, int) = NULL;
    if (!real) real = dlsym(RTLD_NEXT, "__openat64_2");
    return real(dirfd, pathname, flags);
}

char     *gpio_fake_paths[FAKE_FD_GPIO_COUNT];
uint8_t   gpio_fake_read_done[FAKE_FD_GPIO_COUNT];
static pthread_mutex_t gpio_fake_lock = PTHREAD_MUTEX_INITIALIZER;

int is_gpio_sysfs_path(const char *p) {
    return p && strncmp(p, "/sys/class/gpio/", 16) == 0;
}

int alloc_gpio_fake_fd(const char *path) {
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

int is_gpio_fake_fd(int fd) {
    if (fd < FAKE_FD_GPIO_BASE || fd >= FAKE_FD_GPIO_BASE + FAKE_FD_GPIO_COUNT)
        return 0;
    return gpio_fake_paths[fd - FAKE_FD_GPIO_BASE] != NULL;
}

void free_gpio_fake_fd(int fd) {
    int idx = fd - FAKE_FD_GPIO_BASE;
    pthread_mutex_lock(&gpio_fake_lock);
    free(gpio_fake_paths[idx]);
    gpio_fake_paths[idx] = NULL;
    gpio_fake_read_done[idx] = 0;
    pthread_mutex_unlock(&gpio_fake_lock);
}


/* libgpiod chardev stubs. Stubs return opaque non-NULL handles (allocated
 * tracking structs) so pointer identity between open and close works.
 * Game uses GPIO output-only (only `gpiod_line_request_output` is imported
 * — no `_request_input` / `_get_value`).
 *
 * The opaque `struct gpiod_chip` and `struct gpiod_line` in libgpiod's
 * public headers are ABI-compatible with our plain pointer return types.
 */

struct shim_gpiod_chip { char name[32]; };
struct shim_gpiod_line { struct shim_gpiod_chip *chip; unsigned offset;
                         int direction; int value; };

void *gpiod_chip_open_by_name(const char *name) {
    struct shim_gpiod_chip *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    if (name) {
        strncpy(c->name, name, sizeof(c->name) - 1);
        c->name[sizeof(c->name) - 1] = 0;
    } else {
        strcpy(c->name, "?");
    }
    SHIM_LOG("[gpiod] chip_open_by_name(\"%s\") -> %p (fake)", c->name, (void*)c);
    return c;
}

void *gpiod_chip_get_line(void *chip, unsigned offset) {
    if (!chip) return NULL;
    struct shim_gpiod_chip *c = chip;
    struct shim_gpiod_line *l = calloc(1, sizeof(*l));
    if (!l) return NULL;
    l->chip = c;
    l->offset = offset;
    SHIM_LOG("[gpiod] chip_get_line(%s, %u) -> %p (fake)", c->name, offset, (void*)l);
    return l;
}

int gpiod_line_request_output(void *line, const char *consumer, int default_val) {
    if (!line) return -1;
    struct shim_gpiod_line *l = line;
    l->direction = 1;
    l->value = default_val;
    SHIM_LOG("[gpiod] line_request_output(%s:%u, consumer=\"%s\", default=%d) -> 0 (fake)",
             l->chip ? l->chip->name : "?", l->offset,
             consumer ? consumer : "?", default_val);
    return 0;
}

int gpiod_line_set_value(void *line, int value) {
    if (!line) return -1;
    struct shim_gpiod_line *l = line;
    l->value = value;
    static uint32_t _n = 0;
    uint32_t n = __atomic_add_fetch(&_n, 1, __ATOMIC_RELAXED);
    /* Toggle-heavy bit-bang could overwhelm the log; sample like other hooks. */
    if (n <= 8 || (n % 500) == 0) {
        SHIM_LOG("[gpiod] line_set_value #%u (%s:%u) = %d",
                 n, l->chip ? l->chip->name : "?", l->offset, value);
    }
    return 0;
}

void gpiod_line_release(void *line) {
    if (!line) return;
    struct shim_gpiod_line *l = line;
    SHIM_LOG("[gpiod] line_release(%s:%u)", l->chip ? l->chip->name : "?", l->offset);
    free(l);
}

void gpiod_chip_close(void *chip) {
    if (!chip) return;
    struct shim_gpiod_chip *c = chip;
    SHIM_LOG("[gpiod] chip_close(%s)", c->name);
    free(c);
}

