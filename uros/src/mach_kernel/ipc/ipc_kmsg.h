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
 * Revision 2.11.2.4  92/05/28  18:14:33  jeffreyh
 * 	Add ikm_has_dest_right boolean.  Put in union with ikm_norma_acked.
 * 	[92/05/28            dlb]
 * 
 * Revision 2.11.2.3  92/05/26  18:53:43  jeffreyh
 * 	Add ikm_norma_acked field for norma message processing.
 * 	[92/05/05            dlb]
 * 
 * Revision 2.11.2.2  92/04/08  15:44:33  jeffreyh
 * 	Temporary debugging logic.
 * 	[92/04/06            dlb]
 * 
 * Revision 2.11.2.1  92/01/03  16:35:19  jsb
 * 	Added ikm_source_node to support norma_ipc_receive_rright.
 * 	[91/12/28  08:38:53  jsb]
 * 
 * Revision 2.11  91/12/14  14:26:54  jsb
 * 	NORMA_IPC: added ikm_copy to struct kmsg.
 * 
 * Revision 2.10  91/08/28  11:13:31  jsb
 * 	Renamed IKM_SIZE_CLPORT to IKM_SIZE_NORMA.
 * 	[91/08/15  08:12:02  jsb]
 * 
 * Revision 2.9  91/08/03  18:18:24  jsb
 * 	NORMA_IPC: added ikm_page field to struct ipc_kmsg.
 * 	[91/07/17  14:01:38  jsb]
 * 
 * Revision 2.8  91/06/17  15:46:15  jsb
 * 	Renamed NORMA conditionals.
 * 	[91/06/17  10:46:12  jsb]
 * 
 * Revision 2.7  91/05/14  16:33:21  mrt
 * 	Correcting copyright
 * 
 * Revision 2.6  91/03/16  14:48:10  rpd
 * 	Replaced ith_saved with ipc_kmsg_cache.
 * 	[91/02/16            rpd]
 * 
 * Revision 2.5  91/02/05  17:22:08  mrt
 * 	Changed to new Mach copyright
 * 	[91/02/01  15:45:52  mrt]
 * 
 * Revision 2.4  91/01/08  15:14:04  rpd
 * 	Added ipc_kmsg_free.  Generalized the notion of special message sizes.
 * 	[91/01/05            rpd]
 * 	Added declarations of ipc_kmsg_copyout_object, ipc_kmsg_copyout_body.
 * 	[90/12/21            rpd]
 * 
 * Revision 2.3  90/09/28  16:54:48  jsb
 * 	Added NORMA_IPC support (hack in ikm_free).
 * 	[90/09/28  14:03:06  jsb]
 * 
 * Revision 2.2  90/06/02  14:50:24  rpd
 * 	Increased IKM_SAVED_KMSG_SIZE from 128 to 256.
 * 	[90/04/23            rpd]
 * 	Created for new IPC.
 * 	[90/03/26  20:56:16  rpd]
 * 
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
/*
 *	File:	ipc/ipc_kmsg.h
 *	Author:	Rich Draves
 *	Date:	1989
 *
 *	Definitions for kernel messages.
 */

#ifndef	_IPC_IPC_KMSG_H_
#define _IPC_IPC_KMSG_H_

#include <cpus.h>
#include <dipc.h>
#include <mach_rt.h>

#include <mach/machine/vm_types.h>
#include <mach/message.h>
#include <kern/assert.h>
#include <kern/cpu_number.h>
#include <kern/macro_help.h>
#include <kern/kalloc.h>
#include <kern/kern_types.h>
#if	MACH_RT
#include <kern/rtalloc.h>
#endif	/* MACH_RT */
#include <ipc/ipc_object.h>
/* Note: vm_map.h NOT included here to break circular dependency.
 * vm_map_t is forward-declared in kern_types.h */
#if	DIPC
#include <dipc/dipc_types.h>		/* for handle_t definition */
#endif	/* DIPC */

/*
 *	This structure is only the header for a kmsg buffer;
 *	the actual buffer is normally larger.  The rest of the buffer
 *	holds the body of the message.
 *
 *	The kmsg's own ikm_dest and ikm_reply hold pointers to ports, and
 *	those pointers hold references.  The message itself carries names,
 *	as it did when it arrived and as it will when it leaves (#442).
 */

#if	DIPC
/*
 *	Refer to dipc/dipc_kmsg.h for a description of the
 *	meta_kmsg, a placeholder used to enqueue sufficient
 *	information about a kmsg to retrieve it from a
 *	remote sender.  Also look in that file for a description
 *	of the changes in use of certain kmsg fields during
 *	the DIPC receiving process.
 *
 *	The ikm_handle links the local kmsg (or meta_kmsg) to
 *	the remote sender's kmsg.  Unfortunately, it just isn't
 *	possible to overload one of the kmsg fields with the
 *	handle.  We can't overload the next and prev pointers
 *	or the ikm_size field because this information is all
 *	needed while the message is enqueued.  The size in the
 *	message header could be stolen but not easily:  then it
 *	is difficult to have a queued, inline kmsg that still
 *	has remote components.
 */
#endif	/* DIPC */

/*
 *	#442: the destination and reply ports, as pointers, here rather than
 *	inside the message.
 *
 *	A port NAME is thirty-two bits by definition of the interface; a port
 *	POINTER is a kernel address, four bytes on i386 and eight on x86-64.
 *	Mach writes the pointer over the name in msgh_remote_port because on
 *	a 32-bit machine the two are the same size.  That is free there and
 *	not free here, and it puts kernel pointers inside a buffer whose
 *	contents arrived from userland.
 *
 *	These are filled by copyin, which translates the sender's names, and
 *	by the kernel's own producers, which have the ports to begin with.
 *	The message keeps the names it arrived with until copyout replaces
 *	them with the receiver's, and never holds an address at all.
 *
 *	Getting here was a migration, and it was checked rather than assumed:
 *	a boot flag made every send, every clean and every kernel-side send
 *	compare the two copies while both existed, per call site so that a
 *	zero came with the number of times it could have been otherwise.
 *	The last run before the message stopped carrying them read
 *	2999279 / 174 / 1 / 546 seen, none diverging.
 */
typedef struct ipc_kmsg {
	struct ipc_kmsg *ikm_next, *ikm_prev;
	vm_size_t ikm_size;
	vm_offset_t ikm_private;		/* allocator-private info */
#if	DIPC
	handle_t ikm_handle;
#endif	/* DIPC */
	ipc_port_t ikm_dest;			/* where it is going */
	ipc_port_t ikm_reply;			/* where the answer goes */
	ipc_port_t *ikm_ports;			/* one per descriptor (#442) */
	mach_msg_type_number_t ikm_nports;	/* how many ikm_ports has */
	mach_msg_header_t ikm_header;
} *ipc_kmsg_t;

/*
 *	Set a kmsg's destination or reply (#442).
 *
 *	Every path that builds a message inside a kmsg and sends it goes
 *	through these rather than writing a header field, so that the port
 *	the kernel will actually use is never the one nobody remembered.
 *
 *	They used to write the header as well, so that readers not yet moved
 *	kept seeing what they expected.  There are none left, and a port
 *	POINTER no longer goes anywhere near a field whose width the interface
 *	fixes at thirty-two bits.
 */
/*
 *	The descriptors' ports, out of the message too (#442).
 *
 *	Same reason as ikm_dest and ikm_reply: a port descriptor's `name'
 *	field is a NAME, thirty-two bits by definition of the interface,
 *	and the kernel was writing an address over it.  One entry per
 *	descriptor rather than per port descriptor, so the index is the
 *	descriptor's own position and there is no second numbering for the
 *	generated stubs to agree with; entries for out-of-line descriptors
 *	stay IP_NULL.  (Out-of-line port ARRAYS were already outside the
 *	message, in their own allocation.)
 *
 *	Allocated only for complex messages, which the port census measured
 *	at 1.1% of traffic carrying a mean of 1.45 ports.
 */
extern kern_return_t	ikm_ports_alloc(ipc_kmsg_t kmsg,
					mach_msg_type_number_t count);
extern void		ikm_ports_free(ipc_kmsg_t kmsg);
extern void		ikm_ports_report(void);

#define	ikm_set_dest(kmsg, port)					\
	MACRO_BEGIN							\
	    (kmsg)->ikm_dest = (port);					\
	MACRO_END

#define	ikm_set_reply(kmsg, port)				\
	MACRO_BEGIN							\
	    (kmsg)->ikm_reply = (port);					\
	MACRO_END
#define	IKM_NULL		((ipc_kmsg_t) 0)

#define	IKM_OVERHEAD							\
		(sizeof(struct ipc_kmsg) - sizeof(mach_msg_header_t))

/*
 *	The message a header belongs to (#442).
 *
 *	ikm_header is the last member, so a header the kernel is working on
 *	sits at a fixed offset inside its kmsg.  This is for the MIG server
 *	stubs: a server routine is handed a bare mach_msg_header_t because
 *	that is the signature migcom generates, and it needs the destination
 *	port, which is no longer in the message.
 *
 *	Only valid for a header that IS inside a kmsg.  For a server routine
 *	it always is -- ipc_kobject_server dispatches with &request->ikm_header
 *	and nothing else calls one.
 */
#define	ikm_from_header(hdr)						\
	((ipc_kmsg_t) ((vm_offset_t) (hdr) - IKM_OVERHEAD))

#define	ikm_plus_overhead(size)	((vm_size_t)((size) + IKM_OVERHEAD))
#define	ikm_less_overhead(size)	((mach_msg_size_t)((size) - IKM_OVERHEAD))

/*
 * XXX	For debugging.
 */
#define IKM_BOGUS		((ipc_kmsg_t) 0xffffff10)

/*
 *	The size of the kernel message buffers that will be cached.
 *	IKM_SAVED_KMSG_SIZE includes overhead; IKM_SAVED_MSG_SIZE doesn't.
 */

#define	IKM_SAVED_KMSG_SIZE	((vm_size_t) 256)
#define	IKM_SAVED_MSG_SIZE	ikm_less_overhead(IKM_SAVED_KMSG_SIZE)

#define	ikm_alloc(size)							\
	((ipc_kmsg_t) kalloc(ikm_plus_overhead(size)))

#if	MACH_RT

#define	ikm_rtalloc(size)						\
	((ipc_kmsg_t) rtalloc(ikm_plus_overhead(size)))

#define	KMSG_IS_RT(kmsg)						\
	((kmsg)->ikm_header.msgh_bits & MACH_MSGH_BITS_RTALLOC)
#define	KMSG_MARK_RT(kmsg)						\
	((kmsg)->ikm_header.msgh_bits |= MACH_MSGH_BITS_RTALLOC)

#else	/* MACH_RT */

/*
 *	It's legal to ask whether a kmsg is RT-related even
 *	in a non-RT kernel configuration.  The answer is always no.
 *	But note that it's NOT legal to try to do KMSG_MARK_RT
 *	in a non-RT kernel configuration.
 */
#define	KMSG_IS_RT(kmsg)	0

#endif	/* MACH_RT */

#define	ikm_init(kmsg, size)						\
MACRO_BEGIN								\
	ikm_init_special((kmsg), ikm_plus_overhead(size));		\
MACRO_END

/*
 *	#442: the two typed ports start empty.
 *
 *	A kmsg comes off ikm_cache holding whatever the last message left
 *	in it, and a producer that fills only one of the two leaves a stale
 *	POINTER in the other.  While the header still carries the same
 *	values that is invisible -- the producer overwrites both a
 *	moment later -- but the -M census saw it on 15 of 546 kernel sends
 *	(the other 531 got a recycled kmsg whose leftover happened to be
 *	zero), and when the header stops carrying them there is nothing to
 *	overwrite it with.  Emptying them here costs two stores on a buffer
 *	the allocator has just touched, and makes "nobody set it" mean
 *	IP_NULL rather than the previous message.
 */
#if	DIPC
/*
 *	The msgh_bits must be initialized to zero so that
 *	the MACH_MSGH_BITS_META_KMSG flag is initialized
 *	to FALSE.  This might be worth making the default
 *	for the non-DIPC case, too.
 */
#define	ikm_init_special(kmsg, size)					\
MACRO_BEGIN								\
	(kmsg)->ikm_size = (size);					\
	(kmsg)->ikm_dest = IP_NULL;					\
	(kmsg)->ikm_reply = IP_NULL;					\
	(kmsg)->ikm_ports = (ipc_port_t *) 0;				\
	(kmsg)->ikm_nports = 0;						\
	(kmsg)->ikm_header.msgh_bits = 0;				\
MACRO_END
#else	/* !DIPC */
#define	ikm_init_special(kmsg, size)					\
MACRO_BEGIN								\
	(kmsg)->ikm_size = (size);					\
	(kmsg)->ikm_dest = IP_NULL;					\
	(kmsg)->ikm_reply = IP_NULL;					\
	(kmsg)->ikm_ports = (ipc_port_t *) 0;				\
	(kmsg)->ikm_nports = 0;						\
MACRO_END
#endif	/* DIPC */

#define	ikm_check_initialized(kmsg, size)				\
MACRO_BEGIN								\
	assert((kmsg)->ikm_size == (size));				\
MACRO_END

/*
 *	Non-positive message sizes are special.  They indicate that
 *	the message buffer doesn't come from ikm_alloc and
 *	requires some special handling to free.
 *
 *	ipc_kmsg_free is the non-macro form of ikm_free.
 *	It frees kmsgs of all varieties.
 */

#define	IKM_SIZE_NETWORK	-1
#define	IKM_SIZE_INTR_KMSG	-2

#if	MACH_RT

#define	ikm_free(kmsg)							\
MACRO_BEGIN								\
	register vm_size_t _size = (kmsg)->ikm_size;			\
									\
	ikm_ports_free(kmsg);		/* #442 */			\
	if ((integer_t)_size > 0)					\
		if (KMSG_IS_RT(kmsg))					\
			rtfree((vm_offset_t) (kmsg), _size);		\
		else					    		\
			kfree((vm_offset_t) (kmsg), _size);		\
	else								\
		ipc_kmsg_free(kmsg);					\
MACRO_END

#else	/* MACH_RT */

#define	ikm_free(kmsg)							\
MACRO_BEGIN								\
	register vm_size_t _size = (kmsg)->ikm_size;			\
									\
	ikm_ports_free(kmsg);		/* #442 */			\
	if ((integer_t)_size > 0)					\
		kfree((vm_offset_t) (kmsg), _size);			\
	else								\
		ipc_kmsg_free(kmsg);					\
MACRO_END

#endif	/* MACH_RT */


struct ipc_kmsg_queue {
	struct ipc_kmsg *ikmq_base;
};

typedef struct ipc_kmsg_queue *ipc_kmsg_queue_t;

#define	IKMQ_NULL		((ipc_kmsg_queue_t) 0)


/*
 * Exported interfaces
 */

#define	ipc_kmsg_queue_init(queue)		\
MACRO_BEGIN					\
	(queue)->ikmq_base = IKM_NULL;		\
MACRO_END

#define	ipc_kmsg_queue_empty(queue)	((queue)->ikmq_base == IKM_NULL)

/* Enqueue a kmsg */
extern void ipc_kmsg_enqueue(
	ipc_kmsg_queue_t	queue,
	ipc_kmsg_t		kmsg);

/* Dequeue and return a kmsg */
extern ipc_kmsg_t ipc_kmsg_dequeue(
	ipc_kmsg_queue_t        queue);

/* Pull a kmsg out of a queue */
extern void ipc_kmsg_rmqueue(
	ipc_kmsg_queue_t	queue,
	ipc_kmsg_t		kmsg);

#define	ipc_kmsg_queue_first(queue)		((queue)->ikmq_base)

/* Return the kmsg following the given kmsg */
extern ipc_kmsg_t ipc_kmsg_queue_next(
	ipc_kmsg_queue_t	queue,
	ipc_kmsg_t		kmsg);

#define	ipc_kmsg_rmqueue_first_macro(queue, kmsg)			\
MACRO_BEGIN								\
	register ipc_kmsg_t _next;					\
									\
	assert((queue)->ikmq_base == (kmsg));				\
									\
	_next = (kmsg)->ikm_next;					\
	if (_next == (kmsg)) {						\
		assert((kmsg)->ikm_prev == (kmsg));			\
		(queue)->ikmq_base = IKM_NULL;				\
	} else {							\
		register ipc_kmsg_t _prev = (kmsg)->ikm_prev;		\
									\
		(queue)->ikmq_base = _next;				\
		_next->ikm_prev = _prev;				\
		_prev->ikm_next = _next;				\
	}								\
  	/* XXX Debug paranoia */					\
  	kmsg->ikm_next = IKM_BOGUS;					\
  	kmsg->ikm_prev = IKM_BOGUS;					\
MACRO_END

#define	ipc_kmsg_enqueue_macro(queue, kmsg)				\
MACRO_BEGIN								\
	register ipc_kmsg_t _first = (queue)->ikmq_base;		\
									\
	if (_first == IKM_NULL) {					\
		(queue)->ikmq_base = (kmsg);				\
		(kmsg)->ikm_next = (kmsg);				\
		(kmsg)->ikm_prev = (kmsg);				\
	} else {							\
		register ipc_kmsg_t _last = _first->ikm_prev;		\
									\
		(kmsg)->ikm_next = _first;				\
		(kmsg)->ikm_prev = _last;				\
		_first->ikm_prev = (kmsg);				\
		_last->ikm_next = (kmsg);				\
	}								\
MACRO_END

/* scatter list macros */

#define SKIP_PORT_DESCRIPTORS(s, e)					\
MACRO_BEGIN								\
	if ((s) != MACH_MSG_DESCRIPTOR_NULL) {				\
		while ((s) < (e)) {					\
			if ((s)->type.type != MACH_MSG_PORT_DESCRIPTOR)	\
				break;					\
			(s)++;						\
		}							\
		if ((s) >= (e))						\
			(s) = MACH_MSG_DESCRIPTOR_NULL;			\
	}								\
MACRO_END

#define INCREMENT_SCATTER(s)						\
MACRO_BEGIN								\
	if ((s) != MACH_MSG_DESCRIPTOR_NULL) {				\
		(s)++;							\
	}								\
MACRO_END

/* Destroy kernel message */
extern void ipc_kmsg_destroy(
	ipc_kmsg_t	kmsg);

/* Free a kernel message buffer */
extern void ipc_kmsg_free(
	ipc_kmsg_t	kmsg);

/* Allocate a kernel message buffer and copy a user message to the buffer */
extern mach_msg_return_t ipc_kmsg_get(
	mach_msg_header_t	*msg,
	mach_msg_size_t		size,
	ipc_kmsg_t		*kmsgp,
	ipc_space_t		space);

/* Allocate a kernel message buffer and copy a kernel message to the buffer */
extern mach_msg_return_t ipc_kmsg_get_from_kernel(
	ipc_port_t		dest,
	mach_msg_header_t	*msg,
	mach_msg_size_t		size,
	ipc_kmsg_t		*kmsgp);

/* Copy a kernel message buffer to a user message */
extern mach_msg_return_t ipc_kmsg_put(
	mach_msg_header_t	*msg,
	ipc_kmsg_t		kmsg,
	mach_msg_size_t		size);

/* Copy a kernel message buffer to a kernel message */
extern void ipc_kmsg_put_to_kernel(
	mach_msg_header_t	*msg,
	ipc_kmsg_t		kmsg,
	mach_msg_size_t		size);

/* Copyin port rights in the header of a message */
extern mach_msg_return_t ipc_kmsg_copyin_header(
	ipc_kmsg_t		kmsg,
	ipc_space_t		space,
	mach_port_t		notify);

/* Copyin port rights and out-of-line memory from a user message */
extern mach_msg_return_t ipc_kmsg_copyin(
	ipc_kmsg_t	kmsg,
	ipc_space_t	space,
	vm_map_t	map,
	mach_port_t	notify);

/* Copyin port rights and out-of-line memory from a kernel message */
extern void ipc_kmsg_copyin_from_kernel(
	ipc_kmsg_t	kmsg);

/* Copyout port rights in the header of a message */
extern mach_msg_return_t ipc_kmsg_copyout_header(
	ipc_kmsg_t		kmsg,
	ipc_space_t		space,
	mach_port_t		notify);

/* Copyout a port right returning a name */
extern mach_msg_return_t ipc_kmsg_copyout_object(
	ipc_space_t		space,
	ipc_object_t		object,
	mach_msg_type_name_t	msgt_name,
	mach_port_t		*namep);

/* Copyout the header and body to a user message */
extern mach_msg_return_t ipc_kmsg_copyout(
	ipc_kmsg_t		kmsg,
	ipc_space_t		space,
	vm_map_t		map,
	mach_port_t		notify,
	mach_msg_body_t		*slist);

/* Copyout port rights and out-of-line memory from the body of a message */
extern mach_msg_return_t ipc_kmsg_copyout_body(
    	ipc_kmsg_t		kmsg,
	ipc_space_t		space,
	vm_map_t		map,
	mach_msg_body_t		*slist);

/* Copyout port rights and out-of-line memory to a user message,
   not reversing the ports in the header */
extern mach_msg_return_t ipc_kmsg_copyout_pseudo(
	ipc_kmsg_t		kmsg,
	ipc_space_t		space,
	vm_map_t		map,
	mach_msg_body_t		*slist);

/* Copyout the destination port in the message */
extern void ipc_kmsg_copyout_dest( 
	ipc_kmsg_t	kmsg,
	ipc_space_t	space);

/* kernel's version of ipc_kmsg_copyout_dest */
extern void ipc_kmsg_copyout_to_kernel(
	ipc_kmsg_t		kmsg,
	ipc_space_t		space);

/* Check scatter and gather lists for consistency */
extern mach_msg_return_t ipc_kmsg_check_scatter(
	ipc_kmsg_t		kmsg,
	mach_msg_option_t	option,
	mach_msg_body_t		**slistp,
	mach_msg_size_t		*sizep);

extern boolean_t	ikm_cache_get(
	ipc_kmsg_t		*kmsg);
extern boolean_t	ikm_cache_put(
	ipc_kmsg_t		kmsg);

#include <mach_kdb.h>
#if 	MACH_KDB

/* Do a formatted dump of a kernel message */
extern void ipc_kmsg_print(
	ipc_kmsg_t	kmsg);

/* Do a formatted dump of a user message */
extern void ipc_msg_print(
	mach_msg_header_t	*msgh);

#endif	/* MACH_KDB */

#endif	/* _IPC_IPC_KMSG_H_ */
