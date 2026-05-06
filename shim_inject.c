

























#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <time.h>
#include <signal.h>
#include <setjmp.h>
#include <pthread.h>

#include "spi_shim.h"
#include "shim_discovery.h"
/* ========== moved from spi_shim.c (412-678): event-sweep machinery ========== */
/* ---- Step 52: fire_event trampoline + event sweep ---------------------
 *
 * BN Step 52 revealed:
 *   fire_event @ 0x5c12a0: uint64_t(int32_t event_id, int64_t event_data)
 *     - bounds-checks event_id <= 0xce
 *     - walks linked list at data_485ea80[event_id*8]
 *     - each node: {callback_fn_ptr @ +0, priority @ +0x08, next @ +0x10}
 *     - calls callback(event_data); short-circuits on !(ret & 1)
 *
 * Trampoline pattern:
 *   fire_event_addr = game_base + 0x5c12a0
 *
 * Event sweep fires every event_id 0..0xce with event_data=0 and diffs
 * cmode state before/after. Events whose handlers dereference event_data
 * as a pointer will SEGV with fault_addr=0; we catch and recover.
 *
 * Triggered by SIGUSR2 (kept out of the main dump path so we can still
 * use SIGUSR1 for state snapshots without firing events).
 * ------------------------------------------------------------------------ */

/* Sweep globals — read by shim_sigsegv_handler (in shim_diagnostics.c) to
 * longjmp back out of a faulting sweep iteration. Externed in spi_shim.h. */
sigjmp_buf shim_event_sweep_jmp;
volatile sig_atomic_t shim_event_fire_in_progress = 0;
volatile int shim_current_sweep_event_id = -1;

static uint64_t shim_fire_event(int32_t event_id, int64_t event_data) {
    uintptr_t game_base = shim_resolve_game_base();
    if (!game_base) return (uint64_t)-1;
    typedef uint64_t (*fire_event_t)(int32_t, int64_t);
    fire_event_t fn = (fire_event_t)(game_base + 0x5c12a0);
    return fn(event_id, event_data);
}

/* Minimal cmode state snapshot. For each of the 42 cmodes captures
 * 2 u64 words: +0x308 (the field most likely to change on activation,
 * though it's subclass-reinterpreted) and +0x60 (zero at static init,
 * non-zero post-activation in some cmodes). 42×16 = 672 bytes. */
static void shim_capture_cmode_snap(uintptr_t game_base, uint64_t *out) {
    size_t N = shim_cmode_table_count;
    for (size_t i = 0; i < N; i++) {
        uintptr_t rt = game_base + shim_cmode_table[i].va;
        memcpy(&out[i*2 + 0], (void*)(rt + 0x308), 8);
        memcpy(&out[i*2 + 1], (void*)(rt + 0x60),  8);
    }
}

void shim_event_sweep(void) {
    uintptr_t game_base = shim_resolve_game_base();
    if (!game_base) {
        const char *m = "\n--- event sweep: game_base not resolved ---\n";
        ssize_t u = write(STDERR_FILENO, m, strlen(m)); (void)u;
        return;
    }

    const char *hdr =
        "\n--- event sweep: firing events 0x00..0xce with event_data=0 ---\n"
        "format:  event <id>  ret=<ret>  Δ=<#changed>[  <cmode>(+0x308: <before>-><after>)]...\n";
    ssize_t u = write(STDERR_FILENO, hdr, strlen(hdr)); (void)u;

    size_t N = shim_cmode_table_count;
    uint64_t baseline[42 * 2];
    uint64_t post[42 * 2];
    shim_capture_cmode_snap(game_base, baseline);

    int crashed_events = 0;
    int reactive_events = 0;
    int fired_ok = 0;

    int timed_out_events = 0;
    for (int ev = 0; ev <= 0xce; ev++) {
        shim_current_sweep_event_id = ev;
        shim_event_fire_in_progress = 1;

        uint64_t ret = (uint64_t)-1;
        int did_fire = 0;
        int setjmp_result;

        /* alarm(1) delivers SIGALRM to this thread (blocked elsewhere by the
         * sigmask set on the timer thread). SIGALRM handler recognises
         * in_progress and siglongjmps back with value 2. */
        alarm(1);
        setjmp_result = sigsetjmp(shim_event_sweep_jmp, 1);
        if (setjmp_result == 0) {
            ret = shim_fire_event(ev, 0);
            alarm(0);
            did_fire = 1;
            fired_ok++;
        } else if (setjmp_result == 2) {
            alarm(0);
            timed_out_events++;
        } else {
            /* setjmp_result == 1 — came from SEGV */
            alarm(0);
            crashed_events++;
        }
        shim_event_fire_in_progress = 0;

        if (did_fire) {
            shim_capture_cmode_snap(game_base, post);

            char delta_buf[1800];
            size_t delta_off = 0;
            int delta_count = 0;
            for (size_t i = 0; i < N; i++) {
                if (post[i*2 + 0] != baseline[i*2 + 0] ||
                    post[i*2 + 1] != baseline[i*2 + 1]) {
                    delta_count++;
                    if (delta_off < sizeof(delta_buf) - 128) {
                        int added = snprintf(
                            delta_buf + delta_off,
                            sizeof(delta_buf) - delta_off,
                            "  %s(+0x308:%lx->%lx)",
                            shim_cmode_table[i].name_hint,
                            (unsigned long)baseline[i*2 + 0],
                            (unsigned long)post[i*2 + 0]);
                        delta_off += (size_t)added;
                    }
                }
            }

            char line[2048];
            int ln;
            if (delta_count > 0) {
                reactive_events++;
                ln = snprintf(line, sizeof(line),
                    "  event 0x%02x ret=%lu Δ=%d %s\n",
                    ev, (unsigned long)ret, delta_count, delta_buf);
                /* Use post as new baseline for cumulative tracking */
                memcpy(baseline, post, sizeof(baseline));
            } else {
                ln = snprintf(line, sizeof(line),
                    "  event 0x%02x ret=%lu Δ=0\n",
                    ev, (unsigned long)ret);
            }
            u = write(STDERR_FILENO, line, (size_t)ln); (void)u;
        }
    }

    char sum[220];
    int sl = snprintf(sum, sizeof(sum),
        "\n--- sweep complete: fired=%d reactive=%d crashed=%d timeouts=%d (of 207) ---\n",
        fired_ok, reactive_events, crashed_events, timed_out_events);
    u = write(STDERR_FILENO, sum, (size_t)sl); (void)u;
}






static void *shim_discovery_init_thread(void *arg) {
    (void)arg;
    sleep(2);
    shim_discovery_init();

    return NULL;
}

/* ========== moved from spi_shim.c (836-977): alarm handler + timer threads ========== */
/* Step 52: SIGALRM handler used to break out of hung event handlers during
 * the sweep. Installed in the constructor; blocked on all threads except
 * the sweep timer thread so alarm() delivers to the right place. */
static void shim_event_sweep_alarm_handler(int sig, siginfo_t *info, void *uc) {
    (void)info; (void)uc;
    if (shim_event_fire_in_progress) {
        char msg[160];
        int n = snprintf(msg, sizeof(msg),
            "\n  [sweep] event 0x%02x TIMEOUT (sig=%d) — recovering\n",
            shim_current_sweep_event_id, sig);
        ssize_t u = write(STDERR_FILENO, msg, (size_t)n); (void)u;
        shim_event_fire_in_progress = 0;
        siglongjmp(shim_event_sweep_jmp, 2);
    }
    /* else: unexpected SIGALRM outside the sweep — just ignore */
}



/* ---- Public entry point called from shim_install_crash_handler ----
 * Installs the SIGALRM handler used by shim_event_sweep's alarm-per-event
 * timeout, blocks SIGALRM on the main thread (so threads inherit the
 * block; sweep-callers unblock for themselves), then spawns the always-on
 * discovery init thread. */
void shim_inject_start(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = shim_event_sweep_alarm_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);

    sigset_t block_alarm;
    sigemptyset(&block_alarm);
    sigaddset(&block_alarm, SIGALRM);
    pthread_sigmask(SIG_BLOCK, &block_alarm, NULL);

    pthread_t t;
    if (pthread_create(&t, NULL, shim_discovery_init_thread, NULL) == 0)
        pthread_detach(t);
}
