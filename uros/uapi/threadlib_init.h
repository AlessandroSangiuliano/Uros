/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The hooks crt0 calls between the kernel's entry and main() (#422).
 *
 * A thread library that wants the first thread to run on a stack of its own
 * defines _threadlib_init_routine; crt0 calls it before main and MOVES THE
 * STACK POINTER to what it returns.  libmach defines all three weak and null,
 * so a server that links no thread library starts on the kernel's stack.
 *
 * ── Why they are declared here and not in each library ────────────────
 *
 * 🔥 Because they were not, and the four definitions of one object did not
 * agree.  libcthreads said vm_offset_t, libpthreads and libmach's own crt0
 * said int, librthreads said `int (*)()' with a cast in front of it to silence
 * the compilers of 1990.  Nothing compares declarations across translation
 * units, so all four linked, and on i386 all four were the same 32 bits: the
 * disagreement had no consequence and therefore no symptom.
 *
 * On x86-64 it has one.  pthread_init returns a stack pointer, the `int'
 * truncated it, crt0 sign-extended what was left back to sixty-four bits, and
 * bootstrap's first push landed on 0xffffffffffeffde0 -- an address that reads
 * like a kernel one and was never mapped.  The instruction that faulted was
 * `call main'.
 *
 * ⚠️ crt0.c had this written down.  It said, beside a movq it had already
 * widened, that the real fix was the declaration and that the declaration was
 * #425's -- and #425 closed without it.  A note about a defect is not a check
 * for it.  With one declaration in front of all four definitions, the next
 * disagreement is a compile error rather than a comment somebody has to find.
 *
 * ⚠️ vm_offset_t and not uintptr_t: this is a Mach address, it is what
 * libcthreads had right all along, and it is what the thread libraries already
 * use for the stacks they allocate.
 */

#ifndef	_THREADLIB_INIT_H_
#define	_THREADLIB_INIT_H_

#include <mach/machine/vm_types.h>

/*
 * Called before main, returns the stack to switch to -- or zero to stay on the
 * one the kernel gave.
 */
extern vm_offset_t	(*_threadlib_init_routine)(void);

/* The same hook under its older name, which some startup code still uses. */
extern vm_offset_t	(*_thread_init_routine)(void);

/* Called with main's return value, and does not come back. */
extern void		(*_threadlib_exit_routine)(int);

/*
 * ⚠️ An int, and it stays one: this one answers whether initialisation
 * SUCCEEDED.  It is not an address, and widening it to match its neighbours
 * would be tidiness winning over meaning.
 */
extern int		(*_mach_init_routine)(void);

#endif	/* _THREADLIB_INIT_H_ */
