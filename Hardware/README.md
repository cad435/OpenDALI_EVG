# Hardware

PCB designs for the OpenDALI_EVG project.

## Boards

### Controller

The DALI PHY and microcontroller board. Handles all DALI-2 bus communication, protocol processing, and generates the digital PWM/LED control signals. Built around the CH32V003F4P6 RISC-V microcontroller.

### LoadBoard 250W RGBW

LED driver and AC power switching board. Connects to the Controller via a 10-pin FFC cable (0.5 mm pitch) and provides:
- Mains switching for controlling the LED AC/DC Powersupply
- Power limits for the connected PSU: **max 1.2 A continuous** (without airflow), **max 3 A continuous** (with active airflow over the triac)
- 4-Channel PWM LED Driver (RGBW)

## Manufacturing

The Gerber files are ready for upload to any PCB manufacturer. The JLCPCB files (BOM + CPL) allow direct ordering with SMT assembly through [JLCPCB](https://jlcpcb.com/).
