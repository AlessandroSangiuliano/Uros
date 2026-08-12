/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * What libpthreads needs from this machine (#425).
 *
 * ── A note on the thread pointer, because there isn't one ─────────────
 *
 * The obvious question on x86-64 is "where is %fs", and the answer is that
 * this library does not use a thread pointer at all.  A thread's struct
 * _pthread lives at the top of that thread's own stack, and pthread_self()
 * finds it by masking the stack pointer -- pthread.c's STACK_SELF.  That is
 * arithmetic, it is machine-independent, and it works here unchanged.
 *
 * ⚠️ Which is not the same as saying %fs is never needed.  musl reads its
 * thread control block through it, and the stack canary lives at %fs:0x28
 * (#414); libmach sidesteps that today with -mstack-protector-guard=global.
 * When the TCB arrives it will dictate the layout, and the decision belongs
 * there rather than here -- putting a thread pointer in now would mean
 * choosing that layout before the thing that defines it exists.
 */

#ifndef	_PTHREAD_MACHDEP_H_
#define _PTHREAD_MACHDEP_H_

/*
 * ⚠️ `long' here is EIGHT bytes, where i386's is four, and that is deliberate
 * rather than inherited: the lock word is what the atomic instructions in
 * x86_64_lock.S operate on, and `xchg' on a 64-bit register wants a 64-bit
 * location.  A four-byte lock beside an eight-byte access is the kind of
 * mismatch that shows up as a neighbouring field changing by itself.
 */
typedef long pthread_lock_t;

#define	LOCK_INIT(l)		((l) = 0)
#define	LOCK_INITIALIZER	0

#undef	STACK_GROWS_UP

/*
 * Abandon this stack and run fn(arg) on another one.
 *
 * Used where a thread cannot return -- it is finishing on a stack that is
 * about to be handed back -- so the sequence never returns and has no epilogue
 * to preserve.
 *
 * ⚠️ The differences from i386's are the ABI's, not the register names'.  The
 * argument goes in %rdi rather than onto the stack, so there is nothing to
 * push; and the alignment is the ordinary rule -- rsp sixteen-byte aligned AT
 * the call, so that fn sees it eight past a boundary once `call' has pushed
 * the return address.  i386 aligns and then subtracts twelve to make room for
 * the one word it pushes; here the `and' is the whole of it.
 */
#define PTHREAD_RESTART_ON_STACK(top, fn, arg)				\
	__asm__ __volatile__ ("movq %0, %%rsp\n\t"			\
			      "andq $-16, %%rsp\n\t"			\
			      "movq %1, %%rdi\n\t"			\
			      "call *%2"				\
			      : /* never returns */			\
			      : "r" ((unsigned long) (top)),		\
				"r" ((void *) (arg)),			\
				"r" ((void *) (fn))			\
			      : "memory")

#endif	/* _PTHREAD_MACHDEP_H_ */
