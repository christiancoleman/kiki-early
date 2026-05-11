/*
 * shim_devbus.c — switch / driver / lamp observability + control bridge.
 *
 * Phase 1: switches, observability only (read-only). AF_UNIX abstract
 * listener at @pokemon_spike3_devbus. Watcher thread polls the runtime
 * switch state at 1 ms cadence; on detected change, an event is pushed
 * into every connected client's SPSC ring. Per-client writer thread
 * drains the ring and serializes line-delimited JSON to the socket.
 *
 * Architecture is structured so the watcher (the switch event source)
 * is swappable behind the same devbus_emit() entry point — a future
 * hook-based source would call the same function. See PROTOCOL §12 for the
 * current wire format and JOURNAL Step 102 for future-work notes.
 *
 * Dependencies via spi_shim.h: shim_resolve_game_base(),
 * shim_unix_abstract_addr(), SHIM_LOG.
 */

#define _GNU_SOURCE
#include "spi_shim.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>

#define DEVBUS_SOCKET_NAME    "pokemon_spike3_devbus"
#define DEVBUS_PROTO_VERSION  1
#define DEVBUS_TICK_US        1000
#define DEVBUS_RING_SIZE      1024
#define DEVBUS_MAX_CLIENTS    4
#define DEVBUS_FLAP_WINDOW_NS 1000000000ULL
#define DEVBUS_BOOT_DELAY_S   5

typedef struct {
    uint8_t  state;
    uint16_t edge_counter;
    uint8_t  channel;
    uint16_t bit_idx;
    uint8_t  valid;
    uint64_t flap_window_start_ns;
    uint16_t flap_count_this_window;
    uint16_t flap_count_last_window;
} sw_shadow_t;

typedef struct {
    uint8_t  sw_id;
    uint8_t  state;
    uint16_t edge_counter;
    uint16_t flap_count;
    uint8_t  pad[2];
    uint64_t timestamp_ns;
} sw_event_t;

typedef struct devbus_client {
    int       fd;
    pthread_t thread;

    pthread_mutex_t ring_lock;
    pthread_cond_t  ring_cv;
    sw_event_t      events[DEVBUS_RING_SIZE];
    uint32_t        head;
    uint32_t        tail;
    uint32_t        drops;

    _Atomic uint64_t seq;
    _Atomic int      active;

    struct devbus_client *next;
} devbus_client_t;

static pthread_mutex_t  g_clients_lock   = PTHREAD_MUTEX_INITIALIZER;
static devbus_client_t *g_clients_head   = NULL;
static int              g_listen_fd      = -1;
static sw_shadow_t      g_shadow[SHIM_SWITCH_MAX_RUNTIME_ID + 1];
static _Atomic int      g_switch_count   = 0;
static _Atomic int      g_shadow_seeded  = 0;

static uint64_t devbus_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int devbus_write_all(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

static void devbus_emit(const sw_event_t *evt) {
    pthread_mutex_lock(&g_clients_lock);
    for (devbus_client_t *cli = g_clients_head; cli; cli = cli->next) {
        if (!atomic_load_explicit(&cli->active, memory_order_acquire)) continue;

        pthread_mutex_lock(&cli->ring_lock);
        if (cli->head - cli->tail >= DEVBUS_RING_SIZE) {
            cli->tail++;
            cli->drops++;
        }
        cli->events[cli->head % DEVBUS_RING_SIZE] = *evt;
        cli->head++;
        pthread_cond_signal(&cli->ring_cv);
        pthread_mutex_unlock(&cli->ring_lock);
    }
    pthread_mutex_unlock(&g_clients_lock);
}

static int devbus_scan_once(uintptr_t game_base, int seeding) {
    uint8_t *arr = shim_switch_array(game_base);
    if (!arr) return 0;

    uint64_t now_ns = devbus_now_ns();
    int discovered = 0;

    for (int sw_id = 1; sw_id <= SHIM_SWITCH_MAX_RUNTIME_ID; sw_id++) {
        uint8_t *e = shim_switch_entry(arr, sw_id);
        uint16_t recorded = *(uint16_t *)(e + SHIM_SWITCH_ENTRY_ID_OFF);
        if (recorded != (uint16_t)sw_id) continue;

        uint16_t bit_idx  = *(uint16_t *)(e + SHIM_SWITCH_ENTRY_BIT_IDX_OFF);
        uint8_t  channel  = *(uint8_t  *)(e + SHIM_SWITCH_ENTRY_CHANNEL_OFF);
        uint16_t edge     = *(uint16_t *)(e + SHIM_SWITCH_ENTRY_EDGE_OFF);
        uint8_t  state    = *(uint8_t  *)(e + SHIM_SWITCH_ENTRY_STATE_OFF);

        sw_shadow_t *sh = &g_shadow[sw_id];

        if (!sh->valid) {
            sh->state = state;
            sh->edge_counter = edge;
            sh->channel = channel;
            sh->bit_idx = bit_idx;
            sh->flap_window_start_ns = now_ns;
            sh->flap_count_this_window = 0;
            sh->flap_count_last_window = 0;
            sh->valid = 1;
            atomic_fetch_add_explicit(&g_switch_count, 1, memory_order_relaxed);
            discovered++;
            continue;
        }

        if (now_ns - sh->flap_window_start_ns > DEVBUS_FLAP_WINDOW_NS) {
            sh->flap_count_last_window = sh->flap_count_this_window;
            sh->flap_count_this_window = 0;
            sh->flap_window_start_ns = now_ns;
        }

        if (sh->state != state || sh->edge_counter != edge) {
            sh->flap_count_this_window++;
            sh->state = state;
            sh->edge_counter = edge;

            if (!seeding) {
                sw_event_t evt = {
                    .sw_id        = (uint8_t)sw_id,
                    .state        = state,
                    .edge_counter = edge,
                    .flap_count   = sh->flap_count_last_window,
                    .timestamp_ns = now_ns,
                };
                devbus_emit(&evt);
            }
        }
    }
    return discovered;
}

static void *devbus_watcher_thread(void *arg) {
    (void)arg;
    sleep(DEVBUS_BOOT_DELAY_S);

    uintptr_t game_base = shim_resolve_game_base();
    if (!game_base) {
        SHIM_LOG("[devbus] watcher: game_base unresolved; abort");
        return NULL;
    }

    while (atomic_load(&g_shadow_seeded) == 0) {
        int n = devbus_scan_once(game_base, 1);
        if (n > 0) {
            SHIM_LOG("[devbus] watcher seeded %d switch%s",
                     n, n == 1 ? "" : "es");
        }
        if (atomic_load(&g_switch_count) > 0) {
            atomic_store(&g_shadow_seeded, 1);
            break;
        }
        struct timespec wait_ts = {0, 100 * 1000000};
        nanosleep(&wait_ts, NULL);
    }

    SHIM_LOG("[devbus] watcher running (tick=%dus, switches=%d)",
             DEVBUS_TICK_US, atomic_load(&g_switch_count));

    struct timespec sleep_ts = {0, DEVBUS_TICK_US * 1000};
    while (1) {
        nanosleep(&sleep_ts, NULL);
        devbus_scan_once(game_base, 0);
    }
    return NULL;
}

static int devbus_send_hello(devbus_client_t *cli) {
    char buf[256];
    int n = snprintf(buf, sizeof(buf),
                     "{\"v\":%d,\"seq\":%llu,\"event\":\"hello\","
                     "\"ts_ns\":%llu,\"tick_us\":%d,\"switch_count\":%d}\n",
                     DEVBUS_PROTO_VERSION,
                     (unsigned long long)atomic_fetch_add(&cli->seq, 1),
                     (unsigned long long)devbus_now_ns(),
                     DEVBUS_TICK_US,
                     atomic_load(&g_switch_count));
    if (n < 0 || n >= (int)sizeof(buf)) return -1;
    return devbus_write_all(cli->fd, buf, (size_t)n);
}

static int devbus_send_snapshot(devbus_client_t *cli) {
    uintptr_t game_base = shim_resolve_game_base();
    uint8_t channel_bytes[SHIM_SWITCH_CHANNEL_COUNT][SHIM_SWITCH_CHANNEL_BYTES];
    memset(channel_bytes, 0, sizeof(channel_bytes));
    if (game_base) {
        for (uint8_t channel = 0; channel < SHIM_SWITCH_CHANNEL_COUNT; channel++) {
            if (channel == 0) {
                shim_switch_make_channel0_idle(channel_bytes[channel]);
            } else {
                shim_switch_copy_channel_idle(game_base, channel, channel_bytes[channel]);
            }
            shim_switch_hold_apply(channel, channel_bytes[channel]);
        }
    }

    char head[128];
    int n = snprintf(head, sizeof(head),
                     "{\"v\":%d,\"seq\":%llu,\"event\":\"snapshot\","
                     "\"ts_ns\":%llu,\"channels\":[",
                     DEVBUS_PROTO_VERSION,
                     (unsigned long long)atomic_fetch_add(&cli->seq, 1),
                     (unsigned long long)devbus_now_ns());
    if (n < 0 || devbus_write_all(cli->fd, head, (size_t)n) < 0) return -1;

    int first = 1;
    char entry[256];
    for (int channel = 0; channel < SHIM_SWITCH_CHANNEL_COUNT; channel++) {
        n = snprintf(entry, sizeof(entry),
                     "%s{\"channel\":%d,\"bytes\":\""
                     "%02x %02x %02x %02x %02x %02x %02x %02x\"}",
                     first ? "" : ",",
                     channel,
                     channel_bytes[channel][0], channel_bytes[channel][1],
                     channel_bytes[channel][2], channel_bytes[channel][3],
                     channel_bytes[channel][4], channel_bytes[channel][5],
                     channel_bytes[channel][6], channel_bytes[channel][7]);
        if (n < 0 || n >= (int)sizeof(entry)) return -1;
        if (devbus_write_all(cli->fd, entry, (size_t)n) < 0) return -1;
        first = 0;
    }

    if (devbus_write_all(cli->fd, "],\"switches\":[", 14) < 0) return -1;

    first = 1;
    for (int sw_id = 1; sw_id <= SHIM_SWITCH_MAX_RUNTIME_ID; sw_id++) {
        sw_shadow_t *sh = &g_shadow[sw_id];
        if (!sh->valid) continue;
        uint8_t raw_bit = 0;
        if (sh->channel < SHIM_SWITCH_CHANNEL_COUNT &&
            sh->bit_idx < SHIM_SWITCH_BITS_PER_CHANNEL) {
            uint8_t byte_off = (uint8_t)(sh->bit_idx >> 3);
            uint8_t bit_mask = (uint8_t)(1u << (sh->bit_idx & 7));
            raw_bit = (channel_bytes[sh->channel][byte_off] & bit_mask) ? 1 : 0;
        }
        n = snprintf(entry, sizeof(entry),
                     "%s{\"sw_id\":%d,\"state\":%u,\"edge\":%u,"
                     "\"channel\":%u,\"bit_idx\":%u,\"raw\":%u,\"flap\":%u}",
                     first ? "" : ",",
                     sw_id, sh->state, sh->edge_counter,
                     sh->channel, sh->bit_idx, raw_bit,
                     sh->flap_count_last_window);
        if (n < 0 || n >= (int)sizeof(entry)) return -1;
        if (devbus_write_all(cli->fd, entry, (size_t)n) < 0) return -1;
        first = 0;
    }
    return devbus_write_all(cli->fd, "]}\n", 3);
}

static int devbus_send_change(devbus_client_t *cli, const sw_event_t *evt) {
    char buf[256];
    int n = snprintf(buf, sizeof(buf),
                     "{\"v\":%d,\"seq\":%llu,\"event\":\"switch_change\","
                     "\"ts_ns\":%llu,\"sw_id\":%d,\"state\":%u,"
                     "\"edge\":%u,\"flap\":%u}\n",
                     DEVBUS_PROTO_VERSION,
                     (unsigned long long)atomic_fetch_add(&cli->seq, 1),
                     (unsigned long long)evt->timestamp_ns,
                     evt->sw_id, evt->state,
                     evt->edge_counter, evt->flap_count);
    if (n < 0 || n >= (int)sizeof(buf)) return -1;
    return devbus_write_all(cli->fd, buf, (size_t)n);
}

static void devbus_unlink_client(devbus_client_t *cli) {
    pthread_mutex_lock(&g_clients_lock);
    devbus_client_t **pp = &g_clients_head;
    while (*pp) {
        if (*pp == cli) { *pp = cli->next; break; }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&g_clients_lock);
}

static void *devbus_writer_thread(void *arg) {
    devbus_client_t *cli = arg;
    SHIM_LOG("[devbus] client connected fd=%d", cli->fd);

    while (!atomic_load_explicit(&g_shadow_seeded, memory_order_acquire) &&
           atomic_load_explicit(&cli->active, memory_order_acquire)) {
        struct timespec ts = {0, 50 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    if (!atomic_load_explicit(&cli->active, memory_order_acquire))
        goto disconnect;

    if (devbus_send_hello(cli) < 0)    goto disconnect;
    if (devbus_send_snapshot(cli) < 0) goto disconnect;

    while (atomic_load_explicit(&cli->active, memory_order_acquire)) {
        sw_event_t evt;

        pthread_mutex_lock(&cli->ring_lock);
        while (cli->head == cli->tail &&
               atomic_load_explicit(&cli->active, memory_order_acquire)) {
            struct timespec deadline;
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_sec += 1;
            pthread_cond_timedwait(&cli->ring_cv, &cli->ring_lock, &deadline);
        }
        if (!atomic_load_explicit(&cli->active, memory_order_acquire)) {
            pthread_mutex_unlock(&cli->ring_lock);
            break;
        }
        evt = cli->events[cli->tail % DEVBUS_RING_SIZE];
        cli->tail++;
        pthread_mutex_unlock(&cli->ring_lock);

        if (devbus_send_change(cli, &evt) < 0) goto disconnect;
    }

disconnect:
    {
        pthread_mutex_lock(&cli->ring_lock);
        uint32_t drops = cli->drops;
        pthread_mutex_unlock(&cli->ring_lock);
        SHIM_LOG("[devbus] client fd=%d disconnect (drops=%u)",
                 cli->fd, drops);
    }
    atomic_store(&cli->active, 0);
    devbus_unlink_client(cli);
    close(cli->fd);
    pthread_mutex_destroy(&cli->ring_lock);
    pthread_cond_destroy(&cli->ring_cv);
    free(cli);
    return NULL;
}

static int devbus_count_clients_locked(void) {
    int count = 0;
    for (devbus_client_t *c = g_clients_head; c; c = c->next) count++;
    return count;
}

static void *devbus_listener_thread(void *arg) {
    (void)arg;
    while (1) {
        int cli_fd = accept(g_listen_fd, NULL, NULL);
        if (cli_fd < 0) {
            if (errno == EINTR) continue;
            SHIM_LOG("[devbus] accept() failed: %s — listener exit",
                     strerror(errno));
            return NULL;
        }

        pthread_mutex_lock(&g_clients_lock);
        int count = devbus_count_clients_locked();
        pthread_mutex_unlock(&g_clients_lock);
        if (count >= DEVBUS_MAX_CLIENTS) {
            const char *err =
                "{\"v\":1,\"event\":\"err\",\"reason\":\"too many clients\"}\n";
            (void)devbus_write_all(cli_fd, err, strlen(err));
            close(cli_fd);
            continue;
        }

        devbus_client_t *cli = calloc(1, sizeof(*cli));
        if (!cli) { close(cli_fd); continue; }
        cli->fd = cli_fd;
        pthread_mutex_init(&cli->ring_lock, NULL);
        pthread_cond_init(&cli->ring_cv, NULL);
        atomic_store(&cli->active, 1);

        pthread_mutex_lock(&g_clients_lock);
        cli->next = g_clients_head;
        g_clients_head = cli;
        pthread_mutex_unlock(&g_clients_lock);

        if (pthread_create(&cli->thread, NULL,
                           devbus_writer_thread, cli) != 0) {
            SHIM_LOG("[devbus] writer thread spawn failed: %s",
                     strerror(errno));
            devbus_unlink_client(cli);
            close(cli_fd);
            pthread_mutex_destroy(&cli->ring_lock);
            pthread_cond_destroy(&cli->ring_cv);
            free(cli);
            continue;
        }
        pthread_detach(cli->thread);
    }
}

static int devbus_create_listener(const char *abstract_name) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        SHIM_LOG("[devbus] socket(AF_UNIX) failed: %s", strerror(errno));
        return -1;
    }
    struct sockaddr_un addr;
    socklen_t addrlen = shim_unix_abstract_addr(&addr, abstract_name);
    if (bind(fd, (struct sockaddr *)&addr, addrlen) < 0 ||
        listen(fd, DEVBUS_MAX_CLIENTS) < 0) {
        SHIM_LOG("[devbus] bind/listen(@%s) failed: %s",
                 abstract_name, strerror(errno));
        close(fd);
        return -1;
    }
    SHIM_LOG("[devbus] listening on @%s (abstract AF_UNIX)", abstract_name);
    return fd;
}

void shim_devbus_init(void) {
    const char *gate = getenv("SPIKE3_DEVBUS");
    if (gate && gate[0] == '0') {
        SHIM_LOG("[devbus] SPIKE3_DEVBUS=0 — bridge disabled");
        return;
    }

    g_listen_fd = devbus_create_listener(DEVBUS_SOCKET_NAME);
    if (g_listen_fd < 0) return;

    pthread_t listener, watcher;
    if (pthread_create(&watcher, NULL, devbus_watcher_thread, NULL) != 0) {
        SHIM_LOG("[devbus] watcher spawn failed: %s", strerror(errno));
        return;
    }
    pthread_detach(watcher);

    if (pthread_create(&listener, NULL, devbus_listener_thread, NULL) != 0) {
        SHIM_LOG("[devbus] listener spawn failed: %s", strerror(errno));
        return;
    }
    pthread_detach(listener);
}
