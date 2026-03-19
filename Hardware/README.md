# Hardware

PCB designs for the OpenDALI_EVG project.

## Boards

### Controller

The DALI PHY and microcontroller board. Handles all DALI-2 bus communication, protocol processing, and generates the digital PWM/LED control signals. Built around the CH32V003F4P6 RISC-V microcontroller.

### LoadBoard (planned)

LED driver board that takes the digital RGB/RGBW signals from the Controller, drives the LEDs through power output stages, and handles external PSU on/off control. Different LoadBoard variants are planned for different power classes:

- **Small** — Integrated AC/DC supply on-board, no external PSU needed
- **Medium / Large** — Designed for external off-the-shelf PSUs (5V to 48V)

## Manufacturing

The Gerber files are ready for upload to any PCB manufacturer. The JLCPCB files (BOM + CPL) allow direct ordering with SMT assembly through [JLCPCB](https://jlcpcb.com/).
