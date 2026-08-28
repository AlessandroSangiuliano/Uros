/*
 * Copyright 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
 */

/*
 *	limits.h — the ranges of the integer types, for the Uros userland (#506).
 *
 *	Why this file has to exist even though -ffreestanding is set: GCC's own
 *	<limits.h> ends in "syslimits.h", which #include_next's the system's --
 *	unconditionally, not gated on __STDC_HOSTED__ the way <stdint.h> is.  So
 *	the one header the freestanding flag does NOT rescue is this one, and it
 *	was reaching the build machine's glibc on both targets.
 *
 *	🔑 Every value below is derived from a macro the COMPILER predefines,
 *	which is the only source that describes the target rather than the
 *	machine doing the build.  Writing the numbers out per architecture would
 *	be a second place to keep in step with -m32/-m64 -- and the i386 copy in
 *	mach_kernel/i386/machlimits.h shows how that ends: it says
 *	ULONG_MAX == UINT_MAX, true for ILP32 and silently false here.
 *
 *	⚠️ Deliberately NOT defined here: PATH_MAX and NAME_MAX.  glibc's
 *	<limits.h> supplies them through <linux/limits.h>, and four places in
 *	this tree define PATH_MAX themselves with three different values -- one
 *	of them (bootstrap) behind #ifndef, so its 512 holds only while nothing
 *	got there first.  Defining it here would decide #509 by accident, from
 *	the file least likely to be read when someone asks what the limit is.
 *
 *	⚠️ Also not here: PTHREAD_STACK_MIN.  glibc puts it behind this header
 *	and the tree uses it exactly zero times; adding it would be inventing a
 *	contract to satisfy a dependency nothing has.
 */

#ifndef _SA_MACH_LIMITS_H_
#define _SA_MACH_LIMITS_H_

#define	CHAR_BIT	__CHAR_BIT__

#define	SCHAR_MAX	__SCHAR_MAX__
#define	SCHAR_MIN	(-SCHAR_MAX - 1)
#define	UCHAR_MAX	(SCHAR_MAX * 2 + 1)

/*
 * Whether a plain `char' is signed is a property of the target, and the
 * compiler is the one that knows: it defines __CHAR_UNSIGNED__ when it is
 * not.  Both of ours are signed, so the second branch is unused today --
 * and it is written anyway, because the day it is needed is the day nobody
 * is looking at this file.
 */
#ifdef	__CHAR_UNSIGNED__
#define	CHAR_MIN	0
#define	CHAR_MAX	UCHAR_MAX
#else
#define	CHAR_MIN	SCHAR_MIN
#define	CHAR_MAX	SCHAR_MAX
#endif

#define	SHRT_MAX	__SHRT_MAX__
#define	SHRT_MIN	(-SHRT_MAX - 1)
#define	USHRT_MAX	(SHRT_MAX * 2 + 1)

#define	INT_MAX		__INT_MAX__
#define	INT_MIN		(-INT_MAX - 1)
#define	UINT_MAX	(INT_MAX * 2U + 1U)

/*
 * The one that differs between the two targets: 32 bits on i386 and 64 on
 * x86-64, said once, by the compiler that was given -m32 or -m64.
 */
#define	LONG_MAX	__LONG_MAX__
#define	LONG_MIN	(-LONG_MAX - 1L)
#define	ULONG_MAX	(LONG_MAX * 2UL + 1UL)

#define	LLONG_MAX	__LONG_LONG_MAX__
#define	LLONG_MIN	(-LLONG_MAX - 1LL)
#define	ULLONG_MAX	(LLONG_MAX * 2ULL + 1ULL)

/* The BSD spellings, which libmach/compat.h maps QUAD_MAX and friends onto. */
#define	LONG_LONG_MAX	LLONG_MAX
#define	LONG_LONG_MIN	LLONG_MIN
#define	ULONG_LONG_MAX	ULLONG_MAX

/* Must be at least two, for internationalization (NLS/KJI). */
#define	MB_LEN_MAX	4

#endif	/* _SA_MACH_LIMITS_H_ */
