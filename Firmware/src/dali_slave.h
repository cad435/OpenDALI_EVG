/*
    dali_slave.h - DALI control gear (slave) interface for CH32V003 (ch32fun)

    Implements a DALI-2 control gear (IEC 62386-102 + IEC 62386-209 DT8):
    - Forward frame reception (16-bit Manchester decode via EXTI + TIM2)
    - Backward frame transmission (8-bit Manchester encode via TIM2 OC)
    - Broadcast and short address arc power commands
    - Basic query commands (status, gear present, actual level, etc.)
    - Full addressing protocol (INITIALISE, RANDOMISE, binary search,
      PROGRAM SHORT ADDRESS, WITHDRAW, TERMINATE)
    - DT8 colour control: RGBWAF primaries + colour temperature Tc

    Architecture:
    - ISRs (dali_isr_*) run in interrupt context, handle time-critical
      Manchester encoding/decoding via state machines
    - dali_process() runs in main loop, dispatches received frames
    - arc_callback is called from main loop context when level changes
    - colour_callback is called when DT8 ACTIVATE commits new colour

    Usage:
        dali_init();
        dali_set_arc_callback(my_level_cb);
        dali_set_colour_callback(my_colour_cb);
        while (1) { dali_process(); dali_fade_tick(); }
*/
#ifndef _DALI_SLAVE_H
#define _DALI_SLAVE_H

#include <stdint.h>

/*
 * Callback type for arc power level changes.
 * Called from dali_process() (main loop context) when the DALI level changes.
 * Parameter: level 0–254 (0 = off, 254 = maximum).
 */
typedef void (*dali_arc_callback_t)(uint8_t level);

/*
 * Callback type for DT8 colour changes.
 * Called from dali_process() when ACTIVATE commits new colour values.
 * Parameters:
 *   levels: array of per-channel colour levels (0–254), R,G,B,W order
 *   count:  number of channels (= PWM_NUM_CHANNELS from hardware.h)
 */
typedef void (*dali_colour_callback_t)(const uint8_t *levels, uint8_t count);

/*
 * Initialize DALI slave peripherals:
 * - TIM2: free-running 1 MHz counter for edge timing + output compare ISRs
 * - EXTI0 on PC0: both-edge interrupt for forward frame reception
 * - PC5: push-pull GPIO output for backward frame transmission
 */
void dali_init(void);

/*
 * Main loop processing — call this continuously from while(1).
 * Checks for received frames (set by idle timeout ISR) and dispatches
 * them to the protocol handler. Also monitors the 15-minute
 * initialisation state timeout.
 */
void dali_process(void);

/*
 * Fade engine tick — call this continuously from while(1) alongside
 * dali_process(). Steps the fade one level when enough time has elapsed
 * (based on fadeTime or fadeRate). Does nothing when no fade is active.
 */
void dali_fade_tick(void);

/* Register callback for arc power level changes. */
void dali_set_arc_callback(dali_arc_callback_t cb);

/* Register callback for DT8 colour changes (called on ACTIVATE). */
void dali_set_colour_callback(dali_colour_callback_t cb);

/* Get current actual level (0–254). Thread-safe (single byte read). */
uint8_t dali_get_actual_level(void);

/* Returns 1 if TX state machine is idle (no backward frame active). */
uint8_t dali_is_tx_idle(void);

/* Returns millis() of last valid RX edge. Used for WFI bus-idle guard in main loop:
 * only enter __WFI() if (millis() - dali_last_rx_edge_ms() > DALI_WFI_IDLE_MS).
 * This prevents WFI during active frame reception, avoiding timing corruption. */
uint32_t dali_last_rx_edge_ms(void);


/* Get current DT8 colour levels (R,G,B,W). Returns pointer to 4-byte array. */
const volatile uint8_t *dali_get_colour_actual(void);

/* Apply power-on level at boot. Call after dali_init() and callback registration. */
void dali_power_on(void);

/* Forward declaration for NVM struct (defined in dali_nvm.h) */
struct dali_nvm_t;
typedef struct dali_nvm_t dali_nvm_t;

/*
 * Pack current persistent state into an NVM struct for flash storage.
 * Called by nvm_save() before writing to flash.
 */
void dali_get_nvm_state(dali_nvm_t *nvm);

/*
 * Restore persistent state from an NVM struct loaded from flash.
 * Called by nvm_init() at boot when valid data is found.
 */
void dali_set_nvm_state(const dali_nvm_t *nvm);

/*
 * ISR entry points — called from interrupt handlers defined in main.c.
 * These must be called from the corresponding ISR with minimal overhead.
 *
 * dali_isr_rx_edge():      EXTI0 handler — timestamps edge, runs RX state machine
 * dali_isr_tx_tick():      TIM2 CH2 OC handler — generates TX Manchester waveform
 * dali_isr_idle_timeout():  TIM2 CH4 OC handler — detects end-of-frame (no edges for 5 Te)
 */
void dali_isr_rx_edge(void);
void dali_isr_tx_tick(void);
void dali_isr_idle_timeout(void);

#endif
