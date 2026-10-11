# LoadBoard 250W RGBW

LED driver and AC power switching board. Connects to the Controller via a 10-pin FFC cable (0.5 mm pitch, 25-50 mm, reverse) and provides:
- Mains switching for controlling the LED AC/DC Powersupply
- Power limits for the connected PSU: **max 1.2 A continuous** (without airflow), **max 3 A continuous** (with active airflow over the triac)
- 4-Channel PWM LED Driver (RGBW)

> **⚠ Work in progress.** This board is still under development.
>
> **⚠ Not fully IEC 62386 compliant:** the opto-triac that switches the mains stage is driven from the DALI bus. Bus current at 16 V: **1.67 mA** with the LED supply off (measured) and about **2.6 mA** for as long as it is on, so the 2 mA budget is exceeded continuously while the light is on. The 2.6 mA is an estimate for the IL4118 opto-triac and still has to be measured. The bus side is powered through a buck converter, so the current rises at lower bus voltage.

> **⚠ Mains safety:** only screw this board into the enclosure with the housing part in place — the screw head must rest on plastic, never directly on the PCB. See [Enclosure](../README.md#enclosure).

## Top label

Print [`DALI_Load_250W_RGBW_V0.3-TopLabel.png`](DALI_Load_250W_RGBW_V0.3-TopLabel.png) at 100 %, cut on the marks and clamp the 40 x 24 mm plate under the four M2.5 screws on the enclosure top — the hole pattern is 33.15 x 17 mm.

![Top label](DALI_Load_250W_RGBW_V0.3-TopLabel.png)
