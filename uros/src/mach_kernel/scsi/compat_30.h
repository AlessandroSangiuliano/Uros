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
 * Revision 2.6  91/10/09  16:16:40  af
 * 	 Revision 2.5.1.1  91/08/29  18:10:28  af
 * 	 	The 2.5 raw-I/O support code has moved, and became MI.
 * 
 * Revision 2.5.1.1  91/08/29  18:10:28  af
 * 	The 2.5 raw-I/O support code has moved, and became MI.
 * 
 * Revision 2.5  91/08/24  12:27:32  af
 * 	Corrected defines.
 * 	[91/08/02  03:50:58  af]
 * 
 * Revision 2.4  91/06/19  11:56:38  rvb
 * 	File moved here from mips/PMAX since it is now "MI" code, also
 * 	used by Vax3100 and soon -- the omron luna88k.
 * 	[91/06/04            rvb]
 * 
 * Revision 2.3  91/05/14  17:20:23  mrt
 * 	Correcting copyright
 * 
 * Revision 2.2  91/05/13  06:03:39  af
 * 	Created.
 * 	[91/05/12  16:02:24  af]
 * 
 */
/* CMU_ENDHIST */
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
 *	File: compat_30.h
 * 	Author: Alessandro Forin, Carnegie Mellon University
 *	Date:	4/91
 *
 *	Compatibility defs to retrofit Mach 3.0 drivers
 *	into Mach 2.6.  #741: the Mach 3.0 half is what is left, the
 *	one i386's bios_label.c reads.
 */

#ifndef	_SCSI_COMPAT_30_
#define	_SCSI_COMPAT_30_

#include <kern/assert.h>

/*
 * Mach 3.0 compiles with these definitions
 */

#include <device/param.h>
#include <device/io_req.h>
#include <device/device_types.h>
#include <device/disk_status.h>
#include <ddb/db_output.h>

/*
 * Scratch temporary in io_req structure (for error handling)
 */
#define	io_temporary	io_error

#endif	/*_SCSI_COMPAT_30_*/
