/*
    dali_nvm.c - Non-volatile storage for DALI configuration

    Stores DALI operating parameters in the AT24C256 I2C EEPROM.
    Uses the eeprom driver (src/eeprom/) for hardware access.

    EEPROM layout (from eeprom_layout.h):
      0x0000–0x003F  Device identity (GTIN, EVG mode, versions, short addr)
      0x0040–0x007F  DALI config (dali_nvm_t struct — same as former flash layout)
      0x0080–0x7FFF  Firmware staging area (used by bootloader)

    Deferred write: config commands call nvm_mark_dirty(), and
    nvm_tick() saves to EEPROM after 5 seconds of no further changes.
    AT24C256 endurance: 1,000,000 write cycles (vs 10,000 for flash).

    The device identity block is written once at boot and whenever the
    short address changes. The bootloader reads it to validate Block 0
    during firmware updates.
*/

#include "ch32fun.h"
#include <string.h>
#include "dali_nvm.h"
#include "../../logger.h"
#include "../dali_state.h"
#include "../phy/dali_phy.h"
#include "../protocol/dali_addressing.h"
#include "../../eeprom/eeprom.h"
#include "../../eeprom/eeprom_layout.h"
#include "../../config/hardware.h"
#include "../../config/config.h"

/* millis() provided by main.c */
extern uint32_t millis(void);

/* Deferred write state */
static volatile uint8_t  nvm_dirty = 0;
static volatile uint32_t nvm_dirty_time = 0;

/* Split DALI_GTIN (48-bit) into 6 bytes, MSB first */
#define GTIN_B(n)   ((uint8_t)((DALI_GTIN >> (40 - 8*(n))) & 0xFF))

/* ================================================================== *
 *  Identity block — written to EEPROM at boot + on address change     *
 *                                                                     *
 *  This is a *separate* identity copy from the DALI bank 0 in flash   *
 *  (dali_bank0.c). It is consumed only by the bootloader for Block 0  *
 *  validation during firmware updates (GTIN + EVG-mode match) — the   *
 *  DALI master never sees it. Re-written on every boot from the same  *
 *  compile-time defines bank 0 uses, so the two can't diverge.        *
 * ================================================================== */
static void nvm_write_identity(void) {
    ee_identity_t id = {
        .magic        = EE_MAGIC,
        .gtin         = { GTIN_B(0), GTIN_B(1), GTIN_B(2),
                          GTIN_B(3), GTIN_B(4), GTIN_B(5) },
        .evg_mode_id  = EVG_MODE_ID,
        .hw_ver_major = DALI_HW_VERSION_MAJOR,
        .hw_ver_minor = DALI_HW_VERSION_MINOR,
        .fw_ver_major = DALI_FW_VERSION_MAJOR,
        .fw_ver_minor = DALI_FW_VERSION_MINOR,
        .short_address = ds.short_address,
    };
    if (!eeprom_write(EE_ADDR_IDENTITY, (const uint8_t *)&id, sizeof(id)))
        LOG_ERR("EEPROM identity write failed");
}

/* ================================================================== *
 *  nvm_init() — Load persistent state from EEPROM at boot             *
 * ================================================================== */
void nvm_init(void) {
    /* Read config block from EEPROM */
    union {
        dali_nvm_t nvm;
        uint8_t bytes[EE_CONFIG_SIZE];
    } buf;

    if (!eeprom_read(EE_ADDR_CONFIG, buf.bytes, sizeof(buf))) {
        LOG_ERR("EEPROM config read");
        return;
    }

    if (buf.nvm.magic == NVM_MAGIC) {
        nvm_unpack_state(&buf.nvm);
        LOG_NVM("loaded addr=%d", buf.nvm.short_address);
    } else {
        LOG_NVM("no valid data in EEPROM");
    }

    /* Always write identity block (updates GTIN, versions, short addr) */
    nvm_write_identity();
}

/* ================================================================== *
 *  nvm_save() — Write current state to EEPROM                         *
 * ================================================================== */
void nvm_save(void) {
    union {
        dali_nvm_t nvm;
        uint8_t bytes[EE_CONFIG_SIZE];
    } buf;

    memset(&buf, 0xFF, sizeof(buf));
    buf.nvm.magic = NVM_MAGIC;
    nvm_pack_state(&buf.nvm);

    /* Write config to EEPROM (64 bytes = 1 page, no boundary crossing) */
    if (!eeprom_write(EE_ADDR_CONFIG, buf.bytes, sizeof(buf))) {
        LOG_ERR("EEPROM config write");
        nvm_dirty_time = millis();  /* retry after another delay, not immediately */
        return;
    }

    /* Update short address in identity block too */
    nvm_write_identity();

    nvm_dirty = 0;
    LOG_NVM("saved addr=%d", buf.nvm.short_address);
}

/* ================================================================== *
 *  nvm_mark_dirty() / nvm_tick() — Deferred write                     *
 * ================================================================== */
void nvm_mark_dirty(void) {
    nvm_dirty = 1;
    nvm_dirty_time = millis();
}

void nvm_tick(void) {
    if (!nvm_dirty) return;
    if (millis() - nvm_dirty_time < NVM_SAVE_DELAY_MS) return;
    /* Only write when DALI bus is idle (no frame in last 50ms).
     * EEPROM write blocks the main loop for ~17ms, which could
     * delay a backward frame response past the 9.2ms deadline. */
    if (millis() - dali_phy_last_rx_edge_ms() < 50) return;
    nvm_save();
}

/* ================================================================== *
 *  NVM STATE PACK / UNPACK (unchanged from flash version)             *
 * ================================================================== */
void nvm_pack_state(dali_nvm_t *nvm) {
    nvm->short_address   = ds.short_address;
    nvm->max_level       = ds.max_level;
    nvm->min_level       = ds.min_level;
    nvm->power_on_level  = ds.power_on_level;
    nvm->sys_fail_level  = ds.sys_fail_level;
    nvm->fade_time       = ds.fade_time;
    nvm->fade_rate       = ds.fade_rate;
    nvm->group_membership = ds.group_membership;
    for (uint8_t i = 0; i < 16; i++)
        nvm->scene_level[i] = ds.scene_level[i];
    for (uint8_t i = 0; i < 4; i++)
        nvm->colour[i] = ds.colour_actual[i];
#if EVG_HAS_DT8
    nvm->colour_tc = ds.colour_tc;
#else
    nvm->colour_tc = 0xFFFF;
#endif
    nvm->ext_fade = (ds.ext_fade_mult << 4) | ds.ext_fade_base;
    nvm->random_h = dali_addressing_random_h();
    nvm->random_m = dali_addressing_random_m();
    nvm->random_l = dali_addressing_random_l();
}

void nvm_unpack_state(const dali_nvm_t *nvm) {
    ds.short_address   = nvm->short_address;
    ds.max_level       = nvm->max_level;
    ds.min_level       = nvm->min_level;
    ds.power_on_level  = nvm->power_on_level;
    ds.sys_fail_level  = nvm->sys_fail_level;
    ds.fade_time       = nvm->fade_time;
    ds.fade_rate       = nvm->fade_rate;
    ds.group_membership = nvm->group_membership;
    for (uint8_t i = 0; i < 16; i++)
        ds.scene_level[i] = nvm->scene_level[i];
    for (uint8_t i = 0; i < 4; i++)
        ds.colour_actual[i] = (nvm->colour[i] == 0xFF) ? 254 : nvm->colour[i];
#if EVG_HAS_DT8
    ds.colour_tc = (nvm->colour_tc == 0xFFFF) ? 0 : nvm->colour_tc;
#endif
    if (nvm->ext_fade != 0xFF) {
        ds.ext_fade_base = nvm->ext_fade & 0x0F;
        ds.ext_fade_mult = (nvm->ext_fade >> 4) & 0x07;
    }
    /* Restore RANDOM ADDRESS. Pre-existing EEPROMs from firmware
     * versions before random-address persistence had 0xFF here
     * (was part of _reserved), which matches the spec factory
     * default — so old saves auto-upgrade without intervention. */
    dali_addressing_set_random(nvm->random_h, nvm->random_m, nvm->random_l);
    ds.reset_state = 0;
}
