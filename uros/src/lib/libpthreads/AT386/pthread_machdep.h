/*
 * Copyright 1991-1998 by Open Software Foundation, Inc. 
 *              All Rights Reserved 
 *  
 * Permission to use, copy, modify, and distribute this software and 
 * its documentation for any purpose and without fee is hereby granted, 
 * provided that the above copyright notice appears in all copies and 
 * that both the copyright notice and this permission notice appear in 
 * supporting documentation. 
 *  
 * OSF DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE 
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS 
 * FOR A PARTICULAR PURPOSE. 
 *  
 * IN NO EVENT SHALL OSF BE LIABLE FOR ANY SPECIAL, INDIRECT, OR 
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM 
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN ACTION OF CONTRACT, 
 * NEGLIGENCE, OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION 
 * WITH THE USE OR PERFORMANCE OF THIS SOFTWARE. 
 */
/*
 * MkLinux
 */

/* Machine-dependent definitions for pthread internals. */

typedef long pthread_lock_t;

#define LOCK_INIT(l)	((l) = 0)
#define LOCK_INITIALIZER 0

#undef STACK_GROWS_UP

/*
 *	#444: re-enter on a fresh stack.
 *
 *	A pooled thread is woken inside the exit path of the work it has just
 *	finished, several frames deep, and its next act is to run a new user
 *	function.  Calling that from where it stands nests one whole exit path
 *	per reuse and never unwinds it: a thousand create/join cycles walk the
 *	stack into its guard page, which is issue #444.
 *
 *	Returning instead of jumping is not an option, because pthread_exit
 *	may be called by the user at any depth and must not return there.  So
 *	the thread drops its stack pointer back to the top -- everything below
 *	is the finished work's frames and is not needed again -- and re-enters.
 *	Every reuse then starts at the depth the first one did.
 *
 *	libpthreads is standalone: these servers link -nostdlib and have no
 *	musl, so setjmp/longjmp are not available and this is the machine
 *	-dependent header where the alternative belongs.
 *
 *	The alignment dance is the i386 SysV convention: esp must be 16-byte
 *	aligned at the call instruction, so the argument push has to land it
 *	back on a multiple of sixteen.
 */
#define PTHREAD_RESTART_ON_STACK(top, fn, arg)				\
	__asm__ __volatile__ ("movl %0, %%esp\n\t"			\
			      "andl $-16, %%esp\n\t"			\
			      "subl $12, %%esp\n\t"			\
			      "pushl %1\n\t"				\
			      "call *%2"				\
			      : /* never returns */			\
			      : "r" ((unsigned long) (top)),		\
				"r" ((void *) (arg)),			\
				"r" ((void *) (fn))			\
			      : "memory")
