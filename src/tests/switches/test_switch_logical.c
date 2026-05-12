/*
 * test_switch_logical.c — the one "logical switch state" formula, pinned.
 *
 * shim_switch_logical_from_raw(raw, invert) (a static inline in spi_shim.h) is
 * the single definition of the game's logical switch state — read_switch_state
 * @ 0x650850's rule: the raw +0x28 byte if the config invert flag (config_ptr
 * +0x30 bit 2) is set, otherwise raw ^ 1 (low raw = "active" in the UI). Every
 * shim path that reports a switch state routes through it or through the live
 * read_switch_state itself: shim_switch_read_logical trampolines into the game's
 * function; the devbus watcher computes `logical` with this inline; and
 * shim_switch_dump logs the live read_switch_state result AND asserts it folds
 * to this. This test pins the truth table so a refactor can't quietly change
 * "is this switch on?".
 *
 * Host-compiled by ../run_tests.sh (-I points at _emulation/src/). No game,
 * no sysroot — pure logic.
 */
#include <stdio.h>
#include <stdint.h>

#include "spi_shim.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...) do {                                              \
    if (cond) { g_pass++; }                                                \
    else { g_fail++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

int main(void) {
    /* non-inverted (the common case): logical = raw ^ 1 */
    CHECK(shim_switch_logical_from_raw(0, 0) == 1, "raw=0 invert=0 -> 1 (low raw == active)");
    CHECK(shim_switch_logical_from_raw(1, 0) == 0, "raw=1 invert=0 -> 0");
    /* inverted (config flag 0x04 set): logical = raw, verbatim */
    CHECK(shim_switch_logical_from_raw(0, 1) == 0, "raw=0 invert=1 -> 0");
    CHECK(shim_switch_logical_from_raw(1, 1) == 1, "raw=1 invert=1 -> 1");
    /* invert is a boolean test on the caller's side; any nonzero behaves the same */
    CHECK(shim_switch_logical_from_raw(0, 7) == 0, "raw=0 invert=7 -> 0 (nonzero invert == set)");
    CHECK(shim_switch_logical_from_raw(1, 7) == 1, "raw=1 invert=7 -> 1");

    printf("\n[test_switch_logical] %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
