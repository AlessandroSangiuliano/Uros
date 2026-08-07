/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Boot flags (#458).
 *
 * The whole of this machine's boot-argument handling, and it is one function
 * on purpose.  i386 has parse_arguments(), a table of globals, and the rule
 * that every one of those globals must be forced into .data because the
 * parser runs before the BSS is cleared -- a rule that is not visible at the
 * declaration and has been forgotten three times.
 *
 * Here the command line is simply read where the answer is wanted.  There is
 * no parse step to run too early, no global to place, and nothing to forget.
 */

#include <boot/bootarg.h>
#include <boot/multiboot2.h>

/*
 * Flags are single letters after a '-', and several may share one dash:
 * `-rD' is the same as `-r -D'.  A letter only counts inside a word that
 * begins with a dash, so a module path or a value that happens to contain the
 * letter cannot switch a flag on by accident.
 */
int
boot_flag(char c)
{
	const char	*p = mb2_cmdline();
	int		in_flag_word = 0;

	if (p == (const char *) 0)
		return 0;

	for (; *p != '\0'; p++) {
		if (*p == ' ' || *p == '\t') {
			in_flag_word = 0;
			continue;
		}
		if (!in_flag_word) {
			/*
			 * First character of a word: a dash opens a flag
			 * word, anything else opens a word we ignore to its
			 * end.
			 */
			in_flag_word = (*p == '-') ? 1 : -1;
			continue;
		}
		if (in_flag_word == 1 && *p == c)
			return 1;
	}

	return 0;
}
