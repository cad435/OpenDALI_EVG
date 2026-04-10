/*
    dali_nvm.c - Flash persistence for DALI configuration

    Stores DALI operating parameters in the last 64-byte flash page
    (0x08003FC0) of the CH32V003 16 KB flash. Direct register access
    (no HAL — ch32fun doesn't provide one).

    Flash programming sequence (from ch32fun flashtest example):
    1. Unlock: write KEY1/KEY2 to KEYR and MODEKEYR
    2. Erase:  set CR_PAGE_ER, set ADDR, set CR_STRT_Set, wait BSY
    3. Write:  set CR_PAGE_PG, reset buffer, load 16 words, commit
    4. Lock:   set CR_LOCK_Set

    Deferred write: config commands call nvm_mark_dirty(), and
    nvm_tick() saves to flash after 5 seconds of no further changes.
    This batches burst writes (e.g., 16 scenes) into a single erase.
*/

#include "ch32fun.h"
#include <stdio.h>
#include <string.h>
#include "dali_nvm.h"
#include "../dali_state.h"

/* millis() provided by main.c */
extern uint32_t millis(void);

/* Flash programming constants (from ch32v003hw.h) */
#define CR_PAGE_ER      ((uint32_t)0x00020000)  /* Page erase 64 bytes */
#define CR_PAGE_PG      ((uint32_t)0x00010000)  /* Page programming 64 bytes */
#define CR_BUF_LOAD     ((uint32_t)0x00040000)  /* Buffer load */
#define CR_BUF_RST      ((uint32_t)0x00080000)  /* Buffer reset */
#define CR_STRT_Set     ((uint32_t)0x00000040)  /* Start operation */
#define CR_LOCK_Set     ((uint32_t)0x00000080)  /* Lock flash */

/* Deferred write state */
static volatile uint8_t  nvm_dirty = 0;
static volatile uint32_t nvm_dirty_time = 0;

/* ================================================================== *
 *  flash_unlock() — Unlock flash for erase/program operations         *
 * ================================================================== */
static void flash_unlock(void) {
    FLASH->KEYR = FLASH_KEY1;
    FLASH->KEYR = FLASH_KEY2;
    FLASH->MODEKEYR = FLASH_KEY1;
    FLASH->MODEKEYR = FLASH_KEY2;
}

/* ================================================================== *
 *  flash_lock() — Re-lock flash after programming                     *
 * ================================================================== */
static void flash_lock(void) {
    FLASH->CTLR = CR_LOCK_Set;
}

/* ================================================================== *
 *  flash_erase_page() — Erase a 64-byte flash page                    *
 *                                                                     *
 *  Takes ~3ms. Interrupts are NOT disabled — the DALI RX ISR can      *
 *  still fire (it doesn't access flash). The deferred write strategy  *
 *  ensures this happens during bus idle time.                          *
 * ================================================================== */
static void flash_erase_page(uint32_t addr) {
    FLASH->CTLR = CR_PAGE_ER;
    FLASH->ADDR = addr;
    FLASH->CTLR = CR_STRT_Set | CR_PAGE_ER;
    while (FLASH->STATR & FLASH_STATR_BSY);
}

/* ================================================================== *
 *  flash_write_page() — Write 64 bytes (16 words) to flash            *
 *                                                                     *
 *  The CH32V003 uses a 64-byte page buffer. Data is loaded word by    *
 *  word into the buffer, then committed in a single program operation.*
 *  Takes ~3ms for the commit. Total erase+write ≈ 6ms.               *
 * ================================================================== */
static void flash_write_page(uint32_t addr, const uint32_t *data) {
    volatile uint32_t *ptr = (volatile uint32_t *)addr;

    /* Set page programming mode and reset buffer */
    FLASH->CTLR = CR_PAGE_PG;
    FLASH->CTLR = CR_BUF_RST | CR_PAGE_PG;
    FLASH->ADDR = addr;
    while (FLASH->STATR & FLASH_STATR_BSY);

    /* Load 16 words into the page buffer */
    for (int i = 0; i < 16; i++) {
        ptr[i] = data[i];
        FLASH->CTLR = CR_PAGE_PG | CR_BUF_LOAD;
        while (FLASH->STATR & FLASH_STATR_BSY);
    }

    /* Commit buffer to flash */
    FLASH->CTLR = CR_PAGE_PG | CR_STRT_Set;
    while (FLASH->STATR & FLASH_STATR_BSY);
}

/* ================================================================== *
 *  nvm_init() — Load persistent state from flash at boot              *
 *                                                                     *
 *  Reads the flash page and checks the magic number. If valid,        *
 *  restores all persistent variables via nvm_unpack_state().           *
 *  If invalid (first boot, erased, or corrupted), leaves the          *
 *  defaults from dali_protocol.c unchanged.                           *
 * ================================================================== */
void nvm_init(void) {
    const dali_nvm_t *stored = (const dali_nvm_t *)NVM_FLASH_ADDR;

    if (stored->magic == NVM_MAGIC) {
        nvm_unpack_state(stored);
        printf("NVM: loaded addr=%d\n", stored->short_address);
    } else {
        printf("NVM: no valid data (magic=%08lX)\n",
               (unsigned long)stored->magic);
    }
}

/* ================================================================== *
 *  nvm_save() — Write current state to flash                          *
 *                                                                     *
 *  Packs the current DALI state into a dali_nvm_t struct, erases      *
 *  the flash page, and writes the new data. Total time ~6ms.          *
 * ================================================================== */
void nvm_save(void) {
    /* Pack current state into a page-sized buffer */
    union {
        dali_nvm_t nvm;
        uint32_t words[16];  /* 64 bytes = 16 × 32-bit words */
    } buf;

    /* Initialize reserved area to 0xFF (erased state) */
    memset(&buf, 0xFF, sizeof(buf));

    /* Pack state */
    buf.nvm.magic = NVM_MAGIC;
    nvm_pack_state(&buf.nvm);

    /* Write to flash */
    flash_unlock();
    flash_erase_page(NVM_FLASH_ADDR);
    flash_write_page(NVM_FLASH_ADDR, buf.words);
    flash_lock();

    nvm_dirty = 0;
    printf("NVM: saved addr=%d\n", buf.nvm.short_address);
}

/* ================================================================== *
 *  nvm_mark_dirty() — Flag that persistent state has changed          *
 * ================================================================== */
void nvm_mark_dirty(void) {
    nvm_dirty = 1;
    nvm_dirty_time = millis();
}

/* ================================================================== *
 *  nvm_tick() — Deferred save: write flash after timeout              *
 *                                                                     *
 *  Called from main loop. Saves if dirty flag is set and at least      *
 *  NVM_SAVE_DELAY_MS (5 seconds) have passed since the last change.   *
 *  This batches rapid config changes into a single flash write.        *
 * ================================================================== */
void nvm_tick(void) {
    if (!nvm_dirty) return;
    if (millis() - nvm_dirty_time < NVM_SAVE_DELAY_MS) return;
    nvm_save();
}

/* ================================================================== *
 *  NVM STATE PACK / UNPACK                                            *
 *                                                                     *
 *  Transfer state between the shared dali_device_state_t (ds) and     *
 *  the flash NVM struct. Called by nvm_save() and nvm_init().         *
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
    for (uint8_t i = 0; i < 4; i++)
        ds.colour_temp[i] = ds.colour_actual[i];
    ds.colour_tc = (nvm->colour_tc == 0xFFFF) ? 0 : nvm->colour_tc;
#endif
    if (nvm->ext_fade != 0xFF) {
        ds.ext_fade_base = nvm->ext_fade & 0x0F;
        ds.ext_fade_mult = (nvm->ext_fade >> 4) & 0x07;
    }
    ds.reset_state = 0;
}
