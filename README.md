# OpenDALI_EVG

A completely open-source Proof of concept for a ultra low standby current **DALI-2-compatible** Electronic Control Gear (ECG/EVG) for controlling LEDs. Built around the **CH32V003** RISC-V microcontroller. Implements IEC 62386 — not certified by, affiliated with, or endorsed by the DALI Alliance (DiiA).

> [!WARNING]
> This project is a work in progress and not yet ready for production use. Hardware designs and firmware are subject to change.

> [!NOTE]
> **Trademark notice:** *DALI*, *DALI-2*, *D4i*, *DALI+* and *DiiA* are trademarks of the Digital Illumination Interface Alliance (DiiA). This project is an independent open-source implementation and compatible with the IEC 62386 standard. It is referred to as *DALI-2-compatible*, it has not undergone DiiA certification and may not be marketed as a DALI product.
> Use for educational purposes only!

<p align="center">
  <img width="826" height="344" alt="WhatsApp Image 2026-07-07 at 23 02 12"
src="https://github.com/user-attachments/assets/9caa53ac-3016-4afd-b496-2ffd4a69d20e" />
</p>
<p align="center">
  <img width="622" height="219" alt="WhatsApp Image 2026-07-07 at 23 02 12 (1)"
src="https://github.com/user-attachments/assets/420c99ae-399b-4e93-990c-1786f9e44231" />
</p>

## Overview

This project implements a DALI-2-compatible control gear device per **IEC 62386-101** (bus/protocol) and **IEC 62386-102** (control gear commands), including **DT8 colour control** (IEC 62386-209) for RGBW and colour temperature mixing.

Key highlights:
- Full DALI-2-compatible protocol stack in bare-metal C, ~11 KB user flash (16 KB chip)
- 4-channel PWM output (RGBW) at 20 kHz with 2400-step resolution
- WS2812/SK6812 addressable LED strip support (SPI+DMA, up to ~300 LEDs)
- Logarithmic dimming curve per IEC 62386-102
- 16 scenes, 16 groups, fade engine with configurable fade time/rate
- Full commissioning support (INITIALISE, RANDOMISE, SEARCHADDR, PROGRAM SHORT) — incl. spec-conformant DTR roles for memory bank reads (FW v0.3+)
- All configuration persisted to AT24C256 EEPROM (deferred-write, 5 s debounce)
- Bus-powered: the MCU runs entirely from the DALI bus (< 2 mA quiescent)
- Over-the-air firmware updates via DALI bus (IEC 62386-105 bootloader with Fletcher-16 payload check, auto-resume on fault)
- Verified interop with Mi-Boxer DALI controllers and OpenKNX GW-REG1-Dali gateway

## Project Structure

```
OpenDALI_EVG/
├── Firmware/           CH32V003 DALI slave firmware (PlatformIO project, ~11.2 KB user flash)
├── Bootloader/         IEC 62386-105 compatible DALI bootloader (1896 / 1920 B, I2C EEPROM staging)
├── EVG-Updater/        C# WinForms tool (.NET 8) — GUI + CLI for firmware flash and bus scan
├── Hardware/           PCB schematics, Gerbers, JLCPCB BOM/CPL (Controller V0.2 + LoadBoard V0.1)
└── Simulations/        LTspice PHY and power supply simulations
```

### Firmware

The firmware is a standalone PlatformIO project targeting the CH32V003F4U6, built on [ch32v003fun](https://github.com/cnlohr/ch32v003fun). Supports 7 LED output modes selected via a single `EVG_MODE_xxx` define:

- **PWM modes**: SINGLE (1ch), CCT (2ch), RGB (3ch), RGBW (4ch) — TIM1 at 20 kHz, 2400-step resolution
- **Digital LED modes**: WS2812, SK6812_RGB, SK6812_RGBW — SPI1+DMA on PC6

See [Firmware/README.md](Firmware/README.md) for architecture, commands, and test documentation.

### Bootloader

IEC 62386-105 compatible firmware-over-DALI-bus bootloader (1896 / 1920 bytes, 98.8% of the CH32V003 boot area). Uses 32-bit forward frames for bulk data transfer (3 bytes/frame, ~2.5 min for ~11 KB). Firmware is staged in AT24C256 I2C EEPROM before committing to flash. Validates Block 0 GTIN + EVG mode ID, and verifies the Block 1 firmware payload with a Fletcher-16 checksum at FINISH. On checksum mismatch the BL auto-resumes the previous user firmware (no manual recovery needed). See [Bootloader/README.md](Bootloader/README.md).

### EVG-Updater

C# WinForms application (.NET 8) — GUI + CLI for both **firmware flashing** (`flash <bin>`) and **bus discovery** (`scan`). The canonical update client; replaces the Python reference implementation for routine use. Talks to the OpenKNX GW-REG1-Dali gateway via WebSocket. See [EVG-Updater/README.md](EVG-Updater/README.md).

### Hardware

PCB designs (schematics, Gerbers, JLCPCB assembly files). See [Hardware/README.md](Hardware/README.md) for board descriptions and details.

- **Controller** — DALI-compatible PHY + CH32V003 MCU board
- **LoadBoard 250W** — 4-channel RGBW LED driver + AC mains switching (ACST410 triac, MOC3043)

**Design goals:**
- **Ultra-low standby (~30 mW):** When LEDs are off, the triac disconnects the external PSU entirely from mains — eliminating its standby consumption. The Controller remains fully operational, powered solely from the DALI bus (< 2 mA).
- **Modular architecture:** The Controller is a standalone unit handling all communication and signal generation. LoadBoards are interchangeable and connect via a standardized FFC interface — swap them to match different power classes without changing the Controller.

### Simulations

LTspice simulations for the DALI-compatible PHY layer:
- **Phy_Non-Isolated.asc** — Bus-powered PHY with direct MCU connection
- **Phy_Isolated.asc** — Optocoupler-isolated PHY variant

## Hardware Platform

| Parameter | Value |
|-----------|-------|
| MCU | CH32V003F4U6 (RISC-V, 48 MHz, 16 KB Flash, 2 KB RAM) |
| DALI-compatible Interface | Via PHY transceiver (PC3 RX, PC4 TX) |
| PWM Channels | 4 (RGBW via TIM1, partial remap 1) |
| Digital LED | WS2812/SK6812 via SPI1+DMA (same pin as PWM CH1, compile-time select) |
| Supply | Bus-powered from the DALI bus only (DC-DC converter) — MCU is always bus-powered |

See [Hardware/README.md](Hardware/README.md) for the full pin assignment table and board details.

## Status

Firmware is functional and verified with the **OpenKNX GW-REG1-Dali gateway** end-to-end. Tested operations include addressing (INITIALISE/RANDOMISE/COMPARE/PROGRAM SHORT), all 16-bit forward commands (level control, scenes, groups, fade), DT8 colour control (RGBW + Tc), bank 0 memory reads, and IEC 62386-105 firmware updates over the bus (with Fletcher-16 integrity check and auto-resume on fault).

Multi-vendor coexistence verified with Mi-Boxer DALI controllers on the same bus (2026-05-17).

## License

MIT
