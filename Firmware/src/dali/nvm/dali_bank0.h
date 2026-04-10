/*
    dali_bank0.h - DALI memory bank 0 (read-only gear identification)

    IEC 62386-102:2014 §4.3.10 mandates a read-only memory bank 0 that
    identifies the control gear: GTIN, firmware/hardware version, serial
    number, supported standard versions, number of logical units. The
    master accesses it via the READ MEMORY LOCATION command (cmd 197),
    using DTR2 = bank number, DTR1 = address within bank.

    This implementation provides only bank 0 (read-only); bank 1 and
    write access are out of scope. The byte layout below follows the
    most common DALI-2 interpretation of §4.3.10. Field offsets and
    sizes should be cross-checked against the spec version your master
    expects (a few field widths shifted between -102:2009, -102:2014,
    and -102:2014/A1:2018).

    Build-time customisation:
      DALI_FW_VERSION_MAJOR / _MINOR  — firmware version reported in bank 0
      DALI_HW_VERSION_MAJOR / _MINOR  — hardware revision reported in bank 0
      DALI_GTIN_BYTES (optional)      — 6-byte GTIN, MSB first
    These can be defined in hardware.h or via -D on the compiler line.
    GTIN and serial default to all-zero placeholders.
*/
#ifndef _DALI_BANK0_H
#define _DALI_BANK0_H

#include <stdint.h>

/* Last valid byte index in our bank 0 (= the value stored at location 0x00).
 * Layout below ends at 0x1A → 27 bytes total. */
#define DALI_BANK0_LAST_ADDR    0x1A

/* Read a single byte from bank 0. Returns 0xFF if addr is out of range. */
uint8_t dali_bank0_read(uint8_t addr);

#endif /* _DALI_BANK0_H */
