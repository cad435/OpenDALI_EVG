# OpenDALI_EVG

A completely open-source DALI-2 Electronic Control Gear (EVG) for controlling LEDs. Built around the **CH32V003** RISC-V microcontroller — one of the cheapest MCUs available — proving that a fully compliant DALI slave doesn't need an expensive dedicated IC.

## Overview

This project implements a DALI-2 control gear device compliant with **IEC 62386-101** (bus/protocol) and **IEC 62386-102** (control gear commands), including **DT8 colour control** (IEC 62386-209) for RGBW and colour temperature mixing.

Key highlights:
- Full DALI-2 protocol stack in bare-metal C, under 10 KB flash
- 4-channel PWM output (RGBW) at 20 kHz with 2400-step resolution
- Logarithmic dimming curve per IEC 62386-102
- 16 scenes, 16 groups, fade engine with configurable fade time/rate
- Full commissioning support (INITIALISE, RANDOMISE, SEARCHADDR, PROGRAM)
- All configuration persisted to flash (survives power cycles)
- Bus-powered design possible (< 2 mA quiescent from DALI bus)

## Project Structure

```
OpenDALI_EVG/
├── Firmware/       CH32V003 DALI slave firmware (PlatformIO project)
├── Hardware/       Altium PCB design files (coming soon)
└── Simulations/    LTspice PHY and power supply simulations
```

### Firmware

The firmware is a standalone PlatformIO project targeting the CH32V003F4P6, built on [ch32v003fun](https://github.com/cnlohr/ch32v003fun). It includes its own detailed [README](Firmware/README.md), architecture diagram, command implementation status, and test documentation.

### Hardware

PCB design files (Altium). Work in progress.

### Simulations

LTspice simulations for the DALI PHY layer:
- **Phy_Non-Isolated.asc** — Bus-powered PHY with direct MCU connection
- **Phy_Isolated.asc** — Optocoupler-isolated PHY variant
- **Sec_Linear_Regulator.asc** — Secondary-side linear regulator analysis
- **IDEAL_DCDC.lib** — Behavioral DC-DC converter subcircuit for simulation (models efficiency, UVLO, quiescent current)

## Hardware Platform

| Parameter | Value |
|-----------|-------|
| MCU | CH32V003F4P6 (RISC-V, 48 MHz, 16 KB Flash, 2 KB RAM) |
| PWM Channels | 4 (RGBW via TIM1) |
| DALI Interface | GPIO-based Manchester encode/decode (no dedicated transceiver IC) |
| Supply | Bus-powered from DALI (with DC-DC converter) or external |

## Getting Started

### Build & Flash

```bash
cd Firmware
pio run                    # Build
pio run -t upload          # Flash via WCH-Link
```

### Wiring

Connect the DALI bus to the PHY circuit (see Simulations for reference designs), then wire RX/TX to the MCU:
- **PC0** — DALI RX input
- **PC5** — DALI TX output
- **PD2, PA1, PC3, PC4** — PWM outputs (CH1-CH4)
- **PA2** — External PSU control (optional)

## Status

The firmware is functional and tested with DALI-2 masters. See [Firmware/Commands_Implemented.md](Firmware/Commands_Implemented.md) for a full command-by-command status table.

## License

MIT
