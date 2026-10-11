/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Boot flags (#458) and `name=value' words (#373).
 *
 * The whole of this machine's boot-argument handling, and it is two functions
 * that each read the string on purpose.  i386 has parse_arguments(), a table
 * of globals, and the rule that every one of those globals must be forced into
 * .data because the parser runs before the BSS is cleared -- a rule that is
 * not visible at the declaration and has been forgotten three times.
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

/*
 * The number that ends a `name=' word: `0x' and hexadecimal digits, or
 * decimal digits, and nothing else before the end of the word.
 */
static int
boot_number(const char *p, uint64_t *out)
{
	uint64_t	v = 0;
	unsigned	base = 10, digits = 0;

	if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
		base = 16;
		p += 2;
	}
	for (; *p != '\0' && *p != ' ' && *p != '\t'; p++) {
		unsigned	d;

		if (*p >= '0' && *p <= '9')
			d = (unsigned)(*p - '0');
		else if (base == 16 && *p >= 'a' && *p <= 'f')
			d = (unsigned)(*p - 'a') + 10;
		else if (base == 16 && *p >= 'A' && *p <= 'F')
			d = (unsigned)(*p - 'A') + 10;
		else
			return -1;
		if (v > (UINT64_MAX - d) / base)
			return -1;
		v = v * base + d;
		digits++;
	}
	if (digits == 0)
		return -1;
	*out = v;
	return 1;
}

/*
 * A word counts when it begins with the name and an '=', so `xramoops=1' is
 * not `ramoops', and only the first such word is read.  A value that does not
 * parse, or does not fit in 64 bits, is -1 and not 0: it is a different answer
 * from no value at all, and the caller has to be able to say which it got.
 */
int
boot_value(const char *name, uint64_t *out)
{
	const char	*p = mb2_cmdline();

	if (p == (const char *) 0)
		return 0;

	while (*p != '\0') {
		const char	*n = name;

		if (*p == ' ' || *p == '\t') {
			p++;
			continue;
		}
		/* The start of a word. */
		while (*n != '\0' && *p == *n) {
			p++;
			n++;
		}
		if (*n == '\0' && *p == '=')
			return boot_number(p + 1, out);
		while (*p != '\0' && *p != ' ' && *p != '\t')
			p++;
	}

	return 0;
}
