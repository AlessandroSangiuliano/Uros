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

#ifndef	_KERN_STARTUP_H_
#define	_KERN_STARTUP_H_

#include <cpus.h>

/*
 * Kernel and machine startup declarations
 */

/* Initialize kernel */
extern void	setup_main(void);

/* Initialize machine dependent stuff */
extern void	machine_init(void);

/*
 * The second machine-dependent point in setup_main(), and the one that has a
 * working kernel around it (#453).
 *
 * machine_init() above runs early, when nothing else exists.  This one runs
 * after task_init(), act_init(), thread_init() and subsystem_init(), so the
 * machine may do here what it could not do there: take a lock, print, fail an
 * assertion, or trip a fault it can survive.  Everything a machine wants to
 * check about itself, and every late initialisation that needs the kernel to
 * be up, belongs here.
 *
 * ⚠️ Declared here rather than as an `extern' inside setup_main().  It used to
 * be four block-local externs naming i386 functions by hand -- fpu_sanity_check
 * and hwp_init_cpu among them -- which means the machine-independent kernel
 * held a private opinion about their signatures that no other machine could
 * satisfy and no compiler could ever check against the definitions (#448).
 * Every machine must define this, and the empty body is a legitimate answer.
 */
extern void	machine_kernel_ready(void);

#if	NCPUS > 1

extern void	slave_main(void);

/*
 * The following must be implemented in machine dependent code.
 */

/* Slave cpu initialization */
extern void	slave_machine_init(void);

/* Start slave processors */
extern void	start_other_cpus(void);

#endif	/* NCPUS > 1 */
#endif	/* _KERN_STARTUP_H_ */
