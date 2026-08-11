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
 * Revision 2.5.2.1  92/03/03  16:22:09  jeffreyh
 * 	Changes from TRUNK
 * 	[92/02/26  12:09:51  jeffreyh]
 * 
 * Revision 2.6  92/01/15  13:44:25  rpd
 * 	Changed MACH_IPC_COMPAT conditionals to default to not present.
 * 
 * Revision 2.5  91/05/14  16:54:55  mrt
 * 	Correcting copyright
 * 
 * Revision 2.4  91/02/05  17:33:35  mrt
 * 	Changed to new Mach copyright
 * 	[91/02/01  17:18:21  mrt]
 * 
 * Revision 2.3  90/08/06  17:06:07  rpd
 * 	Removed mach_host_priv_self.
 * 	Removed definition of _MACH_INIT_.
 * 	[90/08/04            rpd]
 * 
 * Revision 2.2  90/06/02  14:58:30  rpd
 * 	Converted to new IPC.
 * 	[90/03/26  22:33:07  rpd]
 * 
 * Revision 2.1  89/08/03  15:59:29  rwd
 * Created.
 * 
 * Revision 2.4  89/02/25  18:37:15  gm0w
 * 	Changes for cleanup.
 * 
 * Revision 2.3  89/02/19  12:57:44  rpd
 * 	Moved from kern/ to mach/.
 * 
 * Revision 2.2  89/01/15  16:24:46  rpd
 * 	Updated includes for the new mach/ directory.
 * 	[89/01/15  15:03:03  rpd]
 * 
 * 18-Jan-88  David Golub (dbg) at Carnegie-Mellon University
 *	Add thread_reply.  Leave in task_data as an alternate name -
 *	they are functionally indistinguishable.
 *
 * 15-Oct-86  Avadis Tevanian (avie) at Carnegie-Mellon University
 *	Include ../kern/mach_types.h instead of <kern/mach_types.h> when
 *	building for the kernel.
 *
 *  1-Sep-86  Michael Young (mwyoung) at Carnegie-Mellon University
 *	Created, mostly to help build the lint library.
 *	Should eventually include this in "syscall_sw.c".
 *
 */
/* CMU_ENDHIST */
/* 
 * Mach Operating System
 * Copyright (c) 1991,1990,1989,1988,1987 Carnegie Mellon University
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
/*
 *	Definitions of general Mach system traps.
 *
 *	IPC traps are defined in <mach/message.h>.
 *	Kernel RPC functions are defined in <mach/mach_interface.h>.
 */

#ifndef	_MACH_MACH_TRAPS_H_
#define _MACH_MACH_TRAPS_H_

#include <mach/port.h>
#include <mach/boolean.h>
#include <mach/kern_return.h>
#include <mach/cap_types.h>

mach_port_t	mach_reply_port(void);

mach_port_t	mach_thread_self(void);

mach_port_t	(mach_task_self)(void);

mach_port_t	mach_host_self(void);

/*
 *	Print a string on the kernel's own console.
 *
 *	⚠️ It had no prototype anywhere, and the trap has existed since the
 *	beginning: <mach/syscall_sw.h> emits `kernel_trap(mach_print,-14,1)',
 *	which is an ASSEMBLY stub, and assembly carries no types.  So every C
 *	caller declared it implicitly and the two halves were compared by
 *	nothing -- the shape of #448, with the other half in another language.
 *	Harmless while the argument is a pointer and the ABI passes it in a
 *	register on both targets; not harmless as a habit.
 *
 *	It is the one printing path that needs no port, no device and no
 *	initialisation, which is what makes it the right instrument for
 *	reporting a failure in the paths that set those up.
 */
void		mach_print(const char *);

/*
 *	The rest of the traps C actually calls (#426).
 *
 *	<mach/syscall_sw.h> emits a stub for 117 traps and 41 of them had no
 *	prototype anywhere.  Most of those 41 are dead -- the old msg_*_trap
 *	IPC, the RDMA and RPC experiments, the three obsolete self traps whose
 *	table slots hold kern_invalid -- and are not declared here, because a
 *	declaration for something no caller has and no kernel answers would be
 *	an invitation.  ⚠️ Which ones are LIVE was not judged by reading: the
 *	names below are the intersection of the 41 with the symbols that are
 *	actually UNDEFINED in the objects of a full build, so each one is a
 *	call the linker really has to resolve against the assembly stub.
 *
 *	⚠️ The types come from the kernel's own definitions and not from the
 *	call sites, and the point of putting them here is that kern/syscall_sw.c
 *	-- the file holding mach_trap_table -- includes this header.  So the two
 *	halves are in one translation unit at last, and a disagreement is a
 *	build error on both targets instead of a truncation on one.
 */
kern_return_t	mach_null(void);
kern_return_t	mach_thread_set_name(const char *);
boolean_t	swtch(void);
boolean_t	swtch_pri(int);

/*
 *	The capability fast path (#216).  These are the ones that could have
 *	cost something: a token is a POINTER, and until now libcap declared
 *	them for itself while nothing compared that declaration with the
 *	kernel's.  const, because none of the three writes through the pointer
 *	-- they copyin and work on the copy.
 */
kern_return_t	urmach_cap_verify(const struct uros_cap *, uint32_t, uint64_t);
kern_return_t	urmach_cap_use(const struct uros_cap *, uint32_t, uint64_t);
kern_return_t	urmach_cap_register(const struct uros_cap *);
kern_return_t	urmach_cap_revoke(uint64_t);

#endif	/* _MACH_MACH_TRAPS_H_ */
