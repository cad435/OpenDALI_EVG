/*
    dali_bl_update.c - Bootloader update over the DALI bus (vendor extension)

    Rewrites the DALI bootloader in the BOOT area (0x1FFFF000, 1920 B)
    from the running user firmware. The bootloader is never entered —
    it is only the write target. The app keeps running during the whole
    transfer (PWM, fades and normal 16-bit DALI commands stay live).

    Wire protocol (32-bit forward frames, mirrors IEC 62386-105 so the
    EVG-Updater transfer machinery can be reused):

      [addr] FB 10 00   START BL UPDATE (vendor cmd, config repeat x2)
                         -> answer YES, arm the receive engine
      CB bb bb bb       BEGIN BLOCK   (0 = info block, !=0 = BL payload)
      BD d1 d2 d3       TRANSFER BLOCK DATA (3 payload bytes)
      BF FB 03 00       FINISH (config repeat x2) -> verify Fletcher ->
                         flash BOOT area -> readback verify (3 attempts)
                         -> silence = success, 0xFF = fault
      BF FB 08 00       QUERY BLOCK FAULT -> 0xFF if fault

    Block 0 layout (identical to the app-update Block 0): GTIN at pos
    5..10 (checked against compile-time DALI_GTIN), device key at 0x2B
    must be 0xB0 ("bootloader" — deliberately NOT an EVG mode ID, so an
    app image can never be flashed into the boot area), expected
    Fletcher-16 of the payload at 0x2C/0x2D.
    Block 1: pos 0-1 = payload size (big endian), payload from pos 15.
    Payload larger than 1920 B is rejected (also auto-rejects app
    images, which are ~11 KB).

    BRICK WINDOW (accepted by design, 2026-06-08): option byte
    START_MODE=1 boots the BOOT area at every POR. Power loss inside the
    ~0.5 s erase+program window leaves a corrupt bootloader -> the next
    POR executes garbage -> SWIO is the only recovery. Mitigations: the
    image is fully staged in EEPROM and Fletcher-verified BEFORE the
    first erase, the write loop is tight (no bus waits), and the result
    is readback-verified with up to 3 write attempts. BL updates are
    rare and only happen on explicit command.
*/

#include "ch32fun.h"
#include "dali_bl_update.h"
#include "dali_config_repeat.h"
#include "../dali_state.h"
#include "../phy/dali_phy.h"
#include "../../eeprom/eeprom.h"
#include "../../eeprom/eeprom_layout.h"
#include "../../config/config.h"
#include "../../logger.h"

/* millis() provided by main.c */
extern uint32_t millis(void);

#define BL_AREA_BASE    0x1FFFF000u
#define BL_AREA_SIZE    1920u
#define BL_PAGE_SIZE    64u

#define BL_CMD_START    0x10    /* vendor: START BL UPDATE (BL ignores it) */
#define BL_DEVICE_KEY   0xB0    /* Block-0 key for "bootloader" target */
#define BL_TIMEOUT_MS   30000u  /* disarm if the transfer stalls */

/* Split DALI_GTIN (48-bit) into 6 bytes, MSB first */
#define GTIN_B(n)   ((uint8_t)((DALI_GTIN >> (40 - 8*(n))) & 0xFF))
static const uint8_t my_gtin[6] = {
    GTIN_B(0), GTIN_B(1), GTIN_B(2), GTIN_B(3), GTIN_B(4), GTIN_B(5)
};

/* ── Receive engine state (module-private) ───────────────────────── */
static uint8_t  armed;
static uint8_t  fault;
static uint32_t cur_block;
static uint16_t block_byte;         /* byte position within current block */
static uint16_t block_size;         /* Block-1 payload size from header */
static uint16_t img_size;           /* bytes staged to EEPROM so far */
static uint8_t  fla, flb;           /* Fletcher-16 accumulator */
static uint8_t  exp_fa, exp_fb;     /* expected Fletcher from Block 0 */
static uint8_t  buf[BL_PAGE_SIZE] __attribute__((aligned(4)));
static uint8_t  buf_pos;
static uint16_t ee_addr;
static uint32_t last_frame_ms;
static uint32_t result_until;       /* post-FINISH window: QUERY BLOCK FAULT
                                       still answered so the master can fetch
                                       the flash result deterministically */

/* ── EEPROM staging ──────────────────────────────────────────────── */
static void stage_flush(void) {
    if (buf_pos == 0) return;
    if (!eeprom_write(ee_addr, buf, buf_pos))
        fault = 1;
    ee_addr  += buf_pos;
    img_size += buf_pos;
    buf_pos = 0;
}

/* ── BOOT-area flash writer ──────────────────────────────────────────
 * Same FPEC fast-mode operations the bootloader uses on user flash,
 * plus the third unlock layer: FLASH_BOOT_MODEKEYR opens the BOOT area
 * (RM V1.9 §16.3.10).
 *
 * These routines MUST execute from RAM. While an info-block page
 * erase/program runs (~3 ms) the flash macro is unreadable; an
 * instruction fetch into it silently aborts the operation — EOP is
 * still set, no error flag is raised, and the contents stay unchanged.
 * That is exactly the failure seen on hardware while these functions
 * lived in main flash (2026-08-15). .data has its LMA in flash and its
 * VMA in RAM (ch32fun.ld) and is copied by the startup code, so placing
 * the routines there makes them RAM-resident. Callers must additionally
 * keep interrupts disabled — the vector table is in flash. */
/* NOTE: section(".data") — i.e. RAM execution — was tested on 2026-08-15
 * and did NOT enable boot-area writes: erase stayed a no-op and the
 * program step hung the flash macro (BSY never cleared, which also
 * blocks the debug module). Kept flash-resident for graceful failure. */
#define RAMFUNC __attribute__((noinline))

static RAMFUNC void flash_erase_boot_area(void) {
    for (uint32_t addr = BL_AREA_BASE;
         addr < BL_AREA_BASE + BL_AREA_SIZE;
         addr += BL_PAGE_SIZE) {
        FLASH->CTLR = CR_PAGE_ER;
        FLASH->ADDR = addr;
        FLASH->CTLR = CR_STRT_Set | CR_PAGE_ER;
        while (FLASH->STATR & FLASH_STATR_BSY);
        FLASH->CTLR = 0;
    }
}

static RAMFUNC void flash_write_page64(uint32_t addr, const uint32_t *src) {
    FLASH->CTLR = CR_PAGE_PG;
    FLASH->CTLR = CR_BUF_RST | CR_PAGE_PG;
    FLASH->ADDR = addr;
    while (FLASH->STATR & FLASH_STATR_BSY);
    volatile uint32_t *dst = (volatile uint32_t *)addr;
    for (int i = 0; i < 16; i++) {
        dst[i] = src[i];
        FLASH->CTLR = CR_PAGE_PG | CR_BUF_LOAD;
        while (FLASH->STATR & FLASH_STATR_BSY);
    }
    FLASH->CTLR = CR_PAGE_PG | CR_STRT_Set;
    while (FLASH->STATR & FLASH_STATR_BSY);
    FLASH->CTLR = 0;
}

static void flash_relock(void) {
    FLASH->CTLR  = CR_LOCK_Set | (1 << 15);     /* LOCK + FLOCK */
    FLASH->STATR = (1 << 15);                   /* BOOT lock (write-1-set;
                                                   EOP/WRPRTERR unaffected
                                                   by writing 0) */
}

/* Erase the whole BOOT area, program the staged image from EEPROM,
 * then readback-verify byte-for-byte against the staging copy.
 * Returns 1 on verified success. */
static uint8_t program_and_verify(void) {
    /* Interrupts MUST be off for the whole boot-area session: an ISR's
     * code fetch from main flash during an info-block erase/program
     * silently aborts the operation (EOP still set, no error flag, flash
     * unchanged). SysTick alone (1 kHz) guarantees a hit during every
     * ~3 ms page op. The working STATR[14] path (enter_bootloader) has
     * always run under __disable_irq() — this one needs it too. */
    __disable_irq();
    /* Unlock all three layers: FPEC, BOOT area, fast mode — IN THIS ORDER.
     * The BOOT unlock MUST come before the fast-mode (MODEKEYR) unlock:
     * with MODEKEYR first, the BOOT_MODEKEYR sequence is silently ignored
     * and boot-area erase/program become no-ops (EOP set, no WRPRTERR,
     * flash unchanged — verified on real HW 2026-08-15). Undocumented;
     * order taken from monte-monte/ch32_user_bootloader_flasher, the
     * community-proven user-code BL flasher. */
    FLASH->KEYR          = FLASH_KEY1;
    FLASH->KEYR          = FLASH_KEY2;
    FLASH->BOOT_MODEKEYR = FLASH_KEY1;
    FLASH->BOOT_MODEKEYR = FLASH_KEY2;
    FLASH->MODEKEYR      = FLASH_KEY1;
    FLASH->MODEKEYR      = FLASH_KEY2;

    LOG_CMD("BLW ct=%x st=%x", FLASH->CTLR, FLASH->STATR);

    flash_erase_boot_area();

    LOG_CMD("BLW post-erase [0]=%x", *(volatile uint32_t *)BL_AREA_BASE);

    uint16_t off = 0;
    while (off < img_size) {
        uint8_t chunk = (img_size - off >= BL_PAGE_SIZE)
                      ? BL_PAGE_SIZE : (uint8_t)(img_size - off);
        if (!eeprom_read(EE_ADDR_FW_STAGING + off, buf, chunk))
            goto fail;
        for (uint8_t i = chunk; i < BL_PAGE_SIZE; i++)
            buf[i] = 0xFF;
        flash_write_page64(BL_AREA_BASE + off, (const uint32_t *)buf);
        off += chunk;
    }

    /* Readback verify against the EEPROM staging copy */
    for (uint16_t o = 0; o < img_size; o += BL_PAGE_SIZE) {
        uint8_t chunk = (img_size - o >= BL_PAGE_SIZE)
                      ? BL_PAGE_SIZE : (uint8_t)(img_size - o);
        if (!eeprom_read(EE_ADDR_FW_STAGING + o, buf, chunk))
            goto fail;
        const uint8_t *fl = (const uint8_t *)(BL_AREA_BASE + o);
        for (uint8_t i = 0; i < chunk; i++)
            if (fl[i] != buf[i]) {
                LOG_CMD("BLW vfy@%d fl=%x exp=%x", o + i, fl[i], buf[i]);
                goto fail;
            }
    }
    flash_relock();
    __enable_irq();
    return 1;
fail:
    flash_relock();
    __enable_irq();
    return 0;
}

/* ── FINISH handler ──────────────────────────────────────────────── */
static void finish(void) {
    armed = 0;
    stage_flush();

    if (fault || img_size == 0 || img_size > BL_AREA_SIZE
        || fla != exp_fa || flb != exp_fb) {
        fault = 1;
        result_until = millis() + 30000u;
        dali_phy_send_backward(0xFF);           /* fault — BL untouched */
        LOG_CMD("BL_UPD fault (size=%d)", img_size);
        return;
    }

    /* Image complete and Fletcher-verified in EEPROM — critical window
     * starts here. Up to 3 write+verify attempts. */
    uint8_t ok = 0;
    for (uint8_t attempt = 0; attempt < 3 && !ok; attempt++)
        ok = program_and_verify();

    if (!ok) {
        fault = 1;                  /* queryable in the result window */
        dali_phy_send_backward(0xFF);
        LOG_CMD("BL_UPD FLASH FAIL");
    } else {
        LOG_CMD("BL_UPD OK (%d B)", img_size);
        /* silence = success (same FINISH semantics as the app update) */
    }
    result_until = millis() + 30000u;
}

/* ── Public: 32-bit frame hook ───────────────────────────────────── */
uint8_t dali_bl_update_process(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3) {
    uint32_t now = millis();

    /* START BL UPDATE — addressed, config repeat (2x within 100 ms) */
    if (b1 == 0xFB && b2 == BL_CMD_START && b3 == 0x00) {
        uint8_t my = (ds.short_address <= 63) ? (ds.short_address << 1) : 0xFE;
        if (b0 != my && b0 != 0xFE && b0 != 0xFC)
            return 0;
        if (dali_check_config_repeat(b0, b2, now)) {
            armed = 1;  fault = 0;
            cur_block = 0;  block_byte = 0;  block_size = 0;
            img_size = 0;  fla = flb = 0;  exp_fa = exp_fb = 0;
            buf_pos = 0;  ee_addr = EE_ADDR_FW_STAGING;
            last_frame_ms = now;
            dali_phy_send_backward(0xFF);       /* YES — armed */
            LOG_CMD("BL_UPD armed");
        }
        return 1;
    }

    /* QUERY BLOCK FAULT — answered while armed AND inside the post-FINISH
     * result window, so the master can fetch the flash result even after
     * the engine disarmed. Outside both: not ours (another device's real
     * bootloader might be the addressee). */
    if (b0 == 0xBF && b1 == 0xFB && b2 == 0x08) {
        if (armed || (int32_t)(result_until - now) > 0) {
            if (fault) dali_phy_send_backward(0xFF);
            return 1;
        }
        return 0;
    }

    if (!armed)
        return 0;

    /* Stalled transfer -> disarm, let the frame process normally */
    if (now - last_frame_ms > BL_TIMEOUT_MS) {
        armed = 0;
        LOG_CMD("BL_UPD timeout");
        return 0;
    }
    last_frame_ms = now;

    /* BEGIN BLOCK (0xCB) — only zero/non-zero of the block number matters */
    if (b0 == 0xCB) {
        cur_block = (uint32_t)b1 | b2 | b3;
        block_byte = 0;
        block_size = 0;
        if (cur_block != 0) {
            fault = 0;
            fla = flb = 0;
            buf_pos = 0;  ee_addr = EE_ADDR_FW_STAGING;  img_size = 0;
        }
        return 1;
    }

    /* TRANSFER BLOCK DATA (0xBD) — 3 payload bytes */
    if (b0 == 0xBD) {
        uint32_t payload = ((uint32_t)b1 << 16) | ((uint32_t)b2 << 8) | b3;
        for (int sh = 16; sh >= 0; sh -= 8) {
            uint16_t pos = block_byte++;
            uint8_t  d   = (uint8_t)(payload >> sh);

            if (cur_block == 0) {
                /* Block 0: GTIN (5..10), device key (0x2B), Fletcher (0x2C/2D) */
                if (pos >= 5 && pos <= 10) {
                    if (d != my_gtin[pos - 5]) fault = 1;
                } else if (pos == 0x2B) {
                    if (d != BL_DEVICE_KEY) fault = 1;
                } else if (pos == 0x2C) exp_fa = d;
                else if   (pos == 0x2D) exp_fb = d;
            } else {
                /* Block 1: size header (pos 0-1), payload from pos 15 */
                if (pos < 2) {
                    block_size = (uint16_t)((block_size << 8) | d);
                } else if (pos >= 15 && pos < block_size + 15) {
                    if ((uint16_t)(img_size + buf_pos) >= BL_AREA_SIZE) {
                        fault = 1;          /* oversize — not a BL image */
                    } else {
                        buf[buf_pos++] = d;
                        fla += d;  flb += fla;
                        if (buf_pos >= BL_PAGE_SIZE)
                            stage_flush();
                    }
                }
            }
        }
        return 1;
    }

    /* Update-global standard commands (0xBF-addressed) while armed.
     * (QUERY BLOCK FAULT is handled above, including the result window.) */
    if (b0 == 0xBF && b1 == 0xFB) {
        if (b2 == 0x03) {                       /* FINISH (config repeat) */
            if (dali_check_config_repeat(b0, b2, now))
                finish();
        }
        return 1;
    }

    return 0;
}
