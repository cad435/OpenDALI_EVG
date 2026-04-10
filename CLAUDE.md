# DALI EVG Firmware (ch32fun)

## Project Overview

DALI-2 control gear (slave) firmware for CH32V003F4P6, using cnlohr's ch32fun framework. Implements IEC 62386-101 (physical layer), IEC 62386-102 (protocol), and IEC 62386-209 (DT8 colour control) for RGBW LED dimming with flash persistence.

## Architecture

```
src/
├── funconfig.h          # ch32fun framework config (clock, UART)
├── hardware.h           # Pin definitions, channel count, DALI PHY/NO_PHY switch, EVG mode selection
├── dali_physical.h      # Manchester timing, command numbers, fade/DT8 tables
├── dali_frame.h         # dali_frame_t value type + FORWARD/BACKWARD/ERROR/COLLISION/ECHO flags
├── dali_state.h         # Shared device state struct (dali_device_state_t) + helpers
├── dali_slave.h         # Public API facade (init, process, fade_tick, callbacks, ISRs)
├── dali_slave.c         # Thin facade — delegates to sub-modules
├── dali_phy.h/.c        # Physical layer: RX/TX state machines, collision detection, TIM2/EXTI
├── dali_protocol.h/.c   # Protocol handler: command dispatcher, queries, config, NVM pack/unpack
├── dali_fade.h/.c       # Fade engine: fadeTime/fadeRate transitions
├── dali_addressing.h/.c # Addressing protocol: INITIALISE, RANDOMISE, binary search, PROGRAM SHORT
├── dali_dt8.h/.c        # DT8 colour control: RGBWAF primaries, Tc conversion, DT8 queries
├── dali_bank0.h/.c      # Memory bank 0 (read-only gear identification)
├── dali_nvm.h/.c        # Flash persistence (deferred write with dirty flag)
├── led_driver.h/.c      # LED driver: no-op (ONOFF), TIM1 PWM (1–4ch), or SPI+DMA (WS2812/SK6812)
└── main.c               # Entry point, millis(), PSU control, callbacks, ISR wrappers
```

## Key Configuration (hardware.h)

### EVG Mode Selection

Define ONE `EVG_MODE_xxx` in `hardware.h` (or via `-DEVG_MODE_xxx` compiler flag). All other configuration (DALI device type, channel count, driver selection, DT8 features) is derived automatically. Default: `EVG_MODE_RGBW`.

| Mode | DT | Channels | Driver | Tc | Primary | Flash |
|------|-----|----------|--------|-----|---------|-------|
| `EVG_MODE_ONOFF` | 6 | 0 | PSU_CTRL only | - | - | 8.0 KB |
| `EVG_MODE_SINGLE` | 6 | 1 PWM | TIM1 | - | - | 8.7 KB |
| `EVG_MODE_CCT` | 8 | 2 PWM | TIM1 | yes | no | 9.7 KB |
| `EVG_MODE_RGB` | 8 | 3 PWM | TIM1 | yes | yes | 9.8 KB |
| `EVG_MODE_RGBW` | 8 | 4 PWM | TIM1 | yes | yes | 9.9 KB |
| `EVG_MODE_WS2812` | 8 | 3 (GRB) | SPI+DMA | yes | yes | 10.3 KB |
| `EVG_MODE_SK6812_RGB` | 8 | 3 (GRB) | SPI+DMA | yes | yes | 10.3 KB |
| `EVG_MODE_SK6812_RGBW` | 8 | 4 (GRBW) | SPI+DMA | yes | yes | 10.4 KB |

`EVG_MODE_ONOFF`: No LED driver, no TIM1, no log table. Only PA2 (PSU_CTRL) switches on/off. PHY_MIN=254, so any non-zero arc level is clamped to 254 (full on). Useful for relay/switch/contactor control via DALI.

Derived defines (do not set manually): `DALI_DEVICE_TYPE`, `PWM_NUM_CHANNELS`, `DIGITAL_LED_OUT`, `WS2812_TYPE`, `EVG_NUM_COLOURS`, `EVG_HAS_DT8`, `EVG_DT8_HAS_TC`, `EVG_DT8_HAS_PRIMARY`, `ONOFF_MODE`.

### Other Configuration

| Define | Default | Description |
|--------|---------|-------------|
| `DALI_NO_PHY` | **not** defined | Define for direct GPIO connection (no DALI transceiver). Default: PHY mode. |
| `WS2812_NUM_LEDS` | 30 | Number of LEDs in addressable strip (digital modes only). |
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
- Queries: STATUS (144, with resetState/powerCycleSeen flags), GEAR PRESENT (145), LAMP FAILURE (146), LAMP POWER ON (147), LIMIT ERROR (148), RESET STATE (149), MISSING SHORT (150), VERSION (151), DTR0/DTR1/DTR2 (152/155/156), DEVICE TYPE (153), PHYS MIN (154), ACTUAL LEVEL (160), MAX/MIN LEVEL (161/162), POWER ON LEVEL (163), SYSTEM FAILURE LEVEL (164), FADE SPEEDS (165), SCENE LEVEL (176–191), RANDOM H/M/L (194–196), READ MEMORY LOCATION (197), GROUPS 0–7/8–15 (198/199)
- Memory bank 0 (read-only, `dali_bank0.c`): IEC 62386-102:2014 §4.3.10 layout — GTIN, FW/HW version, serial, 101/102/103 versions, logical-unit count. 27 bytes (last addr 0x1A). Accessed via cmd 197 with DTR2=bank, DTR1=address; DTR1 post-increments and value mirrors into DTR0. GTIN/serial are zero placeholders pending provisioning. Bank 1 + write commands (0xC7/0xC9) not implemented.
- Structured frame type (`dali_frame.h`): `dali_frame_t { data, size, flags, timestamp }` with FORWARD / BACKWARD / ERROR / COLLISION / ECHO flag bits. RX path builds the frame in `dali_process()` and passes a pointer into `process_frame()`; ERROR/ECHO frames are rejected at the top of the dispatcher.
- TX echo / collision detection (IEC 62386-101 §8.2.4.4): `dali_isr_tx_tick()` samples `rx_bus_is_active()` at the start of every Te slot (skipping `TX_SETTLE` and `TX_START_LO`) and compares against the level we drove. Mismatch sets `tx_collision_flag`, releases the bus to idle within 1 Te, and aborts the frame. The flag is read-and-cleared via `dali_tx_consume_collision()` in the main loop, which prints `COLLISION` to debug serial. Requires a real DALI PHY (open-drain dominant-low bus) — the default configuration. With `DALI_NO_PHY` push-pull GPIO, collision detection is non-functional.
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

### Future: I2C EEPROM (AT24C256C)
PC1 (I2C1_SDA) and PC2 (I2C1_SCL) are reserved for external **AT24C256C** EEPROM. PSU_CTRL was moved from PC1 to PA2 to free the I2C pins.

**AT24C256C specs:** 256 Kbit (32 KB), 512 pages × 64 bytes, I2C up to 1 MHz, 5 ms write cycle, 1M write endurance, 1.7–5.5 V, SOIC-8. JLCPCB part with datasheet available. The 64-byte page size matches the CH32V003 internal flash page size exactly — 1:1 page mapping.

**Planned EEPROM memory layout:**
```
AT24C256C (32 KB = 0x0000–0x7FFF)
├── 0x0000–0x003F  DALI config (64 B) — replaces internal flash NVM entirely
└── 0x0040–0x7FFF  Firmware staging area (32,704 B) — for safe DALI bootloader updates
```

**When EEPROM is active:**
- `dali_nvm.c` reads/writes config directly to EEPROM (1M cycles) instead of internal flash (10K cycles)
- Internal flash NVM page at 0x08003FC0 is **no longer used** — full 16 KB available for firmware code
- The "firmware must not extend past 0x3FC0" constraint is eliminated

**Safe firmware staging (DALI bootloader A/B update):**
- DALI bootloader receives firmware over bus → writes to EEPROM staging area page-by-page
- After full transfer: CRC32 verify EEPROM content
- If valid: erase + reprogram internal flash from EEPROM, verify, then boot new firmware
- If DALI transfer fails or power lost during reception → EEPROM has partial/invalid data → chip boots current firmware from internal flash (not bricked)
- If power lost during internal flash reprogram → bootloader (in boot area at 0x1FFFF000, never erased) survives, sees valid EEPROM image on next boot, retries the flash write
- 32 KB staging area fits all firmware modes (largest is SK6812_RGBW at 10.4 KB) with room for future growth up to full 16 KB
- Estimated bootloader size with I2C + EEPROM staging: ~1200–1400 bytes (fits in 1920-byte boot area)

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

- **DALI PHY polarity**: Default (PHY mode): TX HIGH = bus active, TX LOW = bus idle; RX HIGH = active, LOW = idle. With `DALI_NO_PHY` defined: polarity is inverted (TX LOW = active, HIGH = idle) for direct GPIO connection.
- **TIM1 MOE**: TIM1 is an advanced timer — `BDTR.MOE` must be set or PWM outputs won't appear. This is already handled in `pwm_init()`.
- **ISR attribute**: All ISRs must use `__attribute__((interrupt))` for correct RISC-V hardware stacking on CH32V003.
- **Log dimming table**: 508 bytes in flash. Changing PWM frequency (ATRLR) requires regenerating the table. Use: `python -c "for i in range(1,255): x=10**((i-1)*3/253-1); print(round(x/100*ATRLR))"`
- **Flash storage page**: 0x08003FC0 (last 64 bytes of 16KB). Used by dali_nvm.c for persistent storage. Firmware code must not extend past 0x3FC0 (currently ends at ~0x24D0).

## Documentation Consistency

When making changes to the firmware, always check and update all related documentation files to keep them consistent:
- `firmware_architecture.mmd` — Mermaid diagram source
- `evg_mode_switch.mmd` — EVG mode feature matrix (re-render PNG after changes: `mmdc -i evg_mode_switch.mmd -o evg_mode_switch.png -s 3 -b white`)
- `README.md` — GitHub readme
- `Commands_Implemented.md` — Command-by-command status table
- `test/testcases.md` — Test case documentation
- `test/HIL_Testsetup.md` — HIL test setup documentation

## Bootloader

### USB Bootloader (cnlohr ch32v003fun-usb-bootloader)
- Location: `Bootloader/` (pre-built binaries), `Bootloader/src/bootloader/` (source, git-ignored)
- Size: 1896/1920 bytes (98.75% of boot area)
- USB HID device: VID:1209 PID:B003
- Boot entry: PC7 button held LOW at reset (DISABLE_BOOTLOAD + TIMEOUT_PWR=0 = button-only)
- USB pins: PD3 (D-), PD4 (D+), PD0 (DPU pull-up)
- **First-time setup**: Run `configurebootloader.bin` once per chip to set option bytes for boot-from-bootloader mode
- Build: `Bootloader/src/bootloader/make_win.bat` (uses PlatformIO riscv-wch-elf toolchain)
- Deploy: `Bootloader/src/bootloader/deploy.bat` (copies .bin files to `Bootloader/`)
- Flash: `Bootloader/flash.bat` (wlink.exe — configurebootloader.bin + bootloader.bin at 0x1FFFF000)
- See `Bootloader/README.md` for full details

### DALI Bootloader (EXPERIMENTAL — WORKING)
- Location: `DALI_Bootloader/`
- Size: ~976/1920 bytes (51% of boot area)
- **Not validated against standard DALI systems** — tested only with custom Pico bitbang master
- Polling Manchester decoder at 1200 baud on PC0, TX on PC5
- Reads NVM page (0x08003FC0) for device short address — filters frames by address (S=1)
- Protocol: standard DALI 16-bit forward frames with S=1 command addressing
- Commands: CMD_ERASE (0x84/132), CMD_DATA (0x85/133), CMD_COMMIT (0x86/134), CMD_BOOT (0x87/135) — vendor-specific reserved range (IEC 62386-102, bytes 129–143)
- Two-frame data transfer: CMD_DATA sets flag, next frame's data byte is firmware byte
- ACK (0x01) backward frame per 64-byte page for flow control
- Software entry: DALI cmd 131 (vendor-reserved, config repeat) writes RAM magic word at 0x200007F0, resets into bootloader
- Hardware entry: hold PC7 LOW during reset
- Clock fix: explicitly resets to HSI 24 MHz (PFIC system reset doesn't reliably reset PLL)
- Upload script: `DALI_Bootloader/dali_upload.ps1` (~11 min for 10 KB firmware with conservative timing)
- **Verified working** (2026-03-10): 9856 bytes, 154 pages, all ACK'd, firmware booted and responded to DALI commands after upload
- Self-contained build: all dependencies in `DALI_Bootloader/ch32v003fun/` (no USB_Bootloader dependency)
- Build: `DALI_Bootloader/build.bat`
- Flowchart: `DALI_Bootloader/bootloader_protocol.png` (+ `.mmd` source)

### TODO: OpenKNX GW-REG1-Dali as DALI upload master
- The **umbau** branch of [GW-REG1-Dali](https://github.com/OpenKNX/GW-REG1-Dali) (ESP32 variant) has a WebSocket JSON interface that can send arbitrary DALI frames with backward frame listening — no firmware changes needed
- Write a Python WebSocket client to orchestrate the bootloader upload protocol via the gateway
- Blocked on: ESP32 gateway hardware arrival

### Pin Assignments (Bootloader vs Firmware)

| Pin | Bootloader | Firmware |
|-----|-----------|----------|
| PC0 | DALI RX | DALI RX (EXTI0) |
| PC5 | DALI TX | DALI TX |
| PC7 | Boot button (active low) | -- |
| PD0 | USB DPU pull-up | USB DPU pull-up |
| PD3 | USB D- | USB D- |
| PD4 | USB D+ | USB D+ |

**WARNING**: Do not connect an EVG to the DALI bus AND USB simultaneously. USB should only be used for firmware upload while the device is disconnected from the DALI bus.

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

## Resource Usage (RGBW default)

- Flash: 9,668 B (59.0% of 16 KB)
- RAM: 136 B (6.6% of 2 KB)

## Notes on __WFI() / Sleep

`__WFI()` **with a bus-idle guard** works correctly. The guard in `main.c` only enters sleep when both:
1. TX state machine is idle (`dali_is_tx_idle()`)
2. No RX edge in the last 20 ms (`millis() - dali_last_rx_edge_ms() > DALI_WFI_IDLE_MS`)

This prevents WFI from being entered during active frame reception or transmission. During DALI idle periods (seconds to minutes between commands), the CPU sleeps most of the time.

**Root cause of the original failure**: `__WFI()` entered between edges of a DALI frame (during 2Te inter-edge gaps). SysTick (1 ms) wakes the CPU, but the accumulated timing errors over many frames eventually corrupt the TX state machine. The simple `dali_is_tx_idle()` guard was insufficient — it only checked TX state, not whether RX was actively receiving.

CH32V003 Sleep mode (SLEEPDEEP=0, PFIC_SCTLR=0x00): core stops, all peripherals run (TIM2, EXTI, SysTick continue). Safe for DALI. Do **not** use Standby mode (SLEEPDEEP=1) — TIM2 stops and DALI edge timing breaks.
