# Hardware

PCB designs for the OpenDALI_EVG project.

## Boards

### Controller

The DALI PHY and microcontroller board. Handles all DALI-2 bus communication, protocol processing, and generates the digital PWM/LED control signals. Built around the CH32V003F4P6 RISC-V microcontroller.

### LoadBoard 250W

LED driver and AC power switching board. Connects to the Controller via a 10-pin FFC cable (0.5 mm pitch) and provides:

**AC Power Switching**
- Mains switching via **ACST410-8BTR** triac (snubberless, DPAK) + **MOC3043** zero-cross optocoupler with galvanic isolation
- 1.5 A fuse on AC output
- Thermal limits: **max 1.2 A continuous** (without airflow), **max 3 A continuous** (with active airflow over the triac)
- Designed for switching external AC/DC power supplies (capacitive load)

**4-Channel PWM LED Driver (RGBW)**
- 4× **IRLU024N** N-channel MOSFETs (I-PAK) as low-side switches
- 4× **UCC27517** gate drivers for fast, clean switching
- Gate drive supply derived from VCC_LED (12–48 V) via crude 18 V regulation (BZT52C15S zener + MMBT5551)

**Connectors**
- **P1** (KF142R, 5.08 mm) — AC in/out (4-pin screw terminal)
- **P2** (KF2EDGR, 3.81 mm) — LED output RGBW (4-pin)
- **P7** (4-pin male) — PSU input (PSU+/PSU−)
- **J1** (FFC 0.5 mm, 10-pin) — Controller interface (4× PWM, I²C, PSU control)

## Manufacturing

The Gerber files are ready for upload to any PCB manufacturer. The JLCPCB files (BOM + CPL) allow direct ordering with SMT assembly through [JLCPCB](https://jlcpcb.com/).
