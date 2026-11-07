/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * What uros_libc_check.sh has to refuse, so that every build sees it refuse
 * (#733).  It goes into an archive twice under one name: strchr is a name musl
 * defines, and uros_libc_check_canary is then a name two members define for
 * good.  Nothing links it.
 */
char *
strchr(const char *s, int c)
{
	(void)c;
	return (char *)s;
}

int	uros_libc_check_canary = 1;
