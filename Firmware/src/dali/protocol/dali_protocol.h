/*
    dali_protocol.h - DALI protocol handler (IEC 62386-102)

    Command dispatcher, query handler, config commands, arc power,
    NVM state serialization, bootloader entry. Orchestrates all
    sub-modules (dali_phy, dali_fade, dali_addressing, dali_dt8).
*/
#ifndef _DALI_PROTOCOL_H
#define _DALI_PROTOCOL_H

#include <stdint.h>
#include "../dali_slave.h"  /* callback typedefs */

/* Initialize protocol state to defaults. Call after dali_phy_init(). */
void dali_protocol_init(void);

/* Main loop frame dispatcher — processes received frames.
 * Also checks addressing timeout. */
void dali_protocol_process(void);

/* Apply power-on level at boot. Call after nvm_init() + callback registration. */
void dali_protocol_power_on(void);

/* Register callbacks */
void dali_protocol_set_arc_callback(dali_arc_callback_t cb);
void dali_protocol_set_colour_callback(dali_colour_callback_t cb);

/* Getters */
uint8_t dali_protocol_get_actual_level(void);
const volatile uint8_t *dali_protocol_get_colour_actual(void);

#endif /* _DALI_PROTOCOL_H */
