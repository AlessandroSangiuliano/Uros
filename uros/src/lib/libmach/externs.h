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

#include <mach/boolean.h>
#include <mach/error.h>
#include <mach/message.h>
#include <mach/vm_types.h>

#include <stdarg.h>		/* for va_list */
#include <stddef.h>		/* for size_t */

extern void mig_init(void *);
extern void mach_init_ports(void);
extern void mig_allocate(vm_address_t *, vm_size_t);
extern void mig_deallocate(vm_address_t, vm_size_t);

extern char *mach_error_string_int(mach_error_t, boolean_t *);

/* libc-style globals/helpers imported from libsa_mach (#106). */
extern char	**__environment;

/*
 * The one formatter (#432 audit).  The `radix' parameter is gone with the
 * `%r' conversion that was its only user, and both call sites passed 0.
 */
extern void	_doprnt(const char *,
			va_list,
			int,			/* the base %r prints in */
			void (*)(void *, int),	/* character output */
			void *);

extern size_t	strlcpy(char *, const char *, size_t);
extern size_t	strlcat(char *, const char *, size_t);
extern int	snprintf(char *, size_t, const char *, ...);
extern int	vsnprintf(char *, size_t, const char *, va_list);
