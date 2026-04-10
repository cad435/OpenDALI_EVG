/*
    dali_addressing.h - DALI addressing protocol (IEC 62386-102 §9.6)

    Handles special commands: INITIALISE, RANDOMISE, COMPARE,
    SEARCHADDR, PROGRAM SHORT, WITHDRAW, VERIFY SHORT, QUERY SHORT,
    TERMINATE, DTR0/1/2, ENABLE DEVICE TYPE.
*/
#ifndef _DALI_ADDRESSING_H
#define _DALI_ADDRESSING_H

#include <stdint.h>

/* Process a special command (addr_byte pattern 101xxxxx1 or 110xxxxx1).
 * Called from the protocol dispatcher when a special command is detected. */
void dali_addressing_process_special(uint8_t addr_byte, uint8_t data_byte);

/* Check the 15-minute initialisation state timeout.
 * Call from main loop (via dali_process). */
void dali_addressing_check_timeout(void);

/* Returns 1 if the device is in initialisation state (INIT_ENABLED). */
uint8_t dali_addressing_in_init(void);

/* Get random address bytes (for QUERY RANDOM H/M/L commands) */
uint8_t dali_addressing_random_h(void);
uint8_t dali_addressing_random_m(void);
uint8_t dali_addressing_random_l(void);

#endif /* _DALI_ADDRESSING_H */
