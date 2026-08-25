/*
 * swio.h — bit-bang the CH32V003's own single-wire debug interface on PD1.
 *
 * The chip talks to its OWN debug module through the SWIO pin, which lets
 * user code enable the debug module without any external probe. That is the
 * whole point of this firmware: BOOT-area erase/program are silent no-ops
 * unless the debug module is active (see main.c).
 *
 * Protocol (write-only subset, from minichlink via
 * github.com/monte-monte/ch32_user_bootloader_flasher):
 *
 *     start bit '1' | 7 command bits, MSB first | '1' | 32 data bits, MSB first
 *
 * A '1' is a short low pulse followed by a high period; a '0' is a low pulse
 * four times as long. Between edges the pin is handed back to the SWD
 * peripheral so the debug module actually samples the line — hence the
 * SWJ_CFG toggling around every write.
 *
 * Timing is delay-loop based, so interrupts must stay off for a whole frame.
 */
#pragma once

#include "ch32fun.h"

/* Debug module registers (RISC-V debug spec + WCH extensions) */
#define DMCONTROL       0x10
#define DMABSTRACTAUTO  0x18
#define DMCFGR          0x7D
#define DMSHDWCFGR      0x7E

/* Bit period coefficient. 2 is minichlink's default for this part at
 * 48 MHz; raise it if the debug module does not react. */
#ifndef SWIO_T1COEFF
#define SWIO_T1COEFF 2
#endif

static inline void swio_delay(int n) {
    asm volatile(
        "1: addi %[n], %[n], -1\n"
        "   bne  %[n], x0, 1b\n"
        : [n] "+r"(n));
}

static inline void swio_send_1(void) {
    AFIO->PCFR1 |= AFIO_PCFR1_SWCFG_DISABLE;
    funDigitalWrite(PD1, 0);
    AFIO->PCFR1 &= ~(AFIO_PCFR1_SWCFG_DISABLE);
    swio_delay(SWIO_T1COEFF);
    AFIO->PCFR1 |= AFIO_PCFR1_SWCFG_DISABLE;
    funDigitalWrite(PD1, 1);
    AFIO->PCFR1 &= ~(AFIO_PCFR1_SWCFG_DISABLE);
    swio_delay(SWIO_T1COEFF);
}

static inline void swio_send_0(void) {
    AFIO->PCFR1 |= AFIO_PCFR1_SWCFG_DISABLE;
    funDigitalWrite(PD1, 0);
    AFIO->PCFR1 &= ~(AFIO_PCFR1_SWCFG_DISABLE);
    swio_delay(SWIO_T1COEFF * 4);
    AFIO->PCFR1 |= AFIO_PCFR1_SWCFG_DISABLE;
    funDigitalWrite(PD1, 1);
    AFIO->PCFR1 &= ~(AFIO_PCFR1_SWCFG_DISABLE);
    swio_delay(SWIO_T1COEFF);
}

static void swio_write_reg32(uint8_t command, uint32_t value) {
    funDigitalWrite(PD1, 1);

    __disable_irq();
    swio_send_1();                              /* start */
    for (uint32_t m = 1u << 6; m; m >>= 1)      /* 7 command bits */
        (command & m) ? swio_send_1() : swio_send_0();
    swio_send_1();                              /* separator */
    for (uint32_t m = 1u << 31; m; m >>= 1)     /* 32 data bits */
        (value & m) ? swio_send_1() : swio_send_0();
    __enable_irq();

    Delay_Ms(8);        /* the debug module needs time; 2 ms is too short */
}

/* Prepare PD1 for bit-banging: push-pull output, SWD multiplexing off. */
static void swio_begin(void) {
    funPinMode(PD1, GPIO_Speed_50MHz | GPIO_CNF_OUT_PP);
    AFIO->PCFR1 &= ~(AFIO_PCFR1_SWCFG);
    funDigitalWrite(PD1, 0);
}
