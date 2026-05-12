#ifndef SPIKE3_GAMES_POKEMON_LE_0_81_0_MACHINE_PROFILE_H
#define SPIKE3_GAMES_POKEMON_LE_0_81_0_MACHINE_PROFILE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PLE_KIND_COIL    = 0,
    PLE_KIND_MOTOR   = 1,
    PLE_KIND_SERVO   = 2,
    PLE_KIND_MAGNET  = 3,
    PLE_KIND_DIGITAL = 4
} ple_driver_kind_t;

typedef enum {
    PLE_LAMP_GI       = 0,
    PLE_LAMP_FEATURE  = 1,
    PLE_LAMP_RGB_R    = 2,
    PLE_LAMP_RGB_G    = 3,
    PLE_LAMP_RGB_B    = 4,
    PLE_LAMP_FLASHER  = 5,
    PLE_LAMP_DIGITAL  = 6
} ple_lamp_kind_t;

typedef struct {
    const char *manual_id;
    const char *name;
    uint8_t channel;
    uint8_t bit_idx;
    uint16_t sw_id_runtime;
} ple_switch_t;

typedef struct {
    const char *manual_id;
    const char *name;
    uint8_t slot;
    uint8_t dr_idx;
    ple_driver_kind_t kind;
} ple_driver_t;

typedef struct {
    const char *manual_id;
    const char *name;
    uint8_t slot;
    uint16_t lp_idx;
    ple_lamp_kind_t kind;
} ple_lamp_t;

extern const ple_switch_t  ple_switches[];
extern const size_t        ple_switches_count;
extern const ple_driver_t  ple_drivers[];
extern const size_t        ple_drivers_count;
extern const ple_lamp_t    ple_lamps[];
extern const size_t        ple_lamps_count;

const ple_switch_t *ple_lookup_switch_by_channel_bit(uint8_t channel, uint8_t bit_idx);
const ple_switch_t *ple_lookup_switch_by_manual_id(const char *manual_id);

#ifdef __cplusplus
}
#endif

#endif
