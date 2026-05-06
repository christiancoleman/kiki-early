/*
 * shim_fs.c — GPIO sysfs fake-fd pool + libgpiod chardev stubs.
 *
 * Originally also held the LD_PRELOAD path-remap layer (`remap_path`) and
 * a comprehensive set of open/stat/access/etc. wrappers that prefixed
 * absolute paths with $SYSROOT. That layer became redundant with the
 * bind-mount approach (`_emulation/scripts/setup_bind_mounts.sh` — see
 * PROTOCOL §2 "One-time setup: bind-mount the sysroot"): the kernel
 * resolves /games, /connectivity, /data via mount table, so no userspace
 * path-rewrite is needed. The FS hooks were removed in the cleanup pass
 * after Step 80's POC validation.
 *
 * What remains is hardware-related and unchanged by mount-table magic:
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
#include <pthread.h>

#include "spi_shim.h"

/* ========== moved from spi_shim.c: GPIO fake-fd pool state ==========
 * FAKE_FD_GPIO_BASE / FAKE_FD_GPIO_COUNT live in spi_shim.h so the I/O
 * hooks (still in spi_shim.c) can range-check fds against this pool.
 * The arrays are externed in the header — non-static here. */
char     *gpio_fake_paths[FAKE_FD_GPIO_COUNT];
uint8_t   gpio_fake_read_done[FAKE_FD_GPIO_COUNT];
static pthread_mutex_t gpio_fake_lock = PTHREAD_MUTEX_INITIALIZER;

/* ========== moved from spi_shim.c (1441-1474): GPIO sysfs helpers (is_gpio_sysfs_path / alloc / is_gpio_fake / free) ========== */
/* ---- GPIO sysfs fake-fd pool ---- */

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


/* ========== libgpiod chardev stubs (2026-04-24) ==========
 * The game imports six symbols from libgpiod (the modern /dev/gpiochipN
 * character-device API). Known-gap: our shim only faked the legacy
 * /sys/class/gpio/ sysfs interface, so calls through libgpiod silently
 * fell through to real libgpiod, which then opened /dev/gpiochip* — that
 * doesn't exist on this host, so the opens errored and the game's
 * chip-select / interrupt-line logic never got its handshake.
 *
 * Stubs return opaque non-NULL handles (dynamically allocated tracking
 * structs) so pointer identity between open and close works naturally.
 * The game uses GPIO as output-only (only `gpiod_line_request_output` is
 * imported — no `_request_input` / `_get_value`) so we log value writes
 * but don't have to invent a read semantic.
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

