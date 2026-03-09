/*
    funconfig.h - ch32fun framework configuration for DALI EVG

    This file is required by the ch32fun framework (cnlohr/ch32fun).
    It configures the system clock, UART printf, and chip variant.

    ch32fun reads these defines at compile time to set up:
    - PLL configuration (HSI 24 MHz × 2 = 48 MHz)
    - UART printf on PD5 (USART1_TX) for debug output
*/
#ifndef _FUNCONFIG_H
#define _FUNCONFIG_H

#define CH32V003           1            /* Target chip: CH32V003 RISC-V */
#define FUNCONF_USE_PLL    1            /* Enable PLL: HSI 24 MHz -> 48 MHz */
#define FUNCONF_USE_UARTPRINTF 1        /* Enable printf() via USART1 on PD5 */
#define FUNCONF_UART_PRINTF_BAUD 115200 /* Debug serial baud rate */

#endif
