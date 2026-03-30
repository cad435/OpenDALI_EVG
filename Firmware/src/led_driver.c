/*
    led_driver.c - LED output driver implementations

    Contains two compile-time selectable LED drivers:

    Default (DIGITAL_LED_OUT not defined):
      TIM1 PWM on up to 4 channels (PD2, PA1, PC3, PC4).
      2400-step resolution at 20 kHz. Per-channel colour scaling
      for DT8 RGBW support. Direct register writes.

    DIGITAL_LED_OUT defined:
      WS2812 / SK6812 addressable LED strip via SPI1+DMA on PC6 (MOSI).
      Non-blocking DMA transfer. All LEDs output the same colour
      (DALI controls the entire fixture as one unit).
      Supports WS2812 (3-byte GRB) and SK6812 RGBW (4-byte GRBW).

    The driver is selected by DIGITAL_LED_OUT / ONOFF_MODE in hardware.h.
*/

#include "ch32fun.h"
#include "led_driver.h"
#include "hardware.h"

#ifdef ONOFF_MODE
/* ********************************************************************
 * ON/OFF MODE — No LED driver, PSU_CTRL only (handled in main.c)
 * ******************************************************************** */
void led_driver_init(void) {}
void led_driver_apply(uint8_t dali_level, const volatile uint8_t *colour) { (void)dali_level; (void)colour; }
void led_driver_refresh(void) {}

#else /* !ONOFF_MODE */

/* ====================================================================
 * IEC 62386-102 §9.3 Logarithmic Dimming Curve
 * ====================================================================
 * DALI specifies logarithmic light output (Weber-Fechner law).
 * Formula: X(level) = 10^((level - 1) × 3 / 253 - 1)
 *          output = X / 100 × full_scale
 *
 * Table maps DALI level 1–254 → PWM value 0–2399.
 * Level 0 = OFF (not in table, handled as special case).
 *
 * For WS2812: values are scaled to 0–255 at runtime.
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


#ifndef DIGITAL_LED_OUT
/* ********************************************************************
 * PWM DRIVER — TIM1 on up to 4 channels
 * ******************************************************************** */

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

void led_driver_init(void) {
    RCC->APB2PCENR |= RCC_APB2Periph_TIM1;

#if PWM_NUM_CHANNELS >= 1
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOD;
    PWM_CH1_PORT->CFGLR = (PWM_CH1_PORT->CFGLR & ~(0xF << (PWM_CH1_PIN_N * 4)))
                         | ((GPIO_Speed_10MHz | GPIO_CNF_OUT_PP_AF) << (PWM_CH1_PIN_N * 4));
#endif
#if PWM_NUM_CHANNELS >= 2
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOA;
    PWM_CH2_PORT->CFGLR = (PWM_CH2_PORT->CFGLR & ~(0xF << (PWM_CH2_PIN_N * 4)))
                         | ((GPIO_Speed_10MHz | GPIO_CNF_OUT_PP_AF) << (PWM_CH2_PIN_N * 4));
#endif
#if PWM_NUM_CHANNELS >= 3
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOC;
    PWM_CH3_PORT->CFGLR = (PWM_CH3_PORT->CFGLR & ~(0xF << (PWM_CH3_PIN_N * 4)))
                         | ((GPIO_Speed_10MHz | GPIO_CNF_OUT_PP_AF) << (PWM_CH3_PIN_N * 4));
#endif
#if PWM_NUM_CHANNELS >= 4
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOC;
    PWM_CH4_PORT->CFGLR = (PWM_CH4_PORT->CFGLR & ~(0xF << (PWM_CH4_PIN_N * 4)))
                         | ((GPIO_Speed_10MHz | GPIO_CNF_OUT_PP_AF) << (PWM_CH4_PIN_N * 4));
#endif

    TIM1->PSC   = 0;
    TIM1->ATRLR = 2399;

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

    {
        uint16_t chctlr1 = 0, chctlr2 = 0, ccer = 0;
#if PWM_NUM_CHANNELS >= 1
        chctlr1 |= (6 << 4);
        ccer    |= TIM_CC1E;
#endif
#if PWM_NUM_CHANNELS >= 2
        chctlr1 |= (6 << 12);
        ccer    |= TIM_CC2E;
#endif
#if PWM_NUM_CHANNELS >= 3
        chctlr2 |= (6 << 4);
        ccer    |= TIM_CC3E;
#endif
#if PWM_NUM_CHANNELS >= 4
        chctlr2 |= (6 << 12);
        ccer    |= TIM_CC4E;
#endif
        TIM1->CHCTLR1 = chctlr1;
        TIM1->CHCTLR2 = chctlr2;
        TIM1->CCER    = ccer;
    }

    TIM1->BDTR  = TIM_MOE;
    TIM1->CTLR1 = TIM_ARPE | TIM_CEN;
}

void led_driver_apply(uint8_t dali_level, const volatile uint8_t *colour) {
    uint16_t base_pwm = (dali_level == 0) ? 0 : dali_log_table[dali_level - 1];

    for (uint8_t ch = 0; ch < PWM_NUM_CHANNELS; ch++) {
        uint16_t ch_pwm = (uint16_t)((uint32_t)base_pwm * colour[ch] / 254);
        pwm_set_channel(ch, ch_pwm);
    }
}

void led_driver_refresh(void) {
    /* PWM hardware maintains duty cycle autonomously — nothing to do */
}

#else /* DIGITAL_LED_OUT */
/* ********************************************************************
 * WS2812 / SK6812 DRIVER — SPI1+DMA on PC6 (MOSI)
 * ********************************************************************
 *
 * Uses SPI1 in transmit-only mode with DMA to generate the WS2812/SK6812
 * bit stream on PC6 (SPI1 MOSI, default pin mapping on CH32V003).
 *
 * SPI clock = 48 MHz / 16 = 3 MHz → each SPI bit = 333 ns.
 * Each WS2812 data bit is encoded as 4 SPI bits:
 *   WS2812 "1" = 0b1110 → 1000 ns high, 333 ns low
 *   WS2812 "0" = 0b1000 → 333 ns high, 1000 ns low
 *
 * A nibble LUT converts 4 data bits → 16 SPI bits (one uint16_t).
 * Each LED colour byte = 2 nibbles = 2 × uint16_t = 4 bytes of SPI data.
 *
 * All LEDs output the same colour (DALI controls the fixture as a whole).
 *
 * DMA runs in circular mode with half-transfer + transfer-complete
 * interrupts. The ISR refills the buffer halves on-the-fly, allowing
 * arbitrary strip lengths with a small fixed buffer.
 * ******************************************************************** */

#if WS2812_TYPE == WS2812_TYPE_SK6812_RGBW
#define WS2812_BYTES_PER_LED 4
#else
#define WS2812_BYTES_PER_LED 3
#endif

/* DMA buffer: holds SPI data for DMALEDS LEDs at a time (double-buffered).
 * Each LED byte → 2 uint16_t (nibble LUT), so each LED = BYTES_PER_LED × 2.
 * DMALEDS must be divisible by 2 for half-transfer DMA. */
#define DMALEDS 8
#define DMA_BUFFER_LEN (DMALEDS * WS2812_BYTES_PER_LED * 2)

/* Reset period: number of "LED slots" of zeros to send as the reset pulse.
 * 2 slots × bytes × halfwords × 333 ns ≈ 50+ µs (>280 µs with 8-LED slots). */
#define WS2812_RESET_SLOTS 2

static uint16_t ws2812_dma_buf[DMA_BUFFER_LEN];

/* Current strip state */
static volatile int ws2812_total_leds;
static volatile int ws2812_place;
static volatile int ws2812_in_use;

/* Colour to send: computed once per led_driver_apply(), read by ISR.
 * Packed as raw bytes in wire order (GRB or GRBW). */
static uint8_t ws2812_colour[WS2812_BYTES_PER_LED];

/*
 * Nibble-to-SPI lookup table.
 * Each entry encodes 4 WS2812 data bits as 16 SPI bits.
 * SPI transmits MSB first; each WS2812 bit maps to 4 SPI bits:
 *   data "1" → 1110 (high for 3 × 333 ns = 1 µs, low for 333 ns)
 *   data "0" → 1000 (high for 333 ns, low for 1 µs)
 */
static const uint16_t nibble_lut[16] = {
    0b1000100010001000, 0b1000100010001110, 0b1000100011101000, 0b1000100011101110,
    0b1000111010001000, 0b1000111010001110, 0b1000111011101000, 0b1000111011101110,
    0b1110100010001000, 0b1110100010001110, 0b1110100011101000, 0b1110100011101110,
    0b1110111010001000, 0b1110111010001110, 0b1110111011101000, 0b1110111011101110,
};

/*
 * Fill a section of the DMA buffer with SPI-encoded LED data.
 * Called from the DMA ISR to refill each buffer half on-the-fly.
 *
 * Parameters:
 *   ptr:          start of the buffer section to fill
 *   numhalfwords: number of uint16_t entries to fill
 *   is_second:    1 if this is the second (tail) half of the circular buffer
 */
static void ws2812_fill_buf(uint16_t *ptr, int numhalfwords, int is_second) {
    uint16_t *end = ptr + numhalfwords;
    int place = ws2812_place;
    int total = ws2812_total_leds;

    /* Reset period: output zeros */
    while (place < 0 && ptr != end) {
        for (int i = 0; i < WS2812_BYTES_PER_LED * 2 && ptr != end; i++)
            *ptr++ = 0;
        place++;
    }

    /* LED data: all LEDs get the same colour */
    while (ptr != end) {
        if (place >= total) {
            /* Past end of strip: fill zeros, then stop DMA */
            while (ptr != end)
                *ptr++ = 0;
            if (is_second) {
                if (place == total) {
                    DMA1_Channel3->CFGR &= ~DMA_Mode_Circular;
                    ws2812_in_use = 0;
                }
                place++;
            }
            break;
        }

        /* Encode colour bytes as SPI nibble pairs */
        for (int b = 0; b < WS2812_BYTES_PER_LED; b++) {
            uint8_t byte = ws2812_colour[b];
            *ptr++ = nibble_lut[(byte >> 4) & 0xF];
            *ptr++ = nibble_lut[byte & 0xF];
        }
        place++;
    }

    ws2812_place = place;
}

/*
 * DMA1 Channel 3 ISR — SPI1 TX DMA.
 * Fires at half-transfer and transfer-complete to refill the circular buffer.
 */
void DMA1_Channel3_IRQHandler(void) __attribute__((interrupt));
void DMA1_Channel3_IRQHandler(void) {
    volatile int intfr = DMA1->INTFR;
    do {
        DMA1->INTFCR = DMA1_IT_GL3;

        if (intfr & DMA1_IT_HT3) {
            ws2812_fill_buf(ws2812_dma_buf, DMA_BUFFER_LEN / 2, 1);
        }
        if (intfr & DMA1_IT_TC3) {
            ws2812_fill_buf(ws2812_dma_buf + DMA_BUFFER_LEN / 2, DMA_BUFFER_LEN / 2, 0);
        }
        intfr = DMA1->INTFR;
    } while (intfr & DMA1_IT_GL3);
}

/*
 * Start a DMA transfer to update the entire LED strip.
 */
static void ws2812_start(void) {
    /* Wait for any previous transfer to complete */
    while (ws2812_in_use) {}

    __disable_irq();
    ws2812_in_use = 1;
    DMA1_Channel3->CFGR &= ~DMA_Mode_Circular;
    DMA1_Channel3->CNTR  = 0;
    DMA1_Channel3->MADDR = (uint32_t)ws2812_dma_buf;
    __enable_irq();

    ws2812_total_leds = WS2812_NUM_LEDS;
    ws2812_place = -WS2812_RESET_SLOTS;

    /* Pre-fill entire buffer, then start circular DMA */
    ws2812_fill_buf(ws2812_dma_buf, DMA_BUFFER_LEN, 0);
    DMA1_Channel3->CNTR = DMA_BUFFER_LEN;
    DMA1_Channel3->CFGR |= DMA_Mode_Circular;
}

void led_driver_init(void) {
    /* Enable clocks: DMA1, GPIOC, SPI1 */
    RCC->AHBPCENR  |= RCC_AHBPeriph_DMA1;
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOC | RCC_APB2Periph_SPI1;

    /* PC6 = SPI1 MOSI: AF push-pull, 10 MHz */
    GPIOC->CFGLR = (GPIOC->CFGLR & ~(0xF << (6 * 4)))
                 | ((GPIO_Speed_10MHz | GPIO_CNF_OUT_PP_AF) << (6 * 4));

    /* SPI1: master, TX-only, 16-bit, 3 MHz (48/16), CPOL=0/CPHA=0 */
    SPI1->CTLR1 = SPI_NSS_Soft | SPI_CPHA_1Edge | SPI_CPOL_Low
                 | SPI_DataSize_16b | SPI_Mode_Master
                 | SPI_Direction_1Line_Tx
                 | (3 << 3);    /* BR[2:0] = 011 → /16 → 3 MHz */

    SPI1->CTLR2 = SPI_CTLR2_TXDMAEN;

#if defined(CH32V003)
    SPI1->HSCR = 1;    /* High-speed read mode (CH32V003 specific) */
#endif

    SPI1->CTLR1 |= CTLR1_SPE_Set;
    SPI1->DATAR = 0;   /* Set MOSI line LOW initially */

    /* DMA1 Channel 3: SPI1 TX */
    DMA1_Channel3->PADDR = (uint32_t)&SPI1->DATAR;
    DMA1_Channel3->MADDR = (uint32_t)ws2812_dma_buf;
    DMA1_Channel3->CNTR  = 0;
    DMA1_Channel3->CFGR  = DMA_M2M_Disable
                          | DMA_Priority_VeryHigh
                          | DMA_MemoryDataSize_HalfWord
                          | DMA_PeripheralDataSize_HalfWord
                          | DMA_MemoryInc_Enable
                          | DMA_Mode_Normal
                          | DMA_DIR_PeripheralDST
                          | DMA_IT_TC | DMA_IT_HT;

    NVIC_EnableIRQ(DMA1_Channel3_IRQn);
    DMA1_Channel3->CFGR |= DMA_CFGR1_EN;

    /* Default colour: all off */
    for (int i = 0; i < WS2812_BYTES_PER_LED; i++)
        ws2812_colour[i] = 0;
}

void led_driver_apply(uint8_t dali_level, const volatile uint8_t *colour) {
    if (dali_level == 0) {
        for (int i = 0; i < WS2812_BYTES_PER_LED; i++)
            ws2812_colour[i] = 0;
    } else {
        uint16_t base_pwm = dali_log_table[dali_level - 1];

        /* Scale DALI level (0–2399) × colour (0–254) → byte (0–255) */
        uint8_t r = (uint8_t)((uint32_t)base_pwm * colour[0] * 255 / (254 * 2399));
        uint8_t g = (uint8_t)((uint32_t)base_pwm * colour[1] * 255 / (254 * 2399));
        uint8_t b = (uint8_t)((uint32_t)base_pwm * colour[2] * 255 / (254 * 2399));

        /* Ensure minimum visible output at level 1 */
        if (r == 0 && colour[0] > 0) r = 1;
        if (g == 0 && colour[1] > 0) g = 1;
        if (b == 0 && colour[2] > 0) b = 1;

        /* Pack in wire order: GRB */
        ws2812_colour[0] = g;
        ws2812_colour[1] = r;
        ws2812_colour[2] = b;

#if WS2812_TYPE == WS2812_TYPE_SK6812_RGBW
        {
            uint8_t w = (uint8_t)((uint32_t)base_pwm * colour[3] * 255 / (254 * 2399));
            if (w == 0 && colour[3] > 0) w = 1;
            ws2812_colour[3] = w;
        }
#endif
    }

    ws2812_start();
}

void led_driver_refresh(void) {
    ws2812_start();
}

#endif /* DIGITAL_LED_OUT */
#endif /* !ONOFF_MODE */
