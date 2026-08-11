/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The bootstrap server's command line, parsed once for every machine (#422).
 *
 * ⚠️ This function used to live in <machine>/boot_dep.c, and there were three
 * copies of it -- i386, POWERMAC, HP700 -- because that is where the file
 * happened to be when it was written.  Nothing in it is machine-dependent: it
 * walks argv looking for -a, -k, -u and -f, and builds a path.  Adding x86-64
 * would have made a fourth, and four copies of a parser is four places for a
 * switch to be understood differently.
 *
 * It is moved rather than rewritten: the body below is i386's, verbatim, so
 * that target keeps the behaviour it has been booting with.
 */

#include <mach.h>
#include "bootstrap.h"

/*
 * For the AT386, we ask the kernel for the complete boot string
 */
#define DEFAULT_CONF_DIRECTORY "/dev/boot_device/mach_servers/"
#define DEFAULT_BOOT_FILE "bootstrap.conf"

void
parse_args(int argc, char **argv, char *conf_file)
{
	char *arg, ch;
	int conf_file_set = 0;

	while (argc > 1 && *(arg = argv[1]) == '-') {
		switch (ch = *++arg) {
		case 'a':
			prompt = TRUE;
			break;
		case 'k':
			/* Always try to load servers in kernel */
			collocation_autotry = TRUE;
			break;
		case 'u':
			/* Never try to load servers in kernel */
			collocation_prohibit = TRUE;
			break;
		case 'f':
			/* Requires an argument.  */
			if (argc <= 1) {
				BOOTSTRAP_IO_LOCK();
				printf("%s: -f switch lacks argument.\n",
				       program_name);
				BOOTSTRAP_IO_UNLOCK();
				break;
			}

			++argv; --argc;
			/* Use a different bootstrap.conf.  */
			if (*argv[1] == '/') {
				/* Full path name provided; don't do any
				   defaulting.  */
				strlcpy(conf_file, argv[1], PATH_MAX);
			} else {
				/* Path is relative to default directory.  */
				strlcpy(conf_file, DEFAULT_CONF_DIRECTORY, PATH_MAX);
				strlcat(conf_file, argv[1], PATH_MAX);
			}
			conf_file_set = 1;
			break;
		case '-':
			return;
		default:
			BOOTSTRAP_IO_LOCK();
			printf("%s: Unrecognized switch '-%c'\n",
			       program_name, ch);
			BOOTSTRAP_IO_UNLOCK();
			break;
		}
		argv++;
		argc--;
	}
	if (!conf_file_set) {
		/* Default bootstrap configuration.  */
		strlcpy(conf_file, DEFAULT_CONF_DIRECTORY, PATH_MAX);
		strlcat(conf_file, DEFAULT_BOOT_FILE, PATH_MAX);
        }
}

