# Hardware

PCB designs for the OpenDALI_EVG project.

## Boards

### Controller

The DALI PHY and microcontroller board. Handles all DALI-2 bus communication, protocol processing, and generates the digital PWM/LED control signals. Built around the CH32V003F4U6 RISC-V microcontroller (20-pin QFN, 48 MHz, 16 KB Flash, 2 KB RAM).

The PHY transceiver converts between the DALI bus voltage levels (0/16V) and the MCU's 3.3V logic. See `Simulationen/` for LTspice reference designs (isolated and non-isolated variants).

#### Pin Assignment (CH32V003F4U6, TIM1 Partial Remap 1)

| Pin | Function | Direction | Peripheral | Notes |
|-----|----------|-----------|------------|-------|
| PA1 | Boot button | Input | GPIO | Active low at reset → enter USB bootloader |
| PA2 | PSU Control | Output | GPIO | HIGH = external PSU on, LOW = off |
| PC0 | LED3 / Blue PWM | Output | TIM1_CH3 | 20 kHz, 2400-step (11.2 bit) |
| PC1 | I2C SDA | Bidir | I2C1 | Reserved for AT24C256C EEPROM |
| PC2 | I2C SCL | Output | I2C1 | Reserved for AT24C256C EEPROM |
| PC3 | DALI RX | Input | EXTI3 | From PHY RX_OUT, both-edge interrupt |
| PC4 | DALI TX | Output | GPIO | To PHY TX_IN, Manchester encode |
| PC5 | *(spare)* | — | — | Free GPIO for future use |
| PC6 | LED1 / Red PWM **or** WS2812 data | Output | TIM1_CH1 / SPI1_MOSI | Dual-use: PWM in analog modes, SPI+DMA in digital LED modes |
| PC7 | LED2 / Green PWM | Output | TIM1_CH2 | 20 kHz, 2400-step (11.2 bit) |
| PD0 | USB D+ Pull-Up | Output | GPIO | Directly driven by bootloader for USB enumeration |
| PD1 | SWDIO | Bidir | SWD | Single-wire debug (active during programming) |
| PD2 | USB D- | Bidir | GPIO (bit-bang) | USB Low-Speed, active only in bootloader |
| PD3 | LED4 / White PWM | Output | TIM1_CH4 | 20 kHz, 2400-step (11.2 bit) |
| PD4 | USB D+ | Bidir | GPIO (bit-bang) | USB Low-Speed, active only in bootloader |
| PD5 | Debug UART TX | Output | USART1_TX | 115200 baud, via WCH-LinkE bridge |
| PD6 | Debug UART RX | Input | USART1_RX | 115200 baud, available for debug input |
| PD7 | NRST | Input | Reset | Active low hardware reset |

**PWM channel mapping** (TIM1 Partial Remap 1, `AFIO_PCFR1_TIM1_REMAP = 01`):

| Channel | Pin | LED colour (RGBW mode) |
|---------|-----|----------------------|
| TIM1_CH1 | PC6 | Red (shared with WS2812 SPI1_MOSI) |
| TIM1_CH2 | PC7 | Green |
| TIM1_CH3 | PC0 | Blue |
| TIM1_CH4 | PD3 | White |

**Digital LED output** (WS2812/SK6812 modes): SPI1_MOSI on PC6 at 3 MHz, DMA-driven. Same physical pin as TIM1_CH1 — selected at compile time via `EVG_MODE_xxx`.

**USB** (bootloader only): PD4 (D+), PD2 (D-), PD0 (DPU). Active only when boot button (PA1) is held low during reset. Do NOT connect USB while the EVG is on a DALI bus.

### LoadBoard 250W RGBW

LED driver and AC power switching board. Connects to the Controller via a 10-pin FFC cable (0.5 mm pitch) and provides:
- Mains switching for controlling the LED AC/DC Powersupply
- Power limits for the connected PSU: **max 1.2 A continuous** (without airflow), **max 3 A continuous** (with active airflow over the triac)
- 4-Channel PWM LED Driver (RGBW)

## Manufacturing

The Gerber files are ready for upload to any PCB manufacturer. The JLCPCB files (BOM + CPL) allow direct ordering with SMT assembly through [JLCPCB](https://jlcpcb.com/).
