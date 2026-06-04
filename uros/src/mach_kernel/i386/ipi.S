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
 * IDT entries for IPI_VECTOR_RESCHED / MP / CALL_FUNC live in idt.S; this
 * file provides the asm trampolines they point at.  An IPI carries no
 * error code, so the entry layout is just the CPU-pushed EIP/CS/EFLAGS
 * (and possibly SS/ESP for cross-privilege).
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
#include <i386/apic.h>		/* LAPIC_ID_SHIFT/MASK for CPU_NUMBER */
#include <assym.S>		/* KERNEL_DS, CPU_DATA, I_CS/I_EIP/I_EFL */

/*
 * Per-CPU helpers, matching the asm macros locore.S / interrupt.S define
 * locally (there is no shared asm header for them).  ipi.S only ever
 * compiles for NCPUS > 1, so the MP forms are unconditional.  CPU_NUMBER
 * reads the LAPIC ID register exactly like <i386/AT386/mp/mp.h>; CX indexes
 * a per-CPU array by the cpu number held in a register.
 */
#define	CPU_NUMBER(r)			\
	movl	EXT(lapic_id), r	;\
	movl	0(r),r			;\
	shrl	$LAPIC_ID_SHIFT, r	;\
	andl	$LAPIC_ID_MASK, r
#define	CX(addr,reg)	addr(,reg,4)

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

/*
 * #316: the unified MP IPI (sent by cpu_interrupt) is the one that can
 * raise an AST (MP_AST / MP_CLOCK quantum) on this CPU.  After draining
 * cpu_int_word it must NOT just iret: it re-enters the shared all_intrs
 * AST/preempt epilogue (ipi_ast_return, locore.S) so a cross-CPU
 * reschedule is actually taken on return.  The IPI_ENTER frame is
 * byte-identical to struct i386_interrupt_state — the layout all_intrs
 * leaves on the stack for that epilogue — so %esp points at it on entry.
 *
 * BUG2 fix: we MUST run the C handler on this CPU's interrupt stack, not
 * on whatever %esp the LAPIC left us.  When the IPI interrupts a user-mode
 * thread the CPU loads %esp from TSS.esp0, which act_machine_switch_pcb()
 * aims straight at &pcb->iss (kernel entries build their saved-state frame
 * there).  ipi_mp_handler() calls ast_check/hertz_tick/slave_clock and the
 * epilogue may rebuild a full trap frame; running all that on &pcb->iss
 * walks off the saved state into the act/heap and corrupts the preempted
 * thread — its saved %eip comes back garbage and it faults on return.
 * all_intrs sidesteps this by switching to the per-CPU interrupt stack; do
 * the same.  The lone exception is a nested IPI that landed on top of an
 * in-progress hardware interrupt already running on this CPU's interrupt
 * stack: switching again would clobber that frame, so detect it (%esp
 * inside [interrupt_stack, int_stack_top]) and run in place, returning
 * with a plain iret — a nested IPI must not preempt.
 *
 * The saved frame pointer is passed to ipi_mp_handler for the MP_CLOCK
 * usermode/pc fields.  Only %eax/%ecx/%edx are touched before the call
 * (all saved by IPI_ENTER); %ebx/%ebp/%esi/%edi keep the interrupted
 * thread's values so ast_from_interrupt's pusha captures them.
 */
Entry(ipi_mp_entry)
	IPI_ENTER
	CPU_NUMBER(%edx)
	movl	CX(EXT(int_stack_top),%edx),%ecx	/* this CPU's int stack top */
	cmpl	%ecx,%esp
	ja	2f				/* %esp above top: not on int stack */
	movl	CX(EXT(interrupt_stack),%edx),%eax /* ... and its base */
	cmpl	%eax,%esp
	jbe	2f				/* %esp at/below base: not on it */

	/* nested on the interrupt stack: run in place, just return */
	pushl	%esp				/* &i386_interrupt_state */
	call	EXT(ipi_mp_handler)
	addl	$4,%esp
	IPI_LEAVE

2:	/* not on the interrupt stack: switch to it for the handler, then
	 * return to the saved frame (on the thread kernel stack, or in
	 * pcb->iss when the IPI hit user mode). */
	xchgl	%ecx,%esp			/* %esp = int stack top; %ecx = &frame */
	pushl	%ecx				/* save frame pointer */
	pushl	%ecx				/* arg: &i386_interrupt_state */
	call	EXT(ipi_mp_handler)
	addl	$4,%esp				/* pop handler arg */
	popl	%esp				/* restore %esp -> saved frame */
	jmp	EXT(ipi_ast_return)		/* shared AST/preempt + iret epilogue */

Entry(ipi_call_func_entry)
	IPI_ENTER
	call	EXT(ipi_call_func_handler)
	IPI_LEAVE
