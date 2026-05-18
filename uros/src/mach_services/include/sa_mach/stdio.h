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
 * 
 */
/*
 * MkLinux
 */

#ifndef _MACH_STDIO_H_
#define _MACH_STDIO_H_

#include <stdio.h>
#include <stdarg.h>
#include <sa_mach/types.h>

#ifndef NULL
#define NULL ((void *) 0)
#endif

extern int	sprintf(char *, const char *, ...);
extern int	snprintf(char *, size_t, const char *, ...);
extern int	printf(const char *, ...);
#ifndef MACH_OVERRIDE_VPRINTF
#define MACH_OVERRIDE_VPRINTF
/* Override only if not already declared by system stdio */
#if !defined(__GNUC__) || !defined(__USE_BSD)
int vprintf(const char *, va_list);
int vsprintf(char *, const char *, va_list);
int vsnprintf(char *, size_t, const char *, va_list);
#endif
#endif

extern int 	getchar(void);

extern int	fprintf_stderr(const char *, ...);

#endif /* _MACH_STDIO_H_ */
