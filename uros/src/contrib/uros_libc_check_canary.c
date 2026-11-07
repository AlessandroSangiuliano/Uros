/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * What uros_libc_check.sh has to refuse every time libc-uros.a is made (#733).
 * This object goes once into each of the canary's first two parts.  Six of its
 * names are names musl defines: strchr and __errno_location strong, malloc
 * only weak, environ as a weak object, in6addr_any read-only, optind as data.
 * Ours are text (T), zeroed data (B), read-only data (R), a common (C) and data
 * (D).  Five of them and uros_libc_check_canary are then names two members of
 * ours, one in each part, define for good; optind, a common, is not one, since
 * commons merge.  __x86.get_pc_thunk.uros_canary is a name the check has to
 * pass over, spelled as the i386 PIC thunks are.  Built with -fno-common, so
 * that environ is B, and optind is a common only by its attribute.  Nothing
 * links it.
 */
char *
strchr(const char *s, int c)
{
	(void)c;
	return (char *)s;
}

int *
__errno_location(void)
{
	return 0;
}

void *
malloc(__SIZE_TYPE__ n)
{
	(void)n;
	return 0;
}

char		**environ;
const int	in6addr_any = 1;
int		optind __attribute__((common));
int		uros_libc_check_canary = 1;
int		uros_libc_check_canary_thunk __asm__("__x86.get_pc_thunk.uros_canary") = 1;
