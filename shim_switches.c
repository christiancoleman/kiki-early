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

void shim_switch_copy_channel_idle(uintptr_t game_base,
                                   uint8_t channel_id,
                                   uint8_t out[SHIM_SWITCH_CHANNEL_BYTES]) {
    uintptr_t channel_idle = game_base + SHIM_GAME_VA_CHANNEL_IDLE_BASE
                           + (uintptr_t)channel_id * SHIM_GAME_CHANNEL_IDLE_STRIDE;
    memcpy(out, (const void *)channel_idle, SHIM_SWITCH_CHANNEL_BYTES);
}

void shim_switch_make_channel0_idle(uint8_t out[SHIM_SWITCH_CHANNEL_BYTES]) {
    static const uint8_t idle[SHIM_SWITCH_CHANNEL_BYTES] =
        { 0x00, 0xff, 0xff, 0xf8, 0xff, 0xff, 0xff, 0xff };
    memcpy(out, idle, sizeof(idle));

    const char *coin_door = getenv("SPIKE3_COIN_DOOR_INTERLOCK");
    if (!coin_door || !coin_door[0] ||
        strcmp(coin_door, "closed") == 0 ||
        strcmp(coin_door, "restored") == 0 ||
        strcmp(coin_door, "1") == 0) {
        out[SHIM_COIN_DOOR_INTERLOCK_BYTE] &= (uint8_t)~SHIM_COIN_DOOR_INTERLOCK_MASK;
    } else if (strcmp(coin_door, "open") == 0 || strcmp(coin_door, "0") == 0) {
        out[SHIM_COIN_DOOR_INTERLOCK_BYTE] |= SHIM_COIN_DOOR_INTERLOCK_MASK;
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

void shim_switch_hold_apply(uint8_t channel_id, uint8_t buf[SHIM_SWITCH_CHANNEL_BYTES]) {
    if (!buf || channel_id >= SHIM_SWITCH_CHANNEL_COUNT) {
        return;
    }

    uint8_t mask[SHIM_SWITCH_CHANNEL_BYTES];
    pthread_mutex_lock(&shim_switch_hold_mutex);
    memcpy(mask, shim_switch_hold_mask[channel_id], sizeof(mask));
    pthread_mutex_unlock(&shim_switch_hold_mutex);

    for (int i = 0; i < SHIM_SWITCH_CHANNEL_BYTES; i++) {
        if (channel_id == 0) {
            buf[i] &= (uint8_t)~mask[i];
        } else {
            buf[i] ^= mask[i];
        }
    }
}
