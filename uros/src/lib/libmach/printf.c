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

#define EXPORT_BOOLEAN
#include <mach/boolean.h>
#include "externs.h"	/* _doprnt, the one formatter (#432) */
#include <mach.h>
#include <mach/mach_traps.h>	/* mach_print: the console that needs no port */
#include <mach_error.h>		/* mach_error_string */
#include <sa_mach.h>
#include <device/device.h>
#include <device/tty_status.h>
#include <mach/bootstrap.h>
#include <stdarg.h>
#include <stdio.h>

/*
 *  Common code for printf et al.
 *
 *  The calling routine typically takes a variable number of arguments,
 *  and passes the address of the first one.  This implementation
 *  assumes a straightforward, stack implementation, aligned to the
 *  machine's wordsize.  Increasing addresses are assumed to point to
 *  successive arguments (left-to-right), as is the case for a machine
 *  with a downward-growing stack with arguments pushed right-to-left.
 *
 *  This version implements the following printf features:
 *
 *	%d	decimal conversion
 *	%u	unsigned conversion
 *	%x	hexadecimal conversion
 *	%X	hexadecimal conversion with capital letters
 *	%o	octal conversion
 *	%c	character
 *	%s	string
 *	%m.n	field width, precision
 *	%-m.n	left adjustment
 *	%0m.n	zero-padding
 *	%*.*	width and precision taken from arguments
 *
 *  This version does not implement %f, %e, or %g.  It accepts, but
 *  ignores, an `l' as in %ld, %lo, %lx, and %lu, and therefore will not
 *  work correctly on machines for which sizeof(long) != sizeof(int).
 *  It does not even parse %D, %O, or %U; you should be using %ld, %o and
 *  %lu if you mean long conversion.
 *
 *  As mentioned, this version does not return any reasonable value.
 *
 *  Permission is granted to use, modify, or propagate this code as
 *  long as this notice is incorporated.
 *
 *  Steve Summit 3/25/87
 */

/*
 * Added formats for decoding device registers:
 *
 * printf("reg = %b", regval, "<base><arg>*")
 *
 * where <base> is the output base expressed as a control character:
 * i.e. '\10' gives octal, '\20' gives hex.  Each <arg> is a sequence of
 * characters, the first of which gives the bit number to be inspected
 * (origin 1), and the rest (up to a control character (<= 32)) give the
 * name of the register.  Thus
 *	printf("reg = %b\n", 3, "\10\2BITTWO\1BITONE")
 * would produce
 *	reg = 3<BITTWO,BITONE>
 *
 * If the second character in <arg> is also a control character, it
 * indicates the last bit of a bit field.  In this case, printf will extract
 * bits <1> to <2> and print it.  Characters following the second control
 * character are printed before the bit field.
 *	printf("reg = %b\n", 0xb, "\10\4\3FIELD1=\2BITTWO\1BITONE")
 * would produce
 *	reg = b<FIELD1=2,BITONE>
 */
/*
 * Added for general use:
 *	#	prefix for alternate format:
 *		0x (0X) for hex
 *		leading 0 for octal
 *	+	print '+' if positive
 *	blank	print ' ' if positive
 *
 *	z	signed hexadecimal
 *	r	signed, 'radix'
 *	n	unsigned, 'radix'
 *
 *	D,U,O,Z	same as corresponding lower-case versions
 *		(compatibility)
 */

#define isdigit(d) ((d) >= '0' && (d) <= '9')
#define Ctod(c) ((c) - '0')

#define MAXBUF (sizeof(long int) * 8)		 /* enough for binary */

/*
 * 64-bit divmod by an arbitrary base, implemented bit-by-bit to avoid
 * pulling libgcc's __udivmoddi4 into the freestanding -nostdlib build.
 * Slow but called only by printf for at most ~20 iterations per number.
 */
static unsigned long long
udivmod_ll(unsigned long long n, unsigned int base, unsigned int *rem)
{
	unsigned long long q = 0;
	unsigned int r = 0;
	int i;

	for (i = 63; i >= 0; i--) {
		r = (r << 1) | (unsigned int)((n >> i) & 1ULL);
		if (r >= base) {
			r -= base;
			q |= (1ULL << i);
		}
	}
	*rem = r;
	return q;
}

/*
 * 🔴 A SECOND FORMATTER LIVED HERE, and it is gone (#432 audit).
 *
 * printf() called a `static _doprnt' defined in this file, which shadowed
 * the one libmach exports.  The two had different conversion sets: this one
 * had %p and honoured the length modifier, doprnt.c's had neither.  So
 * inside one library printf() and sprintf() ran different formatters, and
 * whether %p worked depended on where you were printing to.
 *
 * 🔑 That is also how the defect survived: #415 taught the KERNEL's copy the
 * `l' modifier, and there were two more copies nobody looked at.  There is
 * one now, in doprnt.c, and this file calls it.
 */


mach_port_t console_port;
static security_token_t null_security_token;

/*
 * Optional printf mirror hook (#199 prep).
 *
 * After the in-kernel VGA driver is removed, the "console" device is
 * serial-only.  Userspace tasks that want their printf output to also
 * appear on the VGA screen install a mirror via printf_set_mirror():
 * libgpu_console (or anyone else) registers a callback that flush()
 * invokes for every drained chunk.
 *
 * Hook invocation is non-blocking by contract — the hook is expected
 * to be a MIG simpleroutine send (gpu_text_puts) or equivalent fire-
 * and-forget primitive.  Failures inside the hook are silent: the
 * primary serial path stays the source of truth for log output.
 */
static void (*printf_mirror_hook)(const char *buf, size_t len) = NULL;

void
printf_set_mirror(void (*hook)(const char *buf, size_t len))
{
	printf_mirror_hook = hook;
}

/*
 * A number, printed by the path that cannot use printf.
 *
 * ⚠️ Needed because mach_error_string does not know every code it can be
 * handed: the device layer's are not in the Mach error space at all -- they
 * are small decimals in <device/device_types.h>, D_NO_SUCH_DEVICE is 2502 --
 * so an honest table answers "unknown error code" and the caller is left with
 * a sentence that names no cause.  Sixteen lines of hexadecimal is the whole
 * difference between a report and a shrug.
 */
static void
mach_print_code(kern_return_t code)
{
	static const char digits[] = "0123456789abcdef";
	unsigned int v = (unsigned int) code;
	char buf[11];
	int i;

	buf[0] = '0';
	buf[1] = 'x';
	for (i = 0; i < 8; i++)
		buf[2 + i] = digits[(v >> ((7 - i) * 4)) & 0xF];
	buf[10] = '\0';

	mach_print(buf);
}

void
printf_init(mach_port_t device_server_port)
{
	kern_return_t kr;

	/*
	 * Open the "console" device (kd on i386).  The kd driver writes to
	 * VGA text RAM and mirrors every character to COM0 (serial) via
	 * com_putc() in cnputc()/kdstart(), so output appears on both the
	 * QEMU window and -serial stdio without needing a separate com0 open.
	 */
	kr = device_open(device_server_port, MACH_PORT_NULL, D_READ|D_WRITE,
			 null_security_token, (char *)"console",
			 &console_port);
	
	if (kr != KERN_SUCCESS) {
		/*
		 * Say why, before saying that.
		 *
		 * The sentinel below is a deliberate page fault with a
		 * recognizable address in cr2, and on its own it is a report
		 * with the reason removed: every way this call can fail
		 * arrives as the same address, and the one thing worth
		 * knowing -- which failure -- is the thing it does not carry.
		 *
		 * mach_print is the right instrument for it and the only one
		 * available here: this IS the printing path, so it cannot
		 * report through itself, and the trap needs no port, no
		 * device and no initialisation.  mach_error_string is already
		 * in this library.
		 *
		 * ⚠️ It matters most on a target with no console device at
		 * all -- x86-64 has dev_name_count == 0 on purpose, rather
		 * than a stub console that would report success and write
		 * nowhere -- because there the FIRST thing every server does
		 * is fail here, and it used to fail as a bare address.
		 */
		mach_print("libmach: device_open(\"console\") failed: ");
		mach_print(mach_error_string(kr));
		mach_print(" (");
		mach_print_code(kr);
		mach_print(")\r\n");

		*(volatile int *)0xDEADBEEF = 0xDEADBEEF;
		console_port = MACH_PORT_NULL;
	}
}

#define	PRINTF_BUFMAX	128

int printf_bufmax = PRINTF_BUFMAX;

struct printf_state {
	char buf[PRINTF_BUFMAX + 1]; /* extra for '\r\n' */
	unsigned int index;
	unsigned int total;
};

/* Try to obtain the console port.  This is not guaranteed to work (we
   may not have a valid bootstrap port) so it is only called when we would
   otherwise have to drop a printf(). */
static void
get_console_port(void)
{
	kern_return_t kr;
	mach_port_t device_server_port, junk_port;
	mach_msg_type_number_t count;
	security_token_t token;

	if (bootstrap_port == MACH_PORT_NULL) {
	    kr = task_get_special_port(mach_task_self(), TASK_BOOTSTRAP_PORT,
				       &bootstrap_port);
	    if (kr != KERN_SUCCESS)
		return;
	}
	kr = bootstrap_ports(bootstrap_port, &junk_port, &device_server_port,
			     &junk_port, &junk_port, &junk_port);
	if (kr != KERN_SUCCESS)
	    return;
	count = TASK_SECURITY_TOKEN_COUNT;
	kr = task_info(mach_task_self(), TASK_SECURITY_TOKEN,
		       (task_info_t) &token, &count);
	if (kr != KERN_SUCCESS)
	    return;
	kr = device_open(device_server_port, MACH_PORT_NULL, D_READ|D_WRITE,
			 token, (char *) "console", &console_port);
}

static void
flush(struct printf_state *state)
{
	io_buf_len_t amt;
	int offset, count;
	kern_return_t kr;

	/*
	 * It is likely that not all characters will be written if
	 * the console is a slow serial device. Make sure we look at
	 * the 'amt' return value and make sure all characters have
	 * been written. 
	 */
	/* If a mirror hook is installed (e.g. libgpu_console), tee the
	 * full buffer to it before draining to the serial console.
	 * Sending the pre-tee buffer (instead of looping on amt below)
	 * keeps the mirror's call rate independent of serial speed. */
	if (printf_mirror_hook != NULL && state->index > 0)
		(*printf_mirror_hook)(state->buf, state->index);

	offset = 0;
	count = state->index;
	while (count) {
	    kr = device_write_inband(console_port, 0, 0,
				     &state->buf[offset], count, &amt);
	    if (kr != D_SUCCESS) {
		if (console_port == MACH_PORT_NULL) {
		    /* If the console port is null it's probably because
		       printf_init() hasn't been called.  This can happen
		       for instance if the thread-package startup function
		       needs to print an error message.  Rather than drop
		       the message on the floor, we attempt to fabricate a
		       console port and only if that fails do we discard
		       the message.  */
		    get_console_port();
		    if (console_port != MACH_PORT_NULL)
			continue;
		}
		break;
	    }
	    count -= amt;
	    offset += amt;
	    if (count) {
		/*
		 * We were unable to send all the data. Sleep for a little
		 * while, then try to send the rest.
		 */
		usleep(USEC_PER_SEC/4);  /* .25 second sleep */ 
	    }
	}
#ifdef	TTY_DRAIN
	{
		int word;
		/* flush console output */
		(void) device_set_status(console_port, TTY_DRAIN, &word, 0);
	}
#endif	/* TTY_DRAIN */
	state->total += state->index;
	state->index = 0;
}

static void
outchar(void *arg, int c)
{
	struct printf_state *state = (struct printf_state *) arg;

	if (c == '\n') {
	    state->buf[state->index] = '\r';
	    state->index++;
	}
	state->buf[state->index] = c;
	state->index++;

	if (state->index >= printf_bufmax)
	    flush(state);
}

/*
 * Printing (to console)
 */
int
vprintf(const char *fmt, va_list args)
{
	struct printf_state state;

	state.index = state.total = 0;
	_doprnt(fmt, args, 0, outchar, (char *) &state);

	if (state.index != 0)
	    flush(&state);
	return (state.total);
}

/*VARARGS1*/
int
printf(const char *fmt, ...)
{
	int ret;
	va_list	args;

	va_start(args, fmt);
	ret = vprintf(fmt, args);
	va_end(args);
	return (ret);
}

static void
savechar(void *arg, int c)
{
	*(*(char **)arg)++ = c;
}

/* sprintf / vsprintf live in sprintf.c (#106 — see libmach/CMakeLists.txt). */

int
getchar(void)
{
	unsigned char ch;
	mach_msg_type_number_t count;

	/* initialise count so as not to cause problems in mig */
	count = sizeof(ch);
	(void) device_read_inband(console_port, 0, 0,
				  sizeof(ch), (char *)&ch, &count);
	return((int)ch);
}
