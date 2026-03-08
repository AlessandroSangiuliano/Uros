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
#include <stdarg.h>

#include "externs.h"
#include <string.h>

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

static void
printnum(unsigned int	u,	/* number to print */
	 int		base,
	 void			(*putc)(void *, int),
	 void			*putc_arg)
{
	char	buf[MAXBUF];	/* build number here */
	char *	p = &buf[MAXBUF-1];
	static char digs[] = "0123456789abcdef";

	do {
	    *p-- = digs[u % base];
	    u /= base;
	} while (u != 0);

	while (++p != &buf[MAXBUF])
	    (*putc)(putc_arg, *p);
}

void
_doprnt(const char *fmt,
	va_list		args,
	int		radix,			/* default radix - for '%r' */
 	void		(*putc)(void *, int),	/* character output */
	void		*putc_arg)		/* argument for putc */
{
	int		length;
	int		prec;
	boolean_t	ladjust;
	char		padc;
	int		n;
	unsigned int	u;
	int		plus_sign;
	int		sign_char;
	boolean_t	altfmt;
	int		base;
	char		c;

	while (*fmt != '\0') {
	    if (*fmt != '%') {
		(*putc)(putc_arg, *fmt++);
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
		if (*fmt == '#') {
		    altfmt = TRUE;
		    fmt++;
		}
		else if (*fmt == '-') {
		    ladjust = TRUE;
		    fmt++;
		}
		else if (*fmt == '+') {
		    plus_sign = '+';
		    fmt++;
		}
		else if (*fmt == ' ') {
		    if (plus_sign == 0)
			plus_sign = ' ';
		    fmt++;
		}
		else
		    break;
	    }

	    if (*fmt == '0') {
		padc = '0';
		fmt++;
	    }

	    if (isdigit(*fmt)) {
		while(isdigit(*fmt))
		    length = 10 * length + Ctod(*fmt++);
	    }
	    else if (*fmt == '*') {
		length = va_arg(args, int);
		fmt++;
		if (length < 0) {
		    ladjust = !ladjust;
		    length = -length;
		}
	    }

	    if (*fmt == '.') {
		fmt++;
		if (isdigit(*fmt)) {
		    prec = 0;
		    while(isdigit(*fmt))
			prec = 10 * prec + Ctod(*fmt++);
		}
		else if (*fmt == '*') {
		    prec = va_arg(args, int);
		    fmt++;
		}
	    }

	    if (*fmt == 'l')
		fmt++;	/* need it if sizeof(int) < sizeof(long) */

	    switch(*fmt) {
		case 'b':
		case 'B':
		{
		    const char *p;
		    boolean_t	  any;
		    int  i;

		    u = va_arg(args, unsigned int);
		    p = va_arg(args, const char *);
		    base = *p++;
		    printnum(u, base, putc, putc_arg);

		    if (u == 0)
			break;

		    any = FALSE;
		    while (i = *p++) {
			if (*p <= 32) {
			    /*
			     * Bit field
			     */
			    int j;
			    if (any)
				(*putc)(putc_arg, ',');
			    else {
				(*putc)(putc_arg, '<');
				any = TRUE;
			    }
			    j = *p++;
			    for (; (c = *p) > 32; p++)
				(*putc)(putc_arg, c);
			    printnum((unsigned)( (u>>(j-1)) & ((2<<(i-j))-1)),
					base, putc, putc_arg);
			}
			else if (u & (1<<(i-1))) {
			    if (any)
				(*putc)(putc_arg, ',');
			    else {
				(*putc)(putc_arg, '<');
				any = TRUE;
			    }
			    for (; (c = *p) > 32; p++)
				(*putc)(putc_arg, c);
			}
			else {
			    for (; *p > 32; p++)
				continue;
			}
		    }
		    if (any)
			(*putc)(putc_arg, '>');
		    break;
		}

		case 'c':
		    c = va_arg(args, int);
		    (*putc)(putc_arg, c);
		    break;

		case 's':
		{
		    const char *p;
		    const char *p2;

		    if (prec == -1)
			prec = 0x7fffffff;	/* MAXINT */

		    p = va_arg(args, char *);

		    if (p == (char *)0)
			p = "";

		    if (length > 0 && !ladjust) {
			n = 0;
			p2 = p;

			for (; *p != '\0' && n < prec; p++)
			    n++;

			p = p2;

			while (n < length) {
			    (*putc)(putc_arg, ' ');
			    n++;
			}
		    }

		    n = 0;

		    while (*p != '\0') {
			if (++n > prec)
			    break;

			(*putc)(putc_arg, *p++);
		    }

		    if (n < length && ladjust) {
			while (n < length) {
			    (*putc)(putc_arg, ' ');
			    n++;
			}
		    }

		    break;
		}

		case 'o':
		case 'O':
		    base = 8;
		    goto print_unsigned;

		case 'd':
		case 'D':
		    base = 10;
		    goto print_signed;

		case 'u':
		case 'U':
		    base = 10;
		    goto print_unsigned;

		case 'x':
		case 'X':
		    base = 16;
		    goto print_unsigned;

		case 'z':
		case 'Z':
		    base = 16;
		    goto print_signed;

		case 'r':
		case 'R':
		    base = radix;
		    goto print_signed;

		case 'n':
		    base = radix;
		    goto print_unsigned;

		print_signed:
		    n = va_arg(args, int);
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
		    u = va_arg(args, unsigned int);
		    goto print_num;

		print_num:
		{
		    char	buf[MAXBUF];	/* build number here */
		    char *	p = &buf[MAXBUF-1];
		    static char digits[] = "0123456789abcdef";
		    const char *prefix = 0;

		    if (u != 0 && altfmt) {
			if (base == 8)
			    prefix = "0";
			else if (base == 16)
			    prefix = "0x";
		    }

		    do {
			*p-- = digits[u % base];
			u /= base;
		    } while (u != 0);

		    length -= (&buf[MAXBUF-1] - p);
		    if (sign_char)
			length--;
		    if (prefix)
			length -= strlen(prefix);

		    if (padc == ' ' && !ladjust) {
			/* blank padding goes before prefix */
			while (--length >= 0)
			    (*putc)(putc_arg, ' ');
		    }
		    if (sign_char)
			(*putc)(putc_arg, sign_char);
		    if (prefix)
			while (*prefix)
			    (*putc)(putc_arg, *prefix++);
		    if (padc == '0') {
			/* zero padding goes after sign and prefix */
			while (--length >= 0)
			    (*putc)(putc_arg, '0');
		    }
		    while (++p != &buf[MAXBUF])
			(*putc)(putc_arg, *p);

		    if (ladjust) {
			while (--length >= 0)
			    (*putc)(putc_arg, ' ');
		    }
		    break;
		}


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
		    double		dval   = va_arg(args, double);
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
			    while (slen < length) { (*putc)(putc_arg, ' '); length--; }
			while (*sp)
			    (*putc)(putc_arg, *sp++);
			if (ladjust)
			    while (slen < length) { (*putc)(putc_arg, ' '); length--; }
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
			while (total < length) { (*putc)(putc_arg, ' '); length--; }

		    if (dneg)          (*putc)(putc_arg, '-');
		    else if (plus_sign) (*putc)(putc_arg, plus_sign);

		    /* Right-align: zero padding after sign */
		    if (!ladjust && padc == '0')
			while (total < length) { (*putc)(putc_arg, '0'); length--; }

		    while (*ip) (*putc)(putc_arg, *ip++);

		    if (show_frac) {
			(*putc)(putc_arg, '.');
			for (fi = 0; fi < flen; fi++) (*putc)(putc_arg, fbuf[fi]);
			for (; fi < efrac; fi++)      (*putc)(putc_arg, '0');
		    }

		    for (fi = 0; fi < elen; fi++) (*putc)(putc_arg, expbuf[fi]);

		    /* Left-align: trailing space padding */
		    if (ladjust)
			while (total < length) { (*putc)(putc_arg, ' '); length--; }
		    break;
		}

		case '\0':
		    fmt--;
		    break;

		default:
		    (*putc)(putc_arg, *fmt);
	    }
	fmt++;
	}
}
