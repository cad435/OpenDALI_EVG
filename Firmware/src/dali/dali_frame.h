/*
    dali_frame.h - Structured DALI frame type with status flags

    A small value type that wraps a received or transmitted DALI frame
    together with its bit length and status flags. Modeled after the
    OpenKNX DALI library's Frame struct (master-side), adapted for
    control-gear (slave) use.

    Used by dali_slave.c to:
    - Pass received frames from dali_process() into process_frame()
      without separate addr/data byte arguments
    - Carry status (FORWARD / BACKWARD / ERROR / COLLISION / ECHO)
      that the dispatcher can branch on
    - Allow future extension to 24-bit eDALI / DT-extended frames
      without breaking call signatures
*/
#ifndef _DALI_FRAME_H
#define _DALI_FRAME_H

#include <stdint.h>

/* ── Frame flag bits ───────────────────────────────────────────────── */
#define DALI_FRAME_FLAG_FORWARD     0x01    /* Forward frame (master → gear), 16 or 24 bit */
#define DALI_FRAME_FLAG_BACKWARD    0x02    /* Backward frame (gear → master), 8 bit */
#define DALI_FRAME_FLAG_ERROR       0x04    /* Decoding / framing error */
#define DALI_FRAME_FLAG_COLLISION   0x08    /* Bus collision detected during TX */
#define DALI_FRAME_FLAG_ECHO        0x10    /* Frame is our own TX seen on RX */

/*
 * dali_frame_t — represents a single DALI frame.
 *
 *  data:      MSB-aligned bit pattern. For 16-bit forward frames, the
 *             upper 16 bits hold [addr_byte:data_byte] (addr in bits 15..8,
 *             data in bits 7..0 of the lower half-word). The convenience
 *             accessors below extract them.
 *  size:      Number of valid bits (8, 16, or 24).
 *  flags:     Bitmask of DALI_FRAME_FLAG_* values.
 *  timestamp: millis() value at frame completion (set by RX path).
 */
typedef struct {
    uint32_t data;
    uint8_t  size;
    uint8_t  flags;
    uint32_t timestamp;
} dali_frame_t;

/* Extract address byte from a 16-bit forward frame. */
static inline uint8_t dali_frame_addr_byte(const dali_frame_t *f) {
    return (uint8_t)(f->data >> 8);
}

/* Extract data byte from a 16-bit forward frame. */
static inline uint8_t dali_frame_data_byte(const dali_frame_t *f) {
    return (uint8_t)(f->data & 0xFF);
}

#endif /* _DALI_FRAME_H */
