/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * va_list for x86-64: the compiler's, because only the compiler knows (#426).
 *
 * ⚠️ i386's version of this file says
 *
 *	typedef char *va_list;
 *
 * and that is right there and nowhere else.  On i386 every argument is pushed,
 * so a walking char pointer IS the argument list.  On x86-64 the first six
 * integer arguments arrive in registers and the rest on the stack, and a
 * va_list has to describe both halves plus how far into each one has got --
 * which is why the ABI makes it a structure with gp_offset, fp_offset,
 * overflow_arg_area and reg_save_area.
 *
 * There is nothing to write here, then, and writing it would be the mistake.
 * __builtin_va_list is that structure as this compiler lays it out, and the
 * register save area it refers to is emitted by the same compiler in the
 * variadic function's prologue.  A hand-rolled definition cannot agree with a
 * prologue it does not generate.
 *
 * ── What it cost before this existed ──────────────────────────────────
 *
 * mach_services/include/machine/va_list.h named sa_mach/i386/va_list.h
 * outright, so libmach on x86-64 got i386's varargs: va_start computed
 * `&fmt + 8' and va_arg walked the stack from there.  printf's literal text
 * still came out -- the format string is an ordinary parameter -- and every
 * %s, %d and %x read whatever the compiler had placed after fmt on the stack.
 * In printf() that is `struct printf_state', so a %s came back as
 * 0x65735f656d616e28: the eight bytes "(name_se", the OUTPUT BUFFER being
 * printed into.  It read as corrupt data and it was a broken ABI.
 */

#ifndef	_MACHINE_VALIST_H
#define _MACHINE_VALIST_H

/*
 * The _HIDDEN_VA_LIST dance is i386's and is kept, because the headers that
 * play it -- <stdio.h>, <stdlib.h> -- are shared and still expect it: a
 * declaration of vprintf() needs the type before <stdarg.h> has been included
 * and must not define the user-visible name.
 */
#if	!defined(_HIDDEN_VA_LIST) && !defined(_VA_LIST)
#define _VA_LIST
typedef	__builtin_va_list va_list;
#elif	defined(_HIDDEN_VA_LIST) && !defined(_VA_LIST)
#define _VA_LIST
typedef __builtin_va_list __va_list;
#elif	defined(_HIDDEN_VA_LIST) && defined(_VA_LIST)
#undef _HIDDEN_VA_LIST
typedef __va_list va_list;
#endif

#endif	/* _MACHINE_VALIST_H */
