# HIL Test Setup — DALI EVG (ch32fun)

## Overview

```
 PC
 ├── COM9  (Pico USB)  ── RP2040 DALI Master ──┐
 │                          GPIO17 (TX) ────100R────── PC0 (RX)   CH32V003 DALI EVG (DUT)
 │                          GPIO16 (RX) ────100R────── PC5 (TX)
 │                          GND ────────────────────── GND
 └── COM11 (WCH-Link)  ── CH32V003 serial debug (PD5 TX via WCH-Link UART)
                           + programming (SWD)
```

## Components

| Component | Details |
|-----------|---------|
| **DALI Master** | Raspberry Pi Pico (RP2040), firmware: `DALI-Master/`, COM9 |
| **DALI Slave (DUT)** | CH32V003F4P6 WCH-Eval board, firmware: `Firmware/`, COM11 |
| **Programmer** | WCH-LinkE (CH32V305), provides SWD + UART + reset for CH32V003 |
| **Signal lines** | 2x 100R resistors on TX/RX lines (current limiting, no PHY) |
| **Logic Analyzer** | Saleae clone (fx2lafw), sigrok-cli at `N:\Programme\sigrok\sigrok-cli\sigrok-cli.exe`, 500 kHz single-ch / 100 kHz multi-ch (CSV bottleneck) |

## CH32V003F4P6 Pinout (TSSOP-20)

```
                        CH32V003F4P6 (TSSOP-20)
                     ┌────────┴────────┐
                     │                 │
     (USB D+)    PD4 │ 1            20 │ PD3    (USB D-)
     UART TX     PD5 │ 2            19 │ PD2    LED1 (R) TIM1_CH1
     (UART RX)   PD6 │ 3            18 │ PD1    SWDIO
     NRST        PD7 │ 4            17 │ PC7    Boot button (active low)
     LED2 (G)    PA1 │ 5            16 │ PC6    WS2812 data (SPI1_MOSI)
     PSU_CTRL    PA2 │ 6            15 │ PC5    DALI TX
     GND         VSS │ 7            14 │ PC4    LED4 (W) TIM1_CH4
     USB DPU     PD0 │ 8            13 │ PC3    LED3 (B) TIM1_CH3
     VDD     3.3/5V  │ 9            12 │ PC2    I2C1_SCL (reserved)
     DALI RX     PC0 │10            11 │ PC1    I2C1_SDA (reserved)
                     │                 │
                     └─────────────────┘

  External connections:
     PC0 (pin 10) ──100R── Pico GPIO17 (master TX)
     PC5 (pin 15) ──100R── Pico GPIO16 (master RX)
     PD5 (pin  2) ──────── WCH-LinkE UART bridge
     PD1 (pin 18) ──────── WCH-LinkE SWDIO

  Free GPIOs:  PD6 (+ PD3, PD4 when USB not in use)
  Clock:       48 MHz from internal 24 MHz HSI + PLL (no crystal)
```

## Pin Mapping

| Signal | Master (Pico) | Slave (CH32V003) | LA Channel | Notes |
|--------|---------------|------------------|------------|-------|
| DALI TX→RX | GPIO17 | PC0 (EXTI0) | D0 | Master TX to slave RX, 100R series |
| DALI RX←TX | GPIO16 | PC5 (GPIO) | D1 | Slave backward frame to master RX, 100R series |
| LED PWM 1 (R) | -- | PD2 (TIM1_CH1) | D4 | 20 kHz PWM, 2400 steps |
| LED PWM 2 (G) | -- | PA1 (TIM1_CH2) | D5 | 20 kHz PWM, 2400 steps |
| LED PWM 3 (B) | -- | PC3 (TIM1_CH3) | D6 | 20 kHz PWM, 2400 steps |
| LED PWM 4 (W) | -- | PC4 (TIM1_CH4) | D7 | 20 kHz PWM, 2400 steps |
| PSU control | -- | PA2 (GPIO) | D3 | HIGH when any channel on |
| WS2812 data | -- | PC6 (SPI1_MOSI) | -- | Alt to PWM via `#define DIGITAL_LED_OUT` |
| Boot button | -- | PC7 (input, pull-up) | -- | Active low, enters bootloader |
| USB D+ | -- | PD4 | -- | USB bootloader only |
| USB D- | -- | PD3 | -- | USB bootloader only |
| USB DPU | -- | PD0 (output) | -- | USB D+ pull-up, driven by bootloader |
| I2C SDA | -- | PC1 (I2C1_SDA) | -- | Reserved for EEPROM |
| I2C SCL | -- | PC2 (I2C1_SCL) | -- | Reserved for EEPROM |
| GND | GND | GND | -- | Common ground required |
| Debug serial | -- | PD5 (USART1_TX) | -- | Via WCH-Link UART bridge, 115200 baud |

## GPIO Inversion (No PHY)

Direct GPIO-to-GPIO connection without DALI transceiver. The master firmware applies pad-level inversion for correct polarity:

```cpp
// In DALI-Master/include/hardware.h:
#define DALI_NO_PHY
```

Remove `#define DALI_NO_PHY` when switching to a real DALI bus with transceiver.

## Test Commands (via COM9 serial at 115200 baud)

```
broadcast <level>          Set all devices to level (0-254)
arc <addr> <level>         Set short address to level
on <addr>                  Recall max level
off <addr>                 Turn off
query <addr> <cmd>         Query command (waits for backward frame)
querybc <cmd>              Query broadcast command
scan                       Scan addresses 0-63
initialise <param>         INITIALISE (0=unaddressed, 0xFF=all)
randomise                  RANDOMISE
searchaddr <H> <M> <L>    Set search address
compare                    COMPARE (expect YES)
setaddr <addr>             PROGRAM SHORT ADDRESS
withdraw                   WITHDRAW
terminate                  TERMINATE
assign <addr>              Auto-assign address (single device)
raw <hex16>                Send raw 16-bit DALI frame
help                       Show all commands
```

## Flashing

```bash
# Pico (auto-detects COM9, reboots to BOOTSEL):
cd DALI-Master && pio run -t upload

# CH32V003 (via WCH-Link):
wlink.exe flash Firmware/.pio/build/genericCH32V003F4P6/firmware.bin

# CH32V003 (via DALI bus bootloader):
cd DALI_Bootloader && powershell -ExecutionPolicy Bypass -File dali_upload.ps1
```

## Firmware Info

| Property | Value |
|----------|-------|
| Framework | ch32fun (cnlohr/ch32fun) |
| Flash | 9,712 B (59.3% of 16 KB) |
| RAM | 136 B (6.6% of 2 KB) |
| Clock | 48 MHz (HSI + PLL) |
| PWM channels | 4 (configurable via `PWM_NUM_CHANNELS`) |
| Dimming curve | IEC 62386-102 §9.3 logarithmic |
| UART | 115200 baud on PD5 (printf via ch32fun UARTPRINTF) |
| NVM | Last 64-byte flash page (0x08003FC0), deferred 5s write |

## Helper Scripts (in `Debug_Helpers/`)

| Script | Purpose |
|--------|---------|
| `ch32fun_test.ps1` | Forward + backward + PWM test |
| `ch32fun_assign_test.ps1` | Short address assignment test |
| `ch32fun_compliance_test.ps1` | DALI-1/2 compliance (min/max, scenes, groups, RESET, DTR) |
| `ch32fun_nvm_test.ps1` | NVM flash persistence (config, reset via wlink, verify) |
| `ch32fun_newcmd_test.ps1` | New commands test (queries 146-149, status flags, cmd 48) |
| `ch32fun_dt8_test.ps1` | DT8 colour control (RGBW, Tc, queries) |
| `ch32fun_la_test.ps1` | LA signal verification |
| `dali_timing_verify.ps1` | IEC 62386-101 timing compliance |
| `dali_logdim_test.ps1` | IEC 62386-102 logarithmic dimming test |
| `la_channel_test.py` | LA channel self-test (all 8 channels) |
| `la_pwm_duty.ps1` | LA PWM duty cycle measurement (D4–D7, per-channel) |
| `la_pwm_test.ps1` | LA PWM edge detection (D4–D7, per-channel) |
| `read_pico.ps1` | Read Pico serial (COM9) |
| `read_ch32.ps1` | Read CH32 serial (COM11) |
| `reset_and_read.ps1` | Reset CH32 via wlink + read serial |
