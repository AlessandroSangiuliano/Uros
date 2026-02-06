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

#ifndef	_KERN_KERN_TYPES_H_
#define	_KERN_KERN_TYPES_H_

#include <mach/port.h>

/*
 * Forward declarations for kernel types.
 * These allow headers to reference types without including their full definitions,
 * breaking circular include dependencies.
 */

/* Thread and task types */
typedef struct thread_shuttle		*thread_t;
typedef	mach_port_t			*thread_array_t;
typedef	mach_port_t			*thread_port_array_t;
typedef mach_port_t			thread_act_port_t;
typedef	thread_act_port_t		*thread_act_port_array_t;
typedef struct task			*task_t;
typedef struct processor		*processor_t;
typedef struct processor_set		*processor_set_t;
typedef struct thread_activation	*thread_act_t;
typedef struct subsystem		*subsystem_t;

/* VM types - forward declarations to break circular dependencies */
typedef struct vm_map			*vm_map_t;
typedef struct vm_object		*vm_object_t;

/* IPC types */
typedef struct ipc_space		*ipc_space_t;
typedef struct ipc_port			*ipc_port_t;
typedef struct ipc_pset			*ipc_pset_t;

/* Wait event type */
typedef void				*event_t;

#endif	/* _KERN_KERN_TYPES_H_ */
