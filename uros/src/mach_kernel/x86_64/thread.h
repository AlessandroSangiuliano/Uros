/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * <machine/thread.h> for x86-64 (#408).
 *
 * i386's version of this header is 223 lines.  Almost none of it is the
 * machine-independent tree's business: STACK_IKS, STACK_IEL, curr_gdt and
 * curr_ktss are used by *zero* machine-independent files -- they describe
 * where i386 keeps saved state on a kernel stack, and only i386 code looks.
 * Counted rather than assumed, and the count is what kept this header short.
 *
 * What the MI tree actually reaches for here is one macro.  Two more it
 * asks about and can do without:
 *
 *   MACHINE_STACK_STASH   kern/thread.h supplies its own under #ifndef, and
 *                         the generic one is right for this machine.
 *   MACHINE_FAST_EXCEPTION  tested with #ifdef.  i386 has a fast exception
 *                         path; x86-64 does not yet, and claiming one would
 *                         send kern/exception.c down 350 lines of code with
 *                         nothing behind them.  Left undefined on purpose.
 *
 * The process control block lives in <machine/thread_act.h>, as it does on
 * i386 -- the MI tree reaches it through thr_act->mact.pcb.
 */

#ifndef _X86_64_THREAD_H_
#define _X86_64_THREAD_H_

/*
 * The return address of the caller of the function that asks.
 *
 * kern/lock.c uses it to record who took a lock, passing the address of its
 * own first argument: one word below it on the stack is the return address
 * the call pushed.  That is true of the SysV AMD64 call sequence as it is of
 * i386's, with the word twice as wide -- and vm_offset_t is what changes
 * size, which is why the cast is written in those terms rather than in a
 * fixed width.
 *
 * ⚠️ Only valid for a frame the compiler has not optimised away.  The kernel
 * is built -fno-omit-frame-pointer, which is what makes that safe here.
 */
#define	GET_RETURN_PC(addr)	(*((vm_offset_t *)(addr) - 1))

#endif /* _X86_64_THREAD_H_ */
