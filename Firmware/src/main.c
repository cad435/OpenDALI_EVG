/*
    main.c - DALI EVG firmware entry point (ch32fun framework)

    This file contains:
    - millis() implementation via SysTick (ch32fun has no built-in millis)
    - TIM1 PWM setup for 1-4 LED channels (configurable via PWM_NUM_CHANNELS)
    - IEC 62386-102 §9.3 logarithmic dimming lookup table (254 entries)
    - ISR wrappers for DALI RX (EXTI0) and TX/idle (TIM2 CH2/CH4)
    - Main loop calling dali_process()

    Architecture:
    ┌──────────────────────────────────────────────────────────────────┐
    │  main loop                                                       │
    │  ┌──────────┐   ┌──────────────┐   ┌──────────────────────┐     │
    │  │ millis() │   │ dali_process │   │ on_level() callback  │     │
    │  │ SysTick  │   │ frame decode │──>│ log table → PWM set  │     │
    │  └──────────┘   └──────────────┘   └──────────────────────┘     │
    │                        ↑                                         │
    │  ISRs:                 │                                         │
    │  ┌─────────────────────┴──────────────────────────────────────┐  │
    │  │ EXTI7_0  → dali_isr_rx_edge()    (Manchester edge decode) │  │
    │  │ TIM2 CC2 → dali_isr_tx_tick()    (backward frame gen)     │  │
    │  │ TIM2 CC4 → dali_isr_idle_timeout() (frame-complete detect)│  │
    │  └───────────────────────────────────────────────────────────┘   │
    └──────────────────────────────────────────────────────────────────┘

    Flash usage: ~6.5 KB (40%) of 16 KB — the log table adds ~508 bytes.
*/

#include "ch32fun.h"
#include <stdio.h>
#include "hardware.h"
#include "dali_slave.h"
#include "dali_nvm.h"

/* ====================================================================
 * millis() — Millisecond counter via SysTick
 * ====================================================================
 * ch32fun does not provide millis(), so we implement it using the
 * RISC-V SysTick timer. SysTick counts down from CMP to 0, generates
 * an interrupt, and auto-reloads. At 48 MHz with CMP = 47999, this
 * fires every 1 ms exactly.
 *
 * Used by dali_slave.c for the 15-minute INITIALISE timeout
 * (IEC 62386-102 §9.6.3).
 * ==================================================================== */
static volatile uint32_t ms_ticks = 0;

/*
 * SysTick ISR — increments the millisecond counter.
 * __attribute__((interrupt)) is required by ch32fun for RISC-V ISRs
 * (uses hardware stacking, not the standard RISC-V mret sequence).
 */
void SysTick_Handler(void) __attribute__((interrupt));
void SysTick_Handler(void) {
    SysTick->SR = 0;        /* Clear the compare-match flag */
    ms_ticks++;
}

/* Thread-safe read: ms_ticks is 32-bit, RV32 loads are atomic. */
uint32_t millis(void) {
    return ms_ticks;
}

static void millis_init(void) {
    /* CMP = (48 MHz / 1000) - 1 = 47999 → 1 ms period */
    SysTick->CMP  = (FUNCONF_SYSTEM_CORE_CLOCK / 1000) - 1;
    SysTick->CNT  = 0;           /* Reset counter */
    SysTick->SR   = 0;           /* Clear any pending flag */
    SysTick->CTLR = 0xF;         /* Bits: STE=1 (enable), STCLK=1 (HCLK),
                                  *        STIE=1 (interrupt), STRE=1 (auto-reload) */
    NVIC_EnableIRQ(SysTick_IRQn);
}

/* ====================================================================
 * PSU Control — Enable/disable external power supply
 * ====================================================================
 * PC1 is configured as a push-pull GPIO output. It goes HIGH when any
 * PWM channel has a non-zero duty cycle (DALI level > 0), and LOW when
 * all channels are off (DALI level = 0). This allows an external FET
 * or relay to control power to the LED driver stage.
 *
 * The PSU turns off immediately when level reaches 0 — no additional
 * delay is needed after fade-down because the PWM is already at 0%.
 * ==================================================================== */
static void psu_ctrl_init(void) {
    /* PA2 = push-pull output 10MHz, start LOW (PSU off) */
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOA;
    PSU_CTRL_PORT->CFGLR = (PSU_CTRL_PORT->CFGLR & ~(0xF << (PSU_CTRL_PIN_N * 4)))
                         | ((GPIO_Speed_10MHz | GPIO_CNF_OUT_PP) << (PSU_CTRL_PIN_N * 4));
    PSU_CTRL_PORT->BCR = (1 << PSU_CTRL_PIN_N);  /* Start LOW (PSU off) */
}

static inline void psu_ctrl_set(uint8_t on) {
    if (on)
        PSU_CTRL_PORT->BSHR = (1 << PSU_CTRL_PIN_N);   /* HIGH = PSU on */
    else
        PSU_CTRL_PORT->BCR  = (1 << PSU_CTRL_PIN_N);   /* LOW  = PSU off */
}

/* ====================================================================
 * TIM1 PWM — 20 kHz on 1-4 LED channels
 * ====================================================================
 * TIM1 is the "advanced" timer on CH32V003 with complementary outputs
 * and a break/dead-time register (BDTR). Unlike TIM2 (general-purpose),
 * TIM1 requires BDTR.MOE = 1 to enable outputs.
 *
 * Default pin mapping (no AFIO remap):
 *   CH1 = PD2   (GPIOD bit 2)
 *   CH2 = PA1   (GPIOA bit 1)
 *   CH3 = PC3   (GPIOC bit 3)
 *   CH4 = PC4   (GPIOC bit 4)
 *
 * PWM parameters:
 *   PSC  = 0      → timer clock = 48 MHz (no division)
 *   ATRLR = 2399  → 2400 steps (~11.2 bits resolution)
 *   Frequency = 48 MHz / (2399 + 1) = 20,000 Hz = 20 kHz
 *   Above audible range (18-22 kHz target), avoids coil whine.
 *
 * The number of active channels is controlled by PWM_NUM_CHANNELS
 * in hardware.h (1-4). All enabled channels receive the same duty
 * cycle from the DALI arc power level.
 * ==================================================================== */
static void pwm_init(void) {
    /* Enable TIM1 clock on APB2 bus */
    RCC->APB2PCENR |= RCC_APB2Periph_TIM1;

    /*
     * Configure GPIO pins for each enabled channel.
     * Each pin must be set to AF push-pull mode (CNF=10, MODE=01 for 10MHz).
     * The CFGLR register packs 4 bits per pin: [CNF1:CNF0:MODE1:MODE0].
     *
     * GPIO_Speed_10MHz = 0x01 (MODE bits)
     * GPIO_CNF_OUT_PP_AF = 0x08 (CNF bits for AF push-pull)
     * Combined: 0x09 per pin slot.
     *
     * Channels on different ports need their respective GPIO clocks enabled.
     */

    /* CH1: PD2 — enable GPIOD clock, configure PD2 as AF push-pull */
#if PWM_NUM_CHANNELS >= 1
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOD;
    PWM_CH1_PORT->CFGLR = (PWM_CH1_PORT->CFGLR & ~(0xF << (PWM_CH1_PIN_N * 4)))
                         | ((GPIO_Speed_10MHz | GPIO_CNF_OUT_PP_AF) << (PWM_CH1_PIN_N * 4));
#endif

    /* CH2: PA1 — enable GPIOA clock, configure PA1 as AF push-pull */
#if PWM_NUM_CHANNELS >= 2
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOA;
    PWM_CH2_PORT->CFGLR = (PWM_CH2_PORT->CFGLR & ~(0xF << (PWM_CH2_PIN_N * 4)))
                         | ((GPIO_Speed_10MHz | GPIO_CNF_OUT_PP_AF) << (PWM_CH2_PIN_N * 4));
#endif

    /* CH3: PC3 — GPIOC clock already enabled by dali_init (PC0/PC5) */
#if PWM_NUM_CHANNELS >= 3
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOC;
    PWM_CH3_PORT->CFGLR = (PWM_CH3_PORT->CFGLR & ~(0xF << (PWM_CH3_PIN_N * 4)))
                         | ((GPIO_Speed_10MHz | GPIO_CNF_OUT_PP_AF) << (PWM_CH3_PIN_N * 4));
#endif

    /* CH4: PC4 — same port as CH3 */
#if PWM_NUM_CHANNELS >= 4
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOC;
    PWM_CH4_PORT->CFGLR = (PWM_CH4_PORT->CFGLR & ~(0xF << (PWM_CH4_PIN_N * 4)))
                         | ((GPIO_Speed_10MHz | GPIO_CNF_OUT_PP_AF) << (PWM_CH4_PIN_N * 4));
#endif

    /* --- TIM1 base configuration --- */
    TIM1->PSC   = 0;       /* No prescaler → 48 MHz timer clock */
    TIM1->ATRLR = 2399;    /* Auto-reload = 2399 → 20 kHz PWM (2400 steps) */

    /* Initialize all compare registers to 0 (LEDs off at startup) */
#if PWM_NUM_CHANNELS >= 1
    TIM1->CH1CVR = 0;
#endif
#if PWM_NUM_CHANNELS >= 2
    TIM1->CH2CVR = 0;
#endif
#if PWM_NUM_CHANNELS >= 3
    TIM1->CH3CVR = 0;
#endif
#if PWM_NUM_CHANNELS >= 4
    TIM1->CH4CVR = 0;
#endif

    /*
     * Set PWM mode 1 for each enabled channel.
     *
     * CHCTLR1 controls CH1 and CH2:
     *   OC1M[2:0] = 110 (PWM mode 1) → bits [6:4] = 0x60
     *   OC2M[2:0] = 110 (PWM mode 1) → bits [14:12] = 0x6000
     *
     * CHCTLR2 controls CH3 and CH4:
     *   OC3M[2:0] = 110 (PWM mode 1) → bits [6:4] = 0x60
     *   OC4M[2:0] = 110 (PWM mode 1) → bits [14:12] = 0x6000
     *
     * PWM mode 1: output HIGH while CNT < CCxCVR, LOW otherwise.
     */
    {
        uint16_t chctlr1 = 0;
        uint16_t chctlr2 = 0;
        uint16_t ccer    = 0;

#if PWM_NUM_CHANNELS >= 1
        chctlr1 |= (6 << 4);       /* OC1M = PWM mode 1 */
        ccer    |= TIM_CC1E;       /* Enable CH1 output */
#endif
#if PWM_NUM_CHANNELS >= 2
        chctlr1 |= (6 << 12);      /* OC2M = PWM mode 1 */
        ccer    |= TIM_CC2E;       /* Enable CH2 output */
#endif
#if PWM_NUM_CHANNELS >= 3
        chctlr2 |= (6 << 4);       /* OC3M = PWM mode 1 */
        ccer    |= TIM_CC3E;       /* Enable CH3 output */
#endif
#if PWM_NUM_CHANNELS >= 4
        chctlr2 |= (6 << 12);      /* OC4M = PWM mode 1 */
        ccer    |= TIM_CC4E;       /* Enable CH4 output */
#endif

        TIM1->CHCTLR1 = chctlr1;
        TIM1->CHCTLR2 = chctlr2;
        TIM1->CCER    = ccer;
    }

    /*
     * BDTR (Break and Dead-Time Register):
     * TIM1 is an "advanced" timer — its outputs are disabled by default
     * until MOE (Main Output Enable) is set. Without this, no PWM appears
     * on the pins even though the timer is running. This is a common
     * gotcha when porting from TIM2/TIM3 code to TIM1.
     */
    TIM1->BDTR = TIM_MOE;

    /*
     * CTLR1: Enable auto-reload preload (ARPE) and start the counter (CEN).
     * ARPE ensures changes to ATRLR take effect at the next update event,
     * preventing glitches during runtime reconfiguration.
     */
    TIM1->CTLR1 = TIM_ARPE | TIM_CEN;
}

/*
 * pwm_set_channel() — Set individual PWM channel duty cycle.
 *
 * Parameters: ch = 0..3 (channel index), value = 0..2399.
 */
static void pwm_set_channel(uint8_t ch, uint16_t value) {
    switch (ch) {
#if PWM_NUM_CHANNELS >= 1
    case 0: TIM1->CH1CVR = value; break;
#endif
#if PWM_NUM_CHANNELS >= 2
    case 1: TIM1->CH2CVR = value; break;
#endif
#if PWM_NUM_CHANNELS >= 3
    case 2: TIM1->CH3CVR = value; break;
#endif
#if PWM_NUM_CHANNELS >= 4
    case 3: TIM1->CH4CVR = value; break;
#endif
    default: break;
    }
}

/* ====================================================================
 * IEC 62386-102 §9.3 Logarithmic Dimming Curve (20 kHz PWM)
 * ====================================================================
 * DALI specifies a logarithmic light output vs. level relationship
 * based on the Weber-Fechner law (human perception of brightness is
 * logarithmic, not linear). The formula from IEC 62386-102 §9.3:
 *
 *   X(level) = 10^((level - 1) × 3 / 253 - 1)
 *   output = X / 100 × full_scale
 *
 * Where:
 *   level   = DALI arc power level (1-254)
 *   output  = PWM compare value (0-2399 for 20 kHz at 48 MHz)
 *
 * Level 0 = OFF (special case, not in the formula)
 * Level 1 = minimum (0.1% light output → PWM ≈ 2)
 * Level 254 = maximum (100% light output → PWM = 2399)
 *
 * The lookup table avoids floating-point math in the ISR/callback.
 * At 508 bytes (254 × uint16_t), it's a worthwhile tradeoff for the
 * CH32V003 which has 16 KB flash. Each step ≈ 2.77% brightness
 * increase, so transitions appear perceptually smooth.
 *
 * Generated with Python:
 *   for i in range(1, 255):
 *       x = 10**((i - 1) * 3.0 / 253.0 - 1.0)
 *       table.append(round(x / 100.0 * 2399))
 * ==================================================================== */
static const uint16_t dali_log_table[254] = {
       2,    2,    3,    3,    3,    3,    3,    3,    3,    3,    3,    3,    3,    3,    4,    4,
       4,    4,    4,    4,    4,    4,    4,    4,    5,    5,    5,    5,    5,    5,    5,    6,
       6,    6,    6,    6,    6,    7,    7,    7,    7,    7,    8,    8,    8,    8,    8,    9,
       9,    9,    9,   10,   10,   10,   10,   11,   11,   11,   12,   12,   12,   13,   13,   13,
      14,   14,   15,   15,   15,   16,   16,   17,   17,   18,   18,   19,   19,   20,   20,   21,
      21,   22,   23,   23,   24,   24,   25,   26,   27,   27,   28,   29,   30,   30,   31,   32,
      33,   34,   35,   36,   37,   38,   39,   40,   41,   42,   43,   45,   46,   47,   48,   50,
      51,   52,   54,   55,   57,   59,   60,   62,   64,   65,   67,   69,   71,   73,   75,   77,
      79,   81,   83,   86,   88,   91,   93,   96,   98,  101,  104,  107,  110,  113,  116,  119,
     122,  126,  129,  133,  136,  140,  144,  148,  152,  156,  161,  165,  170,  174,  179,  184,
     189,  195,  200,  206,  211,  217,  223,  229,  236,  242,  249,  256,  263,  270,  278,  285,
     293,  301,  310,  318,  327,  336,  345,  355,  365,  375,  385,  396,  407,  418,  430,  441,
     454,  466,  479,  492,  506,  520,  534,  549,  564,  580,  596,  613,  630,  647,  665,  683,
     702,  722,  742,  762,  783,  805,  827,  850,  874,  898,  923,  948,  974, 1001, 1029, 1058,
    1087, 1117, 1148, 1180, 1212, 1246, 1280, 1316, 1352, 1390, 1428, 1468, 1508, 1550, 1593, 1637,
    1682, 1729, 1777, 1826, 1876, 1928, 1982, 2036, 2093, 2151, 2210, 2272, 2334, 2399,
};

/* ====================================================================
 * apply_pwm() — Recalculate per-channel PWM from arc level + colour
 * ====================================================================
 * Formula: pwm_ch[n] = log_table[arc_level] * colour_actual[n] / 254
 *
 * When all colour_actual[] values are 254 (default, DT6-compatible),
 * all channels get the same PWM value (identical to the old behavior).
 * When DT8 colour is active, each channel is scaled independently.
 * ==================================================================== */
static void apply_pwm(uint8_t level) {
    uint16_t base_pwm = (level == 0) ? 0 : dali_log_table[level - 1];
    const volatile uint8_t *colour = dali_get_colour_actual();

    for (uint8_t ch = 0; ch < PWM_NUM_CHANNELS; ch++) {
        uint16_t ch_pwm = (uint16_t)((uint32_t)base_pwm * colour[ch] / 254);
        pwm_set_channel(ch, ch_pwm);
    }

    psu_ctrl_set(level > 0);
}

/* ====================================================================
 * DALI Arc Power Callback
 * ====================================================================
 * Called from dali_process() when the DALI actual level changes.
 * Applies per-channel colour scaling via apply_pwm().
 * ==================================================================== */
static void on_level(uint8_t level) {
    apply_pwm(level);
    printf("LVL=%d\n", level);
}

/* ====================================================================
 * DT8 Colour Callback
 * ====================================================================
 * Called from dali_process() when ACTIVATE commits new colour values.
 * Recalculates per-channel PWM using current arc level + new colour.
 * ==================================================================== */
static void on_colour(const uint8_t *levels, uint8_t count) {
    apply_pwm(dali_get_actual_level());
    printf("CLR R=%d G=%d B=%d W=%d\n",
           levels[0], levels[1],
           count > 2 ? levels[2] : 0,
           count > 3 ? levels[3] : 0);
}

/* ====================================================================
 * ISR Wrappers — Connect hardware interrupts to DALI state machines
 * ====================================================================
 * ch32fun uses the __attribute__((interrupt)) annotation for ISRs.
 * The CH32V003 RISC-V core uses hardware stacking (HPE) for fast ISR
 * entry — the compiler inserts the correct prologue/epilogue when
 * this attribute is present.
 *
 * EXTI7_0_IRQHandler: Handles ALL EXTI lines 0-7. We only use EXTI0
 *   (PC0 = DALI RX). The pending flag MUST be cleared first to avoid
 *   re-entry (EXTI->INTFR write-1-to-clear).
 *
 * TIM2_IRQHandler: Handles all TIM2 interrupt sources. We check
 *   individual channel flags:
 *   - CC2 (CH2): TX Manchester waveform generation (Te tick)
 *   - CC4 (CH4): RX idle timeout (frame-complete detection)
 *   Flags are cleared with write-0-to-clear (inverted bit pattern).
 * ==================================================================== */
void EXTI7_0_IRQHandler(void) __attribute__((interrupt));
void EXTI7_0_IRQHandler(void) {
    EXTI->INTFR = EXTI_Line0;   /* Clear EXTI0 pending flag (W1C) */
    dali_isr_rx_edge();          /* Run RX Manchester state machine */
}

void TIM2_IRQHandler(void) __attribute__((interrupt));
void TIM2_IRQHandler(void) {
    /* Check CC2 (TX Te tick) — generates Manchester half-bit transitions */
    if (TIM2->INTFR & TIM_IT_CC2) {
        TIM2->INTFR = ~TIM_IT_CC2;  /* Clear CC2 flag (W0C) */
        dali_isr_tx_tick();
    }
    /* Check CC4 (RX idle timeout) — no edge for 5 Te = frame complete */
    if (TIM2->INTFR & TIM_IT_CC4) {
        TIM2->INTFR = ~TIM_IT_CC4;  /* Clear CC4 flag (W0C) */
        dali_isr_idle_timeout();
    }
}

/* ====================================================================
 * Main Entry Point
 * ====================================================================
 * Initialization order matters:
 *   1. SystemInit()       — PLL setup (24 MHz HSI → 48 MHz), flash wait
 *   2. funGpioInitAll()   — Enable all GPIO port clocks
 *   3. PC6 input-pulldown — Deassert USB D+ pull-up left by bootloader
 *   4. millis_init()      — SysTick 1 ms tick (used by DALI timeout)
 *   5. psu_ctrl_init()    — PSU enable GPIO on PA2
 *   6. pwm_init()         — TIM1 PWM on configured channels
 *   7. dali_init()        — TIM2 + EXTI0 for DALI RX/TX
 *   8. dali_set_arc_callback() — Connect dimming to PWM
 *
 * The main loop just calls dali_process() which checks for received
 * frames and dispatches them. There is no delay or sleep — the MCU
 * runs at full speed. Power consumption is not a concern for a
 * mains-powered DALI control gear.
 * ==================================================================== */
int main(void) {
    SystemInit();           /* Configure PLL: HSI 24 MHz × 2 = 48 MHz */
    funGpioInitAll();       /* Enable all GPIO port clocks */

    /* PC6 (USB_ENUM): input pull-down — deasserts bootloader D+ pull-up */
    USB_ENUM_PORT->CFGLR = (USB_ENUM_PORT->CFGLR & ~(0xF << (USB_ENUM_PIN_N * 4)))
                         | (0x8 << (USB_ENUM_PIN_N * 4));  /* CNF=10 (pull), MODE=00 (in) */
    USB_ENUM_PORT->BCR   = (1 << USB_ENUM_PIN_N);          /* ODR=0 → pull-down */

    millis_init();          /* Start SysTick → millis() available */
    psu_ctrl_init();        /* Configure PA2 as PSU enable output */
    pwm_init();             /* Configure TIM1 PWM on LED channels */
    dali_init();            /* Configure TIM2 + EXTI0 for DALI */
    dali_set_arc_callback(on_level);      /* Connect level changes to PWM */
    dali_set_colour_callback(on_colour);  /* Connect DT8 colour to PWM */
    nvm_init();                           /* Load persistent state from flash */
    dali_power_on();                      /* Apply power-on level */

    printf("DALI DT8 %dch ready\n", PWM_NUM_CHANNELS);

    /* Main loop — all functions are non-blocking, return immediately
     * if nothing to do. dali_process() dispatches received frames.
     * dali_fade_tick() steps the fade engine (1 level per interval).
     * nvm_tick() saves dirty config to flash after 5s delay.
     *
     * __WFI() (Sleep mode, SLEEPDEEP=0): halts CPU core, all peripherals
     * continue running (TIM2, EXTI, SysTick). Only entered when:
     *   1. TX is idle (no backward frame in progress)
     *   2. Bus quiet for >20ms (>48 Te) — no frame in progress or just finished
     * This guarantees WFI is never active during DALI frame reception/transmission.
     * SysTick wakes the CPU every 1ms; the bus-idle check prevents re-entry during
     * active communication. During DALI idle periods (typical: seconds to minutes),
     * the CPU sleeps ~99% of the time. */
    while (1) {
        dali_process();
        dali_fade_tick();
        nvm_tick();
        if (dali_is_tx_idle() && (millis() - dali_last_rx_edge_ms() > 20)) {
            __WFI();
        }
    }
}
