#ifndef	_signal_server_
#define	_signal_server_

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
kern_return_t do_signal_notify
#if	defined(LINTLIBRARY)
    (sigport, thread)
	mach_port_t sigport;
	mach_port_t thread;
{ return do_signal_notify(sigport, thread); }
#else
(
	mach_port_t sigport,
	mach_port_t thread
);
#endif

#endif	/* not defined(_signal_server_) */
