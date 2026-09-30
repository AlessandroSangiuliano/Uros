/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * A report line built in a local buffer, and printed by ONE printf (#593).
 *
 * A line made of several printf calls is a line another processor's output can
 * land inside (#578).  And the kernel's sprintf() is no way out: it writes
 * through one static pointer, so two processors formatting at once write into
 * each other's buffers.  So a line whose parts depend on what was measured --
 * a clock with no rate, a total a gap made meaningless -- is put together here,
 * piece by piece, and handed to printf("%s\n") whole.
 */

#ifndef _X86_64_TIME_LINE_H_
#define _X86_64_TIME_LINE_H_

#include <stdint.h>

struct line {
	char		b[512];
	unsigned	n;
	int		cut;
};

/*
 * 🔴 A LINE THAT DOES NOT FIT IS CUT, AND SAYS SO.  It used to stop at the
 * buffer's end without a word, which reads as a line that had nothing more
 * to say: the HPET's window line under UROS_ABLATE_593_ICR_OPEN ran past 399
 * characters in every window, and lost its last words -- the name of the
 * ablation among them -- with no sign of it.
 */
#define	LINE_CUT_MARK	" [line cut]"

static inline void line_start(struct line *l)
{
	l->n = 0;
	l->cut = 0;
	l->b[0] = '\0';
}

/* Room is kept for the mark, so the mark is never itself cut. */
static inline void put_s(struct line *l, const char *s)
{
	const unsigned room = sizeof(l->b) - sizeof(LINE_CUT_MARK);

	while (!l->cut && *s != '\0') {
		if (l->n == room) {
			const char *m = LINE_CUT_MARK;

			while (*m != '\0')
				l->b[l->n++] = *m++;
			l->cut = 1;
		} else {
			l->b[l->n++] = *s++;
		}
	}
	l->b[l->n] = '\0';
}

static inline void put_u(struct line *l, uint64_t v)
{
	char	d[21];
	char	*p = d + sizeof(d) - 1;

	*p = '\0';
	do {
		*--p = (char)('0' + v % 10);
		v /= 10;
	} while (v != 0);
	put_s(l, p);
}

#endif	/* _X86_64_TIME_LINE_H_ */
