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

/*
 * POSIX Threads - IEEE 1003.1c
 */

#ifndef _POSIX_PTHREAD_INTERNALS_H
#define _POSIX_PTHREAD_INTERNALS_H

#include <mach/port.h>
#include <mach/message.h>
#include <mach/machine/vm_types.h>
#include <mach/std_types.h>
#include <mach/policy.h>
#include <mach/sync.h>
#include <mach/sync_policy.h>
#include <mach/mach_traps.h>
#include <mach/thread_switch.h>
#include <mach/mach_host.h>
#include <mach.h>			/* For generic MACH support */
#include <mach/urmach_futex.h>		/* #324 futex block/wake primitive */
/*
 * ⚠️ For _pthread_timeout_ms() below, and named here rather than left to the
 * includers (#425).  It compiled without them only because every file that
 * includes this one happens to include <sys/timers.h> and <errno.h> first --
 * a header whose correctness depends on its users' include order is a contract
 * nothing checks, and the first file to include this one alone would find out.
 */
#include <sys/timers.h>			/* struct timespec, getclock, TIMEOFDAY */
#include <errno.h>			/* ETIMEDOUT */
#include "posix_sched.h"		/* For POSIX scheduling policy & parameter */
#include "pthread_machdep.h"		/* Machine-dependent definitions. */
#include <signal.h>			/* For sigset_t, signal constants */

/*
 * #324 — block/wake on a 32-bit word via the kernel futex instead of a
 * per-object Mach semaphore.  All libpthreads sync objects are intra-task,
 * so we pass URMACH_FUTEX_PRIVATE (key on (address space, virtual addr),
 * no VM-object lookup).  These back the mutex / cond / rwlock / barrier
 * slow paths; the uncontended fast paths stay pure userspace atomics.
 */
static __inline__ kern_return_t
_pthread_futex_wait(volatile int *w, int val, unsigned int timeout_ms)
{
	return urmach_futex((unsigned int *)w,
			    URMACH_FUTEX_WAIT | URMACH_FUTEX_PRIVATE,
			    (unsigned int)val, timeout_ms, (unsigned int *)0);
}
static __inline__ void
_pthread_futex_wake_one(volatile int *w)
{
	(void) urmach_futex((unsigned int *)w,
			    URMACH_FUTEX_WAKE | URMACH_FUTEX_PRIVATE,
			    1, 0, (unsigned int *)0);
}
static __inline__ void
_pthread_futex_wake_all(volatile int *w)
{
	(void) urmach_futex((unsigned int *)w,
			    URMACH_FUTEX_WAKE | URMACH_FUTEX_PRIVATE,
			    0x7fffffff, 0, (unsigned int *)0);
}

/*
 * The thread-lifecycle (joiners/death/pool wake) and sigwait sync used
 * per-thread Mach semaphores.  Post-#324 those mach_port_t fields hold no
 * port — reuse them in place as 32-bit futex words.  PTH_FW() casts a
 * field to the (volatile int *) the helpers above expect.
 */
#define PTH_FW(field)	((volatile int *)&(field))

/*
 * Compiled-in limits
 */

#define PTHREAD_DESTRUCTOR_ITERATIONS 4
#define _POSIX_THREAD_KEYS_MAX	      16

/*
 * Threads
 */
typedef struct _pthread
{
	long	       sig;	      /* Unique signature for this structure */
	pthread_lock_t lock;	      /* Used for internal mutex on structure */
	int	       detached;
	int	       inherit;
	int	       policy;
	struct sched_param param;
	struct _pthread_mutex *mutexes;
	mach_port_t    joiners;	      /* pthread_join() uses this to wait for death's call */
	int	       num_joiners;
	void	       *exit_value;
	mach_port_t    death;	      /* pthread_exit() uses this to wait for the hearse */
	mach_port_t    kernel_thread; /* kernel thread this thread is bound to */
	void	       *(*fun)(void*);/* Argment for child routine */
	void	       *arg;	      /* Argment for child routine */
	int	       cancel_state;  /* Whether thread can be cancelled */
	struct _pthread_handler_rec *cleanup_stack;
	int		err_no;		/* thread-local errno */
	char	       name[16];      /* Thread name (NUL-terminated, 15 usable) */
	unsigned long  sigmask;	      /* Blocked signal set */
	unsigned long  sigpending;    /* Pending signal set */
	mach_port_t    sig_sem;	      /* Semaphore for sigwait() wakeup */
	int	       pool_wake;      /* #352: per-thread futex seq for pool park/wake.
					 * MUST be per-thread, not per-pool-slot: a slot
					 * address is reused by the next parker before the
					 * previous (already pool_get'd) thread is woken, so a
					 * slot-keyed futex piles two threads on one key and
					 * thread_wakeup_one() strands one of them. */
	/*
	 * #444: where a pooled thread resumes when it is reused.
	 *
	 * _pthread_pool_park used to CALL _pthread_pool_trampoline on
	 * waking, from inside the frames of the pthread_exit that parked
	 * it -- so every reuse nested another trampoline + user function +
	 * exit + park onto the same stack and nothing unwound it.  A
	 * thousand create/join cycles walked the stack into its guard page.
	 *
	 * Returning from the park is not enough: pthread_exit can be called
	 * by the user at any depth, so the thread has to be put back at the
	 * depth it started from, not one frame up -- see
	 * PTHREAD_RESTART_ON_STACK in the machine-dependent header.
	 */
	void	       *tsd[_POSIX_THREAD_KEYS_MAX];  /* Thread specific data */
} *pthread_t;

/*
 * Thread attributes
 */
typedef struct 
{
	long	       sig;	      /* Unique signature for this structure */
	pthread_lock_t lock;	      /* Used for internal mutex on structure */
	int	       detached;
	int	       inherit;
	int	       policy;
	struct sched_param param;
	vm_size_t      stacksize;     /* User-requested stack size (0 = default) */
	vm_address_t   stackaddr;     /* User-supplied stack base (0 = auto) */
	vm_size_t      guardsize;     /* Guard page size (0 = default) */
} pthread_attr_t;

/*
 * Mutex attributes
 */
typedef struct
{
	long sig;		     /* Unique signature for this structure */
	int prioceiling;
	int protocol;
	int type;		     /* PTHREAD_MUTEX_NORMAL, etc. */
	int robust;		     /* PTHREAD_MUTEX_STALLED or _ROBUST */
} pthread_mutexattr_t;

/*
 * Mutex variables
 */
typedef struct _pthread_mutex
{
	long	       sig;	      /* Unique signature for this structure */
	pthread_lock_t lock;	      /* Used for internal mutex on structure */
	int	       prioceiling;
	int	       priority;      /* Priority to restore when mutex unlocked */
	int	       protocol;
	pthread_t      owner;	      /* Which thread has this mutex locked */
	struct _pthread_mutex *next, *prev;  /* List of other mutexes he owns */
	struct _pthread_cond *busy;   /* List of condition variables using this mutex */
	int	       waiters;	      /* Count of threads waiting for this mutex */
	mach_port_t    sem;	      /* Semaphore used for waiting */
	int	       state;	      /* Fast-path: 0=unlocked, 1=locked, 2=contended */
	int	       type;	      /* PTHREAD_MUTEX_NORMAL, etc. */
	int	       lock_count;    /* Recursion depth for RECURSIVE type */
	int	       robust;	      /* PTHREAD_MUTEX_STALLED or _ROBUST */
	int	       owner_dead;    /* Owner died while holding lock */
	int	       unrecoverable; /* consistent() not called before unlock */
} pthread_mutex_t;

/*
 * Condition variable attributes
 */
typedef struct 
{
	long	       sig;	     /* Unique signature for this structure */
	int	       pshared;
	int	       clock;	     /* CLOCK_REALTIME or CLOCK_MONOTONIC */
} pthread_condattr_t;

/*
 * Condition variables
 */
typedef struct _pthread_cond
{
	long	       sig;	     /* Unique signature for this structure */
	pthread_lock_t lock;	     /* Used for internal mutex on structure */
	mach_port_t    sem;	     /* Kernel semaphore */
	struct _pthread_cond *next, *prev;  /* List of condition variables using mutex */
	struct _pthread_mutex *busy; /* mutex associated with variable */
	int	       waiters;	     /* Number of threads waiting */
	int	       clock;	     /* CLOCK_REALTIME or CLOCK_MONOTONIC */
} pthread_cond_t;

/*
 * Initialization control (once) variables
 */
typedef struct
{
	long	       sig;	      /* Unique signature for this structure */
	pthread_lock_t lock;	      /* Used for internal mutex on structure */
} pthread_once_t;

/*
 * Read-write lock attributes
 */
typedef struct
{
	long	       sig;
	int	       pshared;
} pthread_rwlockattr_t;

/*
 * Read-write lock variables
 */
typedef struct _pthread_rwlock
{
	long	       sig;	      /* Unique signature for this structure */
	pthread_lock_t lock;	      /* Internal spinlock */
	int	       readers;	      /* Number of active readers */
	int	       writer;	      /* 1 if a writer holds the lock */
	int	       blocked_writers; /* Writers waiting */
	mach_port_t    reader_sem;    /* Semaphore for blocked readers */
	mach_port_t    writer_sem;    /* Semaphore for blocked writers */
} pthread_rwlock_t;

/*
 * Barrier attributes
 */
typedef struct
{
	long	       sig;
	int	       pshared;
} pthread_barrierattr_t;

/*
 * Barrier variables
 */
typedef struct _pthread_barrier
{
	long	       sig;	      /* Unique signature for this structure */
	pthread_lock_t lock;	      /* Internal spinlock */
	int	       count;	      /* Number of threads required */
	int	       waiting;	      /* Threads currently blocked */
	int	       phase;	      /* Toggles to prevent early reuse */
	mach_port_t    sem;	      /* Semaphore for blocked threads */
} pthread_barrier_t;

/*
 * Spinlock variables
 */
typedef struct
{
	long	       sig;
	pthread_lock_t spinlock;
} pthread_spinlock_t;

#include "pthread.h"

#define _PTHREAD_DEFAULT_INHERITSCHED	PTHREAD_INHERIT_SCHED
#define _PTHREAD_DEFAULT_PROTOCOL	PTHREAD_PRIO_NONE
#define _PTHREAD_DEFAULT_PRIOCEILING	0
/*
 * #273: the default policy must match the policy threads actually run
 * under.  The kernel thread template uses POLICY_TIMESHARE and the
 * default processor set only enables timesharing, so SCHED_OTHER is the
 * truthful default; the old SCHED_FIFO never took effect (FIFO is not
 * enabled in the pset) and left the cached policy diverging from reality.
 */
#define _PTHREAD_DEFAULT_POLICY		SCHED_OTHER
#define _PTHREAD_DEFAULT_STACKSIZE	0x10000	  /* 64K */

#define _PTHREAD_NO_SIG			0x00000000
#define _PTHREAD_MUTEX_ATTR_SIG		0x4D545841  /* 'MTXA' */
#define _PTHREAD_MUTEX_SIG		0x4D555458  /* 'MUTX' */
#define _PTHREAD_MUTEX_SIG_init		0x32AAABA7  /* [almost] ~'MUTX' */
#define _PTHREAD_COND_ATTR_SIG		0x434E4441  /* 'CNDA' */
#define _PTHREAD_COND_SIG		0x434F4E44  /* 'COND' */
#define _PTHREAD_COND_SIG_init		0x3CB0B1BB  /* [almost] ~'COND' */
#define _PTHREAD_ATTR_SIG		0x54484441  /* 'THDA' */
#define _PTHREAD_ONCE_SIG		0x4F4E4345  /* 'ONCE' */
#define _PTHREAD_ONCE_SIG_init		0x30B1BCBA  /* [almost] ~'ONCE' */
#define _PTHREAD_SIG			0x54485244  /* 'THRD' */
#define _PTHREAD_RWLOCK_ATTR_SIG	0x52574C41  /* 'RWLA' */
#define _PTHREAD_RWLOCK_SIG		0x52574C4B  /* 'RWLK' */
#define _PTHREAD_BARRIER_ATTR_SIG	0x42415241  /* 'BARA' */
#define _PTHREAD_BARRIER_SIG		0x42415252  /* 'BARR' */
#define _PTHREAD_SPIN_SIG		0x5350494E  /* 'SPIN' */

#define _PTHREAD_EXITED		     3
#define _PTHREAD_CREATE_PARENT	     4

#define _PTHREAD_CANCEL_STATE_MASK   0xFE
#define _PTHREAD_CANCEL_TYPE_MASK    0xFD
#define _PTHREAD_CANCEL_PENDING	     0x10  /* pthread_cancel() has been called for this thread */

/* Internal mutex locks for data structures */
#define LOCK(v)	  _spin_lock((void *)&v)
#define UNLOCK(v) _spin_unlock((void *)&v)
/* Number of times to spin when the lock is unavailable and we are on a
   multiprocessor.  On a uniprocessor we yield the processor immediately.  */
#define SPIN_TRIES 10
extern int _spin_tries;

/* Convenience symbols */
#ifndef FALSE
#define FALSE 0
#define TRUE  1
#endif
#ifndef NULL
#define NULL  0
#endif

#ifndef MACH_CALL
#define MACH_CALL(expr, ret) (ret) = (expr)
#endif

/* Prototypes. */

/* Functions defined in machine-dependent files. */
extern vm_address_t _sp(void);
extern vm_address_t _adjust_sp(vm_address_t sp);
extern void _spin_lock(pthread_lock_t *lockp);
extern void _spin_unlock(pthread_lock_t *lockp);
extern void _pthread_setup(pthread_t th, void (*f)(pthread_t), vm_address_t sp);

extern void _pthread_tsd_cleanup(pthread_t self);

/*
 * How long is left until `abstime', in the milliseconds urmach_futex wants.
 *
 * Returns 0 with *out_ms set, or ETIMEDOUT if the deadline has already passed
 * -- or if the clock cannot be read at all, which is the same answer for the
 * same reason: a timed wait whose deadline cannot be evaluated must not become
 * an untimed one.  Every caller of this already handles ETIMEDOUT; none of
 * them can handle waiting for ever.
 *
 * ⚠️ Written once and used by all four timed waits (mutex, cond, rwlock,
 * join), because each of them had its own copy and every copy had the same two
 * defects.
 *
 * 🔥 THE RETURN VALUE OF getclock() WAS IGNORED.  It fails -- it is an RPC to
 * the clock service, and if host_get_clock_service() ever fails the port stays
 * null and every later call fails with it -- and on failure it returns without
 * writing through its pointer.  So `now' stayed an UNINITIALISED STACK
 * VARIABLE and the deadline was computed from whatever was there.  A `then'
 * that comes out positive reads as "not expired yet", and the wait that
 * follows is as long as the garbage says.
 *
 * 🔥 AND THE ARITHMETIC WAS DONE IN THE NARROW TYPE.  `struct timespec' has
 * `unsigned long tv_sec' and `long tv_nsec' -- 64 bits each here -- while
 * tvalspec_t has `unsigned int' and `clock_res_t' (an int), so each caller
 * subtracted two 64-bit values and assigned the result to 32 bits.  It
 * survived only because the differences were small; it is the same shape as
 * the int-against-long compare-exchange that hung this library, and it is not
 * worth keeping one of those per file.
 */
static __inline__ int
_pthread_timeout_ms(const struct timespec *abstime, unsigned int *out_ms)
{
	struct timespec	now;
	long long	secs, nsecs;

	now.tv_sec = 0;
	now.tv_nsec = 0;
	if (getclock(TIMEOFDAY, &now) != 0)
		return (ETIMEDOUT);

	secs  = (long long) abstime->tv_sec  - (long long) now.tv_sec;
	nsecs = (long long) abstime->tv_nsec - (long long) now.tv_nsec;
	if (nsecs < 0)
	{
		nsecs += 1000000000LL;
		secs--;
	}
	if (secs < 0 || (secs == 0 && nsecs == 0))
		return (ETIMEDOUT);

	/*
	 * urmach_futex reads 0 as "block for ever", so a sub-millisecond
	 * remainder rounds up to 1 rather than becoming an untimed wait.  And a
	 * deadline further off than the field can hold is clamped rather than
	 * wrapped: waking early is a spurious wakeup, which every caller
	 * already tolerates, while wrapping is another way to wait for ever.
	 */
	if (secs > 4000000LL)
		*out_ms = 0xfffffffful;
	else
	{
		unsigned long long ms = (unsigned long long) secs * 1000ULL
				      + (unsigned long long) (nsecs / 1000000);
		*out_ms = (ms == 0) ? 1u : (unsigned int) ms;
	}
	return (0);
}

#endif /* _POSIX_PTHREAD_INTERNALS_H */
