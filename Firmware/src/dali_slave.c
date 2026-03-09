/*
    dali_slave.c - DALI control gear (slave) for CH32V003 (ch32fun)

    Ported from CH32V003_DALI_Slave.cpp (Arduino version).
    Implements IEC 62386-101 (physical layer), IEC 62386-102 (protocol),
    and IEC 62386-209 (DT8 colour control).

    Supports:
    - Forward frame RX: 16-bit Manchester decoding via EXTI + TIM2
    - Backward frame TX: 8-bit Manchester encoding via TIM2 output compare
    - Broadcast (0xFE/0xFF) and short address (0AAAAAA S) commands
    - Direct arc power (S=0) with 0xFF MASK handling and fadeTime support
    - Fade engine: fadeTime for arc power, fadeRate for UP/DOWN (IEC 62386-102 §9.5)
    - Commands: OFF, UP, DOWN, STEP UP, STEP DOWN, RECALL MAX, RECALL MIN
    - Config commands: STORE DTR AS FADE TIME/RATE (with config repeat)
    - Queries: STATUS, GEAR PRESENT, MISSING SHORT, VERSION, DTR0,
      DEVICE TYPE, PHYS MIN, ACTUAL LEVEL, MAX/MIN LEVEL, POWER ON LEVEL,
      FADE SPEEDS, RANDOM ADDRESS H/M/L
    - Full addressing protocol: INITIALISE, RANDOMISE, COMPARE,
      SEARCHADDR H/M/L, PROGRAM SHORT, WITHDRAW, VERIFY SHORT,
      QUERY SHORT, TERMINATE
    - DTR0/DTR1/DTR2 storage
    - DT8 colour control: ENABLE_DT, ACTIVATE, SET_TEMP_RGB/WAF,
      SET_TEMP_COLOUR_TEMPERATURE, STEP_COOLER/WARMER, DT8 queries 247–252

    Spec compliance:
    - QUERY MISSING SHORT ADDRESS: no backward frame when address IS assigned
    - Direct arc power 0xFF (MASK): no action (level unchanged)
    - Config repeat: commands 32–128 require 2× within 100 ms
    - DT8 extended commands require preceding ENABLE DEVICE TYPE 8
*/

#include "ch32fun.h"
#include <stdio.h>
#include "dali_slave.h"
#include "dali_physical.h"
#include "dali_nvm.h"
#include "hardware.h"

/* millis() is provided by main.c via SysTick — used for config repeat
   timing validation and initialisation state timeout. */
extern uint32_t millis(void);

/* ================================================================== *
 *  RX STATE MACHINE                                                   *
 *                                                                     *
 *  Decodes Manchester-encoded forward frames from EXTI edge events.   *
 *  State transitions:                                                 *
 *    IDLE ──(falling edge)──> START                                   *
 *    START ──(rising edge, Te)──> BIT  (valid start bit detected)     *
 *    START ──(invalid)──> IDLE                                        *
 *    BIT ──(Te/2Te edges)──> BIT  (accumulate half-bits)              *
 *    BIT ──(idle timeout)──> IDLE + frame_ready                       *
 * ================================================================== */
typedef enum {
    RX_IDLE,    /* Waiting for start bit (bus idle, no activity) */
    RX_START,   /* Detected falling edge, verifying start bit duration */
    RX_BIT      /* Decoding data bits (accumulating half-bits) */
} rx_state_t;

/* ================================================================== *
 *  TX STATE MACHINE                                                   *
 *                                                                     *
 *  Generates Manchester-encoded backward frames via TIM2 CH2 OC ISR.  *
 *  Each state transition occurs at a Te (417 µs) interval.            *
 *                                                                     *
 *  Timeline (Te ticks after send_backward_frame() call):              *
 *  ┌─────────┬──────────────────────────────────────────┐             *
 *  │ Tick 1-10 │ TX_SETTLE: wait for bus settle          │            *
 *  │ Tick 11  │ TX_START_LO: pin LOW  (start bit 1st half)│           *
 *  │ Tick 12  │ TX_START_HI: pin HIGH (start bit 2nd half)│           *
 *  │ Tick 13-28 │ TX_BIT_1ST/2ND: 8 data bits × 2 halves│            *
 *  │ Tick 29-32 │ TX_STOP1-4: 2 stop bits (bus HIGH)     │           *
 *  └─────────┴──────────────────────────────────────────┘             *
 * ================================================================== */
typedef enum {
    TX_IDLE,        /* Not transmitting */
    TX_SETTLE,      /* Waiting DALI_SETTLE_TE ticks before frame start */
    TX_START_LO,    /* Start bit: first half = active (LOW) */
    TX_START_HI,    /* Start bit: second half = idle (HIGH) */
    TX_BIT_1ST,     /* Data bit: first half (active for 1, idle for 0) */
    TX_BIT_2ND,     /* Data bit: second half (idle for 1, active for 0) */
    TX_STOP1,       /* Stop bit 1, first half (HIGH) */
    TX_STOP2,       /* Stop bit 1, second half (HIGH) */
    TX_STOP3,       /* Stop bit 2, first half (HIGH) */
    TX_STOP4        /* Stop bit 2, second half (HIGH) → TX_IDLE */
} tx_state_t;

/* ================================================================== *
 *  INITIALISATION STATE (DALI addressing protocol)                    *
 *                                                                     *
 *  IEC 62386-102 §9.6: Control gear enters initialisation state       *
 *  upon receiving INITIALISE (sent twice within 100 ms). In this      *
 *  state, it responds to COMPARE, WITHDRAW, PROGRAM SHORT, etc.      *
 *  WITHDRAWN means the device no longer responds to COMPARE but       *
 *  has not fully left initialisation (can still be re-addressed).     *
 * ================================================================== */
typedef enum {
    INIT_DISABLED,  /* Not in initialisation — ignore addressing cmds */
    INIT_ENABLED,   /* In initialisation — respond to COMPARE etc. */
    INIT_WITHDRAWN  /* Withdrawn — ignore COMPARE, keep init context */
} init_state_t;

/* ================================================================== *
 *  STATE VARIABLES                                                    *
 *                                                                     *
 *  All marked volatile because they are shared between ISR and main   *
 *  loop contexts. On RISC-V CH32V003, single-byte/word reads and     *
 *  writes are atomic, so no additional synchronisation is needed.     *
 * ================================================================== */

/* ── RX state ────────────────────────────────────────────────────── */
static volatile rx_state_t  rx_state = RX_IDLE;
static volatile uint32_t    rx_last_edge_ms = 0;    /* millis() at last valid RX edge */
static volatile uint16_t    rx_last_capture = 0;    /* TIM2 count at last edge */
static volatile uint8_t     rx_last_bus_low = 0;    /* Bus level after prev edge */
static volatile uint8_t     rx_len = 0;             /* Half-bit count received */
static volatile uint8_t     rx_msg[3] = {0};        /* Decoded bytes (up to 24 bits) */
static volatile uint8_t     rx_frame_ready = 0;     /* Flag: frame ready for main loop */

/* ── TX state ────────────────────────────────────────────────────── */
static volatile tx_state_t  tx_state = TX_IDLE;
static volatile uint8_t     tx_msg = 0;             /* Byte being transmitted */
static volatile uint8_t     tx_pos = 0;             /* Current bit position (0–7, MSB first) */
static volatile uint8_t     bus_idle_te_cnt = 0;    /* Settle time counter */

/* ── Protocol state ──────────────────────────────────────────────── */
static volatile uint8_t     actual_level = 0;       /* Current arc power level (0–254) */
static dali_arc_callback_t  arc_callback = 0;       /* User callback for level changes */
static dali_colour_callback_t colour_callback = 0;  /* User callback for DT8 colour changes */
static volatile uint8_t     dtr0 = 0;               /* Data Transfer Register 0 */
static volatile uint8_t     dtr1 = 0;               /* Data Transfer Register 1 */
static volatile uint8_t     dtr2 = 0;               /* Data Transfer Register 2 */
static volatile uint8_t     enabled_device_type = 0xFF; /* ENABLE_DT state, consumed on next cmd */

/* ── DT8 colour state (IEC 62386-209) ───────────────────────────── *
 * colour_temp[]: staging area set by SET_TEMP_RGB/WAF/COLOUR_TEMP.
 * colour_actual[]: active values committed by ACTIVATE.
 * Channel order: [0]=R, [1]=G, [2]=B, [3]=W.
 * Default 254 = DT6-compatible (all channels equal, full output).
 * colour_tc: colour temperature in mirek (0 = not set / using RGB).
 * ──────────────────────────────────────────────────────────────────*/
static volatile uint8_t     colour_temp[4]   = {254, 254, 254, 254};
static volatile uint8_t     colour_actual[4] = {254, 254, 254, 254};
static volatile uint16_t    colour_tc = 0;          /* Mirek, 0 = not set */

/* ── Operating parameters (IEC 62386-102 Table 22) ─────────────── */
static volatile uint8_t     max_level = 254;          /* Maximum allowed arc level */
static volatile uint8_t     min_level = 1;            /* Minimum allowed arc level (>0) */
static volatile uint8_t     power_on_level = 254;     /* Level applied at power-on */
static volatile uint8_t     sys_fail_level = 254;     /* Level on system failure */
static volatile uint8_t     scene_level[16] = {       /* Scene 0–15 levels (0xFF=MASK=not in scene) */
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};
static volatile uint16_t    group_membership = 0;     /* Bit mask: bit N = member of group N */

/* ── Fade engine state ──────────────────────────────────────────── *
 * IEC 62386-102 §9.5: fadeTime controls transition duration for arc
 * power commands; fadeRate controls step speed for UP/DOWN commands.
 * ──────────────────────────────────────────────────────────────────*/
static volatile uint8_t     fade_time = 0;           /* 0–15, 0 = instant (default) */
static volatile uint8_t     fade_rate = 7;           /* 1–15, default 7 (IEC 62386-102) */
static volatile uint8_t     ext_fade_base = 0;       /* DALI-2: 0–16 (lower 4 bits of DTR0) */
static volatile uint8_t     ext_fade_mult = 0;       /* DALI-2: 0–4 (bits 6:4 of DTR0) */
static volatile uint8_t     target_level = 0;        /* Fade target level */
static volatile uint8_t     fade_running = 0;        /* 1 = fade in progress */
static volatile uint16_t    fade_ms_per_step = 0;    /* Milliseconds between level steps */
static volatile uint32_t    last_step_ms = 0;         /* millis() timestamp of last step */

/* ── Status flags (IEC 62386-102 §11.2.1) ───────────────────────── */
static volatile uint8_t     reset_state = 1;         /* 1 after boot/RESET, cleared on config change */
static volatile uint8_t     power_cycle_seen = 1;    /* 1 after power-on, cleared by RESET */

/* ── Addressing state ────────────────────────────────────────────── */
static volatile uint8_t     short_address = 0xFF;   /* 0xFF = unassigned (MASK) */
static volatile uint8_t     random_h = 0, random_m = 0, random_l = 0;  /* 24-bit random address */
static volatile uint8_t     search_h = 0xFF, search_m = 0xFF, search_l = 0xFF; /* Search target */
static volatile init_state_t init_state = INIT_DISABLED;
static volatile uint32_t    init_start_time = 0;    /* millis() when INITIALISE accepted */

/* ── Config repeat validation ────────────────────────────────────── *
 * IEC 62386-102: INITIALISE and RANDOMISE must be sent twice within
 * 100 ms to be accepted. We store the first command and validate
 * the second matches within the time window.
 * ──────────────────────────────────────────────────────────────────*/
static volatile uint8_t     last_addr_byte = 0;
static volatile uint8_t     last_command = 0;
static volatile uint32_t    last_command_time = 0;
static volatile uint8_t     config_repeat_pending = 0;

/* ================================================================== *
 *  TX PIN HELPERS — polarity depends on DALI_NO_PHY                   *
 *                                                                     *
 *  Direct register access for minimum latency in ISR context.         *
 *  BSHR (Bit Set High Register) sets pin HIGH atomically.             *
 *  BCR  (Bit Clear Register) sets pin LOW atomically.                 *
 *  These are single-cycle operations on CH32V003.                     *
 *                                                                     *
 *  NO_PHY:   bus active = GPIO LOW,  bus idle = GPIO HIGH             *
 *  With PHY: bus active = GPIO HIGH, bus idle = GPIO LOW              *
 * ================================================================== */
#ifdef DALI_NO_PHY
/* Direct GPIO: LOW = active (mark), HIGH = idle (space) */
static inline void tx_bus_active(void) {
    DALI_TX_PORT->BCR = (1 << DALI_TX_PIN_N);   /* LOW = bus active */
}
static inline void tx_bus_idle(void) {
    DALI_TX_PORT->BSHR = (1 << DALI_TX_PIN_N);  /* HIGH = bus idle */
}
static inline uint8_t rx_bus_is_active(void) {
    return !(DALI_RX_PORT->INDR & (1 << DALI_RX_PIN_N));  /* LOW = active */
}
#else
/* With PHY transceiver: HIGH = active (mark), LOW = idle (space) */
static inline void tx_bus_active(void) {
    DALI_TX_PORT->BSHR = (1 << DALI_TX_PIN_N);  /* HIGH = bus active */
}
static inline void tx_bus_idle(void) {
    DALI_TX_PORT->BCR = (1 << DALI_TX_PIN_N);   /* LOW = bus idle */
}
static inline uint8_t rx_bus_is_active(void) {
    return !!(DALI_RX_PORT->INDR & (1 << DALI_RX_PIN_N)); /* HIGH = active */
}
#endif

/* ================================================================== *
 *  push_halfbit() — RX half-bit accumulator                           *
 *                                                                     *
 *  Manchester encoding: each data bit is two half-bits.               *
 *  The FIRST half-bit of each pair directly encodes the bit value:    *
 *    - bus_low=1 (active) → first half of bit 1                      *
 *    - bus_low=0 (idle)   → first half of bit 0                      *
 *  We only store even half-bits (rx_len & 1 == 0) which are always   *
 *  the first half of a Manchester bit pair.                           *
 *                                                                     *
 *  Byte packing: rx_len >> 4 selects which byte (0, 1, or 2).        *
 *  Each byte accumulates 8 bits from 16 half-bits.                    *
 *  For a 16-bit forward frame: byte 0 = address, byte 1 = data.      *
 * ================================================================== */
static void push_halfbit(uint8_t bit) {
    bit &= 1;
    if ((rx_len & 1) == 0) {            /* Even half-bit → decode it */
        uint8_t i = rx_len >> 4;        /* Byte index: 0–15→0, 16–31→1, 32–47→2 */
        if (i < 3) {
            rx_msg[i] = (rx_msg[i] << 1) | bit;  /* Shift in MSB-first */
        }
    }
    rx_len++;
}

/* ================================================================== *
 *  ISR: EXTI edge on PC0 (DALI RX)                                   *
 *                                                                     *
 *  Called on every rising and falling edge of the DALI RX line.       *
 *  Timestamps the edge using TIM2->CNT (1 µs resolution) and         *
 *  classifies the interval since the last edge as Te, 2Te, or noise. *
 *                                                                     *
 *  Key design decisions:                                              *
 *  - Ignores edges during TX (tx_state != TX_IDLE) to prevent         *
 *    decoding own backward frame reflections                          *
 *  - Noise filter: edges < 200 µs apart are discarded                *
 *  - Resets idle timeout on each valid edge (TIM2 CH4 OC)             *
 * ================================================================== */
void dali_isr_rx_edge(void) {
    /* Don't process edges while we're transmitting a backward frame */
    if (tx_state != TX_IDLE) return;

    uint16_t capture = (uint16_t)(TIM2->CNT);         /* Timestamp this edge */
    uint8_t bus_low = rx_bus_is_active();              /* 1=active (mark), 0=idle (space) */
    uint16_t dt = capture - rx_last_capture;           /* Interval since last edge (µs) */

    /* Noise filter: ignore glitches shorter than ~half a Te */
    if (dt < 200) return;

    rx_last_edge_ms = millis();   /* Track last valid edge time for WFI bus-idle guard */

    /* Reset idle timeout: if no edge within 5 Te, frame is complete.
       We program TIM2 CH4 output compare to fire at capture + 5*Te. */
    TIM2->CH4CVR = capture + DALI_IDLE_TIMEOUT;
    TIM2->INTFR = ~TIM_IT_CC4;         /* Clear any pending CH4 flag */
    TIM2->DMAINTENR |= TIM_IT_CC4;     /* Enable CH4 interrupt */

    switch (rx_state) {
    case RX_IDLE:
        /* Waiting for start bit. Start bit begins with bus going active (LOW).
           A falling edge here transitions us to RX_START. */
        if (bus_low) {
            rx_last_capture = capture;
            rx_state = RX_START;
        }
        break;

    case RX_START:
        /* We saw the falling edge of the start bit. Now we expect:
           - A rising edge after Te → valid start bit (LOW for Te, then HIGH)
           - Another falling edge → bus glitch during start, re-anchor
           - Anything else → abort, return to idle */
        if (!bus_low && DALI_IS_TE(dt)) {
            /* Valid start bit: Te of LOW followed by rising edge.
               Start bit is always '1' (active→idle). Ready to decode data. */
            rx_last_capture = capture;
            rx_len = 0;
            rx_msg[0] = 0;
            rx_msg[1] = 0;
            rx_msg[2] = 0;
            rx_state = RX_BIT;
            rx_last_bus_low = 0;    /* Bus is now HIGH after start bit */
        } else if (bus_low) {
            /* Another falling edge during start — could be a glitch
               or the bus briefly went high then low again. Re-anchor. */
            rx_last_capture = capture;
        } else {
            /* Rising edge but wrong timing — not a valid start bit */
            rx_state = RX_IDLE;
        }
        break;

    case RX_BIT:
        /* Decoding data bits via Manchester edge timing.
           Each Manchester bit consists of two half-bit periods:
           - Te interval:  one half-bit boundary (single transition)
           - 2Te interval: two half-bit boundaries (level unchanged across
                           a bit boundary, then transitions) */
        if (DALI_IS_TE(dt)) {
            /* Single half-bit: push the current bus level */
            rx_last_capture = capture;
            push_halfbit(bus_low);
        } else if (DALI_IS_2TE(dt)) {
            /* Double half-bit: the bus stayed at the same level across
               a bit boundary. Push the previous level (for the missed
               half-bit at the boundary) then the current level. */
            rx_last_capture = capture;
            push_halfbit(rx_last_bus_low);  /* Boundary half-bit */
            push_halfbit(bus_low);          /* Current half-bit */
        } else {
            /* Interval doesn't match Te or 2Te — frame corrupted or
               we've reached the stop condition. Reset to idle. */
            rx_state = RX_IDLE;
        }
        rx_last_bus_low = bus_low;  /* Remember level for 2Te detection */
        break;
    }
}

/* ================================================================== *
 *  ISR: TIM2 CH2 output compare — TX Te tick                         *
 *                                                                     *
 *  Called every Te (417 µs) while a backward frame is being sent.     *
 *  Steps through the TX state machine, toggling PC5 to generate      *
 *  the Manchester-encoded waveform.                                   *
 *                                                                     *
 *  The first thing each invocation does is schedule the next tick     *
 *  by adding DALI_TE_TICKS to CH2CVR. This ensures precise Te        *
 *  timing regardless of ISR entry latency.                            *
 * ================================================================== */
void dali_isr_tx_tick(void) {
    TIM2->CH2CVR += DALI_TE_TICKS;     /* Schedule next tick (Te later) */

    switch (tx_state) {
    case TX_IDLE:
        /* Shouldn't get here, but stop the interrupt just in case */
        TIM2->DMAINTENR &= ~TIM_IT_CC2;
        return;

    case TX_SETTLE:
        /* Wait for bus settle time before starting the backward frame.
           Count Te ticks until DALI_SETTLE_TE (10) is reached. */
        bus_idle_te_cnt++;
        if (bus_idle_te_cnt >= DALI_SETTLE_TE) {
            tx_state = TX_START_LO;     /* Next tick: start bit begins */
        }
        break;

    case TX_START_LO:
        /* Start bit, first half: bus goes active (LOW).
           Manchester bit 1 = active→idle, start bit is always 1. */
        tx_bus_active();
        tx_state = TX_START_HI;
        break;

    case TX_START_HI:
        /* Start bit, second half: bus goes idle (HIGH).
           Prepare to send data bits starting from MSB (bit 7). */
        tx_bus_idle();
        tx_pos = 0;
        tx_state = TX_BIT_1ST;
        break;

    case TX_BIT_1ST:
        /* Data bit, first half. If all 8 bits sent, go to stop bits. */
        if (tx_pos >= 8) {
            tx_bus_idle();              /* Bus idle for stop condition */
            tx_state = TX_STOP1;
            break;
        }
        /* Manchester encoding:
           Bit 1: first half = active (mark), second half = idle (space)
           Bit 0: first half = idle (space), second half = active (mark) */
        if (tx_msg & (1 << (7 - tx_pos))) {
            tx_bus_active();               /* Bit 1: active first */
        } else {
            tx_bus_idle();              /* Bit 0: idle first */
        }
        tx_state = TX_BIT_2ND;
        break;

    case TX_BIT_2ND:
        /* Data bit, second half — complement of first half */
        if (tx_msg & (1 << (7 - tx_pos))) {
            tx_bus_idle();              /* Bit 1: idle second */
        } else {
            tx_bus_active();               /* Bit 0: active second */
        }
        tx_pos++;                       /* Advance to next bit */
        tx_state = TX_BIT_1ST;
        break;

    /* Stop condition: 2 stop bits = 4 Te of bus idle (HIGH).
       Each stop bit is 2 Te. We use 4 states for clarity. */
    case TX_STOP1:
        tx_bus_idle();
        tx_state = TX_STOP2;
        break;

    case TX_STOP2:
        tx_bus_idle();
        tx_state = TX_STOP3;
        break;

    case TX_STOP3:
        tx_bus_idle();
        tx_state = TX_STOP4;
        break;

    case TX_STOP4:
        /* Last stop bit half — frame complete, return to idle */
        tx_bus_idle();
        tx_state = TX_IDLE;
        TIM2->DMAINTENR &= ~TIM_IT_CC2; /* Disable TX tick interrupt */
        break;
    }
}

/* ================================================================== *
 *  ISR: TIM2 CH4 output compare — idle timeout (frame complete)       *
 *                                                                     *
 *  Fires when no EXTI edge has arrived within DALI_IDLE_TIMEOUT       *
 *  (5 Te = 2085 µs) after the last edge. This means the bus has      *
 *  been idle long enough for the stop condition, so the frame is      *
 *  complete and ready for processing.                                 *
 * ================================================================== */
void dali_isr_idle_timeout(void) {
    if (rx_state == RX_BIT) {
        /* We were decoding data bits — frame is complete */
        rx_frame_ready = 1;
    }
    rx_state = RX_IDLE;
    TIM2->DMAINTENR &= ~TIM_IT_CC4;    /* Disable idle timeout until next frame */
}

/* ================================================================== *
 *  send_backward_frame() — initiate 8-bit Manchester response         *
 *                                                                     *
 *  Starts the TX state machine which will generate the backward       *
 *  frame waveform via TIM2 CH2 output compare ISR.                    *
 *                                                                     *
 *  The settle time (DALI_SETTLE_TE × Te) ensures the backward frame   *
 *  starts within the IEC 62386-101 timing window of 7–22 Te after     *
 *  the forward frame ends. See dali_physical.h for the calculation.   *
 * ================================================================== */
static void send_backward_frame(uint8_t data) {
    if (tx_state != TX_IDLE) return;    /* Already transmitting — shouldn't happen */

    tx_msg = data;                      /* Byte to send */
    tx_pos = 0;                         /* Start from MSB */
    bus_idle_te_cnt = 0;                /* Reset settle counter */
    tx_state = TX_SETTLE;               /* Begin settle phase */

    /* Schedule first TX tick at current time + Te */
    TIM2->CH2CVR = TIM2->CNT + DALI_TE_TICKS;
    TIM2->INTFR = ~TIM_IT_CC2;         /* Clear any pending CH2 flag */
    TIM2->DMAINTENR |= TIM_IT_CC2;     /* Enable TX tick interrupt */
}

/* ================================================================== *
 *  is_addressed_to_me() — check if forward frame targets this device  *
 *                                                                     *
 *  DALI address byte format (IEC 62386-102 §9.2):                    *
 *    0AAAAAA S — Short address (0–63), S=0 arc, S=1 command          *
 *    100GGGG S — Group address (0–15), not implemented                *
 *    1111111 S — Broadcast, always matches                            *
 *    101CCCC 1 — Special command (handled separately, not here)       *
 *    110CCCC 1 — Special command (handled separately, not here)       *
 * ================================================================== */
static uint8_t is_addressed_to_me(uint8_t addr_byte) {
    uint8_t Y = (addr_byte >> 7) & 1;  /* Top bit: 0=short addr, 1=group/broadcast */

    if (Y == 0) {
        /* Short address: 0AAAAAA S — extract 6-bit address */
        uint8_t addr = (addr_byte >> 1) & 0x3F;
        return (addr == short_address);
    }

    /* Broadcast: 1111111 S (0xFE for arc, 0xFF for command) */
    if ((addr_byte & 0xFE) == 0xFE) return 1;

    /* Group address: 100GGGG S — extract 4-bit group, check membership */
    if ((addr_byte & 0xE0) == 0x80) {
        uint8_t group = (addr_byte >> 1) & 0x0F;
        return (group_membership & (1 << group)) ? 1 : 0;
    }

    return 0;
}

/* ================================================================== *
 *  clamp_level() — enforce min/max level constraints                   *
 *                                                                     *
 *  IEC 62386-102 §9.4: Arc power levels must be clamped to the range  *
 *  [minLevel, maxLevel], except level 0 (OFF) which is always valid.  *
 * ================================================================== */
static uint8_t clamp_level(uint8_t level) {
    if (level == 0) return 0;
    if (level < min_level) return min_level;
    if (level > max_level) return max_level;
    return level;
}

/* ================================================================== *
 *  get_ext_fade_time_ms() — compute DALI-2 extended fade time         *
 *                                                                     *
 *  Extended fade time = base × multiplier, where:                     *
 *    mult 0 = disabled, 1 = ×100ms, 2 = ×1s, 3 = ×10s, 4 = ×1min   *
 *  Returns 0 if disabled (caller should fall through to instant).     *
 * ================================================================== */
static uint32_t get_ext_fade_time_ms(void) {
    static const uint16_t mult_ms[5] = { 0, 100, 1000, 10000, 60000 };
    if (ext_fade_mult == 0 || ext_fade_mult > 4 || ext_fade_base == 0) return 0;
    return (uint32_t)ext_fade_base * mult_ms[ext_fade_mult];
}

/* ================================================================== *
 *  check_config_repeat() — validate 2× within 100 ms requirement     *
 *                                                                     *
 *  IEC 62386-102: Configuration commands (32–128), INITIALISE, and    *
 *  RANDOMISE must be received twice within 100 ms to be accepted.     *
 *  Returns 1 on valid second reception, 0 on first (stores state).    *
 * ================================================================== */
static uint8_t check_config_repeat(uint8_t addr, uint8_t cmd, uint32_t now) {
    if (config_repeat_pending && last_addr_byte == addr
        && last_command == cmd
        && (now - last_command_time) <= 100) {
        config_repeat_pending = 0;
        return 1;
    }
    last_addr_byte = addr;
    last_command = cmd;
    last_command_time = now;
    config_repeat_pending = 1;
    return 0;
}

/* ================================================================== *
 *  process_query_command() — handle query commands (cmd 144–196)       *
 *                                                                     *
 *  Query commands require a backward frame response.                  *
 *  IEC 62386-102 §11: Only respond if addressed to this device.       *
 * ================================================================== */
static void process_query_command(uint8_t cmd) {
    switch (cmd) {
    case DALI_CMD_QUERY_STATUS:
        /* IEC 62386-102 §11.2.1: Status byte bits:
           Bit 0: controlGearFailure  (no HW monitoring, always 0)
           Bit 1: lampFailure         (no HW monitoring, always 0)
           Bit 2: lampOn              (1 if actualLevel > 0)
           Bit 3: limitError          (not tracked, always 0)
           Bit 4: fadeRunning         (1 if fade is in progress)
           Bit 5: resetState          (1 after RESET, cleared on config change)
           Bit 6: missingShortAddress (1 if no short address assigned)
           Bit 7: powerCycleSeen      (1 after power-on, cleared by RESET) */
        send_backward_frame(
            (actual_level > 0 ? 0x04 : 0x00) |
            (fade_running ? 0x10 : 0x00) |
            (reset_state ? 0x20 : 0x00) |
            (short_address == 0xFF ? 0x40 : 0x00) |
            (power_cycle_seen ? 0x80 : 0x00));
        break;

    case DALI_CMD_QUERY_GEAR_PRESENT:
        /* IEC 62386-102 §11.3.2: Always respond with YES */
        send_backward_frame(0xFF);
        break;

    case DALI_CMD_QUERY_LAMP_FAILURE:
        /* IEC 62386-102 §11.3.3: YES if lamp failure.
           No hardware lamp monitoring → never respond (no failure). */
        break;

    case DALI_CMD_QUERY_LAMP_POWER_ON:
        /* IEC 62386-102 §11.3.4: YES if lamp is on */
        if (actual_level > 0) send_backward_frame(0xFF);
        break;

    case DALI_CMD_QUERY_LIMIT_ERROR:
        /* IEC 62386-102 §11.3.5: YES if last level was clamped.
           We always achieve requested levels → never respond. */
        break;

    case DALI_CMD_QUERY_RESET_STATE:
        /* IEC 62386-102 §11.3.6: YES if all vars at reset defaults */
        if (reset_state) send_backward_frame(0xFF);
        break;

    case DALI_CMD_QUERY_MISSING_SHORT:
        /* IEC 62386-102 §11.3.3: Respond YES (0xFF) if short address
           is undefined. If address IS assigned: NO RESPONSE (do not
           send any backward frame — silence on the bus). */
        if (short_address == 0xFF) {
            send_backward_frame(0xFF);
        }
        /* No else — intentionally no response when address is assigned */
        break;

    case DALI_CMD_QUERY_ACTUAL_LEVEL:
        /* IEC 62386-102 §11.4.1: Return current actual level */
        send_backward_frame(actual_level);
        break;

    case DALI_CMD_QUERY_MAX_LEVEL:
        send_backward_frame(max_level);
        break;

    case DALI_CMD_QUERY_MIN_LEVEL:
        send_backward_frame(min_level);
        break;

    case DALI_CMD_QUERY_POWER_ON:
        send_backward_frame(power_on_level);
        break;

    case DALI_CMD_QUERY_SYS_FAIL:
        send_backward_frame(sys_fail_level);
        break;

    case DALI_CMD_QUERY_DTR1:
        send_backward_frame(dtr1);
        break;

    case DALI_CMD_QUERY_DTR2:
        send_backward_frame(dtr2);
        break;

    case DALI_CMD_QUERY_GROUPS_0_7:
        send_backward_frame(group_membership & 0xFF);
        break;

    case DALI_CMD_QUERY_GROUPS_8_15:
        send_backward_frame((group_membership >> 8) & 0xFF);
        break;

    case DALI_CMD_QUERY_RANDOM_H:
        send_backward_frame(random_h);
        break;

    case DALI_CMD_QUERY_RANDOM_M:
        send_backward_frame(random_m);
        break;

    case DALI_CMD_QUERY_RANDOM_L:
        send_backward_frame(random_l);
        break;

    case DALI_CMD_QUERY_VERSION:
        /* IEC 62386-102 §11.3.7: Version number. Return 1 (DALI-1). */
        send_backward_frame(1);
        break;

    case DALI_CMD_QUERY_DTR0:
        /* IEC 62386-102 §11.3.8: Return current DTR0 content */
        send_backward_frame(dtr0);
        break;

    case DALI_CMD_QUERY_DEVICE_TYPE:
        /* IEC 62386-102 §11.3.9: Return device type from hardware.h */
        send_backward_frame(DALI_DEVICE_TYPE);
        break;

    case DALI_CMD_QUERY_PHYS_MIN:
        /* IEC 62386-102 §11.3.10: Physical minimum level.
           Level 1 is the lowest settable non-zero level. */
        send_backward_frame(1);
        break;

    case DALI_CMD_QUERY_FADE_SPEEDS:
        /* IEC 62386-102 §11.4.6: Return (fadeTime << 4) | fadeRate */
        send_backward_frame((fade_time << 4) | fade_rate);
        break;

    default:
        /* QUERY SCENE LEVEL (176–191): Return level for scene N */
        if (cmd >= DALI_CMD_QUERY_SCENE_BASE && cmd <= DALI_CMD_QUERY_SCENE_BASE + 15) {
            send_backward_frame(scene_level[cmd - DALI_CMD_QUERY_SCENE_BASE]);
        }
        /* Other unimplemented queries — no response (bus stays idle) */
        break;
    }
}

/* ================================================================== *
 *  process_special_command() — handle addressing/config commands       *
 *                                                                     *
 *  Special commands use address bytes 101CCCC1 (0xA1–0xBF) and        *
 *  110CCCC1 (0xC1–0xDF). They are NOT addressed to individual         *
 *  devices — all control gear must process them.                      *
 *                                                                     *
 *  INITIALISE and RANDOMISE require "config repeat" — the command     *
 *  must be received twice within 100 ms to be accepted.               *
 * ================================================================== */
static void process_special_command(uint8_t addr_byte, uint8_t data_byte) {
    uint32_t now = millis();

    switch (addr_byte) {
    case DALI_SPECIAL_TERMINATE:
        /* IEC 62386-102 §9.6.2: Leave initialisation state immediately */
        init_state = INIT_DISABLED;
        printf("TERM\n");
        break;

    case DALI_SPECIAL_DTR:
        /* IEC 62386-102 §9.6.5: Set DTR0 to data_byte.
           Used by STORE DTR AS FADE TIME (cmd 46) and FADE RATE (cmd 47). */
        dtr0 = data_byte;
        break;

    case DALI_SPECIAL_INITIALISE:
        /* IEC 62386-102 §9.6.3: Enter initialisation state.
           Must be sent twice within 100 ms (config repeat).
           data_byte selects which devices respond:
             0xFF:     all devices
             0x00:     devices without a short address
             AAAAAA1:  device with short address AAAAAA */
        if (check_config_repeat(addr_byte, data_byte, now)) {
            uint8_t addressed = 0;
            if (data_byte == 0xFF) {
                addressed = 1;                          /* All devices */
            } else if (data_byte == 0x00) {
                addressed = (short_address == 0xFF);    /* Unaddressed only */
            } else if (data_byte & 1) {
                addressed = (((data_byte >> 1) & 0x3F) == short_address);
            }
            if (addressed) {
                init_state = INIT_ENABLED;
                init_start_time = now;      /* Start 15-minute timeout */
                printf("INIT ok\n");
            }
        }
        break;

    case DALI_SPECIAL_RANDOMISE:
        /* IEC 62386-102 §9.6.4: Generate new 24-bit random address.
           Only accepted in initialisation state, with config repeat. */
        if (init_state != INIT_ENABLED) break;
        if (check_config_repeat(addr_byte, data_byte, now)) {
            uint32_t seed = SysTick->CNT;
            seed ^= (uint32_t)short_address << 16;
            seed ^= (uint32_t)actual_level << 8;
            seed *= 1103515245UL;           /* LCG multiplier (glibc) */
            seed += 12345;                  /* LCG increment */
            random_h = (seed >> 16) & 0xFF;
            random_m = (seed >> 8) & 0xFF;
            random_l = seed & 0xFF;
            printf("RAND=%02X%02X%02X\n", random_h, random_m, random_l);
        }
        break;

    case DALI_SPECIAL_COMPARE:
        /* IEC 62386-102 §9.6.9: If randomAddress ≤ searchAddress,
           respond with YES (0xFF). Used by master's binary search
           to find the exact random address of each device. */
        if (init_state != INIT_ENABLED) break;
        {
            uint32_t random = ((uint32_t)random_h << 16) | ((uint32_t)random_m << 8) | random_l;
            uint32_t search = ((uint32_t)search_h << 16) | ((uint32_t)search_m << 8) | search_l;
            if (random <= search) {
                send_backward_frame(0xFF);  /* YES */
            }
            /* No response if random > search */
        }
        break;

    case DALI_SPECIAL_WITHDRAW:
        /* IEC 62386-102 §9.6.10: If randomAddress == searchAddress,
           stop responding to COMPARE (but remain in init context).
           This allows the master to address the next device. */
        if (init_state != INIT_ENABLED) break;
        {
            uint32_t random = ((uint32_t)random_h << 16) | ((uint32_t)random_m << 8) | random_l;
            uint32_t search = ((uint32_t)search_h << 16) | ((uint32_t)search_m << 8) | search_l;
            if (random == search) {
                init_state = INIT_WITHDRAWN;
                printf("WITHDRAW\n");
            }
        }
        break;

    case DALI_SPECIAL_SEARCHADDRH:
        /* IEC 62386-102 §9.6.6: Set search address high byte.
           Accepted unconditionally (no init state required). */
        search_h = data_byte;
        break;

    case DALI_SPECIAL_SEARCHADDRM:
        /* IEC 62386-102 §9.6.7: Set search address mid byte */
        search_m = data_byte;
        break;

    case DALI_SPECIAL_SEARCHADDRL:
        /* IEC 62386-102 §9.6.8: Set search address low byte */
        search_l = data_byte;
        break;

    case DALI_SPECIAL_PROGRAM_SHORT:
        /* IEC 62386-102 §9.6.14: Assign short address to this device.
           Only accepted in INIT_ENABLED state when randomAddr == searchAddr.
           data_byte = (addr << 1) | 1  for address 0–63
           data_byte = 0xFF             to delete (unassign) the address */
        if (init_state != INIT_ENABLED) break;
        {
            uint32_t random = ((uint32_t)random_h << 16) | ((uint32_t)random_m << 8) | random_l;
            uint32_t search = ((uint32_t)search_h << 16) | ((uint32_t)search_m << 8) | search_l;
            if (random == search) {
                if (data_byte == 0xFF) {
                    short_address = 0xFF;   /* Delete short address */
                } else {
                    short_address = (data_byte >> 1) & 0x3F;  /* Store 6-bit addr */
                }
                nvm_mark_dirty();
                printf("PROG_SHORT=%d\n", short_address);
            } else {
                printf("PROG_SHORT: random!=search R=%06lX S=%06lX\n",
                       (unsigned long)random, (unsigned long)search);
            }
        }
        break;

    case DALI_SPECIAL_VERIFY_SHORT:
        /* IEC 62386-102 §9.6.15: If the given address matches our
           short address, respond with YES. Only in init state. */
        if (init_state != INIT_ENABLED) break;
        {
            uint8_t addr = (data_byte >> 1) & 0x3F;
            if (addr == short_address) {
                send_backward_frame(0xFF);
            }
        }
        break;

    case DALI_SPECIAL_QUERY_SHORT:
        /* IEC 62386-102 §9.6.16: If randomAddr == searchAddr, respond
           with the current short address (or 0xFF if unassigned). */
        if (init_state != INIT_ENABLED) break;
        {
            uint32_t random = ((uint32_t)random_h << 16) | ((uint32_t)random_m << 8) | random_l;
            uint32_t search = ((uint32_t)search_h << 16) | ((uint32_t)search_m << 8) | search_l;
            if (random == search) {
                if (short_address == 0xFF) {
                    send_backward_frame(0xFF);  /* No short address */
                } else {
                    send_backward_frame((short_address << 1) | 1);
                }
            }
        }
        break;

    case DALI_SPECIAL_DTR1:
        /* IEC 62386-102 §9.6.5: Set DTR1 (used by DT8 for colour values) */
        dtr1 = data_byte;
        break;

    case DALI_SPECIAL_DTR2:
        /* IEC 62386-102 §9.6.5: Set DTR2 (used by DT8 for colour values) */
        dtr2 = data_byte;
        break;

    case DALI_SPECIAL_ENABLE_DT:
        /* IEC 62386-102 §9.6.17: Enable device type for next command.
           Consumed (reset to 0xFF) when the next addressed command arrives. */
        enabled_device_type = data_byte;
        break;

    default:
        break;
    }
}

/* ================================================================== *
 *  tc_to_rgbw() — approximate colour temperature to RGBW conversion   *
 *                                                                     *
 *  Linear interpolation between two reference points:                 *
 *    Warm (2700K = 370 mirek): R=254, G=180, B=80,  W=254            *
 *    Cool (6500K = 154 mirek): R=160, G=210, B=254, W=200            *
 *  Integer math only, no floating point. ~50 bytes of code.           *
 * ================================================================== */
static void tc_to_rgbw(uint16_t mirek, uint8_t *rgbw) {
    if (mirek < 154) mirek = 154;   /* Clamp to cool limit (6500K) */
    if (mirek > 370) mirek = 370;   /* Clamp to warm limit (2700K) */
    uint16_t m = mirek - 154;       /* 0..216 */
    rgbw[0] = 160 + (uint16_t)(94  * m) / 216;   /* R: 160→254 */
    rgbw[1] = 210 - (uint16_t)(30  * m) / 216;   /* G: 210→180 */
    rgbw[2] = 254 - (uint16_t)(174 * m) / 216;   /* B: 254→80  */
    rgbw[3] = 200 + (uint16_t)(54  * m) / 216;   /* W: 200→254 */
}

/* ================================================================== *
 *  process_dt8_command() — handle DT8 extended commands (224–254)      *
 *                                                                     *
 *  Only called when enabled_device_type == 8 (DALI_DEVICE_TYPE).      *
 *  IEC 62386-209: RGBWAF primaries + colour temperature Tc.           *
 * ================================================================== */
static void process_dt8_command(uint8_t cmd) {
    switch (cmd) {
    case DALI_DT8_ACTIVATE:
        /* Commit staged colour values to active output */
        for (uint8_t i = 0; i < 4; i++)
            colour_actual[i] = colour_temp[i];
        if (colour_callback)
            colour_callback((const uint8_t *)colour_actual, PWM_NUM_CHANNELS);
        printf("DT8 ACT R=%d G=%d B=%d W=%d\n",
               colour_actual[0], colour_actual[1],
               colour_actual[2], colour_actual[3]);
        break;

    case DALI_DT8_SET_TEMP_RGB_LEVEL:
        /* Stage RGB levels from DTR2/DTR1/DTR0 */
        colour_temp[0] = dtr2;  /* R */
        colour_temp[1] = dtr1;  /* G */
        colour_temp[2] = dtr0;  /* B */
        colour_tc = 0;          /* Clear Tc mode — using direct RGB */
        break;

    case DALI_DT8_SET_TEMP_WAF_LEVEL:
        /* Stage W level from DTR2 (A and F ignored for 4-channel) */
        colour_temp[3] = dtr2;  /* W */
        break;

    case DALI_DT8_SET_TEMP_COLOUR_TEMP:
        /* Set colour temperature from DTR1:DTR0 (mirek) */
        colour_tc = ((uint16_t)dtr1 << 8) | dtr0;
        tc_to_rgbw(colour_tc, (uint8_t *)colour_temp);
        break;

    case DALI_DT8_STEP_COOLER:
        /* Decrease mirek (= increase Kelvin = cooler white) */
        if (colour_tc > 154 + DALI_DT8_TC_STEP_MIREK)
            colour_tc -= DALI_DT8_TC_STEP_MIREK;
        else
            colour_tc = 154;
        tc_to_rgbw(colour_tc, (uint8_t *)colour_temp);
        break;

    case DALI_DT8_STEP_WARMER:
        /* Increase mirek (= decrease Kelvin = warmer white) */
        if (colour_tc < 370 - DALI_DT8_TC_STEP_MIREK)
            colour_tc += DALI_DT8_TC_STEP_MIREK;
        else
            colour_tc = 370;
        tc_to_rgbw(colour_tc, (uint8_t *)colour_temp);
        break;

    case DALI_DT8_COPY_REPORT_TO_TEMP:
        /* Copy active colour values back to staging area */
        for (uint8_t i = 0; i < 4; i++)
            colour_temp[i] = colour_actual[i];
        break;

    /* DT8 query commands */
    case DALI_DT8_QUERY_GEAR_FEATURES:
        /* No special features (no auto-calibration, etc.) */
        send_backward_frame(0x00);
        break;

    case DALI_DT8_QUERY_COLOUR_STATUS:
        /* Bit 0: colour type Tc active, Bit 2: RGBWAF active */
        send_backward_frame(
            (colour_tc > 0 ? 0x01 : 0x00) |
            (colour_tc == 0 ? 0x04 : 0x00));
        break;

    case DALI_DT8_QUERY_COLOUR_TYPE_FEATURES:
        /* Supported colour types: Tc (bit 1) + RGBWAF primary (bit 2) */
        send_backward_frame(DALI_DT8_COLOUR_TYPE_TC | DALI_DT8_COLOUR_TYPE_PRIMARY);
        break;

    case DALI_DT8_QUERY_COLOUR_VALUE:
        /* Response depends on DTR0:
           DTR0=64 → number of primaries (PWM_NUM_CHANNELS)
           DTR0=240..243 → RGBW channel actual levels */
        if (dtr0 == 64) {
            send_backward_frame(PWM_NUM_CHANNELS);
        } else if (dtr0 >= 240 && dtr0 <= 243) {
            send_backward_frame(colour_actual[dtr0 - 240]);
        } else {
            send_backward_frame(0xFF); /* Not applicable */
        }
        break;

    case DALI_DT8_QUERY_RGBWAF_CONTROL:
        /* Bitmask of active RGBWAF channels: R=bit0, G=bit1, B=bit2, W=bit3 */
        send_backward_frame((1 << PWM_NUM_CHANNELS) - 1);
        break;

    case DALI_DT8_QUERY_ASSIGNED_COLOUR:
        /* 0xFF = all channels / not specifically assigned */
        send_backward_frame(0xFF);
        break;

    default:
        /* Unimplemented DT8 command — silently ignored */
        break;
    }
}

/* ================================================================== *
 *  process_frame() — dispatch a received 16-bit forward frame         *
 *                                                                     *
 *  A forward frame consists of:                                       *
 *    addr_byte (8 bits): address + selector bit S                     *
 *    data_byte (8 bits): arc level (S=0) or command number (S=1)      *
 *                                                                     *
 *  Processing order:                                                  *
 *  1. Check if it's a special command (101CCCC1 or 110CCCC1 pattern)  *
 *  2. Check if addressed to this device (short addr or broadcast)     *
 *  3. If S=0: direct arc power command (set level)                    *
 *  4. If S=1: command dispatch (OFF, RECALL MAX/MIN, queries)         *
 * ================================================================== */
static void process_frame(uint8_t addr_byte, uint8_t data_byte) {
    /* Special command detection: mask out the variable bits and check
       if the address byte matches the 101xxxxx1 or 110xxxxx1 pattern.
       Mask 0xE1 extracts bits [7:5] and [0]:
         101CCCC1 & 0xE1 = 0xA1
         110CCCC1 & 0xE1 = 0xC1 */
    uint8_t top = addr_byte & 0xE1;
    if (top == 0xA1 || top == 0xC1) {
        process_special_command(addr_byte, data_byte);
        return;
    }

    /* Not a special command — check if addressed to this device */
    if (!is_addressed_to_me(addr_byte)) return;

    /* Consume ENABLE_DT state — it applies to the next addressed command
       only, then resets regardless of whether we used it. */
    uint8_t enabled_dt = enabled_device_type;
    enabled_device_type = 0xFF;

    uint8_t S = addr_byte & 1;  /* Selector bit: 0=arc power, 1=command */

    if (S == 0) {
        /* ── Direct arc power command ─────────────────────────────── *
         * data_byte = target level (0–254), 0xFF = MASK (no action).
         * IEC 62386-102 §9.4: If fadeTime > 0, fade to the target.
         * Level 0 = OFF (always instant), 254 = maximum.
         * Levels clamped to [minLevel, maxLevel] (0 always allowed).
         * Any new arc power command cancels a running fade.
         * ──────────────────────────────────────────────────────────── */
        if (data_byte == 0xFF) return;  /* MASK — do not change level */
        fade_running = 0;               /* Cancel any running fade */
        uint8_t level = clamp_level(data_byte);

        /* Determine effective fade duration: standard fade_time takes
           priority; if 0, fall back to extended fade time (DALI-2). */
        uint32_t eff_fade_ms = (fade_time > 0)
                             ? dali_fade_time_ms[fade_time]
                             : get_ext_fade_time_ms();

        if (level == 0 || eff_fade_ms == 0 || actual_level == level) {
            actual_level = level;
            if (arc_callback) arc_callback(level);
        } else {
            /* Start fade from actual_level to level over eff_fade_ms */
            target_level = level;
            uint16_t steps = (actual_level > level)
                           ? (actual_level - level)
                           : (level - actual_level);
            fade_ms_per_step = eff_fade_ms / steps;
            if (fade_ms_per_step < 1) fade_ms_per_step = 1;
            last_step_ms = millis();
            fade_running = 1;
        }
    } else {
        /* ── Command dispatch (S=1) ──────────────────────────────── *
         * data_byte = command number (IEC 62386-102 §9.5).
         * Commands 0–31:   immediate action
         * Commands 32–128: configuration (require config repeat)
         * Commands 144–255: query (require backward frame response)
         * ──────────────────────────────────────────────────────────── */
        uint32_t now = millis();

        switch (data_byte) {
        /* ── Immediate action commands (0–31) ────────────────────── */
        case DALI_CMD_OFF:
            /* Cmd 0: OFF — immediately set level to 0, stop any fade */
            fade_running = 0;
            actual_level = 0;
            if (arc_callback) arc_callback(0);
            break;

        case DALI_CMD_UP:
            /* Cmd 1: UP — fade up at fadeRate to maxLevel.
               If at 0, start from minLevel. If at maxLevel, ignore. */
            fade_running = 0;
            if (actual_level >= max_level) break;
            if (actual_level == 0) {
                actual_level = min_level;
                if (arc_callback) arc_callback(min_level);
            }
            target_level = max_level;
            fade_ms_per_step = dali_fade_rate_ms[fade_rate];
            if (fade_ms_per_step < 1) fade_ms_per_step = 1;
            last_step_ms = millis();
            fade_running = 1;
            break;

        case DALI_CMD_DOWN:
            /* Cmd 2: DOWN — fade down at fadeRate to minLevel.
               Stops at minLevel, never goes to 0 (IEC 62386-102 §9.5). */
            fade_running = 0;
            if (actual_level == 0) break;
            if (actual_level <= min_level) break;  /* Already at/below min */
            target_level = min_level;
            fade_ms_per_step = dali_fade_rate_ms[fade_rate];
            if (fade_ms_per_step < 1) fade_ms_per_step = 1;
            last_step_ms = millis();
            fade_running = 1;
            break;

        case DALI_CMD_STEP_UP:
            /* Cmd 3: STEP UP — increase level by 1, instant.
               If at 0, go to minLevel. Clamp to maxLevel. */
            fade_running = 0;
            if (actual_level == 0) actual_level = min_level;
            else if (actual_level < max_level) actual_level++;
            else break;  /* Already at maxLevel */
            if (arc_callback) arc_callback(actual_level);
            break;

        case DALI_CMD_STEP_DOWN:
            /* Cmd 4: STEP DOWN — decrease level by 1, instant.
               If at minLevel, go to 0 (OFF). If at 0, ignore. */
            fade_running = 0;
            if (actual_level == 0) break;
            if (actual_level <= min_level) {
                actual_level = 0;
            } else {
                actual_level--;
            }
            if (arc_callback) arc_callback(actual_level);
            break;

        case DALI_CMD_RECALL_MAX:
            /* Cmd 5: RECALL MAX LEVEL — instant to maxLevel */
            fade_running = 0;
            actual_level = max_level;
            if (arc_callback) arc_callback(max_level);
            break;

        case DALI_CMD_RECALL_MIN:
            /* Cmd 6: RECALL MIN LEVEL — instant to minLevel */
            fade_running = 0;
            actual_level = min_level;
            if (arc_callback) arc_callback(min_level);
            break;

        case DALI_CMD_STEP_DOWN_OFF:
            /* Cmd 7: STEP DOWN AND OFF — step down, go OFF at minLevel */
            fade_running = 0;
            if (actual_level == 0) break;
            if (actual_level <= min_level) {
                actual_level = 0;  /* At or below min → OFF */
            } else {
                actual_level--;
            }
            if (arc_callback) arc_callback(actual_level);
            break;

        case DALI_CMD_ON_STEP_UP:
            /* Cmd 8: ON AND STEP UP — if OFF go to minLevel, else step up */
            fade_running = 0;
            if (actual_level == 0) {
                actual_level = min_level;
            } else if (actual_level < max_level) {
                actual_level++;
            } else {
                break;  /* Already at maxLevel */
            }
            if (arc_callback) arc_callback(actual_level);
            break;

        /* ── Configuration commands (32–128, require config repeat) ── *
         * Must be received twice within 100 ms to be accepted.
         * Uses check_config_repeat() helper for validation.
         * ──────────────────────────────────────────────────────────── */
        case DALI_CMD_RESET:
            /* Cmd 32: Reset all variables to defaults (IEC 62386-102 Table 22).
               Does NOT clear short address. */
            if (check_config_repeat(addr_byte, data_byte, now)) {
                fade_running = 0;
                actual_level = 254;
                max_level = 254;
                min_level = 1;
                power_on_level = 254;
                sys_fail_level = 254;
                fade_time = 0;
                fade_rate = 7;
                ext_fade_base = 0;
                ext_fade_mult = 0;
                group_membership = 0;
                for (uint8_t i = 0; i < 16; i++) scene_level[i] = 0xFF;
                for (uint8_t i = 0; i < 4; i++) {
                    colour_temp[i] = 254;
                    colour_actual[i] = 254;
                }
                colour_tc = 0;
                reset_state = 1;
                power_cycle_seen = 0;
                if (arc_callback) arc_callback(actual_level);
                if (colour_callback)
                    colour_callback((const uint8_t *)colour_actual, PWM_NUM_CHANNELS);
                nvm_mark_dirty();
                printf("RESET\n");
            }
            break;

        case DALI_CMD_STORE_ACTUAL_DTR0:
            /* Cmd 33: Store actual level in DTR0 (config repeat) */
            if (check_config_repeat(addr_byte, data_byte, now)) {
                dtr0 = actual_level;
            }
            break;

        case DALI_CMD_DTR_AS_MAX_LEVEL:
            /* Cmd 42: Store DTR0 as maxLevel */
            if (check_config_repeat(addr_byte, data_byte, now)) {
                max_level = dtr0;
                if (max_level < min_level) max_level = min_level;
                nvm_mark_dirty();
                reset_state = 0;
                printf("MAX=%d\n", max_level);
            }
            break;

        case DALI_CMD_DTR_AS_MIN_LEVEL:
            /* Cmd 43: Store DTR0 as minLevel */
            if (check_config_repeat(addr_byte, data_byte, now)) {
                min_level = dtr0;
                if (min_level < 1) min_level = 1;  /* Physical minimum */
                if (min_level > max_level) min_level = max_level;
                nvm_mark_dirty();
                reset_state = 0;
                printf("MIN=%d\n", min_level);
            }
            break;

        case DALI_CMD_DTR_AS_POWER_ON:
            /* Cmd 44: Store DTR0 as powerOnLevel */
            if (check_config_repeat(addr_byte, data_byte, now)) {
                power_on_level = dtr0;
                nvm_mark_dirty();
                reset_state = 0;
                printf("PON=%d\n", power_on_level);
            }
            break;

        case DALI_CMD_DTR_AS_SYS_FAIL:
            /* Cmd 45: Store DTR0 as systemFailureLevel */
            if (check_config_repeat(addr_byte, data_byte, now)) {
                sys_fail_level = dtr0;
                nvm_mark_dirty();
                reset_state = 0;
                printf("SFAIL=%d\n", sys_fail_level);
            }
            break;

        case DALI_CMD_DTR_AS_FADE_TIME:
            /* Cmd 46: Store DTR0 as fadeTime (lower 4 bits, 0–15) */
            if (check_config_repeat(addr_byte, data_byte, now)) {
                fade_time = dtr0 & 0x0F;
                nvm_mark_dirty();
                reset_state = 0;
                printf("FADE_TIME=%d\n", fade_time);
            }
            break;

        case DALI_CMD_DTR_AS_FADE_RATE:
            /* Cmd 47: Store DTR0 as fadeRate (lower 4 bits, 1–15) */
            if (check_config_repeat(addr_byte, data_byte, now)) {
                uint8_t r = dtr0 & 0x0F;
                if (r > 0) fade_rate = r;  /* 0 is reserved, ignore */
                nvm_mark_dirty();
                reset_state = 0;
                printf("FADE_RATE=%d\n", fade_rate);
            }
            break;

        case DALI_CMD_DTR_AS_SHORT_ADDR:
            /* Cmd 48: Store DTR0 as short address (config repeat).
               DTR0 = (addr << 1) | 1 for addr 0–63, or 0xFF to delete. */
            if (check_config_repeat(addr_byte, data_byte, now)) {
                if (dtr0 == 0xFF) {
                    short_address = 0xFF;
                } else {
                    short_address = (dtr0 >> 1) & 0x3F;
                }
                nvm_mark_dirty();
                reset_state = 0;
                printf("SHORT_ADDR=%d\n", short_address);
            }
            break;

        case DALI_CMD_DTR_AS_EXT_FADE:
            /* Cmd 128: Store DTR0 as extended fade time (DALI-2).
               Bits [3:0] = base (0–16), bits [6:4] = multiplier (0–4).
               DTR0 > 0x4F resets both to 0 (IEC 62386-102). */
            if (check_config_repeat(addr_byte, data_byte, now)) {
                if (dtr0 > 0x4F) {
                    ext_fade_base = 0;
                    ext_fade_mult = 0;
                } else {
                    ext_fade_base = dtr0 & 0x0F;
                    ext_fade_mult = (dtr0 >> 4) & 0x07;
                }
                nvm_mark_dirty();
                reset_state = 0;
                printf("EXTFADE b=%d m=%d\n", ext_fade_base, ext_fade_mult);
            }
            break;

        default:
            /* ── Range-based commands ─────────────────────────────── */
            if (data_byte >= DALI_CMD_GO_TO_SCENE_BASE
                && data_byte <= DALI_CMD_GO_TO_SCENE_BASE + 15) {
                /* Cmd 16–31: GO TO SCENE — recall scene level with fadeTime */
                uint8_t scene = data_byte - DALI_CMD_GO_TO_SCENE_BASE;
                uint8_t slevel = scene_level[scene];
                if (slevel == 0xFF) break;  /* MASK — not in scene, ignore */
                fade_running = 0;
                slevel = clamp_level(slevel);
                uint32_t eff_fade_ms = (fade_time > 0)
                                     ? dali_fade_time_ms[fade_time]
                                     : get_ext_fade_time_ms();
                if (slevel == 0 || eff_fade_ms == 0 || actual_level == slevel) {
                    actual_level = slevel;
                    if (arc_callback) arc_callback(actual_level);
                } else {
                    target_level = slevel;
                    uint16_t steps = (actual_level > slevel)
                                   ? (actual_level - slevel)
                                   : (slevel - actual_level);
                    fade_ms_per_step = eff_fade_ms / steps;
                    if (fade_ms_per_step < 1) fade_ms_per_step = 1;
                    last_step_ms = now;
                    fade_running = 1;
                }
            } else if (data_byte >= DALI_CMD_STORE_SCENE_BASE
                       && data_byte <= DALI_CMD_STORE_SCENE_BASE + 15) {
                /* Cmd 64–79: STORE DTR AS SCENE LEVEL (config repeat) */
                if (check_config_repeat(addr_byte, data_byte, now)) {
                    uint8_t scene = data_byte - DALI_CMD_STORE_SCENE_BASE;
                    scene_level[scene] = dtr0;
                    nvm_mark_dirty();
                    reset_state = 0;
                    printf("SCENE%d=%d\n", scene, dtr0);
                }
            } else if (data_byte >= DALI_CMD_REMOVE_SCENE_BASE
                       && data_byte <= DALI_CMD_REMOVE_SCENE_BASE + 15) {
                /* Cmd 80–95: REMOVE FROM SCENE (config repeat) */
                if (check_config_repeat(addr_byte, data_byte, now)) {
                    uint8_t scene = data_byte - DALI_CMD_REMOVE_SCENE_BASE;
                    scene_level[scene] = 0xFF;  /* MASK */
                    nvm_mark_dirty();
                    reset_state = 0;
                    printf("RMSCENE%d\n", scene);
                }
            } else if (data_byte >= DALI_CMD_ADD_GROUP_BASE
                       && data_byte <= DALI_CMD_ADD_GROUP_BASE + 15) {
                /* Cmd 96–111: ADD TO GROUP (config repeat) */
                if (check_config_repeat(addr_byte, data_byte, now)) {
                    uint8_t group = data_byte - DALI_CMD_ADD_GROUP_BASE;
                    group_membership |= (1 << group);
                    nvm_mark_dirty();
                    reset_state = 0;
                    printf("ADDGRP%d\n", group);
                }
            } else if (data_byte >= DALI_CMD_REMOVE_GROUP_BASE
                       && data_byte <= DALI_CMD_REMOVE_GROUP_BASE + 15) {
                /* Cmd 112–127: REMOVE FROM GROUP (config repeat) */
                if (check_config_repeat(addr_byte, data_byte, now)) {
                    uint8_t group = data_byte - DALI_CMD_REMOVE_GROUP_BASE;
                    group_membership &= ~(1 << group);
                    nvm_mark_dirty();
                    reset_state = 0;
                    printf("RMGRP%d\n", group);
                }
            } else if (data_byte >= 224 && enabled_dt == DALI_DEVICE_TYPE) {
                /* Cmd 224–254: DT8 extended commands */
                process_dt8_command(data_byte);
            } else if (data_byte >= 144) {
                /* Cmd 144–223: Query commands — backward frame response */
                process_query_command(data_byte);
            }
            break;
        }
    }
}

/* ================================================================== *
 *  FADE ENGINE TICK                                                   *
 *                                                                     *
 *  Called from main loop alongside dali_process(). Steps the fade     *
 *  one level when enough time has elapsed. For fadeTime fades, the    *
 *  step interval = fade_time_ms[n] / abs(target - start). For        *
 *  fadeRate fades (UP/DOWN), the interval = fade_rate_ms[n].          *
 * ================================================================== */
void dali_fade_tick(void) {
    if (!fade_running) return;

    uint32_t now = millis();
    if (now - last_step_ms < fade_ms_per_step) return;
    last_step_ms = now;

    if (actual_level < target_level) {
        actual_level++;
    } else if (actual_level > target_level) {
        actual_level--;
    }

    if (arc_callback) arc_callback(actual_level);

    if (actual_level == target_level) {
        fade_running = 0;
    }
}

/* ================================================================== *
 *  PUBLIC API                                                         *
 * ================================================================== */

/*
 * dali_init() — configure peripherals for DALI communication.
 *
 * Sets up:
 * 1. TIM2 as a free-running 1 MHz counter (1 µs resolution) for
 *    edge timestamping and output compare ISRs
 * 2. EXTI0 on PC0 for both-edge interrupts (forward frame RX)
 * 3. PC5 as push-pull GPIO output for backward frame TX
 */
void dali_init(void) {
    /* ── TIM2: free-running 1 MHz counter ──────────────────────── */
    RCC->APB1PCENR |= RCC_APB1Periph_TIM2; /* Enable TIM2 clock */
    TIM2->PSC   = DALI_TIMER_PSC;           /* 48 MHz / 48 = 1 MHz */
    TIM2->ATRLR = DALI_TIMER_ARR;           /* 16-bit free-running */
    TIM2->DMAINTENR = 0;                    /* All interrupts off initially */
    TIM2->CTLR1 = TIM_CEN;                 /* Start counting */
    NVIC_EnableIRQ(TIM2_IRQn);

    /* ── EXTI0 on PC0: both-edge for DALI RX ──────────────────── */
    RCC->APB2PCENR |= RCC_APB2Periph_AFIO; /* AFIO clock for EXTI routing */
    /* PC0 = floating input (high-impedance) */
    GPIOC->CFGLR = (GPIOC->CFGLR & ~(0xF << (DALI_RX_PIN_N * 4)))
                 | (GPIO_CNF_IN_FLOATING << (DALI_RX_PIN_N * 4));
    /* Route EXTI line 0 to port C (AFIO_EXTICR[1:0] = 0b10 = port C) */
    AFIO->EXTICR = (AFIO->EXTICR & ~0x03) | 0x02;
    EXTI->RTENR  |= EXTI_Line0;            /* Rising edge trigger */
    EXTI->FTENR  |= EXTI_Line0;            /* Falling edge trigger */
    EXTI->INTENR |= EXTI_Line0;            /* Enable EXTI0 interrupt */
    NVIC_EnableIRQ(EXTI7_0_IRQn);

    /* ── TX pin: PC5 push-pull output, idle HIGH ──────────────── */
    GPIOC->CFGLR = (GPIOC->CFGLR & ~(0xF << (DALI_TX_PIN_N * 4)))
                 | ((GPIO_Speed_10MHz | GPIO_CNF_OUT_PP) << (DALI_TX_PIN_N * 4));
    tx_bus_idle();                          /* Bus idle = HIGH */
}

/*
 * dali_process() — main loop frame dispatcher.
 *
 * Must be called continuously from the main while(1) loop.
 * Checks the rx_frame_ready flag (set by idle timeout ISR),
 * validates frame length, and dispatches to process_frame().
 * Also monitors the 15-minute initialisation state timeout.
 */
void dali_process(void) {
    /* Check 15-minute initialisation timeout (IEC 62386-102 §9.6.3) */
    if (init_state == INIT_ENABLED) {
        if (millis() - init_start_time > DALI_INIT_TIMEOUT_MS) {
            init_state = INIT_DISABLED;
        }
    }

    /* Check if a complete frame has been received */
    if (!rx_frame_ready) return;
    rx_frame_ready = 0;

    /* Convert half-bit count to bit count.
       A valid 16-bit forward frame has 32 half-bits → 16 bits. */
    uint8_t bitlen = rx_len >> 1;

    if (bitlen == 16) {
        process_frame(rx_msg[0], rx_msg[1]);
    }
    /* Frames with bitlen != 16 are silently discarded.
       (24-bit frames for device type extensions are not supported.) */
}

void dali_set_arc_callback(dali_arc_callback_t cb) {
    arc_callback = cb;
}

void dali_set_colour_callback(dali_colour_callback_t cb) {
    colour_callback = cb;
}

uint8_t dali_get_actual_level(void) {
    return actual_level;
}

/* Returns 1 if TX state machine is idle (no backward frame in progress). */
uint8_t dali_is_tx_idle(void) {
    return (tx_state == TX_IDLE);
}

/* Returns millis() timestamp of the last valid RX edge.
 * Used by main loop to implement bus-idle guard for __WFI(). */
uint32_t dali_last_rx_edge_ms(void) {
    return rx_last_edge_ms;
}


const volatile uint8_t *dali_get_colour_actual(void) {
    return colour_actual;
}

/* ================================================================== *
 *  NVM STATE ACCESSORS — pack/unpack persistent variables             *
 *                                                                     *
 *  Called by dali_nvm.c to transfer state between RAM and flash.      *
 *  The NVM struct layout is defined in dali_nvm.h.                    *
 * ================================================================== */

void dali_get_nvm_state(dali_nvm_t *nvm) {
    nvm->short_address   = short_address;
    nvm->max_level       = max_level;
    nvm->min_level       = min_level;
    nvm->power_on_level  = power_on_level;
    nvm->sys_fail_level  = sys_fail_level;
    nvm->fade_time       = fade_time;
    nvm->fade_rate       = fade_rate;
    nvm->group_membership = group_membership;
    for (uint8_t i = 0; i < 16; i++)
        nvm->scene_level[i] = scene_level[i];
    for (uint8_t i = 0; i < 4; i++)
        nvm->colour[i] = colour_actual[i];
    nvm->colour_tc = colour_tc;
    nvm->ext_fade = (ext_fade_mult << 4) | ext_fade_base;
}

void dali_set_nvm_state(const dali_nvm_t *nvm) {
    short_address   = nvm->short_address;
    max_level       = nvm->max_level;
    min_level       = nvm->min_level;
    power_on_level  = nvm->power_on_level;
    sys_fail_level  = nvm->sys_fail_level;
    fade_time       = nvm->fade_time;
    fade_rate       = nvm->fade_rate;
    group_membership = nvm->group_membership;
    for (uint8_t i = 0; i < 16; i++)
        scene_level[i] = nvm->scene_level[i];
    /* DT8 colour: 0xFF = not stored (old firmware), use default 254 */
    for (uint8_t i = 0; i < 4; i++) {
        colour_actual[i] = (nvm->colour[i] == 0xFF) ? 254 : nvm->colour[i];
        colour_temp[i] = colour_actual[i];
    }
    colour_tc = (nvm->colour_tc == 0xFFFF) ? 0 : nvm->colour_tc;
    /* Extended fade time: 0xFF = not stored (old firmware), use defaults (0,0) */
    if (nvm->ext_fade != 0xFF) {
        ext_fade_base = nvm->ext_fade & 0x0F;
        ext_fade_mult = (nvm->ext_fade >> 4) & 0x07;
    }
    reset_state = 0;  /* NVM data loaded = not at defaults */
}

void dali_power_on(void) {
    /* IEC 62386-102 §9.7: Apply powerOnLevel at power-on.
       0xFF (MASK) means "last known level" — with flash persistence,
       we now have the actual last level. Level is clamped to [minLevel, maxLevel]. */
    uint8_t level = power_on_level;
    if (level == 0xFF) level = max_level;
    level = clamp_level(level);
    actual_level = level;
    power_cycle_seen = 1;
    if (arc_callback) arc_callback(actual_level);
    if (colour_callback)
        colour_callback((const uint8_t *)colour_actual, PWM_NUM_CHANNELS);
}
