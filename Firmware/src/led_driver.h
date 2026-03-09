/*
    led_driver.h - LED output driver interface for DALI EVG

    Abstracts the LED output hardware so main.c doesn't need to know
    whether we're driving analog PWM channels or digital LED strips.

    Selected at compile time via LED_DRIVER in hardware.h:
      LED_DRIVER_PWM     — TIM1 PWM on up to 4 channels (RGBW)
      LED_DRIVER_WS2812  — WS2812 / SK6812 addressable LED strip via SPI+DMA on PC6
*/
#ifndef _LED_DRIVER_H
#define _LED_DRIVER_H

#include <stdint.h>

/*
 * Initialize the LED output hardware.
 * For PWM: configures TIM1 channels and GPIO pins.
 * For WS2812: configures SPI1+DMA on PC6 (MOSI).
 */
void led_driver_init(void);

/*
 * Apply a new DALI level + DT8 colour to the LED output.
 *
 * Parameters:
 *   dali_level:    DALI arc power level (0 = off, 1–254 = dimmed)
 *   colour_actual: pointer to 4-byte array [R, G, B, W] with values 0–254
 *                  (from DT8 colour control; default 254 = full channel)
 *
 * For PWM: sets per-channel duty cycle using the logarithmic dimming curve.
 * For WS2812: computes RGB(W) bytes (all LEDs same colour) and streams via DMA.
 */
void led_driver_apply(uint8_t dali_level, const volatile uint8_t *colour_actual);

#endif
