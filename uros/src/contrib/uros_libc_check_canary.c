/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * What uros_libc_check.sh has to refuse every time libc-uros.a is made (#733).
 * This object goes once into each of the canary's two parts: strchr (T) and
 * environ (B) are names musl defines, and those two and uros_libc_check_canary
 * (D) are then names two members of ours, one in each part, define for good.
 * Built with -fno-common, so that environ is B and not a common.  Nothing
 * links it.
 */
char *
strchr(const char *s, int c)
{
	(void)c;
	return (char *)s;
}

char	**environ;
int	uros_libc_check_canary = 1;
