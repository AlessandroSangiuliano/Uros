/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The framebuffer consoles' UTF-8 (#661).
 *
 * The kernel and the programs it runs print UTF-8 -- some twelve hundred em
 * dashes in the sources' strings -- and the font draws one glyph of 256.  A
 * console that drew a glyph per BYTE showed the em dash's three, e2 80 94, as
 * "ÔÇö": on a machine with no serial port, the only console there is.  i386
 * decoded the dashes since #372 and drew everything else as '-'; x86-64's
 * console, ported from it for #568, decoded nothing.
 *
 * So both decode here, one byte at a time, and ask device/fbcons_font.h --
 * the font's own Unicode table, generated with its glyphs -- which glyph a
 * code point is.
 *
 * ── What is drawn where the text cannot be ────────────────────────────
 *
 * FBCONS_FONT_REPLACEMENT, ■, in two cases that are counted apart:
 *
 *   - a code point the font has no glyph for: the text says something the
 *     screen cannot show, and that line does not read as the wire's;
 *   - bytes that are not UTF-8: the wire carries the same broken bytes, and a
 *     terminal shows U+FFFD where this shows ■.
 *
 * 🔑 Broken sequences are replaced as The Unicode Standard 16.0 says to, in
 * §3.9.6 "U+FFFD Substitution of Maximal Subparts": one ■ for the longest
 * start of a well-formed sequence that a byte then broke, and that byte taken
 * afresh.  It is the rule Python's errors="replace" follows, so the screen and
 * a decoded serial log agree on how many there are -- which is what
 * scripts/fbcons-readback.py compares.  That is why the second byte's range
 * depends on the first (§3.9.3, Table 3-7): E0 must be followed by A0..BF,
 * not 80..BF, or an overlong form would decode, and ED by 80..9F, or a
 * surrogate would.
 *
 * ⚠️ No lock, and the writers are not serialised.  The state belongs to one
 * console, and on x86-64 cons_wire_byte() draws before it takes the port's
 * lock -- the ring off, the trap reporter, the way down -- while on i386
 * cnputc() takes none.  So two processors' bytes can meet in the middle of a
 * sequence: then the screen draws a ■ where the wire may have split them
 * otherwise, and the two can count differently, as the drawing position can
 * already lose a cell (fbcons.h: no locks).  A lost update costs no more: the
 * next byte that does not continue a sequence closes it.
 */

#ifndef _DEVICE_FBCONS_UTF8_H_
#define _DEVICE_FBCONS_UTF8_H_

#include <device/fbcons_font.h>

/*
 * What fbcons_utf8_feed() hands back: below 0x100, an ASCII byte, control
 * codes included, for the console to act on as it always has; with this bit,
 * a glyph to draw where the cursor is, whatever its number -- U+2192 is glyph
 * 0x1A, which as a byte would be a control code.
 */
#define FBCONS_UTF8_GLYPH	0x100u

struct fbcons_utf8 {
	unsigned int	cp;		/* the code point so far */
	unsigned int	need;		/* continuation bytes still due */
	unsigned char	lo, hi;		/* the range the next one must be in */
	unsigned int	no_glyph;	/* code points the font cannot draw */
	unsigned int	first_no_glyph;	/* the first of them */
	unsigned int	not_utf8;	/* maximal subparts that were not UTF-8 */
};

/* The glyph for a code point above ASCII, or the replacement, counted. */
static inline unsigned int
fbcons_utf8_glyph(struct fbcons_utf8 *u, unsigned int cp)
{
	unsigned int lo = 0;
	unsigned int hi = sizeof(fbcons_font_unicode) /
			  sizeof(fbcons_font_unicode[0]);

	while (lo < hi) {
		unsigned int mid = lo + (hi - lo) / 2;

		if (fbcons_font_unicode[mid].cp == cp)
			return FBCONS_UTF8_GLYPH | fbcons_font_unicode[mid].glyph;
		if (fbcons_font_unicode[mid].cp < cp)
			lo = mid + 1;
		else
			hi = mid;
	}
	/*
	 * The first before the count, which a report reads the other way
	 * round: one that sees a count sees the first it names (#661's
	 * review).  The compiler is held to the order; both targets' stores
	 * already keep it.
	 */
	if (u->no_glyph == 0)
		u->first_no_glyph = cp;
	__asm__ volatile("" ::: "memory");
	u->no_glyph++;
	return FBCONS_UTF8_GLYPH | FBCONS_FONT_REPLACEMENT;
}

/*
 * One byte in; in out[], what to draw for it, and the count.  Zero while a
 * sequence is still being read; two when a byte broke a sequence and is then
 * something of its own -- the ■ for what came before, then it.
 */
static inline unsigned int
fbcons_utf8_feed(struct fbcons_utf8 *u, unsigned char b, unsigned int out[2])
{
	unsigned int n = 0;

	if (u->need) {
		if (b >= u->lo && b <= u->hi) {
			u->cp = (u->cp << 6) | (b & 0x3f);
			u->lo = 0x80;
			u->hi = 0xbf;
			if (--u->need)
				return 0;
			out[0] = fbcons_utf8_glyph(u, u->cp);
			return 1;
		}
		u->need = 0;
		u->not_utf8++;
		out[n++] = FBCONS_UTF8_GLYPH | FBCONS_FONT_REPLACEMENT;
	}

	if (b < 0x80) {
		out[n++] = b;
		return n;
	}

	u->lo = 0x80;
	u->hi = 0xbf;
	if (b >= 0xc2 && b <= 0xdf) {
		u->need = 1;
		u->cp = b & 0x1f;
	} else if (b >= 0xe0 && b <= 0xef) {
		u->need = 2;
		u->cp = b & 0x0f;
		if (b == 0xe0)
			u->lo = 0xa0;		/* past it, an overlong form */
		else if (b == 0xed)
			u->hi = 0x9f;		/* past it, a surrogate */
	} else if (b >= 0xf0 && b <= 0xf4) {
		u->need = 3;
		u->cp = b & 0x07;
		if (b == 0xf0)
			u->lo = 0x90;		/* past it, an overlong form */
		else if (b == 0xf4)
			u->hi = 0x8f;		/* past it, beyond U+10FFFF */
	} else {
		/* A continuation with nothing to continue, C0, C1, F5..FF. */
		u->not_utf8++;
		out[n++] = FBCONS_UTF8_GLYPH | FBCONS_FONT_REPLACEMENT;
	}
	return n;
}

#endif	/* _DEVICE_FBCONS_UTF8_H_ */
