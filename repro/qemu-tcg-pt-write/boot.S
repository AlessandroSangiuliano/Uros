/*
 * Multiboot1 entry stub for the QEMU TCG PT-write reproducer.
 * Sets up a stack, zeroes BSS, and jumps into kmain().
 */

.section .multiboot, "a"
.align 4
.long 0x1BADB002              /* magic */
.long 0x00000000              /* flags (no special requests) */
.long -(0x1BADB002)           /* checksum */

.section .bss
.align 16
stack_bottom:
.skip 16384
stack_top:

.section .text
.globl _start
.type _start, @function
_start:
    movl    $stack_top, %esp

    /* Zero BSS so page tables / globals start clean. */
    movl    $_bss_start, %edi
    movl    $_bss_end, %ecx
    subl    %edi, %ecx
    xorl    %eax, %eax
    cld
    rep stosb

    call    kmain
1:  hlt
    jmp     1b
.size _start, . - _start
