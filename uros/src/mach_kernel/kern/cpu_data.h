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
 * 
 */
/*
 * MkLinux
 */

#ifndef	_CPU_DATA_H_
#define	_CPU_DATA_H_

#include <mach_rt.h>

/*
 * #329 phase 5: pad each CPU's slot to a full cache line (aligned(64) rounds
 * sizeof up to 64) so adjacent CPUs never share one.  Without this, the hot
 * per-CPU fields below -- written on every context switch (active_thread),
 * interrupt (interrupt_level) and simple_lock -- packed 4 CPUs into a single
 * 64-byte line on a 4-way box, bouncing it across all cores.  The padding is
 * tail-only, so the field offsets stay stable for the %gs-relative asm
 * (genassym CPU_DATA_*) and the offsetof(cpu_data_t, cpu_id) _Static_assert.
 */
typedef struct
{
	struct thread_shuttle	*active_thread;
#if	MACH_RT
	int		preemption_level;
#endif	/* MACH_RT */
	int		simple_lock_count;
	int		interrupt_level;
	/*
	 * #301: per-CPU data via %gs.  cpu_id is appended to keep
	 * pre-existing offsets (active_thread, preemption_level, …)
	 * stable for callers that read them through %gs-relative inline
	 * asm.  Zero for the BSP (matches BSS), set to the AP's slot
	 * by mp_desc_init() right after it bzero's the slot.
	 */
	int		cpu_id;
	/*
	 * #331 step 2 (QSBR RCU): per-CPU quiescent-state bookkeeping for the
	 * lock-free capability-table reads.  Appended AFTER cpu_id so the
	 * %gs-relative field offsets (active_thread/.../cpu_id) stay fixed;
	 * these two are read in C via cpu_data[cpu_number()], never through
	 * %gs, so they need no genassym entry.  rcu_read_depth is CPU-local
	 * (nesting of active read sections) -- valid because MACH_RT==0 makes
	 * the kernel non-preemptive, so a reader never migrates CPU
	 * mid-section.  rcu_qs_seq is bumped at each quiescent state and is the
	 * only field read cross-CPU (by urmach_synchronize_rcu).  See
	 * <kern/rcu.h>.
	 */
	int			rcu_read_depth;
	volatile unsigned int	rcu_qs_seq;
	/*
	 * #331 step 2: set while this CPU is in the idle wait loop.  An idle
	 * CPU holds no RCU read reference, so it is quiescent by definition;
	 * urmach_synchronize_rcu counts it immediately rather than waiting for
	 * it to tick (a HLTed idle CPU may not report a quiescent state for a
	 * long time).  Read cross-CPU; written only by the owning CPU.
	 */
	volatile int		rcu_cpu_idle;
} __attribute__((aligned(64))) cpu_data_t;

_Static_assert(sizeof(cpu_data_t) == 64,
	       "cpu_data_t must own a whole cache line (#329 phase 5)");

extern cpu_data_t	cpu_data[NCPUS];

/*
 * NOTE: For non-realtime configurations, the accessor functions for
 * cpu_data fields are here, coded in C. For MACH_RT configurations,
 * the accessor functions must be defined in the machine-specific
 * cpu_data.h header file.
 */

#if	MACH_RT

#include <machine/cpu_data.h>

#else	/* MACH_RT */

#include <kern/cpu_number.h>

#if	defined(__GNUC__)

#ifndef __OPTIMIZE__
#define extern static
#endif

static struct thread_shuttle __inline__ *current_thread(void);
static int __inline__		current_cpu_id(void);
static int __inline__		get_preemption_level(void);
static int __inline__		get_simple_lock_count(void);
static int __inline__		get_interrupt_level(void);

static void __inline__		disable_preemption(void);
static void __inline__		enable_preemption(void);
static void __inline__		enable_preemption_no_check(void);

static void __inline__		mp_disable_preemption(void);
static void __inline__		mp_enable_preemption(void);
static void __inline__		mp_enable_preemption_no_check(void);

static struct thread_shuttle __inline__ *current_thread(void)
{
	return (cpu_data[cpu_number()].active_thread);
}

/*
 * #301: cpu_id accessor for builds without MACH_RT.  Falls back to
 * indexing through cpu_number().  Real per-%gs version lives in
 * machine/cpu_data.h and gets used when MACH_RT is on.
 */
static int __inline__	current_cpu_id(void)
{
	return (cpu_data[cpu_number()].cpu_id);
}

static int __inline__	get_preemption_level(void)
{
	return (0);
}

static int __inline__	get_simple_lock_count(void)
{
	return (cpu_data[cpu_number()].simple_lock_count);
}

static int __inline__	get_interrupt_level(void)
{
	return (cpu_data[cpu_number()].interrupt_level);
}

static void __inline__	disable_preemption(void)
{
}

static void __inline__	enable_preemption(void)
{
}

static void __inline__	enable_preemption_no_check(void)
{
}

static void __inline__	mp_disable_preemption(void)
{
}

static void __inline__	mp_enable_preemption(void)
{
}

static void __inline__	mp_enable_preemption_no_check(void)
{
}

#ifndef	__OPTIMIZE__
#undef 	extern
#endif

#else	/* !defined(__GNUC__) */

#define current_thread()	(cpu_data[cpu_number()].active_thread)
#define current_cpu_id()	(cpu_data[cpu_number()].cpu_id)
#define get_preemption_level()	(0)
#define get_simple_lock_count()	(cpu_data[cpu_number()].simple_lock_count)
#define get_interrupt_level()	(cpu_data[cpu_number()].interrupt_level)
#define disable_preemption()	
#define enable_preemption()
#define enable_preemption_no_check()
#define mp_disable_preemption()	
#define mp_enable_preemption()
#define mp_enable_preemption_no_check()

#endif	/* defined(__GNUC__) */

#endif	/* MACH_RT */

#endif	/* _CPU_DATA_H_ */
