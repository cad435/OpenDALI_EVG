/*
    hardware.h - Pin assignments for DALI EVG on CH32V003F4U6 (ch32fun)

    Physical wiring (with DALI PHY transceiver):
    ┌─────────────┐       ┌──────────┐       ┌──────────────────┐
    │ DALI Master │──bus──│ DALI PHY │       │ CH32V003 Slave   │
    │             │       │  RX_OUT ─┼───────┤ PC3 (RX, EXTI3)  │
    │             │       │  TX_IN  ─┼───────┤ PC4 (TX, GPIO)   │
    │             │       │  GND ────┼───────┤ GND              │
    └─────────────┘       └──────────┘       │ PC6 (TIM1_CH1) ──┤── LED1 / WS2812
    TIM1 Partial Remap 1 (RM=01):             │ PC7 (TIM1_CH2) ──┤── LED2 (PWM)
      CH1=PC6  CH2=PC7  CH3=PC0  CH4=PD3     │ PC0 (TIM1_CH3) ──┤── LED3 (PWM)
                                              │ PD3 (TIM1_CH4) ──┤── LED4 (PWM)
    PC6 dual-use: TIM1_CH1 (PWM modes)       │ PA2 (GPIO) ──────┤── PSU_CTRL
                  SPI1_MOSI (WS2812 modes)    │ PC1 (I2C1_SDA) ──┤── (EEPROM)
                                              │ PC2 (I2C1_SCL) ──┤── (EEPROM)
                                              │ PC5 ─────────────┤── (spare)
                                              │ PD5 (USART1_TX) ─┤── Debug TX
                                              │ PD6 (USART1_RX) ─┤── Debug RX
                                              └──────────────────┘

    Bus polarity (with PHY transceiver):
    - TX: HIGH = pull bus active (mark), LOW = release bus (idle)
    - RX: HIGH = bus active (mark), LOW = bus idle (space)
    - Manchester bit 1: active→idle = HIGH→LOW
    - Manchester bit 0: idle→active = LOW→HIGH
    - Bus collision detection: PHY open-drain allows readback during TX
*/
#ifndef _HARDWARE_H
#define _HARDWARE_H

/* ── EVG Mode Selection ────────────────────────────────────────────
   Define ONE of the following to select the LED output mode.
   All other configuration (DALI device type, channel count, driver
   selection, DT8 colour features) is derived automatically.

   On/off mode (no PWM, no timer — PSU_CTRL pin only):
     EVG_MODE_ONOFF       — 1 channel, relay/switch output (DT6)

   PWM modes (TIM1, up to 4 channels):
     EVG_MODE_SINGLE      — 1 channel, single-colour LEDs (DT6)
     EVG_MODE_CCT         — 2 channels, warm/cool white Tc control (DT8)
     EVG_MODE_RGB         — 3 channels, RGB LEDs (DT8, Tc + primary)
     EVG_MODE_RGBW        — 4 channels, RGBW LEDs (DT8, Tc + primary)

   Addressable LED modes (SPI1+DMA on PC6):
     EVG_MODE_WS2812      — WS2812 strip, 3 bytes/LED GRB (DT8)
     EVG_MODE_SK6812_RGB  — SK6812 strip, 3 bytes/LED GRB (DT8)
     EVG_MODE_SK6812_RGBW — SK6812 strip, 4 bytes/LED GRBW (DT8)

   Can also be set via compiler flag: -DEVG_MODE_RGBW
   ──────────────────────────────────────────────────────────────────── */

/* Default mode — override via -DEVG_MODE_xxx compiler flag */
#if !defined(EVG_MODE_ONOFF) && !defined(EVG_MODE_SINGLE) && !defined(EVG_MODE_CCT) \
 && !defined(EVG_MODE_RGB) && !defined(EVG_MODE_RGBW) && !defined(EVG_MODE_WS2812) \
 && !defined(EVG_MODE_SK6812_RGB) && !defined(EVG_MODE_SK6812_RGBW)
#define EVG_MODE_RGBW
#endif




/* WS2812 type constants (used by mode switch below) */
#define WS2812_TYPE_WS2812      0   /* 3 bytes GRB (WS2812, SK6812 RGB) */
#define WS2812_TYPE_SK6812_RGBW 1   /* 4 bytes GRBW (SK6812 RGBW) */

/* ── Mode → derived configuration ─────────────────────────────────
   DALI_DEVICE_TYPE:    6 (DT6 LED gear) or 8 (DT8 colour control)
   PWM_NUM_CHANNELS:    1–4 (PWM modes only)
   DIGITAL_LED_OUT:     defined for WS2812/SK6812 modes
   WS2812_TYPE:         byte format for digital LED modes
   EVG_NUM_COLOURS:     number of colour channels (1–4)
   EVG_HAS_DT8:         1 if DT8 extended commands are supported
   EVG_DT8_HAS_TC:      1 if colour temperature (Tc/mirek) is supported
   EVG_DT8_HAS_PRIMARY: 1 if RGBWAF primaries are supported
   ──────────────────────────────────────────────────────────────────── */
#if defined(EVG_MODE_ONOFF)
  #define EVG_MODE_NAME       "ONOFF"
  #define EVG_MODE_SERIAL     0x4F4E4F4646000000ULL  /* "ONOFF\0\0\0" */
  #define DALI_DEVICE_TYPE    6
  #define PWM_NUM_CHANNELS    0
  #define EVG_NUM_COLOURS     1
  #define EVG_HAS_DT8         0
  #define EVG_DT8_HAS_TC      0
  #define EVG_DT8_HAS_PRIMARY 0
  #define ONOFF_MODE                  /* Guards: skip TIM1, skip led_driver */

#elif defined(EVG_MODE_SINGLE)
  #define EVG_MODE_NAME       "SINGLE"
  #define EVG_MODE_SERIAL     0x53494E474C450000ULL  /* "SINGLE\0\0" */
  #define DALI_DEVICE_TYPE    6
  #define PWM_NUM_CHANNELS    1
  #define EVG_NUM_COLOURS     1
  #define EVG_HAS_DT8         0
  #define EVG_DT8_HAS_TC      0
  #define EVG_DT8_HAS_PRIMARY 0

#elif defined(EVG_MODE_CCT)
  #define EVG_MODE_NAME       "CCT"
  #define EVG_MODE_SERIAL     0x4343540000000000ULL  /* "CCT\0\0\0\0\0" */
  #define DALI_DEVICE_TYPE    8
  #define PWM_NUM_CHANNELS    2
  #define EVG_NUM_COLOURS     2
  #define EVG_HAS_DT8         1
  #define EVG_DT8_HAS_TC      1
  #define EVG_DT8_HAS_PRIMARY 0

#elif defined(EVG_MODE_RGB)
  #define EVG_MODE_NAME       "RGB"
  #define EVG_MODE_SERIAL     0x5247420000000000ULL  /* "RGB\0\0\0\0\0" */
  #define DALI_DEVICE_TYPE    8
  #define PWM_NUM_CHANNELS    3
  #define EVG_NUM_COLOURS     3
  #define EVG_HAS_DT8         1
  #define EVG_DT8_HAS_TC      1
  #define EVG_DT8_HAS_PRIMARY 1

#elif defined(EVG_MODE_RGBW)
  #define EVG_MODE_NAME       "RGBW"
  #define EVG_MODE_SERIAL     0x5247425700000000ULL  /* "RGBW\0\0\0\0" */
  #define DALI_DEVICE_TYPE    8
  #define PWM_NUM_CHANNELS    4
  #define EVG_NUM_COLOURS     4
  #define EVG_HAS_DT8         1
  #define EVG_DT8_HAS_TC      1
  #define EVG_DT8_HAS_PRIMARY 1

#elif defined(EVG_MODE_WS2812)
  #define EVG_MODE_NAME       "WS2812"
  #define EVG_MODE_SERIAL     0x5753323831320000ULL  /* "WS2812\0\0" */
  #define DALI_DEVICE_TYPE    8
  #define DIGITAL_LED_OUT
  #define WS2812_TYPE         WS2812_TYPE_WS2812
  #define EVG_NUM_COLOURS     3
  #define EVG_HAS_DT8         1
  #define EVG_DT8_HAS_TC      1
  #define EVG_DT8_HAS_PRIMARY 1

#elif defined(EVG_MODE_SK6812_RGB)
  #define EVG_MODE_NAME       "SK6812_RGB"
  #define EVG_MODE_SERIAL     0x534B363852474200ULL  /* "SK68RGB\0" */
  #define DALI_DEVICE_TYPE    8
  #define DIGITAL_LED_OUT
  #define WS2812_TYPE         WS2812_TYPE_WS2812  /* same 3-byte GRB protocol */
  #define EVG_NUM_COLOURS     3
  #define EVG_HAS_DT8         1
  #define EVG_DT8_HAS_TC      1
  #define EVG_DT8_HAS_PRIMARY 1

#elif defined(EVG_MODE_SK6812_RGBW)
  #define EVG_MODE_NAME       "SK6812_RGBW"
  #define EVG_MODE_SERIAL     0x534B363852474257ULL  /* "SK68RGBW" */
  #define DALI_DEVICE_TYPE    8
  #define DIGITAL_LED_OUT
  #define WS2812_TYPE         WS2812_TYPE_SK6812_RGBW
  #define EVG_NUM_COLOURS     4
  #define EVG_HAS_DT8         1
  #define EVG_DT8_HAS_TC      1
  #define EVG_DT8_HAS_PRIMARY 1

#else
  #error "No EVG_MODE defined. Define one of: EVG_MODE_ONOFF, EVG_MODE_SINGLE, EVG_MODE_CCT, EVG_MODE_RGB, EVG_MODE_RGBW, EVG_MODE_WS2812, EVG_MODE_SK6812_RGB, EVG_MODE_SK6812_RGBW"
#endif

/* ── DALI Bus Mode ──────────────────────────────────────────────────
   Define DALI_NO_PHY for direct GPIO-to-GPIO connection (no transceiver).
   Comment out / undefine when using a real DALI PHY transceiver.

   NO_PHY (direct GPIO):
     TX: LOW = bus active (mark), HIGH = bus idle (space)
     RX: LOW = bus active (mark), HIGH = bus idle (space)

   With PHY transceiver (e.g. TI DALI-1 PHY, SN65HVD62):
     TX: HIGH = pull bus active (mark), LOW = release bus (idle)
     RX: HIGH = bus active (mark), LOW = bus idle (space)
   ──────────────────────────────────────────────────────────────────── */
/* #define DALI_NO_PHY */       /* Uncomment for direct GPIO (no transceiver) */

/* ── DALI Bus Interface ──────────────────────────────────────────────
   RX: PC3 — EXTI3 triggers on both edges, TIM2->CNT timestamps them.
   TX: PC4 — GPIO push-pull output, driven by TIM2 CH2 output compare ISR
             to generate Manchester-encoded backward frames.
   ──────────────────────────────────────────────────────────────────── */
#define DALI_RX_PORT    GPIOC
#define DALI_RX_PIN_N   3       /* PC3 — DALI forward frame input (EXTI3) */
#define DALI_RX_EXTI_LINE  EXTI_Line3   /* EXTI line matching pin number */
#define DALI_TX_PORT    GPIOC
#define DALI_TX_PIN_N   4       /* PC4 — DALI backward frame output */

/* ── LED PWM Output Configuration (PWM modes only) ─────────────────
   TIM1 advanced timer with Partial Remap 1 (AFIO TIM1_RM=01).
   This remap puts CH1 on PC6, sharing the pin with SPI1_MOSI —
   enabling dual-use of PC6 for either PWM or WS2812 output
   depending on EVG_MODE (selected at compile time).

   PWM_NUM_CHANNELS is derived from EVG_MODE above.
   All enabled channels output identical PWM (~20 kHz at 48 MHz)
   with IEC 62386-102 logarithmic dimming curve.

     1 channel:  CH1 only        (PC6)
     2 channels: CH1 + CH2       (PC6, PC7)
     3 channels: CH1 + CH2 + CH3 (PC6, PC7, PC0)
     4 channels: CH1..CH4        (PC6, PC7, PC0, PD3)
   ──────────────────────────────────────────────────────────────────── */

/* TIM1 channel-to-pin mapping (Partial Remap 1, AFIO TIM1_RM=01):
   CH1 = PC6  (GPIOC bit 6) — shared with SPI1_MOSI (WS2812)
   CH2 = PC7  (GPIOC bit 7)
   CH3 = PC0  (GPIOC bit 0)
   CH4 = PD3  (GPIOD bit 3)
*/
#define PWM_CH1_PORT    GPIOC
#define PWM_CH1_PIN_N   6       /* PC6 — TIM1 channel 1 (dual-use with SPI1_MOSI) */
#define PWM_CH2_PORT    GPIOC
#define PWM_CH2_PIN_N   7       /* PC7 — TIM1 channel 2 */
#define PWM_CH3_PORT    GPIOC
#define PWM_CH3_PIN_N   0       /* PC0 — TIM1 channel 3 */
#define PWM_CH4_PORT    GPIOD
#define PWM_CH4_PIN_N   3       /* PD3 — TIM1 channel 4 */

/* ── WS2812 / SK6812 Configuration (digital LED modes only) ───────
   Data output is on PC6 (SPI1 MOSI — same pin as TIM1_CH1 in
   partial remap 1). SPI1 runs at 3 MHz; each WS2812 data bit is
   encoded as 4 SPI bits. DMA1 Channel 3 handles the transfer.
   WS2812_TYPE is derived from EVG_MODE above.
   ──────────────────────────────────────────────────────────────────── */
#define WS2812_NUM_LEDS         30

/* ── PSU Control Output ─────────────────────────────────────────────
   PA2 — GPIO push-pull output. HIGH when any PWM channel is active
   (duty > 0), LOW when all channels are off (level = 0).
   Used to enable/disable an external power supply or LED driver stage.
   ──────────────────────────────────────────────────────────────────── */
#define PSU_CTRL_PORT   GPIOA
#define PSU_CTRL_PIN_N  2       /* PA2 — PSU enable output */

/* ── I2C Bus (reserved for external EEPROM) ────────────────────────
   Hardware I2C1 peripheral, default pin mapping (no AFIO remap).
   Reserved for AT24C256C I2C EEPROM (32 KB, 64-byte pages, 1M write
   cycles). Planned for NVM persistence and safe firmware staging.

   Default I2C1:  SDA=PC1, SCL=PC2
   Not active yet — using internal flash for persistence.
   ──────────────────────────────────────────────────────────────────── */
#define I2C_SDA_PORT    GPIOC
#define I2C_SDA_PIN_N   1       /* PC1 — I2C1 SDA (default) */
#define I2C_SCL_PORT    GPIOC
#define I2C_SCL_PIN_N   2       /* PC2 — I2C1 SCL (default) */

/* ── Serial Debug ────────────────────────────────────────────────────
   USART1 TX = PD5, RX = PD6 (default mapping).
   TX auto-configured by ch32fun when FUNCONF_USE_UARTPRINTF=1.
   Connected via WCH-LinkE UART bridge to host PC (115200 baud).
   ──────────────────────────────────────────────────────────────────── */
#define SERIAL_TX_PIN       PD5     // USART1_TX
#define SERIAL_RX_PIN       PD6     // USART1_RX

/* ── USB (bootloader only, active only when boot button held) ────── */
#define USB_DP_PIN          PD4     // USB D+
#define USB_DM_PIN          PD2     // USB D-
#define USB_ENUM_PORT       GPIOD
#define USB_ENUM_PIN_N      0       /* PD0 — USB D+ pull-up (driven by bootloader; input-pulldown in firmware) */

/* ── Bootloader ──────────────────────────────────────────────────── */
#define BOOTLOADER_EN_PIN   PA1     // Bootloader enable (pull low at reset to enter bootloader)

/* ── System Pins (active, do not use as GPIO) ────────────────────── */
#define NRST_PIN            PD7     // Reset
#define SWDIO_PIN           PD1     // Single-wire debug interface

/* ── Spare GPIO ──────────────────────────────────────────────────── */
// PC5 — free, available for future use

#endif
