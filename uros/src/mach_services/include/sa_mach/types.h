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

#ifndef	_SIZE_T
#define _SIZE_T
typedef unsigned long	size_t;
#endif	/* _SIZE_T */

/*
 * Common type definitions that lots of old files seem to want.
 */

typedef unsigned char u_char;        /* unsigned char */
typedef unsigned short u_short;    /* unsigned short */
typedef unsigned int u_int;        /* unsigned int */

typedef unsigned long u_long;        /* unsigned long */
typedef char * caddr_t;  /* address of a (signed) char */
/*
 * Avoid type conflicts with system headers (e.g. sys/types.h from glibc)
 * Only define these if not already defined by libc.
 */
#ifndef _SYS_TYPES_H
#ifndef _DEV_T_DECLARED
typedef long            dev_t;          /* device number (major+minor) */
#define _DEV_T_DECLARED
#endif
#ifndef _TIME_T_DECLARED
typedef int             time_t;         /* a signed 32    */
#define _TIME_T_DECLARED
#endif
#ifndef _DADDR_T_DECLARED
typedef unsigned int    daddr_t;        /* an unsigned 32 */
#define _DADDR_T_DECLARED
#endif
#ifndef _OFF_T_DECLARED
typedef unsigned int    off_t;          /* another unsigned 32 */
#define _OFF_T_DECLARED
#endif
#endif /* _SYS_TYPES_H */
typedef	unsigned short	ushort_t;
typedef	unsigned int	uint_t;
typedef unsigned long	ulong_t;
typedef	volatile unsigned char	vuchar_t;
typedef	volatile unsigned short	vushort_t;
typedef	volatile unsigned int	vuint_t;
typedef volatile unsigned long	vulong_t;

#endif	/* _MACH_TYPES_H_ */
