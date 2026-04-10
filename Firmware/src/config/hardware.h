/*
    hardware.h - Pin assignments for DALI EVG on CH32V003F4P6 (ch32fun)

    Physical wiring (with DALI PHY transceiver):
    ┌─────────────┐       ┌───────────┐       ┌──────────────────┐
    │ DALI Master │──bus──│ DALI PHY  │       │ CH32V003 Slave   │
    │             │       │ (e.g.     │       │                  │
    │             │       │ SN65HVD62)│       │                  │
    │             │       │  RX_OUT ──┼───────┤ PC0 (RX, EXTI0)  │
    │             │       │  TX_IN  ──┼───────┤ PC5 (TX, GPIO)   │
    │             │       │  GND ─────┼───────┤ GND              │
    └─────────────┘       └───────────┘       │ PD2 (TIM1_CH1) ──┤── LED1 (PWM)
                                              │ PA1 (TIM1_CH2) ──┤── LED2 (PWM)
                                              │ PC3 (TIM1_CH3) ──┤── LED3 (PWM)
                                              │ PC4 (TIM1_CH4) ──┤── LED4 (PWM)
                                              │ PC6 (SPI1_MOSI) ─┤── WS2812 data
                                              │ PA2 (GPIO) ──────┤── PSU_CTRL
                                              │ PC1 (I2C1_SDA) ──┤── (reserved for EEPROM)
                                              │ PC2 (I2C1_SCL) ──┤── (reserved for EEPROM)
                                              │ PD5 (USART1_TX) ─┤── Debug serial
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
  #define DALI_DEVICE_TYPE    6
  #define PWM_NUM_CHANNELS    0
  #define EVG_NUM_COLOURS     1
  #define EVG_HAS_DT8         0
  #define EVG_DT8_HAS_TC      0
  #define EVG_DT8_HAS_PRIMARY 0
  #define ONOFF_MODE                  /* Guards: skip TIM1, skip led_driver */

#elif defined(EVG_MODE_SINGLE)
  #define EVG_MODE_NAME       "SINGLE"
  #define DALI_DEVICE_TYPE    6
  #define PWM_NUM_CHANNELS    1
  #define EVG_NUM_COLOURS     1
  #define EVG_HAS_DT8         0
  #define EVG_DT8_HAS_TC      0
  #define EVG_DT8_HAS_PRIMARY 0

#elif defined(EVG_MODE_CCT)
  #define EVG_MODE_NAME       "CCT"
  #define DALI_DEVICE_TYPE    8
  #define PWM_NUM_CHANNELS    2
  #define EVG_NUM_COLOURS     2
  #define EVG_HAS_DT8         1
  #define EVG_DT8_HAS_TC      1
  #define EVG_DT8_HAS_PRIMARY 0

#elif defined(EVG_MODE_RGB)
  #define EVG_MODE_NAME       "RGB"
  #define DALI_DEVICE_TYPE    8
  #define PWM_NUM_CHANNELS    3
  #define EVG_NUM_COLOURS     3
  #define EVG_HAS_DT8         1
  #define EVG_DT8_HAS_TC      1
  #define EVG_DT8_HAS_PRIMARY 1

#elif defined(EVG_MODE_RGBW)
  #define EVG_MODE_NAME       "RGBW"
  #define DALI_DEVICE_TYPE    8
  #define PWM_NUM_CHANNELS    4
  #define EVG_NUM_COLOURS     4
  #define EVG_HAS_DT8         1
  #define EVG_DT8_HAS_TC      1
  #define EVG_DT8_HAS_PRIMARY 1

#elif defined(EVG_MODE_WS2812)
  #define EVG_MODE_NAME       "WS2812"
  #define DALI_DEVICE_TYPE    8
  #define DIGITAL_LED_OUT
  #define WS2812_TYPE         WS2812_TYPE_WS2812
  #define EVG_NUM_COLOURS     3
  #define EVG_HAS_DT8         1
  #define EVG_DT8_HAS_TC      1
  #define EVG_DT8_HAS_PRIMARY 1

#elif defined(EVG_MODE_SK6812_RGB)
  #define EVG_MODE_NAME       "SK6812_RGB"
  #define DALI_DEVICE_TYPE    8
  #define DIGITAL_LED_OUT
  #define WS2812_TYPE         WS2812_TYPE_WS2812  /* same 3-byte GRB protocol */
  #define EVG_NUM_COLOURS     3
  #define EVG_HAS_DT8         1
  #define EVG_DT8_HAS_TC      1
  #define EVG_DT8_HAS_PRIMARY 1

#elif defined(EVG_MODE_SK6812_RGBW)
  #define EVG_MODE_NAME       "SK6812_RGBW"
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
   RX: PC0 — EXTI0 triggers on both edges, TIM2->CNT timestamps them.
   TX: PC5 — GPIO push-pull output, driven by TIM2 CH2 output compare ISR
             to generate Manchester-encoded backward frames.
   ──────────────────────────────────────────────────────────────────── */
#define DALI_RX_PORT    GPIOC
#define DALI_RX_PIN_N   0       /* PC0 — DALI forward frame input */
#define DALI_TX_PORT    GPIOC
#define DALI_TX_PIN_N   5       /* PC5 — DALI backward frame output */

/* ── LED PWM Output Configuration (PWM modes only) ─────────────────
   TIM1 advanced timer, default pin mapping (no AFIO remap needed).
   PWM_NUM_CHANNELS is derived from EVG_MODE above.
   All enabled channels output identical PWM (~20 kHz at 48 MHz)
   with IEC 62386-102 logarithmic dimming curve.

     1 channel:  CH1 only        (PD2)
     2 channels: CH1 + CH2       (PD2, PA1)
     3 channels: CH1 + CH2 + CH3 (PD2, PA1, PC3)
     4 channels: CH1..CH4        (PD2, PA1, PC3, PC4)
   ──────────────────────────────────────────────────────────────────── */

/* TIM1 channel-to-pin mapping (CH32V003 default, no AFIO remap):
   CH1 = PD2  (GPIOD bit 2)
   CH2 = PA1  (GPIOA bit 1)
   CH3 = PC3  (GPIOC bit 3)
   CH4 = PC4  (GPIOC bit 4)
*/
#define PWM_CH1_PORT    GPIOD
#define PWM_CH1_PIN_N   2       /* PD2 — TIM1 channel 1 */
#define PWM_CH2_PORT    GPIOA
#define PWM_CH2_PIN_N   1       /* PA1 — TIM1 channel 2 */
#define PWM_CH3_PORT    GPIOC
#define PWM_CH3_PIN_N   3       /* PC3 — TIM1 channel 3 */
#define PWM_CH4_PORT    GPIOC
#define PWM_CH4_PIN_N   4       /* PC4 — TIM1 channel 4 */

/* ── WS2812 / SK6812 Configuration (digital LED modes only) ───────
   Data output is on PC6 (SPI1 MOSI, default pin mapping).
   SPI1 runs at 3 MHz; each WS2812 data bit is encoded as 4 SPI bits.
   DMA1 Channel 3 handles the transfer in the background.
   WS2812_TYPE is derived from EVG_MODE above.
   ──────────────────────────────────────────────────────────────────── */
#define WS2812_NUM_LEDS         30

/* ── PSU Control Output ─────────────────────────────────────────────
   PA2 — GPIO push-pull output. HIGH when any PWM channel is active
   (duty > 0), LOW when all channels are off (level = 0).
   Used to enable/disable an external power supply or LED driver stage.
   The PSU turns off immediately when all PWM duties reach zero
   (no additional delay after fade-down).
   Moved from PC1 to PA2 to free PC1 for I2C1 SDA (EEPROM).
   ──────────────────────────────────────────────────────────────────── */
#define PSU_CTRL_PORT   GPIOA
#define PSU_CTRL_PIN_N  2       /* PA2 — PSU enable output */

/* ── I2C Bus (reserved for external EEPROM) ────────────────────────
   Hardware I2C1 peripheral, default pin mapping (no AFIO remap).
   Reserved for AT24C256C I2C EEPROM (32 KB, 64-byte pages, 1M write
   cycles). Planned for NVM persistence and safe firmware staging
   (DALI bootloader A/B update with EEPROM as staging area).

   Default I2C1:  SDA=PC1, SCL=PC2 (both free since PSU_CTRL moved to PA2)
   Remap option1: SDA=PD0, SCL=PD1 (PD1 conflicts with SWDIO)
   Remap option2: SDA=PC6, SCL=PC5 (PC6 conflicts with WS2812, PC5 with DALI TX)

   Not active yet — using internal flash for persistence.
   ──────────────────────────────────────────────────────────────────── */
#define I2C_SDA_PORT    GPIOC
#define I2C_SDA_PIN_N   1       /* PC1 — I2C1 SDA (default) */
#define I2C_SCL_PORT    GPIOC
#define I2C_SCL_PIN_N   2       /* PC2 — I2C1 SCL (default) */

/* ── Serial Debug ────────────────────────────────────────────────────
   USART1 TX = PD5, auto-configured by ch32fun when FUNCONF_USE_UARTPRINTF=1.
   Connected via WCH-LinkE UART bridge to host PC (115200 baud).
   ──────────────────────────────────────────────────────────────────── */
#define SERIAL_TX_PIN       PD5     // USART1_TX
#define SERIAL_RX_PIN       PD6     // USART1_RX

// --- USB (bootloader only, no firmware support) ---
#define USB_DP_PIN          PD4     // USB D+
#define USB_DM_PIN          PD3     // USB D-
#define USB_ENUM_PORT       GPIOD
#define USB_ENUM_PIN_N      0       /* PD0 — USB D+ pull-up (driven by bootloader; input-pulldown in firmware) */

// --- Bootloader ---
#define BOOTLOADER_EN_PIN   PC7     // Bootloader enable (pull low at reset to enter bootloader)

// --- System Pins (active, do not use as GPIO) ---
#define NRST_PIN            PD7     // Reset
#define SWDIO_PIN           PD1     // Single-wire debug interface

#endif
