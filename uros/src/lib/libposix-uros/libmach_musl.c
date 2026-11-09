/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * libposix-uros -- the four names of libmach a program of musl has from here
 * (#917).
 *
 * libmach_core is libmach less what musl has, and it leaves fprintf_stderr.c
 * and panic.c out with the printf and the exit they stand on.  But
 * mach_error() prints through fprintf_stderr(), port_obj.c calls panic(), and
 * <mach.h> and <mach_init.h> declare panic_init() and vprintf_stderr_func:
 * a program of musl that reached any of them did not link.  Here they are,
 * with the same contracts, on musl's stdio.
 *
 *   fprintf_stderr()	 prints through vprintf_stderr_func when a program
 *			 has set it, as libmach's does, and otherwise to
 *			 musl's stderr, where libmach's went to its console
 *			 printf.
 *
 *   panic()		 says "panic: " and the message on stderr.  A program
 *			 that gave panic_init() the host port asks it to enter
 *			 the kernel debugger, as libmach's panic() does; one
 *			 that gave none, or a kernel that has no debugger, ends
 *			 with abort(), where libmach's faulted on purpose at
 *			 0xDEAD and spun.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include <mach.h>		/* panic, panic_init, vprintf_stderr_func */
#include <mach/mach_host.h>	/* host_reboot */

/*
 * Declared in libmach's <sa_mach/stdio.h>, the standalone stdio, which a file
 * of musl cannot include beside musl's <stdio.h>.  libmach's fprintf_stderr.c
 * defines it with this type.
 */
int fprintf_stderr(const char *format, ...);

int (*vprintf_stderr_func)(const char *format, va_list ap);

int
fprintf_stderr(const char *format, ...)
{
	va_list	ap;
	int	r;

	va_start(ap, format);
	if (vprintf_stderr_func != NULL)
		r = (*vprintf_stderr_func)(format, ap);
	else
		r = vfprintf(stderr, format, ap);
	va_end(ap);
	return r;
}

static mach_port_t	panic_host_port = MACH_PORT_NULL;

void
panic_init(mach_port_t port)
{
	panic_host_port = port;
}

/* libmach's panic.c asks for the debugger with this flag. */
#define	PANIC_RB_DEBUGGER	0x1000

void
panic(const char *s, ...)
{
	va_list	ap;

	fputs("panic: ", stderr);
	va_start(ap, s);
	vfprintf(stderr, s, ap);
	va_end(ap);
	fputc('\n', stderr);
	if (panic_host_port != MACH_PORT_NULL)
		(void) host_reboot(panic_host_port, PANIC_RB_DEBUGGER);
	abort();
}
