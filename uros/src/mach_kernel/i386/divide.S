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
/* CMU_HIST */
/*
 * Revision 2.4  91/05/14  16:07:50  mrt
 * 	Correcting copyright
 * 
 * Revision 2.3  91/02/05  17:11:48  mrt
 * 	Changed to new Mach copyright
 * 	[91/02/01  17:34:04  mrt]
 * 
 * Revision 2.2  90/05/03  15:27:21  dbg
 * 	First checkin.
 * 
 *	Replacement for gnulib calls made from kernel.
 *	[89/12/21  17:58:40  rvb]
 *
 * 20-Dec-89  Robert Baron (rvb) at Carnegie-Mellon University
 *	Created.
 */
/* CMU_ENDHIST */
/* 
 * Mach Operating System
 * Copyright (c) 1991,1990,1989 Carnegie Mellon University
 * All Rights Reserved.
 * 
 * Permission to use, copy, modify and distribute this software and its
 * documentation is hereby granted, provided that both the copyright
 * notice and this permission notice appear in all copies of the
 * software, derivative works or modified versions, and any portions
 * thereof, and that both notices appear in supporting documentation.
 * 
 * CARNEGIE MELLON ALLOWS FREE USE OF THIS SOFTWARE IN ITS "AS IS"
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
 * any improvements or extensions that they make and grant Carnegie Mellon
 * the rights to redistribute these changes.
 */
/*
 */

#include <i386/asm.h>

/*
 * ── What this file is, now that its name says so (#415) ──────────────
 *
 * It was called gcc.S, which named the COMPILER rather than the contents.
 * These are the runtime helpers a compiler emits calls to when a machine has
 * no instruction for an operation -- and this kernel links -nostdlib, so it
 * does not get them from the compiler's own runtime library and has to
 * supply them itself.  divide64.c beside it is the same job for operands
 * twice this wide, which is what made the naming worth fixing: two files
 * doing one thing cannot both be called after the compiler.
 *
 * ⚠️ Nothing calls these two.  `nm' on the linked kernel finds __divsi3 and
 * __udivsi3 defined and referenced nowhere: dividing a 32-bit value on i386
 * is one instruction, so no compiler of this century emits a call for it.
 * They are kept because deleting inherited code that still builds is #433's
 * question and not this issue's -- but the fact is recorded here rather than
 * left for the next reader to measure again.
 */

ENTRY(__divsi3)
	movl	4(%esp), %eax
	cdq
	idivl	8(%esp), %eax
	ret

ENTRY(__udivsi3)
	movl	4(%esp), %eax
	xorl	%edx, %edx
	divl	8(%esp), %eax
	ret

