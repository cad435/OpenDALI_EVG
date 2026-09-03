# Hardware

PCB designs for the OpenDALI_EVG project.

> Trademark notice — see [root README](../README.md): *DALI*, *DALI-2* etc. are DiiA trademarks; this project is an independent IEC 62386 implementation, not DiiA-certified.

## Boards

| Board | Description |
|---|---|
| [Controller](Controller/Readme.md) | DALI PHY + CH32V003, bus-powered. Pin assignment, J4 load-board interface, hardware validation. |
| [LoadBoard 250W RGBW](DALI_Load_250W_RGBW/Readme.md) | Mains switching and 4-channel PWM LED driver. Work in progress. |
| [LoadBoard 250W Digital LED](DALI_Load_250W_DigitalLED/Readme.md) | Mains switching and isolated single-wire output for WS2812/SK6812 strips. Work in progress. |

## Manufacturing

The Gerber files are ready for upload to any PCB manufacturer. The JLCPCB files (BOM + CPL) allow direct ordering with SMT assembly through [JLCPCB](https://jlcpcb.com/).
