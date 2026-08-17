/*
 * Copyright 1991-1998 by Open Software Foundation, Inc. 
 *              All Rights Reserved 
 *  
 * Permission to use, copy, modify, and distribute this software and 
 * its documentation for any purpose and without fee is hereby granted, 
 * provided that the above copyright notice appears in all copies and 
 * that both the copyright notice and this permission notice appear in 
 * supporting documentation. 
 *  
 * OSF DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE 
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS 
 * FOR A PARTICULAR PURPOSE. 
 *  
 * IN NO EVENT SHALL OSF BE LIABLE FOR ANY SPECIAL, INDIRECT, OR 
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM 
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN ACTION OF CONTRACT, 
 * NEGLIGENCE, OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION 
 * WITH THE USE OR PERFORMANCE OF THIS SOFTWARE. 
 */
/*
 * MkLinux
 */

#ifndef _MACH_TYPES_H_
#define _MACH_TYPES_H_
#include "machine/types.h"

/*
 * 🔥 THE LIBC TYPES ARE NOT OURS TO SIZE (#480).
 *
 * These four -- size_t, time_t, off_t, dev_t -- used to be declared here
 * with 1993's widths, behind `#ifndef _SYS_TYPES_H'.  That guard works:
 * a unit that included <sys/types.h> first got musl's, and a unit that did
 * not got ours.  Which is the defect.  Same name, two widths, chosen by
 * include order, and no diagnostic on either path:
 *
 *              sa_mach             musl
 *   time_t     32-bit signed       64-bit signed
 *   off_t      32-bit UNSIGNED     64-bit signed
 *   dev_t      32-bit signed       64-bit unsigned
 *
 * An off_t that is 32 bits and unsigned caps a file at 4 GiB and gets
 * every `< 0' test wrong; a 32-bit time_t ends in 2038.  But the danger is
 * not the old widths -- it is that two translation units can already
 * disagree about how wide the value they pass each other is.
 *
 * So they are declared here only when NOTHING else declares them, and then
 * with musl's widths rather than our own.  __INT64_TYPE__ and friends are
 * the compiler's own names for the canonical types, which is exactly how
 * musl picks (_Int64 is `long long' on i386 and `long' on x86-64) -- so the
 * two agree in spelling as well as in width, and a redeclaration is not
 * merely compatible, it is identical.
 *
 * ⚠️ The guards are musl's `__DEFINED_<type>' as well as the historical
 * `_<TYPE>_DECLARED', and we SET both when we declare.  Honouring only our
 * own convention is why size_t collided: musl never heard of _SIZE_T.
 */
#if !defined(__DEFINED_size_t) && !defined(_SIZE_T)
#define _SIZE_T
#define __DEFINED_size_t
typedef __SIZE_TYPE__	size_t;
#endif

#if !defined(__DEFINED_dev_t) && !defined(_DEV_T_DECLARED)
#define _DEV_T_DECLARED
#define __DEFINED_dev_t
typedef __UINT64_TYPE__	dev_t;		/* device number (major+minor) */
#endif

#if !defined(__DEFINED_time_t) && !defined(_TIME_T_DECLARED)
#define _TIME_T_DECLARED
#define __DEFINED_time_t
typedef __INT64_TYPE__	time_t;
#endif

#if !defined(__DEFINED_off_t) && !defined(_OFF_T_DECLARED)
#define _OFF_T_DECLARED
#define __DEFINED_off_t
typedef __INT64_TYPE__	off_t;
#endif

/*
 * ssize_t was ABSENT here, which is its own way of disagreeing: libvfs
 * declared a local one because sa_mach had none, and a name declared in
 * one place and improvised in another is the same defect as a name
 * declared twice.  musl's is `_Addr' -- signed and pointer-wide -- which
 * is what __INTPTR_TYPE__ names.
 */
#if !defined(__DEFINED_ssize_t) && !defined(_SSIZE_T_DECLARED)
#define _SSIZE_T_DECLARED
#define __DEFINED_ssize_t
typedef __INTPTR_TYPE__	ssize_t;
#endif

/*
 * Common type definitions that lots of old files seem to want.
 *
 * These are ours: musl declares u_char and caddr_t identically and the rest
 * not at all, so there is nothing to disagree with.  daddr_t stays 32-bit
 * unsigned because it is a Mach disk address and no libc has an opinion.
 */
typedef unsigned char u_char;        /* unsigned char */
typedef unsigned short u_short;    /* unsigned short */
typedef unsigned int u_int;        /* unsigned int */

typedef unsigned long u_long;        /* unsigned long */
typedef char * caddr_t;  /* address of a (signed) char */

#ifndef _DADDR_T_DECLARED
#define _DADDR_T_DECLARED
typedef unsigned int    daddr_t;        /* an unsigned 32 */
#endif

/*
 * From the second copy of this file, which used to sit in
 * export/include/sa_mach and is gone (#480).  These three were the only
 * thing it had that this one did not; everything else it declared was
 * this file's types at the OLD widths, so which set a target got came
 * down to whether export/include preceded mach_services/include on its
 * -I line.  Two headers of the same name, and the include path choosing:
 * the same defect #471 removed from the .defs, one directory over.
 */
typedef struct _quad_ {
	unsigned int	val[2];		/* 2 32-bit values make... */
} quad;					/* an 8-byte item */

#define	major(i)	(((i) >> 8) & 0xFF)
#define	minor(i)	((i) & 0xFF)
typedef	unsigned short	ushort_t;
typedef	unsigned int	uint_t;
typedef unsigned long	ulong_t;
typedef	volatile unsigned char	vuchar_t;
typedef	volatile unsigned short	vushort_t;
typedef	volatile unsigned int	vuint_t;
typedef volatile unsigned long	vulong_t;

#endif	/* _MACH_TYPES_H_ */
