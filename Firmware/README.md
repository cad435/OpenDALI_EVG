# DALI-2-compatible EVG Firmware

DALI-2-compatible control gear (slave) firmware for the **CH32V003F4U6** RISC-V microcontroller, built on [cnlohr's ch32fun](https://github.com/cnlohr/ch32v003fun) framework. Drives up to 4 PWM channels (RGBW) with full IEC 62386 protocol support, DT8 colour control, and EEPROM persistence — ~11 KB of code (RGBW build).

> Trademark notice — see [root README](../README.md): *DALI*, *DALI-2* etc. are DiiA trademarks; this project is an independent IEC 62386 implementation, not DiiA-certified.

![Firmware Architecture](firmware_architecture.png)

## EVG Modes

The firmware supports 8 LED output modes, selected via a single `EVG_MODE_xxx` define in `hardware.h` (or `-DEVG_MODE_xxx` compiler flag). All internal configuration (DALI device type, channel count, driver, DT8 features) is derived automatically.

![EVG Mode Feature Matrix](evg_mode_switch.png)

| Mode | DT | Channels | Driver | Tc | Primary | Flash (approx) |
|------|-----|----------|--------|-----|---------|---------------|
| `EVG_MODE_ONOFF` | 6 | 0 | PSU_CTRL only | - | - | ~9 KB |
| `EVG_MODE_SINGLE` | 6 | 1 PWM | TIM1 | - | - | ~10 KB |
| `EVG_MODE_CCT` | 8 | 2 PWM | TIM1 | yes | no | ~11 KB |
| `EVG_MODE_RGB` | 8 | 3 PWM | TIM1 | yes | yes | ~11 KB |
| `EVG_MODE_RGBW` | 8 | 4 PWM | TIM1 | yes | yes | **11.2 KB (default)** |
| `EVG_MODE_WS2812` | 8 | 3 (GRB) | SPI+DMA | yes | yes | ~11.6 KB |
| `EVG_MODE_SK6812_RGB` | 8 | 3 (GRB) | SPI+DMA | yes | yes | ~11.6 KB |
| `EVG_MODE_SK6812_RGBW` | 8 | 4 (GRBW) | SPI+DMA | yes | yes | ~11.7 KB |

Default: `EVG_MODE_RGBW`. ONOFF mode compiles out all LED drivers, log table, and TIM1 — only PSU_CTRL (PA2) switches on/off. PHY_MIN = 254 (any non-zero arc level → full on). SINGLE mode compiles out all DT8 code (~1 KB savings).

## Features

- **IEC 62386-101** — Manchester-encoded physical layer at 1200 baud with DALI PHY transceiver, bus collision detection
- **IEC 62386-102** — Full DALI protocol: addressing, arc power, fading, scenes, groups, configuration
- **IEC 62386-209 (DT8)** — RGBW colour control with colour temperature (Tc) support
- **Logarithmic dimming** — IEC 62386-102 compliant 254-step lookup table
- **Flash persistence** — All configuration survives power cycles (deferred write with 5s debounce)
- **On/off mode** — PSU_CTRL-only relay/switch output, no timers or PWM
- **20 kHz PWM** — 4 channels via TIM1 with 2400-step resolution (11.2 bit); dynamic frequency shift to 6.67 kHz at deep dimming levels (keeps every pulse ≥ 250 ns for the isolator/gate-driver chain, duty unchanged; engages only while every channel is ≤ 3.75 % duty to avoid audible MLCC whine) plus a 250 ns minimum-pulse failsafe clamp
- **WS2812/SK6812 support** — Alternative digital LED output via SPI1+DMA on PC6. The core must not sleep during a transfer (`__WFI()` stalls SPI/DMA on this part), so `led_driver_busy()` guards the sleep condition; strip data is re-sent every `LED_REFRESH_MS` (default 1 s) purely as glitch recovery.

## What Works

| Area | Status |
|------|--------|
| Forward frame RX (16-bit and 32-bit Manchester decode) | Working |
| Backward frame TX (8-bit Manchester encode) | Working |
| Short address, group, and broadcast addressing | Working |
| Full addressing protocol (INITIALISE, RANDOMISE, SEARCHADDR, PROGRAM) | Working |
| Arc power commands (DAPC, OFF, UP/DOWN, STEP, RECALL, SCENE) | Working |
| Fade engine (fadeTime + fadeRate + extended fade time) | Working |
| Configuration commands (42-128) with config repeat validation | Working |
| All standard queries (144-199) | Working |
| READ MEMORY LOCATION (cmd 197) → Memory Bank 0 (read-only gear info) | Working — spec-conformant DTR roles since v0.3 (DTR1=bank, DTR0=offset post-inc, DTR2=look-ahead) per IEC 62386-102:2009 §11.x cmd 197 |
| Structured frame type (`dali_frame_t` with FORWARD/BACKWARD/ERROR/COLLISION/ECHO flags) | Working |
| TX echo / collision detection (via DALI PHY) | Working |
| Status byte (resetState, powerCycleSeen, lampOn, fadeRunning) | Working |
| 16 scenes, 16 groups | Working |
| DT8 RGBW colour control (SET TEMP RGB/WAF, ACTIVATE) | Working |
| DT8 colour temperature with Tc-to-RGBW conversion (2700K-6500K) | Working |
| DT8 queries (247-252) | Working |
| NVM persistence via I2C EEPROM (AT24C256, all config + colour + identity) | Working |
| PSU control output (PA2, auto on/off) | Working |
| WS2812/SK6812 digital LED strip output (SPI1+DMA) | Working — verified on hardware with a logic analyser (bit-exact frames on the wire) |
| DALI PHY transceiver mode (TX/RX with collision detection) | Working |
| IEC 62386-105 START FW TRANSFER (32-bit frame) → bootloader entry | Working |

## What Doesn't Work / Not Implemented

| Area | Reason |
|------|--------|
| Memory bank 1 + write access | Read-only bank 0 only; writable banks not implemented |
| DALI-2 diagnostic queries (166-175) | Require hardware monitoring circuitry (current/voltage sensing) |
| CIE xy chromaticity | Requires per-LED spectral calibration |
| ENABLE DAPC SEQUENCE (cmd 9) | Rarely used, complex timing |
| Tc temperature limits (store/query) | Not yet implemented |
| controlGearFailure / lampFailure status bits | Require hardware monitoring (I2C ADC planned) |

## Hardware

CH32V003F4U6 (QFN20, 48 MHz, 16 KB Flash, 2 KB RAM) with DALI PHY transceiver. TIM1 Partial Remap 1 (`RM=01`) enables PC6 dual-use for both PWM and WS2812.

| Pin | Function | Peripheral | Notes |
|-----|----------|------------|-------|
| PA1 | Boot button | GPIO input | Active low at reset → enter DALI bootloader (firmware also reroutes into BL when PA1 is held during NRST/SYSRESETREQ) |
| PA2 | PSU Control | GPIO output | HIGH = external PSU on, LOW = off |
| PC0 | LED3 / Blue PWM | TIM1_CH3 | 20 kHz, 2400-step |
| PC1 | I2C SDA | I2C1 | Reserved for AT24C256C EEPROM |
| PC2 | I2C SCL | I2C1 | Reserved for AT24C256C EEPROM |
| PC3 | DALI RX | EXTI3 | From PHY RX_OUT, both-edge interrupt |
| PC4 | DALI TX | GPIO output | To PHY TX_IN, Manchester encode |
| PC5 | SPI1 SCK → J4.3 | SPI1 | SPI clock to the load board (ribbon J4 pin 3); unused in direct-PWM modes |
| PC6 | LED1 / Red PWM **or** WS2812 | TIM1_CH1 / SPI1_MOSI | Dual-use: PWM or SPI+DMA (compile-time) |
| PC7 | LED2 / Green PWM | TIM1_CH2 | 20 kHz, 2400-step |
| PD0 | *(spare)* | — | Free GPIO (was USB D+ pull-up in old USB-BL prototype; current BL is DALI-only) |
| PD1 | SWDIO | SWD | Single-wire debug |
| PD2 | *(spare)* | — | Free GPIO |
| PD3 | LED4 / White PWM | TIM1_CH4 | 20 kHz, 2400-step |
| PD4 | *(spare)* | — | Free GPIO |
| PD5 | Debug UART TX | USART1_TX | 115200 baud |
| PD6 | Debug UART RX | USART1_RX | 115200 baud |
| PD7 | NRST | Reset | Hardware reset |

See [Hardware/README.md](../Hardware/README.md) for full board details.

## Build & Flash

```bash
pio run                    # Build
pio run -t upload          # Flash via PlatformIO
# or directly:
wlink flash .pio/build/genericCH32V003F4P6/firmware.bin
```

## Bulk Provisioning of Blank EVGs (`flash_blank_evg.ps1`)

`flash_blank_evg.ps1` runs the full first-time provisioning sequence in a loop and is the one-stop tool for programming blank CH32V003-based EVG boards.

For every chip it performs:

1. Bootloader (`Bootloader/dali_bootloader.bin`) → `0x1FFFF000` (boot area, 1920 B fixed)
2. configurebootloader (`Bootloader/configurebootloader.bin`) → `0x08000000` (runs once, writes the option bytes that enable boot-from-bootloader)
3. Firmware (PlatformIO `pio run -t upload`) → `0x08000000` (overwrites configurebootloader)
4. Verify option bytes by reading `0x1FFFF800` and `FLASH_OBR` (`0x4002201C`):
   - `RDPR == 0xA5`, `RDPR + ~RDPR == 0xFF`
   - `USER == 0xEF`, `USER + ~USER == 0xFF`
   - `OPTERR == 0`, `RDPRT == 0`, `STARTMODE == 1`

The loop polls `wlink status` once per second; when a CH32V003 is detected it runs all four steps and prints **PASS** (green) with the elapsed time, or **FAIL** (red) with the failing step. After completion it waits for the chip to be disconnected before accepting the next one. **ESC** exits cleanly. On hosts that don't support raw-key polling the loop falls back to **Ctrl+C**.

### Prerequisites

- **WCH-LinkE programmer** connected to the host
- **PlatformIO CLI** installed at `%USERPROFILE%\.platformio\penv\Scripts\platformio.exe` (default location)
- **wlink** at `%USERPROFILE%\.platformio\packages\tool-wlink\wlink.exe` (installed automatically by the `ch32v` PlatformIO platform)
- Pre-built artefacts present:
  - `../Bootloader/dali_bootloader.bin` (build via `Bootloader/build.bat` once)
  - `../Bootloader/configurebootloader.bin` (shipped with the bootloader)
- The script auto-builds the firmware via `pio run -t upload` — no separate build step needed

### Usage

```powershell
powershell -ExecutionPolicy Bypass -File flash_blank_evg.ps1
```

Connect chips one by one. Typical wall-clock time per chip: **~8 seconds**.

## Documentation

- [Commands_Implemented.md](Commands_Implemented.md) — Full command-by-command status table
- [firmware_architecture.mmd](firmware_architecture.mmd) — Mermaid source for architecture diagram
- [evg_mode_switch.mmd](evg_mode_switch.mmd) — Mermaid source for EVG mode feature matrix
- [test/](test/) — HIL test setup and test scripts

## License

MIT
