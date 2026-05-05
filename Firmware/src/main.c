/*
    main.c - DALI EVG firmware entry point (ch32fun framework)

    This file contains:
    - millis() implementation via SysTick (ch32fun has no built-in millis)
    - PSU control (PA2) for external power supply enable/disable
    - ISR wrappers for DALI RX (EXTI0) and TX/idle (TIM2 CH2/CH4)
    - Main loop calling dali_protocol_process(), dali_fade_tick(), nvm_tick()

    LED output is handled by led_driver.c, selected at compile time
    via EVG_MODE in hardware.h (PWM or WS2812/SK6812).

    Architecture:
    ┌──────────────────────────────────────────────────────────────────┐
    │  main loop                                                       │
    │  ┌──────────┐   ┌──────────────────┐   ┌────────────────────┐   │
    │  │ millis() │   │ dali_protocol    │   │ on_level() callback│   │
    │  │ SysTick  │   │ _process()       │──>│ led_driver_apply() │   │
    │  └──────────┘   └──────────────────┘   └────────────────────┘   │
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
#include "config/hardware.h"
#include "logger.h"
#include "dali/phy/dali_phy.h"
#include "dali/protocol/dali_protocol.h"
#include "dali/protocol/dali_fade.h"
#include "dali/dali_state.h"
#include "dali/nvm/dali_nvm.h"
#include "eeprom/eeprom.h"
#include "led/led_driver.h"

/* ====================================================================
 * millis() — Millisecond counter via SysTick
 * ====================================================================
 * ch32fun does not provide a millis() function. We implement one using
 * the SysTick timer, which counts down from CMP to zero and generates
 * an interrupt on each underflow. At 48 MHz, CMP = 47999 gives a
 * precise 1 ms tick. The volatile counter is read by all modules via
 * the extern millis() declaration.
 * ==================================================================== */
static volatile uint32_t ms_ticks = 0;

/* SysTick ISR — fires every 1 ms, increments the global tick counter.
 * __attribute__((interrupt)) is required on CH32V003 for correct
 * RISC-V hardware stacking (mret vs ret).
 *
 * Without auto-reload, we must advance CMP by the period each tick
 * so the next compare match fires 1 ms later. The CNT register is
 * free-running (never resets), so CMP accumulates. */
void SysTick_Handler(void) __attribute__((interrupt));
void SysTick_Handler(void) {
    SysTick->SR = 0;        /* Clear the compare-match flag */
    SysTick->CMP += (FUNCONF_SYSTEM_CORE_CLOCK / 8 / 1000);  /* Next match in 6000 ticks */
    ms_ticks++;
}

/* Returns the number of milliseconds since boot. Wraps after ~49 days.
 * Thread-safe: 32-bit aligned read is atomic on RISC-V. */
uint32_t millis(void) {
    return ms_ticks;
}

/* Configure SysTick for 1 ms interrupts.
 * Uses auto-reload mode (STRE=1) with HCLK/8 source to stay compatible
 * with ch32fun's DelaySysTick/Delay_Ms which expects a free-running
 * HCLK/8 counter.
 *
 * CTLR bits: 0=STE(enable), 1=STIE(irq), 2=STCLK(0=HCLK/8), 3=STRE(auto-reload)
 * HCLK/8 = 6 MHz → CMP = 5999 for 1 ms period.
 *
 * NOTE: We must NOT set STCLK=1 (HCLK) here because FUNCONF_SYSTICK_USE_HCLK
 * is 0 (default), so Delay_Ms() computes tick counts assuming HCLK/8 = 6 MHz.
 * Setting STCLK=1 would make the counter run 8x faster than Delay_Ms expects.
 *
 * Auto-reload (STRE=1) is safe because CMP = 5999 and Delay_Ms(5) needs
 * only 30000 ticks which exceeds the CMP reload boundary. We must therefore
 * NOT use auto-reload — it would wrap CNT at 5999, breaking DelaySysTick
 * for any delay > 1 ms.
 *
 * Solution: Use HCLK/8 free-running counter (STRE=0, STCLK=0) with compare
 * interrupt. CNT counts 0→2^32, CMP triggers IRQ every 6000 ticks, but we
 * must update CMP each ISR to fire again (since no auto-reload).
 */
static void millis_init(void) {
    SysTick->CNT  = 0;
    SysTick->CMP  = (FUNCONF_SYSTEM_CORE_CLOCK / 8 / 1000) - 1;  /* 6MHz / 1000 - 1 = 5999 */
    SysTick->SR   = 0;             /* Clear pending flag */
    SysTick->CTLR = 0x3;           /* STE + STIE, HCLK/8, no auto-reload */
    NVIC_EnableIRQ(SysTick_IRQn);
}

/* ====================================================================
 * PSU Control — Enable/disable external power supply
 * ====================================================================
 * PA2 is a push-pull GPIO output that controls an external FET or relay
 * powering the LED driver stage. HIGH = PSU on, LOW = PSU off.
 * The PSU turns on when any DALI channel has a non-zero level, and
 * turns off immediately when all channels reach zero (no delay).
 * ==================================================================== */

/* Configure PA2 as push-pull output, initially LOW (PSU off). */
static void psu_ctrl_init(void) {
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOA;    /* Enable GPIOA clock */
    /* Set PA2 to push-pull output, 10 MHz drive strength */
    PSU_CTRL_PORT->CFGLR = (PSU_CTRL_PORT->CFGLR & ~(0xF << (PSU_CTRL_PIN_N * 4)))
                         | ((GPIO_Speed_10MHz | GPIO_CNF_OUT_PP) << (PSU_CTRL_PIN_N * 4));
    PSU_CTRL_PORT->BCR = (1 << PSU_CTRL_PIN_N); /* BCR = bit clear → LOW */
}

/* Set PSU state. Uses atomic BSHR (bit set) / BCR (bit clear) registers
 * for glitch-free single-cycle GPIO writes. */
static inline void psu_ctrl_set(uint8_t on) {
    if (on)
        PSU_CTRL_PORT->BSHR = (1 << PSU_CTRL_PIN_N);   /* HIGH = PSU on */
    else
        PSU_CTRL_PORT->BCR  = (1 << PSU_CTRL_PIN_N);    /* LOW  = PSU off */
}

/* ====================================================================
 * USB Enumeration Pin — Deassert bootloader pull-up
 * ====================================================================
 * The USB bootloader drives PD0 HIGH to pull up USB D+ for host
 * enumeration. After jumping to firmware, we must deassert this
 * pull-up so the host doesn't see a phantom USB device.
 * Configure PD0 as input with pull-down (ODR=0 selects pull-down
 * when CNF=10 input-with-pull mode).
 * ==================================================================== */
static void usb_enum_init(void) {
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOC;
    /* CNF=10 (input with pull-up/down), MODE=00 (input) = 0x8 */
    USB_ENUM_PORT->CFGLR = (USB_ENUM_PORT->CFGLR & ~(0xF << (USB_ENUM_PIN_N * 4)))
                         | (0x8 << (USB_ENUM_PIN_N * 4));
    USB_ENUM_PORT->BCR   = (1 << USB_ENUM_PIN_N);  /* ODR=0 → pull-down selected */
}

/* ====================================================================
 * DALI Arc Power Callback
 * ====================================================================
 * Called from dali_protocol_process() (main loop context) whenever the
 * DALI actual level changes — via DAPC, scenes, fade completion, OFF,
 * RECALL MAX/MIN, STEP UP/DOWN, etc.
 *
 * Responsibilities:
 * 1. Apply the new level + current colour to the LED driver (PWM or WS2812)
 * 2. Enable/disable the external PSU based on whether output is active
 * 3. Print debug output to serial (115200 baud on PD5)
 * ==================================================================== */
static void on_level(uint8_t level) {
    led_driver_apply(level, dali_protocol_get_colour_actual());
    uint8_t psu = (level > 0);
    psu_ctrl_set(psu);
    printf("LVL=%d PSU=%d\n", level, psu);
}

/* ====================================================================
 * DT8 Colour Callback
 * ====================================================================
 * Called from dali_protocol_process() when a DT8 ACTIVATE command
 * commits new colour values (RGBW channel levels or Tc conversion).
 * Recalculates LED output using the current arc level combined with
 * the new colour ratios. The arc level itself doesn't change here —
 * only the per-channel colour scaling is updated.
 * ==================================================================== */
static void on_colour(const uint8_t *levels, uint8_t count) {
    led_driver_apply(dali_protocol_get_actual_level(), dali_protocol_get_colour_actual());
    printf("CLR");
    for (uint8_t i = 0; i < count && i < 4; i++)
        printf(" %d", levels[i]);
    printf("\n");
}

/* ====================================================================
 * ISR Wrappers — Connect hardware interrupts to DALI PHY state machines
 * ====================================================================
 * CH32V003 has a shared EXTI7_0 handler for all EXTI lines 0-7.
 * We only use EXTI line 0 (PC0 = DALI RX pin).
 *
 * TIM2 has a single ISR for all channels. We check which channel
 * triggered (CC2 = TX tick, CC4 = idle timeout) and dispatch accordingly.
 * The flag is cleared by writing ~flag (inverted) to INTFR — this is
 * a CH32V003 quirk where INTFR bits are write-0-to-clear.
 * ==================================================================== */

/* EXTI line 3 — DALI RX edge on PC3 (via PHY transceiver).
 * Fires on every rising and falling edge of the DALI bus signal.
 * The PHY RX state machine timestamps each edge and decodes
 * Manchester-encoded forward frames (16-bit, 1200 baud). */
void EXTI7_0_IRQHandler(void) __attribute__((interrupt));
void EXTI7_0_IRQHandler(void) {
    EXTI->INTFR = DALI_RX_EXTI_LINE;   /* Clear EXTI pending flag */
    dali_isr_rx_edge();                 /* Run RX Manchester decoder */
}

/* TIM2 — DALI TX waveform generation + RX idle timeout detection.
 * Two output compare channels share this single ISR:
 *   CC2: TX Te tick — fires every 417 us during backward frame
 *         transmission, stepping the TX Manchester encoder state machine
 *   CC4: RX idle timeout — fires 5 Te (2085 us) after the last RX edge,
 *         signaling that the forward frame is complete (stop condition) */
void TIM2_IRQHandler(void) __attribute__((interrupt));
void TIM2_IRQHandler(void) {
    if (TIM2->INTFR & TIM_IT_CC2) {
        TIM2->INTFR = ~TIM_IT_CC2;     /* Clear CC2 flag (write-0-to-clear) */
        dali_isr_tx_tick();             /* Step TX Manchester encoder */
    }
    if (TIM2->INTFR & TIM_IT_CC4) {
        TIM2->INTFR = ~TIM_IT_CC4;     /* Clear CC4 flag */
        dali_isr_idle_timeout();        /* Signal frame complete to protocol layer */
    }
}

/* ====================================================================
 * Main Entry Point
 * ====================================================================
 * Initialization order matters — each step depends on the previous:
 *   1. SystemInit()      — PLL: 24 MHz HSI → 48 MHz, flash wait states
 *   2. funGpioInitAll()  — Enable all GPIO port clocks (ch32fun helper)
 *   3. usb_enum_init()   — Deassert USB D+ pull-up from bootloader
 *   4. millis_init()     — SysTick 1 ms tick (needed by DALI timeouts)
 *   5. psu_ctrl_init()   — PSU enable GPIO (PA2), initially off
 *   6. led_driver_init() — LED output (TIM1 PWM or SPI+DMA for WS2812)
 *   7. dali_phy_init()   — TIM2 + EXTI0 for DALI Manchester RX/TX
 *   8. callbacks         — Connect DALI level/colour changes to LED driver
 *   9. eeprom_init()     — Initialize I2C1 for AT24C256 EEPROM
 *  10. nvm_init()        — Load persisted config from EEPROM + write identity block
 *  11. dali_power_on()   — Apply power-on level (from NVM or default 254)
 * ==================================================================== */
int main(void) {
    SystemInit();
    funGpioInitAll();

    usb_enum_init();
    millis_init();
    psu_ctrl_init();
    led_driver_init();
    dali_phy_init();
    dali_protocol_set_arc_callback(on_level);
    dali_protocol_set_colour_callback(on_colour);
    eeprom_init();
    nvm_init();
    dali_protocol_power_on();

    /* Print boot message with mode info (visible on debug serial PD5) */
    if (ds.short_address <= 63)
        printf("ADDR=%d\n", ds.short_address);
    else
        printf("ADDR=none\n");
#ifdef ONOFF_MODE
    printf("DALI %s DT%d ON/OFF ready\n", EVG_MODE_NAME, DALI_DEVICE_TYPE);
#elif defined(DIGITAL_LED_OUT)
    printf("DALI %s DT%d %d LEDs ready\n", EVG_MODE_NAME, DALI_DEVICE_TYPE, WS2812_NUM_LEDS);
#else
    printf("DALI %s DT%d %dch PWM ready\n", EVG_MODE_NAME, DALI_DEVICE_TYPE, PWM_NUM_CHANNELS);
#endif
    printf("Build: " __DATE__ " " __TIME__ "\n");

    uint32_t led_refresh_ms = 0;    /* Timestamp for periodic LED refresh */

    /* ── Main loop ─────────────────────────────────────────────────
     * Non-blocking cooperative loop. Each function returns quickly:
     *   - dali_protocol_process(): check for received frame, dispatch
     *   - dali_fade_tick(): step fade by one level if interval elapsed
     *   - nvm_tick(): save dirty config to flash after 5s debounce
     * Between bus activity, the CPU sleeps via __WFI() to save power.
     * ──────────────────────────────────────────────────────────────── */
    while (1) {
        dali_protocol_process();    /* Dispatch any received DALI frame */
        dali_fade_tick();           /* Advance running fades (if any) */
        nvm_tick();                 /* Deferred flash write (5s after last change) */

        /* Log bus collisions detected by the PHY during backward frame TX.
         * The PHY samples the bus each Te and aborts if another device
         * is pulling the line active while we drove idle (IEC 62386-101 §8.2.4.4).
         * Only functional with a real DALI PHY transceiver (open-drain bus). */
        if (dali_phy_consume_collision())
            LOG_PHY("COLLISION");

        /* Periodically re-send LED data to recover from electrical glitches.
         * For PWM mode this is a no-op (TIM1 hardware maintains duty cycle).
         * For WS2812/SK6812 mode this re-streams the colour data via SPI+DMA. */
        uint32_t now = millis();
        if (now - led_refresh_ms >= 250) {
            led_refresh_ms = now;
            led_driver_refresh();
        }

        /* Enter sleep (WFI) when the DALI bus is idle to save power.
         * Only sleep when:
         *   1. No backward frame is being transmitted (TX idle)
         *   2. No RX activity in the last 20 ms (no frame in progress)
         * SysTick (1 ms) or EXTI0 (next DALI edge) will wake the CPU.
         * CH32V003 Sleep mode: core stops, all peripherals keep running
         * (TIM2, EXTI, SysTick continue). Do NOT use Standby (SLEEPDEEP=1)
         * — that stops TIM2 and breaks DALI edge timing. */
        if (dali_phy_is_tx_idle() && (millis() - dali_phy_last_rx_edge_ms() > 20)) {
            __WFI();
        }
    }
}
