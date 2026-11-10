/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * mach_error_test.c -- a program of musl asks libmach what an error is called
 * (#917).
 *
 * Built twice: statically against uros_libc as /mach_error_test, and against
 * the umbrella libc.so as /mach_error_dyn.  Either way it asks
 * mach_error_string() for KERN_INVALID_ARGUMENT's text and prints it on
 * stdout, then hands the same error to mach_error(), which prints it on
 * stderr through fprintf_stderr().  It exits 0 only if the text is the one
 * libmach's table holds.  musl_test runs both, as [27] and [28].
 *
 * libmach_core had left out the table and fprintf_stderr(), and libc.so
 * exported neither function, so neither program linked.
 */

#include <stdio.h>
#include <string.h>

#include <mach/kern_return.h>
#include <mach_error.h>

#define	WANT	"(os/kern) invalid argument"

int
main(int argc, char **argv)
{
	const char	*name = argc > 0 && argv[0] != NULL ? argv[0] : "?";
	const char	*text = mach_error_string(KERN_INVALID_ARGUMENT);
	char		prefix[96];

	printf("%s: mach_error_string(KERN_INVALID_ARGUMENT) is \"%s\"\n",
	       name, text != NULL ? text : "(null)");
	(void)fflush(stdout);
	snprintf(prefix, sizeof prefix, "%s: mach_error() says", name);
	mach_error(prefix, KERN_INVALID_ARGUMENT);
	return text != NULL && strcmp(text, WANT) == 0 ? 0 : 1;
}
