/*
    main.c - DALI EVG firmware entry point (ch32fun framework)

    This file contains:
    - millis() implementation via SysTick (ch32fun has no built-in millis)
    - PSU control (PA2) for external power supply enable/disable
    - ISR wrappers for DALI RX (EXTI0) and TX/idle (TIM2 CH2/CH4)
    - Main loop calling dali_process()

    LED output is handled by led_driver.c, selected at compile time
    via LED_DRIVER in hardware.h (PWM or WS2812/SK6812).

    Architecture:
    ┌──────────────────────────────────────────────────────────────────┐
    │  main loop                                                       │
    │  ┌──────────┐   ┌──────────────┐   ┌──────────────────────┐     │
    │  │ millis() │   │ dali_process │   │ on_level() callback  │     │
    │  │ SysTick  │   │ frame decode │──>│ led_driver_apply()   │     │
    │  └──────────┘   └──────────────┘   └──────────────────────┘     │
    │                        ↑                                         │
    │  ISRs:                 │                                         │
    │  ┌─────────────────────┴──────────────────────────────────────┐  │
    │  │ EXTI7_0  → dali_isr_rx_edge()    (Manchester edge decode) │  │
    │  │ TIM2 CC2 → dali_isr_tx_tick()    (backward frame gen)     │  │
    │  │ TIM2 CC4 → dali_isr_idle_timeout() (frame-complete detect)│  │
    │  └───────────────────────────────────────────────────────────┘   │
    └──────────────────────────────────────────────────────────────────┘
*/

#include "ch32fun.h"
#include <stdio.h>
#include "hardware.h"
#include "dali_slave.h"
#include "dali_nvm.h"
#include "led_driver.h"

/* ====================================================================
 * millis() — Millisecond counter via SysTick
 * ==================================================================== */
static volatile uint32_t ms_ticks = 0;

void SysTick_Handler(void) __attribute__((interrupt));
void SysTick_Handler(void) {
    SysTick->SR = 0;
    ms_ticks++;
}

uint32_t millis(void) {
    return ms_ticks;
}

static void millis_init(void) {
    SysTick->CMP  = (FUNCONF_SYSTEM_CORE_CLOCK / 1000) - 1;
    SysTick->CNT  = 0;
    SysTick->SR   = 0;
    SysTick->CTLR = 0xF;
    NVIC_EnableIRQ(SysTick_IRQn);
}

/* ====================================================================
 * PSU Control — Enable/disable external power supply
 * ====================================================================
 * PA2 is configured as a push-pull GPIO output. It goes HIGH when any
 * channel has a non-zero level (DALI level > 0), and LOW when all
 * channels are off (DALI level = 0). This allows an external FET or
 * relay to control power to the LED driver stage.
 * ==================================================================== */
static void psu_ctrl_init(void) {
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOA;
    PSU_CTRL_PORT->CFGLR = (PSU_CTRL_PORT->CFGLR & ~(0xF << (PSU_CTRL_PIN_N * 4)))
                         | ((GPIO_Speed_10MHz | GPIO_CNF_OUT_PP) << (PSU_CTRL_PIN_N * 4));
    PSU_CTRL_PORT->BCR = (1 << PSU_CTRL_PIN_N);
}

static inline void psu_ctrl_set(uint8_t on) {
    if (on)
        PSU_CTRL_PORT->BSHR = (1 << PSU_CTRL_PIN_N);
    else
        PSU_CTRL_PORT->BCR  = (1 << PSU_CTRL_PIN_N);
}

/* ====================================================================
 * USB Enumeration Pin — Deassert bootloader pull-up
 * ====================================================================
 * The CH32V003 bootloader drives USB_ENUM high to pull up USB D+ for
 * enumeration. In firmware we configure it as input with pull-down to
 * deassert the pull-up and prevent the host from seeing a USB device.
 * ==================================================================== */
static void usb_enum_init(void) {
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOC;
    USB_ENUM_PORT->CFGLR = (USB_ENUM_PORT->CFGLR & ~(0xF << (USB_ENUM_PIN_N * 4)))
                         | (0x8 << (USB_ENUM_PIN_N * 4));   /* CNF=10 (pull), MODE=00 (in) */
    USB_ENUM_PORT->BCR   = (1 << USB_ENUM_PIN_N);           /* ODR=0 → pull-down */
}

/* ====================================================================
 * DALI Arc Power Callback
 * ====================================================================
 * Called from dali_process() when the DALI actual level changes.
 * Applies per-channel colour scaling via the LED driver and
 * enables/disables the external PSU.
 * ==================================================================== */
static void on_level(uint8_t level) {
    led_driver_apply(level, dali_get_colour_actual());
    psu_ctrl_set(level > 0);
    printf("LVL=%d\n", level);
}

/* ====================================================================
 * DT8 Colour Callback
 * ====================================================================
 * Called from dali_process() when ACTIVATE commits new colour values.
 * Recalculates LED output using current arc level + new colour.
 * ==================================================================== */
static void on_colour(const uint8_t *levels, uint8_t count) {
    led_driver_apply(dali_get_actual_level(), dali_get_colour_actual());
    printf("CLR");
    for (uint8_t i = 0; i < count && i < 4; i++)
        printf(" %d", levels[i]);
    printf("\n");
}

/* ====================================================================
 * ISR Wrappers — Connect hardware interrupts to DALI state machines
 * ==================================================================== */
void EXTI7_0_IRQHandler(void) __attribute__((interrupt));
void EXTI7_0_IRQHandler(void) {
    EXTI->INTFR = EXTI_Line0;
    dali_isr_rx_edge();
}

void TIM2_IRQHandler(void) __attribute__((interrupt));
void TIM2_IRQHandler(void) {
    if (TIM2->INTFR & TIM_IT_CC2) {
        TIM2->INTFR = ~TIM_IT_CC2;
        dali_isr_tx_tick();
    }
    if (TIM2->INTFR & TIM_IT_CC4) {
        TIM2->INTFR = ~TIM_IT_CC4;
        dali_isr_idle_timeout();
    }
}

/* ====================================================================
 * Main Entry Point
 * ====================================================================
 * Initialization order matters:
 *   1. SystemInit()         — PLL setup (24 MHz HSI → 48 MHz), flash wait
 *   2. funGpioInitAll()     — Enable all GPIO port clocks
 *   3. usb_enum_init()      — Deassert USB D+ pull-up from bootloader
 *   4. millis_init()        — SysTick 1 ms tick (used by DALI timeout)
 *   5. psu_ctrl_init()      — PSU enable GPIO on PA2
 *   6. led_driver_init()    — LED output (PWM or WS2812 depending on config)
 *   7. dali_init()          — TIM2 + EXTI0 for DALI RX/TX
 *   8. dali_set_arc_callback() — Connect dimming to LED driver
 * ==================================================================== */
int main(void) {
    SystemInit();
    funGpioInitAll();

    usb_enum_init();
    millis_init();
    psu_ctrl_init();
    led_driver_init();
    dali_init();
    dali_set_arc_callback(on_level);
    dali_set_colour_callback(on_colour);
    nvm_init();
    dali_power_on();

#ifdef DIGITAL_LED_OUT
    printf("DALI %s DT%d %d LEDs ready\n", EVG_MODE_NAME, DALI_DEVICE_TYPE, WS2812_NUM_LEDS);
#else
    printf("DALI %s DT%d %dch PWM ready\n", EVG_MODE_NAME, DALI_DEVICE_TYPE, PWM_NUM_CHANNELS);
#endif

    uint32_t led_refresh_ms = 0;

    while (1) {
        dali_process();
        dali_fade_tick();
        nvm_tick();

        /* Re-send LED data periodically to recover from glitches */
        uint32_t now = millis();
        if (now - led_refresh_ms >= 250) {
            led_refresh_ms = now;
            led_driver_refresh();
        }

        if (dali_is_tx_idle() && (millis() - dali_last_rx_edge_ms() > 20)) {
            __WFI();
        }
    }
}
