/*
    dali_bank2.c - Static read-only DALI memory bank 2 (build identity)

    See dali_bank2.h. EVG_BUILD_TIME / EVG_BUILD_GIT / EVG_BUILD_DIRTY are
    injected for this file only by scripts/build_info.py; a build without
    the script reports zeros.
*/
#include "dali_bank2.h"

#ifndef EVG_BUILD_TIME
#define EVG_BUILD_TIME  0UL
#endif
#ifndef EVG_BUILD_GIT
#define EVG_BUILD_GIT   0UL
#endif
#ifndef EVG_BUILD_DIRTY
#define EVG_BUILD_DIRTY 0
#endif

/* Split a 32-bit value into 4 bytes, MSB first */
#define B32(v, n)   ((uint8_t)(((uint32_t)(v) >> (24 - 8*(n))) & 0xFF))

/* ── Bank 2 layout (IEC 62386-102 §9.10.2, Table 9) ────────────────── *
 *
 *   Loc   Bytes  Content
 *   0x00  1      Last accessible memory location in this bank (0x0B)
 *   0x01  1      Indicator byte (manufacturer specific, unused: 0xFF)
 *   0x02  1      Lock byte (0xFF, bank is read-only)
 *   0x03  4      Build time, Unix seconds UTC (MSB first)
 *   0x07  4      Git commit, first 8 hex digits of HEAD (MSB first)
 *   0x0B  1      Flags: bit 0 = built from a dirty working tree
 *
 * The magic in front lets EVG-Updater find the bank image in firmware.bin.
 * Both live in one object, so the linker keeps them together.
 * ──────────────────────────────────────────────────────────────────────*/
static const struct {
    char    magic[8];
    uint8_t bank[DALI_BANK2_LAST_ADDR + 1];
} build_record = {
    { 'E', 'V', 'G', 'B', 'U', 'I', 'L', 'D' },
    {
        /* 0x00 */ DALI_BANK2_LAST_ADDR,
        /* 0x01 */ 0xFF,
        /* 0x02 */ 0xFF,
        /* 0x03..0x06  build time */
        B32(EVG_BUILD_TIME, 0), B32(EVG_BUILD_TIME, 1),
        B32(EVG_BUILD_TIME, 2), B32(EVG_BUILD_TIME, 3),
        /* 0x07..0x0A  git commit */
        B32(EVG_BUILD_GIT, 0), B32(EVG_BUILD_GIT, 1),
        B32(EVG_BUILD_GIT, 2), B32(EVG_BUILD_GIT, 3),
        /* 0x0B */ EVG_BUILD_DIRTY ? 0x01 : 0x00,
    },
};

uint8_t dali_bank2_read(uint8_t addr) {
    if (addr > DALI_BANK2_LAST_ADDR) return 0xFF;
    return build_record.bank[addr];
}
