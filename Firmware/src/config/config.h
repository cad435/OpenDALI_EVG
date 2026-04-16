/*
    config.h - Device identity and version configuration

    Defines version numbers, GTIN, and serial number reported in
    DALI memory bank 0 (IEC 62386-102 §4.3.10).
    Override any value via -D compiler flags in platformio.ini.
*/
#ifndef _CONFIG_H
#define _CONFIG_H

#include <stdint.h>

/* ── Firmware version ───────────────────────────────────────────────
   Reported in bank 0 at addresses 0x09 (major) / 0x0A (minor).
   Increment on each firmware release. */
#ifndef DALI_FW_VERSION_MAJOR
#define DALI_FW_VERSION_MAJOR   0
#endif
#ifndef DALI_FW_VERSION_MINOR
#define DALI_FW_VERSION_MINOR   1
#endif

/* ── Hardware version ───────────────────────────────────────────────
   Reported in bank 0 at addresses 0x13 (major) / 0x14 (minor).
   Increment when the PCB layout or component selection changes. */
#ifndef DALI_HW_VERSION_MAJOR
#define DALI_HW_VERSION_MAJOR   0
#endif
#ifndef DALI_HW_VERSION_MINOR
#define DALI_HW_VERSION_MINOR   1
#endif

/* ── GTIN (Global Trade Item Number) ────────────────────────────────
   48-bit integer stored MSB first in bank 0 at addresses 0x03-0x08.
   This is the EAN/UPC barcode number assigned by GS1 to identify
   the product type. Leave at 0 for development / open-source use.
   For commercial products, register at gs1.org.
   Example: -DDALI_GTIN=0x091201234567ULL */
#ifndef DALI_GTIN
#define DALI_GTIN   0x3452334E0CADULL
#endif

/* ── Identification / Serial Number ─────────────────────────────────
   64-bit integer stored MSB first in bank 0 at addresses 0x0B-0x12.
   Should be unique per device (distinguishes units with the same GTIN).
   The CH32V003 has no factory-programmed UID, so the serial must be
   assigned during production via -D flag per unit.

   Default: ASCII-encoded EVG mode name (e.g. "RGBW\0\0\0\0") from
   hardware.h. This lets a commissioning tool identify the firmware
   variant via bank 0 before uploading. Override with -DDALI_SERIAL=...
   for per-unit unique serials in production. */
#ifndef DALI_SERIAL
#define DALI_SERIAL EVG_MODE_SERIAL
#endif

#endif /* _CONFIG_H */
