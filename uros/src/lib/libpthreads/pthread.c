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
 * POSIX Pthread Library
 */

#include "pthread_internals.h"
#include <stdio.h>	/* For printf(). */
#include <errno.h>	/* For __mach_errno_addr() prototype. */
#include <mach/thread_info.h>	/* For THREAD_SCHED_RR_INFO (#153). */

extern void mig_init(void *initial);

/* Kernel trap to propagate thread name for DDB visibility */
extern kern_return_t mach_thread_set_name(const char *name);

/*
 * [Internal] stack support
 */

/* Weak so consumers (bootstrap, default_pager, ...) can override the
 * default stack size with their own strong definition (#106 — used to be
 * masked by --allow-multiple-definition). */
int _pthread_stack_size __attribute__((weak));
/* This 'shadow' stack size is used to prevent the user from changing the fundamental  
 * stack size after some threads have been created.  Since we use the stack for thread 
 * data structures, the stack size must be well known at all times and cannot vary from
 * thread to thread.
 */
static int __pthread_stack_size, __pthread_stack_mask;
static vm_address_t lowest_stack;
static vm_address_t *free_stacks;
/* #377: serialize the stack free-list.  The thread pool has _thread_pool_lock;
 * this list had none, so a concurrent pop (_pthread_allocate_stack) and push
 * (_pthread_free_stack) raced under SMP thread churn -- corrupting the list and
 * handing the same stack to two threads (stack-smash crash, scale sweep >8). */
static pthread_lock_t _free_stacks_lock = 0;

#define STACK_LOWEST(sp)	((sp) & ~__pthread_stack_mask)
#define STACK_RESERVED		(sizeof (struct _pthread))

#ifdef STACK_GROWS_UP

/* The stack grows towards higher addresses:
   |struct _pthread|user stack---------------->|
   ^STACK_BASE     ^STACK_START
   ^STACK_SELF
   ^STACK_LOWEST  */
#define STACK_BASE(sp)		STACK_LOWEST(sp)
#define STACK_START(stack_low)	(STACK_BASE(stack_low) + STACK_RESERVED)
#define STACK_SELF(sp)		STACK_BASE(sp)

#else

/* The stack grows towards lower addresses:
   |<----------------user stack|struct _pthread|
   ^STACK_LOWEST               ^STACK_START    ^STACK_BASE
			       ^STACK_SELF  */

#define STACK_BASE(sp)		(((sp) | __pthread_stack_mask) + 1)
#define STACK_START(stack_low)	(STACK_BASE(stack_low) - STACK_RESERVED)
#define STACK_SELF(sp)		STACK_START(sp)

#endif


#ifndef	PTHREAD_RESTART_ON_STACK
#error	"#444: this architecture has no way to re-enter a pooled thread on \
a fresh stack.  Without one the pool nests a whole exit path per reuse and \
walks the stack into its guard page."
#endif

static vm_address_t
_pthread_allocate_stack(void)
{
	vm_address_t cur_stack;
	kern_return_t kr;

	LOCK(_free_stacks_lock);			/* #377 */
	if (free_stacks == 0)
	{
	    /* Allocating guard pages is done by doubling
	     * the actual stack size, since STACK_BASE() needs
	     * to have stacks aligned on stack_size. Allocating just 
	     * one page takes as much memory as allocating more pages
	     * since it will remain one entry in the vm map.
	     * Besides, allocating more than one page allows tracking the
	     * overflow pattern when the overflow is bigger than one page.
	     */
#ifndef	NO_GUARD_PAGES
# define	GUARD_SIZE(a)	(2*(a))
# define	GUARD_MASK(a)	(((a)<<1) | 1)
#else
# define	GUARD_SIZE(a)	(a)
# define	GUARD_MASK(a)	(a)
#endif
		while (lowest_stack > GUARD_SIZE(__pthread_stack_size))
		{
			lowest_stack -= GUARD_SIZE(__pthread_stack_size);
			/* Ensure stack is there */
			kr = vm_allocate(mach_task_self(),
					 &lowest_stack,
					 GUARD_SIZE(__pthread_stack_size),
					 FALSE);
#ifndef	NO_GUARD_PAGES
			if (kr == KERN_SUCCESS) {
# ifdef	STACK_GROWS_UP
			    kr = vm_protect(mach_task_self(),
					    lowest_stack+__pthread_stack_size,
					    __pthread_stack_size,
					    FALSE, VM_PROT_NONE);
# else	/* STACK_GROWS_UP */
			    kr = vm_protect(mach_task_self(),
					    lowest_stack,
					    __pthread_stack_size,
					    FALSE, VM_PROT_NONE);
			    lowest_stack += __pthread_stack_size;
# endif	/* STACK_GROWS_UP */
			    if (kr == KERN_SUCCESS)
				break;
			}
#else
			if (kr == KERN_SUCCESS)
			    break;
#endif
		}
		if (lowest_stack > 0)
			free_stacks = (vm_address_t *)lowest_stack;
		else
		{
			/* Too bad.  We'll just have to take what comes.
			   Use vm_map instead of vm_allocate so we can
			   specify alignment.  */
			kr = vm_map(mach_task_self(), &lowest_stack,
				    GUARD_SIZE(__pthread_stack_size),
				    GUARD_MASK(__pthread_stack_mask),
				    TRUE /* anywhere */, MEMORY_OBJECT_NULL,
				    0, FALSE, VM_PROT_DEFAULT, VM_PROT_ALL,
				    VM_INHERIT_DEFAULT);
			/* This really shouldn't fail and if it does I don't
			   know what to do.  */
#ifndef	NO_GUARD_PAGES
			if (kr == KERN_SUCCESS) {
# ifdef	STACK_GROWS_UP
			    kr = vm_protect(mach_task_self(),
					    lowest_stack+__pthread_stack_size,
					    __pthread_stack_size,
					    FALSE, VM_PROT_NONE);
# else	/* STACK_GROWS_UP */
			    kr = vm_protect(mach_task_self(),
					    lowest_stack,
					    __pthread_stack_size,
					    FALSE, VM_PROT_NONE);
			    lowest_stack += __pthread_stack_size;
# endif	/* STACK_GROWS_UP */
			}
#endif
			free_stacks = (vm_address_t *)lowest_stack;
			lowest_stack = 0;
		}
		*free_stacks = 0; /* No other free stacks */
	}
	cur_stack = STACK_START((vm_address_t) free_stacks);
	free_stacks = (vm_address_t *)*free_stacks;
	UNLOCK(_free_stacks_lock);			/* #377 */
	cur_stack = _adjust_sp(cur_stack); /* Machine dependent stack fudging */
	return (cur_stack);
}

static void
_pthread_free_stack(pthread_t self)
{
	vm_address_t *base = (vm_address_t *) STACK_LOWEST((vm_address_t)self);
	LOCK(_free_stacks_lock);			/* #377 */
	*base = (vm_address_t) free_stacks;
	free_stacks = base;
	UNLOCK(_free_stacks_lock);			/* #377 */
}

/*
 * [Internal] thread pool — recycle kernel threads instead of destroying them.
 *
 * When a pthread exits, its kernel thread parks on a futex (self->pool_wake)
 * instead of calling thread_terminate().  pthread_create() can then reuse a
 * parked thread, avoiding the cost of thread_create() + thread_set_state()
 * RPCs.  The wake word lives in the pthread (per-thread), NOT here in the
 * slot: a slot is reused by the next parker before the previous (already
 * pool_get'd) thread is woken, so a slot-keyed futex would pile two threads
 * on one key and strand one of them (#352).
 */

#define _THREAD_POOL_MAX	8	/* Maximum cached kernel threads */

struct _pool_entry {
	thread_port_t	kernel_thread;	/* Mach thread port */
	vm_address_t	stack;		/* Stack assigned to this thread */
	pthread_t	self;		/* pthread_t at base of stack (has pool_wake) */
};

static struct _pool_entry _thread_pool[_THREAD_POOL_MAX];
static int _thread_pool_count;		/* Number of threads in pool */
static pthread_lock_t _thread_pool_lock = 0;	/* Protects pool */

/*
 * _pthread_pool_trampoline — entry point for recycled threads.
 *
 * The thread wakes from the pool semaphore, runs the user function,
 * then returns to the pool (or terminates if the pool is full).
 */
static void
_pthread_pool_trampoline(pthread_t self)
{
#ifdef	PTHREAD_POOL_DEPTH_PROBE
	/*
	 * #444: how deep is a reused thread, and does that change?
	 *
	 * This is what proved the fix, and it is left here because the
	 * absence of a crash could not: the fault it was hunting happened
	 * about once in nine runs.  Build with -DPTHREAD_POOL_DEPTH_PROBE
	 * and read the stack pointer across reuses.
	 *
	 *   before:  0xbff5ed8c 0xbff5dacc 0xbff5c80c ...  48 bytes per reuse
	 *   after:   0xbff8ff28 0xbff8ff28 0xbff8ff28 ...  flat
	 *
	 * (Compare each line with the previous one, not with the first: the
	 * first reuse can be on a different thread's stack, so that delta
	 * measures nothing.)
	 */
	{
		static unsigned long reuses;
		static unsigned long first_sp;
		unsigned long sp = (unsigned long) &sp;

		if (first_sp == 0)
			first_sp = sp;
		if ((++reuses % 100) == 0)
			printf("  pool depth: reuse %lu sp=0x%lx "
			       "(%ld bytes below the first)\n",
			       reuses, sp, (long) (first_sp - sp));
	}
#endif
	for (;;) {
		/* Run the user function.  fun and arg were published by the
		 * pthread_create that woke us. */
		pthread_exit((self->fun)(self->arg));
		/* NOT REACHED — pthread_exit either parks us or terminates */
	}
}

/*
 * _pthread_pool_park — try to park the current kernel thread in the pool.
 *
 * Returns 1 if parked (caller must NOT call thread_terminate),
 * returns 0 if pool is full (caller should terminate).
 * When the thread is woken, it starts executing _pthread_pool_trampoline.
 */
static int
_pthread_pool_park(pthread_t self)
{
	int idx;
	int wake_seq;

	LOCK(_thread_pool_lock);
	if (_thread_pool_count >= _THREAD_POOL_MAX) {
		UNLOCK(_thread_pool_lock);
		return 0;	/* Pool full — terminate */
	}
	idx = _thread_pool_count++;

	/* #352: park on a PER-THREAD futex word (self->pool_wake), not a
	 * per-slot one.  A slot address is reused by the next parker before
	 * this thread (once pool_get'd) is actually woken, so a slot-keyed
	 * futex would pile two threads on one key and thread_wakeup_one()
	 * would strand one of them (the lost wakeup #352). */
	_thread_pool[idx].kernel_thread = self->kernel_thread;
	_thread_pool[idx].stack = (vm_address_t)STACK_LOWEST((vm_address_t)self);
	_thread_pool[idx].self = self;
	/* #352: reset to the deterministic "parked" value (0) under the lock.
	 * pthread_create() reuses us by memset()ing the whole pthread (which
	 * includes pool_wake) and only THEN publishing fun/arg and bumping the
	 * word non-zero.  Anchoring the wait value at 0 means the memset (also
	 * 0) never makes our re-read mismatch — so we can only wake on the
	 * post-fun/arg bump, never escape early into a NULL fun.  No waker can
	 * race this store: pulling us needs the pool lock we still hold. */
	self->pool_wake = 0;
	wake_seq = 0;
	UNLOCK(_thread_pool_lock);

	/* Park: block until pthread_create publishes fun/arg and bumps our word */
	(void) _pthread_futex_wait(PTH_FW(self->pool_wake), wake_seq, 0);

	/*
	 * Woken up — self->fun and self->arg have been set by pthread_create.
	 *
	 * #444: re-enter the trampoline on a FRESH stack rather than calling
	 * it from here.  Calling it left this park, and the whole exit path
	 * above it, on the stack for the rest of the thread's life -- one
	 * more set of frames per reuse, until the guard page stopped it.
	 *
	 * Everything below the top of this stack belongs to work that has
	 * already finished; self sits at the top, outside it.
	 */
	PTHREAD_RESTART_ON_STACK(self, _pthread_pool_trampoline, self);
	/* NOT REACHED */
	return 1;
}

/*
 * _pthread_pool_get — try to get a recycled thread from the pool.
 *
 * On success, fills in *thread and *stack and returns 1.
 * The kernel thread is still blocked on its semaphore.
 */
static int
_pthread_pool_get(pthread_t *thread, vm_address_t *stack,
		  thread_port_t *kernel_thread, volatile int **wake_word)
{
	int idx;

	LOCK(_thread_pool_lock);
	if (_thread_pool_count == 0) {
		UNLOCK(_thread_pool_lock);
		return 0;
	}
	idx = --_thread_pool_count;
	*thread = _thread_pool[idx].self;
	*stack = _thread_pool[idx].stack;
	*kernel_thread = _thread_pool[idx].kernel_thread;
	/* #352: return the address of the PARKED THREAD's own wake futex word
	 * (not the slot's) so the caller bumps+wakes exactly that thread. */
	*wake_word = PTH_FW((*thread)->pool_wake);
	UNLOCK(_thread_pool_lock);
	return 1;
}

/*
 * Destroy a thread attribute structure
 */
int       
pthread_attr_destroy(pthread_attr_t *attr)
{
	if (attr->sig == _PTHREAD_ATTR_SIG)
	{
		return (ESUCCESS);
	} else
	{
		return (EINVAL); /* Not an attribute structure! */
	}
}

/*
 * Get the 'detach' state from a thread attribute structure.
 * Note: written as a helper function for info hiding
 */
int       
pthread_attr_getdetachstate(const pthread_attr_t *attr, 
			    int *detachstate)
{
	if (attr->sig == _PTHREAD_ATTR_SIG)
	{
		if (detachstate != (int *)NULL)
			*detachstate = attr->detached;
		return (ESUCCESS);
	} else
	{
		return (EINVAL); /* Not an attribute structure! */
	}
}

/*
 * Get the 'inherit scheduling' info from a thread attribute structure.
 * Note: written as a helper function for info hiding
 */
int       
pthread_attr_getinheritsched(const pthread_attr_t *attr, 
			     int *inheritsched)
{
	if (attr->sig == _PTHREAD_ATTR_SIG)
	{
		if (inheritsched != (int *)NULL)
			*inheritsched = attr->inherit;
		return (ESUCCESS);
	} else
	{
		return (EINVAL); /* Not an attribute structure! */
	}
}

/*
 * Get the scheduling parameters from a thread attribute structure.
 * Note: written as a helper function for info hiding
 */
int       
pthread_attr_getschedparam(const pthread_attr_t *attr, 
			   struct sched_param *param)
{
	if (attr->sig == _PTHREAD_ATTR_SIG)
	{
		if (param != (struct sched_param *)NULL)
			*param = attr->param;
		return (ESUCCESS);
	} else
	{
		return (EINVAL); /* Not an attribute structure! */
	}
}

/*
 * Get the scheduling policy from a thread attribute structure.
 * Note: written as a helper function for info hiding
 */
int       
pthread_attr_getschedpolicy(const pthread_attr_t *attr, 
			    int *policy)
{
	if (attr->sig == _PTHREAD_ATTR_SIG)
	{
		if (policy != (int *)NULL)
			*policy = attr->policy;
		return (ESUCCESS);
	} else
	{
		return (EINVAL); /* Not an attribute structure! */
	}
}

/*
 * Initialize a thread attribute structure to default values.
 */
int       
pthread_attr_init(pthread_attr_t *attr)
{
	attr->sig = _PTHREAD_ATTR_SIG;
	attr->policy = _PTHREAD_DEFAULT_POLICY;
	attr->param.sched_priority = 0;	/* #273: kernel keeps the real base */
	attr->inherit = _PTHREAD_DEFAULT_INHERITSCHED;
	attr->detached = PTHREAD_CREATE_JOINABLE;
	attr->stacksize = 0;	/* 0 = use default */
	attr->stackaddr = 0;	/* 0 = auto-allocate */
	attr->guardsize = 0;	/* 0 = use default (page size) */
	return (ESUCCESS);
}

/*
 * Set the 'detach' state in a thread attribute structure.
 * Note: written as a helper function for info hiding
 */
int       
pthread_attr_setdetachstate(pthread_attr_t *attr, 
			    int detachstate)
{
	if (attr->sig == _PTHREAD_ATTR_SIG)
	{
		if ((detachstate == PTHREAD_CREATE_JOINABLE) ||
		    (detachstate == PTHREAD_CREATE_DETACHED))
		{
			attr->detached = detachstate;
			return (ESUCCESS);
		} else
		{
			return (EINVAL);
		}
	} else
	{
		return (EINVAL); /* Not an attribute structure! */
	}
}

/*
 * Set the 'inherit scheduling' state in a thread attribute structure.
 * Note: written as a helper function for info hiding
 */
int       
pthread_attr_setinheritsched(pthread_attr_t *attr, 
			     int inheritsched)
{
	if (attr->sig == _PTHREAD_ATTR_SIG)
	{
		if ((inheritsched == PTHREAD_INHERIT_SCHED) ||
		    (inheritsched == PTHREAD_EXPLICIT_SCHED))
		{
			attr->inherit = inheritsched;
			return (ESUCCESS);
		} else
		{
			return (EINVAL);
		}
	} else
	{
		return (EINVAL); /* Not an attribute structure! */
	}
}

/*
 * Set the scheduling paramters in a thread attribute structure.
 * Note: written as a helper function for info hiding
 */
int       
pthread_attr_setschedparam(pthread_attr_t *attr, 
			   const struct sched_param *param)
{
	if (attr->sig == _PTHREAD_ATTR_SIG)
	{
		/* TODO: Validate sched_param fields */
		attr->param = *param;
		return (ESUCCESS);
	} else
	{
		return (EINVAL); /* Not an attribute structure! */
	}
}

/*
 * Set the scheduling policy in a thread attribute structure.
 * Note: written as a helper function for info hiding
 */
int       
pthread_attr_setschedpolicy(pthread_attr_t *attr, 
			    int policy)
{
	if (attr->sig == _PTHREAD_ATTR_SIG)
	{
		if ((policy == SCHED_OTHER) ||
		    (policy == SCHED_RR) ||
		    (policy == SCHED_FIFO))
		{
			attr->policy = policy;
			return (ESUCCESS);
		} else
		{
			return (EINVAL);
		}
	} else
	{
		return (EINVAL); /* Not an attribute structure! */
	}
}

/*
 * Set the stack size attribute.
 * The size must be a power of two and at least _PTHREAD_DEFAULT_STACKSIZE.
 */
int
pthread_attr_setstacksize(pthread_attr_t *attr, size_t stacksize)
{
	if (attr->sig != _PTHREAD_ATTR_SIG)
		return (EINVAL);
	if (stacksize < _PTHREAD_DEFAULT_STACKSIZE)
		return (EINVAL);
	if (stacksize & (stacksize - 1))
		return (EINVAL);	/* Must be power of two */
	if (stacksize > (0x7FFFFFFF / 2))
		return (EINVAL);	/* Would overflow GUARD_SIZE */
	attr->stacksize = stacksize;
	return (ESUCCESS);
}

/*
 * Get the stack size attribute.
 */
int
pthread_attr_getstacksize(const pthread_attr_t *attr, size_t *stacksize)
{
	if (attr->sig != _PTHREAD_ATTR_SIG)
		return (EINVAL);
	if (stacksize != (size_t *)NULL)
		*stacksize = attr->stacksize ? attr->stacksize : _PTHREAD_DEFAULT_STACKSIZE;
	return (ESUCCESS);
}

/*
 * Set stack address and size together (POSIX.1-2001).
 * The caller is responsible for allocating the stack memory.
 */
int
pthread_attr_setstack(pthread_attr_t *attr, void *stackaddr, size_t stacksize)
{
	if (attr->sig != _PTHREAD_ATTR_SIG)
		return (EINVAL);
	if (stacksize < _PTHREAD_DEFAULT_STACKSIZE)
		return (EINVAL);
	if (stacksize > (0x7FFFFFFF / 2))
		return (EINVAL);
	attr->stackaddr = (vm_address_t)stackaddr;
	attr->stacksize = stacksize;
	return (ESUCCESS);
}

/*
 * Get stack address and size together (POSIX.1-2001).
 */
int
pthread_attr_getstack(const pthread_attr_t *attr, void **stackaddr,
		      size_t *stacksize)
{
	if (attr->sig != _PTHREAD_ATTR_SIG)
		return (EINVAL);
	if (stackaddr != (void **)NULL)
		*stackaddr = (void *)attr->stackaddr;
	if (stacksize != (size_t *)NULL)
		*stacksize = attr->stacksize ? attr->stacksize
					     : _PTHREAD_DEFAULT_STACKSIZE;
	return (ESUCCESS);
}

/*
 * Set the guard area size attribute (POSIX.1-2001).
 * The guard size is rounded up to a page boundary by the implementation.
 * Setting guardsize to 0 disables the guard area.
 */
int
pthread_attr_setguardsize(pthread_attr_t *attr, size_t guardsize)
{
	if (attr->sig != _PTHREAD_ATTR_SIG)
		return (EINVAL);
	attr->guardsize = guardsize;
	return (ESUCCESS);
}

/*
 * Get the guard area size attribute (POSIX.1-2001).
 */
int
pthread_attr_getguardsize(const pthread_attr_t *attr, size_t *guardsize)
{
	if (attr->sig != _PTHREAD_ATTR_SIG)
		return (EINVAL);
	if (guardsize != (size_t *)NULL)
		*guardsize = attr->guardsize;
	return (ESUCCESS);
}

/*
 * Set contention scope (POSIX.1-2001).
 * Only PTHREAD_SCOPE_SYSTEM is supported (1:1 threading model).
 */
int
pthread_attr_setscope(pthread_attr_t *attr, int scope)
{
	if (attr->sig != _PTHREAD_ATTR_SIG)
		return (EINVAL);
	if (scope == PTHREAD_SCOPE_SYSTEM)
		return (ESUCCESS);
	if (scope == PTHREAD_SCOPE_PROCESS)
		return (ENOTSUP);
	return (EINVAL);
}

/*
 * Get contention scope (POSIX.1-2001).
 * Always returns PTHREAD_SCOPE_SYSTEM.
 */
int
pthread_attr_getscope(const pthread_attr_t *attr, int *scope)
{
	if (attr->sig != _PTHREAD_ATTR_SIG)
		return (EINVAL);
	if (scope != (int *)NULL)
		*scope = PTHREAD_SCOPE_SYSTEM;
	return (ESUCCESS);
}

/*
 * Query the actual attributes of a live thread (non-POSIX, de-facto standard).
 * Fills in `attr` with the thread's current detach state, scheduling policy
 * and parameters, stack address/size, and guard size.
 */
int
pthread_getattr_np(pthread_t thread, pthread_attr_t *attr)
{
	if (thread->sig != _PTHREAD_SIG)
		return (ESRCH);
	attr->sig = _PTHREAD_ATTR_SIG;
	attr->detached = thread->detached;
	attr->inherit = thread->inherit;
	attr->policy = thread->policy;
	attr->param = thread->param;
	attr->stackaddr = (vm_address_t)STACK_LOWEST((vm_address_t)thread);
	attr->stacksize = __pthread_stack_size;
	attr->guardsize = 0;
	return (ESUCCESS);
}

/*
 * Create and start execution of a new thread.
 */

static void
_pthread_body(pthread_t self)
{
	pthread_exit((self->fun)(self->arg));
}

/* Defined further down; applies (policy, base priority) to the kernel. */
static int _pthread_push_sched(pthread_t thread, int policy, int prio);

static int
_pthread_create(pthread_t t,
		const pthread_attr_t *attrs,
		const thread_port_t kernel_thread)
{
	int res;
	kern_return_t kern_res;
	extern int _mig_multithreaded;

	/*
	 * #299 (SMP reply-port aliasing): libmach also ships a single-threaded
	 * mig_support (one shared static reply port); its symbols are weak, so
	 * the only thing that guarantees the per-thread libpthreads version is
	 * linked in is a *strong* reference to a libpthreads-only mig symbol.
	 * _mig_multithreaded is exactly that — touching it here forces the
	 * per-thread mig_support to win the link for every program that creates
	 * threads.  Functionally: the moment a second thread is created, switch
	 * to per-thread reply ports (migrating the creator's current shared port
	 * into its own TSD) BEFORE the new thread can run.  Otherwise, on SMP the
	 * new thread races on another CPU and calls mig_get_reply_port() while
	 * _mig_multithreaded is still 0, grabbing the SAME reply port as its
	 * creator: two threads then receive on one reply port, ipc_mqueue_deliver
	 * hands the RPC reply to whichever is first in line, and the caller hangs
	 * forever (gpu_server's render worker was the first victim).  mig_init()
	 * is idempotent once _mig_multithreaded is already 1.
	 */
	if (!_mig_multithreaded)
		mig_init((void *)pthread_self());

	res = ESUCCESS;
	do
	{
		memset(t, 0, sizeof(*t));
		t->kernel_thread = kernel_thread;
		t->detached = attrs->detached;
		t->inherit = attrs->inherit;
		t->policy = attrs->policy;
		t->param = attrs->param;
		t->mutexes = (struct _pthread_mutex *)NULL;
		t->sig = _PTHREAD_SIG;
		LOCK_INIT(t->lock);
		t->cancel_state = PTHREAD_CANCEL_ENABLE | PTHREAD_CANCEL_DEFERRED;
		t->cleanup_stack = (struct _pthread_handler_rec *)NULL;
		t->sigmask = 0;
		t->sigpending = 0;
		t->sig_sem = 0;
		/* #324: joiners/death are futex words now (the join handshake
		 * blocks/wakes on them via urmach_futex), not Mach semaphores
		 * — just zero them; `detached` carries the joinable state. */
		t->death = 0;
		t->joiners = 0;
		t->num_joiners = 0;

		/*
		 * #274: with PTHREAD_EXPLICIT_SCHED the thread must start
		 * under the policy/priority from its attributes rather than
		 * inheriting the kernel default.  Push it now that the
		 * kernel thread exists; ignore the result so a policy the
		 * pset rejects doesn't fail thread creation (the thread
		 * still runs under the inherited policy).
		 */
		if (t->inherit == PTHREAD_EXPLICIT_SCHED)
			(void)_pthread_push_sched(t, t->policy,
						  t->param.sched_priority);
	} while (0);
	return (res);
}

int       
pthread_create(pthread_t *thread, 
	       const pthread_attr_t *attr,
	       void *(*start_routine)(void *), 
	       void *arg)
{
	pthread_attr_t _attr, *attrs;
	vm_address_t stack;
	int res;
	pthread_t t;
	kern_return_t kern_res;
	thread_port_t kernel_thread;
	if ((attrs = (pthread_attr_t *)attr) == (pthread_attr_t *)NULL)
	{			/* Set up default paramters */
		attrs = &_attr;
		pthread_attr_init(attrs);
	}
	res = ESUCCESS;

	/* Try to reuse a pooled kernel thread first */
	{
		volatile int *wake_word;
		if (_pthread_pool_get(&t, &stack, &kernel_thread, &wake_word))
		{
			*thread = t;
			if ((res = _pthread_create(t, attrs, kernel_thread)) != 0)
				return (res);
			t->arg = arg;
			t->fun = start_routine;
			/* Wake the parked thread (bump its seq) — it will run
			 * _pthread_pool_trampoline and read fun/arg. */
			(*wake_word)++;
			_pthread_futex_wake_one(wake_word);
			return (ESUCCESS);
		}
	}

	/* No pooled thread available — create a new one */
	do
	{
		/* Allocate a stack for the thread */
		stack = _pthread_allocate_stack();
		/* Thread structure lives at base of stack */
		t = (pthread_t)STACK_SELF(stack);
		*thread = t;
		/* Create the Mach thread for this thread */
		MACH_CALL(thread_create(mach_task_self(), &kernel_thread), kern_res);
		if (kern_res != KERN_SUCCESS)
		{
			printf("Can't create thread: %d\n", kern_res);
			res = EINVAL; /* Need better error here? */
			break;
		}
		if ((res = _pthread_create(t, attrs, kernel_thread)) != 0)
		{
			break;
		}
		t->arg = arg;
		t->fun = start_routine;
		/* Now set it up to execute */
		_pthread_setup(t, _pthread_body, stack);
		/* Send it on it's way */
		MACH_CALL(thread_resume(kernel_thread), kern_res);
		if (kern_res != KERN_SUCCESS)
		{
			printf("Can't resume thread: %d\n", kern_res);
			res = EINVAL; /* Need better error here? */
			break;
		}
	} while (0);
	return (res);
}

/*
 * Make a thread 'undetached' - no longer 'joinable' with other threads.
 */
int
pthread_detach(pthread_t thread)
{
	int num_joiners;
	if (thread->sig == _PTHREAD_SIG)
	{
		LOCK(thread->lock);
		if (thread->detached == PTHREAD_CREATE_JOINABLE)
		{
			thread->detached = PTHREAD_CREATE_DETACHED;
			num_joiners = thread->num_joiners;
			/* #324: bump the joiners futex under the lock so a
			 * racing joiner's snapshot is invalidated; wake them
			 * (they re-check, see DETACHED, return EINVAL). */
			(*PTH_FW(thread->joiners))++;
			UNLOCK(thread->lock);
			if (num_joiners > 0)
				_pthread_futex_wake_all(PTH_FW(thread->joiners));
			/* #324: no control semaphores to destroy. */
			return (ESUCCESS);
		} else
		{
			UNLOCK(thread->lock);
			return (EINVAL);
		}
	} else
	{
		return (ESRCH); /* Not a valid thread */
	}
}

/*
 * Terminate a thread.
 */
void 
pthread_exit(void *value_ptr)
{
	pthread_t self = pthread_self();
	struct _pthread_handler_rec *handler;
	kern_return_t kern_res;
	int num_joiners;
	while ((handler = self->cleanup_stack) != 0)
	{
		(handler->routine)(handler->arg);
		self->cleanup_stack = handler->next;
	}
	_pthread_tsd_cleanup(self);
	LOCK(self->lock);
	if (self->detached == PTHREAD_CREATE_JOINABLE)
	{
		self->detached = _PTHREAD_EXITED;
		self->exit_value = value_ptr;
		num_joiners = self->num_joiners;
		/* #324: publish EXITED + bump the joiners futex under the lock
		 * (invalidates a joiner's earlier snapshot — no lost wakeup). */
		(*PTH_FW(self->joiners))++;
		UNLOCK(self->lock);
		if (num_joiners > 0)
			_pthread_futex_wake_all(PTH_FW(self->joiners));
		/* Wait on the death futex until the joiner that harvests our
		 * exit value releases us by setting it non-zero. */
		while (*PTH_FW(self->death) == 0)
			(void) _pthread_futex_wait(PTH_FW(self->death), 0, 0);
	} else
		UNLOCK(self->lock);
	/* #324: nothing to destroy — joiners/death/sig_sem are futex words
	 * in user memory, not Mach semaphores. */
	/* Try to park this kernel thread in the pool for reuse */
	if (_pthread_pool_park(self))
		return;		/* NOT REACHED — park loops into trampoline */
	/* Pool full — actually terminate */
	_pthread_free_stack(self);
	MACH_CALL(thread_terminate(mach_thread_self()), kern_res);
}

/*
 * Wait for a thread to terminate and obtain its exit value.
 */
int       
pthread_join(pthread_t thread, 
	     void **value_ptr)
{
	kern_return_t kern_res;
	if (thread->sig == _PTHREAD_SIG)
	{
		LOCK(thread->lock);
		if (thread->detached == PTHREAD_CREATE_JOINABLE)
		{
			int j_snap;
			thread->num_joiners++;
			j_snap = *PTH_FW(thread->joiners);  /* snapshot under lock */
			UNLOCK(thread->lock);
			/* Block until exit/detach bumps the joiners futex. */
			(void) _pthread_futex_wait(PTH_FW(thread->joiners),
						   j_snap, 0);
			LOCK(thread->lock);
			thread->num_joiners--;
		}
		if (thread->detached == _PTHREAD_EXITED)
		{
			if (thread->num_joiners == 0)
			{	/* Give the result to this thread */
				if (value_ptr)
				{
					*value_ptr = thread->exit_value;
				}
				UNLOCK(thread->lock);
				/* #324: release the exiting thread (the hearse)
				 * via the death futex. */
				*PTH_FW(thread->death) = 1;
				_pthread_futex_wake_one(PTH_FW(thread->death));
				return (ESUCCESS);
			} else
			{	/* This 'joiner' missed the catch! */
				UNLOCK(thread->lock);
				return (ESRCH);
			}
		} else
		{		/* The thread has become anti-social! */
			UNLOCK(thread->lock);
			return (EINVAL);
		}
	} else
	{
		return (ESRCH); /* Not a valid thread */
	}
}

/*
 * pthread_timedjoin_np — like pthread_join but with an absolute
 * CLOCK_REALTIME deadline (#156).  GNU/Linux extension; non-POSIX.
 *
 * On timeout the joiner walks back the num_joiners increment so the
 * thread doesn't think it still has a watcher.  Same race window as
 * pthread_join itself: between semaphore_timedwait returning
 * KERN_OPERATION_TIMED_OUT and us re-acquiring thread->lock, the
 * thread may have just exited and signalled joiners — in that
 * case we honour the join and harvest the exit value (we don't
 * want to drop the corpse on the floor just because we were a few
 * cycles slow).
 */
int
pthread_timedjoin_np(pthread_t thread,
		     void **value_ptr,
		     const struct timespec *abstime)
{
	kern_return_t kern_res;

	if (thread->sig != _PTHREAD_SIG)
		return (ESRCH);
	if (abstime == NULL)
		return (EINVAL);

	LOCK(thread->lock);
	if (thread->detached != PTHREAD_CREATE_JOINABLE &&
	    thread->detached != _PTHREAD_EXITED) {
		UNLOCK(thread->lock);
		return (EINVAL);
	}

	if (thread->detached == PTHREAD_CREATE_JOINABLE) {
		struct timespec now;
		tvalspec_t then;
		int j_snap;

		thread->num_joiners++;
		j_snap = *PTH_FW(thread->joiners);  /* snapshot under lock */
		UNLOCK(thread->lock);

		getclock(TIMEOFDAY, &now);
		then.tv_nsec = abstime->tv_nsec - now.tv_nsec;
		then.tv_sec  = abstime->tv_sec  - now.tv_sec;
		if (then.tv_nsec < 0) {
			then.tv_nsec += 1000000000;
			then.tv_sec--;
		}
		if ((int)then.tv_sec < 0) {
			then.tv_sec  = 0;
			then.tv_nsec = 0;
		}

		/* #324: deadline already passed -> immediate timeout (don't
		 * pass 0 ms to the futex, which would mean "wait forever"). */
		if (then.tv_sec == 0 && then.tv_nsec == 0) {
			kern_res = KERN_OPERATION_TIMED_OUT;
		} else {
			unsigned int tmo_ms =
				(unsigned int)then.tv_sec * 1000u +
				(unsigned int)(then.tv_nsec / 1000000);
			if (tmo_ms == 0)
				tmo_ms = 1;
			kern_res = _pthread_futex_wait(PTH_FW(thread->joiners),
						       j_snap, tmo_ms);
		}
		LOCK(thread->lock);
		thread->num_joiners--;

		if (kern_res == KERN_OPERATION_TIMED_OUT &&
		    thread->detached != _PTHREAD_EXITED) {
			UNLOCK(thread->lock);
			return (ETIMEDOUT);
		}
		/* Either the wait succeeded, or it timed out *just* as
		 * the thread exited — fall through to the exit harvest
		 * below. */
	}

	if (thread->detached == _PTHREAD_EXITED) {
		if (thread->num_joiners == 0) {
			if (value_ptr)
				*value_ptr = thread->exit_value;
			UNLOCK(thread->lock);
			/* #324: release the exiting thread via the death futex. */
			*PTH_FW(thread->death) = 1;
			_pthread_futex_wake_one(PTH_FW(thread->death));
			return (ESUCCESS);
		}
		UNLOCK(thread->lock);
		return (ESRCH);
	}

	UNLOCK(thread->lock);
	return (EINVAL);
}

/*
 * Push a (policy, base priority) pair down to the Mach scheduler for a
 * thread and keep the pthread bookkeeping in sync.  Shared by
 * pthread_setschedparam (explicit policy) and pthread_setschedprio
 * (current policy).  Returns a POSIX errno.
 *
 * Priority range is policy- and kernel-defined (invalid_pri in
 * kern/sched.h: 0..lowest-schedulable, and a thread may only lower its
 * own priority below its max); the kernel is the authority and we map
 * its failures to errno.  A policy the processor set does not enable
 * comes back as KERN_INVALID_POLICY -> ENOTSUP.
 */
static int
_pthread_push_sched(pthread_t thread, int policy, int prio)
{
	kern_return_t	kr;

	switch (policy)
	{
	case POLICY_FIFO:
	{
		struct policy_fifo_base	fifo_base;

		fifo_base.base_priority = prio;
		kr = thread_policy(thread->kernel_thread, POLICY_FIFO,
				   (policy_base_t)&fifo_base,
				   POLICY_FIFO_BASE_COUNT, FALSE);
		break;
	}
	case POLICY_RR:
	{
		struct policy_rr_base	rr_base;
		policy_rr_info_data_t	rr_info;
		mach_msg_type_number_t	cnt = POLICY_RR_INFO_COUNT;

		/*
		 * Preserve the current round-robin quantum if the thread
		 * already runs RR; otherwise let the kernel pick a default.
		 */
		rr_base.quantum = 0;
		if (thread_info(thread->kernel_thread, THREAD_SCHED_RR_INFO,
				(thread_info_t)&rr_info, &cnt) == KERN_SUCCESS)
			rr_base.quantum = rr_info.quantum;
		rr_base.base_priority = prio;
		kr = thread_policy(thread->kernel_thread, POLICY_RR,
				   (policy_base_t)&rr_base,
				   POLICY_RR_BASE_COUNT, FALSE);
		break;
	}
	case POLICY_TIMESHARE:
	{
		struct policy_timeshare_base	ts_base;

		ts_base.base_priority = prio;
		kr = thread_policy(thread->kernel_thread, POLICY_TIMESHARE,
				   (policy_base_t)&ts_base,
				   POLICY_TIMESHARE_BASE_COUNT, FALSE);
		break;
	}
	default:
		return (EINVAL);
	}

	if (kr == KERN_INVALID_POLICY)
		return (ENOTSUP);
	if (kr != KERN_SUCCESS)
		return (EINVAL);

	/* Keep the pthread bookkeeping consistent with the kernel. */
	LOCK(thread->lock);
	thread->policy = policy;
	thread->param.sched_priority = prio;
	UNLOCK(thread->lock);
	return (ESUCCESS);
}

/*
 * Refresh the pthread struct's cached policy/priority from the kernel
 * for the policy currently in effect.  Best-effort: leaves the cache
 * untouched if the thread cannot be queried.
 */
static void
_pthread_refresh_sched(pthread_t thread, int policy)
{
	int			base = -1;
	mach_msg_type_number_t	c;

	switch (policy)
	{
	case POLICY_TIMESHARE:
	{
		policy_timeshare_info_data_t ti;

		c = POLICY_TIMESHARE_INFO_COUNT;
		if (thread_info(thread->kernel_thread,
				THREAD_SCHED_TIMESHARE_INFO,
				(thread_info_t)&ti, &c) == KERN_SUCCESS)
			base = ti.base_priority;
		break;
	}
	case POLICY_RR:
	{
		policy_rr_info_data_t ri;

		c = POLICY_RR_INFO_COUNT;
		if (thread_info(thread->kernel_thread, THREAD_SCHED_RR_INFO,
				(thread_info_t)&ri, &c) == KERN_SUCCESS)
			base = ri.base_priority;
		break;
	}
	case POLICY_FIFO:
	{
		policy_fifo_info_data_t fi;

		c = POLICY_FIFO_INFO_COUNT;
		if (thread_info(thread->kernel_thread, THREAD_SCHED_FIFO_INFO,
				(thread_info_t)&fi, &c) == KERN_SUCCESS)
			base = fi.base_priority;
		break;
	}
	default:
		return;
	}

	LOCK(thread->lock);
	thread->policy = policy;
	if (base >= 0)
		thread->param.sched_priority = base;
	UNLOCK(thread->lock);
}

/*
 * Get the scheduling policy and parameters for a thread, reading the
 * real state in effect from the kernel — the pthread struct cache can
 * lag the kernel (e.g. the default policy differs from what the
 * processor set actually runs).  Falls back to the cache if the thread
 * cannot be queried.
 */
int
pthread_getschedparam(pthread_t thread,
		      int *policy,
		      struct sched_param *param)
{
	thread_basic_info_data_t	binfo;
	mach_msg_type_number_t		cnt = THREAD_BASIC_INFO_COUNT;

	if (thread->sig != _PTHREAD_SIG)
		return (ESRCH);  /* Not a valid thread structure */

	if (thread_info(thread->kernel_thread, THREAD_BASIC_INFO,
			(thread_info_t)&binfo, &cnt) == KERN_SUCCESS)
		_pthread_refresh_sched(thread, binfo.policy);

	LOCK(thread->lock);
	if (policy != (int *)NULL)
		*policy = thread->policy;
	if (param != (struct sched_param *)NULL)
		*param = thread->param;
	UNLOCK(thread->lock);
	return (ESUCCESS);
}

/*
 * Set the scheduling policy and parameters for a thread.  Unlike the
 * historical stub, this actually pushes the requested policy + base
 * priority to the Mach scheduler (a policy the processor set does not
 * enable returns ENOTSUP).
 */
int
pthread_setschedparam(pthread_t thread,
		      int policy,
		      const struct sched_param *param)
{
	if (thread->sig != _PTHREAD_SIG)
		return (ESRCH);  /* Not a valid thread structure */
	if (param == (const struct sched_param *)NULL)
		return (EINVAL);
	if (policy != SCHED_OTHER && policy != SCHED_FIFO && policy != SCHED_RR)
		return (EINVAL);
	if (param->sched_priority < 0)
		return (EINVAL);

	return (_pthread_push_sched(thread, policy, param->sched_priority));
}

/*
 * Change a thread's scheduling priority without changing its policy
 * (POSIX.1-2001).  Lighter than pthread_setschedparam: it keeps the
 * thread's current policy and only pushes the new base priority down to
 * the Mach scheduler.
 *
 * Uses the thread's *actual* scheduling policy as the kernel reports it,
 * not the policy cached in the pthread struct: the two can diverge
 * (#273), and the kernel's view is authoritative.
 */
int
pthread_setschedprio(pthread_t thread, int prio)
{
	thread_basic_info_data_t binfo;
	mach_msg_type_number_t	cnt = THREAD_BASIC_INFO_COUNT;

	if (thread->sig != _PTHREAD_SIG)
		return (ESRCH);
	if (prio < 0)
		return (EINVAL);

	if (thread_info(thread->kernel_thread, THREAD_BASIC_INFO,
			(thread_info_t)&binfo, &cnt) != KERN_SUCCESS)
		return (ESRCH);

	return (_pthread_push_sched(thread, binfo.policy, prio));
}

/*
 * Set the name of a thread (non-POSIX, de-facto standard).
 * Name is truncated to 15 characters + NUL.
 */
int
pthread_setname_np(pthread_t thread, const char *name)
{
	int i;
	if (thread->sig != _PTHREAD_SIG)
		return (ESRCH);
	LOCK(thread->lock);
	for (i = 0; i < 15 && name[i] != '\0'; i++)
		thread->name[i] = name[i];
	thread->name[i] = '\0';
	UNLOCK(thread->lock);
	/* Propagate to kernel for DDB visibility (best-effort, ignore errors) */
	if (thread == pthread_self())
		mach_thread_set_name(thread->name);
	return (ESUCCESS);
}

/*
 * Get the name of a thread (non-POSIX, de-facto standard).
 */
int
pthread_getname_np(pthread_t thread, char *buf, int len)
{
	int i;
	if (thread->sig != _PTHREAD_SIG)
		return (ESRCH);
	if (len <= 0)
		return (EINVAL);
	LOCK(thread->lock);
	for (i = 0; i < len - 1 && thread->name[i] != '\0'; i++)
		buf[i] = thread->name[i];
	buf[i] = '\0';
	UNLOCK(thread->lock);
	return (ESUCCESS);
}

/*
 * Determine if two thread identifiers represent the same thread.
 */
int       
pthread_equal(pthread_t t1, 
	      pthread_t t2)
{
	return (t1 == t2);
}

/*
 * Return the thread identifier for the current thread.
 */
pthread_t 
pthread_self(void)
{
	return ((pthread_t)STACK_SELF(_sp()));
}

/*
 * Execute a function exactly one time in a thread-safe fashion.
 *
 * Three-state protocol on sig (no spinlock held during init_routine):
 *   _PTHREAD_ONCE_SIG_init  →  not yet started
 *   _PTHREAD_NO_SIG         →  in progress (one thread running init)
 *   _PTHREAD_ONCE_SIG       →  done
 *
 * CAS(init→0): winner runs init_routine, then publishes SIG.
 * Losers spin until sig == _PTHREAD_ONCE_SIG.
 * Reentrant call from init_routine sees in-progress and returns
 * immediately (POSIX says behavior is undefined, but we avoid deadlock).
 */
int
pthread_once(pthread_once_t *once_control,
	     void (*init_routine)(void))
{
	long state = __atomic_load_n(&once_control->sig, __ATOMIC_ACQUIRE);
	if (state == _PTHREAD_ONCE_SIG)
		return (ESUCCESS);

	if (state == _PTHREAD_ONCE_SIG_init)
	{
		long expected = _PTHREAD_ONCE_SIG_init;
		if (__atomic_compare_exchange_n(&once_control->sig, &expected,
						_PTHREAD_NO_SIG, 0,
						__ATOMIC_ACQUIRE,
						__ATOMIC_ACQUIRE))
		{
			(*init_routine)();
			__atomic_store_n(&once_control->sig,
					 _PTHREAD_ONCE_SIG,
					 __ATOMIC_RELEASE);
			return (ESUCCESS);
		}
	}

	/* Another thread is running init, or reentrant — spin until done */
	while (__atomic_load_n(&once_control->sig, __ATOMIC_ACQUIRE)
	       != _PTHREAD_ONCE_SIG)
		;
	return (ESUCCESS);
}

/*
 * Cancel a thread
 */
int
pthread_cancel(pthread_t thread)
{
	if (thread->sig == _PTHREAD_SIG)
	{
		thread->cancel_state |= _PTHREAD_CANCEL_PENDING;
		return (ESUCCESS);
	} else
	{
		return (ESRCH);
	}
}

/*
 * Insert a cancellation point in a thread.
 */
static void
_pthread_testcancel(pthread_t thread)
{
	LOCK(thread->lock);
	if ((thread->cancel_state & (PTHREAD_CANCEL_ENABLE|_PTHREAD_CANCEL_PENDING)) == 
	    (PTHREAD_CANCEL_ENABLE|_PTHREAD_CANCEL_PENDING))
	{
		UNLOCK(thread->lock);
		pthread_exit(0);
	}
	UNLOCK(thread->lock);
}

void
pthread_testcancel(void)
{
	pthread_t self = pthread_self();
	_pthread_testcancel(self);
}

/*
 * Query/update the cancelability 'state' of a thread
 */
int
pthread_setcancelstate(int state, int *oldstate)
{
	pthread_t self = pthread_self();
	int err = ESUCCESS;
	LOCK(self->lock);
	if (oldstate != (int *)NULL)
		*oldstate = self->cancel_state & _PTHREAD_CANCEL_STATE_MASK;
	if ((state == PTHREAD_CANCEL_ENABLE) || (state == PTHREAD_CANCEL_DISABLE))
	{
		self->cancel_state = (self->cancel_state & _PTHREAD_CANCEL_STATE_MASK) | state;
	} else
	{
		err = EINVAL;
	}
	UNLOCK(self->lock);
	_pthread_testcancel(self);  /* See if we need to 'die' now... */
	return (err);
}

/*
 * Query/update the cancelability 'type' of a thread
 */
int
pthread_setcanceltype(int type, int *oldtype)
{
	pthread_t self = pthread_self();
	int err = ESUCCESS;
	LOCK(self->lock);
	if (oldtype != (int *)NULL)
		*oldtype = self->cancel_state & _PTHREAD_CANCEL_TYPE_MASK;
	if ((type == PTHREAD_CANCEL_DEFERRED) || (type == PTHREAD_CANCEL_ASYNCHRONOUS))
	{
		self->cancel_state = (self->cancel_state & _PTHREAD_CANCEL_TYPE_MASK) | type;
	} else
	{
		err = EINVAL;
	}
	UNLOCK(self->lock);
	_pthread_testcancel(self);  /* See if we need to 'die' now... */
	return (err);
}

/*
 * Concurrency level hint (POSIX.1-2001).
 * With 1:1 threading this is a no-op; the value is stored but not used.
 */
static int _pthread_concurrency;

int
pthread_setconcurrency(int level)
{
	if (level < 0)
		return (EINVAL);
	_pthread_concurrency = level;
	return (ESUCCESS);
}

int
pthread_getconcurrency(void)
{
	return (_pthread_concurrency);
}

/*
 * Perform package initialization - called automatically when application starts
 */

static int
pthread_init(void)
{
	vm_address_t new_stack;
	pthread_attr_t _attr, *attrs;
	pthread_t thread;
	kern_return_t kr;
	host_basic_info_data_t info;
	mach_msg_type_number_t count;

	/* See if we're on a multiprocessor and set _spin_tries if so.  */
	count = HOST_BASIC_INFO_COUNT;
	kr = host_info(mach_host_self(), HOST_BASIC_INFO, (host_info_t) &info,
		       &count);
	if (kr != KERN_SUCCESS)
		printf("host_info failed (%d); probably need privilege.\n", kr);
	else if (info.avail_cpus > 1)
		_spin_tries = SPIN_TRIES;

	if (_pthread_stack_size == 0)
	{
		_pthread_stack_size = _PTHREAD_DEFAULT_STACKSIZE;
	} else
	{ /* Validate user-supplied stack size */
		if (_pthread_stack_size & (_pthread_stack_size-1))
		{  /* Not a power of two!  Can't use it! */
			_pthread_stack_size = _PTHREAD_DEFAULT_STACKSIZE;
		}
	}
	/* Cap at INT_MAX/2 to prevent GUARD_SIZE(2*stacksize) overflow */
	if (_pthread_stack_size > (0x7FFFFFFF / 2))
		_pthread_stack_size = _PTHREAD_DEFAULT_STACKSIZE;
	__pthread_stack_size = _pthread_stack_size;
	__pthread_stack_mask = __pthread_stack_size - 1;
	lowest_stack = STACK_LOWEST(_sp());
	attrs = &_attr;
	pthread_attr_init(attrs);
	new_stack = _pthread_allocate_stack();
	thread = (pthread_t)STACK_SELF(new_stack);
	_pthread_create(thread, attrs, mach_thread_self());
	thread->detached = _PTHREAD_CREATE_PARENT;
	/* Initialize MIG reply port support for multi-threaded mode */
	mig_init((void *)thread);
	return (new_stack);
}

/*
 * Thread-local errno
 */
int *
__mach_errno_addr(void)
{
	return &pthread_self()->err_no;
}

/*
 * These function pointers are picked up by crt0.c (__start_mach):
 *   _threadlib_init_routine — called before main(), returns new SP
 *   _thread_init_routine    — legacy alias (some startup code uses this)
 *   _threadlib_exit_routine — called after main() returns
 */
int (*_thread_init_routine)(void) = pthread_init;
int (*_threadlib_init_routine)(void) = pthread_init;

static void
_pthread_exit_routine(int status)
{
	pthread_exit((void *)(long)status);
}

void (*_threadlib_exit_routine)(int) = _pthread_exit_routine;
