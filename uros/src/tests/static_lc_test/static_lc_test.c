/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * static_lc_test.c -- a program linked the way a build that is not ours links
 * it (#929): the compiler driven by the musl-gcc.specs the install directory
 * holds, -static, and the -lc the driver adds.  It starts in musl's crt1 and
 * __libc_start_main(), not in libmach's crt0, and it finds libc.a in the
 * install directory.
 *
 * Before #929 that libc.a was musl's own, whose system calls are weak stubs
 * answering -ENOSYS, and nothing gave such a program the Mach personality: no
 * task port seeded, no proc_server identity.  So it asks for its pid and
 * prints it, and exits 0 only if both came back; musl_test runs it as [29].
 */

#include <stdio.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	pid_t	pid = getpid();
	int	n;

	n = printf("%s: linked -static with a plain -lc, and getpid() "
		   "answered %d\n", argc > 0 && argv[0] ? argv[0] : "?",
		   (int)pid);
	if (n < 0 || fflush(stdout) != 0)
		return 2;
	return pid > 0 ? 0 : 1;
}
