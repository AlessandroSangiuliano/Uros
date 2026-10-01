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

#ifndef	_I386_CLOCK_WATCH_H_
#define	_I386_CLOCK_WATCH_H_

/*
 * The application processors watch processor 0's tick and say so when it
 * stops (#599).  See clock_watch.c.
 *
 *   clock_watch_init()	 the boot line that says the watch exists, so that a
 *			 log without a stop line can be told from a kernel
 *			 that could not have printed one;
 *   clock_watch_tick()	 from each application processor's own tick;
 *   clock_watch_spin()	 from a wait on another processor, which takes no
 *			 tick of its own while it spins (MACHINE_SPIN_WATCH
 *			 in <i386/lock.h> calls it);
 *   clock_watch_nmi()	 from processor 0's NMI handler: 1 when the NMI was
 *			 the watch's, after printing where processor 0 was.
 */

struct i386_saved_state;

extern void	clock_watch_init(void);
extern void	clock_watch_tick(int cpu);
extern void	clock_watch_spin(const char *where);
extern int	clock_watch_nmi(struct i386_saved_state *regs);

#if defined(ABLATE_599_MASK_PIT) || defined(ABLATE_599_CLI_SPIN)
extern void	clock_watch_ablate(int cpu);	/* from hardclock */
#endif
#ifdef ABLATE_599_SPL_LEAK
extern void	clock_watch_ablate_spl_leak(void); /* from thread_switch */
#endif
#ifdef ABLATE_599_NESTED_PRINTF
extern void	clock_watch_ablate_nested_printf(void); /* from consolewrite */
#endif

#endif	/* _I386_CLOCK_WATCH_H_ */
