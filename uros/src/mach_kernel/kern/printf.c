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
/* CMU_HIST */
/*
 * Revision 2.9.8.2  92/09/15  17:21:39  jeffreyh
 * 	Use print_lock for MBUS as well, unused only for SYMMETRY.
 * 	[92/07/17            bernadat]
 * 
 * Revision 2.9.8.1  92/02/18  19:09:25  jeffreyh
 * 	Use printf_lock only for CBUS. Does not work with Sequent
 * 	[91/12/11            bernadat]
 * 
 * Revision 2.9.3.1  91/09/26  04:47:46  bernadat
 * 	Support for Corollary MP
 * 	Use a printf_lock and print cpu number
 * 	[91/06/25            bernadat]
 * 
 * Revision 2.9  91/06/06  17:07:22  jsb
 * 	Added gets (derived from boot_gets).
 * 	[91/05/14  09:18:50  jsb]
 * 
 * Revision 2.8  91/05/14  16:44:59  mrt
 * 	Correcting copyright
 * 
 * Revision 2.7  91/02/05  17:28:14  mrt
 * 	Changed to new Mach copyright
 * 	[91/02/01  16:15:41  mrt]
 * 
 * Revision 2.6  90/10/25  14:45:18  rwd
 * 	Purged uprintf.
 * 	[90/10/21            rpd]
 * 
 * Revision 2.5  90/08/27  22:03:08  dbg
 * 	Add extra formats to printf: '#- ' prefixes, %z (signed hex),
 * 	%r and %n (signed and unsigned, current radix).
 * 	[90/08/20            dbg]
 * 
 * Revision 2.4  90/01/11  11:43:44  dbg
 * 	De-linted.
 * 	[90/01/03            dbg]
 * 
 * Revision 2.3  89/11/29  14:09:06  af
 * 	Ooops, a typo.
 * 	[89/10/29  09:34:26  af]
 * 
 * 	Changed the case for %c to load ints and not chars. Or
 * 	else it is byte-order dependent since C passes char as ints.
 * 	[89/10/13            af]
 * 
 * 	Turned the unused 'file descriptor' field for _doprnt and putchar
 * 	into a more useful pointer to an (optional) specialized putchar
 * 	routine.  This can be used, for instance, to divert debugging
 * 	printouts to some specialized interface or IOP.
 * 	[89/10/09            af]
 * 
 * Revision 2.2  89/09/25  11:00:58  rwd
 * 	Added case 'X' same as 'x' for now.
 * 	[89/09/20            rwd]
 * 
 * Revision 2.1  89/08/03  15:51:14  rwd
 * Created.
 * 
 *  8-Aug-88  David Golub (dbg) at Carnegie-Mellon University
 *	Converted for MACH kernel use.  Removed %r, %R, %b; added %b
 *	from Berkeley's kernel to print bit fields in device registers;
 *	changed to use varargs.
 *
 */
/* CMU_ENDHIST */
/* 
 * Mach Operating System
 * Copyright (c) 1991,1990,1989,1988 Carnegie Mellon University
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
 */

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
 *  To write, for example, fprintf() using this routine, the code
 *
 *	fprintf(fd, format, args)
 *	FILE *fd;
 *	char *format;
 *	{
 *	_doprnt(format, &args, fd);
 *	}
 *
 *  would suffice.  (This example does not handle the fprintf's "return
 *  value" correctly, but who looks at the return value of fprintf
 *  anyway?)
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
 *  This version implements %f/%F (fixed decimal), %e/%E (scientific
 *  notation), and %g/%G (shorter of the two), with full support for
 *  width, precision, sign, padding, and the # flag.
 *  It accepts, but
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
 *
 * The %B format is like %b but the bits are numbered from the most
 * significant (the bit weighted 31), which is called 1, to the least
 * significant, called 32.
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

#include <platforms.h>
#include <mach/boolean.h>
#include <cpus.h>
#include <kern/cpu_number.h>
#include <kern/lock.h>
#include <kern/thread.h>
#include <kern/misc_protos.h>
#include <kern/klog.h>
#include <kern/spl.h>		/* #655: sprintf() at splhigh */
#include <kern/cpu_data.h>	/* #757: get_interrupt_level, the writer */
#include <stdarg.h>
#include <stdint.h>		/* #757: uintptr_t */
#include <string.h>

/*
 * Forward declarations
 */
void printnum(
	register unsigned int	u,
	register int		base,
	void			(*putc)(char));

#define	MP_PRINTF	((NCPUS > 1) || MACH_ASSERT)

/*
 * Per-CPU "{N} " prefix on every printf line.  Handy for attributing
 * concurrent SMP output to a CPU, but very noisy in normal runs (the flag
 * latches on after the first AP print, so every line gets tagged).  Off by
 * default; flip to 1 (or build with -DMP_PRINTF_CPU_PREFIX=1) to re-enable.
 * The printf_lock serialization below stays on regardless.
 */
#ifndef MP_PRINTF_CPU_PREFIX
#define	MP_PRINTF_CPU_PREFIX	0
#endif

#define isdigit(d) ((d) >= '0' && (d) <= '9')
#define Ctod(c) ((c) - '0')

#define MAXBUF (sizeof(long int) * 8)		 /* enough for binary */

void
printnum(
	register unsigned int	u,		/* number to print */
	register int		base,
	void			(*putc)(char))
{
	char	buf[MAXBUF];	/* build number here */
	register char *	p = &buf[MAXBUF-1];
	static char digs[] = "0123456789abcdef";

	do {
	    *p-- = digs[u % base];
	    u /= base;
	} while (u != 0);

	while (++p != &buf[MAXBUF])
	    (*putc)(*p);

}

boolean_t	_doprnt_truncates = FALSE;

/*
 * How wide the next integer argument is (#415).  Short names locally; the
 * shared spellings the debugger uses are DOPRNT_LEN_* in misc_protos.h, and
 * they are the same numbers.
 */
#define	LEN_INT		DOPRNT_LEN_INT
#define	LEN_LONG	DOPRNT_LEN_LONG
#define	LEN_LONGLONG	DOPRNT_LEN_LONGLONG

/*
 * Take the next integer argument at the width the conversion asked for.
 *
 * Split out of the parser so that the debugger's extra conversions fetch the
 * same way this one does, rather than carrying a second copy of the rule
 * (#415).  The signed side must fetch signed: an int arrives sign-extended
 * into its slot, and reading it as a long takes that sign for data.
 */
long
_doprnt_signed_arg(
	const struct doprnt_spec	*spec,
	va_list				*argp)
{
	switch (spec->ds_lensize) {
	case DOPRNT_LEN_LONGLONG:
		return (long) va_arg(*argp, long long);
	case DOPRNT_LEN_LONG:
		return va_arg(*argp, long);
	default:
		return va_arg(*argp, int);
	}
}

unsigned long
_doprnt_unsigned_arg(
	const struct doprnt_spec	*spec,
	va_list				*argp)
{
	switch (spec->ds_lensize) {
	case DOPRNT_LEN_LONGLONG:
		return (unsigned long) va_arg(*argp, unsigned long long);
	case DOPRNT_LEN_LONG:
		return va_arg(*argp, unsigned long);
	default:
		return va_arg(*argp, unsigned int);
	}
}

/*
 * Emit a number that has already been fetched, with the width, padding, sign
 * and alternate-form prefix the conversion asked for.
 *
 * Also split out for the debugger's benefit: %r and %n differ from %d and %u
 * only in which base they use, and everything after choosing the base is the
 * same work.  Duplicating it would mean the two could drift, and the way that
 * shows up is one conversion padding differently from its neighbour in a
 * column of output nobody is reading closely.
 */
void
_doprnt_number(
	unsigned long			u,
	int				base,
	int				capitals,
	int				sign_char,
	const struct doprnt_spec	*spec,
	void				(*putc)(char))
{
	char		buf[MAXBUF];		/* build number here */
	register char	*p = &buf[MAXBUF-1];
	static char	digits[] = "0123456789abcdef0123456789ABCDEF";
	char		*prefix = 0;
	int		length = spec->ds_length;

	if (_doprnt_truncates)
		u = (long)((int)(u));

	if (u != 0 && spec->ds_altfmt) {
		if (base == 8)
			prefix = "0";
		else if (base == 16)
			prefix = "0x";
	}

	do {
		/* Print in the correct case */
		*p-- = digits[(u % base)+capitals];
		u /= base;
	} while (u != 0);

	length -= (&buf[MAXBUF-1] - p);
	if (sign_char)
		length--;
	if (prefix)
		length -= strlen((const char *) prefix);

	if (spec->ds_padc == ' ' && !spec->ds_ladjust) {
		/* blank padding goes before prefix */
		while (--length >= 0)
			(*putc)(' ');
	}
	if (sign_char)
		(*putc)(sign_char);
	if (prefix)
		while (*prefix)
			(*putc)(*prefix++);
	if (spec->ds_padc == '0') {
		/* zero padding goes after sign and prefix */
		while (--length >= 0)
			(*putc)('0');
	}
	while (++p != &buf[MAXBUF])
		(*putc)(*p);

	if (spec->ds_ladjust) {
		while (--length >= 0)
			(*putc)(' ');
	}
}

void
_doprnt(
	register const char	*fmt,
	va_list			*argp,
						/* character output routine */
	void			(*putc)(char),
	int			radix)		/* what %r and %n print in */
{
	_doprnt_ext(fmt, argp, putc, radix, (doprnt_ext_t) 0);
}

void
_doprnt_ext(
	register const char	*fmt,
	va_list			*argp,
						/* character output routine */
	void			(*putc)(char),
	int			radix,		/* what %r and %n print in */
	doprnt_ext_t		ext)		/* conversions outside C's set */
{
	struct doprnt_spec spec;
	int		length;
	int		prec;
	boolean_t	ladjust;
	char		padc;
	long		n;
	unsigned long	u;
	int		plus_sign;
	int		sign_char;
	boolean_t	altfmt, truncate;
	int		base;
	register char	c;
	int		capitals;
	int		lensize;

	while ((c = *fmt) != '\0') {
	    if (c != '%') {
		(*putc)(c);
		fmt++;
		continue;
	    }

	    fmt++;

	    length = 0;
	    prec = -1;
	    ladjust = FALSE;
	    padc = ' ';
	    plus_sign = 0;
	    sign_char = 0;
	    altfmt = FALSE;

	    while (TRUE) {
		c = *fmt;
		if (c == '#') {
		    altfmt = TRUE;
		}
		else if (c == '-') {
		    ladjust = TRUE;
		}
		else if (c == '+') {
		    plus_sign = '+';
		}
		else if (c == ' ') {
		    if (plus_sign == 0)
			plus_sign = ' ';
		}
		else
		    break;
		fmt++;
	    }

	    if (c == '0') {
		padc = '0';
		c = *++fmt;
	    }

	    if (isdigit(c)) {
		while(isdigit(c)) {
		    length = 10 * length + Ctod(c);
		    c = *++fmt;
		}
	    }
	    else if (c == '*') {
		length = va_arg(*argp, int);
		c = *++fmt;
		if (length < 0) {
		    ladjust = !ladjust;
		    length = -length;
		}
	    }

	    if (c == '.') {
		c = *++fmt;
		if (isdigit(c)) {
		    prec = 0;
		    while(isdigit(c)) {
			prec = 10 * prec + Ctod(c);
			c = *++fmt;
		    }
		}
		else if (c == '*') {
		    prec = va_arg(*argp, int);
		    c = *++fmt;
		}
	    }

	    /*
	     * The length modifier, which this formatter used to read and
	     * throw away (#415):
	     *
	     *   if (c == 'l')
	     *       c = *++fmt;  // need it if sizeof(int) < sizeof(long)
	     *
	     * The comment above it, in the 1987 original, says the rest: "It
	     * accepts, but ignores, an `l' ... and therefore will not work
	     * correctly on machines for which sizeof(long) != sizeof(int)."
	     * That machine is x86-64, and the note has been waiting here for
	     * thirty-nine years for somebody to arrive on one.
	     *
	     * Discarding it was survivable only because every conversion below
	     * then fetched a long regardless, which on i386 is the same four
	     * bytes an int would have been.  It is not the same eight bytes:
	     * an int argument read as a long takes its own value and whatever
	     * the ABI left in the upper half -- and for a *negative* int that
	     * upper half is not sign, so `printf("%d", -1)` becomes 4294967295
	     * and print_signed's `n >= 0` agrees that it is positive.
	     */
	    lensize = LEN_INT;
	    while (c == 'l') {
		lensize = (lensize == LEN_LONG) ? LEN_LONGLONG : LEN_LONG;
		c = *++fmt;
	    }
	    while (c == 'h') {
		/*
		 * Accepted and deliberately not acted on: default argument
		 * promotion has already widened a short or a char to an int
		 * by the time it reaches here, so there is nothing narrower
		 * to fetch. Recorded rather than silently skipped, which is
		 * how the `l' came to be ignored.
		 */
		c = *++fmt;
	    }

	    truncate = FALSE;
	    capitals=0;		/* Assume lower case printing */

	    /*
	     * Everything the parser worked out, in one place, so that a
	     * formatter built on this one is handed it rather than working it
	     * out again from a format string it would have to re-walk (#415).
	     */
	    spec.ds_conv	= c;
	    spec.ds_length	= length;
	    spec.ds_prec	= prec;
	    spec.ds_ladjust	= ladjust;
	    spec.ds_padc	= padc;
	    spec.ds_altfmt	= altfmt;
	    spec.ds_plus_sign	= plus_sign;
	    spec.ds_lensize	= lensize;
	    spec.ds_radix	= radix;

	    switch(c) {
		case 'c':
		    c = va_arg(*argp, int);
		    (*putc)(c);
		    break;

		case 's':
		{
		    register char *p;
		    register char *p2;

		    if (prec == -1)
			prec = 0x7fffffff;	/* MAXINT */

		    p = va_arg(*argp, char *);

		    if (p == (char *)0)
			p = "";

		    if (length > 0 && !ladjust) {
			n = 0;
			p2 = p;

			for (; *p != '\0' && n < prec; p++)
			    n++;

			p = p2;

			while (n < length) {
			    (*putc)(' ');
			    n++;
			}
		    }

		    n = 0;

		    while (*p != '\0') {
			if (++n > prec || (length > 0 && n > length))
			    break;

			(*putc)(*p++);
		    }

		    if (n < length && ladjust) {
			while (n < length) {
			    (*putc)(' ');
			    n++;
			}
		    }

		    break;
		}

		/*
		 * C's integer conversions, and only those.  The capitalised
		 * spellings this formatter used to accept -- %D, %O, %U -- were
		 * how the dialect said "long" before C had `l' to say it with,
		 * and they have gone to ddb/ with the rest of the dialect
		 * (#415).  %X stays: it is upper-case hex and standard, however
		 * much it looks like one of them.
		 */
		case 'o':
		    base = 8;
		    goto print_unsigned;

		case 'd':
		case 'i':
		    base = 10;
		    goto print_signed;

		case 'u':
		    base = 10;
		    goto print_unsigned;

		case 'x':
		    base = 16;
		    goto print_unsigned;

		case 'X':
		    base = 16;
		    capitals=16;	/* Print in upper case */
		    goto print_unsigned;

		case 'p':
		    /* Pointer: emit "0x" then the unsigned hex value, zero-
		     * padded to the host pointer width so columns align.
		     *
		     * #415: fetched at pointer width, not at whatever the
		     * format's length modifier happened to say.  A pointer is
		     * not an int here any more, and reading one as an int is
		     * how %p comes to print the lower half of an address.
		     */
		    base = 16;
		    if (length == 0) {
			length = (int)(2 * sizeof(void *));
			padc   = '0';
		    }
		    (*putc)('0');
		    (*putc)('x');
		    u = (unsigned long)(vm_offset_t) va_arg(*argp, void *);
		    goto print_num;

		/*
		 * Fetch at the width the conversion was written for, not at
		 * one width for all of them (#415).  On i386 every arm below
		 * reads the same four bytes and the distinction costs
		 * nothing; on x86-64 it is the difference between a value and
		 * a value with eight unrelated bytes attached.
		 *
		 * The signed side must fetch signed: an int arrives
		 * sign-extended into its slot, and reading it as a long takes
		 * that sign for data.
		 */
		print_signed:
		    n = _doprnt_signed_arg(&spec, argp);
		    if (n >= 0) {
			u = n;
			sign_char = plus_sign;
		    }
		    else {
			u = -n;
			sign_char = '-';
		    }
		    goto print_num;

		print_unsigned:
		    u = _doprnt_unsigned_arg(&spec, argp);
		    goto print_num;

		print_num:
		    /*
		     * spec carries the flags the parser found, but %p and the
		     * debugger's conversions adjust width and padding after
		     * that, so the two fields they touch are refreshed here.
		     */
		    spec.ds_length = length;
		    spec.ds_padc   = padc;
		    _doprnt_number(u, base, capitals, sign_char, &spec, putc);
		    break;


		/*
		 * ⚠️ Floating-point output, and only on a machine whose kernel
		 * may execute floating-point instructions (#453).
		 *
		 * The formatting below does real arithmetic on `double' --
		 * divides by ten to find the exponent, subtracts to split off
		 * the fraction.  On x86-64 this kernel is built
		 * -mgeneral-regs-only, deliberately: a kernel that touches
		 * vector or x87 state has to save and restore it on every
		 * entry from user mode, which is a cost paid by every system
		 * call so that printf can render a percentage.
		 *
		 * That is not a trade worth making, and it is not a historical
		 * solution worth keeping either -- a kernel prints diagnostics,
		 * and a diagnostic that needs a fraction can carry it as two
		 * integers.  So on a machine without kernel floating point the
		 * conversion is refused rather than approximated: %f prints
		 * <no-fp>, which is visible in a log and cannot be mistaken for
		 * a value.
		 */
		/*
		 * 🔴 The labels are OUTSIDE the #if, and that is a fix.
		 *
		 * They used to be inside it, so a kernel built with
		 * KERNEL_FLOAT_OK=0 -- x86-64, since #453 -- had no case for
		 * these at all, and the <no-fp> block below had no label
		 * reaching it: dead code, guarding nothing.  A %f there fell
		 * through to `default', which echoes the character and does
		 * NOT consume the double.  The eight bytes stayed in the
		 * argument list and every conversion after it read from the
		 * middle of them.
		 *
		 * So the comment above promised a property the code did not
		 * have, on the one target that had asked for it.
		 */
		case 'f':
		case 'F':
		case 'e':
		case 'E':
		case 'g':
		case 'G':
#if	KERNEL_FLOAT_OK
		{
		    /*
		     * Floating-point output.  Supports %f (fixed decimal),
		     * %e/%E (scientific notation), %g/%G (shorter of the two).
		     * The # flag forces a decimal point and, for %g/%G, suppresses
		     * trailing-zero stripping.
		     */
		    double		dval   = va_arg(*argp, double);
		    boolean_t		dneg   = FALSE;
		    unsigned long long	dint;
		    double		dfrac;
		    char		ibuf[24];	/* integer digits, null-terminated */
		    char		fbuf[20];	/* fractional digits */
		    char		expbuf[8];	/* exponent string, e.g. "e+03" */
		    char		*ip;
		    int			ilen, flen, elen;
		    int			fi, use_e, strip_zeros;
		    int			eval   = 0;
		    int			total, show_frac, efrac, effprec;
		    union { double d; unsigned long long u; } dchk;

		    if (prec < 0)
			prec = 6;

		    /* Detect NaN/Inf without triggering FP comparison edge-cases */
		    dchk.d = dval;
		    if (((dchk.u >> 52) & 0x7FFULL) == 0x7FFULL) {
			unsigned long long mant = dchk.u & 0x000FFFFFFFFFFFFFULL;
			const char *sp = mant ? "nan"
					      : (dchk.u >> 63 ? "-inf" : "inf");
			int slen = mant ? 3 : (dchk.u >> 63 ? 4 : 3);
			if (!ladjust)
			    while (slen < length) { (*putc)(' '); length--; }
			while (*sp)
			    (*putc)(*sp++);
			if (ladjust)
			    while (slen < length) { (*putc)(' '); length--; }
			break;
		    }

		    if (dval < 0.0) { dneg = TRUE; dval = -dval; }

		    use_e       = (c == 'e' || c == 'E');
		    strip_zeros = (c == 'g' || c == 'G');

		    /* Compute decimal exponent of the value */
		    if (dval != 0.0) {
			double t = dval;
			if (t >= 1.0) { while (t >= 10.0) { t /= 10.0; eval++; } }
			else          { while (t <  1.0)  { t *= 10.0; eval--; } }
		    }

		    /* %g/%G: choose %e or %f; precision counts significant digits */
		    if (strip_zeros) {
			effprec = (prec == 0) ? 1 : prec;
			use_e   = (eval < -4 || eval >= effprec);
			prec    = use_e ? effprec - 1 : effprec - eval - 1;
			if (prec < 0) prec = 0;
		    }

		    /* Normalise value to [1, 10) for %e output */
		    if (use_e && dval != 0.0) {
			if (eval > 0)
			    for (fi = 0; fi < eval;  fi++) dval /= 10.0;
			else
			    for (fi = 0; fi < -eval; fi++) dval *= 10.0;
		    }

		    /* Separate integer and fractional parts */
		    dint  = (unsigned long long)dval;
		    dfrac = dval - (double)dint;

		    /* Round to prec decimal places */
		    {
			double rnd = 0.5;
			for (fi = 0; fi < prec; fi++) rnd /= 10.0;
			dfrac += rnd;
			if (dfrac >= 1.0) {
			    dint++;
			    dfrac -= 1.0;
			    /* %e: renormalise if rounding produced 10.xxx */
			    if (use_e && dint >= 10) {
				dint = 1; dfrac = 0.0; eval++;
			    }
			}
		    }

		    /* Build fractional digit string */
		    flen = (prec < (int)sizeof(fbuf)) ? prec : (int)sizeof(fbuf) - 1;
		    for (fi = 0; fi < flen; fi++) {
			int d;
			dfrac *= 10.0;
			d = (int)dfrac;
			if (d > 9) d = 9;
			fbuf[fi] = '0' + d;
			dfrac -= (double)d;
		    }

		    /* Build integer digit string */
		    ip = ibuf + sizeof(ibuf) - 1;
		    *ip = '\0';
		    if (dint == 0) {
			*--ip = '0';
		    } else {
			unsigned long long t = dint;
			while (t > 0) { *--ip = '0' + (int)(t % 10); t /= 10; }
		    }
		    ilen = (int)((ibuf + sizeof(ibuf) - 1) - ip);

		    /* Build exponent string */
		    elen = 0;
		    if (use_e) {
			int a = (eval >= 0) ? eval : -eval;
			expbuf[elen++] = (c == 'E' || c == 'G') ? 'E' : 'e';
			expbuf[elen++] = (eval >= 0) ? '+' : '-';
			if (a >= 100) expbuf[elen++] = '0' + a / 100;
			expbuf[elen++] = '0' + (a / 10) % 10;
			expbuf[elen++] = '0' + a % 10;
		    }

		    /* %g/%G strips trailing fractional zeros (unless # flag) */
		    if (strip_zeros && !altfmt)
			while (flen > 0 && fbuf[flen - 1] == '0')
			    flen--;

		    /*
		     * show_frac: whether to emit '.' and fractional digits.
		     * efrac:     number of fractional digit positions to emit
		     *            (fbuf contents + zero-padding).
		     */
		    show_frac = altfmt      ? 1
			      : strip_zeros ? (flen > 0)
			      :               (prec > 0);
		    efrac = show_frac
			    ? ((!strip_zeros || altfmt) ? prec : flen)
			    : 0;

		    total = (dneg || plus_sign ? 1 : 0)
			  + ilen
			  + (show_frac ? 1 + efrac : 0)
			  + elen;

		    /* Right-align: space padding before sign */
		    if (!ladjust && padc == ' ')
			while (total < length) { (*putc)(' '); length--; }

		    if (dneg)          (*putc)('-');
		    else if (plus_sign) (*putc)(plus_sign);

		    /* Right-align: zero padding after sign */
		    if (!ladjust && padc == '0')
			while (total < length) { (*putc)('0'); length--; }

		    while (*ip) (*putc)(*ip++);

		    if (show_frac) {
			(*putc)('.');
			for (fi = 0; fi < flen; fi++) (*putc)(fbuf[fi]);
			for (; fi < efrac; fi++)      (*putc)('0');
		    }

		    for (fi = 0; fi < elen; fi++) (*putc)(expbuf[fi]);

		    /* Left-align: trailing space padding */
		    if (ladjust)
			while (total < length) { (*putc)(' '); length--; }
		    break;
		}
#else	/* KERNEL_FLOAT_OK */
		{
		    static const char	nofp[] = "<no-fp>";
		    const char		*p = nofp;

		    /*
		     * 🔴 THE ARGUMENT IS NOT CONSUMED, AND THAT IS NOT AN
		     * OMISSION.
		     *
		     * This used to be `(void) va_arg(*argp, double);', which
		     * looks like the careful thing to do -- skip the eight
		     * bytes so the conversions after it stay aligned.  It does
		     * not compile once the case labels above actually reach
		     * here: gcc answers
		     *
		     *	  error: SSE register argument with SSE disabled
		     *
		     * because on x86-64 a double travels in an XMM register,
		     * and a kernel built -mgeneral-regs-only never saved those
		     * to a va_list.  There is nothing at that offset to skip.
		     *
		     * 🔑 Which also says why skipping is not needed: a caller
		     * cannot PASS a double either.  On x86-64 the build forbids
		     * the compiler the registers it would travel in; on i386
		     * loading one takes an x87 instruction, and
		     * scripts/kernel-vector-check.py fails the build on those
		     * outside the few symbols that declare them.  So a %f here
		     * has no argument behind it -- this prints the marker for a
		     * thing that cannot happen, and stays in case it does.
		     */
		    while (*p != '\0')
			(*putc)(*p++);
		    break;
		}
#endif	/* KERNEL_FLOAT_OK */

		case '\0':
		    fmt--;
		    break;

		default:
		    /*
		     * Not one of C's.  A formatter built on this one gets it
		     * first, and only what nobody claims is echoed as the
		     * character it is -- which is what this did with every
		     * unknown conversion before there was anyone to ask (#415).
		     */
		    if (ext == (doprnt_ext_t) 0 || !(*ext)(&spec, argp, putc))
			(*putc)(c);
	    }
	fmt++;
	}
}

#if	MP_PRINTF 
boolean_t	new_printf_cpu_number = FALSE;
#endif	/* MP_PRINTF */


decl_simple_lock_data(,printf_lock)
decl_simple_lock_data(static, sprintf_lock)	/* #655: see sprintf() */

/*
 * #599: the processor that holds printf_lock, or -1.
 *
 * On i386 the spin lock leaves interrupts as the caller had them, and
 * consolewrite() holds printf_lock for a line at the device's pace with them
 * on.  An interrupt handler on that processor that calls printf() -- intnull()
 * for a vector nobody handles, among others -- used to spin for ever on the
 * lock its own interrupted thread held, before its EOI: the vector stayed in
 * service, and the tick's, in the same priority class, was never delivered
 * again.  #599's widening arm stopped the tick exactly that way.
 *
 * Only the holder writes its own number here, so a printf() that finds its
 * own number is certainly nested inside the holder.  It takes the path that
 * does not take printf_lock, the one DDB already takes (klog_putc, on it,
 * has the same check for its own lock): its line may land inside the
 * interrupted one, which is the price of not stopping the machine.  Masking
 * interrupts for the hold instead would keep them off for a line at the
 * device's pace, milliseconds under KVM (#567).
 */
volatile int	printf_lock_holder = -1;

static __inline__ int
printf_nested(void)
{
	return printf_lock_holder == cpu_number();
}

/*
 * #200: every char that goes to the console also lands in the kernel
 * ring buffer so userspace can drain it via host_get_log.  Used as
 * the putc callback in _doprnt and as a direct cnputc wrapper for
 * the MP cpu-number prefix.
 */
static void
klog_cnputc(char c)
{
	cnputc(c);
	klog_putc(c);
}

/*
 * ── 🔴 #757: A LINE STAYS A LINE, AND A REPORT A REPORT ───────────────
 *
 * printf_lock keeps ONE CALL together, and a line is often several: a phase
 * line of the profile is three, so another processor's whole line landed
 * inside it (#757; #578 and #644 were the same shape, fixed in their callers).
 * And a report of several lines -- one of the profile's dumps -- was
 * interleaved with another report printed at the same moment: on omen's metal
 * 43 of intra's 52 dumps a boot lost their phases (#737).
 *
 * Fixed here, at the source, and chosen for debugging, by four rules:
 *
 *   1. NOTHING IS HELD BACK.  Every call reaches the console's ring when it is
 *      made.  A line buffer per writer, emitted whole at its newline, would
 *      lose the first half of a line to a hang between its two calls -- and
 *      in a hang the last line is the one that matters.
 *   2. NO LINE IS SPLICED IN SILENCE.  Below, under printf_lock, the console
 *      knows whose line is open.  A different writer closes it first, with a
 *      mark naming whose it was; when that writer goes on, the rest starts a
 *      line of its own with a mark naming it again.  A reader rejoins the two
 *      by the name.
 *   3. A REPORT IS A BLOCK.  printf_block_begin()/printf_block_end() keep
 *      every other THREAD out of the console for a report's length; the
 *      holder is not preempted, and its bytes go to the ring, the wire after.
 *      A processor in an interrupt, the debugger and a nested printf are not
 *      kept out -- they print at once, and rule 2 marks where they did.
 *   4. EVERY INTRUSION SAYS WHO, and ordinary output carries no tag at all.
 *
 * The idea -- a record that knows its writer, a lock that keeps a multi-line
 * dump together -- was studied in Linux's printk; the design and the code here
 * are this kernel's.
 */

/*
 * Who is writing: a thread, by its address; or a processor in an interrupt,
 * odd so that it is never a thread's -- the interrupted thread did not write
 * what its interrupt printed.  get_interrupt_level() is i386's locore and, on
 * x86-64, trap_common() (#757).  PRINTF_WRITER_RAW is the debugger's and a
 * nested printf's, which print without printf_lock.
 */
typedef uintptr_t	printf_writer_t;

#define	PRINTF_WRITER_RAW	(~(printf_writer_t) 0)

static printf_writer_t
printf_writer_self(void)
{
	thread_t	t;
	printf_writer_t	w;

	/*
	 * get_interrupt_level() is this processor's, so it is read where the
	 * thread cannot move to another: #626 caught it read with preemption
	 * allowed, from printf()'s last line and from consolewrite().
	 */
	disable_preemption();
	if (get_interrupt_level() > 0 || (t = current_thread()) == THREAD_NULL)
		w = ((printf_writer_t) cpu_number() << 1) | 1;
	else
		w = (printf_writer_t) t;
	enable_preemption_no_check();
	return w;
}

/*
 * The console's line, under printf_lock: whose line is open, and whose line
 * another writer cut and who has not gone on with it yet.  The set is small
 * because a cut is rare; a writer it has to give up -- it was full -- goes on
 * at the start of a line without its mark, and is counted.
 */
#define	PRINTF_CUT_SLOTS	16

struct printf_lines {
	printf_writer_t	open;			/* 0: no line is open */
	printf_writer_t	cut[PRINTF_CUT_SLOTS];	/* 0: a free slot */
	unsigned int	cut_next;		/* the slot a full set gives up */
	unsigned int	cut_lost;		/* writers given up so */
};

static struct printf_lines	printf_console_lines;

static void
printf_put_str(const char *s, void (*put)(char))
{
	while (*s != '\0')
		put(*s++);
}

/*
 * A writer's name in a mark: "t:" and the thread's address in hex, "cpuN:irq",
 * or "raw".  Written here, character by character, because this runs under
 * printf_lock and printf() is the one thing it cannot call.
 */
static void
printf_put_writer(printf_writer_t w, void (*put)(char))
{
	char		digits[2 * sizeof (printf_writer_t)];
	unsigned int	n = 0;

	if (w == PRINTF_WRITER_RAW) {
		printf_put_str("raw", put);
		return;
	}
	if (w & 1) {
		unsigned int cpu = (unsigned int) (w >> 1);

		printf_put_str("cpu", put);
		do {
			digits[n++] = (char) ('0' + cpu % 10);
			cpu /= 10;
		} while (cpu != 0);
		while (n > 0)
			put(digits[--n]);
		printf_put_str(":irq", put);
		return;
	}
	printf_put_str("t:", put);
	do {
		digits[n++] = "0123456789abcdef"[w & 0xf];
		w >>= 4;
	} while (w != 0);
	while (n > 0)
		put(digits[--n]);
}

/*
 * Before writer w's characters.  Another writer's open line is closed with
 * " <-cut [it]" and a newline, and remembered; a line of w's own that was cut
 * goes on behind "[w]-> ".  The two marks are the whole format a reader needs:
 * a line ending in the first is unfinished, and the next line that begins
 * with the second and the same name is its rest.
 */
static void
printf_lines_turn_begin(struct printf_lines *L, printf_writer_t w,
			void (*put)(char))
{
	unsigned int	i, free_slot;

	if (L->open != 0 && L->open != w) {
		printf_put_str(" <-cut [", put);
		printf_put_writer(L->open, put);
		printf_put_str("]\n", put);

		free_slot = PRINTF_CUT_SLOTS;
		for (i = 0; i < PRINTF_CUT_SLOTS; i++) {
			if (L->cut[i] == L->open)
				break;
			if (L->cut[i] == 0 && free_slot == PRINTF_CUT_SLOTS)
				free_slot = i;
		}
		if (i == PRINTF_CUT_SLOTS) {
			if (free_slot == PRINTF_CUT_SLOTS) {
				free_slot = L->cut_next;
				L->cut_next = (L->cut_next + 1) % PRINTF_CUT_SLOTS;
				L->cut_lost++;
			}
			L->cut[free_slot] = L->open;
		}
		L->open = 0;
	}

	if (L->open == 0)
		for (i = 0; i < PRINTF_CUT_SLOTS; i++)
			if (L->cut[i] == w) {
				L->cut[i] = 0;
				put('[');
				printf_put_writer(w, put);
				printf_put_str("]-> ", put);
				break;
			}
}

/*
 * After them: n characters were written, the last one `last'.  A call that
 * wrote nothing changes nothing -- it opened no line.
 */
static void
printf_lines_turn_end(struct printf_lines *L, printf_writer_t w,
		      unsigned int n, char last)
{
	if (n != 0)
		L->open = (last == '\n') ? 0 : w;
}

/*
 * The thread holding a report's block, or 0; and how deep its own blocks
 * nest.  Written under printf_lock, read outside it by a writer waiting.
 */
static volatile printf_writer_t	printf_block_owner;
static unsigned int		printf_block_depth;

/* What a writer waiting for a block does on each turn of its loop. */
#define	printf_pause()	__asm__ __volatile__("pause" : : : "memory")

/*
 * The turn of the writer holding printf_lock, and what it has written: the
 * console's writers count their characters through these, so that the turn's
 * end knows whether the line they leave is open.
 */
static printf_writer_t	printf_turn_writer;
static unsigned int	printf_turn_n;
static char		printf_turn_last;

static void
printf_turn_putc(char c)
{
	klog_cnputc(c);
	printf_turn_n++;
	printf_turn_last = c;
}

/*
 * The same count for the raw writer, which prints without printf_lock: kept
 * apart, so that a printf nested inside a turn does not overwrite the count of
 * the turn it interrupted.
 */
static unsigned int	printf_raw_n;
static char		printf_raw_last;

static void
printf_raw_putc(char c)
{
	klog_cnputc(c);
	printf_raw_n++;
	printf_raw_last = c;
}

/*
 * Take the console for one turn: printf_lock, after any other thread's block.
 * A processor in an interrupt does not wait for a block -- the block's holder
 * may be the thread it interrupted -- and rule 2 marks where it wrote.
 */
void
printf_writer_take(void)
{
	printf_writer_t	self = printf_writer_self();

	for (;;) {
		simple_lock(&printf_lock);
		if (printf_block_owner == 0 || printf_block_owner == self ||
		    (self & 1))
			break;
		simple_unlock(&printf_lock);
		while (printf_block_owner != 0)
			printf_pause();
	}
	printf_lock_holder = cpu_number();
	printf_turn_writer = self;
	printf_turn_n = 0;
	printf_turn_last = '\0';
	printf_lines_turn_begin(&printf_console_lines, self, klog_cnputc);
}

/*
 * Give it back.  `n' characters were written in the turn and `last' was the
 * last one; printf() passes its own count, read from printf_turn_putc().
 */
void
printf_writer_give(unsigned int n, char last)
{
	printf_lines_turn_end(&printf_console_lines, printf_turn_writer, n,
			      last);
	printf_lock_holder = -1;
	simple_unlock(&printf_lock);
}

/*
 * A report of several lines, as one block (rule 3).  Thread context only: a
 * processor in an interrupt prints at once and is marked, so these do nothing
 * there.  Nests for its holder.  The holder must not block inside -- it is not
 * preempted, and every other thread that prints waits for it -- so a block is
 * a report, never a conversation.
 */
void
printf_block_begin(void)
{
	printf_writer_t	self = printf_writer_self();

	if (self & 1)
		return;
	disable_preemption();
	if (printf_block_owner == self) {
		printf_block_depth++;
		return;
	}
	for (;;) {
		simple_lock(&printf_lock);
		if (printf_block_owner == 0) {
			printf_block_owner = self;
			printf_block_depth = 1;
			simple_unlock(&printf_lock);
			return;
		}
		simple_unlock(&printf_lock);
		while (printf_block_owner != 0)
			printf_pause();
	}
}

void
printf_block_end(void)
{
	printf_writer_t	self = printf_writer_self();
	unsigned int	left;

	if ((self & 1) || printf_block_owner != self)
		return;
	left = --printf_block_depth;
	if (left == 0) {
		simple_lock(&printf_lock);
		printf_block_owner = 0;
		simple_unlock(&printf_lock);
	}
	enable_preemption();

	/*
	 * The wire, which printf() left to the end of the block: a report of
	 * twenty lines written to the wire inside it would keep this processor
	 * unpreemptible for the device's time -- 170 ms of a 2 KB dump under
	 * KVM -- and every other printing thread waiting as long.
	 */
	if (left == 0)
		cnflush();
}

/*
 * The rules of the marks, asked at every boot from printf_init() on writers
 * made up for the purpose and a buffer of its own -- not on the console, where
 * whoever else prints at that moment would be in the answer.  A WRONG names
 * the case and what came out.
 */
static char		lines_test_buf[256];
static unsigned int	lines_test_len;

static void
lines_test_put(char c)
{
	if (lines_test_len < sizeof lines_test_buf - 1)
		lines_test_buf[lines_test_len++] = c;
	lines_test_buf[lines_test_len] = '\0';
}

static void
lines_test_write(struct printf_lines *L, printf_writer_t w, const char *s)
{
	unsigned int	n = 0;
	char		last = '\0';

	printf_lines_turn_begin(L, w, lines_test_put);
	for (; *s != '\0'; s++, n++) {
		lines_test_put(*s);
		last = *s;
	}
	printf_lines_turn_end(L, w, n, last);
}

static void
printf_lines_selftest(void)
{
	static const struct {
		const char	*writes;	/* writer, text; ... */
		const char	*expect;
	} cases[] = {
		/* whole lines: nothing to mark */
		{ "A|one\n|B|two\n",
		  "one\ntwo\n" },
		/* one writer, one line in three calls */
		{ "A|ph|A|ase|A| line\n",
		  "phase line\n" },
		/* a line cut, and gone on with */
		{ "A|half|B|whole\n|A| rest\n",
		  "half <-cut [t:1000]\nwhole\n[t:1000]->  rest\n" },
		/* an interrupt's line inside a thread's */
		{ "A|half|I|tick\n|A| rest\n",
		  "half <-cut [t:1000]\ntick\n[t:1000]->  rest\n" },
		/* two lines cut, each gone on with by its writer */
		{ "A|a|B|b|A|A\n|B|B\n",
		  "a <-cut [t:1000]\nb <-cut [t:2000]\n[t:1000]-> A\n"
		  "[t:2000]-> B\n" },
		/* a call that writes nothing opens no line */
		{ "A||B|x\n",
		  "x\n" },
		/* the interrupt's own line cut by a thread */
		{ "I|irq|A|line\n|I| end\n",
		  "irq <-cut [cpu2:irq]\nline\n[cpu2:irq]->  end\n" },
	};
	struct printf_lines	L;
	unsigned int		c, wrong = 0;

	for (c = 0; c < sizeof cases / sizeof cases[0]; c++) {
		const char	*p = cases[c].writes;
		char		text[64];

		memset(&L, 0, sizeof L);
		lines_test_len = 0;
		lines_test_buf[0] = '\0';
		while (*p != '\0') {
			printf_writer_t	w;
			unsigned int	k = 0;

			w = (*p == 'A') ? 0x1000 : (*p == 'B') ? 0x2000
			    : (((printf_writer_t) 2 << 1) | 1);
			p += 2;				/* the writer and its '|' */
			while (*p != '\0' && *p != '|' && k < sizeof text - 1)
				text[k++] = *p++;
			text[k] = '\0';
			if (*p == '|')
				p++;
			lines_test_write(&L, w, text);
		}
		if (strcmp(lines_test_buf, cases[c].expect) != 0) {
			wrong++;
			printf("printf: lines: case %u wrote \"%s\" -- WRONG "
			       "(#757)\n", c, lines_test_buf);
		}
	}

	/*
	 * And the set giving up: seventeen lines cut, one more than it holds,
	 * so one writer goes on without its mark -- counted, not lost.  Each of
	 * eighteen writers leaves a line open and the next one cuts it.
	 */
	memset(&L, 0, sizeof L);
	lines_test_len = 0;
	for (c = 0; c < PRINTF_CUT_SLOTS + 2; c++) {
		lines_test_write(&L, 0x1000 * (c + 1), "x");
		lines_test_len = 0;
	}
	if (L.cut_lost != 1) {
		wrong++;
		printf("printf: lines: %u cut lines gave up %u writers, not 1 "
		       "-- WRONG (#757)\n", PRINTF_CUT_SLOTS + 1, L.cut_lost);
	}

	printf("printf: lines: %u cases of the console's marks, %u wrong -- "
	       "%s (#757)\n",
	       (unsigned int) (sizeof cases / sizeof cases[0]) + 1, wrong,
	       wrong == 0 ? "PASS" : "WRONG");
}

void
printf_init(void)
{
	/*
	 * Lock is only really needed after the first thread is created.
	 */
	simple_lock_init(&printf_lock, ETAP_MISC_PRINTF);
	simple_lock_init(&sprintf_lock, ETAP_MISC_PRINTF);	/* #655 */
	klog_init();

	/*
	 * And from here the console may stop writing the wire in the caller's
	 * thread (#567).  Here and not earlier because this is the moment
	 * printf() becomes the writer: what runs before it prints through the
	 * machine's own early path, which has no ring behind it and must not
	 * -- a byte queued before anybody exists to drain it is a byte that
	 * waits for the first line somebody prints.
	 *
	 * A target whose console is synchronous answers this with nothing.
	 */
	cnasync(TRUE);

	/*
	 * #757: the rules of the marks, before anything relies on them.  Here,
	 * once, on both targets: printf() works by now, and asks nothing of
	 * the console the test is about.
	 */
	printf_lines_selftest();
}

/* derived from boot_gets */
void
safe_gets(
	char	*str,
	int	maxlen)
{
	register char *lp;
	register int c;
	char *strmax = str + maxlen - 1; /* allow space for trailing 0 */

	lp = str;
	for (;;) {
		c = cngetc();
		switch (c) {
		case '\n':
		case '\r':
			printf("\n");
			*lp++ = 0;
			return;
			
		case '\b':
		case '#':
		case '\177':
			if (lp > str) {
				printf("\b \b");
				lp--;
			}
			continue;

		case '@':
		case 'u'&037:
			lp = str;
			printf("\n\r");
			continue;

		default:
			if (c >= ' ' && c < '\177') {
				if (lp < strmax) {
					*lp++ = c;
					printf("%c", c);
				}
				else {
					printf("%c", '\007'); /* beep */
				}
			}
		}
	}
}

#include <mach_assert.h>
/*VARARGS1*/
void
printf(const char *fmt, ...)
{
	va_list	listp;
	extern int db_active;		/* #346: while a CPU is in DDB, skip
					 * printf_lock -- it may be held by the
					 * faulting context or a stopped peer CPU,
					 * which deadlocks the debugger's console. */

	/*
	 * ── The window: one line, at the device's pace (#551) ────────────
	 *
	 * From here to enable_preemption() this processor is not rescheduled,
	 * and from simple_lock() to simple_unlock() -- on a target whose spin
	 * lock masks interrupts for the hold, which x86-64 does (#528) -- it
	 * takes no interrupt either.  The device wait is inside that: on
	 * x86-64 cnputc() is a polled UART, and a byte costs what the DEVICE
	 * costs -- which is three different numbers, and calling any of them
	 * "the wire" would be wrong twice over (#567).
	 *
	 * Measured rather than assumed, and printed on every boot by
	 * cons_cost_report(): 83 us a byte under KVM, where there is no wire
	 * at all and the cost is an exit to the host plus a chardev write;
	 * 6.3 us under TCG, where it is a helper call in the same process.
	 * The wire proper is the one nobody has measured: 8N1 at the 115200
	 * baud boot.S programs is ten bits a byte, 87 us -- the same order as
	 * KVM's exit, where at the 38400 that stood there before it was three
	 * times slower than the emulator (#567).  A line is milliseconds
	 * whichever of the three it is.
	 *
	 * 🔴 AND THE DEVICE IS NOT INSIDE THE WINDOW ANY MORE (#567) -- on a
	 * target whose console buffers, which x86-64's now does.  What stood
	 * here said the wait was inside on purpose, and the reason given was
	 * what the lock is FOR: it protects no data structure, it makes a line
	 * a line on a wire that every processor shares, so rendering under the
	 * lock and emitting outside it would put the interleaving back -- two
	 * writers' bytes mixed on the wire, the shape #544 chased through
	 * thirteen red runs.
	 *
	 * That was right about the lock and wrong about the conclusion.  A
	 * RING answers the objection instead of arguing with it: bytes enter
	 * it in the order the lock granted and leave it in ring order, so a
	 * line still arrives whole whichever processor hands it over.  The
	 * hold is now a memory write per byte, and cnflush() below writes the
	 * port with preemption allowed and interrupts as the caller had them.
	 *
	 * ⚠️ It does not make a byte cheaper, and nothing here should be read
	 * as saying so: under an accelerator the cost IS the `outb' and
	 * somebody still pays it.  What changed is who, and when.
	 *
	 * ⚠️ On a target that has no ring -- i386 -- every word above the 🔴
	 * still describes what happens, and the device time is still inside
	 * the hold.
	 *
	 * What #551 changed is the window's LENGTH.  It was unbounded: a
	 * transmitter that never emptied kept this processor here for ever,
	 * and an unprivileged trap could ask for it.  cons_putc() now gives a
	 * byte a bounded wait and a stuck port one poll a byte, so a hold is
	 * at most one line at the device's pace on a healthy port and about one
	 * poll a byte on a dead one.  The longest line ring 3 can choose is
	 * MACH_PRINT_MAX-1 bytes through mach_print(), and one line or
	 * CONSOLE_CHUNK bytes through the console device (consolewrite).
	 *
	 * ⚠️ i386 is the same shape with a different mask: its spin lock does
	 * not touch the interrupt flag, and MACH_RT being 0 there means the
	 * preemption calls are empty (#486) -- the window is one line with
	 * interrupts as the caller had them, which for a trap is on.
	 */
	disable_preemption();
	va_start(listp, fmt);
#if	MP_PRINTF
	if (cpu_data[master_cpu].active_thread && !db_active &&
	    !printf_nested()) {
		/*
		 * #757: printf_lock after any other thread's block, and the
		 * marks of a line this cuts or goes on with; every character
		 * through printf_turn_putc(), so the turn knows whether it
		 * leaves a line open.
		 */
		printf_writer_take();
		if (cpu_number() != master_cpu)
			new_printf_cpu_number = TRUE;
		if (MP_PRINTF_CPU_PREFIX && new_printf_cpu_number) {
			int i;

			i = cpu_number();
			printf_turn_putc('{');
			if (i > 99) {
				printf_turn_putc('?');
			} else {
				if (i > 9) {
					printf_turn_putc('0'+ (i / 10));
					i = i % 10;
				}
				printf_turn_putc('0' + i);
			}
			printf_turn_putc('}');
			printf_turn_putc(' ');
		}
		_doprnt(fmt, &listp, printf_turn_putc, 16);
		printf_writer_give(printf_turn_n, printf_turn_last);
      } else
#endif	/* MP_PRINTF */
	{
		/*
		 * #757: the debugger, a printf nested in the holder of
		 * printf_lock, and the boot before its first thread write
		 * without the lock, as the raw writer.  A line somebody left
		 * open is closed with its mark first, at best: the machine is
		 * stopped, or this processor is the one holding the console.
		 */
		printf_raw_n = 0;
		printf_raw_last = '\0';
		printf_lines_turn_begin(&printf_console_lines, PRINTF_WRITER_RAW,
					klog_cnputc);
		_doprnt(fmt, &listp, printf_raw_putc, 16);
		printf_lines_turn_end(&printf_console_lines, PRINTF_WRITER_RAW,
				      printf_raw_n, printf_raw_last);
	}
	va_end(listp);
	enable_preemption();

	/*
	 * And NOW the wire, outside everything above (#567).
	 *
	 * On a target whose console buffers, the loop just run put the line
	 * into a ring and the device has not been touched yet; this hands it
	 * over, with preemption allowed and interrupts as the caller had them.
	 * The thread that printed still pays for the wire -- somebody must --
	 * but no longer inside a window where it cannot be rescheduled.
	 *
	 * ⚠️ AFTER enable_preemption() and not between the unlock and it: the
	 * point is the window, and leaving the device inside the outer half of
	 * it would keep most of what this removes.
	 *
	 * ⚠️ And it is the whole ring that goes, not this line: another
	 * processor may have left bytes behind because it could not get the
	 * port.  That is what keeps the ring short, and it is why a line's
	 * order on the wire is the ring's order and not the caller's.
	 *
	 * A target whose console is synchronous answers this with nothing.
	 *
	 * #757: not inside a block of this thread's own, which goes to the
	 * wire when it ends (printf_block_end()).
	 */
	if (printf_block_owner != printf_writer_self())
		cnflush();
}

static char *copybyte_str;

/*
 * 🔴 ONE sprintf() AT A TIME (#655).  copybyte() writes through copybyte_str,
 * a static, because _doprnt()'s character routine is handed the character and
 * nothing else.  Two calls at once -- on two processors, or an interrupt on
 * one -- each set it to their own buffer and wrote into the other's, past its
 * end if that one was shorter.  The lock is held at splhigh, so no interrupt
 * on this processor can call in while it is held here.
 */

static void
copybyte(
        char byte)
{
  *copybyte_str++ = byte;
  *copybyte_str = '\0';
}

int
sprintf(char *buf, const char *fmt, ...)
{
        va_list listp;
        spl_t   s;
        int     n;

        s = splhigh();
        simple_lock(&sprintf_lock);
        va_start(listp, fmt);
        copybyte_str = buf;
        _doprnt(fmt, &listp, copybyte, 16);
        va_end(listp);
        n = strlen(buf);
        simple_unlock(&sprintf_lock);
        splx(s);
        return n;
}
