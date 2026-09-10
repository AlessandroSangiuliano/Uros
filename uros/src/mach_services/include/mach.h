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
 * MkLinux
 */
/* 
 *  Includes all the types that a normal user
 *  of Mach programs should need
 */

#ifndef	_MACH_H_
#define	_MACH_H_

#include <mach/mach_types.h>
#include <mach/mach_interface.h>
#include <mach/mach_port.h>
#include <mach_init.h>
#include <device/device_types.h>

/*
 * Standard prototypes
 */
extern void			panic_init(mach_port_t);
extern void			panic(const char *, ...);

extern void			safe_gets(char *,
					  char *,
					  int);

extern void			slot_name(cpu_type_t,
					  cpu_subtype_t,
					  char **,
					  char **);

extern void			mig_reply_setup(mach_msg_header_t *,
						mach_msg_header_t *);

extern void			mach_msg_destroy(mach_msg_header_t *);

extern mach_msg_return_t	mach_msg_receive(mach_msg_header_t *);

extern mach_msg_return_t	mach_msg_send(mach_msg_header_t *);

/*
 * ── The allocator's single-threaded fast path (#542) ──────────────────────
 *
 * malloc and free skip their lock while this task has never held a second
 * thread, which is worth 96 cycles a pair against 47 and covers all but four
 * programs in the system.  Anybody who creates a thread IN THIS TASK must say
 * so BEFORE creating it -- afterwards is too late, because a thread can be
 * made running and could allocate before the creator returns.
 *
 * 🔴 IT CANNOT LIVE IN THE THREAD LIBRARY, however obvious that looks:
 * libposix-uros' clone, kernel242_test and tgdb all call
 * thread_create(mach_task_self(), ...) themselves, and a flag only libpthreads
 * maintained would tell those tasks they were alone while they ran two threads
 * -- which is #540 again, reopened where nobody would think to look.
 *
 * ⚠️ Forgetting the call is not left to memory: scripts/thread-create-notify-check.py
 * fails on any thread_create(mach_task_self(), ...) that does not have it.
 *
 * 🔴 WEAK, BECAUSE NOT EVERY PROGRAM HAS THIS ALLOCATOR, and the linker said
 * so before any argument did.  libmach_core.a carries no malloc at all -- what
 * links it gets musl's -- and on i386 default_pager ships its own kalloc.c and
 * defines malloc and free itself.  A hard reference dragged libmach's malloc.o
 * into those links and collided.  So the call compiles to nothing where this
 * allocator is absent, which is also the truth: there is no flag there to set.
 */
extern void			_malloc_note_thread_created(void)
					__attribute__((weak));
extern int			_malloc_is_multithreaded(void);

static inline void
mach_note_thread_created(void)
{
	if (_malloc_note_thread_created != 0)
		_malloc_note_thread_created();
}

extern mach_msg_return_t	mach_msg_server_once(boolean_t (*)
						     (mach_msg_header_t *,
						      mach_msg_header_t *),
						     mach_msg_size_t,
						     mach_port_t,
						     mach_msg_options_t);
extern mach_msg_return_t	mach_msg_server(boolean_t (*)
						(mach_msg_header_t *,
						 mach_msg_header_t *),
						mach_msg_size_t,
						mach_port_t,
						mach_msg_options_t);

extern kern_return_t		device_read_overwrite_request(mach_port_t,
							      mach_port_t,
							      dev_mode_t,
							      recnum_t,
							      io_buf_len_t,
							      vm_address_t);

extern kern_return_t		device_read_overwrite(mach_port_t,
						      dev_mode_t,
						      recnum_t,
						      io_buf_len_t,
						      vm_address_t,
						      mach_msg_type_number_t *);

extern kern_return_t		vm_read_overwrite(mach_port_t,
						  vm_address_t,
						  vm_size_t,
						  vm_address_t,
						  vm_size_t *);

extern void			*sbrk(int);

extern int			 brk(void *);

/*
 * Prototypes for compatibility
 */
extern kern_return_t	clock_get_res(mach_port_t,
				      clock_res_t *);
extern kern_return_t	clock_set_res(mach_port_t,
				      clock_res_t);

extern kern_return_t	clock_sleep(mach_port_t,
				    int,
				    tvalspec_t,
				    tvalspec_t *);
#endif	/* _MACH_H_ */
