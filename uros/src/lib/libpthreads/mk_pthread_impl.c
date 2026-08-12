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
 * 
 */
/*
 * MkLinux
 */

/*
 * This program will generate the stuff necessary to "publish" the POSIX
 * header <pthread.h> in a machine dependent fashion.
 */

#include <pthread_internals.h>
#include <stdio.h>

int
main(void)
{
	printf("/*\n");
	printf(" * POSIX Real-Time Threads - Machine implementation details\n");
	printf(" *   *** CAUTION!! Do not edit!!\n");
	printf(" */\n");
	printf("\n");
	printf("#define __PTHREAD_SIZE__           %ld\n", (long) sizeof(struct _pthread)-sizeof(long));
	printf("#define __PTHREAD_ATTR_SIZE__      %ld\n", (long) sizeof(pthread_attr_t)-sizeof(long));
	printf("#define __PTHREAD_MUTEXATTR_SIZE__ %ld\n", (long) sizeof(pthread_mutexattr_t)-sizeof(long));
	printf("#define __PTHREAD_MUTEX_SIZE__     %ld\n", (long) sizeof(pthread_mutex_t)-sizeof(long));
	printf("#define __PTHREAD_CONDATTR_SIZE__  %ld\n", (long) sizeof(pthread_condattr_t)-sizeof(long));
	printf("#define __PTHREAD_COND_SIZE__      %ld\n", (long) sizeof(pthread_cond_t)-sizeof(long));
	printf("#define __PTHREAD_ONCE_SIZE__      %ld\n", (long) sizeof(pthread_once_t)-sizeof(long));
	/*
	 * ⚠️ The five below were in the generated header and NOT in this
	 * generator.  pthread_rwlock, pthread_barrier and pthread_spin arrived
	 * with #82 and their sizes were written into AT386/pthread_impl.h by
	 * hand -- into a file whose first line says "*** CAUTION!! Do not
	 * edit!!" -- because no rule ran this program to say otherwise.
	 *
	 * They are the ones that could cost something.  pthread_t is a POINTER
	 * to a library-allocated object, so a short __PTHREAD_SIZE__ never
	 * reaches a caller's memory; a rwlock, a barrier and a spinlock are
	 * declared BY VALUE by the client, so their opaque buffer is the
	 * client's own storage and a number that is too small is written past.
	 */
	printf("#define __PTHREAD_RWLOCKATTR_SIZE__ %ld\n", (long) sizeof(pthread_rwlockattr_t)-sizeof(long));
	printf("#define __PTHREAD_RWLOCK_SIZE__    %ld\n", (long) sizeof(pthread_rwlock_t)-sizeof(long));
	printf("#define __PTHREAD_BARRIERATTR_SIZE__ %ld\n", (long) sizeof(pthread_barrierattr_t)-sizeof(long));
	printf("#define __PTHREAD_BARRIER_SIZE__   %ld\n", (long) sizeof(pthread_barrier_t)-sizeof(long));
	printf("#define __PTHREAD_SPIN_SIZE__      %ld\n", (long) sizeof(pthread_spinlock_t)-sizeof(long));
	printf("/*\n");
	printf(" * [Internal] data structure signatures\n");
	printf(" */\n");
	printf("#define _PTHREAD_MUTEX_SIG_init		0x%08X\n", _PTHREAD_MUTEX_SIG_init);
	printf("#define _PTHREAD_COND_SIG_init		0x%08X\n", _PTHREAD_COND_SIG_init);
	printf("#define _PTHREAD_ONCE_SIG_init		0x%08X\n", _PTHREAD_ONCE_SIG_init);
	printf("#define _PTHREAD_RWLOCK_SIG		0x%08X\n", _PTHREAD_RWLOCK_SIG);
	printf("/*\n");
	printf(" * POSIX scheduling policies \n");
	printf(" */\n");
	printf("\n");
	printf("#define SCHED_OTHER      %d\n", SCHED_OTHER);
	printf("#define SCHED_FIFO       %d\n", SCHED_FIFO);
	printf("#define SCHED_RR         %d\n", SCHED_RR);
	printf("#define __SCHED_PARAM_SIZE__       %ld\n", (long) sizeof(struct sched_param)-sizeof(int));
	/*
	 * ⚠️ return, not exit(): this file includes <stdio.h> and nothing else,
	 * and the tree's stdio does not declare exit().  It compiled in 1994
	 * because an implicit declaration was a warning; it is an error now, and
	 * that is one of the reasons nothing had run this generator in years.
	 * main returning zero says the same thing and needs no header.
	 */
	return 0;
} 
