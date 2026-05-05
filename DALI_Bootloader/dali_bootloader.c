/*
 * DALI Bootloader for CH32V003 — IEC 62386-105 compatible
 *
 * Accepts firmware updates from standard DALI masters using 32-bit forward
 * frames (IEC 62386-105). Responds to standard commands (START FW TRANSFER,
 * FINISH FW UPDATE, etc.) so it looks compliant from the outside.
 *
 * Block parsing: extracts firmware data from Block 1..n, skips Block 0
 * (info block), block headers, and trailing CRCs. No CRC verification
 * is performed — integrity is trusted or verified externally.
 *
 * Flow:
 *   1. START FW TRANSFER → enter update mode, respond YES
 *   2. BEGIN BLOCK / TRANSFER BLOCK DATA → store firmware bytes in EEPROM
 *   3. FINISH FW UPDATE → copy EEPROM → flash, respond success (silence)
 *   4. RESTART FW → jump to user code
 *
 * Hardware:
 *   PC1 — I2C SDA (AT24C256 EEPROM)
 *   PC2 — I2C SCL
 *   PC3 — DALI RX (via PHY)
 *   PC4 — DALI TX (via PHY)
 *   PA1 — Boot button (active low)
 *
 * Clock: 24 MHz HSI, Manchester 1200 baud
 *
 * ENTRY CONTRACT — IMPORTANT:
 *   This bootloader MUST ONLY be entered after a full system reset
 *   (PFIC->SCTLR bit 31 = SYSRESETREQ, hardware NRST, or power-on).
 *   It assumes power-on default state for all peripherals — clock at
 *   HSI 24 MHz with no PLL, I2C1 off in reset state, GPIOs floating.
 *   The init code skips redundant resets and clock-switch waits to fit
 *   in the 1920-byte boot area.
 *
 *   DO NOT enter via direct jump (e.g. `goto *(void(*)())0x1FFFF000`).
 *   That would bypass the reset and leave the chip running at the user-
 *   firmware's clock (48 MHz HSI+PLL) — Manchester timing would be 2x
 *   too fast and the DALI bus would not work. I2C1 would also be in
 *   whatever state the user firmware left it in.
 *
 *   Both legitimate entry paths are reset-mediated:
 *     - cold boot          → power-on reset
 *     - software-triggered → firmware sets FLASH->STATR bit 14, then
 *                            triggers PFIC->SCTLR bit 31 (SYSRESETREQ)
 */

#define SYSTEM_CORE_CLOCK 24000000
#define SYSTICK_USE_HCLK
#include "ch32v003fun.h"

#define USER_CODE_BASE  0x08000000
#define PAGE_SIZE       64
#define NUM_PAGES       ((16384 - 1920) / PAGE_SIZE)

#define BIT_CYCLES      20000
#define HALF_BIT_CYCLES 10000

/* IEC 62386-105 frame identifiers */
#define OP_STANDARD     0xFB
#define OP_BEGIN_BLOCK  0xCB
#define OP_BLOCK_DATA   0xBD

/* Standard command codes (opcode byte 2) */
#define CMD_START           0x00
#define CMD_RESTART         0x01
#define CMD_ENABLE_RESTART  0x02
#define CMD_FINISH          0x03
#define CMD_Q_FEATURES      0x05
#define CMD_Q_RUNNING       0x07
#define CMD_Q_BLOCK_FAULT   0x08

/* 32-bit frame addresses */
#define ADDR_BROADCAST      0xFE
#define ADDR_BROADCAST_UA   0xFC
#define ADDR_FW_UPDATE      0xBF    /* update-global commands */

#define CONFIG_REPEAT_TIMEOUT  2400000

/* Boot flag: FLASH->STATR bit 14 — set by firmware before reset, survives PFIC reset.
 * RAM magic word does NOT survive PFIC system reset on CH32V003. */

/* EEPROM (AT24C256) — layout must match firmware eeprom_layout.h */
#define EE_I2C_ADDR     0xA0
#define EE_PAGE_SIZE    64
#define EE_MAGIC        0x44414C49
/* Identity block at 0x0000: [magic:4][gtin:6][evg_mode:1][hw_maj:1][hw_min:1][fw_maj:1][fw_min:1][short_addr:1] */
#define EE_ID_ADDR      0x0000
#define EE_ID_SIZE      16      /* bytes we need to read */
/* Firmware staging starts at 0x0080 */
#define EE_FW_ADDR      0x0080

/* ── GPIO helpers ────────────────────────────────────────────────── */
static inline int  bus_read(void)   { return (GPIOC->INDR >> 3) & 1; }
static inline int  btn_read(void)   { return (GPIOA->INDR >> 1) & 1; }
/* PHY polarity: HIGH = bus active (mark), LOW = bus idle (space) */
static inline void tx_active(void)  { GPIOC->BSHR = 1 << 4; }
static inline void tx_idle(void)    { GPIOC->BSHR = 1 << (4 + 16); }

static inline void delay(uint32_t cycles) {
    uint32_t start = SysTick->CNT;
    while ((uint32_t)(SysTick->CNT - start) < cycles);
}

/* ── Manchester RX: 32-bit polling decoder ───────────────────────── */
static int wait_start(uint32_t timeout) {
    while (!bus_read()) { if (--timeout == 0) return 0; }
    while ( bus_read()) { if (--timeout == 0) return 0; }
    return 1;
}

static void rx_frame32(uint32_t *out) {
    delay(BIT_CYCLES + HALF_BIT_CYCLES + HALF_BIT_CYCLES / 2);
    uint32_t data = 0;
    for (int i = 0; i < 32; i++) {
        data = (data << 1) | bus_read();
        if (i < 31) delay(BIT_CYCLES);
    }
    *out = data;
}

/* ── Manchester TX: 8-bit backward frame ─────────────────────────── */
/* PHY: active=HIGH, idle=LOW. Bit 1 = active→idle, bit 0 = idle→active. */
#define BSHR_ACT  (1u << 4)
#define BSHR_IDLE (1u << 20)
static void tx_byte(uint8_t val) {
    delay(BIT_CYCLES * 7);                  /* settle time */
    /* Prepend start bit (1) at MSB of 9-bit shift register. */
    uint16_t bits = 0x100u | val;
    for (uint16_t mask = 0x100; mask; mask >>= 1) {
        uint32_t first = (bits & mask) ? BSHR_ACT : BSHR_IDLE;
        GPIOC->BSHR = first;
        delay(HALF_BIT_CYCLES);
        GPIOC->BSHR = first ^ (BSHR_ACT | BSHR_IDLE);
        delay(HALF_BIT_CYCLES);
    }
    tx_idle();                              /* return to idle */
    delay(BIT_CYCLES * 4);                  /* stop bits */
}

/* ── Flash programming ───────────────────────────────────────────── */
static void flash_unlock(void) {
    FLASH->KEYR = FLASH_KEY1;
    FLASH->KEYR = FLASH_KEY2;
    FLASH->MODEKEYR = FLASH_KEY1;
    FLASH->MODEKEYR = FLASH_KEY2;
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
    for (int i = 0; i < 16; i++) {
        dst[i] = buf[i];
        FLASH->CTLR = CR_PAGE_PG | CR_BUF_LOAD;
        while (FLASH->STATR & FLASH_STATR_BSY);
    }
    FLASH->CTLR = CR_PAGE_PG | CR_STRT_Set;
    while (FLASH->STATR & FLASH_STATR_BSY);
    FLASH->CTLR = 0;
}

static void boot_usercode(void) {
    FLASH->BOOT_MODEKEYR = FLASH_KEY1;
    FLASH->BOOT_MODEKEYR = FLASH_KEY2;
    FLASH->STATR = 0;
    FLASH->CTLR = CR_LOCK_Set;
    PFIC->SCTLR = 1 << 31;
}

/* ── Config repeat ───────────────────────────────────────────────── */
static uint32_t repeat_frame;
static uint32_t repeat_time;

static int config_repeat(uint32_t frame) {
    uint32_t now = SysTick->CNT;
    if (frame == repeat_frame && (now - repeat_time) < CONFIG_REPEAT_TIMEOUT) {
        repeat_frame = 0;
        return 1;
    }
    repeat_frame = frame;
    repeat_time = now;
    return 0;
}

/* ── I2C EEPROM ──────────────────────────────────────────────────── */
static void i2c_init(void) {
    RCC->APB1PCENR |= RCC_APB1Periph_I2C1;
    GPIOC->CFGLR &= ~(0xFFu << 4);
    GPIOC->CFGLR |= (0xDDu << 4);
    I2C1->CTLR2 = 24;
    I2C1->CKCFGR = 120;
    I2C1->CTLR1 = I2C_CTLR1_PE;
}

static void ee_write(uint16_t addr, uint8_t *data, uint8_t len) {
    I2C1->CTLR1 |= I2C_CTLR1_START;
    while (!(I2C1->STAR1 & I2C_STAR1_SB));
    I2C1->DATAR = EE_I2C_ADDR;
    while (!(I2C1->STAR1 & I2C_STAR1_ADDR));
    (void)I2C1->STAR2;
    I2C1->DATAR = addr >> 8;
    while (!(I2C1->STAR1 & I2C_STAR1_TXE));
    I2C1->DATAR = addr & 0xFF;
    while (!(I2C1->STAR1 & I2C_STAR1_TXE));
    for (uint8_t i = 0; i < len; i++) {
        I2C1->DATAR = data[i];
        while (!(I2C1->STAR1 & I2C_STAR1_TXE));
    }
    while (!(I2C1->STAR1 & I2C_STAR1_BTF));
    I2C1->CTLR1 |= I2C_CTLR1_STOP;
    delay(120000);
}

static void ee_read(uint16_t addr, uint8_t *buf, uint8_t len) {
    I2C1->CTLR1 |= I2C_CTLR1_START;
    while (!(I2C1->STAR1 & I2C_STAR1_SB));
    I2C1->DATAR = EE_I2C_ADDR;
    while (!(I2C1->STAR1 & I2C_STAR1_ADDR));
    (void)I2C1->STAR2;
    I2C1->DATAR = addr >> 8;
    while (!(I2C1->STAR1 & I2C_STAR1_TXE));
    I2C1->DATAR = addr & 0xFF;
    while (!(I2C1->STAR1 & I2C_STAR1_BTF));

    I2C1->CTLR1 |= I2C_CTLR1_START;
    while (!(I2C1->STAR1 & I2C_STAR1_SB));
    I2C1->DATAR = EE_I2C_ADDR | 1;
    I2C1->CTLR1 |= I2C_CTLR1_ACK;
    while (!(I2C1->STAR1 & I2C_STAR1_ADDR));
    (void)I2C1->STAR2;
    for (uint8_t i = 0; i < len; i++) {
        if (i == len - 1)
            I2C1->CTLR1 &= ~I2C_CTLR1_ACK;
        while (!(I2C1->STAR1 & I2C_STAR1_RXNE));
        buf[i] = I2C1->DATAR;
    }
    I2C1->CTLR1 |= I2C_CTLR1_STOP;
}

/* ── EEPROM write buffer ─────────────────────────────────────────── */
static uint8_t  page_buf[EE_PAGE_SIZE] __attribute__((aligned(4)));
static uint8_t  page_pos;
static uint16_t ee_write_addr;
static uint16_t fw_total_size;

static void flush_to_eeprom(void) {
    if (page_pos == 0) return;
    ee_write(ee_write_addr, page_buf, page_pos);
    ee_write_addr += page_pos;
    fw_total_size += page_pos;
    page_pos = 0;
}

/* ── Copy EEPROM → flash ─────────────────────────────────────────── */
static void copy_eeprom_to_flash(void) {
    flash_unlock();
    for (int p = 0; p < NUM_PAGES; p++)
        flash_erase_page(USER_CODE_BASE + p * PAGE_SIZE);

    uint32_t addr = USER_CODE_BASE;
    uint16_t remaining = fw_total_size;
    uint16_t ee_addr = EE_FW_ADDR;
    while (remaining > 0) {
        uint8_t chunk = (remaining >= PAGE_SIZE) ? PAGE_SIZE : remaining;
        ee_read(ee_addr, page_buf, chunk);
        for (uint8_t i = chunk; i < PAGE_SIZE; i++)
            page_buf[i] = 0xFF;
        flash_write_page(addr, (uint32_t *)page_buf);
        addr += PAGE_SIZE;
        ee_addr += chunk;
        remaining -= chunk;
    }
    FLASH->CTLR = CR_LOCK_Set;
}

/* ── Main ────────────────────────────────────────────────────────── */
int main(void) {
    RCC->CTLR |= (1 << 0);
    RCC->CFGR0 = 0;
    RCC->CTLR &= ~(1 << 24);

    SysTick->CTLR = 5;
    RCC->APB2PCENR |= RCC_APB2Periph_AFIO | RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOC;

    /* PC0..3 + PC5..7 = floating input (0x4 nibble), PC4 = 50MHz PP output (0x3 nibble) */
    GPIOC->CFGLR = 0x44434444;
    tx_idle();
    /* PA1 = input with pull-up/down (0x8 nibble), pull-up via BSHR */
    GPIOA->CFGLR = 0x44444484;
    GPIOA->BSHR = (1 << 1);

    /* Sample button state BEFORE touching FLASH registers, in case the
     * FLASH unlock/STATR sequence has any side-effect on the read path
     * for PA1. */
    delay(480000);          /* 20 ms — settle PA1 pull-up / debounce cap */
    uint8_t btn_held = !btn_read();   /* 1 = button pressed (PA1 LOW) */

    /* Software-triggered entry: firmware sets FLASH->STATR bit 14 before reset.
     * Clear immediately so a debugger reset or crash doesn't loop forever. */
    uint8_t sw_boot = (FLASH->STATR & (1 << 14)) ? 1 : 0;
    FLASH->BOOT_MODEKEYR = FLASH_KEY1;
    FLASH->BOOT_MODEKEYR = FLASH_KEY2;
    FLASH->STATR &= ~(1 << 14);

    if (!sw_boot && !btn_held)
        boot_usercode();

    i2c_init();

    /* Read device identity from EEPROM */
    uint8_t ee_id[EE_ID_SIZE];
    ee_read(EE_ID_ADDR, ee_id, EE_ID_SIZE);

    uint8_t *my_gtin = &ee_id[4];       /* 6 bytes at offset 4 */
    uint8_t  my_evg_mode = ee_id[10];   /* 1 byte at offset 10 */
    uint8_t  my_addr = ADDR_BROADCAST;
    if (*(uint32_t *)ee_id == EE_MAGIC && ee_id[15] <= 63)
        my_addr = ee_id[15] << 1;

    /* IEC 62386-105 state — updateEnabled is implicit: bootloader is always
     * in update mode; firmware handles START FW TRANSFER. */
    uint8_t  blockFault = 0;

    /* Block receive state */
    uint32_t currentBlock = 0;
    uint16_t currentBlockByte = 0;
    uint16_t blockDataSize = 0;

    /* Fletcher-16 over Block 1 firmware payload.
     * Master writes expected fa/fb into Block 0 [0x2C/0x2D]; BL accumulates
     * over each received firmware byte and validates at FINISH FW UPDATE. */
    uint8_t fa = 0, fb = 0, expected_fa = 0, expected_fb = 0;

    page_pos = 0;
    ee_write_addr = EE_FW_ADDR;
    fw_total_size = 0;

    while (1) {
        uint32_t frame;
        if (!wait_start(0x00FFFFFF)) continue;
        rx_frame32(&frame);

        uint8_t b0 = (frame >> 24);
        uint8_t b1 = (frame >> 16);
        uint8_t b2 = (frame >> 8);
        uint8_t b3 = frame;

        /* ── BEGIN BLOCK (0xCB) ─────────────────────────────────── */
        if (b0 == OP_BEGIN_BLOCK) {
            currentBlock = (uint32_t)b1 | b2 | b3;  /* only zero/non-zero matters */
            currentBlockByte = 0;
            blockDataSize = 0;
            if (currentBlock != 0) {
                blockFault = 0;
                fa = fb = 0;            /* reset Fletcher accumulator */
            }
            continue;
        }

        /* ── TRANSFER BLOCK DATA (0xBD) ─────────────────────────── */
        if (b0 == OP_BLOCK_DATA) {
            uint8_t bytes[3] = { b1, b2, b3 };
            for (int i = 0; i < 3; i++) {
                uint16_t pos = currentBlockByte++;
                uint8_t d = bytes[i];

                if (currentBlock == 0) {
                    /* Block 0 — validate GTIN (pos 5..10), device key (0x2B),
                     * and capture Fletcher-16 expected at [0x2C/0x2D]. */
                    if (pos >= 5 && pos <= 10) {
                        if (d != my_gtin[pos - 5])
                            blockFault = 1;
                    } else if (pos == 0x2B) {
                        if (d != my_evg_mode)
                            blockFault = 1;
                    } else if (pos == 0x2C) expected_fa = d;
                    else if  (pos == 0x2D) expected_fb = d;
                } else {
                    /* Block 1..n — extract firmware data */
                    if (pos < 2) {
                        blockDataSize = (blockDataSize << 8) | d;
                    } else if (pos >= 15 && pos < blockDataSize + 15) {
                        page_buf[page_pos++] = d;
                        fa += d; fb += fa;          /* Fletcher-16 update */
                        if (page_pos >= EE_PAGE_SIZE)
                            flush_to_eeprom();
                    }
                }
            }
            continue;
        }

        /* ── 0xBF-addressed standard commands (update-global) ──── */
        if (b0 == ADDR_FW_UPDATE && b1 == OP_STANDARD) {
            switch (b2) {
            case CMD_FINISH:
                if (!config_repeat(frame)) continue;
                if (fa != expected_fa || fb != expected_fb)
                    blockFault = 1;     /* Fletcher mismatch -> abort commit */
                if (blockFault) {
                    tx_byte(0xFF);  /* YES = not done (fault) */
                } else {
                    flush_to_eeprom();
                    copy_eeprom_to_flash();
                    delay(BIT_CYCLES * 8);
                    boot_usercode();
                }
                break;
            case CMD_Q_RUNNING:
                tx_byte(0xFF);      /* YES */
                break;
            case CMD_Q_BLOCK_FAULT:
                if (blockFault) tx_byte(0xFF);  /* YES = fault */
                break;
            }
            continue;
        }

        /* ── Device-addressed standard commands ──────────────────── */
        if (b1 != OP_STANDARD) continue;
        if (b0 != my_addr && b0 != ADDR_BROADCAST && b0 != ADDR_BROADCAST_UA)
            continue;

        switch (b2) {
        case CMD_START:
            tx_byte(0xFF);          /* YES — always ready */
            break;

        case CMD_RESTART:
            tx_byte(0xFF);          /* YES — always allow */
            delay(BIT_CYCLES * 8);
            boot_usercode();
            break;
        }
    }
}
