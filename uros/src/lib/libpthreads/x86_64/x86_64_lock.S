/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The spin lock libpthreads builds everything else on (#425).
 *
 * The shape is i386's and the reasoning is worth keeping: try once, and if
 * the lock is held, spin _spin_tries times before giving the processor away
 * with thread_switch.  Spinning is right when the holder is running on
 * another processor and about to release; yielding is right when it is not
 * running at all, and a spin lock cannot tell the two apart, so it does the
 * cheap thing for a while and then the correct one.
 *
 * ⚠️ No <machine/asm.h>: symbols are declared with .globl/.type/.size, the
 * way the rest of the x86-64 port does it.
 */

/*
 * void _spin_lock(pthread_lock_t *lock)
 *
 * ⚠️ xchgq and not xchgl, and this is the one that would have been silent.
 * pthread_lock_t is `long', which is EIGHT bytes here -- a 32-bit exchange
 * would take the lock by writing half the word, leave the top half holding
 * whatever it held, and unlock by clearing only the same half.  Two threads
 * could then hold it at once with nothing in the log to say so.
 *
 * ⚠️ xchg with a memory operand is atomic without a `lock' prefix -- the
 * processor asserts it for that instruction alone.  Written out because the
 * missing prefix looks like an omission and is not.
 */
	.text
	.globl	_spin_lock
	.type	_spin_lock,@function
_spin_lock:
.Lrespin:
	movq	$1, %rax
	xchgq	%rax, (%rdi)		/* the lock is the first argument */
	testq	%rax, %rax		/* was it already held? */
	jne	.Llocked
.Lret:
	ret

.Llocked:
	/*
	 * How many times to try before yielding.  ⚠️ RIP-relative, where
	 * i386 has to `call' the next instruction and pop the return address
	 * to discover where it is: this machine can name a global relative to
	 * the instruction pointer, so the whole PIC thunk disappears.
	 */
	movl	_spin_tries(%rip), %edx
.Ltry:
	movq	$1, %rax
	xchgq	%rax, (%rdi)
	testq	%rax, %rax
	je	.Lret
	decl	%edx
	jne	.Ltry

	/*
	 * Give the processor away and start again.  thread_switch takes three
	 * arguments and they all go in registers here -- new_thread, option
	 * and timeout, all zero: "someone else, whoever, no deadline".
	 *
	 * ⚠️ %rdi holds the lock and thread_switch's first argument is also
	 * %rdi, so it is saved across the call.  The stack must be sixteen-byte
	 * aligned at the call, and pushing one register would leave it eight
	 * out -- hence the pair.
	 */
	pushq	%rdi
	pushq	%rdi
	xorl	%edi, %edi		/* new_thread = 0 */
	xorl	%esi, %esi		/* option     = 0 */
	xorl	%edx, %edx		/* timeout    = 0 */
	call	thread_switch
	popq	%rdi
	popq	%rdi
	jmp	.Lrespin
	.size	_spin_lock, . - _spin_lock

/*
 * void _spin_unlock(pthread_lock_t *lock)
 *
 * The exchange again rather than a plain store, for the same reason i386 uses
 * one: it is a full barrier, so everything the critical section wrote is
 * visible before the lock reads as free.
 */
	.globl	_spin_unlock
	.type	_spin_unlock,@function
_spin_unlock:
	xorl	%eax, %eax
	xchgq	%rax, (%rdi)
	ret
	.size	_spin_unlock, . - _spin_unlock
