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
    /* Load global pointer — enables gp-relative linker relaxation for
     * all global variable accesses (single lw/sw instead of lui+op). */
    .option push
    .option norelax
    la gp, __global_pointer$
    .option pop

    /* Set stack pointer */
    la sp, _eusrstack

    /* No BSS zeroing: every global is explicitly initialized in main
     * before use (page_pos/ee_write_addr/fw_total_size/repeat_frame);
     * page_buf and repeat_time are written before they are read. */

    /* Jump to main */
    j main
