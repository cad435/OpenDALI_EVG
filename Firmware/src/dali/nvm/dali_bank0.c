/*
    dali_bank0.c - Static read-only DALI memory bank 0

    See dali_bank0.h for the rationale and the spec reference. The byte
    layout follows IEC 62386-102:2014 §4.3.10 (DALI-2 control gear).
*/
#include "dali_bank0.h"
#include "../../config/hardware.h"

/* ── Build-time defaults ───────────────────────────────────────────── *
 * Override any of these in hardware.h or via -D on the compiler line. */
#ifndef DALI_FW_VERSION_MAJOR
#define DALI_FW_VERSION_MAJOR   1
#endif
#ifndef DALI_FW_VERSION_MINOR
#define DALI_FW_VERSION_MINOR   0
#endif
#ifndef DALI_HW_VERSION_MAJOR
#define DALI_HW_VERSION_MAJOR   1
#endif
#ifndef DALI_HW_VERSION_MINOR
#define DALI_HW_VERSION_MINOR   0
#endif

/* GTIN: 6 bytes, MSB first. Defaults to all zeros (placeholder).
 * Replace with the real GTIN once allocated. */
#ifndef DALI_GTIN_B0
#define DALI_GTIN_B0  0x00
#define DALI_GTIN_B1  0x00
#define DALI_GTIN_B2  0x00
#define DALI_GTIN_B3  0x00
#define DALI_GTIN_B4  0x00
#define DALI_GTIN_B5  0x00
#endif

/* Identification number / serial: 8 bytes, MSB first. Defaults to all
 * zeros. For real product builds, this should be a per-unit unique ID
 * (e.g., MCU UID hashed into 8 bytes) — best done at provisioning time.
 * On CH32V003 there is no factory-programmed UID, so a per-unit serial
 * has to be flashed during production. */
#ifndef DALI_SERIAL_B0
#define DALI_SERIAL_B0  0x00
#define DALI_SERIAL_B1  0x00
#define DALI_SERIAL_B2  0x00
#define DALI_SERIAL_B3  0x00
#define DALI_SERIAL_B4  0x00
#define DALI_SERIAL_B5  0x00
#define DALI_SERIAL_B6  0x00
#define DALI_SERIAL_B7  0x00
#endif

/* ── Bank 0 layout (IEC 62386-102:2014 §4.3.10) ────────────────────── *
 *
 *   Loc   Bytes  Content
 *   0x00  1      Last accessible memory location in this bank
 *   0x01  1      Reserved (0xFF)
 *   0x02  1      Number of last accessible memory bank (0 = bank 0 only)
 *   0x03  6      GTIN (MSB first)
 *   0x09  1      Firmware version major (vendor-defined)
 *   0x0A  1      Firmware version minor
 *   0x0B  8      Identification number / serial (MSB first)
 *   0x13  1      Hardware version major
 *   0x14  1      Hardware version minor
 *   0x15  1      IEC 62386-101 version (DALI-2 = 0x08)
 *   0x16  1      IEC 62386-102 version (DALI-2 = 0x08)
 *   0x17  1      IEC 62386-103 version (0xFF = not a control device)
 *   0x18  1      Number of logical control device units (0xFF = none)
 *   0x19  1      Number of logical control gear units (1)
 *   0x1A  1      Index of this logical control gear unit (0)
 *
 *  Total length: 27 bytes (last addr = 0x1A).
 * ──────────────────────────────────────────────────────────────────────*/
static const uint8_t dali_bank0[DALI_BANK0_LAST_ADDR + 1] = {
    /* 0x00 */ DALI_BANK0_LAST_ADDR,
    /* 0x01 */ 0xFF,
    /* 0x02 */ 0x00,
    /* 0x03..0x08  GTIN (6 bytes, MSB first) */
    DALI_GTIN_B0, DALI_GTIN_B1, DALI_GTIN_B2,
    DALI_GTIN_B3, DALI_GTIN_B4, DALI_GTIN_B5,
    /* 0x09 */ DALI_FW_VERSION_MAJOR,
    /* 0x0A */ DALI_FW_VERSION_MINOR,
    /* 0x0B..0x12  Identification number (8 bytes, MSB first) */
    DALI_SERIAL_B0, DALI_SERIAL_B1, DALI_SERIAL_B2, DALI_SERIAL_B3,
    DALI_SERIAL_B4, DALI_SERIAL_B5, DALI_SERIAL_B6, DALI_SERIAL_B7,
    /* 0x13 */ DALI_HW_VERSION_MAJOR,
    /* 0x14 */ DALI_HW_VERSION_MINOR,
    /* 0x15 */ 0x08,    /* 101 version: DALI-2 */
    /* 0x16 */ 0x08,    /* 102 version: DALI-2 */
    /* 0x17 */ 0xFF,    /* 103 version: not a control device */
    /* 0x18 */ 0xFF,    /* logical control device units: none */
    /* 0x19 */ 0x01,    /* logical control gear units: 1 */
    /* 0x1A */ 0x00,    /* index of this logical control gear unit */
};

uint8_t dali_bank0_read(uint8_t addr) {
    if (addr > DALI_BANK0_LAST_ADDR) return 0xFF;
    return dali_bank0[addr];
}
