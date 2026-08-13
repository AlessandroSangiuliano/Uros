/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * stdarg for x86-64: four builtins, and deliberately nothing else (#426).
 *
 * ⚠️ i386's version of this file computes the argument list by arithmetic:
 *
 *	#define va_start(AP, LASTARG)					\
 *		(AP = ((char *) &(LASTARG) + __va_rounded_size (LASTARG)))
 *	#define va_arg(AP, mode)					\
 *		(AP += __va_rounded_size (mode),			\
 *		 *((mode *) (AP - __va_rounded_size (mode))))
 *
 * Take the address of the last named argument, step over it, and keep
 * stepping.  That is correct on a machine that pushes every argument, and it
 * is the whole of what varargs needs there.
 *
 * It cannot be adapted here, and the reason is not width.  On x86-64 the first
 * six integer arguments are never on the stack at all: they are in registers,
 * which have no addresses.  &LASTARG does not point into the argument list --
 * it points at wherever the callee happened to spill that one parameter, if it
 * spilled it.  The list itself only exists because the variadic prologue
 * *builds* it, copying the argument registers into a save area whose position
 * the compiler chooses.
 *
 * So the compiler is the only thing that can implement this, and these four
 * builtins are the interface it exposes.  Writing the arithmetic version for
 * this machine would mean predicting a stack frame the optimiser is free to
 * lay out differently.
 */

#ifndef	_MACHINE_STDARG_H
#define _MACHINE_STDARG_H

#include <machine/va_list.h>

#define	va_start(AP, LASTARG)	__builtin_va_start(AP, LASTARG)
#define	va_arg(AP, TYPE)	__builtin_va_arg(AP, TYPE)
#define	va_end(AP)		__builtin_va_end(AP)
#define	va_copy(DEST, SRC)	__builtin_va_copy(DEST, SRC)

#endif	/* _MACHINE_STDARG_H */
