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
#include <stdarg.h>
#include <string.h>

/*
 * Forward declarations
 */
void printnum(
	register unsigned int	u,
	register int		base,
	void			(*putc)(char));

#ifndef hp_pa
#define	MP_PRINTF	((NCPUS > 1) || MACH_ASSERT)
#endif /* !hp_pa */

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


		case 'f':
		case 'F':
		case 'e':
		case 'E':
		case 'g':
		case 'G':
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

void
printf_init(void)
{
	/*
	 * Lock is only really needed after the first thread is created.
	 */
	simple_lock_init(&printf_lock, ETAP_MISC_PRINTF);
	klog_init();
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

#if !defined(__alpha)

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

	disable_preemption();
	va_start(listp, fmt);
#if	MP_PRINTF
	if (cpu_data[master_cpu].active_thread && !db_active) {
		simple_lock(&printf_lock);
		if (cpu_number() != master_cpu)
			new_printf_cpu_number = TRUE;
		if (MP_PRINTF_CPU_PREFIX && new_printf_cpu_number) {
			int i;

			i = cpu_number();
			klog_cnputc('{');
			if (i > 99) {
				klog_cnputc('?');
			} else {
				if (i > 9) {
					klog_cnputc('0'+ (i / 10));
					i = i % 10;
				}
				klog_cnputc('0' + i);
			}
			klog_cnputc('}');
			klog_cnputc(' ');
		}
		_doprnt(fmt, &listp, klog_cnputc, 16);
		simple_unlock(&printf_lock);
      } else
#endif	/* MP_PRINTF */
      _doprnt(fmt, &listp, klog_cnputc, 16);
	va_end(listp);
	enable_preemption();
}

static char *copybyte_str;

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
        va_start(listp, fmt);
        copybyte_str = buf;
        _doprnt(fmt, &listp, copybyte, 16);
        va_end(listp);
        return strlen(buf);
}
#endif /* !defined(__alpha) */
