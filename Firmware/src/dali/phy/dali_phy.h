/*
    dali_phy.h - DALI physical layer (IEC 62386-101)

    Manchester-encoded RX/TX via EXTI + TIM2 on CH32V003.
    Completely self-contained — no dependency on protocol state.
    The only interface to the protocol layer is:
      - "here's a received frame" (dali_phy_frame_ready / frame_bytes)
      - "send this backward byte" (dali_phy_send_backward)
*/
#ifndef _DALI_PHY_H
#define _DALI_PHY_H

#include <stdint.h>

/* Initialize DALI physical layer peripherals:
 * - TIM2: free-running 1 MHz counter for edge timing + output compare ISRs
 * - EXTI0 on PC0: both-edge interrupt for forward frame reception
 * - PC5: push-pull GPIO output for backward frame transmission */
void dali_phy_init(void);

/* ISR entry points — called from interrupt handlers in main.c */
void dali_isr_rx_edge(void);       /* EXTI0: RX edge timestamp + decode */
void dali_isr_tx_tick(void);       /* TIM2 CH2: TX Manchester waveform gen */
void dali_isr_idle_timeout(void);  /* TIM2 CH4: end-of-frame detection */

/* Initiate 8-bit Manchester backward frame transmission.
 * Starts the TX state machine; waveform generated via TIM2 CH2 ISR. */
void dali_phy_send_backward(uint8_t data);

/* Returns 1 if TX state machine is idle (no backward frame in progress). */
uint8_t dali_phy_is_tx_idle(void);

/* Read-and-clear bus collision flag. Returns 1 once per collision event.
 * The TX ISR sets this when the bus level diverges from what we drove
 * (IEC 62386-101 §8.2.4.4). Requires real DALI PHY. */
uint8_t dali_phy_consume_collision(void);

/* Check if a complete frame has been received. Returns 1 and clears
 * the flag atomically. Returns 0 if no frame pending. */
uint8_t dali_phy_frame_ready(void);

/* Get the number of decoded bits in the last received frame (rx_len / 2).
 * Valid only after dali_phy_frame_ready() returned 1. */
uint8_t dali_phy_frame_bits(void);

/* Copy the decoded frame bytes into caller's buffer (3 bytes max).
 * Valid only after dali_phy_frame_ready() returned 1. */
void dali_phy_frame_bytes(uint8_t *out);

/* millis() timestamp of the last valid RX edge.
 * Used for WFI bus-idle guard in main loop. */
uint32_t dali_phy_last_rx_edge_ms(void);

#endif /* _DALI_PHY_H */
