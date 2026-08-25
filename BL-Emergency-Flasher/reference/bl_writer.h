/*
    dali_bl_update.h - Bootloader update over the DALI bus (vendor extension)

    Updates the 1920-byte DALI bootloader in the BOOT area (0x1FFFF000)
    from the RUNNING user firmware — the bootloader itself is never
    entered, the app (LEDs, DALI level commands) stays alive throughout.

    Self-contained module: receive state machine, EEPROM staging,
    Fletcher-16 verification, and the BOOT-area flash writer all live in
    dali_bl_update.c. The only integration point is one call at the top
    of the firmware's 32-bit frame dispatcher.
*/
#ifndef _DALI_BL_UPDATE_H
#define _DALI_BL_UPDATE_H

#include <stdint.h>

/* Feed every received 32-bit forward frame to the BL-update engine.
 * Returns 1 if the frame was consumed (caller must not process it),
 * 0 if the frame is none of the engine's business. */
uint8_t dali_bl_update_process(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3);

#endif /* _DALI_BL_UPDATE_H */
