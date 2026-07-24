/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * x86-64 boot entry (#406, MD contract 1/6).
 *
 * The bootloader — `qemu -kernel` via multiboot1, or GRUB via multiboot2 —
 * hands control to us in 32-bit protected mode, paging off.  This entry is
 * therefore 32-bit code regardless of the fact that the target is x86-64:
 * long mode is a transition we perform ourselves, and it lands in a later
 * increment of this contract.  For now the job is only to prove the path —
 * the image is multiboot-loadable, the entry executes — by capturing the
 * boot handoff and announcing itself on the serial line before halting.
 *
 * Everything here runs at its physical load address (1 MiB): paging is off,
 * so there is no higher half yet.  The kernel image moves to
 * 0xffffffff80000000 (docs/uros_design.md ch.11) once the long-mode page
 * tables exist to map it there.
 */

#define COM1 0x3F8			/* first serial port (QEMU -serial) */

/* ------------------------------------------------------------------ */
/*  Multiboot headers — mirror the i386 kernel (start.S) so the same   */
/*  loaders accept us: mb1 for `qemu -kernel`, mb2 for GRUB/UEFI.       */
/* ------------------------------------------------------------------ */
	.section .multiboot, "a"
	.align	4
mb1_header:
	.long	0x1BADB002			/* magic */
	.long	0x00000002			/* flags: MEMORY_INFO */
	.long	-(0x1BADB002 + 0x00000002)	/* checksum */

	.align	8
mb2_header:
	.long	0xE85250D6			/* mb2 magic */
	.long	0				/* architecture 0 = i386 (32-bit handoff) */
	.long	mb2_header_end - mb2_header	/* header length */
	.long	-(0xE85250D6 + 0 + (mb2_header_end - mb2_header))
	/* end tag */
	.align	8
	.short	0
	.short	0
	.long	8
mb2_header_end:

/* ------------------------------------------------------------------ */
/*  32-bit protected-mode entry                                        */
/* ------------------------------------------------------------------ */
	.section .boot, "ax"
	.code32
	.globl	_start
_start:
	/* Capture the multiboot handoff before anything clobbers it:
	 * %eax = bootloader magic, %ebx = boot-info pointer.  A later
	 * increment parses these; here we only stash them. */
	movl	%eax, boot_magic
	movl	%ebx, boot_info

	/* Minimal COM1 init: 38400 8N1, FIFO on.  QEMU accepts writes
	 * without this, but real hardware and a clean state want it. */
	movw	$COM1+1, %dx ; xorb %al, %al   ; outb %al, %dx	/* IER: no interrupts */
	movw	$COM1+3, %dx ; movb $0x80, %al ; outb %al, %dx	/* LCR: DLAB=1 */
	movw	$COM1+0, %dx ; movb $0x03, %al ; outb %al, %dx	/* divisor low = 3 */
	movw	$COM1+1, %dx ; xorb %al, %al   ; outb %al, %dx	/* divisor high = 0 */
	movw	$COM1+3, %dx ; movb $0x03, %al ; outb %al, %dx	/* LCR: 8N1, DLAB=0 */
	movw	$COM1+2, %dx ; movb $0xC7, %al ; outb %al, %dx	/* FCR: enable+clear FIFO */
	movw	$COM1+4, %dx ; movb $0x0B, %al ; outb %al, %dx	/* MCR: DTR/RTS/OUT2 */

	leal	msg_boot, %esi
	call	serial_puts32

	/* Nothing more to do this increment: long mode lands next. */
hang:
	cli
	hlt
	jmp	hang

/*
 * serial_puts32 — write a NUL-terminated string to COM1.
 *   in: %esi = string pointer.  Clobbers nothing the caller relies on.
 */
serial_puts32:
	pushl	%eax
	pushl	%ebx
	pushl	%edx
1:	movb	(%esi), %bl
	testb	%bl, %bl
	jz	2f
	/* wait for the transmit holding register to drain (LSR bit 5) */
	movw	$COM1+5, %dx
3:	inb	%dx, %al
	testb	$0x20, %al
	jz	3b
	movb	%bl, %al
	movw	$COM1+0, %dx
	outb	%al, %dx
	incl	%esi
	jmp	1b
2:	popl	%edx
	popl	%ebx
	popl	%eax
	ret

/* ------------------------------------------------------------------ */
	.section .data
	.align	4
	.globl	boot_magic
boot_magic:	.long	0
	.globl	boot_info
boot_info:	.long	0

	.section .rodata
msg_boot:
	.asciz	"UrMach x86-64: reached 32-bit multiboot entry\r\n"
