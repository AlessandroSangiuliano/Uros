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

#ifndef	_IPC_TYPES_H_
#define	_IPC_TYPES_H_

typedef struct ipc_space	*ipc_space_t;
typedef struct ipc_port		*ipc_port_t;

/*
 *	The two sizes of an array of ports (#415).
 *
 *	The kernel holds such an array as one POINTER per port; userland is
 *	handed one NAME per port.  On i386 those are the same four bytes,
 *	which is why the whole tree wrote sizeof(mach_port_t) for both and was
 *	right for thirty years.  They are not the same on x86-64, and the
 *	place that hurts is where one number is used for both: a buffer
 *	allocated at one width and freed at the other corrupts the allocator,
 *	and one filled at pointer width after being sized at name width runs
 *	off its end by as much again -- with the count coming from the caller.
 *
 *	Named so that the allocation, the free and the copyout say which of
 *	the two they mean, and so a reader can see when they disagree.
 */
#define	ipc_port_array_size(count)					\
		((vm_size_t) (count) * sizeof(ipc_port_t))
#define	mach_port_name_array_size(count)				\
		((vm_size_t) (count) * sizeof(mach_port_t))

#endif	/* _IPC_TYPES_H_ */
