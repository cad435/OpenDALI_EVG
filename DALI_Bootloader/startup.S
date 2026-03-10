/*
 * Minimal startup for CH32V003 bootloader (1920-byte boot area).
 * No interrupts needed — pure polling. Just vector table + jump to main.
 */

.section .init, "ax"
.global InterruptVector
.type InterruptVector, @function
InterruptVector:
    j _start

.section .text
.global _start
.type _start, @function
_start:
    /* Set stack pointer */
    la sp, _eusrstack

    /* Zero BSS */
    la a0, _sbss
    la a1, _ebss
1:  bgeu a0, a1, 2f
    sw zero, 0(a0)
    addi a0, a0, 4
    j 1b
2:

    /* Jump to main */
    j main
