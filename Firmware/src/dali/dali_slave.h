/*
    dali_slave.h - DALI control gear (slave) public API

    Convenience header — re-exports the sub-module APIs so callers
    can include a single header. Each sub-module can also be included
    individually for finer-grained dependency control.

    Sub-modules:
      dali_phy.h        — Physical layer (RX/TX, collision detection)
      dali_protocol.h   — Protocol handler (commands, queries, NVM)
      dali_fade.h        — Fade engine
      dali_addressing.h — Addressing protocol
      dali_dt8.h        — DT8 colour control
      dali_state.h      — Shared device state struct

    Usage:
        dali_phy_init();
        dali_protocol_set_arc_callback(my_level_cb);
        dali_protocol_set_colour_callback(my_colour_cb);
        nvm_init();
        dali_protocol_power_on();
        while (1) {
            dali_protocol_process();
            dali_fade_tick();
            nvm_tick();
        }
*/
#ifndef _DALI_SLAVE_H
#define _DALI_SLAVE_H

#include <stdint.h>

/* Callback typedefs (needed by dali_state.h and dali_protocol.h) */
typedef void (*dali_arc_callback_t)(uint8_t level);
typedef void (*dali_colour_callback_t)(const uint8_t *levels, uint8_t count);

/* Sub-module headers */
#include "phy/dali_phy.h"
#include "protocol/dali_protocol.h"
#include "protocol/dali_fade.h"
#include "protocol/dali_addressing.h"

#endif /* _DALI_SLAVE_H */
