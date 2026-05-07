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

#ifndef _SA_MACH_H_
#define	_SA_MACH_H_

#include <mach.h>
#include <stddef.h>

/*
 * Prototypes defined in libsa_mach.a
 */

#if defined(__cplusplus)
extern "C" {
#endif

extern int  isascii(int);
extern int  toascii(int);
extern void printf_init(mach_port_t);
extern void printf_set_mirror(void (*hook)(const char *buf, size_t len));
extern void _exit(int);
extern void sleep(int);
extern void usleep(int);

#if defined(__cplusplus)
}
#endif

#endif /* _SA_MACH_H_ */
