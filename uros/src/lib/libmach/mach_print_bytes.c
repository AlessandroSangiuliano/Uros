/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Bytes through mach_print, the console that needs no port (#510, #761).
 *
 * mach_print takes a C string, and two writers have bytes: libmach's printf,
 * when the console refused them and they go out this way instead (#510), and
 * libposix-uros's console descriptors, fds 0, 1 and 2 of a task with no
 * controlling tty (#761).  The second used to cut its bytes into 127-byte
 * pieces and hand each to the trap as it was, so a NUL among them ended the
 * string there and the rest of the piece never reached the console -- while
 * write() said every byte had gone.  One loop for both now, so the two cannot
 * disagree about what a line is.
 *
 * ⚠️ A line to a trap, while it fits.  Two traps are two kernel printf()s, and
 * another processor's line can land between them (#578, #644).  A line longer
 * than MACH_PRINT_MAX - 1 bytes is split, because the trap would not carry it
 * whole: x86-64's copyinstr() refuses it and i386's cuts it (#763).
 *
 * ⚠️ A NUL is left out of the piece, not used to end it.  A C string cannot
 * carry one and a terminal shows nothing for one, so leaving it out changes
 * nothing a reader sees -- and ending the piece there would split the line it
 * sits in, which is what the paragraph above exists to prevent.
 */

#include <mach/mach_traps.h>
#include <mach/mach_syscalls.h>		/* MACH_PRINT_MAX */

void
mach_print_bytes(const char *p, vm_size_t n)
{
	char		piece[MACH_PRINT_MAX];
	vm_size_t	took, k;

	while (n > 0) {
		took = 0;
		k = 0;
		while (took < n && k < sizeof (piece) - 1) {
			char	c = p[took++];

			if (c == '\0')
				continue;
			piece[k++] = c;
			if (c == '\n')
				break;
		}
		if (k > 0) {
			piece[k] = '\0';
			mach_print(piece);
		}
		p += took;
		n -= took;
	}
}
