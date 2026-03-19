/*
 * DALI Bootloader for CH32V003 — Proof of Concept
 * Fits in 1920-byte boot area. Receives firmware via Manchester-encoded
 * frames on PC0 (DALI RX), programs user flash starting at 0x08000000.
 *
 * Clock: 24 MHz HSI (no PLL setup, saves code space)
 * Manchester: 1200 baud (DALI standard timing)
 * Protocol: standard DALI 16-bit frames addressed to device's short address.
 *           Address read from NVM page (0x08003FC0). Falls back to broadcast
 *           if NVM is invalid (fresh chip).
 *
 * Pin usage:
 *   PC0 — DALI RX (input, floating)
 *   PC5 — DALI TX (output, push-pull) for ACK/NAK responses
 *   PC7 — Boot button (input, pull-up, active low)
 */

#define SYSTEM_CORE_CLOCK 24000000
#define SYSTICK_USE_HCLK

#include "ch32v003fun.h"

/* Flash constants from ch32v003fun.h: CR_PAGE_ER, CR_PAGE_PG,
 * CR_BUF_LOAD, CR_BUF_RST, CR_STRT_Set, CR_LOCK_Set */

#define USER_CODE_BASE  0x08000000
#define PAGE_SIZE       64
#define NUM_PAGES       ((16384 - 1920) / PAGE_SIZE)  /* ~224 pages */

/* ── NVM page (last 64-byte flash page, shared with dali_nvm.c) ──
 *   Offset 0: uint32_t magic   = 0x44414C49 ("DALI")
 *   Offset 4: uint8_t  short_address (0-63, or 0xFF = unassigned)
 */
#define NVM_PAGE_ADDR   0x08003FC0
#define NVM_MAGIC       0x44414C49

/* ── Manchester timing at 24 MHz, 1200 baud ─────────────────────
 *   bit period  = 833.3 µs = 20000 cycles
 *   half-bit    = 416.7 µs = 10000 cycles
 */
#define BIT_CYCLES      20000
#define HALF_BIT_CYCLES 10000

/* ── Protocol ────────────────────────────────────────────────────
 *   Uses standard DALI 16-bit forward frames:
 *     Address byte = (short_addr << 1) | 1   (command addressing, S=1)
 *     Data byte    = bootloader command or firmware data
 *
 *   Other devices on the bus ignore these frames because they are
 *   addressed to this device's short address only.
 *   If NVM has no valid address, falls back to broadcast (0xFF).
 *
 *   Bootloader commands (data byte, vendor-specific reserved range 129-143):
 *     CMD_ENTER  0x83 (131) — enter bootloader mode (sent twice, config repeat)
 *     CMD_ERASE  0x84 (132) — erase all user flash
 *     CMD_DATA   0x85 (133) — next frame's data byte is a firmware byte
 *     CMD_COMMIT 0x86 (134) — write remaining partial page + lock flash
 *     CMD_BOOT   0x87 (135) — jump to user code
 *
 *   Data transfer: master sends CMD_DATA, then a second frame where
 *   the data byte IS the firmware byte. This avoids needing a separate
 *   command range for 256 possible data values.
 *
 *   Slave→Master (backward, 8 bits Manchester):
 *     ACK  0x01  — success / page programmed
 *     NAK  0x00  — error
 */
#define CMD_ENTER   0x83    /* 131 — vendor-specific reserved (IEC 62386-102 §9.5) */
#define CMD_ERASE   0x84    /* 132 — vendor-specific reserved */
#define CMD_DATA    0x85    /* 133 — vendor-specific reserved */
#define CMD_COMMIT  0x86    /* 134 — vendor-specific reserved */
#define CMD_BOOT    0x87    /* 135 — vendor-specific reserved */
#define RESP_ACK    0x01
#define RESP_NAK    0x00

/* Software-triggered boot: firmware writes this magic to RAM before reset.
 * RAM survives software system reset on CH32V003. */
#define BOOTLOADER_MAGIC_ADDR   ((volatile uint32_t *)0x200007F0)
#define BOOTLOADER_MAGIC_VALUE  0xDAB00DAD

/* ── GPIO helpers ───────────────────────────────────────────────── */
static inline int  bus_read(void) { return (GPIOC->INDR >> 0) & 1; } /* PC0 */
static inline int  btn_read(void) { return (GPIOC->INDR >> 7) & 1; } /* PC7 */
static inline void tx_high(void)  { GPIOC->BSHR = 1 << 5; }         /* PC5 = idle */
static inline void tx_low(void)   { GPIOC->BSHR = 1 << (5 + 16); }  /* PC5 = active */

/* ── Delay using SysTick (counts up at HCLK) ────────────────────── */
static inline void delay(uint32_t cycles) {
    uint32_t start = SysTick->CNT;
    while ((uint32_t)(SysTick->CNT - start) < cycles);
}

/* ── Manchester RX: polling decoder ─────────────────────────────── */

/*
 * Wait for bus to go idle (high) then detect falling edge (start bit).
 * Returns 1 on edge detected, 0 on timeout.
 */
static int wait_start(uint32_t timeout) {
    /* Wait for idle (high) */
    while (!bus_read()) { if (--timeout == 0) return 0; }
    /* Wait for falling edge (high→low = start of start bit) */
    while (bus_read())  { if (--timeout == 0) return 0; }
    return 1;
}

/*
 * Decode 16-bit Manchester frame after start bit falling edge detected.
 *
 * Sampling strategy: after the falling edge of the start bit,
 * wait 1.25 bit periods to land in the middle of the first data bit's
 * second half-bit. Then sample every bit period for remaining 15 bits.
 *
 * NO_PHY Manchester encoding:
 *   bit 1 = LOW→HIGH (active→idle): second half is HIGH → sample reads 1
 *   bit 0 = HIGH→LOW (idle→active): second half is LOW  → sample reads 0
 */
static int rx_frame(uint16_t *out) {
    delay(BIT_CYCLES + HALF_BIT_CYCLES + HALF_BIT_CYCLES / 2);  /* 1.75 bit periods — sample middle of 2nd half-bit */

    uint16_t data = 0;
    for (int i = 0; i < 16; i++) {
        data = (data << 1) | bus_read();
        if (i < 15) delay(BIT_CYCLES);
    }

    *out = data;
    return 1;
}

/* ── Manchester TX: bit-bang 8-bit backward frame ───────────────── */
/*
 * DALI backward frame: start bit (1) + 8 data bits, MSB first.
 * Sent after a settling time (~7 Te forward-to-backward).
 * NO_PHY: bit 1 = LOW→HIGH, bit 0 = HIGH→LOW.
 */
static void tx_byte(uint8_t val) {
    /* Settling time before backward frame (~6ms) */
    delay(BIT_CYCLES * 7);

    /* Start bit: always 1 (LOW→HIGH) */
    tx_low();  delay(HALF_BIT_CYCLES);
    tx_high(); delay(HALF_BIT_CYCLES);

    /* 8 data bits MSB first */
    for (int i = 7; i >= 0; i--) {
        if (val & (1 << i)) {
            tx_low();  delay(HALF_BIT_CYCLES);
            tx_high(); delay(HALF_BIT_CYCLES);
        } else {
            tx_high(); delay(HALF_BIT_CYCLES);
            tx_low();  delay(HALF_BIT_CYCLES);
        }
    }

    /* Stop: return to idle + inter-frame gap */
    tx_high();
    delay(BIT_CYCLES * 4);
}

/* ── Flash programming (register-level, no HAL) ────────────────── */

static void flash_unlock(void) {
    FLASH->KEYR = 0x45670123;
    FLASH->KEYR = 0xCDEF89AB;
    FLASH->MODEKEYR = 0x45670123;
    FLASH->MODEKEYR = 0xCDEF89AB;
}

static void flash_lock(void) {
    FLASH->CTLR = CR_LOCK_Set;
}

static void flash_erase_page(uint32_t addr) {
    FLASH->CTLR = CR_PAGE_ER;
    FLASH->ADDR = addr;
    FLASH->CTLR = CR_STRT_Set | CR_PAGE_ER;
    while (FLASH->STATR & FLASH_STATR_BSY);
    FLASH->CTLR = 0;
}

static void flash_write_page(uint32_t addr, uint32_t *buf) {
    FLASH->CTLR = CR_PAGE_PG;
    FLASH->CTLR = CR_BUF_RST | CR_PAGE_PG;
    FLASH->ADDR = addr;
    while (FLASH->STATR & FLASH_STATR_BSY);

    volatile uint32_t *dst = (volatile uint32_t *)addr;
    for (int i = 0; i < 16; i++) {   /* 16 × 4 = 64 bytes */
        dst[i] = buf[i];
        FLASH->CTLR = CR_PAGE_PG | CR_BUF_LOAD;
        while (FLASH->STATR & FLASH_STATR_BSY);
    }

    FLASH->CTLR = CR_PAGE_PG | CR_STRT_Set;
    while (FLASH->STATR & FLASH_STATR_BSY);
    FLASH->CTLR = 0;
}

/* ── Boot to user code ──────────────────────────────────────────── */
static void boot_usercode(void) {
    FLASH->BOOT_MODEKEYR = FLASH_KEY1;
    FLASH->BOOT_MODEKEYR = FLASH_KEY2;
    FLASH->STATR = 0;   /* bit 14 = 0 → boot user code */
    FLASH->CTLR = CR_LOCK_Set;
    PFIC->SCTLR = 1 << 31;  /* system reset */
}

/* ── Main ───────────────────────────────────────────────────────── */
int main(void) {
    /* Reset clock to HSI 24 MHz — PLL may still be active after software reset.
     * The PFIC system reset does NOT always reset RCC on CH32V003. */
    RCC->CTLR |= (1 << 0);           /* Ensure HSI is on */
    RCC->CFGR0 = 0;                   /* SW=00 → HSI as system clock, no prescalers */
    while (RCC->CFGR0 & 0x0C) {}      /* Wait until SWS=00 (HSI active) */
    RCC->CTLR &= ~(1 << 24);         /* Disable PLL */

    /* Enable SysTick at HCLK (24 MHz) */
    SysTick->CTLR = 5;

    /* Enable GPIOC clock */
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOC;

    /*
     * PC0 = floating input   (DALI RX)     — CNF=01, MODE=00
     * PC5 = push-pull output (DALI TX)     — CNF=00, MODE=11
     * PC7 = input pull-up    (boot button) — CNF=10, MODE=00
     * Leave other pins at reset default (input floating = 0x4).
     */
    GPIOC->CFGLR = 0x44444444;                        /* all floating input (reset) */
    GPIOC->CFGLR &= ~((0xFu << (5*4)) | (0xFu << (7*4)));
    GPIOC->CFGLR |= ((GPIO_Speed_50MHz | GPIO_CNF_OUT_PP) << (5*4))
                   | ((GPIO_Speed_In | GPIO_CNF_IN_PUPD) << (7*4));
    GPIOC->BSHR = (1 << 7);  /* PC7 pull-UP */
    tx_high();               /* TX idle */

    /*
     * Boot decision:
     * 1. Check RAM magic word (set by firmware's ENTER BOOTLOADER command)
     * 2. Check PC7 button (held LOW = enter bootloader)
     * If neither, jump to user code.
     */
    uint8_t sw_boot = (*BOOTLOADER_MAGIC_ADDR == BOOTLOADER_MAGIC_VALUE);
    *BOOTLOADER_MAGIC_ADDR = 0;  /* always clear to prevent boot loop */

    delay(24000);  /* ~1ms at 24 MHz for pull-up settle */
    if (!sw_boot && btn_read()) {
        boot_usercode();
        /* never returns */
    }

    /* ── Read device address from NVM ──────────────────────────── */
    /*
     * NVM page layout: [magic:4][short_address:1][...]
     * If magic is valid, use stored short address.
     * Otherwise fall back to broadcast (0xFF = accept all).
     */
    volatile uint32_t *nvm = (volatile uint32_t *)NVM_PAGE_ADDR;
    uint8_t my_addr_byte;  /* DALI address byte to match (upper byte of frame) */

    if (nvm[0] == NVM_MAGIC) {
        uint8_t short_addr = ((volatile uint8_t *)NVM_PAGE_ADDR)[4];
        if (short_addr <= 63)
            my_addr_byte = (short_addr << 1) | 1;  /* command addressing: 0AAAAAA1 (S=1) */
        else
            my_addr_byte = 0xFF;  /* unassigned → broadcast */
    } else {
        my_addr_byte = 0xFF;  /* no valid NVM → broadcast */
    }

    /* ── Bootloader active — wait for commands ─────────────────── */

    uint8_t  page_buf[PAGE_SIZE] __attribute__((aligned(4)));
    uint8_t  buf_pos = 0;
    uint32_t write_addr = USER_CODE_BASE;
    uint8_t  awaiting_data = 0;  /* 1 = next frame's data byte is firmware data */
    uint8_t  flash_unlocked = 0; /* 1 = flash erased and unlocked, safe to write */

    while (1) {
        uint16_t frame;
        if (!wait_start(0x00FFFFFF))
            continue;
        if (!rx_frame(&frame))
            continue;

        uint8_t addr = frame >> 8;
        uint8_t data = frame & 0xFF;

        /* Address filter: only accept frames for our address or broadcast */
        if (my_addr_byte != 0xFF && addr != my_addr_byte && addr != 0xFF)
            continue;

        /* Data transfer mode: this frame's data byte is firmware data */
        if (awaiting_data) {
            awaiting_data = 0;
            if (!flash_unlocked || write_addr >= NVM_PAGE_ADDR) {
                tx_byte(RESP_NAK);
                continue;
            }
            page_buf[buf_pos++] = data;
            if (buf_pos >= PAGE_SIZE) {
                flash_write_page(write_addr, (uint32_t *)page_buf);
                write_addr += PAGE_SIZE;
                buf_pos = 0;
                tx_byte(RESP_ACK);  /* ACK per page (every 64 bytes) */
            }
            continue;
        }

        /* Command dispatch */
        switch (data) {
        case CMD_ERASE:
            tx_byte(RESP_ACK);  /* ACK first — erase takes ~1s, too late for master */
            flash_unlock();
            for (int p = 0; p < NUM_PAGES; p++)
                flash_erase_page(USER_CODE_BASE + p * PAGE_SIZE);
            buf_pos = 0;
            write_addr = USER_CODE_BASE;
            flash_unlocked = 1;
            break;

        case CMD_DATA:
            awaiting_data = 1;  /* next frame carries the firmware byte */
            break;

        case CMD_COMMIT:
            if (buf_pos > 0 && flash_unlocked && write_addr < NVM_PAGE_ADDR) {
                /* Pad with 0xFF — avoid memset (not linked in bootloader) */
                for (uint8_t p = buf_pos; p < PAGE_SIZE; p++)
                    page_buf[p] = 0xFF;
                flash_write_page(write_addr, (uint32_t *)page_buf);
            }
            flash_lock();
            flash_unlocked = 0;
            tx_byte(RESP_ACK);
            break;

        case CMD_BOOT:
            tx_byte(RESP_ACK);
            delay(BIT_CYCLES * 8);
            boot_usercode();
            break;

        default:
            break;
        }
    }
}
