/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * strchr — locate the first occurrence of c in the NUL-terminated
 * string s.  The terminating NUL is considered part of the string, so
 * strchr(s, '\0') returns a pointer to it.  Returns NULL if c is not
 * found.
 */

#include <sa_mach/string.h>

char *
strchr(const char *s, int c)
{
	const char ch = (char)c;

	for (;; s++) {
		if (*s == ch)
			return (char *)s;
		if (*s == '\0')
			return (char *)0;
	}
}
