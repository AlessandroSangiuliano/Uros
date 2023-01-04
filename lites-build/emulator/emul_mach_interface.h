#ifndef	_mach_user_
#define	_mach_user_

/* Module mach */

#include <mach/kern_return.h>
#include <mach/port.h>
#include <mach/message.h>

#include <mach/std_types.h>
#include <mach/mach_types.h>

/* Routine emul_thread_suspend */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t emul_thread_suspend
#if	defined(LINTLIBRARY)
    (target_thread, reply)
	mach_port_t target_thread;
	mach_port_t reply;
{ return emul_thread_suspend(target_thread, reply); }
#else
(
	mach_port_t target_thread,
	mach_port_t reply
);
#endif

#endif	/* not defined(_mach_user_) */
