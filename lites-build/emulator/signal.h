#ifndef	_signal_user_
#define	_signal_user_

/* Module signal */

#include <mach/kern_return.h>
#include <mach/port.h>
#include <mach/message.h>

#include <mach/std_types.h>
#include <mach/mach_types.h>
#include <serv/bsd_types.h>
#include <mach_debug/mach_debug_types.h>

/* SimpleRoutine signal_notify */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t signal_notify
#if	defined(LINTLIBRARY)
    (sigport, thread)
	mach_port_t sigport;
	mach_port_t thread;
{ return signal_notify(sigport, thread); }
#else
(
	mach_port_t sigport,
	mach_port_t thread
);
#endif

#endif	/* not defined(_signal_user_) */
