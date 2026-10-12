/*
    dali_bank2.h - DALI memory bank 2 (read-only build identity)

    Manufacturer-specific memory bank (IEC 62386-102 §9.10.9, layout per
    Table 9) that tells which firmware build the device is running:
    build time and git commit, stamped by scripts/build_info.py.

    Read with the standard READ MEMORY LOCATION command, DTR1 = 2,
    DTR0 = address — the same way as bank 0.

    The same bytes are stored in firmware.bin behind the magic "EVGBUILD",
    so EVG-Updater can compare the file it flashed with what the device
    reports after the reboot.
*/
#ifndef _DALI_BANK2_H
#define _DALI_BANK2_H

#include <stdint.h>

#define DALI_BANK2_NUMBER       2

/* Last valid byte index in bank 2 (= the value stored at location 0x00). */
#define DALI_BANK2_LAST_ADDR    0x0B

/* Read a single byte from bank 2. Returns 0xFF if addr is out of range. */
uint8_t dali_bank2_read(uint8_t addr);

#endif /* _DALI_BANK2_H */
