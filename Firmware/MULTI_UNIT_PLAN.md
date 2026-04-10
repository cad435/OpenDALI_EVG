# Plan: DALI-2 Multiple Logical Control Gear Units

## Context

DALI-2 (IEC 62386-102) supports multiple "logical control gear units" in a single physical device. Each unit gets its own short address (0-63), operating parameters (level, min/max, scenes, groups, fade), and responds independently to DALI commands. This lets one EVG with 4 PWM channels act as 4 independently addressable single-channel dimmers on the bus.

The firmware is already modularized. The key change: the single global `ds` state instance becomes an array of `DALI_NUM_UNITS` instances, and all command dispatch loops over addressed units.

## Design Decisions (confirmed with user)

- **LED mapping**: Multi-unit forces `EVG_MODE_SINGLE` or `EVG_MODE_ONOFF`. Each unit = 1 PWM channel. Unit 0 = CH1, unit 1 = CH2, etc. Up to 4 units. RGBW/RGB/CCT/DT8/WS2812 modes remain single-unit only.
- **Compile-time**: `DALI_NUM_UNITS` define (default 1). When 1, compiler eliminates all multi-unit overhead.
- **NVM**: N flash pages (one per unit), growing downward from 0x08003FC0.

## Implementation Phases

### Phase 1: Preparatory refactors (no behavioral change)

These make the multi-unit conversion mechanical. Each step builds and tests independently.

#### 1.1 Move DTRs out of `dali_device_state_t`

DTR0/1/2 and `enabled_device_type` are device-wide per the DALI spec, not per-unit. Move them to standalone globals in `dali_protocol.c`, extern'd in `dali_state.h`.

**Files**: `dali_state.h`, `dali_protocol.c`, `dali_addressing.c`, `dali_query.c`, `dali_dt8.c`
**Change**: `ds.dtr0` → `dali_dtr0` everywhere (find/replace)

#### 1.2 Move callbacks out of struct, add unit parameter

Callbacks are device-wide. Move to module-private statics in `dali_protocol.c`. Update typedefs to include `uint8_t unit`:

```c
typedef void (*dali_arc_callback_t)(uint8_t unit, uint8_t level);
typedef void (*dali_colour_callback_t)(uint8_t unit, const uint8_t *levels, uint8_t count);
```

**Files**: `dali_slave.h`, `dali_state.h`, `dali_protocol.c`, `dali_fade.c`, `main.c`
**Note**: For now, `unit` is always 0. No behavioral change.

#### 1.3 Convert `ds` to array, add `dali_cur_unit`

```c
extern dali_device_state_t ds[DALI_NUM_UNITS];  // array (size 1 for now)
extern uint8_t dali_cur_unit;                    // always 0 for now
```

Replace all `ds.field` with `ds[dali_cur_unit].field`. Update `clamp_level()` and `is_addressed_to_me()` in `dali_state.h`.

**Files**: `dali_state.h`, `dali_protocol.c`, `dali_query.c`, `dali_addressing.c`, `dali_fade.c`, `dali_dt8.c`, `dali_nvm.c`

#### 1.4 Add `DALI_NUM_UNITS` define + compile guards

In `hardware.h`:
```c
#ifndef DALI_NUM_UNITS
#define DALI_NUM_UNITS 1
#endif

#if DALI_NUM_UNITS > 1
  #if EVG_HAS_DT8
    #error "Multi-unit requires EVG_MODE_SINGLE or EVG_MODE_ONOFF"
  #endif
  #if defined(DIGITAL_LED_OUT)
    #error "Multi-unit not supported with WS2812/SK6812"
  #endif
  #if DALI_NUM_UNITS > 4
    #error "DALI_NUM_UNITS must be 1-4"
  #endif
  /* Override channel count: one channel per unit */
  #ifndef ONOFF_MODE
    #undef PWM_NUM_CHANNELS
    #define PWM_NUM_CHANNELS DALI_NUM_UNITS
  #endif
#endif
```

No code changes beyond the header. Build all modes to verify N=1 is clean.

### Phase 2: Multi-unit logic

#### 2.1 `dali_state.h` — `dali_addressed_units()` replaces `is_addressed_to_me()`

Returns a bitmask of which units the frame addresses:
```c
static inline uint8_t dali_addressed_units(uint8_t addr_byte) {
    if ((addr_byte & 0xFE) == 0xFE) return (1 << DALI_NUM_UNITS) - 1; /* broadcast */
    uint8_t mask = 0;
    for (uint8_t u = 0; u < DALI_NUM_UNITS; u++) {
        if (!(addr_byte >> 7)) { /* short address */
            if (((addr_byte >> 1) & 0x3F) == ds[u].short_address) mask |= (1 << u);
        } else if ((addr_byte & 0xE0) == 0x80) { /* group */
            uint8_t g = (addr_byte >> 1) & 0x0F;
            if (ds[u].group_membership & (1 << g)) mask |= (1 << u);
        }
    }
    return mask;
}
```

For N=1 the loop is trivially unrolled by the compiler.

#### 2.2 `dali_protocol.c` — Unit iteration in `process_frame()`

After special command check and ENABLE_DT consumption:
```c
uint8_t unit_mask = dali_addressed_units(addr_byte);
if (!unit_mask) return;

for (uint8_t u = 0; u < DALI_NUM_UNITS; u++) {
    if (!(unit_mask & (1 << u))) continue;
    dali_cur_unit = u;
    /* existing dispatch code, unchanged except ds.X → ds[u].X */
}
```

Also add `dali_protocol_init()` with runtime initialization loop (needed for N>1 since C can't repeat struct initializers).

#### 2.3 `dali_fade.c` — Per-unit fade arrays

```c
static volatile uint8_t  fade_running[DALI_NUM_UNITS];
static volatile uint8_t  target_level[DALI_NUM_UNITS];
static volatile uint16_t fade_ms_per_step[DALI_NUM_UNITS];
static volatile uint32_t last_step_ms[DALI_NUM_UNITS];
```

`dali_fade_tick()` iterates all units. Other functions use `dali_cur_unit` as index.

#### 2.4 `dali_addressing.c` — Per-unit init/random state

```c
static volatile init_state_t init_state[DALI_NUM_UNITS];
static volatile uint32_t    init_start_time[DALI_NUM_UNITS];
static volatile uint8_t     random_h[DALI_NUM_UNITS], random_m[DALI_NUM_UNITS], random_l[DALI_NUM_UNITS];
```

Search address (`search_h/m/l`) stays device-wide. Key command changes:
- **INITIALISE**: loop all units, enter init based on `data_byte` matching per-unit address
- **RANDOMISE**: each unit generates own random (seed mixed with unit index)
- **COMPARE**: first matching unit sends 0xFF (if multiple match, same value = no collision)
- **WITHDRAW/PROGRAM_SHORT/VERIFY_SHORT/QUERY_SHORT**: iterate units, match random==search

Random query API gets unit parameter: `dali_addressing_random_h(uint8_t unit)`

#### 2.5 `dali_nvm.c` — Multi-page persistence

```c
#define NVM_FLASH_ADDR_UNIT(u)  (0x08003FC0 - (u) * 64)
```

Per-unit dirty flags. `nvm_init()` loads all units. `nvm_tick()` saves dirty units. `nvm_pack_state(nvm, unit)` / `nvm_unpack_state(nvm, unit)` take unit parameter.

#### 2.6 `dali_bank0.c` — Dynamic unit index

Override bytes 0x19/0x1A in `dali_bank0_read()`:
```c
if (addr == 0x19) return DALI_NUM_UNITS;
if (addr == 0x1A) return dali_cur_unit;
```

#### 2.7 `led_driver.c` — Per-channel function

Add `led_driver_set_channel(uint8_t ch, uint8_t dali_level)` for multi-unit mode. Sets single TIM1 channel via log table lookup.

#### 2.8 `main.c` — Multi-unit callbacks

```c
static void on_level(uint8_t unit, uint8_t level) {
#if DALI_NUM_UNITS > 1
    led_driver_set_channel(unit, level);
    /* PSU on if any unit has level > 0 */
    uint8_t any_on = 0;
    for (uint8_t u = 0; u < DALI_NUM_UNITS; u++)
        if (ds[u].actual_level > 0) { any_on = 1; break; }
    psu_ctrl_set(any_on);
#else
    led_driver_apply(level, dali_protocol_get_colour_actual(0));
    psu_ctrl_set(level > 0);
#endif
    printf("U%d LVL=%d\n", unit, level);
}
```

Getter APIs take unit parameter: `dali_protocol_get_actual_level(uint8_t unit)`.

## Resource Estimates

| Config | Flash | RAM | NVM pages |
|--------|-------|-----|-----------|
| RGBW, N=1 (default) | ~9.7 KB (unchanged) | ~136 B | 1 × 64 B |
| SINGLE, N=1 | ~8.6 KB | ~136 B | 1 × 64 B |
| SINGLE, N=2 | ~8.9 KB (+300 B) | ~200 B (+65 B) | 2 × 64 B |
| SINGLE, N=4 | ~9.2 KB (+600 B) | ~330 B (+195 B) | 4 × 64 B |

All well within 16 KB flash / 2 KB RAM.

## Verification

1. **Build matrix**: all 8 EVG modes with N=1 (must match current sizes), SINGLE+N=2, SINGLE+N=4, ONOFF+N=2. Verify DT8+N=2 and WS2812+N=2 produce compile errors.
2. **Hardware test (N=2)**: INITIALISE → RANDOMISE → binary search finds 2 devices → PROGRAM SHORT 0 and 1 → DAPC to addr 0 changes only CH1 → DAPC to addr 1 changes only CH2 → broadcast OFF → both off → power cycle → addresses restored from NVM.
3. **Regression (N=1)**: all existing test scripts pass unchanged.

## File Change Summary

| File | Change type |
|------|------------|
| `config/hardware.h` | Add DALI_NUM_UNITS, compile guards |
| `dali/dali_state.h` | ds→array, DTRs→extern globals, dali_addressed_units() |
| `dali/dali_slave.h` | Callback typedefs add unit param |
| `dali/protocol/dali_protocol.c` | ds array init, unit loop in process_frame, DTR globals |
| `dali/protocol/dali_protocol.h` | Getter APIs add unit param |
| `dali/protocol/dali_query.c` | ds[dali_cur_unit], DTR globals |
| `dali/protocol/dali_addressing.c` | Per-unit arrays for init/random state |
| `dali/protocol/dali_addressing.h` | Random query API adds unit param |
| `dali/protocol/dali_fade.c` | Per-unit fade arrays, tick iterates all |
| `dali/protocol/dali_dt8.c` | ds[dali_cur_unit], DTR globals (DT8 off when N>1) |
| `dali/nvm/dali_nvm.c` | Multi-page, per-unit dirty, pack/unpack take unit |
| `dali/nvm/dali_nvm.h` | NVM_FLASH_ADDR_UNIT macro, API changes |
| `dali/nvm/dali_bank0.c` | Override bytes 0x19/0x1A |
| `led/led_driver.c` | Add led_driver_set_channel() |
| `led/led_driver.h` | Declare led_driver_set_channel() |
| `main.c` | Multi-unit callbacks, PSU logic, init sequence |
