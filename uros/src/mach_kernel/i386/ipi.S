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
 * %ds/%es to KERNEL_DS and %gs to CPU_DATA, calls the C handler (which is
 * responsible for lapic_eoi()), then unwinds and iret's.  We deliberately
 * do NOT route through alltraps/all_intrs: IPIs originate from the LAPIC,
 * the PIC EOI sequence in all_intrs would send a spurious OCW2 to the
 * 8259 and stale interrupts could leak through.
 *
 * #310: %gs MUST be reloaded to CPU_DATA.  When the IPI is taken while
 * the CPU is running a user thread (CPL 3), %gs holds the user's TLS
 * selector, not the kernel per-CPU descriptor.  The C handler reaches
 * pmap_update_interrupt() -> process_pmap_updates(), which calls
 * current_thread()/cpu_number() — both read %gs:(off).  With the user
 * %gs still loaded those reads return garbage and the handler faults on
 * a near-NULL pointer (cr2=0x4 / 0x24).  This mirrors what all_intrs
 * does for the #280 gs-leak fix: save the caller's %gs, load CPU_DATA,
 * restore on exit.  Reloading CPU_DATA when the IPI came from kernel
 * mode is a harmless no-op (%gs was already CPU_DATA).
 */

#include <i386/asm.h>
#include <assym.S>		/* KERNEL_DS, CPU_DATA */

#define IPI_ENTER	\
	pushl	%eax	;\
	pushl	%ecx	;\
	pushl	%edx	;\
	pushl	%ds	;\
	pushl	%es	;\
	pushl	%gs	;\
	mov	$KERNEL_DS,%ax	;\
	mov	%ax,%ds	;\
	mov	%ax,%es	;\
	mov	$CPU_DATA,%ax	;\
	mov	%ax,%gs

#define IPI_LEAVE	\
	popl	%gs	;\
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
