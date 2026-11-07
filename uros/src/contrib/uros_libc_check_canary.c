/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * What uros_libc_check.sh has to refuse every time libc-uros.a is made (#733).
 * This object goes once into each of the canary's first two parts.  strchr and
 * __errno_location are names musl defines strong, environ one it defines as a
 * weak object and malloc one it defines only weak; those four and
 * uros_libc_check_canary are then names two members of ours, one in each part,
 * define for good, as text (T), zeroed data (B) and data (D).  The last name
 * is one the check has to pass over: the i386 PIC thunks are spelled like it.
 * Built with -fno-common, so that environ is B and not a common.  Nothing
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

char	**environ;
int	uros_libc_check_canary = 1;
int	uros_libc_check_canary_thunk __asm__("__x86.get_pc_thunk.uros_canary") = 1;
