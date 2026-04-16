/*
    dali_phy.c - DALI physical layer (IEC 62386-101) for CH32V003

    Manchester-encoded forward frame RX (16-bit) and backward frame TX
    (8-bit) via EXTI edge interrupts and TIM2 output compare ISRs.

    Self-contained module — no dependency on DALI protocol state.
    Communicates with the protocol layer only through:
      - dali_phy_frame_ready() / dali_phy_frame_bytes() (RX output)
      - dali_phy_send_backward() (TX input)
*/

#include "ch32fun.h"
#include "dali_phy.h"
#include "../dali_physical.h"
#include "../../config/hardware.h"

/* millis() provided by main.c */
extern uint32_t millis(void);

/* ================================================================== *
 *  RX STATE MACHINE                                                   *
 * ================================================================== */
typedef enum {
    RX_IDLE,
    RX_START,
    RX_BIT
} rx_state_t;

/* ================================================================== *
 *  TX STATE MACHINE                                                   *
 * ================================================================== */
typedef enum {
    TX_IDLE,
    TX_SETTLE,
    TX_START_LO,
    TX_START_HI,
    TX_BIT_1ST,
    TX_BIT_2ND,
    TX_STOP1,
    TX_STOP2,
    TX_STOP3,
    TX_STOP4
} tx_state_t;

/* ── RX state ────────────────────────────────────────────────────── */
static volatile rx_state_t  rx_state = RX_IDLE;
static volatile uint32_t    rx_last_edge_ms = 0;
static volatile uint16_t    rx_last_capture = 0;
static volatile uint8_t     rx_last_bus_low = 0;
static volatile uint8_t     rx_len = 0;
static volatile uint8_t     rx_msg[3] = {0};
static volatile uint8_t     rx_frame_ready = 0;

/* ── TX state ────────────────────────────────────────────────────── */
static volatile tx_state_t  tx_state = TX_IDLE;
static volatile uint8_t     tx_msg = 0;
static volatile uint8_t     tx_pos = 0;
static volatile uint8_t     bus_idle_te_cnt = 0;

/* Echo / collision tracking (IEC 62386-101 §8.2.4.4) */
static volatile uint8_t     tx_last_active = 0;
static volatile uint8_t     tx_collision_flag = 0;

/* ================================================================== *
 *  TX PIN HELPERS — polarity depends on DALI_NO_PHY                   *
 * ================================================================== */
#ifdef DALI_NO_PHY
static inline void tx_bus_active(void) {
    DALI_TX_PORT->BCR = (1 << DALI_TX_PIN_N);
}
static inline void tx_bus_idle(void) {
    DALI_TX_PORT->BSHR = (1 << DALI_TX_PIN_N);
}
static inline uint8_t rx_bus_is_active(void) {
    return !(DALI_RX_PORT->INDR & (1 << DALI_RX_PIN_N));
}
#else
static inline void tx_bus_active(void) {
    DALI_TX_PORT->BSHR = (1 << DALI_TX_PIN_N);
}
static inline void tx_bus_idle(void) {
    DALI_TX_PORT->BCR = (1 << DALI_TX_PIN_N);
}
static inline uint8_t rx_bus_is_active(void) {
    return !!(DALI_RX_PORT->INDR & (1 << DALI_RX_PIN_N));
}
#endif

static inline void tx_drive_active(void) { tx_bus_active(); tx_last_active = 1; }
static inline void tx_drive_idle(void)   { tx_bus_idle();   tx_last_active = 0; }

/* ================================================================== *
 *  push_halfbit() — RX half-bit accumulator                           *
 * ================================================================== */
static void push_halfbit(uint8_t bit) {
    bit &= 1;
    if ((rx_len & 1) == 0) {
        uint8_t i = rx_len >> 4;
        if (i < 3) {
            rx_msg[i] = (rx_msg[i] << 1) | bit;
        }
    }
    rx_len++;
}

/* ================================================================== *
 *  ISR: EXTI edge on PC0 (DALI RX)                                   *
 * ================================================================== */
void dali_isr_rx_edge(void) {
    if (tx_state != TX_IDLE) return;

    uint16_t capture = (uint16_t)(TIM2->CNT);
    uint8_t bus_low = rx_bus_is_active();
    uint16_t dt = capture - rx_last_capture;

    if (dt < 200) return;

    rx_last_edge_ms = millis();

    TIM2->CH4CVR = capture + DALI_IDLE_TIMEOUT;
    TIM2->INTFR = ~TIM_IT_CC4;
    TIM2->DMAINTENR |= TIM_IT_CC4;

    switch (rx_state) {
    case RX_IDLE:
        if (bus_low) {
            rx_last_capture = capture;
            rx_state = RX_START;
        }
        break;

    case RX_START:
        if (!bus_low && DALI_IS_TE(dt)) {
            rx_last_capture = capture;
            rx_len = 0;
            rx_msg[0] = 0;
            rx_msg[1] = 0;
            rx_msg[2] = 0;
            rx_state = RX_BIT;
            rx_last_bus_low = 0;
        } else if (bus_low) {
            rx_last_capture = capture;
        } else {
            rx_state = RX_IDLE;
        }
        break;

    case RX_BIT:
        if (DALI_IS_TE(dt)) {
            rx_last_capture = capture;
            push_halfbit(bus_low);
        } else if (DALI_IS_2TE(dt)) {
            rx_last_capture = capture;
            push_halfbit(rx_last_bus_low);
            push_halfbit(bus_low);
        } else {
            rx_state = RX_IDLE;
        }
        rx_last_bus_low = bus_low;
        break;
    }
}

/* ================================================================== *
 *  ISR: TIM2 CH2 output compare — TX Te tick                         *
 * ================================================================== */
void dali_isr_tx_tick(void) {
    TIM2->CH2CVR += DALI_TE_TICKS;

    /* Echo / collision check */
    if (tx_state != TX_SETTLE && tx_state != TX_START_LO) {
        uint8_t bus_active_now = rx_bus_is_active();
        if (bus_active_now != tx_last_active) {
            tx_collision_flag = 1;
            tx_drive_idle();
            tx_state = TX_IDLE;
            TIM2->DMAINTENR &= ~TIM_IT_CC2;
            return;
        }
    }

    switch (tx_state) {
    case TX_IDLE:
        TIM2->DMAINTENR &= ~TIM_IT_CC2;
        return;

    case TX_SETTLE:
        bus_idle_te_cnt++;
        if (bus_idle_te_cnt >= DALI_SETTLE_TE) {
            tx_state = TX_START_LO;
        }
        break;

    case TX_START_LO:
        tx_drive_active();
        tx_state = TX_START_HI;
        break;

    case TX_START_HI:
        tx_drive_idle();
        tx_pos = 0;
        tx_state = TX_BIT_1ST;
        break;

    case TX_BIT_1ST:
        if (tx_pos >= 8) {
            tx_drive_idle();
            tx_state = TX_STOP1;
            break;
        }
        if (tx_msg & (1 << (7 - tx_pos))) {
            tx_drive_active();
        } else {
            tx_drive_idle();
        }
        tx_state = TX_BIT_2ND;
        break;

    case TX_BIT_2ND:
        if (tx_msg & (1 << (7 - tx_pos))) {
            tx_drive_idle();
        } else {
            tx_drive_active();
        }
        tx_pos++;
        tx_state = TX_BIT_1ST;
        break;

    case TX_STOP1: tx_drive_idle(); tx_state = TX_STOP2; break;
    case TX_STOP2: tx_drive_idle(); tx_state = TX_STOP3; break;
    case TX_STOP3: tx_drive_idle(); tx_state = TX_STOP4; break;
    case TX_STOP4:
        tx_drive_idle();
        tx_state = TX_IDLE;
        TIM2->DMAINTENR &= ~TIM_IT_CC2;
        break;
    }
}

/* ================================================================== *
 *  ISR: TIM2 CH4 output compare — idle timeout (frame complete)       *
 * ================================================================== */
void dali_isr_idle_timeout(void) {
    if (rx_state == RX_BIT) {
        rx_frame_ready = 1;
    }
    rx_state = RX_IDLE;
    TIM2->DMAINTENR &= ~TIM_IT_CC4;
}

/* ================================================================== *
 *  PUBLIC API                                                         *
 * ================================================================== */

void dali_phy_init(void) {
    /* TIM2: free-running 1 MHz counter */
    RCC->APB1PCENR |= RCC_APB1Periph_TIM2;
    TIM2->PSC   = DALI_TIMER_PSC;
    TIM2->ATRLR = DALI_TIMER_ARR;
    TIM2->DMAINTENR = 0;
    TIM2->CTLR1 = TIM_CEN;
    NVIC_EnableIRQ(TIM2_IRQn);

    /* EXTI on DALI_RX_PIN: both-edge for DALI RX.
     * AFIO EXTICR: 2 bits per line, port select (00=PA, 10=PC, 11=PD).
     * Line number = pin number, so EXTI3 for PC3. */
    RCC->APB2PCENR |= RCC_APB2Periph_AFIO;
    DALI_RX_PORT->CFGLR = (DALI_RX_PORT->CFGLR & ~(0xF << (DALI_RX_PIN_N * 4)))
                         | (GPIO_CNF_IN_FLOATING << (DALI_RX_PIN_N * 4));
    AFIO->EXTICR = (AFIO->EXTICR & ~(0x03 << (DALI_RX_PIN_N * 2)))
                 | (0x02 << (DALI_RX_PIN_N * 2));   /* 0x02 = port C */
    EXTI->RTENR  |= DALI_RX_EXTI_LINE;
    EXTI->FTENR  |= DALI_RX_EXTI_LINE;
    EXTI->INTENR |= DALI_RX_EXTI_LINE;
    NVIC_EnableIRQ(EXTI7_0_IRQn);

    /* TX pin: push-pull output, idle state */
    DALI_TX_PORT->CFGLR = (DALI_TX_PORT->CFGLR & ~(0xF << (DALI_TX_PIN_N * 4)))
                        | ((GPIO_Speed_10MHz | GPIO_CNF_OUT_PP) << (DALI_TX_PIN_N * 4));
    tx_bus_idle();
}

void dali_phy_send_backward(uint8_t data) {
    if (tx_state != TX_IDLE) return;

    tx_msg = data;
    tx_pos = 0;
    bus_idle_te_cnt = 0;
    tx_state = TX_SETTLE;

    TIM2->CH2CVR = TIM2->CNT + DALI_TE_TICKS;
    TIM2->INTFR = ~TIM_IT_CC2;
    TIM2->DMAINTENR |= TIM_IT_CC2;
}

uint8_t dali_phy_is_tx_idle(void) {
    return (tx_state == TX_IDLE);
}

uint8_t dali_phy_consume_collision(void) {
    uint8_t c = tx_collision_flag;
    tx_collision_flag = 0;
    return c;
}

uint8_t dali_phy_frame_ready(void) {
    if (rx_frame_ready) {
        rx_frame_ready = 0;
        return 1;
    }
    return 0;
}

uint8_t dali_phy_frame_bits(void) {
    return rx_len >> 1;
}

void dali_phy_frame_bytes(uint8_t *out) {
    out[0] = rx_msg[0];
    out[1] = rx_msg[1];
    out[2] = rx_msg[2];
}

uint32_t dali_phy_last_rx_edge_ms(void) {
    return rx_last_edge_ms;
}
