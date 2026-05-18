/*
 * Copyright (c) 2026 Alessandro Sangiuliano <alex22_7@hotmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * POSIX signal definitions for Uros standalone (sa_mach) environment.
 *
 * Signals are implemented entirely in userspace by libpthreads.
 * Delivery uses Mach semaphores for thread wakeup.
 */

#ifndef _SA_MACH_SIGNAL_H
#define _SA_MACH_SIGNAL_H

/*
 * Signal numbers (POSIX.1-2001 subset)
 */
#define SIGHUP		1
#define SIGINT		2
#define SIGQUIT		3
#define SIGILL		4
#define SIGTRAP		5
#define SIGABRT		6
#define SIGBUS		7
#define SIGFPE		8
#define SIGKILL		9
#define SIGUSR1		10
#define SIGSEGV		11
#define SIGUSR2		12
#define SIGPIPE		13
#define SIGALRM		14
#define SIGTERM		15
#define SIGCHLD		17
#define SIGCONT		18
#define SIGSTOP		19
#define SIGTSTP		20
#define SIGTTIN		21
#define SIGTTOU		22
#define SIGURG		23
#define SIGXCPU		24
#define SIGXFSZ		25
#define SIGVTALRM	26
#define SIGPROF		27
#define SIGWINCH	28
#define SIGIO		29
#define SIGSYS		31

#define _NSIG		32	/* Total signal slots (0 unused, 1-31 valid) */

/*
 * Signal set type — 32 signals fit in one unsigned long.
 */
typedef unsigned long	sigset_t;

/*
 * Signal set manipulation (POSIX.1)
 */
#define sigemptyset(set)	(*(set) = 0, 0)
#define sigfillset(set)		(*(set) = ~0UL, 0)
#define sigaddset(set, sig)	(*(set) |=  (1UL << ((sig) - 1)), 0)
#define sigdelset(set, sig)	(*(set) &= ~(1UL << ((sig) - 1)), 0)
#define sigismember(set, sig)	((*(set) & (1UL << ((sig) - 1))) != 0)

/*
 * How to change the signal mask (pthread_sigmask 'how' argument)
 */
#define SIG_BLOCK	0	/* Add signals to mask */
#define SIG_UNBLOCK	1	/* Remove signals from mask */
#define SIG_SETMASK	2	/* Replace mask entirely */

#endif /* _SA_MACH_SIGNAL_H */
