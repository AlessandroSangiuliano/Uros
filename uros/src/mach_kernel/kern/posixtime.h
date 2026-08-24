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

#ifndef	_KERN_POSIXTIME_H_
#define	_KERN_POSIXTIME_H_

#include <mach_assert.h>
#include <mach/clock_types.h>
#include <mach/time_value.h>	/* time_value_t -- utime_get/utime_set speak it */

/*
 * Universal (Posix) time declarations.
 */

/*
 * The battery-backed clock: the machine's only source of wall-clock time at
 * boot, read once by utime_init() to seed `time'.
 *
 * ⚠️ tvalspec_t, and it matters.  This is also the c_gettime slot of struct
 * clock_ops in <kern/clock.h>, which is where i386 installs the same function
 * -- so tvalspec_t (seconds and NANOseconds) is the real contract.
 *
 * kern/posixtime.c used to carry its own `extern' saying time_value_t
 * (seconds and MICROseconds) and then cast `&time' to match.  Both structures
 * are two 32-bit words, so the compiler saw nothing and the sub-second field
 * came out in the wrong unit -- invisible only because this clock has no
 * sub-second resolution and always writes zero there.  Declared once, here,
 * so the two halves are finally compared (#448, #453).
 */
extern kern_return_t	bbc_gettime(tvalspec_t *cur_time);
extern kern_return_t	bbc_settime(tvalspec_t *new_time);

/*
 * Universal (Posix) time initialization.
 */
extern void	utime_init(void);

/*
 * Universal (Posix) time tick. This is called from the clock
 * interrupt path at splclock() interrupt level.
 */
extern void	utime_tick(void);

/*
 * Read and write the Universal (Posix) time, with no host port involved.
 *
 * host_get_time() and host_set_time() are these two plus the privilege check;
 * a clock device sits below that check, having been reached through a port
 * the kernel handed out, so it needs the mechanism without the question.
 * ⚠️ utime_get() carries the mapped copy's seqlock and utime_set() the
 * master-processor binding -- both are why these exist as functions instead
 * of as two lines each caller writes for itself.
 */
extern void	utime_get(time_value_t *current_time);
extern void	utime_set(time_value_t new_time);

#endif	/* _KERN_POSIXTIME_H_ */
