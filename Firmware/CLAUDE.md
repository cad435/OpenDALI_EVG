# DALI EVG Firmware (ch32fun)

## Project Overview

DALI-2 control gear (slave) firmware for CH32V003F4P6, using cnlohr's ch32fun framework. Implements IEC 62386-101 (physical layer), IEC 62386-102 (protocol), and IEC 62386-209 (DT8 colour control) for RGBW LED dimming with flash persistence.

## Architecture

```
src/
├── funconfig.h      # ch32fun framework config (clock, UART)
├── hardware.h       # Pin definitions, channel count, DALI_NO_PHY switch
├── dali_physical.h  # Manchester timing, command numbers, fade/DT8 tables
├── dali_slave.h     # DALI slave API (init, process, fade_tick, callbacks, ISRs)
├── dali_slave.c     # RX/TX state machines, protocol handler, fade engine, DT8, addressing
├── dali_nvm.h       # Flash persistence struct and API
├── dali_nvm.c       # Flash unlock/erase/write, deferred save with dirty flag
└── main.c           # Entry point, millis(), TIM1 PWM, log dimming, per-channel colour
```

## Key Configuration (hardware.h)

| Define | Default | Description |
|--------|---------|-------------|
| `DALI_NO_PHY` | defined | Direct GPIO connection (no DALI transceiver). Comment out when using a PHY. |
| `DALI_DEVICE_TYPE` | 8 | DALI device type (8=colour control, DT8). Returned by QUERY DEVICE TYPE. |
| `PWM_NUM_CHANNELS` | 4 | Number of active TIM1 PWM channels (1-4). Controls which pins are initialized. |
| `PSU_CTRL_PORT/PIN_N` | PA2 | GPIO output: HIGH when any channel is on, LOW when all off. |

### TIM1 PWM Channel Mapping (default, no AFIO remap)

| Channel | Pin | Port | LED | LA Channel |
|---------|-----|------|-----|------------|
| DALI bus | PC0 | GPIOC | -- | D0, D1 (two probe points on DALI bus) |
| CH1 | PD2 | GPIOD | Red | D4 |
| CH2 | PA1 | GPIOA | Green | D5 |
| CH3 | PC3 | GPIOC | Blue | D6 |
| CH4 | PC4 | GPIOC | White | D7 |
| PSU_CTRL | PA2 | GPIOA | -- | D3 |
| I2C1_SDA | PC1 | GPIOC | -- | -- | (reserved for EEPROM) |
| I2C1_SCL | PC2 | GPIOC | -- | -- | (reserved for EEPROM) |

## Build & Flash

```bash
# Build
pio run

# Flash CH32V003 via WCH-Link
wlink.exe flash .pio/build/genericCH32V003F4P6/firmware.bin

# Or via PlatformIO
pio run -t upload
```

## DALI Protocol Support

### Implemented (IEC 62386-102)
- Forward frame RX (16-bit Manchester decode via EXTI + TIM2)
- Backward frame TX (8-bit Manchester encode via TIM2 output compare)
- Direct arc power (broadcast, short address, group address), 0xFF MASK handling
- Min/max level clamping on all arc power paths (IEC 62386-102 §9.4)
- Fade engine (IEC 62386-102 §9.5): fadeTime for arc power + scenes, fadeRate for UP/DOWN
- Commands: OFF (0), UP (1), DOWN (2), STEP UP (3), STEP DOWN (4), RECALL MAX (5), RECALL MIN (6), STEP DOWN AND OFF (7), ON AND STEP UP (8)
- RESET (32): restore all variables to IEC 62386-102 Table 22 defaults
- STORE ACTUAL LEVEL IN DTR0 (33), STORE DTR AS SHORT ADDRESS (48)
- GO TO SCENE (16–31): recall scene level with fadeTime
- Config commands with config repeat (2× within 100 ms):
  - STORE DTR AS MAX LEVEL (42), MIN LEVEL (43), POWER ON LEVEL (44), SYSTEM FAILURE LEVEL (45)
  - STORE DTR AS FADE TIME (46), FADE RATE (47), SHORT ADDRESS (48), EXTENDED FADE TIME (128)
  - STORE DTR AS SCENE LEVEL (64–79), REMOVE FROM SCENE (80–95)
  - ADD TO GROUP (96–111), REMOVE FROM GROUP (112–127)
- Group addressing: 16-bit membership bitmask, commands 96–127
- DTR0/DTR1/DTR2 storage (via DALI special commands 0xA3, 0xC3, 0xC5)
- Queries: STATUS (144, with resetState/powerCycleSeen flags), GEAR PRESENT (145), LAMP FAILURE (146), LAMP POWER ON (147), LIMIT ERROR (148), RESET STATE (149), MISSING SHORT (150), VERSION (151), DTR0/DTR1/DTR2 (152/155/156), DEVICE TYPE (153), PHYS MIN (154), ACTUAL LEVEL (160), MAX/MIN LEVEL (161/162), POWER ON LEVEL (163), SYSTEM FAILURE LEVEL (164), FADE SPEEDS (165), SCENE LEVEL (176–191), RANDOM H/M/L (194–196), GROUPS 0–7/8–15 (198/199)
- Full addressing: INITIALISE, RANDOMISE, COMPARE, SEARCHADDR, PROGRAM SHORT, WITHDRAW, VERIFY SHORT, QUERY SHORT, TERMINATE
- Config repeat validation (100 ms window for INITIALISE/RANDOMISE and commands 32–128)
- 15-minute initialisation timeout
- Power-on level: applied at boot via dali_power_on()
- IEC 62386-102 §9.3 logarithmic dimming curve (254-entry LUT)
- PSU control output (PA2): HIGH when any channel on, LOW when all off
- **Flash persistence**: short address, min/max/power-on/sys-fail levels, fade time/rate, scenes, groups, DT8 colour stored in last 64-byte flash page (0x08003FC0). Deferred write with 5-second dirty timer to batch config changes.
- Full command list: see `Commands_Implemented.md`

### Implemented (IEC 62386-209, DT8 Colour Control)
- ENABLE DEVICE TYPE 8 protocol (consume-on-use for extended commands)
- Per-channel RGBW PWM output: `pwm[ch] = log_table[arc_level] × colour[ch] / 254`
- SET_TEMP_RGB_LEVEL (235): stage R/G/B from DTR2/DTR1/DTR0
- SET_TEMP_WAF_LEVEL (236): stage W from DTR2 (A/F ignored)
- ACTIVATE (226): commit staged colour to output
- SET_TEMP_COLOUR_TEMPERATURE (231): Tc in mirek → RGBW conversion
- STEP_COOLER (232) / STEP_WARMER (233): ±10 mirek steps
- COPY_REPORT_TO_TEMP (238): copy active → staging
- DT8 queries (247–252): GEAR_FEATURES, COLOUR_STATUS, COLOUR_TYPE_FEATURES, COLOUR_VALUE, RGBWAF_CONTROL, ASSIGNED_COLOUR
- Tc→RGBW: linear interpolation (2700K–6500K), integer math only
- DT6 backward compatibility: when no DT8 colour is set, all channels equal
- **Colour persistence**: RGBW levels + Tc stored in NVM, restored at boot

## Flash Persistence (dali_nvm.c)

### Storage Layout (64 bytes at 0x08003FC0)
```
Offset  Size  Field
0x00    4     magic (0x44414C49 = "DALI")
0x04    1     short_address
0x05    1     max_level
0x06    1     min_level
0x07    1     power_on_level
0x08    1     sys_fail_level
0x09    1     fade_time
0x0A    1     fade_rate
0x0B    1     (padding)
0x0C    2     group_membership
0x0E    16    scene_level[16]
0x1E    4     colour[4] (R,G,B,W — 0xFF = default 254)
0x22    2     colour_tc (mirek — 0xFFFF = not set)
0x24    1     ext_fade ((mult<<4)|base, 0xFF = not set)
0x25    23    (reserved for future Tc limits, etc.)
```

### Write Strategy
- Config commands set a **dirty flag** via `nvm_mark_dirty()`
- `nvm_tick()` in main loop saves to flash after **5 seconds** of no further changes
- Batches rapid config changes (e.g., 16 scene stores) into a single flash erase+write
- Flash erase+write takes ~6ms total
- CH32V003 flash endurance: ~10,000 erase cycles per page

### Future: I2C EEPROM
PC1 (I2C1_SDA) and PC2 (I2C1_SCL) are reserved for external AT24C02/M24C02 EEPROM (1M write cycles). PSU_CTRL was moved from PC1 to PA2 to free the I2C pins.

## Peripheral Usage

| Peripheral | Purpose |
|------------|---------|
| TIM1 | PWM generation on CH1-CH4 (20 kHz, 2400-step resolution) |
| TIM2 | Free-running 1 MHz counter for DALI edge timing (CH2=TX OC, CH4=idle timeout OC) |
| EXTI0 | Both-edge interrupt on PC0 for DALI RX |
| SysTick | 1 ms tick for millis() — used by DALI timeout logic |
| USART1 | Debug printf on PD5 (115200 baud, configured by ch32fun) |
| Flash | Last 64-byte page (0x08003FC0) for NVM persistence |

## Important Notes

- **DALI_NO_PHY polarity**: With `DALI_NO_PHY` defined, TX LOW = bus active, TX HIGH = bus idle. Without it, polarity is inverted (for use with a DALI PHY transceiver).
- **TIM1 MOE**: TIM1 is an advanced timer — `BDTR.MOE` must be set or PWM outputs won't appear. This is already handled in `pwm_init()`.
- **ISR attribute**: All ISRs must use `__attribute__((interrupt))` for correct RISC-V hardware stacking on CH32V003.
- **Log dimming table**: 508 bytes in flash. Changing PWM frequency (ATRLR) requires regenerating the table. Use: `python -c "for i in range(1,255): x=10**((i-1)*3/253-1); print(round(x/100*ATRLR))"`
- **Flash storage page**: 0x08003FC0 (last 64 bytes of 16KB). Used by dali_nvm.c for persistent storage. Firmware code must not extend past 0x3FC0 (currently ends at ~0x24D0).

## Documentation Consistency

When making changes to the firmware, always check and update all related documentation files to keep them consistent:
- `firmware_architecture.mmd` — Mermaid diagram source
- `README.md` — GitHub readme
- `Commands_Implemented.md` — Command-by-command status table
- `test/testcases.md` — Test case documentation
- `test/HIL_Testsetup.md` — HIL test setup documentation

## Testing

Test cases and HIL setup documentation are in `test/`. Test scripts are in `Debug_Helpers/`.

Key scripts:
- `ch32fun_test.ps1` — Forward + backward frame + PWM test
- `ch32fun_assign_test.ps1` — Full addressing protocol test
- `ch32fun_compliance_test.ps1` — DALI-1/2 compliance (min/max, scenes, groups, RESET, DTR1/2)
- `ch32fun_nvm_test.ps1` — NVM flash persistence (config + reset via wlink + verify, 36/36 pass)
- `ch32fun_newcmd_test.ps1` — New commands test (queries 146-149, status flags, cmd 48, 13/13 pass)
- `ch32fun_dt8_test.ps1` — DT8 colour control (RGBW, Tc, queries)
- `dali_timing_verify.ps1` — IEC 62386-101 timing compliance (LA)
- `dali_logdim_test.ps1` — Logarithmic dimming curve verification
- `la_pwm_duty.ps1` — LA PWM duty cycle measurement per channel (D4–D7)
- `ch32_led_test/` — Standalone LED PWM test firmware (ch32fun, no DALI)

## Resource Usage

- Flash: 9,712 B (59.3% of 16 KB)
- RAM: 136 B (6.6% of 2 KB)

## Notes on __WFI() / Sleep

`__WFI()` **with a bus-idle guard** works correctly. The guard in `main.c` only enters sleep when both:
1. TX state machine is idle (`dali_is_tx_idle()`)
2. No RX edge in the last 20 ms (`millis() - dali_last_rx_edge_ms() > DALI_WFI_IDLE_MS`)

This prevents WFI from being entered during active frame reception or transmission. During DALI idle periods (seconds to minutes between commands), the CPU sleeps most of the time.

**Root cause of the original failure**: `__WFI()` entered between edges of a DALI frame (during 2Te inter-edge gaps). SysTick (1 ms) wakes the CPU, but the accumulated timing errors over many frames eventually corrupt the TX state machine. The simple `dali_is_tx_idle()` guard was insufficient — it only checked TX state, not whether RX was actively receiving.

CH32V003 Sleep mode (SLEEPDEEP=0, PFIC_SCTLR=0x00): core stops, all peripherals run (TIM2, EXTI, SysTick continue). Safe for DALI. Do **not** use Standby mode (SLEEPDEEP=1) — TIM2 stops and DALI edge timing breaks.
