/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Application processor trampoline (#438).
 *
 * A processor woken by a startup interrupt begins in real mode, at a page
 * below 1 MiB — the message carries a vector the CPU shifts left by twelve,
 * so that is the only kind of address it can be told.  Everything the boot
 * processor did in #406 has to be done again here, per processor and
 * without a bootloader: real mode, protected mode, page tables, long mode,
 * and finally the higher half.
 *
 * It is copied to that page before use, and needs no relocating when it
 * gets there: every address it refers to is already written as the constant
 * it will have at run time, which is what the fixed offsets in
 * <cpu/ap_trampoline.h> are for.  A section VMA would be the usual way to
 * arrange that, and is not needed here precisely because nothing is
 * relative.
 *
 * Its only input is the parameter block, which the boot processor fills in:
 * CR3 cannot be a constant, and a 64-bit entry point does not fit the
 * 32-bit relocation this page would otherwise need.
 *
 * ── Why there is no lock in here ──────────────────────────────────────
 *
 * The i386 trampoline runs every AP on one shared boot stack, whose frames
 * hold values that depend on how far along it is — so two APs inside it at
 * once corrupt each other, and #367 has to serialise the transit through
 * start_lock and pace the wake-ups accordingly.
 *
 * This one never touches a stack.  It pushes nothing and calls nothing:
 * every step is a register or a control register, and the stack pointer is
 * set for the first time in the higher half, from a table each processor
 * indexes with its own APIC id.  CPUID needs no stack to answer, so a
 * processor can find out who it is before it has anywhere to put anything.
 *
 * What is shared is shared read-only — this page, the GDT in it, the page
 * tables — which is idempotent by construction.  So there is no mutual
 * exclusion to arrange, no funnel to wait on, and no wedged-AP case that
 * could damage the next one: they have nothing between them to damage.
 */

#include <cpu/ap_trampoline.h>

	.section .trampoline, "ax"
	.code16
	.globl	ap_trampoline_start
ap_trampoline_start:
	cli
	cld

	/*
	 * Segment registers hold whatever the startup message implied; make
	 * them agree with where this page actually is, so the descriptor
	 * table can be found.
	 */
	xorw	%ax, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %ss

	lgdt	AP_GDT_PTR

	/* Protected mode: the first of the two climbs. */
	movl	%cr0, %eax
	orl	$1, %eax
	movl	%eax, %cr0

	ljmpl	$0x08, $AP_PROT_ENTRY

	.code32
	.org	AP_PROT_OFFSET
ap_prot:
	movw	$0x10, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %ss
	movw	%ax, %fs
	movw	%ax, %gs

	/*
	 * The same climb #406 made, with the tables already built: the boot
	 * processor's own, so every processor translates through one set from
	 * its first instruction in long mode.
	 */
	movl	%cr4, %eax
	orl	$(1 << 5), %eax			/* PAE */
	movl	%eax, %cr4

	movl	AP_PARAM_CR3, %eax
	movl	%eax, %cr3

	movl	$0xC0000080, %ecx		/* IA32_EFER */
	rdmsr
	orl	$(1 << 8), %eax			/* LME */
	orl	$(1 << 11), %eax		/* NXE: the tables already use NX */
	wrmsr

	movl	%cr0, %eax
	orl	$(1 << 31), %eax		/* PG */
	movl	%eax, %cr0

	ljmpl	$0x18, $AP_LONG_ENTRY

	.code64
	.org	AP_LONG_OFFSET
ap_long:
	/*
	 * Into the higher half, and out of this page for good.  The address
	 * is a full 64 bits, so it comes from the parameter block rather than
	 * from a relocation this page could not hold.
	 */
	movq	AP_PARAM_ENTRY, %rax
	jmp	*%rax

/* ------------------------------------------------------------------ */
/*  A descriptor table of its own                                      */
/*                                                                     */
/*  The kernel's lives in the higher half, which is unreachable until   */
/*  paging is on — and paging cannot be turned on without one.  So this */
/*  page carries the minimum to climb: flat 32-bit code and data, and   */
/*  the 64-bit code segment the last jump needs.                        */
/* ------------------------------------------------------------------ */
	.org	AP_GDT_OFFSET
ap_gdt:
	.quad	0				/* 0x00 null */
	.quad	0x00CF9A000000FFFF		/* 0x08 code32, flat */
	.quad	0x00CF92000000FFFF		/* 0x10 data32, flat */
	.quad	0x00209A0000000000		/* 0x18 code64, L */
ap_gdt_end:

	.org	AP_GDT_PTR_OFFSET
ap_gdt_ptr:
	.word	ap_gdt_end - ap_gdt - 1
	.long	AP_GDT_ENTRY

/*
 * The parameter block.  Two values that cannot be known when this is
 * assembled: where the page tables are, and where in the higher half to
 * continue.  The boot processor writes both before waking anyone.
 */
	.org	AP_PARAM_OFFSET
ap_params:
	.long	0				/* CR3 */
	.long	0				/* padding */
	.quad	0				/* 64-bit entry point */

	.globl	ap_trampoline_end
ap_trampoline_end:

/* ------------------------------------------------------------------ */
/*  The higher half, where each processor picks its own stack           */
/*                                                                     */
/*  This is the point of the whole arrangement.  CPUID answers without  */
/*  a stack, so a processor can learn its own identity before it has    */
/*  anywhere to put anything — and index a table of stacks with it.     */
/*  Nothing is handed to it and nothing is taken in turn.               */
/* ------------------------------------------------------------------ */
	.section .text
	.code64
	.globl	ap_entry64
ap_entry64:
	movl	$1, %eax
	cpuid
	shrl	$24, %ebx			/* leaf 1: EBX[31:24] = APIC id */

	leaq	ap_stack_top(%rip), %rax
	movq	(%rax, %rbx, 8), %rsp

	/*
	 * A stack exists from here.  Clear the frame pointer first so a
	 * backtrace taken on this processor ends at its entry rather than
	 * walking into whatever the register happened to hold.
	 */
	xorq	%rbp, %rbp
	movl	%ebx, %edi
	call	ap_start_c

1:	cli
	hlt
	jmp	1b
