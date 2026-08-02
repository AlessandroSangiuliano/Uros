/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Block moves and fills for x86-64 (#453).
 *
 * These are not optional and not only for callers who ask: the compiler
 * emits calls to memcpy() and memset() for structure assignment and for
 * array initialisation whatever -fno-builtin says, so a kernel without them
 * does not link.  They are the first thing the machine-independent tree
 * needs from this machine that is not a header.
 *
 * ── Why `rep movsb' and not a hand-unrolled loop ──────────────────────
 *
 * i386's bcopy.S copies whole words with `rep movsl' and mops up the tail
 * with `rep movsb', because on the processors it was written for the
 * byte-at-a-time form ran one byte per cycle and the word form four.  That
 * is no longer the machine we are on.
 *
 * Every x86-64 part since Ivy Bridge implements ERMSB, and the recent ones
 * FSRM, which make `rep movsb' the fastest available copy: the hardware
 * recognises the sequence and moves whole cache lines, using store
 * bandwidth a software loop cannot reach and without polluting the pipeline
 * with the loop itself.  The word-at-a-time version is now the slow one, and
 * an unrolled SSE copy is not available to us at all -- this kernel is built
 * -mgeneral-regs-only, deliberately, so that entering the kernel never has
 * to save vector state (#408).
 *
 * ⚠️ The exception is small sizes, where the instruction's startup cost
 * dominates.  That is why each routine branches on a small-size threshold
 * and does those by hand.  The threshold is a plain constant rather than
 * something derived from CPUID: dispatching per processor is a real
 * technique, but it is worth doing only once there is a measurement of this
 * kernel's own copy sizes, and there is none yet.  Written this way so that
 * the day that measurement exists, one constant moves.
 *
 * ── Direction ─────────────────────────────────────────────────────────
 *
 * memcpy() may assume its regions do not overlap; bcopy() may not, and the
 * heritage tree uses it for exactly that.  So bcopy() checks and copies
 * backwards when it must, with the direction flag set for the duration and
 * cleared immediately -- the System V ABI requires DF clear at every call
 * boundary, and a routine that returns with it set corrupts the next string
 * operation anywhere in the kernel.
 */

#include <stdint.h>

/*
 * Its own declarations, so that a signature drifting apart from them is a
 * build error rather than a silent disagreement between two translation
 * units (#448).  That is not theoretical here: the heritage spellings take
 * `char *' where the C ones take `void *', and writing them the modern way
 * without reading the header would compile in this file and warn at every
 * caller.
 */
#include <string.h>

/*
 * Below this many bytes, the fixed cost of starting a `rep' sequence is more
 * than the copy itself.  Measured on nothing yet -- see above.
 */
#define	REP_THRESHOLD	64

void *
memcpy(void *dst, const void *src, vm_size_t n)
{
	unsigned char		*d = dst;
	const unsigned char	*s = src;

	if (n < REP_THRESHOLD) {
		while (n--)
			*d++ = *s++;
		return dst;
	}

	__asm__ volatile("rep movsb"
			 : "+D"(d), "+S"(s), "+c"(n)
			 : : "memory");
	return dst;
}

void *
memset(void *dst, int c, vm_size_t n)
{
	unsigned char	*d = dst;
	unsigned char	 v = (unsigned char) c;

	if (n < REP_THRESHOLD) {
		while (n--)
			*d++ = v;
		return dst;
	}

	__asm__ volatile("rep stosb"
			 : "+D"(d), "+c"(n)
			 : "a"(v)
			 : "memory");
	return dst;
}

int
memcmp(const void *a, const void *b, vm_size_t n)
{
	const unsigned char	*p = a, *q = b;

	while (n--) {
		if (*p != *q)
			return (int) *p - (int) *q;
		p++;
		q++;
	}

	return 0;
}

/*
 * The heritage spelling, and the one that may overlap.  Note the argument
 * order is the reverse of memcpy's, which is the single most common way to
 * get a call to this wrong.
 */
void
bcopy(const char *src, char *dst, vm_size_t n)
{
	unsigned char		*d = (unsigned char *) dst;
	const unsigned char	*s = (const unsigned char *) src;

	if (n == 0)
		return;

	if (d <= s || d >= s + n) {
		(void) memcpy(dst, src, n);
		return;
	}

	/*
	 * Overlapping with the destination above the source: copy from the
	 * top down.  `std' is in force for exactly this instruction and is
	 * cleared before returning, because every ABI-conforming caller is
	 * entitled to assume DF is clear.
	 */
	d += n - 1;
	s += n - 1;
	__asm__ volatile("std\n\t"
			 "rep movsb\n\t"
			 "cld"
			 : "+D"(d), "+S"(s), "+c"(n)
			 : : "memory");
}

void
bzero(char *dst, vm_size_t n)
{
	(void) memset(dst, 0, n);
}

/*
 * Also the heritage spelling, with the arguments the other way round again.
 */
int
bcmp(const char *a, const char *b, vm_size_t n)
{
	return memcmp(a, b, n);
}

/*
 * The one string routine the kernel's printf needs, and it needs it for
 * padding: %s with a width has to know how long the string is before it can
 * decide how many spaces go in front (#453).
 *
 * Byte at a time.  There are word-at-a-time tricks and this is not the place
 * for them: the strings a kernel formats are short, and a word-wise scan
 * reads past the terminator by up to seven bytes -- which is harmless on a
 * mapped page and a fault on the last string of a mapping.
 */
vm_size_t
strlen(const char *s)
{
	const char	*p = s;

	while (*p != '\0')
		p++;

	return (vm_size_t) (p - s);
}
