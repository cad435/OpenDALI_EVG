/*
    funconfig.h - ch32fun framework configuration for the BL emergency flasher

    Mirrors the application firmware's configuration so the DALI PHY timing
    behaves identically: HSI 24 MHz x 2 = 48 MHz, printf on USART1 (PD5).
*/
#ifndef _FUNCONFIG_H
#define _FUNCONFIG_H

#define CH32V003           1            /* Target chip: CH32V003 RISC-V */
#define FUNCONF_USE_PLL    1            /* Enable PLL: HSI 24 MHz -> 48 MHz */
#define FUNCONF_USE_UARTPRINTF 1        /* Enable printf() via USART1 on PD5 */
#define FUNCONF_UART_PRINTF_BAUD 115200 /* Debug serial baud rate */

#endif
