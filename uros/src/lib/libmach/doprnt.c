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

/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * doprnt.c — the formatter userland has, rewritten (#432 audit).
 *
 * ── What was here, and why none of it could be kept ──────────────────
 *
 * Steve Summit's 1987 formatter, whose own header says:
 *
 *	It accepts, but ignores, an `l' as in %ld, %lo, %lx, and %lu, and
 *	therefore will not work correctly on machines for which
 *	sizeof(long) != sizeof(int).
 *
 * That machine is x86-64.  Every conversion fetched an `unsigned int', so
 * there was NO SPELLING that printed a 64-bit value: %x truncated, and %lx
 * truncated identically while satisfying a compiler's format check.
 *
 * 🔴 AND IT WAS ONE OF THREE.  libmach/printf.c carries its OWN static
 * _doprnt, with %p and with the length modifier honoured; kern/printf.c
 * carries a third, taught the modifier by #415.  So inside this one library
 * printf() and sprintf() ran different formatters with different conversion
 * sets, and whether %p worked depended on whether you were printing to the
 * console or to a string.  #415 fixed one copy of a defect that existed in
 * three places, which is what having three copies does.
 *
 * ⚠️ None of that was visible because userland had no -Wall and printf had no
 * format attribute.  Both are on now, and turning them on is what found this.
 *
 * ── What it implements, and how that was decided ─────────────────────
 *
 * By counting, not by taste.  Every printf-family call site in 575 userland
 * files, format strings only:
 *
 *	%s 1407   %d 983   %u 386   %x 242   %X 40
 *	%c 16     %% 8     %b 6     %p 4     %o 1
 *
 * So: s d i u o x X c p b B %, with flags, width, precision, and the length
 * modifiers hh h l ll z t j.
 *
 * 🔑 %p was NOT in the old public formatter's switch at all.  Its four
 * callers reached `default:', which prints the letter and DOES NOT CONSUME
 * THE ARGUMENT -- so every following conversion in that call read the wrong
 * one.  That is worse than a truncation: it is a desynchronised va_list.
 *
 * ── What was deleted, and how that was decided ───────────────────────
 *
 * The same way: nothing in those 575 files calls any of it.
 *
 *	%e %E %f %F %g %G   floating point, about 170 lines
 *	%r %R               "default radix", the only user of the radix
 *	                    parameter -- which is why the parameter is gone
 *	                    too and both callers passed 0 anyway
 *	%n                  writes through a caller's pointer; unused, and a
 *	                    liability the moment a format string is not a
 *	                    literal
 *	%D %O %U            long forms, which %ld %lo %lu now cover honestly
 *	%z %Z               Mach's SIGNED HEX.  ⚠️ Reclaimed as C's size_t
 *	                    length modifier: a caller writing %zu meant
 *	                    size_t, and would have got a hex number followed
 *	                    by a literal `u'.
 *
 * ── Speed ────────────────────────────────────────────────────────────
 *
 * Digits come out by shifting for base 16 and 8, and a value that fits in 32
 * bits is divided as a 32-bit value.  That matters on i386, where a 64-bit
 * divide is a call to __udivdi3 (#415) -- per digit, on the path every
 * diagnostic in the system takes.  The 64-bit divide is by a constant, so the
 * compiler turns it into a multiply where it can.
 */

#include <mach/boolean.h>
#include <stdarg.h>
#include "externs.h"
#include <string.h>

#define	FL_LEFT		0x01	/* -  */
#define	FL_PLUS		0x02	/* +  */
#define	FL_SPACE	0x04	/*    */
#define	FL_ALT		0x08	/* #  */
#define	FL_ZERO		0x10	/* 0  */

#define	LEN_INT		0
#define	LEN_LONG	1
#define	LEN_LLONG	2

/*
 * 2^64-1 is twenty digits in decimal and twenty-two in octal, and a leading
 * "0x" or sign is emitted separately.  Twenty-four leaves room to be wrong by
 * a little rather than by a lot.
 */
#define	NUMBUF		24

static const char digits_lower[] = "0123456789abcdef";
static const char digits_upper[] = "0123456789ABCDEF";

/*
 * Write the digits of `v' in `base' into `out', most significant first, and
 * return how many there are.  `out' holds at least NUMBUF.
 */
static unsigned int
to_digits(char *out, unsigned long long v, unsigned int base, const char *dig)
{
	char		tmp[NUMBUF];
	unsigned int	n = 0;
	unsigned int	i;

	if (base == 16) {
		do {
			tmp[n++] = dig[v & 15u];
			v >>= 4;
		} while (v != 0);
	} else if (base == 8) {
		do {
			tmp[n++] = dig[v & 7u];
			v >>= 3;
		} while (v != 0);
	} else if (v <= 0xFFFFFFFFULL) {
		unsigned int w = (unsigned int) v;

		do {
			tmp[n++] = dig[w % 10u];
			w /= 10u;
		} while (w != 0);
	} else {
		do {
			tmp[n++] = dig[v % 10ULL];
			v /= 10ULL;
		} while (v != 0);
	}

	for (i = 0; i < n; i++)
		out[i] = tmp[n - 1 - i];
	return n;
}

static void
pad(void (*putc)(void *, int), void *arg, char c, int n)
{
	while (n-- > 0)
		(*putc)(arg, c);
}

/*
 * The Mach register-decoding format, kept because six callers use it:
 *
 *	printf("reg = %b\n", regval, "\10\2BITTWO\1BITONE")
 *
 * The string's first byte is the base as a control character ('\10' octal,
 * '\20' hex); each field after it is a bit number followed by its name, up to
 * the next control character.
 */
static void
print_bitfield(void (*putc)(void *, int), void *arg,
	       unsigned long long v, const char *p)
{
	unsigned int	base = (unsigned int) *p++;
	char		buf[NUMBUF];
	unsigned int	n;
	int		any = 0;

	if (base != 8 && base != 16)
		base = 16;
	n = to_digits(buf, v, base, digits_lower);
	for (unsigned int i = 0; i < n; i++)
		(*putc)(arg, buf[i]);

	while (*p != '\0') {
		unsigned int bit = (unsigned int) *p++;

		if (v & (1ULL << (bit - 1))) {
			(*putc)(arg, any ? ',' : '<');
			any = 1;
			while (*p > 32)
				(*putc)(arg, *p++);
		} else {
			while (*p > 32)
				p++;
		}
	}
	if (any)
		(*putc)(arg, '>');
}

void
_doprnt(const char *fmt,
	va_list			args,
	void			(*putc)(void *, int),
	void			*putc_arg)
{
	while (*fmt != '\0') {
		unsigned int	flags = 0;
		int		width = 0;
		int		prec = -1;
		int		lensize = LEN_INT;
		unsigned int	base = 10;
		int		is_signed = 0;
		const char	*dig = digits_lower;
		const char	*prefix = "";
		char		sign = '\0';
		char		numbuf[NUMBUF];
		unsigned int	ndig = 0;
		int		len;

		if (*fmt != '%') {
			(*putc)(putc_arg, *fmt++);
			continue;
		}
		fmt++;

		/* Flags, in any order and any number. */
		for (;;) {
			if (*fmt == '-')		flags |= FL_LEFT;
			else if (*fmt == '+')		flags |= FL_PLUS;
			else if (*fmt == ' ')		flags |= FL_SPACE;
			else if (*fmt == '#')		flags |= FL_ALT;
			else if (*fmt == '0')		flags |= FL_ZERO;
			else break;
			fmt++;
		}

		/* Width. */
		if (*fmt == '*') {
			width = va_arg(args, int);
			if (width < 0) {
				flags |= FL_LEFT;
				width = -width;
			}
			fmt++;
		} else {
			while (*fmt >= '0' && *fmt <= '9')
				width = width * 10 + (*fmt++ - '0');
		}

		/* Precision. */
		if (*fmt == '.') {
			fmt++;
			prec = 0;
			if (*fmt == '*') {
				prec = va_arg(args, int);
				fmt++;
			} else {
				while (*fmt >= '0' && *fmt <= '9')
					prec = prec * 10 + (*fmt++ - '0');
			}
			if (prec < 0)
				prec = -1;
		}

		/*
		 * Length.  `h' and `hh' are accepted and deliberately not
		 * acted on: default argument promotion has already widened a
		 * short or a char to an int, so there is nothing narrower to
		 * fetch.  Recorded rather than silently skipped -- which is
		 * exactly how the `l' came to be ignored for thirty-nine
		 * years.
		 */
		while (*fmt == 'h')
			fmt++;
		while (*fmt == 'l') {
			lensize = (lensize == LEN_LONG) ? LEN_LLONG : LEN_LONG;
			fmt++;
		}
		if (*fmt == 'z' || *fmt == 't' || *fmt == 'j') {
			lensize = LEN_LONG;
			fmt++;
		}

		switch (*fmt) {
		case '\0':
			/* A format ending in `%': emit it and stop. */
			(*putc)(putc_arg, '%');
			return;

		case '%':
			(*putc)(putc_arg, '%');
			fmt++;
			continue;

		case 'c':
			pad(putc, putc_arg, ' ',
			    (flags & FL_LEFT) ? 0 : width - 1);
			(*putc)(putc_arg, (char) va_arg(args, int));
			pad(putc, putc_arg, ' ',
			    (flags & FL_LEFT) ? width - 1 : 0);
			fmt++;
			continue;

		case 's': {
			const char *s = va_arg(args, const char *);

			if (s == 0)
				s = "(null)";
			len = 0;
			while (s[len] != '\0' && (prec < 0 || len < prec))
				len++;
			pad(putc, putc_arg, ' ',
			    (flags & FL_LEFT) ? 0 : width - len);
			for (int i = 0; i < len; i++)
				(*putc)(putc_arg, s[i]);
			pad(putc, putc_arg, ' ',
			    (flags & FL_LEFT) ? width - len : 0);
			fmt++;
			continue;
		}

		case 'b':
		case 'B': {
			unsigned long long v = (lensize == LEN_LLONG)
				? va_arg(args, unsigned long long)
				: (lensize == LEN_LONG)
				? (unsigned long long) va_arg(args, unsigned long)
				: (unsigned long long) va_arg(args, unsigned int);
			const char *p = va_arg(args, const char *);

			print_bitfield(putc, putc_arg, v, p);
			fmt++;
			continue;
		}

		case 'p': {
			unsigned long long v =
				(unsigned long long)(unsigned long)
				va_arg(args, void *);

			ndig = to_digits(numbuf, v, 16, digits_lower);
			prefix = "0x";
			break;
		}

		case 'd':
		case 'i':
			is_signed = 1;
			base = 10;
			break;

		case 'u':
			base = 10;
			break;

		case 'o':
			base = 8;
			if (flags & FL_ALT)
				prefix = "0";
			break;

		case 'X':
			dig = digits_upper;
			/* FALLTHROUGH */
		case 'x':
			base = 16;
			break;

		default:
			/*
			 * An unknown conversion.  The character is emitted
			 * and NO ARGUMENT IS CONSUMED, which is what the old
			 * formatter did for %p -- so say it rather than
			 * leaving the caller to discover a shifted va_list
			 * from the values that follow.
			 */
			(*putc)(putc_arg, '%');
			(*putc)(putc_arg, *fmt);
			fmt++;
			continue;
		}

		if (*fmt != 'p') {
			if (is_signed) {
				long long n = (lensize == LEN_LLONG)
					? va_arg(args, long long)
					: (lensize == LEN_LONG)
					? (long long) va_arg(args, long)
					: (long long) va_arg(args, int);
				unsigned long long u;

				if (n < 0) {
					sign = '-';
					/* -LLONG_MIN does not fit; negate in
					 * the unsigned domain, where it does */
					u = (unsigned long long) -(n + 1) + 1;
				} else {
					u = (unsigned long long) n;
					if (flags & FL_PLUS)
						sign = '+';
					else if (flags & FL_SPACE)
						sign = ' ';
				}
				ndig = to_digits(numbuf, u, base, dig);
			} else {
				unsigned long long u = (lensize == LEN_LLONG)
					? va_arg(args, unsigned long long)
					: (lensize == LEN_LONG)
					? (unsigned long long)
					  va_arg(args, unsigned long)
					: (unsigned long long)
					  va_arg(args, unsigned int);

				if (base == 16 && (flags & FL_ALT) && u != 0)
					prefix = (dig == digits_upper)
						? "0X" : "0x";
				ndig = to_digits(numbuf, u, base, dig);
			}
		}

		/* Emit: [pad] [sign] [prefix] [zeros] digits [pad] */
		{
			int nprefix = (int) strlen(prefix);
			int nzero = (prec > (int) ndig) ? prec - (int) ndig : 0;
			int total = (int) ndig + nzero + nprefix
				  + (sign != '\0' ? 1 : 0);

			/*
			 * `0' is ignored when a precision is given, which is
			 * what C says and what stops "%08.3d" from padding
			 * twice.
			 */
			if ((flags & FL_ZERO) && !(flags & FL_LEFT)
			    && prec < 0 && width > total) {
				nzero += width - total;
				total = width;
			}

			if (!(flags & FL_LEFT))
				pad(putc, putc_arg, ' ', width - total);
			if (sign != '\0')
				(*putc)(putc_arg, sign);
			while (*prefix != '\0')
				(*putc)(putc_arg, *prefix++);
			pad(putc, putc_arg, '0', nzero);
			for (unsigned int i = 0; i < ndig; i++)
				(*putc)(putc_arg, numbuf[i]);
			if (flags & FL_LEFT)
				pad(putc, putc_arg, ' ', width - total);
		}
		fmt++;
	}
}
