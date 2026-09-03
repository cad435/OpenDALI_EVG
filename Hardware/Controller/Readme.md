# Controller

<img width="833" height="546" alt="image" src="https://github.com/user-attachments/assets/650aa0ba-976a-4230-8549-4366e191131c" />

<img width="416" height="195" alt="image" src="https://github.com/user-attachments/assets/f6a6d045-d7e3-48be-8826-83b9e46a826c" /> <img width="404" height="195" alt="image" src="https://github.com/user-attachments/assets/e595dce8-a9e5-443e-a36c-ad8549adabf1" />




The DALI PHY and microcontroller board. Handles all DALI-bus communication (IEC 62386-101/102 compatible), protocol processing, and generates the digital PWM/LED control signals. Built around the CH32V003F4U6 RISC-V microcontroller (20-pin QFN, 48 MHz, 16 KB Flash, 2 KB RAM).

The PHY transceiver converts between the DALI bus voltage levels (0/16V) and the MCU's 3.3V logic. See [`Simulations/`](../../Simulations/) for LTspice reference designs (isolated and non-isolated variants).

## Pin Assignment, CH32V003F4U6

| Pin | Function | Direction | Peripheral | Notes |
|-----|----------|-----------|------------|-------|
| PA1 | Boot button | Input | GPIO | Active low at reset → enter DALI bootloader (also routed through firmware's `boot_button_check()` on NRST/SYSRESETREQ) |
| PA2 | PSU Control | Output | GPIO | HIGH = external PSU on, LOW = off |
| PC0 | LED3 / Blue PWM | Output | TIM1_CH3 | 20 kHz, 2400-step (11.2 bit) |
| PC1 | I2C SDA | Bidir | I2C1 | AT24C256C EEPROM (persistence + firmware staging) |
| PC2 | I2C SCL | Output | I2C1 | AT24C256C EEPROM |
| PC3 | DALI RX | Input | EXTI3 | From PHY RX_OUT, both-edge interrupt |
| PC4 | DALI TX | Output | GPIO | To PHY TX_IN, Manchester encode |
| PC5 | SPI1 SCK → J4.3 | Output | SPI1 | SPI clock to the load board (ribbon J4 pin 3). Idle/unused in direct-PWM LED modes; driven only when a digital (SPI) load board is used. *(Was previously documented as "spare" — it is physically routed to the connector.)* |
| PC6 | LED1 / Red PWM **or** WS2812 data | Output | TIM1_CH1 / SPI1_MOSI | Dual-use: PWM in analog modes, SPI+DMA in digital LED modes |
| PC7 | LED2 / Green PWM | Output | TIM1_CH2 | 20 kHz, 2400-step (11.2 bit) |
| PD0 | *(spare)* | — | — | Free GPIO |
| PD1 | SWDIO | Bidir | SWD | Single-wire debug (active during programming) |
| PD2 | *(spare)* | — | — | Free GPIO |
| PD3 | LED4 / White PWM | Output | TIM1_CH4 | 20 kHz, 2400-step (11.2 bit) |
| PD4 | *(spare)* | — | — | Free GPIO |
| PD5 | Debug UART TX | Output | USART1_TX | 115200 baud, via WCH-LinkE bridge |
| PD6 | Debug UART RX | Input | USART1_RX | 115200 baud, available for debug input |
| PD7 | NRST | Input | Reset | Active low hardware reset |


**Digital LED output** (WS2812/SK6812 modes): SPI1_MOSI on PC6 at 3 MHz, DMA-driven. Same physical pin as TIM1_CH1 — selected at compile time via `EVG_MODE_xxx`.

> **⚠ Galvanic isolation is MANDATORY.** AC-mains switching of the 250 W load board was tested on real hardware: the LED-power domain **must be galvanically isolated** from the DALI/MCU domain. The isolation barrier lives on the **load board**. The J4 ribbon connector is the controller↔load-board interface that feeds the isolator's input side.

## J4 — Load-board interface (10-pin FFC, 0.5 mm pitch)

Pins 9 and 10 are dual-function (PWM **or** SPI), selected by the controller's `EVG_MODE` at compile time — so the same connector drives either a direct-PWM load board or a digital (SPI) load board with no controller hardware change:

| Pin | Net | MCU pin | Function |
|-----|-----|---------|----------|
| 1 | `GND` | — | Controller-domain ground (DALI/MCU side; isolated from LED domain on the load board) |
| 2 | `+3V3-D` | — | 3.3 V controller-domain supply (feeds the isolator input side VCC1 in Route A) |
| 3 | `SCK` | PC5 | SPI1 clock (digital load board) |
| 4 | `SDA` | PC1 | I²C data (shared EEPROM bus; load-board diagnostics option) |
| 5 | `SCL` | PC2 | I²C clock |
| 6 | `PSU-CTRL` | PA2 | LED AC/DC PSU enable — HIGH = on (also used as AUX-MCU power-cycle / bootloader entry in Route B) |
| 7 | `PWM_CH4` | PD3 | White PWM (free in SPI mode) |
| 8 | `PWM_CH3` | PC0 | Blue PWM (free in SPI mode) |
| 9 | `PWM_CH2 / MISO` | PC7 | Green PWM **or** SPI MISO (load-board → controller, e.g. diagnostics) |
| 10 | `PWM_CH1 / D_LED / MOSI` | PC6 | Red PWM **or** WS2812/SK6812 data **or** SPI MOSI |
| M ×2 | `GND` | — | Connector mounting / shield tabs |


The connector is deliberately over-provisioned, because **one controller is meant to serve many generations of load board.** Each load board taps only the signals it needs, and the firmware selects the matching mode at compile time via `EVG_MODE` — so adopting a new load-board design is a recompile, not a controller respin. Power (`+3V3-D`, `GND`) and the `PSU-CTRL` enable line are always present; what varies from board to board is *how the colour and level data is actuated.*

**I²C (`SDA`/`SCL`) is always routed**, shared with the controller's on-board EEPROM bus. It is held in reserve for a future diagnostic/measurement channel (LED current, temperature, fault feedback) or an I²C PWM/LED expander.

> ⚠ **One caveat on isolating I²C.** I²C is *bidirectional*, so taking it across the galvanic barrier requires a bidirectional I²C isolator — those are typically mA class parts which might crash the 2mA current budget.

For the actuation itself, each load board picks **one** of three paths:

1. **Direct PWM** — the four 20 kHz channels (CH1…CH4) drive the load-side gate drivers straight through the isolator. The simplest option, but it needs a fast enough isolator so that even the shortest dimming pulses survive the barrier intact. Tested is the π160M30 from 2Pai-Semi. Implemented by the [LoadBoard 250W RGBW](../DALI_Load_250W_RGBW/Readme.md).
2. **Addressable LED (WS2812 / SK6812)** — a single-wire data stream on the CH1/MOSI pin, generated by SPI1 + DMA. Implemented by the [LoadBoard 250W Digital LED](../DALI_Load_250W_DigitalLED/Readme.md).
3. **SPI to a load-side companion MCU** — the unidirectional SPI lines isolate cleanly with a cheap **µA** digital isolator. A second MCU on the load board, powered locally from the LED PSU, acts as an **SPI slave**: it receives colour/level commands and handles everything downstream itself — optional diagnostics returned over MISO. A slow and inexpensive isolator suffices, and the bus-side channel count stays minimal.

*Example SPI mapping* — a 6-channel isolator (e.g. 2Pai π161U3x, 5 forward + 1 reverse), reusing the otherwise-PWM pins:

| Dir | Signal | J4 | MCU |
|-----|--------|----|-----|
| → | MOSI | 10 | PC6 |
| → | SCK | 3 | PC5 |
| → | NCS | 8 | PC0 |
| → | PSU-CTRL | 6 | PA2 |
| → | BL-Enable | 7 | PD3 |
| ← | MISO | 9 | PC7 |

> Note: the π161U3x (U-Grade) limits the SPI-Speed to 150kHz. In case of availability issues the higher grades (M/E) are also OK. 

**Firmware updates** happen over the DALI bus itself via the IEC 62386-105 bootloader at `0x1FFFF000`. PA1 held low at reset routes into the bootloader (works for POR, NRST, and SYSRESETREQ thanks to the firmware-side `boot_button_check()`). PD1 (SWDIO) is the recovery path via WCH-LinkE for chips that have lost their flash content entirely.

## Hardware Validation

| Test | Target | Result | Evidence |
|------|--------|--------|----------|
| DALI RX Manchester waveform | Clean edges, correct timing | **PASS** — 3.7V swing, sharp edges, correct 1200 baud timing | See below |
| C24 voltage under sustained DALI traffic | > 12.5V | **PASS** — 14.40V min / 14.84V max (0.44V drop at 16V supply, 32 fps) | See below |
| 3.3V rail stability under sustained traffic | Stable within CH32V003 spec (2.7–5.5V) | **PASS** — 3.42V min / 3.63V max (20mV additional ripple = noise only) | See below |
| Current consumption (firmware running) | < 2 mA | **PASS** — 1.69 mA (169mV over 100R shunt) | — |


**Note**: Waveform-Images are from an LeCroy LT264 with a custom visualisation. You looking at real measured waveforms.

##### DALI RX Waveform

Oscilloscope capture of the raw Manchester-encoded signal on PC3 (DALI RX, via PHY transceiver). C1 is the raw 16V DALI Signal, C2 is the signal after the receiver on PC3.
<img width="2000" height="993" alt="lecroy-2026-05-22T19-55-04" src="https://github.com/user-attachments/assets/76f940d1-4202-4610-8ca2-937a646bca81" />

##### C24 Voltage Stability

Voltage across C24 (DALI bus power supply decoupling) while rapidly sending DALI commands over an extended period. The voltage must remain above **12.5V** to ensure the circuit works at the lowest allowed DALI bus voltage (9.5V).

**Calculation:** The DC-DC converter requires a minimum of 5V input. The bridge rectifier drops ~1V (2 diodes). So C24 must stay above 6V for the converter to regulate. At the minimum DALI bus voltage of 9.5V, this allows a maximum voltage drop of 3.5V (9.5V - 6V). Our test supply runs at 16V, so the equivalent pass/fail threshold is 16V - 3.5V = **12.5V**. If C24 stays above 12.5V at 16V supply, it will stay above 6V at 9.5V supply. Note: this is a linear approximation — actual energy stored in C24 scales with V², so the real margin at 9.5V will be slightly worse. Best estimate for now until tested at actual minimum bus voltage.

**Measurement procedure:** DALI EVG firmware (RGBW mode) running on the target. An OpenKNX DALI gateway sent continuous broadcast QUERY GEAR PRESENT (0xFF 0x91) frames at maximum bus rate (~32 frames/sec, 25ms spacing per IEC 62386-101) for 5 seconds (~160 frames total). Oscilloscope probe on C24, DC coupling, 2V/div, 50ms/div timebase, min/max measurement enabled.

**Result (2026-05-02):**
- Bus supply voltage: 16V
- Maximum at C24: **15.4V**
- Minimum at C24: **14.8V**
- Voltage drop: **0.6V** (well within 3.5V budget)
- **PASS** — extrapolated minimum at 9.5V bus: ~8.9V (above 6V threshold)


**3.3V rail (after buck converter):**
- No traffic: 3.63V max / 3.44V min
- Under sustained traffic (32 fps): 3.63V max / 3.42V min
- **Stable** — only 20mV additional ripple under load. CH32V003 operating range is 2.7–5.5V, so 3.42V is well within spec.

Note: All Channels have different V/div settings.<br>
C1: DALI bus, you can see sustained heavy traffic. C2: Voltage at Capacitor C24 (before the Buck). C3:. Voltage at 3.3V Rail after the Buck.
<img width="2000" height="993" alt="lecroy-2026-05-22T20-21-50" src="https://github.com/user-attachments/assets/f04c51c8-fded-4714-8699-d7472f81fe63" />


### Current Consumption

Total current draw of the Controller board with firmware running. Target: < 2 mA to stay within DALI bus power limits.

**Measurement (2026-05-02):** 100R shunt resistor in series with the supply. DALI EVG firmware (RGBW mode) running, bus idle, Bus Voltage 16V. Measured 169mV across the shunt → **1.69 mA**. Well within the 2 mA DALI bus power budget.
