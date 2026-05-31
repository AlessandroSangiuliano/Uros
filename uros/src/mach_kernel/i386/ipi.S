/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 */

/*
 * ipi.S — IPI vector entry stubs (#302 increment 2).
 *
 * IDT entries for IPI_VECTOR_RESCHED / TLB_SHOOT / CALL_FUNC live in
 * idt.S; this file provides the asm trampolines they point at.  An
 * IPI carries no error code, so the entry layout is just the CPU-pushed
 * EIP/CS/EFLAGS (and possibly SS/ESP for cross-privilege).
 *
 * Each stub saves the caller-clobbered set + segment registers, switches
 * %ds/%es to KERNEL_DS, calls the C handler (which is responsible for
 * lapic_eoi()), then unwinds and iret's.  We deliberately do NOT route
 * through alltraps/all_intrs: IPIs originate from the LAPIC, the PIC
 * EOI sequence in all_intrs would send a spurious OCW2 to the 8259 and
 * stale interrupts could leak through.
 */

#include <i386/asm.h>
#include <assym.S>		/* KERNEL_DS */

#define IPI_ENTER	\
	pushl	%eax	;\
	pushl	%ecx	;\
	pushl	%edx	;\
	pushl	%ds	;\
	pushl	%es	;\
	mov	$KERNEL_DS,%ax	;\
	mov	%ax,%ds	;\
	mov	%ax,%es

#define IPI_LEAVE	\
	popl	%es	;\
	popl	%ds	;\
	popl	%edx	;\
	popl	%ecx	;\
	popl	%eax	;\
	iret

Entry(ipi_resched_entry)
	IPI_ENTER
	call	EXT(ipi_resched_handler)
	IPI_LEAVE

Entry(ipi_tlb_shoot_entry)
	IPI_ENTER
	call	EXT(ipi_tlb_shoot_handler)
	IPI_LEAVE

Entry(ipi_call_func_entry)
	IPI_ENTER
	call	EXT(ipi_call_func_handler)
	IPI_LEAVE
