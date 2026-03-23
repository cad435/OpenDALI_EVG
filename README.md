# OpenDALI_EVG

A completely open-source DALI-2 Electronic Control Gear (EVG) for controlling LEDs. Built around the **CH32V003** RISC-V microcontroller.

> [!WARNING]
> This project is a work in progress and not yet ready for production use. Hardware designs and firmware are subject to change.

## Overview

This project implements a DALI-2 control gear device compliant with **IEC 62386-101** (bus/protocol) and **IEC 62386-102** (control gear commands), including **DT8 colour control** (IEC 62386-209) for RGBW and colour temperature mixing.

Key highlights:
- Full DALI-2 protocol stack in bare-metal C, under 10 KB flash
- 4-channel PWM output (RGBW) at 20 kHz with 2400-step resolution
- WS2812/SK6812 addressable LED strip support (SPI+DMA, up to ~300 LEDs)
- Logarithmic dimming curve per IEC 62386-102
- 16 scenes, 16 groups, fade engine with configurable fade time/rate
- Full commissioning support (INITIALISE, RANDOMISE, SEARCHADDR, PROGRAM)
- All configuration persisted to flash (survives power cycles)
- Bus-powered design possible (< 2 mA quiescent from DALI bus)
- Over-the-air firmware updates via DALI bus (DALI bootloader)

## Project Structure

```
OpenDALI_EVG/
├── Firmware/           CH32V003 DALI slave firmware (PlatformIO project)
├── DALI_Bootloader/    Firmware-over-DALI bootloader (976 bytes, experimental)
├── USB_Bootloader/     USB HID bootloader (cnlohr ch32v003fun, pre-built binaries)
├── DALI-Master/        Pico-based DALI master for testing
├── Debug_Helpers/      Test scripts and tools (PowerShell, Python)
├── Hardware/           PCB Schematics and Gerbers
└── Simulationen/       LTspice PHY and power supply simulations
```

### Firmware

The firmware is a standalone PlatformIO project targeting the CH32V003F4P6, built on [ch32v003fun](https://github.com/cnlohr/ch32v003fun). Supports 7 LED output modes selected via a single `EVG_MODE_xxx` define:

- **PWM modes**: SINGLE (1ch), CCT (2ch), RGB (3ch), RGBW (4ch) — TIM1 at 20 kHz, 2400-step resolution
- **Digital LED modes**: WS2812, SK6812_RGB, SK6812_RGBW — SPI1+DMA on PC6

See [Firmware/README.md](Firmware/README.md) for architecture, commands, and test documentation.

### DALI Bootloader

Firmware-over-DALI-bus bootloader fitting in the 1920-byte boot area. Allows firmware updates via the DALI bus without a programmer — uses standard DALI forward frames with vendor-specific command bytes. See [DALI_Bootloader/README.md](DALI_Bootloader/README.md).

### USB Bootloader

USB HID bootloader from [cnlohr's ch32v003fun](https://github.com/cnlohr/ch32v003fun/tree/master/bootloader). Pre-built binaries included; source is git-ignored. See [USB_Bootloader/README.md](USB_Bootloader/README.md).

### Hardware

PCB designs (schematics, Gerbers, JLCPCB assembly files). See [Hardware/README.md](Hardware/README.md) for board descriptions and details.

- **Controller** — DALI PHY + CH32V003 MCU board
- **LoadBoard 250W** — 4-channel RGBW LED driver + AC mains switching (ACST410 triac, MOC3043)

### Simulations

LTspice simulations for the DALI PHY layer:
- **Phy_Non-Isolated.asc** — Bus-powered PHY with direct MCU connection
- **Phy_Isolated.asc** — Optocoupler-isolated PHY variant

## Hardware Platform

| Parameter | Value |
|-----------|-------|
| MCU | CH32V003F4P6 (RISC-V, 48 MHz, 16 KB Flash, 2 KB RAM) |
| PWM Channels | 4 (RGBW via TIM1) |
| Digital LED | WS2812/SK6812 via SPI1+DMA (alt to PWM, up to ~300 LEDs) |
| DALI Interface | GPIO-based Manchester encode/decode (no dedicated transceiver IC) |
| Supply | Bus-powered from DALI (with DC-DC converter) or external |

### Wiring

Connect the DALI bus to the PHY circuit (see Simulations for reference designs), then wire RX/TX to the MCU:
- **PC0** — DALI RX input
- **PC5** — DALI TX output
- **PD2, PA1, PC3, PC4** — PWM outputs (CH1-CH4)
- **PC6** — WS2812/SK6812 data (SPI1_MOSI, alternative to PWM)
- **PA2** — External PSU control (optional)
- **PC7** — Boot button (active low, enters bootloader)

## Status

The firmware is functional and tested with a custom Pico-based DALI master. See [Firmware/Commands_Implemented.md](Firmware/Commands_Implemented.md) for a full command-by-command status table.

## License

MIT
