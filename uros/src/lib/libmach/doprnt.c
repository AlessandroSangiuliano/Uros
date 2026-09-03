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


/* ================================================================
 * Floating point
 * ================================================================ */

/*
 * 🔴 REIMPLEMENTED. The 1987 branch this replaces
 * was a hundred and seventy lines inside the switch and nobody had ever
 * compared its output with anything.
 *
 * ⚠️ Digits come out through `long double', which on x86 carries a 64-bit
 * significand -- about nineteen decimal digits, against the seventeen a
 * double can need. Where the two disagree it will be in the last digit of a 
 * very long conversion, and the harness says how often.
 */

/*
 * 🔑 Sized for what %f of DBL_MAX actually needs: 309 integer digits plus the
 * precision.
 */
#define	FLT_MAXDIG	460		/* significant digits we will produce */
/*
 * The most digits after the point this will emit.  A precision beyond it
 * is CLAMPED rather than honoured: past nineteen significant digits a
 * double has nothing left to say, and the alternative is a buffer whose
 * size depends on a number the caller chooses.
 */
#define	FLT_MAXPREC	128


/* ================================================================
 * Exact decimal digits of a double
 * ================================================================ */

/* The obvious way -- normalise by dividing by ten until the value is in
 * [1,10) and then peel digits off -- is what was here, and it is wrong in a
 * way that shows on ordinary numbers: 255.5 is exact in binary, 2.555 is not,
 * so after two divisions the digits are 2,5,5,4,9,9,... and "%.0f" printed
 * 255 where every other C library prints 256.  No amount of extra precision
 * fixes it; the error is introduced by the division itself.
 *
 * 🔑 So no division.  A double IS an exact rational: m * 2^k with m a 53-bit
 * integer.  For k >= 0 the value is the integer m << k; for k < 0 it is
 * (m >> s) + (m & (2^s - 1)) / 2^s.  Both halves are computed in binary and
 * converted by repeated operations that are themselves exact:
 *
 *	integer part    divide the big value by 10^9, keep remainders
 *	fraction        multiply the numerator by 10, take what crosses the
 *	                binary point, and keep going
 *
 * Every digit produced this way is the digit the value actually has.  What
 * is left over says whether the tail was zero, which is what round-half-to-
 * even needs to tell an exact half from something just above it.
 *
 * The widest thing that must fit is 2^1023 * (2^53-1), just over 1076 bits.
 */
#define	BN_LIMBS	40			/* 1280 bits */

struct bignat {
	unsigned int	l[BN_LIMBS];		/* little-endian, 32-bit limbs */
	int		n;			/* limbs in use */
};

static void
bn_set_u64(struct bignat *b, unsigned long long v)
{
	int i;

	for (i = 0; i < BN_LIMBS; i++)
		b->l[i] = 0;
	b->l[0] = (unsigned int)(v & 0xFFFFFFFFu);
	b->l[1] = (unsigned int)(v >> 32);
	b->n = (b->l[1] != 0) ? 2 : (b->l[0] != 0 ? 1 : 0);
}

static int
bn_is_zero(const struct bignat *b)
{
	return b->n == 0;
}

static void
bn_shl(struct bignat *b, int bits)
{
	int	words = bits / 32;
	int	rest  = bits % 32;
	int	i;

	if (bn_is_zero(b) || bits <= 0)
		return;

	if (words > 0) {
		for (i = BN_LIMBS - 1; i >= words; i--)
			b->l[i] = b->l[i - words];
		for (i = 0; i < words; i++)
			b->l[i] = 0;
		b->n += words;
		if (b->n > BN_LIMBS)
			b->n = BN_LIMBS;
	}
	if (rest > 0) {
		unsigned int carry = 0;

		for (i = 0; i < BN_LIMBS; i++) {
			unsigned int nc = b->l[i] >> (32 - rest);

			b->l[i] = (b->l[i] << rest) | carry;
			carry = nc;
		}
	}
	while (b->n < BN_LIMBS && b->l[b->n] != 0)
		b->n++;
	while (b->n > 0 && b->l[b->n - 1] == 0)
		b->n--;
}

/* b /= d, returning the remainder.  d must fit in 32 bits. */
static unsigned int
bn_divmod_small(struct bignat *b, unsigned int d)
{
	unsigned long long	rem = 0;
	int			i;

	for (i = b->n - 1; i >= 0; i--) {
		unsigned long long cur = (rem << 32) | b->l[i];

		b->l[i] = (unsigned int)(cur / d);
		rem = cur % d;
	}
	while (b->n > 0 && b->l[b->n - 1] == 0)
		b->n--;
	return (unsigned int) rem;
}

/* b *= m, m < 2^32. */
static void
bn_mul_small(struct bignat *b, unsigned int m)
{
	unsigned long long	carry = 0;
	int			i;

	for (i = 0; i < BN_LIMBS; i++) {
		unsigned long long cur = (unsigned long long) b->l[i] * m
				       + carry;

		b->l[i] = (unsigned int)(cur & 0xFFFFFFFFu);
		carry = cur >> 32;
	}
	b->n = BN_LIMBS;
	while (b->n > 0 && b->l[b->n - 1] == 0)
		b->n--;
}

/*
 * The bits at or above `s', removed from b and returned.
 *
 * ⚠️ Called only just after a multiply by ten, so what sits above `s' is at
 * most four bits -- a decimal digit.  The first version of this walked the
 * limbs above and OR-ed them in RAW before the shifted read could use them,
 * which corrupted the digit and zeroed the limb it needed: 1287 of 40000
 * random doubles came out with wrong digits, and a hand-picked test of 725
 * had found none of them.
 */
static unsigned int
bn_take_above(struct bignat *b, int s)
{
	int		word = s / 32;
	int		bit  = s % 32;
	unsigned int	got;
	int		i;

	if (word >= BN_LIMBS)
		return 0;

	got = b->l[word] >> bit;
	if (bit != 0 && word + 1 < BN_LIMBS)
		got |= b->l[word + 1] << (32 - bit);

	b->l[word] &= (bit == 0) ? 0u : ((1u << bit) - 1u);
	for (i = word + 1; i < BN_LIMBS; i++)
		b->l[i] = 0;

	b->n = word + 1;
	while (b->n > 0 && b->l[b->n - 1] == 0)
		b->n--;
	return got;
}

/*
 * Up to `n' significant decimal digits of |dv|, exactly, with the decimal
 * exponent of the first.  `*more' says whether anything nonzero follows.
 * No rounding happens here: the caller rounds, because only it knows which
 * digit it stopped at.
 */
static int
float_digits(double dv, int n, char *out, int *exp10, int *more)
{
	union { double d; unsigned long long u; } c;
	struct bignat		ip, fp;
	unsigned long long	m;
	int			k, s = 0;
	char			ibuf[400];
	int			ni = 0, nd = 0, e = 0, i;

	c.d = dv;
	m = c.u & 0x000FFFFFFFFFFFFFULL;
	k = (int)((c.u >> 52) & 0x7FFULL);
	if (k == 0)
		k = -1074;
	else {
		m |= 0x0010000000000000ULL;
		k = k - 1075;
	}

	bn_set_u64(&ip, 0);
	bn_set_u64(&fp, 0);
	if (m == 0) {
		out[0] = '0'; out[1] = '\0';
		*exp10 = 0; *more = 0;
		return 1;
	}

	if (k >= 0) {
		bn_set_u64(&ip, m);
		bn_shl(&ip, k);
	} else {
		s = -k;
		if (s < 64)
			bn_set_u64(&ip, m >> s);
		bn_set_u64(&fp, m & ((s >= 64) ? m : ((1ULL << s) - 1ULL)));
	}

	/* Integer digits, least significant first, then reversed. */
	if (!bn_is_zero(&ip)) {
		struct bignat t = ip;

		while (!bn_is_zero(&t)) {
			unsigned int r = bn_divmod_small(&t, 1000000000u);
			int j;

			for (j = 0; j < 9; j++) {
				if (ni < (int)sizeof ibuf)
					ibuf[ni++] = (char)('0' + r % 10u);
				r /= 10u;
			}
		}
		while (ni > 1 && ibuf[ni - 1] == '0')
			ni--;
	}

	if (ni > 0) {
		e = ni - 1;
		for (i = 0; i < ni && nd < n; i++)
			out[nd++] = ibuf[ni - 1 - i];
		if (nd == n) {
			/* anything left, integer or fraction, is "more" */
			*more = (i < ni) || !bn_is_zero(&fp);
			for (; i < ni && !*more; i++)
				if (ibuf[ni - 1 - i] != '0')
					*more = 1;
			out[nd] = '\0';
			*exp10 = e;
			return nd;
		}
	}

	/* Fraction digits: multiply by ten and take what crosses the point. */
	{
		int leading = (ni == 0);

		while (nd < n && !bn_is_zero(&fp)) {
			unsigned int d;

			bn_mul_small(&fp, 10u);
			d = bn_take_above(&fp, s);
			if (leading && d == 0) {
				e--;
				continue;
			}
			leading = 0;
			out[nd++] = (char)('0' + d);
		}
		if (leading) {
			out[0] = '0'; out[1] = '\0';
			*exp10 = 0; *more = 0;
			return 1;
		}
		if (ni == 0)
			e = e - 1;
	}

	*more = !bn_is_zero(&fp);
	out[nd] = '\0';
	*exp10 = e;
	return nd;
}

/*
 * `n' significant digits of |dv|, ROUNDED half to even, with the decimal
 * exponent.  Returns how many digits are in `out'.
 *
 * 🔑 Half to even and not half up, because that is what IEEE 754 says.
 */

static int
float_round(double dv, int n, char *out, int *exp10)
{
	int	more = 0, e = 0, got, i;

	if (n < 1)
		n = 1;
	if (n > FLT_MAXDIG)
		n = FLT_MAXDIG;

	got = float_digits(dv, n + 1, out, &e, &more);
	if (got <= n) {
		out[got] = '\0';
		*exp10 = e;
		return got;
	}

	if (out[n] > '5'
	    || (out[n] == '5' && (more || ((out[n - 1] - '0') & 1)))) {
		for (i = n - 1; i >= 0; i--) {
			if (out[i] != '9') { out[i]++; break; }
			out[i] = '0';
		}
		if (i < 0) {
			for (i = n - 1; i > 0; i--)
				out[i] = out[i - 1];
			out[0] = '1';
			e++;
		}
	}
	out[n] = '\0';
	*exp10 = e;
	return n;
}

static void
print_float(double dv, int conv, int prec, int width, unsigned int flags,
	    void (*putc)(void *, int), void *putc_arg)
{
	union { double d; unsigned long long u; } chk;
	char		digs[FLT_MAXDIG + 2];
	/*
	 * ⚠️ Sized for %f of DBL_MAX, whose integer part is 309 digits.  The
	 * first version of this held forty and smashed the stack on the very
	 * first big value the differential harness handed it -- which is what
	 * a harness is for.  309 digits, a point, the precision, a sign and
	 * slack.
	 */
	char		body[309 + 1 + FLT_MAXPREC + 8];
	int		nbody = 0;
	int		e10 = 0;
	int		upper = (conv == 'E' || conv == 'G' || conv == 'F');
	int		style = (conv == 'f' || conv == 'F') ? 'f'
			      : (conv == 'e' || conv == 'E') ? 'e' : 'g';
	char		sign = '\0';
	int		i;
	int		total, padn;
	int		strip = 0;

	chk.d = dv;
	if (chk.u >> 63)
		sign = '-';
	else if (flags & FL_PLUS)
		sign = '+';
	else if (flags & FL_SPACE)
		sign = ' ';

	/* NaN and infinity, spelled the way the conversion's case asks. */
	if (((chk.u >> 52) & 0x7FFULL) == 0x7FFULL) {
		const char *s = (chk.u & 0x000FFFFFFFFFFFFFULL)
			? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf");

		total = 3 + (sign != '\0' ? 1 : 0);
		if (!(flags & FL_LEFT))
			pad(putc, putc_arg, ' ', width - total);
		if (sign != '\0')
			(*putc)(putc_arg, sign);
		while (*s != '\0')
			(*putc)(putc_arg, *s++);
		if (flags & FL_LEFT)
			pad(putc, putc_arg, ' ', width - total);
		return;
	}

	if (prec < 0)
		prec = 6;
	if (prec > FLT_MAXPREC)
		prec = FLT_MAXPREC;

	{
		double	v = (dv < 0.0) ? -dv : dv;
		int	more = 0;
		int	nd;

		if (style == 'g') {
			int sig = (prec == 0) ? 1 : prec;

			(void) float_round(v, sig, digs, &e10);
			/*
			 * C's rule: scientific when the exponent is below -4
			 * or not less than the precision; otherwise fixed,
			 * with the precision becoming digits after the point.
			 */
			if (e10 < -4 || e10 >= sig) {
				style = 'e';
				prec = sig - 1;
			} else {
				style = 'f';
				prec = sig - 1 - e10;
				if (prec < 0)
					prec = 0;
			}
			if (!(flags & FL_ALT))
				strip = 1;	/* %g drops trailing zeros */
		}

		if (style == 'e') {
			(void) float_round(v, prec + 1, digs, &e10);
		} else {
			int sig;

			/*
			 * Fixed notation asks for `prec' digits AFTER the
			 * point, so how many significant digits that is
			 * depends on where the point falls -- which has to be
			 * learned without rounding first, or a value that
			 * carries into a new decade moves the answer.
			 */
			nd = float_digits(v, 2, digs, &e10, &more);
			sig = e10 + 1 + prec;

			/*
			 * ⚠️ `nd' and not just the buffer: the generator is
			 * exact, so 0.5 comes back as ONE digit and digs[1]
			 * is the terminator.  Reading it as a digit made an
			 * exact half look like something above it, and
			 * "%.0f" of 0.5 printed 1 instead of 0.
			 */
			if (sig >= 1) {
				(void) float_round(v, sig, digs, &e10);
			} else if (sig == 0
				   && (digs[0] > '5'
				       || (digs[0] == '5'
					   && ((nd > 1 && digs[1] != '0')
					       || more)))) {
				/*
				 * Every significant digit is below the last
				 * place asked for, and the value still rounds
				 * up into it: 0.667 at "%.0f" is 1.  An exact
				 * half goes to even, and the digit it would
				 * carry into is zero, so it stays 0.
				 */
				digs[0] = '1';
				digs[1] = '\0';
				e10 = e10 + 1;
			} else {
				digs[0] = '0';
				digs[1] = '\0';
				e10 = 0;
			}
		}
	}

	/* Lay the body out: digits, point, fraction, exponent. */
	if (style == 'e') {
		int nfrac = prec;
		int ndig  = (int) strlen(digs);

		/*
		 * ⚠️ `digs' can be SHORTER than asked for: the generator is
		 * exact, so 1.0 yields one digit and not prec+1 of them.  A
		 * position past the end is a zero, and reading it out of the
		 * buffer instead is what printed "1." for "%e" of 1.
		 */
		if (strip)
			while (nfrac > 0
			       && (nfrac >= ndig || digs[nfrac] == '0'))
				nfrac--;
		body[nbody++] = digs[0];
		if (nfrac > 0 || (flags & FL_ALT)) {
			body[nbody++] = '.';
			for (i = 1; i <= nfrac; i++)
				body[nbody++] = (i < ndig) ? digs[i] : '0';
		}
		body[nbody++] = (char)(upper ? 'E' : 'e');
		body[nbody++] = (char)(e10 < 0 ? '-' : '+');
		{
			int a = e10 < 0 ? -e10 : e10;

			if (a >= 100)
				body[nbody++] = (char)('0' + a / 100);
			body[nbody++] = (char)('0' + (a / 10) % 10);
			body[nbody++] = (char)('0' + a % 10);
		}
	} else {
		int ndig = (int) strlen(digs);
		int nint = e10 + 1;
		int nfrac = prec;
		int k;

		if (nint <= 0) {
			body[nbody++] = '0';
		} else {
			for (k = 0; k < nint; k++)
				body[nbody++] = (k < ndig) ? digs[k] : '0';
		}

		if (strip) {
			/* Trailing zeros of the fraction go, and so does a
			 * point with nothing after it. */
			int last = nint + nfrac;

			while (nfrac > 0) {
				k = nint + nfrac - 1;
				if (k >= 0 && k < ndig && digs[k] != '0')
					break;
				if (k >= ndig)
					{ nfrac--; continue; }
				if (k < 0)
					break;
				nfrac--;
			}
			(void) last;
		}

		if (nfrac > 0 || (flags & FL_ALT)) {
			body[nbody++] = '.';
			for (k = 0; k < nfrac; k++) {
				int idx = nint + k;

				body[nbody++] = (idx >= 0 && idx < ndig)
					? digs[idx] : '0';
			}
		}
	}

	total = nbody + (sign != '\0' ? 1 : 0);
	padn = width - total;

	if (!(flags & FL_LEFT) && !(flags & FL_ZERO))
		pad(putc, putc_arg, ' ', padn);
	if (sign != '\0')
		(*putc)(putc_arg, sign);
	if (!(flags & FL_LEFT) && (flags & FL_ZERO))
		pad(putc, putc_arg, '0', padn);
	for (i = 0; i < nbody; i++)
		(*putc)(putc_arg, body[i]);
	if (flags & FL_LEFT)
		pad(putc, putc_arg, ' ', padn);
}


/*
 * Mach's register decoder: the value, then the names of the bits and the
 * bit-fields that are set in it.
 *
 *	printf("reg = %b\n", 0xb, "\10\4\3FIELD1=\2BITTWO\1BITONE")
 *
 * The string's first byte is the base as a control character ('\10' octal,
 * '\20' hex); each entry after it is a bit number -- or a PAIR of them for a
 * field -- followed by the name, up to the next control character.
 *
 * ⚠️ Kept, and kept whole.  Nothing calls it today: the six uses a census
 * found were the worked examples in these very comments.  That makes it an
 * unused FEATURE and not a duplicate, and only duplicates were the thing to
 * delete.  A shorter version that handled single bits and dropped the fields
 * was written first, which would have been a quiet loss of half of it.
 */
static void
print_bitfield(unsigned long long u, const char *p, unsigned int base,
	       void (*putc)(void *, int), void *putc_arg)
{
	char		buf[NUMBUF];
	unsigned int	n;
	int		any = 0;
	int		i;

	if (base != 8 && base != 16 && base != 10)
		base = 16;

	n = to_digits(buf, u, base, digits_lower);
	for (i = 0; i < (int) n; i++)
		(*putc)(putc_arg, buf[i]);

	if (u == 0)
		return;

	while ((i = (unsigned char) *p++) != 0) {
		if (*p <= 32 && *p != '\0') {
			/* A field: this bit number and the next bound it. */
			int j = (unsigned char) *p++;

			(*putc)(putc_arg, any ? ',' : '<');
			any = 1;
			while (*p > 32)
				(*putc)(putc_arg, *p++);
			n = to_digits(buf,
				      (u >> (j - 1)) & ((2ULL << (i - j)) - 1),
				      base, digits_lower);
			{
				int k;

				for (k = 0; k < (int) n; k++)
					(*putc)(putc_arg, buf[k]);
			}
		} else if (u & (1ULL << (i - 1))) {
			(*putc)(putc_arg, any ? ',' : '<');
			any = 1;
			while (*p > 32)
				(*putc)(putc_arg, *p++);
		} else {
			while (*p > 32)
				p++;
		}
	}
	if (any)
		(*putc)(putc_arg, '>');
}

void
_doprnt(const char *fmt,
	va_list			args,
	int			radix,	/* the base %r and %R print in */
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

		case 'D':
			is_signed = 1;
			base = 10;
			lensize = LEN_LONG;
			break;

		case 'r':
		case 'R':
			/* The base the caller handed _doprnt. */
			is_signed = 1;
			base = (radix >= 2 && radix <= 16)
				? (unsigned int) radix : 10;
			break;

		case 'n': {
			/*
			 * ⚠️ The one conversion here that WRITES THROUGH A
			 * CALLER'S POINTER.  Kept because it is a feature and
			 * not a duplicate, but it is a liability the moment a
			 * format string stops being a literal, and several C
			 * libraries have dropped it for exactly that.  Worth
			 * revisiting deliberately rather than by accident.
			 */
			int *np = va_arg(args, int *);

			if (np != 0)
				*np = 0;	/* no count is tracked */
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
			const char *bp = va_arg(args, const char *);
			unsigned int bbase = (unsigned int)(unsigned char) *bp++;

			print_bitfield(v, bp, bbase, putc, putc_arg);
			fmt++;
			continue;
		}

		case 'u':
			base = 10;
			break;

		case 'U':
			base = 10;
			lensize = LEN_LONG;
			break;

		case 'O':
			base = 8;
			lensize = LEN_LONG;
			break;

		case 'o':
			base = 8;
			/*
			 * ⚠️ `#' on octal is not a prefix, which is how this
			 * was written. C says it raises the precision enough
			 * to force a leading zero, so "%#o" of 0 is "0" and
			 * not "00".  Decided below, once the digits exist.
			 */
			break;

		case 'X':
			dig = digits_upper;
			/* FALLTHROUGH */
		case 'x':
			base = 16;
			break;

		case 'f':
		case 'F':
		case 'e':
		case 'E':
		case 'g':
		case 'G':
			print_float(va_arg(args, double), *fmt, prec, width,
				    flags, putc, putc_arg);
			fmt++;
			continue;

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
				/*
				 * ⚠️ ...and only when the precision has not
				 * already supplied it.  C says `#' raises the
				 * precision enough to force a leading zero,
				 * so "%#.12o" of a ten-digit value already
				 * has two and needs none added.  Adding one
				 * anyway made 39 of 40000 random cases one
				 * character too long.
				 */
				if (base == 8 && (flags & FL_ALT)
				    && numbuf[0] != '0'
				    && prec <= (int) ndig)
					prefix = "0";
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
