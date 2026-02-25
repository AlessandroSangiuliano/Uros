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
 * Mach Operating System
 * Copyright (c) 1991,1990,1989 Carnegie Mellon University
 * All Rights Reserved.
 * 
 * Permission to use, copy, modify and distribute this software and its
 * documentation is hereby granted, provided that both the copyright
 * notice and this permission notice appear in all copies of the
 * software, derivative works or modified versions, and any portions
 * thereof, and that both notices appear in supporting documentation.
 * 
 * CARNEGIE MELLON ALLOWS FREE USE OF THIS SOFTWARE IN ITS "AS IS"
 * CONDITION.  CARNEGIE MELLON DISCLAIMS ANY LIABILITY OF ANY KIND FOR
 * ANY DAMAGES WHATSOEVER RESULTING FROM THE USE OF THIS SOFTWARE.
 * 
 * Carnegie Mellon requests users of this software to return to
 * 
 *  Software Distribution Coordinator  or  Software.Distribution@CS.CMU.EDU
 *  School of Computer Science
 *  Carnegie Mellon University
 *  Pittsburgh PA 15213-3890
 * 
 * any improvements or extensions that they make and grant Carnegie Mellon
 * the rights to redistribute these changes.
 */
/*
 * MkLinux
 */

#include <stdarg.h>
#include <stddef.h>

static void
savechar(char *arg, int c)
{
	*(*(char **)arg)++ = c;
}

int
vsprintf(char *s, const char *fmt, va_list args)
{
	_doprnt(fmt, args, 0, (void (*)()) savechar, (char *) &s);
	*s = 0;
}

int
sprintf(char *s, const char *fmt, ...)
{
	va_list	args;

	va_start(args, fmt);
	vsprintf(s, fmt, args);
	va_end(args);
}

struct snprintf_state {
	char	*buf;
	size_t	remaining;
};

static void
savechar_n(char *arg, int c)
{
	struct snprintf_state *state = (struct snprintf_state *)arg;

	if (state->remaining > 1) {
		*state->buf++ = c;
		state->remaining--;
	}
}

int
vsnprintf(char *s, size_t n, const char *fmt, va_list args)
{
	struct snprintf_state state;

	if (n == 0)
		return 0;
	state.buf = s;
	state.remaining = n;
	_doprnt(fmt, args, 0, (void (*)()) savechar_n, (char *) &state);
	*state.buf = '\0';
	return (state.buf - s);
}

int
snprintf(char *s, size_t n, const char *fmt, ...)
{
	va_list	args;
	int ret;

	va_start(args, fmt);
	ret = vsnprintf(s, n, fmt, args);
	va_end(args);
	return ret;
}
