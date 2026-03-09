/*
    dali_nvm.h - Non-volatile memory (flash) persistence for DALI configuration

    Stores DALI operating parameters in the last 64-byte flash page
    (0x08003FC0) of the CH32V003 16 KB flash. Uses deferred write:
    config commands set a dirty flag, and nvm_tick() writes to flash
    after 5 seconds of no changes (batches burst writes, reduces wear).

    CH32V003 flash endurance: ~10,000 erase cycles per page.
    For future upgrade path: swap to I2C EEPROM (1M cycles) by
    replacing nvm_save/nvm_load internals without changing the API.
*/
#ifndef _DALI_NVM_H
#define _DALI_NVM_H

#include <stdint.h>

/* Flash page address — last 64 bytes of 16 KB flash.
   Must not overlap with firmware code (check linker output). */
#define NVM_FLASH_ADDR      0x08003FC0
#define NVM_FLASH_PAGE_SIZE 64

/* Magic number to validate stored data ("DALI" in ASCII) */
#define NVM_MAGIC           0x44414C49

/* Deferred write delay: milliseconds after last change before saving */
#define NVM_SAVE_DELAY_MS   5000

/*
 * NVM data structure — packed into 64-byte flash page.
 * Field order chosen for natural alignment (uint32_t first, then
 * uint16_t, then uint8_t arrays). Total: 60 bytes < 64 bytes.
 *
 * The _reserved block allows adding fields (e.g., DT8 Tc limits)
 * in future firmware versions without changing the struct layout
 * or invalidating existing stored data.
 */
typedef struct dali_nvm_t {
    uint32_t magic;              /* 0x44414C49 = valid data */
    uint8_t  short_address;      /* 0–63 or 0xFF (unassigned) */
    uint8_t  max_level;          /* 1–254, default 254 */
    uint8_t  min_level;          /* 1–254, default 1 */
    uint8_t  power_on_level;     /* 0–254 or 0xFF (=last level), default 254 */
    uint8_t  sys_fail_level;     /* 0–254 or 0xFF, default 254 */
    uint8_t  fade_time;          /* 0–15, default 0 */
    uint8_t  fade_rate;          /* 1–15, default 7 */
    uint8_t  _pad1;              /* Alignment padding */
    uint16_t group_membership;   /* Bit N = member of group N */
    uint8_t  scene_level[16];    /* 0–254 or 0xFF (MASK = not in scene) */
    uint8_t  colour[4];          /* DT8 RGBW levels, 0xFF = default (254) */
    uint16_t colour_tc;          /* DT8 colour temp in mirek, 0xFFFF = not set */
    uint8_t  ext_fade;           /* DALI-2 extended fade time: (mult<<4)|base, 0xFF = not set */
    uint8_t  _reserved[23];     /* Future use — initialized to 0xFF */
} dali_nvm_t;

/*
 * Initialize NVM subsystem.
 * Reads flash page, validates magic. If valid, calls dali_set_nvm_state()
 * to restore persistent variables. If invalid (first boot or corrupted),
 * leaves defaults from dali_slave.c unchanged.
 * Call BEFORE dali_power_on().
 */
void nvm_init(void);

/*
 * Write current state to flash immediately.
 * Calls dali_get_nvm_state() to pack variables, then erases and
 * programs the flash page. Takes ~6 ms (interrupts may be delayed).
 * Normally called by nvm_tick(); can be called directly if needed.
 */
void nvm_save(void);

/*
 * Mark NVM as dirty — a persistent variable has changed.
 * Records the current timestamp. nvm_tick() will save after
 * NVM_SAVE_DELAY_MS of no further changes.
 */
void nvm_mark_dirty(void);

/*
 * Main loop tick — call continuously from while(1).
 * Checks if dirty flag is set and enough time has elapsed since
 * the last change, then saves to flash.
 */
void nvm_tick(void);

#endif
