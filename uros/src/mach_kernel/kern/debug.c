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
 * Revision 2.18.2.2  92/03/28  10:09:54  jeffreyh
 * 	Pick up changes from MK71
 * 	[92/03/20  13:15:42  jeffreyh]
 * 
 * Revision 2.19  92/02/19  11:19:26  elf
 * 	Added cpu number print, locking to print in Assert for
 * 	 multiprocessor case.
 * 	[92/01/12            danner]
 * 
 * Revision 2.18.2.1  92/01/21  21:50:38  jsb
 * 	Added panic_on_assertion_failure flag, set true by default.
 * 	[92/01/16  21:31:13  jsb]
 * 
 * Revision 2.18  91/12/10  16:32:45  jsb
 * 	Fixes from Intel
 * 	[91/12/10  15:51:54  jsb]
 * 
 * Revision 2.17  91/08/03  18:18:47  jsb
 * 	Replaced obsolete NORMA tag with NORMA_IPC.
 * 	[91/07/27  18:13:01  jsb]
 * 
 * Revision 2.16  91/07/31  17:44:28  dbg
 * 	Minor SUN changes.
 * 	[91/07/12            dbg]
 * 
 * Revision 2.15  91/07/09  23:16:17  danner
 * 	Luna88k support.
 * 	[91/06/26            danner]
 * 
 * Revision 2.14  91/06/17  15:46:57  jsb
 * 	Renamed NORMA conditionals.
 * 	[91/06/17  10:49:18  jsb]
 * 
 * Revision 2.13  91/05/14  16:40:46  mrt
 * 	Correcting copyright
 * 
 * Revision 2.12  91/03/16  14:49:39  rpd
 * 	In panic, only call halt_cpu when not calling Debugger.
 * 	[91/03/12            rpd]
 * 
 * Revision 2.11  91/02/05  17:25:52  mrt
 * 	Changed to new Mach copyright
 * 	[91/02/01  16:11:43  mrt]
 * 
 * Revision 2.10  90/12/14  11:02:02  jsb
 * 	NORMA_IPC support: print node number as well as cpu in panic.
 * 	[90/12/13  21:40:37  jsb]
 * 
 * Revision 2.9  90/12/04  14:51:00  jsb
 * 	Added i860 support for Debugger function.
 * 	[90/12/04  11:01:25  jsb]
 * 
 * Revision 2.8  90/11/05  14:30:49  rpd
 * 	Added Assert.
 * 	[90/11/04            rpd]
 * 
 * Revision 2.7  90/10/25  14:45:10  rwd
 * 	Change sun3 debugger invocation.
 * 	[90/10/17            rwd]
 * 
 * Revision 2.6  90/09/09  23:20:09  rpd
 * 	Fixed panic and log to supply cnputc to _doprnt.
 * 	[90/09/09            rpd]
 * 
 * Revision 2.5  90/09/09  14:32:03  rpd
 * 	Use decl_simple_lock_data.
 * 	[90/08/30            rpd]
 * 
 * Revision 2.4  90/08/27  22:02:14  dbg
 * 	Pass extra argument to _doprnt.
 * 	[90/08/21            dbg]
 * 
 * Revision 2.3  90/05/03  15:46:53  dbg
 * 	Added i386 case.
 * 	[90/02/21            dbg]
 * 
 * Revision 2.2  89/11/29  14:09:04  af
 * 	Added mips case, RCS-ed.
 * 	[89/10/28            af]
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

#include <mach_assert.h>
#include <mach_kdb.h>
#include <dipc.h>
#include <mach_kgdb.h>
#include <cpus.h>

#include <kern/cpu_number.h>
#include <kern/lock.h>
#include <kern/spl.h>
#include <kern/thread.h>
#include <kern/assert.h>
#include <kern/misc_protos.h>
#include <stdarg.h>

#ifdef PPC
#include <consfeed.h>
extern void console_feed_cancel_and_flush(void);
#endif /* def PPC */

int panic_on_assertion_failure = 1;

#if	MACH_KDB
#include <ddb/db_command.h>	/* for db_error() prototype */
#endif	/* MACH_KDB */

#if	MACH_ASSERT

int mach_assert = 1;

#if	NCPUS > 1
extern int kdb_active[NCPUS];
#endif	/* NCPUS > 1 */

/* Break into debugger.
 * If debugger is not present, system will panic.
 */

void
Assert(
	const char	*file,
	int		line,
	const char	*expression)
{
	if (! mach_assert) {
		return;
	}
#ifdef PPC
#if NCONSFEED
	console_feed_cancel_and_flush();
#endif /* NCONSFEED */
#endif /* PPC */

#if NCPUS > 1
	mp_disable_preemption();
	printf("{%d} Assertion failed: file \"%s\", line %d: %s\n", 
	       cpu_number(), file, line, expression);
	mp_enable_preemption();
#else
	printf("Assertion failed: file \"%s\", line %d: %s\n", file, line,
		expression);
#endif
	if (panic_on_assertion_failure) {
#if	MACH_KDB
		mp_disable_preemption();
		if (!db_active
#if	NCPUS > 1
		    && !kdb_active[cpu_number()]
#endif	/* NCPUS > 1 */
		    ) {
			mp_enable_preemption();
			Debugger("assertion failure");
		} else {
			mp_enable_preemption();
			db_error("assertion failure");
		}
#else	/* MACH_KDB */
		Debugger("assertion failure");
#endif	/* MACH_KDB */
	}
}

#endif	/* MACH_ASSERT */

const char		*panicstr;
decl_simple_lock_data(,panic_lock)
int			paniccpu;
volatile int		panicwait;
unsigned int		panic_is_inited = 0;

/*
 *	Carefully use the panic_lock.  There's always a chance that
 *	somehow we'll call panic before getting to initialize the
 *	panic_lock -- in this case, we'll assume that the world is
 *	in uniprocessor mode and just avoid using the panic lock.
 */
#define	PANIC_LOCK()							\
MACRO_BEGIN								\
	if (panic_is_inited)						\
		simple_lock(&panic_lock);				\
MACRO_END

#define	PANIC_UNLOCK()							\
MACRO_BEGIN								\
	if (panic_is_inited)						\
		simple_unlock(&panic_lock);				\
MACRO_END


void
panic_init(void)
{
	simple_lock_init(&panic_lock, ETAP_NO_TRACE);
	panic_is_inited = 1;
}

#if	DIPC
#include <mach/kkt_request.h>
#endif	/* DIPC */

void
panic(const char *str, ...)
{
	va_list	listp;
	spl_t	s = splhigh();	/* do not let other CPUs interrupt */

	mp_disable_preemption();

#ifdef PPC
#if NCONSFEED
	console_feed_cancel_and_flush();
#endif /* NCONSFEED */
#endif /* PPC */

	for (;;) {
	PANIC_LOCK();
	if (panicstr) {
	    if (cpu_number() != paniccpu) {
		PANIC_UNLOCK();
#if	(MACH_KDB || MACH_KGDB)
		/*
		 * Wait until message has been printed to identify correct
		 * cpu that made the first panic.
		 */
		while (panicwait)
		    continue;
		Debugger("panic");
		continue;	/* restart loop */
	    } else {
		Debugger("Double panic");
		PANIC_UNLOCK();
		splx(s);
		mp_enable_preemption();
		return;
#else	/* MACH_KDB || MACH_KGDB */
		mp_enable_preemption();
		halt_cpu();
		/* NOTREACHED */
#endif	/* MACH_KDB || MACH_KGDB */
	    }
	}
	break;	/* fall through to set panicstr */
	}
	panicstr = str;
	paniccpu = cpu_number();
	panicwait = 1;

	PANIC_UNLOCK();
	printf("panic");
#if	DIPC
	printf("(node %d)", KKT_NODE_SELF());
#endif
#if	NCPUS > 1
	printf("(cpu %d)", (unsigned) paniccpu);
#endif
	printf(": ");
	va_start(listp, str);
	_doprnt(str, &listp, cnputc, 0);
	va_end(listp);
	printf("\n");

	/*
	 * Release panicwait indicator so that other cpus may call Debugger().
	 */
	panicwait = 0;
#if	(MACH_KDB || MACH_KGDB)
	Debugger("panic");

	/*
	 * Release panicstr so that we can handle normally other panics.
	 */
	PANIC_LOCK();
	panicstr = (char *)0;
	PANIC_UNLOCK();
	splx(s);
#elif	MACH_DEBUGGER
	/*
	 * ⚠️ The guard above asks whether the 1990 debugger is COMPILED IN,
	 * and means "is there a debugger to enter" (#428).  Those were the
	 * same question until a target had one of its own: x86-64 builds with
	 * MACH_KDB off -- the machine-independent ddb/ tree is not in its
	 * build -- and has a debugger regardless, so a panic went straight to
	 * halt_cpu() with a prompt sitting there unused.  A panic is the
	 * moment that prompt is worth most.
	 *
	 * ⚠️ HALTS FIRST when there is nothing to enter, and the order is the
	 * whole of it.  The first version of this arm asked for the debugger
	 * and then fell into the KDB path's tail -- release panicstr, splx,
	 * return -- which is right there because an operator at the prompt
	 * asked to continue, and is a catastrophe without one: panic() stopped
	 * halting.  The kernel printed "No bootstrap code loaded" and then
	 * carried on into the boot script, three boot entries deep, before the
	 * harness caught it.
	 *
	 * Asked at RUNTIME and not only at compile time, because the answer
	 * depends on a boot flag: without -r there is nothing to enter, and
	 * calling Debugger() then would panic from inside panic -- which
	 * recurses through the double-panic arm until the stack is gone.
	 */
	if (!debugger_available())
		halt_cpu();
		/* NOTREACHED */

	Debugger("panic");

	/*
	 * Release panicstr so that we can handle normally other panics.
	 */
	PANIC_LOCK();
	panicstr = (char *)0;
	PANIC_UNLOCK();
	splx(s);
#else	/* MACH_KDB || MACH_KGDB */
	halt_cpu();
	/* NOTREACHED */
#endif	/* MACH_KDB || MACH_KGDB */
	mp_enable_preemption();
}

/*
 * We'd like to use BSD's log routines here...
 */

void
log(int level, char *fmt, ...)
{
	va_list	listp;

#ifdef lint
	level++;
#endif /* lint */
	va_start(listp, fmt);
	_doprnt(fmt, &listp, cnputc, 0);
	va_end(listp);
}

#if	DEBUG
/*
 * The tracing hook thirteen call sites wanted and nobody ever wrote (#415).
 *
 * It was declared twice, in kern/clock.c as `void (thread_t, char *)' and in
 * kern/sched_prim.c as `void (char *, long, long, long)', and defined in
 * neither -- so the two declarations disagreed about their own argument list
 * and no compiler ever compared them, because C compares nothing across
 * translation units and the linker was never asked: every call is inside
 * `#if DEBUG', and a Debug build of this tree did not link.
 *
 * That is the whole reason it is here.  A configuration nobody can build is a
 * configuration nobody checks, and behind this one had accumulated
 * twenty-six format conversions printing sixty-four-bit values as thirty-two.
 * Making it link is what lets the compiler see that code at all.
 *
 * The four-argument form is the one the live callers use -- device/
 * ds_routines.c's log_io_map(), which passes a message and three longs.
 * clock.c's is inside `#if 0' and its declaration went with the call.
 *
 * ⚠️ It prints rather than records.  A ring buffer would be the useful
 * version and is not this issue's to design; what this issue owes the hook is
 * an existence, so that its callers are compiled and checked.
 */
void
log_thread_action(char *msg, long a, long b, long c)
{
	printf("thread %p: %s (%#lx, %#lx, %#lx)\n",
	       (void *) current_thread(), msg,
	       (unsigned long) a, (unsigned long) b, (unsigned long) c);
}
#endif	/* DEBUG */
