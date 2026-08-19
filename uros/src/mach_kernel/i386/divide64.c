/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Sixty-four-bit division on a thirty-two-bit machine (#415).
 *
 * ── Why this file exists ─────────────────────────────────────────────
 *
 * i386 has no instruction that divides a 64-bit value, so the compiler emits
 * a call to a helper.  This kernel links -nostdlib and therefore without the
 * compiler's runtime library, so the helper has to be here.  That is not a
 * new arrangement: i386/divide.S next door already supplies __divsi3 and
 * __udivsi3 for exactly the same reason, and has since 1990 -- under the name
 * gcc.S until this issue, which named the compiler rather than the contents.
 * This is the same answer for the wider operands, and it stays in the tree
 * under the project's own licence rather than being borrowed from a runtime
 * whose licence would follow it.
 *
 * ── What asked for it ────────────────────────────────────────────────
 *
 * printf's conversion of a `long long', which is #415's own work: the
 * formatter used to read the `l' length modifier and throw it away, so every
 * conversion fetched a long and no 64-bit value was ever divided.  Teaching
 * it to fetch what the format asks for is what made the division appear.
 *
 * ⚠️ And the Release build did not notice, which is the part worth knowing:
 * at -O2 the compiler unrolls the loop in printnum() and the division by a
 * small constant base becomes a multiply, so the helper is never referenced.
 * At -O0 it is, and the link fails.  A kernel that builds in one
 * configuration and not another because of an optimisation is a kernel whose
 * arithmetic is one compiler flag away from not linking; the helper removes
 * that, whether or not anyone builds -O0 again.
 *
 * ── The algorithm ────────────────────────────────────────────────────
 *
 * Shift-and-subtract, sixty-four iterations, no cleverness.  It is called to
 * print a number and never in a loop that matters, so the version that is
 * obviously correct is worth more than the version that is fast: a wrong
 * long division would produce plausible digits, which is the worst possible
 * failure for something whose entire job is to report other failures.
 */

#include <stdint.h>

/*
 * ⚠️ Division by zero is undefined behaviour in C, and there is no answer
 * this function could return that would be right.  It is not special-cased:
 * with d == 0 the comparison below always succeeds, so the quotient comes out
 * all ones and the remainder zero.  Said here rather than pretended about --
 * the caller has already made the mistake by the time control arrives.
 */
static uint64_t
udivmod64(uint64_t n, uint64_t d, uint64_t *rem)
{
	uint64_t	q = 0;
	uint64_t	r = 0;
	int		i;

	for (i = 63; i >= 0; i--) {
		r = (r << 1) | ((n >> i) & 1);
		if (r >= d) {
			r -= d;
			q |= (uint64_t) 1 << i;
		}
	}

	if (rem != 0)
		*rem = r;

	return q;
}

uint64_t __udivdi3(uint64_t n, uint64_t d);
uint64_t __umoddi3(uint64_t n, uint64_t d);
int64_t  __divdi3(int64_t n, int64_t d);
int64_t  __moddi3(int64_t n, int64_t d);

uint64_t
__udivdi3(uint64_t n, uint64_t d)
{
	return udivmod64(n, d, 0);
}

uint64_t
__umoddi3(uint64_t n, uint64_t d)
{
	uint64_t	r;

	(void) udivmod64(n, d, &r);
	return r;
}

/*
 * The signed pair, in terms of the unsigned one.
 *
 * ⚠️ The negation is done on the UNSIGNED value, not on the signed one.
 * -(-9223372036854775808) has no representation as a signed 64-bit integer,
 * so negating the signed value first is undefined for exactly one input --
 * and it is the input somebody eventually prints.
 *
 * C99 rounds the quotient toward zero and gives the remainder the sign of the
 * dividend, which is what these two produce.
 */
int64_t
__divdi3(int64_t n, int64_t d)
{
	int		negate = 0;
	uint64_t	un, ud, uq;

	un = (n < 0) ? (uint64_t) 0 - (uint64_t) n : (uint64_t) n;
	ud = (d < 0) ? (uint64_t) 0 - (uint64_t) d : (uint64_t) d;
	negate = (n < 0) != (d < 0);

	uq = udivmod64(un, ud, 0);

	return negate ? (int64_t) ((uint64_t) 0 - uq) : (int64_t) uq;
}

int64_t
__moddi3(int64_t n, int64_t d)
{
	uint64_t	un, ud, ur;

	un = (n < 0) ? (uint64_t) 0 - (uint64_t) n : (uint64_t) n;
	ud = (d < 0) ? (uint64_t) 0 - (uint64_t) d : (uint64_t) d;

	(void) udivmod64(un, ud, &ur);

	return (n < 0) ? (int64_t) ((uint64_t) 0 - ur) : (int64_t) ur;
}
