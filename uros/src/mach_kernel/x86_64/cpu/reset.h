/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Restarting the machine (#373).
 */

#ifndef	_X86_64_CPU_RESET_H_
#define	_X86_64_CPU_RESET_H_

/*
 * Read the FADT's reset register, judge whether it is one this kernel can
 * write, map it when it is in memory, and say what was found.  Once, at boot,
 * after the ACPI tables can be read: the mapping can allocate, and a reset
 * from a panic must not.
 */
void	reset_init(void);

/*
 * Restart the machine: the FADT's register, then 0xcf9, the keyboard
 * controller and a triple fault, each said before it is tried.  For a caller
 * that has stopped the other processors, or is the only one left; it does
 * not return.
 */
void	reset_machine(void) __attribute__((noreturn));

#endif	/* _X86_64_CPU_RESET_H_ */
