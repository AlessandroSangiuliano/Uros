/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The Mach trap stubs, x86-64 (#426).
 *
 * <mach/syscall_sw.h> lists every trap once, machine-independently, as
 *
 *	kernel_trap(name, number, argument_count)
 *
 * and includes this file to say what one expands to.  So the whole trap
 * surface of the system — every server, every library, every test reaches the
 * kernel through here — is three macros wide, and this is the machine's half.
 *
 * ══ What a stub is, and what it is not ════════════════════════════════
 *
 * A C function, callable with a C prototype, whose body is the instruction.
 * The kernel's side of the contract is written down in
 * <machine/syscall/syscall.h> and read from there rather than restated:
 * SYSCALL_MACH_BASE below is that file's definition, so the number a stub
 * puts in %rax and the number the entry path subtracts cannot drift apart.
 *
 * ⚠️ It is NOT the i386 sequence with wider registers.  Two things differ and
 * both are decisions:
 *
 *   the instruction   i386 enters through SYSENTER, reached by the SVC macro
 *                     in <machine/asm.h>.  ⚠️ libmach also carries an
 *                     i386/SYS.h and an i386/asm.h whose SVC is
 *                     `lcall $7,$0' — the BSD call gate of 1988.  Those are
 *                     dead heritage: nothing includes them on the live path,
 *                     and porting them would have been porting the wrong
 *                     thing (#44 chose SYSENTER deliberately).
 *
 *   the numbering     i386 puts Mach traps at NEGATIVE call numbers and the
 *                     entry negates.  Here they are a RANGE above
 *                     SYSCALL_MACH_BASE, so the entry's bound check stays one
 *                     unsigned comparison — see <machine/syscall/syscall.h>
 *                     for why that was worth a different convention.
 *
 * The machine-independent list still carries the i386 numbers, because they
 * are also the INDEX into mach_trap_table[] and that table is the same on
 * every architecture.  So the arithmetic here is a translation, done once, at
 * assembly time: trap -14 is table entry 14 is call number 0x10e.
 */

#ifndef	_MACH_X86_64_SYSCALL_SW_H_
#define _MACH_X86_64_SYSCALL_SW_H_

#include <machine/syscall/syscall.h>

#ifdef	__ASSEMBLER__

/*
 * ══ Where the arguments go ════════════════════════════════════════════
 *
 * A caller has put them where the C ABI says: rdi, rsi, rdx, rcx, r8, r9, and
 * the rest on the stack above the return address.  The kernel wants them
 * where <machine/syscall/syscall.h> says: rdi, rsi, rdx, R10, r8, r9, and the
 * rest in rbx and r12-r15.  So a stub is a translation between two
 * conventions, and the whole of it is:
 *
 *   the fourth       rcx -> r10, because SYSCALL destroys rcx to hold the
 *                    return address.  One move, and only for traps that have
 *                    a fourth argument.
 *
 *   seven and up     off the caller's stack and into the callee-saved
 *                    registers.  Which means SAVING them first: they are the
 *                    caller's, and a stub that clobbered them would be a
 *                    function that breaks its own ABI.  Sixteen traps in the
 *                    table are this wide; mach_msg_overwrite_trap is one.
 *
 * ⚠️ The offsets are counted from the stack as it is AFTER the saves, which
 * is why they carry (nargs-6) in them: K pushes move the return address to
 * K*8(%rsp), and the seventh argument sits one word above that.  Written as
 * arithmetic on the argument count rather than as five sets of constants,
 * because five sets of constants is five places to get one wrong and no way
 * to tell.
 */
	.macro	MACH_TRAP_STUB name, number, nargs
	.text
	.globl	\name
	.type	\name,@function
	.align	16
\name:
	.if	\nargs > 11
	.error	"a Mach trap with more arguments than the contract carries"
	.endif

	.if	\nargs > 6
	pushq	%rbx
	.endif
	.if	\nargs > 7
	pushq	%r12
	.endif
	.if	\nargs > 8
	pushq	%r13
	.endif
	.if	\nargs > 9
	pushq	%r14
	.endif
	.if	\nargs > 10
	pushq	%r15
	.endif

	.if	\nargs > 6
	movq	((\nargs-6)*8+8)(%rsp), %rbx
	.endif
	.if	\nargs > 7
	movq	((\nargs-6)*8+16)(%rsp), %r12
	.endif
	.if	\nargs > 8
	movq	((\nargs-6)*8+24)(%rsp), %r13
	.endif
	.if	\nargs > 9
	movq	((\nargs-6)*8+32)(%rsp), %r14
	.endif
	.if	\nargs > 10
	movq	((\nargs-6)*8+40)(%rsp), %r15
	.endif

	.if	\nargs >= 4
	movq	%rcx, %r10
	.endif

	movq	$\number, %rax
	syscall

	.if	\nargs > 10
	popq	%r15
	.endif
	.if	\nargs > 9
	popq	%r14
	.endif
	.if	\nargs > 8
	popq	%r13
	.endif
	.if	\nargs > 7
	popq	%r12
	.endif
	.if	\nargs > 6
	popq	%rbx
	.endif

	ret
	.size	\name, . - \name
	.endm

/*
 * The number a stub asks for, from the number the list carries.
 *
 * The list is machine-independent and its numbers are i386's, which are
 * negative; the same magnitude is the index into mach_trap_table[], which is
 * what this architecture adds to its base.  Subtracting a negative is the
 * whole translation, and it happens at assembly time.
 */
#define kernel_trap(trap_name,trap_number,number_args)			\
	MACH_TRAP_STUB trap_name, (SYSCALL_MACH_BASE - (trap_number)), number_args

/*
 * ⚠️ rpc_trap EMITS NOTHING, and that is the point.
 *
 * i386 reaches these two through a SECOND call gate — RPC_SVC, vector 0xf —
 * for the collocated-server RPC path.  There is no second gate here, and the
 * kernel does not pretend otherwise: kern/syscall_sw.c guards entries 35 and
 * 36 with `#ifdef i386' and fills them with not_implemented everywhere else.
 *
 * So a stub would be a function that assembles, links, and asks the kernel
 * for a trap the kernel has said it does not have — which is exactly the
 * shape of a plausible-looking stub that answers wrongly.  Emitting nothing
 * makes a caller fail at LINK time with an undefined symbol naming the trap
 * it wanted, which is a better sentence than any run-time error code.
 *
 * Nothing in the tree calls them today (checked against a grep that finds the
 * kernel's own references, so it could have found a caller).  The day the
 * collocated RPC path is wanted on this architecture, it is a design question
 * about SYSCALL and not a macro to fill in here.
 */
#define rpc_trap(trap_name,trap_number,number_args)	/* no such gate here */

#endif	/* __ASSEMBLER__ */

#endif	/* _MACH_X86_64_SYSCALL_SW_H_ */
