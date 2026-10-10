/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Boot flags, read from the command line the loader passed (#458), and
 * `name=value' words whose value is a number (#373).
 */

#ifndef	_X86_64_BOOT_BOOTARG_H_
#define	_X86_64_BOOT_BOOTARG_H_

#include <stdint.h>

/*
 * Was `-<c>' given on the boot command line?
 *
 * ⚠️ Asks the command line each time rather than parsing once into a global,
 * and that is deliberate.  i386 parses into globals and every one of them has
 * to be placed in .data by hand, because parse_arguments() runs before the
 * BSS is cleared and a flag in BSS is set and then wiped (#337 -- it has cost
 * this project three separate bugs).  A function that reads the string has no
 * such hazard, and the string is read a handful of times at boot.
 */
int	boot_flag(char c);

/*
 * Was `name=<number>' given?  1 and the number when it was, 0 when no word
 * has that name, -1 when one does and its value is not a number (#373).
 * Read the same way as boot_flag(), for the same reason.
 */
int	boot_value(const char *name, uint64_t *out);

#endif	/* _X86_64_BOOT_BOOTARG_H_ */
