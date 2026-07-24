/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * x86-64 boot entry (#406, MD contract 1/6).
 *
 * The bootloader — `qemu -kernel` via multiboot1, or GRUB via multiboot2 —
 * hands control to us in 32-bit protected mode, paging off.  This entry is
 * therefore 32-bit code to begin with, regardless of the fact that the
 * target is x86-64: long mode is a transition we perform ourselves.
 *
 * Sequence:
 *   1. capture the multiboot handoff, bring up COM1, announce (32-bit);
 *   2. build identity page tables for the low 1 GiB (2 MiB pages);
 *   3. enable PAE, set EFER.LME, turn on paging → long mode active;
 *   4. far-jump into a 64-bit code segment → true 64-bit execution;
 *   5. announce again (64-bit), then halt.
 *
 * Everything still runs identity-mapped at low physical addresses: the
 * kernel does not yet live at its higher-half base (0xffffffff80000000,
 * docs/uros_design.md ch.11) — that mapping, and running the 64-bit code
 * there, is the next increment.  The two banners bracket the transition so
 * a fault leaves the last-reached one on the wire.
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
	.align	8
	.short	0				/* end tag */
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
	 * %eax = bootloader magic, %ebx = boot-info pointer. */
	movl	%eax, boot_magic
	movl	%ebx, boot_info

	/* Minimal COM1 init: 38400 8N1, FIFO on. */
	movw	$COM1+1, %dx ; xorb %al, %al   ; outb %al, %dx	/* IER: no interrupts */
	movw	$COM1+3, %dx ; movb $0x80, %al ; outb %al, %dx	/* LCR: DLAB=1 */
	movw	$COM1+0, %dx ; movb $0x03, %al ; outb %al, %dx	/* divisor low = 3 */
	movw	$COM1+1, %dx ; xorb %al, %al   ; outb %al, %dx	/* divisor high = 0 */
	movw	$COM1+3, %dx ; movb $0x03, %al ; outb %al, %dx	/* LCR: 8N1, DLAB=0 */
	movw	$COM1+2, %dx ; movb $0xC7, %al ; outb %al, %dx	/* FCR: enable+clear FIFO */
	movw	$COM1+4, %dx ; movb $0x0B, %al ; outb %al, %dx	/* MCR: DTR/RTS/OUT2 */

	leal	msg_boot, %esi
	call	serial_puts32

	/* --- build identity page tables: low 1 GiB, 2 MiB pages --- *
	 * Only the entries on the walk for low addresses are consulted
	 * (PML4[0] -> PDPT[0] -> PD[0..511]); zero PML4/PDPT first anyway
	 * so a stray present bit in .bss cannot send a walk astray. */
	xorl	%eax, %eax
	movl	$pml4, %edi ; movl $1024, %ecx ; rep stosl	/* zero PML4 */
	movl	$pdpt, %edi ; movl $1024, %ecx ; rep stosl	/* zero PDPT */

	movl	$pdpt, %eax ; orl $0x03, %eax ; movl %eax, pml4	/* PML4[0] -> PDPT */
	movl	$pd,   %eax ; orl $0x03, %eax ; movl %eax, pdpt	/* PDPT[0] -> PD   */

	movl	$pd, %edi			/* PD[i] = i*2MiB | present|write|PS */
	movl	$0x83, %eax			/* base 0, present|write|PS(huge) */
	xorl	%ebx, %ebx			/* entry high dword */
	movl	$512, %ecx
1:	movl	%eax, (%edi)
	movl	%ebx, 4(%edi)
	addl	$0x200000, %eax			/* next 2 MiB */
	adcl	$0, %ebx
	addl	$8, %edi
	loop	1b

	/* --- PAE on --- */
	movl	%cr4, %eax
	orl	$(1 << 5), %eax			/* CR4.PAE */
	movl	%eax, %cr4

	/* --- CR3 = PML4 --- */
	movl	$pml4, %eax
	movl	%eax, %cr3

	/* --- EFER.LME on --- */
	movl	$0xC0000080, %ecx		/* IA32_EFER */
	rdmsr
	orl	$(1 << 8), %eax			/* LME */
	wrmsr

	/* --- paging on: long mode becomes active (compat mode until the
	 *     far jump reloads CS with a 64-bit segment) --- */
	movl	%cr0, %eax
	orl	$(1 << 31), %eax		/* CR0.PG */
	movl	%eax, %cr0

	lgdt	gdt64_ptr
	ljmp	$0x08, $long_entry		/* 0x08 = 64-bit code selector */

/*
 * serial_puts32 — write a NUL-terminated string to COM1.  in: %esi.
 */
serial_puts32:
	pushl	%eax
	pushl	%ebx
	pushl	%edx
1:	movb	(%esi), %bl
	testb	%bl, %bl
	jz	2f
	movw	$COM1+5, %dx			/* wait THR empty (LSR bit 5) */
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
/*  64-bit long-mode entry                                             */
/* ------------------------------------------------------------------ */
	.code64
long_entry:
	movw	$0x10, %ax			/* data selector */
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %ss
	movw	%ax, %fs
	movw	%ax, %gs

	leaq	msg_long(%rip), %rsi
	call	serial_puts64

hang64:
	cli
	hlt
	jmp	hang64

/*
 * serial_puts64 — write a NUL-terminated string to COM1.  in: %rsi.
 */
serial_puts64:
1:	movb	(%rsi), %bl
	testb	%bl, %bl
	jz	2f
	movw	$COM1+5, %dx
3:	inb	%dx, %al
	testb	$0x20, %al
	jz	3b
	movb	%bl, %al
	movw	$COM1+0, %dx
	outb	%al, %dx
	incq	%rsi
	jmp	1b
2:	ret

/* ------------------------------------------------------------------ */
/*  64-bit GDT: null, code (L=1), data                                 */
/* ------------------------------------------------------------------ */
	.section .rodata
	.align	8
gdt64:
	.quad	0				/* 0x00 null */
	.quad	0x00209A0000000000		/* 0x08 code: L, present, DPL0, R/X */
	.quad	0x0000920000000000		/* 0x10 data: present, DPL0, R/W */
gdt64_end:
gdt64_ptr:
	.word	gdt64_end - gdt64 - 1
	.long	gdt64				/* 32-bit base (lgdt runs in 32-bit mode) */

msg_boot:
	.asciz	"UrMach x86-64: reached 32-bit multiboot entry\r\n"
msg_long:
	.asciz	"UrMach x86-64: long mode active (64-bit)\r\n"

/* ------------------------------------------------------------------ */
	.section .data
	.align	4
	.globl	boot_magic
boot_magic:	.long	0
	.globl	boot_info
boot_info:	.long	0

/* Initial page tables — filled at runtime; page-aligned, zero-init. */
	.section .bss
	.align	4096
pml4:	.skip	4096
pdpt:	.skip	4096
pd:	.skip	4096
