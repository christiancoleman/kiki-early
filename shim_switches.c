/*
 * shim_switches.c — shared switch-table and held-input helpers.
 */

#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "spi_shim.h"

static pthread_mutex_t shim_switch_hold_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint8_t shim_switch_hold_mask[SHIM_SWITCH_CHANNEL_COUNT][SHIM_SWITCH_CHANNEL_BYTES];
static uint8_t shim_switch_stable_idle[SHIM_SWITCH_CHANNEL_COUNT][SHIM_SWITCH_CHANNEL_BYTES];
static uint8_t shim_switch_stable_idle_valid[SHIM_SWITCH_CHANNEL_COUNT];
static int shim_coin_door_open_override = -1;

static int env_enabled(const char *name, int default_value) {
    const char *v = getenv(name);
    if (!v || !v[0]) {
        return default_value;
    }
    if (strcmp(v, "0") == 0 || strcmp(v, "false") == 0 ||
        strcmp(v, "off") == 0 || strcmp(v, "no") == 0) {
        return 0;
    }
    return 1;
}

static void switch_set_bit(uint8_t buf[SHIM_SWITCH_CHANNEL_BYTES],
                           uint16_t bit_idx,
                           int active) {
    if (!buf || bit_idx >= SHIM_SWITCH_BITS_PER_CHANNEL) {
        return;
    }
    uint8_t byte_off = (uint8_t)(bit_idx >> 3);
    uint8_t bit_mask = (uint8_t)(1u << (bit_idx & 7));
    if (active) {
        buf[byte_off] |= bit_mask;
    } else {
        buf[byte_off] &= (uint8_t)~bit_mask;
    }
}

static void shim_switch_apply_expected_idle(uint8_t channel_id,
                                            uint8_t buf[SHIM_SWITCH_CHANNEL_BYTES]) {
    if (!buf) {
        return;
    }
    if (!env_enabled("SHIM_EXPECTED_SWITCH_IDLE", 0)) {
        return;
    }

    if (channel_id == 0) {
        static const uint8_t bits[] = {8, 9, 10, 11, 16, 17, 18, 19, 20, 21, 22, 23};
        for (size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); i++) {
            switch_set_bit(buf, bits[i], 0);
        }
    } else if (channel_id == 1) {
        static const uint8_t bits[] = {2, 7, 8, 11, 12, 14, 16, 17, 18, 19, 20, 21, 22};
        for (size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); i++) {
            switch_set_bit(buf, bits[i], 0);
        }
    } else if (channel_id == 8) {
        static const uint8_t bits[] = {
            0, 1, 2, 3, 4, 5, 8, 9, 10, 11, 12, 16, 17, 18, 19, 20,
            24, 25, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38
        };
        for (size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); i++) {
            switch_set_bit(buf, bits[i], 0);
        }
    } else if (channel_id == 9) {
        static const uint8_t bits[] = {
            0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 14, 15, 17,
            18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31
        };
        for (size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); i++) {
            switch_set_bit(buf, bits[i], 0);
        }
    }
}

static void shim_switch_apply_default_idle_corrections(uint8_t channel_id,
                                                       uint8_t buf[SHIM_SWITCH_CHANNEL_BYTES]) {
    if (!buf) {
        return;
    }

    if (channel_id == 1) {
        /* Coin inputs rest high. The game-derived channel-1 image can settle
         * with 1-SW-16..21 low, which makes the switch test show manual
         * switches 96, 97, 98, 99, 100, and 102 as already active. */
        static const uint8_t coin_bits[] = {16, 17, 18, 19, 20, 21};
        for (size_t i = 0; i < sizeof(coin_bits) / sizeof(coin_bits[0]); i++) {
            switch_set_bit(buf, coin_bits[i], 1);
        }
        /* Channel 1 is also synthesized by FUN_00674b90 from CPU-SPI byte 3.
         * With our default channel-0 idle (byte 3 = 0xf8, low bits clear),
         * that path clears these bits before calling sub_651610(1, ...).
         * Match that derived idle here so nodebus switch-poll and CPU-SPI
         * derived updates do not ping-pong channel-1 edges. */
        static const uint8_t derived_low_bits[] = {13};
        for (size_t i = 0; i < sizeof(derived_low_bits) / sizeof(derived_low_bits[0]); i++) {
            switch_set_bit(buf, derived_low_bits[i], 0);
        }
    } else if (channel_id == 8) {
        /* These normally-resting closed switches appear active in the
         * cabinet's Switch Test. In this game build their config is not
         * inverted, so raw/state low is the UI-active value. */
        static const uint8_t active_low_bits[] = {
            16, /* 11 Left Flipper EOS */
            31, /* 12 Right Flipper EOS */
            32, /* 15 Trough 6 */
            33, /* 16 Trough 5 */
            34, /* 17 Trough 4 */
            35, /* 18 Trough 3 */
            36, /* 19 Trough 2 */
            37, /* 20 Trough 1 */
        };
        for (size_t i = 0; i < sizeof(active_low_bits) / sizeof(active_low_bits[0]); i++) {
            switch_set_bit(buf, active_low_bits[i], 0);
        }
    } else if (channel_id == 9) {
        static const uint8_t active_low_bits[] = {
            4,  /* 71 Captive Ball-Lower */
            5,  /* 72 Captive Ball-Upper */
            7,  /* 78 Spinner Opto */
            25, /* 61 Meowth Down */
        };
        for (size_t i = 0; i < sizeof(active_low_bits) / sizeof(active_low_bits[0]); i++) {
            switch_set_bit(buf, active_low_bits[i], 0);
        }
    }
}

void shim_switch_copy_channel_idle(uintptr_t game_base,
                                   uint8_t channel_id,
                                   uint8_t out[SHIM_SWITCH_CHANNEL_BYTES]) {
    uintptr_t channel_idle = game_base + SHIM_GAME_VA_CHANNEL_IDLE_BASE
                           + (uintptr_t)channel_id * SHIM_GAME_CHANNEL_IDLE_STRIDE;
    memcpy(out, (const void *)channel_idle, SHIM_SWITCH_CHANNEL_BYTES);
    shim_switch_apply_default_idle_corrections(channel_id, out);
    shim_switch_apply_expected_idle(channel_id, out);
}

void shim_switch_copy_channel_stable_idle(uintptr_t game_base,
                                          uint8_t channel_id,
                                          uint8_t out[SHIM_SWITCH_CHANNEL_BYTES]) {
    if (!out || channel_id >= SHIM_SWITCH_CHANNEL_COUNT) {
        return;
    }
    if (channel_id == 0) {
        shim_switch_make_channel0_idle(out);
        return;
    }

    pthread_mutex_lock(&shim_switch_hold_mutex);
    if (shim_switch_stable_idle_valid[channel_id]) {
        memcpy(out, shim_switch_stable_idle[channel_id], SHIM_SWITCH_CHANNEL_BYTES);
        pthread_mutex_unlock(&shim_switch_hold_mutex);
        return;
    }
    pthread_mutex_unlock(&shim_switch_hold_mutex);

    uint8_t candidate[SHIM_SWITCH_CHANNEL_BYTES] = {0};
    shim_switch_copy_channel_idle(game_base, channel_id, candidate);

    int nonzero = 0;
    for (int i = 0; i < SHIM_SWITCH_CHANNEL_BYTES; i++) {
        nonzero |= candidate[i];
    }

    pthread_mutex_lock(&shim_switch_hold_mutex);
    if (!shim_switch_stable_idle_valid[channel_id] && nonzero) {
        memcpy(shim_switch_stable_idle[channel_id], candidate, SHIM_SWITCH_CHANNEL_BYTES);
        shim_switch_stable_idle_valid[channel_id] = 1;
    }
    if (shim_switch_stable_idle_valid[channel_id]) {
        memcpy(out, shim_switch_stable_idle[channel_id], SHIM_SWITCH_CHANNEL_BYTES);
    } else {
        memcpy(out, candidate, SHIM_SWITCH_CHANNEL_BYTES);
    }
    pthread_mutex_unlock(&shim_switch_hold_mutex);
}

void shim_switch_make_channel0_idle(uint8_t out[SHIM_SWITCH_CHANNEL_BYTES]) {
    static const uint8_t idle[SHIM_SWITCH_CHANNEL_BYTES] =
        { 0x00, 0xff, 0xff, 0xf8, 0xff, 0xff, 0xff, 0xff };
    memcpy(out, idle, sizeof(idle));
    shim_switch_apply_expected_idle(0, out);

    const char *coin_door = getenv("SPIKE3_COIN_DOOR_INTERLOCK");
    if (!coin_door || !coin_door[0] ||
        strcmp(coin_door, "closed") == 0 ||
        strcmp(coin_door, "restored") == 0 ||
        strcmp(coin_door, "1") == 0) {
        out[SHIM_COIN_DOOR_INTERLOCK_BYTE] &= (uint8_t)~SHIM_COIN_DOOR_INTERLOCK_MASK;
    } else if (strcmp(coin_door, "open") == 0 || strcmp(coin_door, "0") == 0) {
        out[SHIM_COIN_DOOR_INTERLOCK_BYTE] |= SHIM_COIN_DOOR_INTERLOCK_MASK;
    }

    pthread_mutex_lock(&shim_switch_hold_mutex);
    int override = shim_coin_door_open_override;
    pthread_mutex_unlock(&shim_switch_hold_mutex);
    if (override == 1) {
        out[SHIM_COIN_DOOR_INTERLOCK_BYTE] |= SHIM_COIN_DOOR_INTERLOCK_MASK;
    } else if (override == 0) {
        out[SHIM_COIN_DOOR_INTERLOCK_BYTE] &= (uint8_t)~SHIM_COIN_DOOR_INTERLOCK_MASK;
    }
}

uint8_t *shim_switch_array(uintptr_t game_base) {
    if (!game_base) {
        return NULL;
    }
    uint8_t **arr_pp = (uint8_t **)(uintptr_t)(game_base + SHIM_GAME_VA_SWITCH_ARRAY_PTR);
    return *arr_pp;
}

uint8_t *shim_switch_entry(uint8_t *switch_array, int sw_id) {
    if (!switch_array) {
        return NULL;
    }
    if (sw_id < 1 || sw_id > SHIM_SWITCH_MAX_RUNTIME_ID) {
        return NULL;
    }
    return switch_array + (size_t)sw_id * SHIM_SWITCH_ENTRY_SIZE;
}

void shim_switch_hold_set(uint8_t channel_id, uint16_t bit_idx, int held) {
    if (channel_id >= SHIM_SWITCH_CHANNEL_COUNT ||
        bit_idx >= SHIM_SWITCH_BITS_PER_CHANNEL) {
        return;
    }
    uint8_t byte_off = (uint8_t)(bit_idx >> 3);
    uint8_t bit_mask = (uint8_t)(1u << (bit_idx & 7));

    pthread_mutex_lock(&shim_switch_hold_mutex);
    if (held) {
        shim_switch_hold_mask[channel_id][byte_off] |= bit_mask;
    } else {
        shim_switch_hold_mask[channel_id][byte_off] &= (uint8_t)~bit_mask;
    }
    pthread_mutex_unlock(&shim_switch_hold_mutex);
}

void shim_switch_set_coin_door_open(int open) {
    pthread_mutex_lock(&shim_switch_hold_mutex);
    shim_coin_door_open_override = open ? 1 : 0;
    pthread_mutex_unlock(&shim_switch_hold_mutex);
}

void shim_switch_hold_apply(uint8_t channel_id, uint8_t buf[SHIM_SWITCH_CHANNEL_BYTES]) {
    if (!buf || channel_id >= SHIM_SWITCH_CHANNEL_COUNT) {
        return;
    }

    uint8_t mask[SHIM_SWITCH_CHANNEL_BYTES];
    pthread_mutex_lock(&shim_switch_hold_mutex);
    memcpy(mask, shim_switch_hold_mask[channel_id], sizeof(mask));
    pthread_mutex_unlock(&shim_switch_hold_mutex);

    for (int i = 0; i < SHIM_SWITCH_CHANNEL_BYTES; i++) {
        buf[i] ^= mask[i];
    }
}
