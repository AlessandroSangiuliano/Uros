/*
 * Copyright (c) 1995, 1994, 1993, 1992, 1991, 1990  
 * Open Software Foundation, Inc. 
 *  
 * Permission to use, copy, modify, and distribute this software and 
 * its documentation for any purpose and without fee is hereby granted, 
 * provided that the above copyright notice appears in all copies and 
 * that both the copyright notice and this permission notice appear in 
 * supporting documentation, and that the name of ("OSF") or Open Software 
 * Foundation not be used in advertising or publicity pertaining to 
 * distribution of the software without specific, written prior permission. 
 *  
 * OSF DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE 
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS 
 * FOR A PARTICULAR PURPOSE. IN NO EVENT SHALL OSF BE LIABLE FOR ANY 
 * SPECIAL, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES 
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN 
 * ACTION OF CONTRACT, NEGLIGENCE, OR OTHER TORTIOUS ACTION, ARISING 
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE 
 */
/*
 * OSF Research Institute MK6.1 (unencumbered) 1/31/1995
 */
/* 
 * Mach Operating System
 * Copyright (c) 1991,1990 Carnegie Mellon University
 * All Rights Reserved.
 * 
 * Permission to use, copy, modify and distribute this software and its
 * documentation is hereby granted, provided that both the copyright
 * notice and this permission notice appear in all copies of the
 * software, derivative works or modified versions, and any portions
 * thereof, and that both notices appear in supporting documentation.
 * 
 * CARNEGIE MELLON ALLOWS FREE USE OF THIS SOFTWARE IN ITS 
 * CONDITION.  CARNEGIE MELLON DISCLAIMS ANY LIABILITY OF ANY KIND FOR
 * ANY DAMAGES WHATSOEVER RESULTING FROM THE USE OF THIS SOFTWARE.
 * 
 * Carnegie Mellon requests users of this software to return to
 * 
 *  Software Distribution Coordinator  or  Software.Distribution@CS.CMU.EDU
 *  School of Computer Science
 *  Carnegie Mellon University
 *  Pittsburgh PA 15213-3890
 * 
 * any improvements or extensions that they make and grant Carnegie the
 * rights to redistribute these changes.
 */
/*
 * 91/02/05  17:55:52  mrt
 * 	Changed to new Mach copyright
 * 	[91/02/01  17:55:57  mrt]
 * 
 * 90/06/02  15:05:46  rpd
 * 	Created for new IPC.
 * 	[90/03/26  21:13:46  rpd]
 * 
 * 07-Apr-89  Richard Draves (rpd) at Carnegie-Mellon University
 *	Extensive revamping.  Added polymorphic arguments.
 *	Allow multiple variable-sized inline arguments in messages.
 *
 * 15-Jun-87  David Black (dlb) at Carnegie-Mellon University
 *	Declare and initialize charNULL here for strNull def in string.h
 *
 * 27-May-87  Richard Draves (rpd) at Carnegie-Mellon University
 *	Created.
 */

#include <mach/boolean.h>
#include <ctype.h>
#include "error.h"
#include "alloc.h"
#include "strdefs.h"
#include <stdio.h>

string_t
strmake(char *string)
{
    register string_t saved;

    saved = malloc(strlen(string) + 1);
    if (saved == strNULL)
	fatal("strmake('%s'): %s", string, strerror(errno));
    return strcpy(saved, string);
}

string_t
strconcat(string_t left, string_t right)
{
    register string_t saved;

    saved = malloc(strlen(left) + strlen(right) + 1);
    if (saved == strNULL)
	fatal("strconcat('%s', '%s'): %s",
	      left, right, strerror(errno));
    strcat(strcpy(saved, left), right);
    /* Debug: scan saved for non-ASCII to catch corrupt concatenations */
    {
        size_t __len = strlen(saved);
        size_t __i;
        for (__i = 0; __i < __len; ++__i) {
            unsigned char __c = (unsigned char)saved[__i];
            if ((__c < 32 && __c != 9 && __c != 10 && __c != 13) || __c > 126) {
                fprintf(stderr, "[DEBUG-strconcat] non-ASCII 0x%02x at pos %zu\n", __c, __i);
                break;
            }
        }
    }
    return saved;
}

void
strfree(string_t string)
{
    free(string);
}

char *
strbool(boolean_t b)
{
    if (b)
	return "TRUE";
    else
	return "FALSE";
}

char *
strstring(string_t string)
{
    if (string == strNULL)
	return "NULL";
    else
	return string;
}

char *
toupperstr(char *p)
{
    register char *s = p;
    char c;

    while ((c = *s)) {
        if (islower(c))
	    *s = toupper(c);
        s++;
    }
    return(p);
}
