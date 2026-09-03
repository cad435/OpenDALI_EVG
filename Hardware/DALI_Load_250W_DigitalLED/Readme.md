# LoadBoard 250W Digital LED

Addressable-LED driver and AC power switching board for WS2812 / SK6812 strips. Connects to the Controller via a 10-pin FFC cable (0.5 mm pitch) and provides:
- Mains switching for controlling the LED AC/DC power supply
- Power limits for the connected PSU: **max 1.2 A continuous** (without airflow), **max 3 A continuous** (with active airflow over the triac)
- One galvanically isolated single-wire data output

Same AC stage as the [LoadBoard 250W RGBW](../DALI_Load_250W_RGBW/Readme.md) — the four PWM channels are replaced by a single isolated data line on `PWM_CH1/D_LED/MOSI` (Controller PC6). Firmware side: `EVG_MODE_WS2812`, `EVG_MODE_SK6812_RGB` or `EVG_MODE_SK6812_RGBW`.

## Connectors

| Ref | Type | Function |
|-----|------|----------|
| J1 | FFC 0.5 mm, 10-pin | Controller interface (Controller J4). `+3V3-PRI` here is the Controller's `+3V3-D`; `GND` is common to both. |
| P1 | KF142R-5.08, 4-pin | Mains in / switched mains out to the LED PSU |
| P7 | 1x4 header | DC input from the LED PSU (`PSU+` / `GND-SEC`) |
| P2 | KF2EDGR-3.81, 3-pin | LED strip: `PSU+`, `DATA`, `GND-SEC` |

## Strip supply

`PSU+` is passed straight through from P7 to the strip connector, so **the strip runs at the PSU voltage** — match the PSU to the strip. Range **5–24 V**. A local buck (LGS5145) derives the 5 V that supplies the isolator's output side.

> At `PSU+` = 5 V the buck has no headroom. Fit **R7 (0 Ω)** and remove **R2** to bypass it.

## Isolation

The data line crosses the barrier through a π110M30 single-channel digital isolator (3 kVrms) — input side powered from the controller's 3.3 V rail, output side from the local 5 V. Neither I²C nor a reverse channel crosses the barrier.

With the LED PSU off, the isolator's output side is unpowered and its output goes high-impedance. The π110M30 is the **Default Low** variant, so an absent input drives the strip line low (strip idle) rather than high.

An isolated 5 V rail (B0505S) is fed back from the LED side to the bus side, taking the opto-triac's LED current off the DALI bus in steady state.

> **⚠ Work in progress.** V0.1 fabrication data generated; not yet built or validated on hardware.
>
> **⚠ Not fully IEC 62386 compliant:** as on the RGBW board, the bus draw briefly exceeds the 2 mA budget during each off→on transition, until the feedback rail takes over.
