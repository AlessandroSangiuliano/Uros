/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * x86-64 trap entry stubs (#409, MD contract 4/6).
 *
 * One stub per architectural vector, all funnelling into a single path that
 * builds the frame trap.h describes and calls C.
 *
 * The stubs exist because of an asymmetry: some exceptions push an error
 * code and the rest do not, so the stack the handler inherits has a
 * different shape depending on which one fired.  Each stub that fires
 * without an error code pushes a zero in its place, after which every
 * vector looks the same from here on and nothing downstream has to
 * remember which is which.
 */

#include <cpu/regs.h>

	.section .text
	.code64

/*
 * Vectors 8, 10-14 and 17 push an error code; 21 does too but belongs to a
 * feature we do not enable.  Everything else does not.
 */
	.macro ISR_NOERR vec
	.align 16
isr_\vec:
	pushq	$0			/* stand in for the missing error */
	pushq	$\vec
	jmp	trap_common
	.endm

	.macro ISR_ERR vec
	.align 16
isr_\vec:
	pushq	$\vec			/* the CPU already pushed the error */
	jmp	trap_common
	.endm

/*
 * And the four that go somewhere else, for a reason that is about %gs and
 * not about error codes (#440).
 *
 * The common path below decides whether to exchange %gs by looking at the
 * ring in the saved code segment.  That is right for every vector whose
 * arrival the kernel controls, and wrong for the ones whose arrival it does
 * not — because there are two instruction boundaries where the processor is
 * already at ring 0 and %gs still belongs to a user program: after SYSCALL
 * and before the entry's own swapgs, and after the exit's swapgs and before
 * SYSRET.  A vector delivered there reads a ring-0 code segment, concludes
 * correctly that it must not swap, and proceeds to run the kernel on a
 * segment base the user chose.
 *
 * Which vectors can be delivered there is not a matter of taste.  Both
 * windows run with interrupts disabled — SYSCALL clears IF through FMASK on
 * the way in, and the way out never sets it — so every maskable interrupt is
 * excluded by the flag.  What is left is exactly what IF does not cover:
 *
 *   #DB   armed by something other than the code it interrupts, so it can
 *         land on any instruction, including these;
 *   NMI   not maskable, which is the whole of its name;
 *   #DF   the fault whose own handler faulted; it arrives when the machine
 *         is already not in a state anyone arranged;
 *   #MC   asynchronous and reported when the hardware says so.
 *
 * So the set is the complement of what the flag protects, and not a list of
 * vectors that seemed dangerous.
 */
	.macro ISR_PARANOID_NOERR vec
	.align 16
isr_\vec:
	pushq	$0
	pushq	$\vec
	jmp	trap_paranoid
	.endm

	.macro ISR_PARANOID_ERR vec
	.align 16
isr_\vec:
	pushq	$\vec
	jmp	trap_paranoid
	.endm

	ISR_NOERR 0
	ISR_PARANOID_NOERR 1
	ISR_PARANOID_NOERR 2
	ISR_NOERR 3
	ISR_NOERR 4
	ISR_NOERR 5
	ISR_NOERR 6
	ISR_NOERR 7
	ISR_PARANOID_ERR 8
	ISR_NOERR 9
	ISR_ERR   10
	ISR_ERR   11
	ISR_ERR   12
	ISR_ERR   13
	ISR_ERR   14
	ISR_NOERR 15
	ISR_NOERR 16
	ISR_ERR   17
	ISR_PARANOID_NOERR 18
	ISR_NOERR 19
	ISR_NOERR 20
	ISR_NOERR 21
	ISR_NOERR 22
	ISR_NOERR 23
	ISR_NOERR 24
	ISR_NOERR 25
	ISR_NOERR 26
	ISR_NOERR 27
	ISR_NOERR 28
	ISR_NOERR 29
	ISR_NOERR 30
	ISR_NOERR 31

/*
 * And the two hundred and twenty-four above them, which are ours to assign.
 *
 * None pushes an error code — that is a property of the exceptions, and an
 * external interrupt never has one — so they are all of the first kind, and
 * writing them out by hand would be two hundred and twenty-four chances to
 * type the wrong number in the one field that distinguishes them.
 *
 * .altmacro is what makes `isr_\vec` become `isr_240` rather than the
 * literal text: the % below evaluates the counter instead of passing its
 * name.  Turned off again immediately, because it also changes how strings
 * are quoted for everything that follows.
 */
	.altmacro
	vec = 32
	.rept 224
	ISR_NOERR %vec
	vec = vec + 1
	.endr
	.noaltmacro

/*
 * Registers are pushed in reverse of the order they are declared in struct
 * trap_frame, because a push moves down: whatever goes last ends up at the
 * lowest address, which is the first field.
 *
 * A macro rather than two copies, because there are now two paths that build
 * this frame and a disagreement between them about its shape would not be a
 * build error — it would be a handler reading the wrong registers, correctly,
 * forever.
 *
 * The stack is 16-byte aligned after these by construction: the CPU aligns
 * it before pushing its own five words, the stub adds two, and fifteen more
 * bring the total back to a multiple of sixteen.  So no fixup is needed and
 * none is done, which keeps %rsp pointing at the frame itself.
 */
	.macro SAVE_REGS
	pushq	%rax
	pushq	%rbx
	pushq	%rcx
	pushq	%rdx
	pushq	%rsi
	pushq	%rdi
	pushq	%rbp
	pushq	%r8
	pushq	%r9
	pushq	%r10
	pushq	%r11
	pushq	%r12
	pushq	%r13
	pushq	%r14
	pushq	%r15
	.endm

	.macro RESTORE_REGS
	popq	%r15
	popq	%r14
	popq	%r13
	popq	%r12
	popq	%r11
	popq	%r10
	popq	%r9
	popq	%r8
	popq	%rbp
	popq	%rdi
	popq	%rsi
	popq	%rdx
	popq	%rcx
	popq	%rbx
	popq	%rax
	.endm

/* The common path, for the vectors whose saved code segment can be believed. */
trap_common:
	/*
	 * If this came from ring 3, %gs is the user's and has to become the
	 * kernel's before anything reads it.
	 *
	 * The processor does not do this — unlike the stack, which a gate
	 * switches, the segment base is left exactly as the interrupted code
	 * had it.  So a fault from a user program arrives with %gs pointing
	 * wherever that program put it, and any kernel code reading %gs:0
	 * would be following a pointer a user chose.  Nothing in this path
	 * reads it today; the day something does, it must already be right,
	 * because the symptom then is not a crash.
	 *
	 * The saved code segment is the only trustworthy record of where the
	 * trap came from, and its low two bits are the ring.  It sits three
	 * words above the vector this stub pushed.
	 */
	testb	$3, 24(%rsp)
	jz	1f
	swapgs
1:
	SAVE_REGS

	movq	%rsp, %rdi		/* the frame is the argument */
	call	trap_dispatch

	RESTORE_REGS

	/*
	 * And back, on the same evidence — read again rather than remembered,
	 * because the handler is entitled to change where this returns to.
	 * The recovery from a fault in ring 3 does exactly that: it rewrites
	 * the frame to resume in the kernel, and a return that swapped on the
	 * strength of how it arrived would hand the kernel the user's %gs.
	 */
	testb	$3, 24(%rsp)
	jz	2f
	swapgs
2:
	addq	$16, %rsp		/* drop the vector and error code */
	iretq

/*
 * The path for the four vectors that can arrive inside the window (#440).
 *
 * It asks the machine instead of the frame.  IA32_GS_BASE holds the base
 * that is loaded *right now*, which is the one fact the window cannot
 * misrepresent: the kernel's block is in the upper half of the address space
 * and a user's base is in the lower half, always, because §11.1 gives the
 * two halves to two different owners and nothing may write a base that
 * crosses that line.  So the sign bit of the base is the answer.
 *
 * Bit 63 rather than a comparison against KERNEL_HALF_BASE, and they are the
 * same test: WRMSR refuses a non-canonical base, so every value this can
 * read is canonical, and for a canonical address bit 63 set and "at or above
 * 0xffff800000000000" are the same statement.  One instruction instead of
 * loading a 64-bit immediate and comparing.
 *
 * ⚠️ The test is only as good as that ownership.  Two things could break it,
 * and both are decided rather than assumed: CR4.FSGSBASE is left clear, so
 * ring 3 cannot write its own base at all, and a base arriving from outside
 * through thread state is refused unless it is in the lower half.  If either
 * ever changes, this instruction stops being a proof and the entry has to
 * find the per-CPU block without %gs instead.
 *
 * The cost is an RDMSR on four vectors, three of which mean the machine is
 * in trouble and the fourth of which is a debugger single-stepping.  Nothing
 * is being optimised on any of them.
 */
trap_paranoid:
	SAVE_REGS

	/*
	 * The frame's address before anything else goes on the stack — the two
	 * words pushed below sit under it, and the C side is handed the frame,
	 * not the stack pointer.
	 */
	movq	%rsp, %rdi

	movl	$MSR_GS_BASE, %ecx
	rdmsr				/* edx:eax, zero-extended into the 64s */
	shlq	$32, %rdx
	orq	%rdx, %rax
	movq	%rax, %rsi		/* the base the window left in place */

	xorl	%edx, %edx		/* and what this path decided to do */
	testq	%rsi, %rsi
	js	1f			/* upper half: already the kernel's */
	swapgs
	movl	$1, %edx
1:
	/*
	 * Kept, because the way out cannot work them out again.
	 *
	 * The exit has to undo exactly what the entry did, and by then the base
	 * is the kernel's either way — the evidence the entry read is gone.
	 * Nor can the base simply be written back with WRMSR: swapgs moved
	 * *both* halves of the pair, so restoring one of them would leave
	 * IA32_KERNEL_GS_BASE holding the user's base and the next entry
	 * swapping to it.  The inverse of swapgs is swapgs, which means the
	 * only thing that has to survive the call is whether it happened.
	 */
	pushq	%rsi			/* what was found */
	pushq	%rdx			/* whether it was exchanged */

	call	trap_dispatch_paranoid

	popq	%rdx
	popq	%rsi
	testl	%edx, %edx
	jz	2f
	swapgs
2:
	RESTORE_REGS

	addq	$16, %rsp		/* drop the vector and error code */
	iretq

/*
 * trap_probe_write(addr) — store a recognisable pattern at addr and report
 * whether the machine allowed it.  Returns 0 when the store completed and 1
 * when it faulted, the handler having been told to resume at
 * trap_probe_faulted.
 *
 * A pattern rather than a zero, so the caller can check that an allowed
 * store actually landed.  Storing zero would leave a caller unable to tell a
 * store that happened from one that never needed to: most of what it might
 * probe is zero already.
 *
 * In assembly on purpose.  The obvious C version — take the address of a
 * label, arm the handler with it, fault, resume there — is exactly the case
 * the compiler manual warns about: a label whose address escapes into
 * another function is not reachable in the control-flow graph, so the block
 * is optimised away and the address resolves to whatever is nearby.  It did
 * here, landing back in the prologue and looping.
 *
 * Two instructions and a known symbol have no such opinions.  The resume
 * point is in this same frame, which is what makes the return safe: iretq
 * restores the stack pointer too, and nothing here has moved it.
 */
	.globl	trap_probe_write
	.globl	trap_probe_faulted
	.align	16
trap_probe_write:
	movl	$0x5a5a5a5a, (%rdi)
	xorl	%eax, %eax		/* the store was allowed */
	ret
trap_probe_faulted:
	movl	$1, %eax		/* the store was refused */
	ret

/*
 * Probes that raise one particular exception each, so the stubs can be
 * checked against the machine rather than against the manual I read while
 * writing them.  Thirty-two of them were assigned an error-code convention
 * by hand, and until something fires them only two of those assignments have
 * ever been tested.
 *
 * All of them share trap_probe_faulted as their resume point and return the
 * same way as trap_probe_write: 0 if the instruction completed, 1 if the
 * handler had to step over it.  None pushes, so the frame the resume lands
 * in is still the caller's.
 */

/* Invalid opcode: no error code, and the CPU is certain about it. */
	.globl	trap_probe_ud
	.align	16
trap_probe_ud:
	ud2
	xorl	%eax, %eax
	ret

/* Breakpoint: a trap rather than a fault, so the saved rip is the one after. */
	.globl	trap_probe_bp
	.align	16
trap_probe_bp:
	int3
	xorl	%eax, %eax
	ret

/* Divide error: edx:eax over a zero divisor. */
	.globl	trap_probe_de
	.align	16
trap_probe_de:
	xorl	%edx, %edx
	movl	$1, %eax
	xorl	%ecx, %ecx
	divl	%ecx
	xorl	%eax, %eax
	ret

/*
 * General protection: load a selector past the end of the GDT.  The point of
 * this one is the error code — the CPU pushes the offending selector, which
 * is the case the stubs must *not* invent a zero for.
 */
	.globl	trap_probe_gp
	.align	16
trap_probe_gp:
	movw	$0x30, %ax
	movw	%ax, %ds
	xorl	%eax, %eax
	ret

/*
 * The stub addresses, in vector order, so trap_init() can install them
 * without needing two hundred and fifty-six declarations of things that
 * differ only by a number.
 *
 * The first thirty-two are written out because they are the ones worth
 * reading against the error-code table above; the rest are generated the
 * same way they were defined, from the same counter, so the two cannot
 * disagree about how many there are.
 */
	.section .rodata
	.align	8
	.globl	isr_table
isr_table:
	.quad	isr_0,  isr_1,  isr_2,  isr_3,  isr_4,  isr_5,  isr_6,  isr_7
	.quad	isr_8,  isr_9,  isr_10, isr_11, isr_12, isr_13, isr_14, isr_15
	.quad	isr_16, isr_17, isr_18, isr_19, isr_20, isr_21, isr_22, isr_23
	.quad	isr_24, isr_25, isr_26, isr_27, isr_28, isr_29, isr_30, isr_31

	.altmacro
	.macro ISR_ENTRY vec
	.quad	isr_\vec
	.endm

	vec = 32
	.rept 224
	ISR_ENTRY %vec
	vec = vec + 1
	.endr
	.noaltmacro

/*
 * ⚠️ Back to .text, and this line is not decoration.
 *
 * The file switched to .rodata further up for the vector table, and an
 * assembler stays where it was put: code appended after that lands in a
 * section the kernel maps read-only and NO-EXECUTE.  It assembles, it links,
 * and `nm' reports the symbols with a lower-case `r' -- and the first jump
 * to one of them faults.  Caught here by reading nm's type letter, which is
 * the only place it showed (#453).
 */
	.section .text

/*
 * ── Returning to a thread rather than from a trap (#453) ──────────────
 *
 * The machine-independent kernel has four ways of leaving the kernel that
 * are not "return from the trap that got us here", and all four end in the
 * same place: restore the thread's saved user frame and iretq.
 *
 * The difference from trap_common's tail above is where the frame comes
 * from.  There, it is on the stack because the processor and SAVE_REGS put
 * it there.  Here, the thread is being resumed from somewhere else entirely
 * -- an exception was handled, a new thread is starting, a system call is
 * being re-run -- and the frame to restore is the one in the thread's pcb.
 *
 * ⚠️ These do not return.  Each switches to its thread's frame and leaves
 * ring 0; there is no caller left to come back to, which is why they are
 * written here rather than as C functions with a tail call the compiler
 * would be entitled to turn into something else.
 *
 * ⚠️ And each takes pending ASTs FIRST, which they did not (#422).  i386 gets
 * this for nothing -- its thread_bootstrap_return is a label on
 * return_from_trap, whose first act is the AST check -- and here the three
 * went straight to act_user_frame.  A thread created by thread_create() has
 * AST_APC pending and suspends itself when it takes it; without the check it
 * never took it, and reached act_user_frame with no frame to return to.  See
 * thread_return_ast() in trap/trap.c.
 */

	.globl	thread_exception_return
	.globl	thread_bootstrap_return
	.globl	restart_mach_syscall

/*
 * An exception has been handled and the thread should carry on where it was.
 *
 * The frame in the pcb is what trap entry saved on the way in, possibly
 * rewritten by thread_set_state() while the thread was stopped -- which is
 * the whole point: a debugger that moved the instruction pointer expects the
 * thread to resume there.
 */
thread_exception_return:
	callq	thread_return_ast	/* AST_APC first: see trap.c */
	callq	act_user_frame		/* %rax = this thread's trap frame */
	movq	%rax, %rsp
	jmp	thread_frame_return

/*
 * A thread that has never run, starting for the first time.
 *
 * Identical to the above, and separate because the machine-independent side
 * calls them from different places and a machine may want to distinguish
 * them.  This one does not -- a thread that has never run has a frame
 * prepared by thread_set_state(), and resuming it is resuming it.
 */
thread_bootstrap_return:
	callq	thread_return_ast	/* AST_APC first: see trap.c */
	callq	act_user_frame
	movq	%rax, %rsp
	jmp	thread_frame_return

/*
 * Run the system call again.
 *
 * ⚠️ The frame's rip still points AT the syscall instruction, not past it:
 * the entry path saves rcx, which SYSCALL loaded with the return address,
 * and the return path puts it back -- so "again" is achieved by returning
 * normally to an instruction pointer that was never advanced.  Nothing here
 * has to rewind anything, and anything that did would be wrong.
 */
restart_mach_syscall:
	callq	thread_return_ast	/* AST_APC first: see trap.c */
	callq	act_user_frame
	movq	%rax, %rsp
	jmp	thread_frame_return

/*
 * The tail all three share: the frame is at %rsp, we are going to ring 3.
 *
 * swapgs unconditionally, because every one of these is a return TO USER --
 * unlike trap_common's tail, which has to test, because a trap may have come
 * from either ring.
 */
thread_frame_return:
	RESTORE_REGS
	swapgs
	addq	$16, %rsp		/* drop the vector and error code */
	iretq
