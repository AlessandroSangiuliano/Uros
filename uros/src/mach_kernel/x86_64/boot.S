/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * x86-64 boot entry (#406, MD contract 1/6).
 *
 * GRUB loads this ELF64 image via multiboot2 and hands control in 32-bit
 * protected mode, paging off (qemu's -kernel multiboot1 loader rejects
 * ELF64, so the dev loop is a GRUB rescue ISO).  This entry is therefore
 * 32-bit code to begin with, regardless of the fact that the target is
 * x86-64: long mode is a transition we perform ourselves.
 *
 * Sequence:
 *   1. capture the multiboot handoff, bring up COM1, announce (32-bit);
 *   2. build page tables: identity for the low 1 GiB (needed to keep
 *      executing while paging comes on) AND the kernel's higher-half
 *      window at 0xffffffff80000000 (docs/uros_design.md ch.11);
 *   3. enable PAE, set EFER.LME, turn on paging → long mode active;
 *   4. far-jump into a 64-bit code segment (low identity) → true 64-bit;
 *   5. jump to the kernel's higher-half virtual address and run there;
 *   6. announce from the higher half, printing our own runtime %rip as
 *      proof it reads 0xffffffff80... , then halt.
 *
 * The kernel code/rodata are linked at the higher-half base (see boot.ld):
 * their symbols already hold high virtual addresses, so reaching them is a
 * single absolute jump once the high window is mapped — no runtime fixups.
 */

#define COM1 0x3F8			/* first serial port (QEMU -serial) */

/* ------------------------------------------------------------------ */
/*  Multiboot2 header — GRUB reads this to load the ELF64 image.  (No   */
/*  multiboot1 header: it would be dead weight, since the mb1 loader    */
/*  cannot load an ELF64 kernel anyway.)                                */
/* ------------------------------------------------------------------ */
	.section .multiboot2, "a"
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

/* ================================================================== */
/*  LOW half — 32-bit boot, trampoline, tables (VMA == LMA at 1 MiB)   */
/* ================================================================== */
	.section .boot, "ax"
	.code32
	.globl	_start
_start:
	/* A known-good low stack: multiboot leaves %esp undefined, and the
	 * 32-bit serial routine below pushes.  boot_stack_top is low. */
	movl	$boot_stack_top, %esp

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

	/* --- build page tables --- *
	 * Two windows onto the same physical low 1 GiB (2 MiB pages):
	 *   identity   PML4[0]   -> pdpt_low[0]   -> pd   (for the transition)
	 *   higher-half PML4[511] -> pdpt_high[510] -> pd  (0xffffffff80000000)
	 * They share one PD, so both show phys 0..1 GiB.  Zero the upper
	 * tables first so no stray present bit sends a walk astray. */
	xorl	%eax, %eax
	movl	$pml4,      %edi ; movl $1024, %ecx ; rep stosl	/* zero PML4      */
	movl	$pdpt_low,  %edi ; movl $1024, %ecx ; rep stosl	/* zero PDPT low  */
	movl	$pdpt_high, %edi ; movl $1024, %ecx ; rep stosl	/* zero PDPT high */

	movl	$pdpt_low,  %eax ; orl $0x03, %eax ; movl %eax, pml4		/* PML4[0]   */
	movl	$pd,        %eax ; orl $0x03, %eax ; movl %eax, pdpt_low		/* PDPT[0]   */
	movl	$pdpt_high, %eax ; orl $0x03, %eax ; movl %eax, pml4 + 511*8	/* PML4[511] */
	movl	$pd,        %eax ; orl $0x03, %eax ; movl %eax, pdpt_high + 510*8	/* PDPT[510] */

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
	ljmp	$0x08, $low64_entry		/* 0x08 = 64-bit code selector */

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

/*
 * low64_entry — first 64-bit instructions, still on the low identity map.
 * Hop to the kernel's higher-half virtual address: high_entry is linked at
 * KERNEL_VBASE, so its symbol is already the high VMA (an absolute imm64).
 */
	.code64
low64_entry:
	leaq	boot_stack_top(%rip), %rsp	/* low stack, identity-mapped */
	movabsq	$high_entry, %rax		/* high VMA (R_X86_64_64) */
	jmp	*%rax

/* ------------------------------------------------------------------ */
/*  Low read-only: 64-bit GDT (null, code L=1, data) + 32-bit banner    */
/*  The GDT base in gdt64_ptr is a 32-bit .long — the GDT must be low.   */
/* ------------------------------------------------------------------ */
	.section .boot.rodata, "a"
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

/* ------------------------------------------------------------------ */
	.section .boot.data, "aw"
	.align	8
	.globl	boot_magic
boot_magic:	.long	0
	.globl	boot_info
boot_info:	.long	0

/* Initial page tables + boot stack — low, page-aligned, zero-init. */
	.section .boot.bss, "aw", @nobits
	.align	4096
pml4:		.skip	4096
pdpt_low:	.skip	4096
pdpt_high:	.skip	4096
pd:		.skip	4096
	.align	16
boot_stack_bottom:
	.skip	4096
boot_stack_top:

/* ================================================================== */
/*  HIGH half — kernel image at KERNEL_VBASE (0xffffffff80000000)      */
/* ================================================================== */
	.section .text, "ax"
	.code64
high_entry:
	movw	$0x10, %ax			/* data selector */
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %ss
	movw	%ax, %fs
	movw	%ax, %gs

	/* Switch to the kernel stack (high, in .bss).  The low boot stack was
	 * only for the transition; C runs on this one. */
	leaq	kernel_stack_top(%rip), %rsp

	leaq	msg_long(%rip), %rsi
	call	serial_puts64

	/* Proof of higher-half: print our own runtime address.  high_entry is
	 * linked high, and %rip is now high, so leaq yields 0xffffffff80... */
	leaq	msg_hh(%rip), %rsi
	call	serial_puts64
	leaq	high_entry(%rip), %rdi
	call	serial_puthex64
	leaq	msg_crlf(%rip), %rsi
	call	serial_puts64

	/* Hand off to C (x86_64_boot).  SysV: %edi = magic, %esi = info.
	 * boot_magic/boot_info are low (.boot.data) — reach them by absolute
	 * address, not %rip-relative (that would overflow ±2 GiB from here). */
	movabsq	$boot_magic, %rax
	movl	(%rax), %edi
	movabsq	$boot_info, %rax
	movl	(%rax), %esi
	call	x86_64_boot

hang64:
	cli
	hlt
	jmp	hang64

/*
 * putc64 — write %al to COM1, waiting for the transmit holding reg empty.
 * Preserves %rax's caller value is NOT expected; clobbers %al only here.
 */
putc64:
	pushq	%rdx
	pushq	%rcx
	movb	%al, %cl			/* stash the char */
	movw	$COM1+5, %dx			/* wait THR empty (LSR bit 5) */
1:	inb	%dx, %al
	testb	$0x20, %al
	jz	1b
	movb	%cl, %al
	movw	$COM1+0, %dx
	outb	%al, %dx
	popq	%rcx
	popq	%rdx
	ret

/*
 * serial_puts64 — write a NUL-terminated string to COM1.  in: %rsi.
 */
serial_puts64:
	pushq	%rsi
	pushq	%rax
1:	movb	(%rsi), %al
	testb	%al, %al
	jz	2f
	call	putc64
	incq	%rsi
	jmp	1b
2:	popq	%rax
	popq	%rsi
	ret

/*
 * serial_puthex64 — print %rdi as "0x" followed by 16 hex digits.  in: %rdi.
 */
serial_puthex64:
	pushq	%rdi
	pushq	%rcx
	pushq	%rax
	movb	$'0', %al ; call putc64
	movb	$'x', %al ; call putc64
	movl	$16, %ecx			/* 16 nibbles, most significant first */
1:	rolq	$4, %rdi			/* bring the top nibble to the bottom */
	movb	%dil, %al
	andb	$0x0F, %al
	cmpb	$9, %al
	jbe	2f
	addb	$('a' - 10), %al
	jmp	3f
2:	addb	$'0', %al
3:	call	putc64
	loop	1b
	popq	%rax
	popq	%rcx
	popq	%rdi
	ret

/* ------------------------------------------------------------------ */
	.section .rodata, "a"
msg_long:
	.asciz	"UrMach x86-64: long mode active (64-bit)\r\n"
msg_hh:
	.asciz	"UrMach x86-64: running higher-half at "
msg_crlf:
	.asciz	"\r\n"

/* Kernel stack — high (.bss), what the first C runs on. */
	.section .bss, "aw", @nobits
	.align	16
kernel_stack_bottom:
	.skip	16384
kernel_stack_top:
