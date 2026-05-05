# DALI EVG Firmware (ch32fun)

## Project Overview

DALI-2 control gear (slave) firmware for CH32V003F4U6, using cnlohr's ch32fun framework. Implements IEC 62386-101 (physical layer), IEC 62386-102 (protocol), and IEC 62386-209 (DT8 colour control) for RGBW LED dimming with flash persistence.

## Architecture

```
src/
├── funconfig.h          # ch32fun framework config (clock, UART)
├── hardware.h           # Pin definitions, channel count, EVG mode selection
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
| `EVG_MODE_ONOFF` | 6 | 0 | PSU_CTRL only | - | - | 8.6 KB |
| `EVG_MODE_SINGLE` | 6 | 1 PWM | TIM1 | - | - | 9.3 KB |
| `EVG_MODE_CCT` | 8 | 2 PWM | TIM1 | yes | no | 10.4 KB |
| `EVG_MODE_RGB` | 8 | 3 PWM | TIM1 | yes | yes | 10.5 KB |
| `EVG_MODE_RGBW` | 8 | 4 PWM | TIM1 | yes | yes | 10.6 KB |
| `EVG_MODE_WS2812` | 8 | 3 (GRB) | SPI+DMA | yes | yes | 11.0 KB |
| `EVG_MODE_SK6812_RGB` | 8 | 3 (GRB) | SPI+DMA | yes | yes | 11.0 KB |
| `EVG_MODE_SK6812_RGBW` | 8 | 4 (GRBW) | SPI+DMA | yes | yes | 11.1 KB |

`EVG_MODE_ONOFF`: No LED driver, no TIM1, no log table. Only PA2 (PSU_CTRL) switches on/off. PHY_MIN=254, so any non-zero arc level is clamped to 254 (full on). Useful for relay/switch/contactor control via DALI.

Derived defines (do not set manually): `DALI_DEVICE_TYPE`, `PWM_NUM_CHANNELS`, `DIGITAL_LED_OUT`, `WS2812_TYPE`, `EVG_NUM_COLOURS`, `EVG_HAS_DT8`, `EVG_DT8_HAS_TC`, `EVG_DT8_HAS_PRIMARY`, `ONOFF_MODE`.

### Other Configuration

| Define | Default | Description |
|--------|---------|-------------|
| `WS2812_NUM_LEDS` | 30 | Number of LEDs in addressable strip (digital modes only). |
| `PSU_CTRL_PORT/PIN_N` | PA2 | GPIO output: HIGH when any channel is on, LOW when all off. |

### TIM1 PWM Channel Mapping (Partial Remap 1, `AFIO->PCFR1 |= AFIO_PCFR1_TIM1_REMAP_PARTIALREMAP1`)

| Channel | Pin | Port | LED | LA Channel |
|---------|-----|------|-----|------------|
| DALI RX | PC3 | GPIOC | -- | (probe DALI bus directly) |
| DALI TX | PC4 | GPIOC | -- | -- |
| CH1 (TIM1_CH1) | PC6 | GPIOC | Red | D0 |
| CH2 (TIM1_CH2) | PC7 | GPIOC | Green | D1 |
| CH3 (TIM1_CH3) | PC0 | GPIOC | Blue | D2 |
| CH4 (TIM1_CH4) | PD3 | GPIOD | White | D3 |
| PSU_CTRL | PA2 | GPIOA | -- | D4 |
| I2C1_SDA | PC1 | GPIOC | -- | -- (reserved for EEPROM) |
| I2C1_SCL | PC2 | GPIOC | -- | -- (reserved for EEPROM) |
| Debug UART TX | PD5 | GPIOD | -- | -- (115200 baud) |

PC6 is dual-use: in PWM modes it's TIM1_CH1; in WS2812/SK6812 modes it's SPI1_MOSI.

## Build & Flash

```bash
# Build
pio run

# Flash CH32V003 via WCH-Link
wlink.exe flash .pio/build/genericCH32V003F4U6/firmware.bin

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
- TX echo / collision detection (IEC 62386-101 §8.2.4.4): `dali_isr_tx_tick()` samples `rx_bus_is_active()` at the start of every Te slot (skipping `TX_SETTLE` and `TX_START_LO`) and compares against the level we drove. Mismatch sets `tx_collision_flag`, releases the bus to idle within 1 Te, and aborts the frame. The flag is read-and-cleared via `dali_phy_consume_collision()` in the main loop, which prints `COLLISION` to debug serial. Requires a real DALI PHY transceiver (open-drain dominant-low bus).
- Full addressing: INITIALISE, RANDOMISE, COMPARE, SEARCHADDR, PROGRAM SHORT, WITHDRAW, VERIFY SHORT, QUERY SHORT, TERMINATE
- 32-bit forward frame support (IEC 62386-101, 7.4.3): PHY decodes both 16-bit and 32-bit frames, protocol dispatcher routes by frame length. Currently handles START FW TRANSFER (IEC 62386-105) for bootloader entry via 32-bit frame.
- Config repeat validation (100 ms window for INITIALISE/RANDOMISE and commands 32–128)
- 15-minute initialisation timeout
- Power-on level: applied at boot via dali_power_on()
- IEC 62386-102 §9.3 logarithmic dimming curve (254-entry LUT)
- PSU control output (PA2): HIGH when any channel on, LOW when all off
- **EEPROM persistence**: short address, min/max/power-on/sys-fail levels, fade time/rate, scenes, groups, DT8 colour stored in AT24C256 I2C EEPROM (see "Persistent Storage" section below). Deferred write with 5-second dirty timer to batch config changes.
- Full command list: see `Commands_Implemented.md`

### Implemented (IEC 62386-209, DT8 Colour Control)
- ENABLE DEVICE TYPE 8 protocol (consume-on-use for extended commands)
- Per-channel RGBW PWM output: `pwm[ch] = log_table[arc_level] × colour[ch] / 254`
- SET_TEMP_RGB_LEVEL (235): stage R/G/B from DTR0/DTR1/DTR2 (per IEC 62386-209)
- SET_TEMP_WAF_LEVEL (236): stage W from DTR0 (A/F ignored)
- ACTIVATE (226): commit staged colour to output
- SET_TEMP_COLOUR_TEMPERATURE (231): Tc in mirek → RGBW conversion
- STEP_COOLER (232) / STEP_WARMER (233): ±10 mirek steps
- COPY_REPORT_TO_TEMP (238): copy active → staging
- DT8 queries (247–252): GEAR_FEATURES, COLOUR_STATUS, COLOUR_TYPE_FEATURES, COLOUR_VALUE, RGBWAF_CONTROL, ASSIGNED_COLOUR
- Tc→RGBW: linear interpolation (2700K–6500K), integer math only
- DT6 backward compatibility: when no DT8 colour is set, all channels equal
- **Colour persistence**: RGBW levels + Tc stored in NVM, restored at boot

## Persistent Storage (I2C EEPROM)

### AT24C256C EEPROM
- 256 Kbit (32 KB), 512 pages × 64 bytes, I2C 100 kHz on PC1 (SDA) / PC2 (SCL)
- 5 ms write cycle, 1,000,000 write endurance (vs 10,000 for internal flash)
- Driver: `src/eeprom/eeprom.c` — `eeprom_init()`, `eeprom_write()`, `eeprom_read()`
- Layout defined in `src/eeprom/eeprom_layout.h` (shared between firmware and bootloader)

### EEPROM Memory Layout
```
AT24C256C (32 KB = 0x0000–0x7FFF)
├── 0x0000–0x003F  Device identity (64 B) — written by firmware at boot
│   ├── 0x0000  magic (4 B, "DALI" = 0x44414C49)
│   ├── 0x0004  GTIN (6 B, MSB first)
│   ├── 0x000A  EVG mode ID (1 B, from hardware.h EVG_MODE_ID)
│   ├── 0x000B  HW version major/minor (2 B)
│   ├── 0x000D  FW version major/minor (2 B)
│   └── 0x000F  short_address (1 B, updated on address change)
├── 0x0040–0x007F  DALI config (64 B, dali_nvm_t struct)
└── 0x0080–0x7FFF  Firmware staging area (32,640 B) — used by bootloader
```

### NVM API (dali_nvm.c)
- Internal flash NVM page at 0x08003FC0 is **no longer used** — full 16 KB available for firmware
- `dali_nvm.c` reads/writes config to EEPROM at address 0x0040
- Same API: `nvm_init()`, `nvm_save()`, `nvm_mark_dirty()`, `nvm_tick()`
- Deferred write: dirty flag + 5-second debounce timer (unchanged)
- Identity block (GTIN, EVG mode, versions, short address) written at every boot and on address change — read by bootloader for Block 0 validation

### EVG Mode IDs (for firmware update device key validation)
| Mode | ID | Defined in |
|------|-----|-----------|
| ONOFF | 0x01 | hardware.h `EVG_MODE_ID` |
| SINGLE | 0x02 | |
| CCT | 0x03 | |
| RGB | 0x04 | |
| RGBW | 0x05 | |
| WS2812 | 0x06 | |
| SK6812_RGB | 0x07 | |
| SK6812_RGBW | 0x08 | |

## Peripheral Usage

| Peripheral | Purpose |
|------------|---------|
| TIM1 | PWM generation on CH1-CH4 (20 kHz, 2400-step resolution) |
| TIM2 | Free-running 1 MHz counter for DALI edge timing (CH2=TX OC, CH4=idle timeout OC) |
| EXTI0 | Both-edge interrupt on PC0 for DALI RX |
| I2C1 | AT24C256 EEPROM (PC1=SDA, PC2=SCL, 100 kHz) |
| SysTick | 1 ms tick for millis() — HCLK/8 (6 MHz), free-running (no auto-reload), CMP advanced in ISR |
| USART1 | Debug printf on PD5 (115200 baud, configured by ch32fun) |

## Important Notes

- **SysTick / Delay_Ms compatibility**: `millis_init()` must NOT use auto-reload mode (STRE=1) or HCLK source (STCLK=1). ch32fun's `DelaySysTick()` / `Delay_Ms()` expects SysTick to be a free-running 32-bit upcount at HCLK/8 (6 MHz). Auto-reload wraps CNT at CMP, causing `DelaySysTick` to hang when the target exceeds CMP. Correct config: `CTLR=0x3` (STE + STIE, no auto-reload, HCLK/8), CMP advanced by 6000 in the ISR. `FUNCONF_SYSTICK_USE_HCLK` is 0 (default) so `DELAY_MS_TIME = SYSCLK/8000 = 6000`.
- **DALI PHY polarity**: TX HIGH = bus active (mark), TX LOW = bus idle. RX LOW = bus active (PHY inverts), HIGH = idle. Requires a real DALI PHY transceiver for collision detection.
- **TIM1 MOE**: TIM1 is an advanced timer — `BDTR.MOE` must be set or PWM outputs won't appear. This is already handled in `pwm_init()`.
- **ISR attribute**: All ISRs must use `__attribute__((interrupt))` for correct RISC-V hardware stacking on CH32V003.
- **Log dimming table**: 508 bytes in flash. Changing PWM frequency (ATRLR) requires regenerating the table. Use: `python -c "for i in range(1,255): x=10**((i-1)*3/253-1); print(round(x/100*ATRLR))"`
- **EEPROM storage**: All persistent config is now in AT24C256 EEPROM. Internal flash page 0x08003FC0 is no longer used — full 16 KB available for firmware code.

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

### DALI Bootloader — Original Vendor Protocol (WORKING)
- Location: `DALI_Bootloader/`
- Size: ~1616/1920 bytes (84% of boot area)
- Protocol: standard DALI 16-bit forward frames with vendor-specific commands (IEC 62386-102, bytes 129–143)
- Commands: CMD_ERASE (0x84), CMD_DATA (0x85), CMD_COMMIT (0x86), CMD_BOOT (0x87)
- Two-frame data transfer: CMD_DATA sets flag, next frame's data byte is firmware byte (~11 min for 10 KB)
- I2C EEPROM staging: firmware received via DALI is stored in AT24C256 EEPROM, then copied to flash on CMD_COMMIT
- Software entry: DALI cmd 131 (vendor-reserved, config repeat) writes RAM magic word, resets into bootloader
- Hardware entry: hold PA1 LOW during reset
- Build: `DALI_Bootloader/build.bat`
- Flowchart: `DALI_Bootloader/bootloader_protocol.png` (+ `.mmd` source)

### DALI Bootloader — IEC 62386-105 Compatible (NEW)
- Location: `DALI_Bootloader/`
- Size: ~1876/1920 bytes (97.7% of boot area)
- Protocol: 32-bit forward frames (IEC 62386-101, 7.4.3) for bulk data, standard IEC 62386-105 commands
- Bulk transfer uses TRANSFER BLOCK DATA (0xBD) — 3 firmware bytes per frame, no per-frame response (~2.5 min for 10 KB, ~4.5x faster)
- **Block 0 validation**: GTIN (6 bytes) + Device key byte 0 (EVG mode ID) compared against EEPROM identity block. Mismatch → QUERY BLOCK FAULT returns YES → master aborts
- Block 1..n firmware data extracted from block structure (headers/CRCs skipped, no CRC verification)
- I2C EEPROM staging: firmware stored in AT24C256 staging area (0x0080+), copied to flash on FINISH FW UPDATE
- Reads device identity (GTIN, EVG mode, short address) from EEPROM identity block (0x0000), written by firmware at boot
- Standard commands: START FW TRANSFER, FINISH FW UPDATE, RESTART FW, QUERY FW UPDATE RUNNING, QUERY BLOCK FAULT
- QUERY FW UPDATE FEATURES handled by firmware (responds 0x00 = cancel not supported)
- Software entry: firmware handles START FW TRANSFER (32-bit, config repeat), responds YES, writes magic word, resets. Bootloader detects magic word → update mode enabled immediately
- Hardware entry: hold PA1 LOW during reset → bootloader responds YES to any subsequent START FW TRANSFER
- Auto-reboots after successful FINISH FW UPDATE (no separate RESTART FW needed)
- Compatible with standard DALI firmware update masters
- Build: `DALI_Bootloader/build.bat`
- Flowchart: `DALI_Bootloader/bootloader_protocol.png` (+ `.mmd` source)
- See `DALI_Bootloader/README.md` for full protocol details

### TODO: OpenKNX GW-REG1-Dali as DALI upload master
- The **umbau** branch of [GW-REG1-Dali](https://github.com/OpenKNX/GW-REG1-Dali) (ESP32 variant) has a WebSocket JSON interface that can send arbitrary DALI frames with backward frame listening — no firmware changes needed
- Write a Python WebSocket client to orchestrate the bootloader upload protocol via the gateway
- Blocked on: ESP32 gateway hardware arrival

### Pin Assignments (Bootloader vs Firmware)

Both the DALI bootloader (`DALI_Bootloader/dali_bootloader.c`) and the firmware use the same DALI bus and EEPROM pins. The USB bootloader uses PD0/PD3/PD4 for USB.

| Pin | Bootloader | Firmware |
|-----|-----------|----------|
| PA1 | Boot button (active low) | -- |
| PC1 | I2C SDA (EEPROM) | I2C SDA (EEPROM) |
| PC2 | I2C SCL (EEPROM) | I2C SCL (EEPROM) |
| PC3 | DALI RX | DALI RX (EXTI3) |
| PC4 | DALI TX | DALI TX |
| PC6 | -- | TIM1_CH1 (Red) / SPI1_MOSI |
| PC7 | -- | TIM1_CH2 (Green) |
| PC0 | -- | TIM1_CH3 (Blue) |
| PD3 | -- | TIM1_CH4 (White) |
| PA2 | -- | PSU_CTRL |
| PD0 | USB DPU pull-up (USB BL) | -- |
| PD2 | USB D- (USB BL) | -- |
| PD4 | USB D+ (USB BL) | -- |
| PD5 | -- | Debug UART TX (115200) |

**WARNING**: Do not connect an EVG to the DALI bus AND USB simultaneously. USB should only be used for firmware upload while the device is disconnected from the DALI bus.

## Testing

HIL test setup and test tooling live in `Debug_Helpers/`. The single source of truth for the physical setup (target, programmer, DALI bus, gateway, KNX/IP router, logic analyzer, UART logger) is **`Debug_Helpers/HIL_Setup.md`** — read it first when picking up a debug session.

Subdirectories:

| Path | Purpose |
|------|---------|
| `Debug_Helpers/HIL_Setup.md` | Pin map, IPs, tool paths, sample-rate guidance |
| `Debug_Helpers/uart_logger.py` | Continuous COM14 capture to `logs/uart_*.log` |
| `Debug_Helpers/PWM_Test/` | Standalone CH32V003 GPIO toggle firmware + Python `send_colors.py` (KNX→Gateway→DALI→PWM end-to-end test) |
| `Debug_Helpers/DALI_RX_Test/` | Python WS-direct DALI test scripts. `full_update.py` drives a full IEC-62386-105 firmware update with Fletcher-16 in Block 0 and periodic mid-transfer QUERY BLOCK FAULT polling |
| `Debug_Helpers/EEPROM_Dump/` | Standalone CH32V003 firmware that streams all 32 KB of AT24C256 EEPROM over UART, plus `check_eeprom_dump.py` to verify identity block + firmware-staging byte-for-byte against `firmware.bin`. The end-of-chain verification step after a DALI BL update |
| `Debug_Helpers/DALI_Sniffer_Host/` | USB sniffer host app |
| `Debug_Helpers/DALI_UART_Sniffer/` | UART-based DALI sniffer |
| `Debug_Helpers/EEPROM_Test/` | Standalone CH32V003 I2C EEPROM DMA test |
| `Debug_Helpers/dali_monitor.py` | Read-only WebSocket monitor for the gateway's daliMonitor stream, decodes specials/short-addr/broadcast and tees to `logs/dali_<ts>.log` |
| `Debug_Helpers/logs/` | UART log archive |

### End-to-end DALI bootloader verification

After a DALI-bus firmware update, the full chain (KNX → gateway → DALI → BL RX → page_buf → I2C → EEPROM staging → flash commit → user FW boots) can be byte-for-byte verified:

1. **Build the firmware** with a `Build: __DATE__ __TIME__` print at boot (already in `main.c`).
2. **Run the update**: `cd Debug_Helpers/DALI_RX_Test && python full_update.py`. Includes Fletcher-16 in Block 0 and periodic QUERY BLOCK FAULT every 500 frames.
3. **Watch the boot banner** in the UART log — the new `Build:` timestamp must match the just-built `firmware.bin`'s mtime.
4. **Dump the EEPROM**: `cd Debug_Helpers/EEPROM_Dump && pio run -t upload`. Overwrites user flash with the dumper, which streams all 32 KB of EEPROM to UART in ~14 s.
5. **Compare**: `python check_eeprom_dump.py` parses the latest `uart_*.log`, reconstructs the 32 KB image, and verifies identity block (magic/GTIN/mode/short_addr) + firmware-staging area at `0x0080+` byte-equal to `firmware.bin`. PASS = chain verified.
6. **Recover**: re-flash the EVG firmware via `pio run -t upload` (in `Firmware/`) or via DALI BL again.

## Resource Usage (RGBW default)

- Flash: 10,588 B (64.6% of 16 KB) — includes I2C EEPROM driver
- RAM: 136 B (6.6% of 2 KB)

## Notes on __WFI() / Sleep

`__WFI()` **with a bus-idle guard** works correctly. The guard in `main.c` only enters sleep when both:
1. TX state machine is idle (`dali_is_tx_idle()`)
2. No RX edge in the last 20 ms (`millis() - dali_last_rx_edge_ms() > DALI_WFI_IDLE_MS`)

This prevents WFI from being entered during active frame reception or transmission. During DALI idle periods (seconds to minutes between commands), the CPU sleeps most of the time.

**Root cause of the original failure**: `__WFI()` entered between edges of a DALI frame (during 2Te inter-edge gaps). SysTick (1 ms) wakes the CPU, but the accumulated timing errors over many frames eventually corrupt the TX state machine. The simple `dali_is_tx_idle()` guard was insufficient — it only checked TX state, not whether RX was actively receiving.

CH32V003 Sleep mode (SLEEPDEEP=0, PFIC_SCTLR=0x00): core stops, all peripherals run (TIM2, EXTI, SysTick continue). Safe for DALI. Do **not** use Standby mode (SLEEPDEEP=1) — TIM2 stops and DALI edge timing breaks.
