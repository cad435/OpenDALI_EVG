/*
 * DALI Bootloader Emergency Flasher — CH32V003
 *
 * Single-purpose firmware: replaces the DALI bootloader in the BOOT area
 * (0x1FFFF000, 1920 B). It is deployed over the DALI bus through the
 * EXISTING bootloader like any normal firmware update, runs once, and is
 * then overwritten again by the regular application firmware.
 *
 * Being flashed IS the trigger — there is no command to send. On boot it
 * verifies the embedded image, writes the boot area, verifies the result
 * by read-back, and reports on UART (PD5, 115200).
 *
 * The bootloader image is compiled in (src/bl_image.h, generated from
 * ../Bootloader by embed_bootloader.py), so nothing has to travel over the
 * bus twice and no EEPROM staging is involved.
 *
 * ────────────────────────────────────────────────────────────────────────
 * THE ONE THING THAT MAKES THIS WORK — and the reason it does not belong
 * in the application firmware:
 *
 * Writing the BOOT area from user code requires an ACTIVE DEBUG MODULE.
 * Without it, FPEC erase/program on 0x1FFFF000 are SILENT NO-OPS: EOP is
 * set, WRPRTERR stays clear, and the flash simply does not change. Proven
 * on hardware 2026-08-15 — the identical code failed for hours until
 * `wlink sdi-print enable` (which keeps the DM alive) was running, after
 * which it succeeded on the first try and verified byte-for-byte via SWD.
 * Unlock order, interrupt state and RAM execution were all ruled out.
 *
 * So this firmware must enable the debug module itself: the CH32V003 can
 * bit-bang its own single-wire debug protocol on PD1 and write DMCONTROL /
 * DMCFGR — see swio_self_unlock() below.
 * ──────────────────────────────────────────────────────────────────────── */

#include "ch32fun.h"
#include <stdio.h>
#include "bl_image.h"
#include "swio.h"

#define BL_AREA_BASE   0x1FFFF000u
#define BL_AREA_SIZE   1920u
#define BL_PAGE_SIZE   64u

#define FLASH_KEY1     0x45670123u
#define FLASH_KEY2     0xCDEF89ABu

/* ── Step 1: activate our own debug module ───────────────────────────
 * Bit-bangs the single-wire debug protocol on PD1 against this chip's own
 * debug module (see swio.h). Sequence follows attempt_unlock() from
 * monte-monte/ch32_user_bootloader_flasher — 0x5AA5.... is WCH's config
 * unlock key, bit 10 enables output from the slave.
 *
 * ONE DELIBERATE DEVIATION: the reference writes DMCONTROL with haltreq
 * (0x80000001) and then resumereq (0x40000001). That is safe only with a
 * host attached that can resume us; halting ourselves here would leave the
 * device stuck, since a halted hart cannot send its own resume. We only
 * need dmactive (bit 0), so haltreq is omitted. */
static void swio_self_unlock(void) {
    swio_begin();
    swio_write_reg32(DMSHDWCFGR, 0x5AA50000u | (1u << 10));
    swio_begin();
    swio_write_reg32(DMCFGR,     0x5AA50000u | (1u << 10));
    swio_begin();
    swio_write_reg32(DMSHDWCFGR, 0x5AA50000u | (1u << 10));
    swio_begin();
    swio_write_reg32(DMCFGR,     0x5AA50000u | (1u << 10));
    swio_begin();
    swio_write_reg32(DMABSTRACTAUTO, 0x00000000u);
    swio_begin();
    swio_write_reg32(DMCONTROL, 0x00000001u | (1u << 10));   /* dmactive */
}

/* ── Flash primitives (FPEC fast mode, 64-byte pages) ────────────────
 * Unlock order matters: FPEC, then BOOT area, then fast mode. */
static void flash_unlock_boot(void) {
    FLASH->KEYR          = FLASH_KEY1;
    FLASH->KEYR          = FLASH_KEY2;
    FLASH->BOOT_MODEKEYR = FLASH_KEY1;
    FLASH->BOOT_MODEKEYR = FLASH_KEY2;
    FLASH->MODEKEYR      = FLASH_KEY1;
    FLASH->MODEKEYR      = FLASH_KEY2;
}

static void flash_relock(void) {
    FLASH->CTLR  = CR_LOCK_Set | (1 << 15);   /* LOCK + FLOCK */
    FLASH->STATR = (1 << 15);                 /* BOOT lock (write-1-set) */
}

static void flash_erase_page64(uint32_t addr) {
    FLASH->CTLR = CR_PAGE_ER;
    FLASH->ADDR = addr;
    FLASH->CTLR = CR_STRT_Set | CR_PAGE_ER;
    while (FLASH->STATR & FLASH_STATR_BSY);
    FLASH->CTLR = 0;
}

static void flash_write_page64(uint32_t addr, const uint32_t *src) {
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

/* Fletcher-16 over the embedded image — checked BEFORE the first erase so
 * a corrupted build can never leave the device without a bootloader. */
static int image_ok(void) {
    uint8_t fa = 0, fb = 0;
    for (unsigned i = 0; i < BL_IMAGE_SIZE; i++) {
        fa += bl_image[i];
        fb += fa;
    }
    return fa == BL_IMAGE_FLETCHER_FA && fb == BL_IMAGE_FLETCHER_FB;
}

static int already_installed(void) {
    const uint8_t *fl = (const uint8_t *)BL_AREA_BASE;
    for (unsigned i = 0; i < BL_IMAGE_SIZE; i++)
        if (fl[i] != bl_image[i]) return 0;
    return 1;
}

/* Erase the whole boot area, program the embedded image, verify by
 * read-back. Interrupts stay off for the whole session. Returns 1 on a
 * verified success. */
static int write_bootloader(void) {
    uint8_t page[BL_PAGE_SIZE] __attribute__((aligned(4)));

    __disable_irq();
    flash_unlock_boot();

    for (uint32_t a = BL_AREA_BASE; a < BL_AREA_BASE + BL_AREA_SIZE; a += BL_PAGE_SIZE)
        flash_erase_page64(a);

    for (unsigned off = 0; off < BL_IMAGE_SIZE; off += BL_PAGE_SIZE) {
        for (unsigned i = 0; i < BL_PAGE_SIZE; i++) {
            unsigned idx = off + i;
            page[i] = (idx < BL_IMAGE_SIZE) ? bl_image[idx] : 0xFF;
        }
        flash_write_page64(BL_AREA_BASE + off, (const uint32_t *)page);
    }

    flash_relock();
    __enable_irq();

    return already_installed();
}

int main(void) {
    SystemInit();
    Delay_Ms(3000);          /* let a UART terminal attach before we report */

    printf("\n=== DALI BL emergency flasher ===\n");
    printf("embedded image: %u bytes (fa=%02X fb=%02X)\n",
           (unsigned)BL_IMAGE_SIZE, BL_IMAGE_FLETCHER_FA, BL_IMAGE_FLETCHER_FB);

    if (!image_ok()) {
        printf("ABORT: embedded image failed its own checksum\n");
        while (1) { printf("BL FLASH ABORTED\n"); Delay_Ms(2000); }
    }

    if (already_installed()) {
        printf("boot area already matches — nothing to do\n");
        while (1) { printf("BL FLASH OK (was current)\n"); Delay_Ms(2000); }
    }

    /* Retry: the debug module occasionally needs a second nudge, and this
     * also covers the fallback of enabling it externally with a WCH-Link
     * (`wlink sdi-print enable`) while we are already running. */
    int ok = 0;
    for (int attempt = 1; attempt <= 5 && !ok; attempt++) {
        printf("attempt %d: enabling debug module via SWIO...\n", attempt);
        swio_self_unlock();
        ok = write_bootloader();
        printf("  -> %s\n", ok ? "VERIFIED" : "boot area unchanged");
        if (!ok) Delay_Ms(2000);
    }

    /* Repeat the verdict forever so it can be read at any time. On failure
     * the old bootloader is still in place — nothing is bricked. */
    while (1) {
        printf("BL FLASH %s\n", ok ? "OK" : "FAILED");
        Delay_Ms(2000);
    }
}
