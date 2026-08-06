/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The thread and activation surface the machine-independent kernel calls
 * (#453, on #408's ground).
 *
 * None of the mechanism is here.  x86_64/thread/ already has the switch
 * (context.c), the floating-point state (fpu.c) and the conversions between
 * a trap frame and the exported flavors (state.c).  This file is the layer
 * between those and the names kern/thread.c, kern/thread_act.c and
 * kern/machine.c reach for -- seventeen of them, which the link wanted all
 * at once.
 *
 * switch_context() is here now, and the convention that kept it out is
 * answered by a slot in the per-CPU block -- see <cpu/percpu.h> and the
 * routine itself below.
 *
 * ── Where the pcb lives ───────────────────────────────────────────────
 *
 * In the activation, with a pointer beside it, as on i386: `mact.xxx_pcb'
 * is the storage and `mact.pcb' points at it.  The indirection buys nothing
 * on either machine and is kept because the machine-independent tree writes
 * thr_act->mact.pcb and would otherwise need a per-machine spelling.
 */

#include <string.h>

#include <kern/kalloc.h>

#include <kern/thread.h>
#include <time/clock_event.h>	/* #459: start this processor's clock */
#include <boot/bootarg.h>		/* #459: boot_flag */
extern thread_t preempt_test_run(void);	/* #459: x86_64/time/preempt_test.c */
#include <kern/thread_act.h>
#include <kern/task.h>
#include <kern/sched_prim.h>
#include <kern/misc_protos.h>
#include <mach/thread_status.h>

#include <thread/context.h>
#include <thread/fpu.h>
#include <thread/state.h>
#include <trap/trap.h>
#include <cpu/percpu.h>

/*
 * Module init.  Both halves exist because the machine-independent tree calls
 * them at different moments -- act_machine_init() before any activation and
 * thread_machine_init() before any thread -- and on this machine there is
 * one thing to do, which is to let the floating-point code work out what the
 * processor supports before anything asks it to save state.
 */
void
act_machine_init(void)
{
}

void
thread_machine_init(void)
{
	fpu_init();
}

kern_return_t
act_machine_create(struct task *task, thread_act_t thr_act)
{
	pcb_t	pcb = &thr_act->mact.xxx_pcb;

	(void) task;

	memset(pcb, 0, sizeof *pcb);
	simple_lock_init(&pcb->lock, ETAP_MISC_PCB);
	thr_act->mact.pcb = pcb;

	/*
	 * No user frame yet, and that is the honest state: `user' is the
	 * frame trap entry leaves behind, so a thread that has never been to
	 * ring 3 has none.  USER_REGS() asserts on it rather than handing
	 * back a zeroed one, because a zeroed frame reads as a thread stopped
	 * at address zero, which is a lie a debugger would repeat.
	 */
	return KERN_SUCCESS;
}

void
act_machine_destroy(thread_act_t thr_act)
{
	thr_act->mact.pcb = PCB_NULL;
}

/*
 * Make this activation's state the one the processor will use.
 *
 * Two things, and neither is the register file: the registers are switched
 * by context_switch() on the way through.  What is left is where an entry
 * from ring 3 should land, and whose floating-point area the next lazy
 * restore should read.
 */
void
act_machine_switch_pcb(thread_act_t thr_act)
{
	pcb_t	pcb = thr_act->mact.pcb;

	if (pcb == PCB_NULL)
		return;

	/*
	 * Only the stack.  The floating-point state is switched eagerly by
	 * context_switch(), which saves the outgoing area and restores the
	 * incoming one -- there is no lazy model here to point at an area for
	 * later (#408 chose eager XSAVE over CR0.TS deliberately), so there
	 * is nothing for this to do about it.
	 */
	percpu()->kernel_rsp = pcb->ctx.kernel_stack_top;
}

/*
 * Prepare a thread that has never run so that switching to it arrives at
 * `start_pos'.
 *
 * The kernel stack is already attached by the machine-independent side; all
 * that is left is to write it to look exactly like a thread that was
 * switched out, which is what context_init() does and why a never-run thread
 * and an interrupted one are indistinguishable to the switch.
 */
/*
 * Where a thread that has never run begins.
 *
 * context_init() can only place a function and one argument, and the argument
 * a new thread needs -- which thread it took the processor from -- is not
 * known when its context is built.  It is known at the switch, and
 * switch_context() leaves it in this processor's prev_thread, so it is read
 * here rather than carried.
 */
static void
thread_begin_trampoline(void *unused)
{
	thread_t prev = (thread_t) percpu()->prev_thread;

	(void) unused;

	/*
	 * The thread we took the processor from, first: it is still accounted
	 * as running until somebody says otherwise, and nothing else may
	 * happen on this processor until it has been said.
	 */
	if (prev != THREAD_NULL)
		thread_dispatch(prev);

	/*
	 * Interrupts on, HERE and not sooner (#459).
	 *
	 * A thread that has never run reaches this from inside the interrupt
	 * that preempted its predecessor, so it inherits IF clear -- and on
	 * this machine nothing else will set it.  spl is a software priority
	 * kept in the per-CPU block (x86_64/cpu/spl.c): splx() moves the level
	 * and does not touch the flag, and only sploff()/splon() do.  A resumed
	 * thread gets its flag back from the IRETQ of its own trap; a new one
	 * has no trap to return from.
	 *
	 * ⚠️ Order measured, not assumed.  Enabling at the top of the
	 * trampoline -- before the dispatch above -- lets the next tick in
	 * while the switch is still letting go of the previous thread, and the
	 * machine stops on that first tick.  After the dispatch, there is
	 * nothing left half-done for an interrupt to walk into.
	 */
	__asm__ __volatile__("sti" : : : "memory");

	/*
	 * And the rest to the machine-independent trampoline, with THREAD_NULL
	 * because the dispatch is already done: enable_preemption() -- which
	 * thread_invoke() disabled on the way in and which a new thread never
	 * reaches the way out of -- then spllo(), then this thread's own
	 * function through self->continuation.
	 */
	thread_continue(THREAD_NULL);
	/*NOTREACHED*/
}

kern_return_t
thread_machine_create(thread_t thread, thread_act_t thr_act,
		      void (*start_pos)(void))
{
	pcb_t	pcb = thr_act->mact.pcb;

	if (pcb == PCB_NULL)
		return KERN_FAILURE;

	/*
	 * The kernel stack is allocated HERE, and that is this function's job
	 * rather than its precondition (#458).
	 *
	 * ⚠️ This used to test `thread->kernel_stack == 0' and answer
	 * KERN_RESOURCE_SHORTAGE, on the reading that the stack arrives with
	 * the thread.  It does not: kern/thread.c's thread_create_in() calls
	 * this before anything has given the shuttle a stack, so the test was
	 * true on the very first kernel thread and the boot stopped there --
	 * with a resource shortage on a machine with 116 MiB free, which is
	 * the kind of answer that sends you looking in the wrong place.
	 *
	 * i386 does the same thing in the same function.  The interface reads
	 * as though the stack were an input because the field is already in
	 * the structure; what settles it is who assigns it, and nobody else
	 * does.
	 */
	thread->kernel_stack = stack_alloc();
	if (thread->kernel_stack == 0)
		return KERN_RESOURCE_SHORTAGE;

	/*
	 * The entry point is the MI trampoline, not the thread's own function
	 * (#459).
	 *
	 * kern/sched_prim.c says why, in thread_continue() itself: "the first
	 * time a newly-created thread executes, it magically appears here, and
	 * never executes the enable_preemption() call in thread_invoke()".
	 * Three things are owed to a thread that has never run, and every one
	 * of them is done by code the resumed path runs and the new path skips:
	 *
	 *   thread_dispatch(old)   the thread we took the processor FROM is
	 *                          still holding it in its own accounting.
	 *   enable_preemption()    thread_invoke() disabled it on the way in
	 *                          and re-enables on the way out, and a new
	 *                          thread never reaches that way out.
	 *   spllo()                the level, and with it the interrupt flag.
	 *                          Without it the first thread preempted into
	 *                          runs forever with interrupts off: no tick
	 *                          arrives, no quantum expires, and the
	 *                          processor is never taken back.
	 *
	 * ⚠️ Measured, and fixing the last one alone is not enough: an STI at
	 * the top of the trampoline lets the tick in BEFORE the switch has
	 * finished letting go, and the machine stops on the first one.  The
	 * three belong together and in this order, which is what
	 * thread_continue() already does.
	 *
	 * The thread's own function is reached from there through
	 * self->continuation, which thread_start() set to this same start_pos.
	 */
	(void) start_pos;
	context_init(&pcb->ctx,
		     (uint64_t) thread->kernel_stack + KERNEL_STACK_SIZE,
		     thread_begin_trampoline, (void *) 0,
		     fpu_area_alloc());

	return KERN_SUCCESS;
}

void
thread_machine_destroy(thread_t thread)
{
	thread_act_t	thr_act = thread->top_act;

	if (thr_act == THR_ACT_NULL || thr_act->mact.pcb == PCB_NULL)
		return;

	fpu_area_free(thr_act->mact.pcb->ctx.fpu_area);
	thr_act->mact.pcb->ctx.fpu_area = (void *) 0;
}

/*
 * Throw away whatever floating-point state this activation has, without
 * saving it.  Called when the state is about to be replaced wholesale, so
 * writing it out first would be work whose result is immediately discarded.
 */
void
thread_machine_flush(thread_act_t thr_act)
{
	if (thr_act != THR_ACT_NULL && thr_act->mact.pcb != PCB_NULL)
		fpu_area_init(thr_act->mact.pcb->ctx.fpu_area);
}

/*
 * Point a kernel stack at `continuation' -- the thread will resume there
 * rather than where it was interrupted.
 *
 * The machine-independent scheduler uses this to discard a stack's contents
 * on purpose: a thread that blocks with a continuation does not need its
 * stack kept, so the stack is reset and the thread costs nothing while it
 * waits.  That is the whole point of continuations and it is why this is
 * initialisation rather than a jump.
 */
void
machine_kernel_stack_init(thread_t thread, void (*continuation)(void))
{
	thread_act_t	thr_act = thread->top_act;
	pcb_t		pcb;

	assert(thr_act != THR_ACT_NULL);
	pcb = thr_act->mact.pcb;
	assert(pcb != PCB_NULL);

	context_init(&pcb->ctx,
		     (uint64_t) thread->kernel_stack + KERNEL_STACK_SIZE,
		     (void (*)(void *)) continuation, (void *) 0,
		     pcb->ctx.fpu_area);
}

/*
 * A thread has changed which activation is on top.  The address space may
 * change with it -- an activation can be borrowing another task's map -- so
 * this is where the processor is told, and the new activation's frame and
 * floating-point area become the current ones.
 */
void
machine_switch_act(thread_t thread, thread_act_t old, thread_act_t new,
		   unsigned cpu)
{
	(void) thread;
	(void) old;
	(void) cpu;

	act_machine_switch_pcb(new);
}

/*
 * The value a returning system call leaves for its caller.
 *
 * rax, because that is where the System V return register is and because
 * the syscall path restores the frame wholesale on the way out -- there is
 * no second copy of the register file to keep in step.
 */
void
thread_set_syscall_return(thread_t thread, kern_return_t retval)
{
	thread_act_t	thr_act = thread->top_act;

	assert(thr_act != THR_ACT_NULL);
	assert(thr_act->mact.pcb != PCB_NULL);
	assert(thr_act->mact.pcb->user != (struct trap_frame *) 0);

	thr_act->mact.pcb->user->rax = (uint64_t) retval;
}

kern_return_t
act_machine_get_state(thread_act_t thr_act, thread_flavor_t flavor,
		      thread_state_t state, mach_msg_type_number_t *count)
{
	pcb_t	pcb = thr_act->mact.pcb;

	if (pcb == PCB_NULL || pcb->user == (struct trap_frame *) 0)
		return KERN_INVALID_ARGUMENT;

	switch (flavor) {

	case x86_64_THREAD_STATE:
		if (*count < x86_64_THREAD_STATE_COUNT)
			return KERN_INVALID_ARGUMENT;
		thread_state_from_frame(pcb->user,
					(struct x86_64_thread_state *) state);
		*count = x86_64_THREAD_STATE_COUNT;
		return KERN_SUCCESS;

	case x86_64_FLOAT_STATE:
		if (*count < x86_64_FLOAT_STATE_COUNT)
			return KERN_INVALID_ARGUMENT;
		float_state_from_area(pcb->ctx.fpu_area,
				      (struct x86_64_float_state *) state);
		*count = x86_64_FLOAT_STATE_COUNT;
		return KERN_SUCCESS;

	case x86_64_EXCEPTION_STATE:
		if (*count < x86_64_EXCEPTION_STATE_COUNT)
			return KERN_INVALID_ARGUMENT;
		exception_state_from_frame(pcb->user,
					   (struct x86_64_exception_state *) state);
		*count = x86_64_EXCEPTION_STATE_COUNT;
		return KERN_SUCCESS;

	default:
		return KERN_INVALID_ARGUMENT;
	}
}

kern_return_t
act_machine_set_state(thread_act_t thr_act, thread_flavor_t flavor,
		      thread_state_t state, mach_msg_type_number_t count)
{
	pcb_t	pcb = thr_act->mact.pcb;

	if (pcb == PCB_NULL || pcb->user == (struct trap_frame *) 0)
		return KERN_INVALID_ARGUMENT;

	switch (flavor) {

	case x86_64_THREAD_STATE:
		if (count < x86_64_THREAD_STATE_COUNT)
			return KERN_INVALID_ARGUMENT;
		/*
		 * ⚠️ Refused rather than clamped when the segment bases are
		 * not ones this thread may have: thread_state_to_frame()
		 * answers whether they are, and a set_state that quietly
		 * corrected them would let a task ask for a frame it could
		 * not otherwise reach and be told it succeeded.
		 */
		if (!thread_state_to_frame((const struct x86_64_thread_state *)
					   state, pcb->user))
			return KERN_INVALID_ARGUMENT;
		return KERN_SUCCESS;

	case x86_64_FLOAT_STATE:
		if (count < x86_64_FLOAT_STATE_COUNT)
			return KERN_INVALID_ARGUMENT;
		float_state_to_area((const struct x86_64_float_state *) state,
				    pcb->ctx.fpu_area);
		return KERN_SUCCESS;

	/*
	 * The exception state is what the machine reported about a fault --
	 * the vector, the error code, the faulting address.  It is read-only
	 * by nature: writing it would not change what happened.
	 */
	case x86_64_EXCEPTION_STATE:
		return KERN_INVALID_ARGUMENT;

	default:
		return KERN_INVALID_ARGUMENT;
	}
}

/*
 * Nothing accumulates that a sweep could return.
 *
 * i386's is empty too, and here the reason is one step stronger: the only
 * per-activation machine resource is the floating-point area, and it is
 * released in thread_machine_destroy() rather than left for a collector.
 * An empty function with a reason is worth more than no function, because
 * the machine-independent side calls this and would otherwise not link.
 */
void
consider_machine_collect(void)
{
}

int
dump_act(thread_act_t thr_act)
{
	pcb_t	pcb;

	if (thr_act == THR_ACT_NULL) {
		printf("dump_act: THR_ACT_NULL\n");
		return 0;
	}

	pcb = thr_act->mact.pcb;
	printf("act %p: task %p pcb %p\n", thr_act, thr_act->task, pcb);

	if (pcb == PCB_NULL)
		return 0;

	printf("  kernel rsp 0x%lx  stack top 0x%lx  fpu %p\n",
	       (unsigned long) pcb->ctx.rsp,
	       (unsigned long) pcb->ctx.kernel_stack_top,
	       pcb->ctx.fpu_area);

	if (pcb->user == (struct trap_frame *) 0) {
		printf("  no user frame -- never left the kernel\n");
		return 0;
	}

	printf("  user rip 0x%lx  rsp 0x%lx  rflags 0x%lx  cs 0x%lx\n",
	       (unsigned long) pcb->user->rip,
	       (unsigned long) pcb->user->rsp,
	       (unsigned long) pcb->user->rflags,
	       (unsigned long) pcb->user->cs);
	return 0;
}

/*
 * ── The two that belong to a facility this machine does not have ──────
 *
 * Both exist for tasks created by kernel_task_create() -- servers linked
 * into the kernel's own address space.  x86-64 has no collocation and no RPC
 * trap (#453, <mach/x86_64/rpc.h>), so there is no such task to convert an
 * activation for and no short-circuited call to return from.
 *
 * They panic rather than doing nothing.  A silent no-op here would let a
 * caller believe an activation had been converted and continue with one that
 * had not, which is the failure this project refuses on purpose: the
 * plausible return.  If collocation ever arrives, it arrives with these.
 */
void
pcb_user_to_kernel(thread_act_t thr_act)
{
	(void) thr_act;
	panic("pcb_user_to_kernel: this machine has no kernel-loaded tasks "
	      "(#453)");
}

void
act_machine_return(kern_return_t code)
{
	panic("act_machine_return(0x%x): this machine has no RPC activation "
	      "chain to return through (#453)", code);
}

/*
 * An activation for a thread of the KERNEL's own making.
 *
 * ⚠️ The name misleads, and it misled me.  "kernel" here qualifies the thread
 * being created, not the "kernel-loaded" (collocated) task facility that
 * pcb_user_to_kernel and act_machine_return above belong to.  Every kernel
 * thread comes through here: kern/thread.c's thread_create_at() calls it
 * unconditionally, which is the first thing the boot proved when this
 * panicked on the startup thread (#458).
 *
 * So it is what i386's is -- a forwarder to the machine-independent
 * act_create() -- and there is no machine-dependent work in it on either
 * machine.  The hook exists because a machine COULD need to do something
 * here; this one does not, and the ones above genuinely refuse a facility
 * we have decided against.  Two different things that share a word.
 */
kern_return_t
act_create_kernel(task_t task, vm_offset_t stack, vm_size_t stack_size,
		  thread_act_t *out_act)
{
	return act_create(task, stack, stack_size, out_act);
}

/*
 * Somewhere for a thread's floating-point state to live (#453).
 *
 * Here and not in fpu.c because it is the machine-independent thread
 * lifecycle that needs it -- and because fpu.c is in the kernel today while
 * this file is not, so an allocator there would drag kalloc() across a
 * boundary the build has not reached yet.
 *
 * The area is not a fixed size -- fpu_area_size() answers what this
 * processor's enabled feature set needs -- so it cannot be a member of the
 * pcb without sizing that structure for the largest machine we might ever
 * run on.  It is allocated instead, once per thread, and released with it.
 *
 * ⚠️ The alignment is the reason this is a function and not a kalloc() at
 * the call site.  XSAVE and XRSTOR require their area to be 64-byte aligned
 * and raise #GP otherwise, and kalloc() promises no such thing -- its
 * buckets are powers of two, which *usually* come back aligned enough, and
 * "usually" is how a fault arrives on one machine and not another.  So the
 * request is over-sized deliberately, the pointer is aligned up by hand, and
 * the original is kept in the bytes immediately before it so that the free
 * can hand back what the allocator gave.
 */
void *fpu_area_alloc(void)
{
	vm_size_t	size = (vm_size_t) fpu_area_size();
	vm_size_t	total = size + FPU_AREA_ALIGN + sizeof(void *);
	vm_offset_t	raw = (vm_offset_t) kalloc(total);
	vm_offset_t	area;

	if (raw == 0)
		return (void *) 0;

	area = (raw + sizeof(void *) + FPU_AREA_ALIGN - 1)
	       & ~((vm_offset_t) FPU_AREA_ALIGN - 1);

	((vm_offset_t *) area)[-1] = raw;

	fpu_area_init((void *) area);
	return (void *) area;
}

void fpu_area_free(void *area)
{
	vm_offset_t	raw;
	vm_size_t	total;

	if (area == (void *) 0)
		return;

	raw   = ((vm_offset_t *) area)[-1];
	total = (vm_size_t) fpu_area_size() + FPU_AREA_ALIGN + sizeof(void *);

	kfree(raw, total);
}

/*
 * ── The switch ────────────────────────────────────────────────────────
 *
 * Everything that has to happen in the right order, and the order is not
 * negotiable:
 *
 *   1. the outgoing thread's floating-point state is saved and the
 *      incoming one's restored -- by context_switch(), eagerly, because
 *      this machine does not do lazy FPU (#408);
 *   2. the address space changes, if it is changing;
 *   3. the processor is told where an entry from ring 3 lands now, which
 *      must be true BEFORE the switch, because a system call arriving on
 *      the first instruction of the new thread reads it out of the
 *      per-CPU block before it has a stack to look anything up with;
 *   4. this thread's identity is left where the thread being resumed will
 *      look for it;
 *   5. the registers change.
 *
 * ⚠️ The answer is read AFTER the switch, and it is not `old'.  Mach's
 * interface says: the thread that was running before the caller resumed.
 * From the point of view of the code below the switch, that is whoever
 * switched *to us*, which is not known when this call is made and may be a
 * thread that does not exist yet.  Returning `old' would be right only for
 * the very first switch of each thread and wrong every time after.
 *
 * ⚠️ `continuation' is not used, and that is not an omission.  It tells the
 * machine that the outgoing thread does not need its stack kept, so a
 * machine that had something cheaper to do in that case would do it here.
 * This one does not: the stack is the thread's own page and is not returned
 * on a switch either way, and machine_kernel_stack_init() is what resets it
 * when the scheduler actually discards the contents.  Ignoring it costs a
 * saved register file that will be overwritten; noticing it would cost a
 * branch on every switch to save that.  If a measurement ever says
 * otherwise, this is the comment that says what to change.
 */
thread_t
switch_context(thread_t old, void (*continuation)(void), thread_t new)
{
	thread_act_t	old_act = old->top_act;
	thread_act_t	new_act = new->top_act;

	(void) continuation;

	assert(old_act != THR_ACT_NULL && old_act->mact.pcb != PCB_NULL);
	assert(new_act != THR_ACT_NULL && new_act->mact.pcb != PCB_NULL);

	/*
	 * The address space, if it is changing.  An activation may be
	 * borrowing another task's map, so this is asked of the activations
	 * and not of the tasks.
	 */
	if (old_act->map != new_act->map)
		PMAP_SWITCH_USER(new_act, new_act->map, cpu_number());

	/*
	 * Who this processor is running, BEFORE the switch (#459).
	 *
	 * load_context() has always done this and switch_context() did not --
	 * two functions doing the same job, one of which updated the
	 * processor's idea of its current thread and the other of which left
	 * it naming the thread being switched away from.
	 *
	 * It stayed invisible because nothing executed it: load_context()
	 * starts the FIRST thread, and a switch to a thread that has never run
	 * only happens once something takes a processor away and gives it to a
	 * new one -- which is preemption, and preemption did not work until
	 * this issue made it.
	 *
	 * ⚠️ Same shape as the defect #458 records in thread_machine_set_current
	 * below: current_thread() is cpu_data[cpu_number()].active_thread, the
	 * machine-independent array, and a thread that starts while that still
	 * names somebody else reads its own fields through the wrong pointer.
	 * The symptom was a call through a null function pointer into the low
	 * physical page -- rip 0x3, with the interrupt vector table
	 * disassembling as instructions.
	 */
	thread_machine_set_current(new);

	/* Where ring 3 lands now, and whose floating-point area is next. */
	act_machine_switch_pcb(new_act);

	/*
	 * Leave our identity for whoever we are about to become, then go.
	 * Everything after context_switch() runs in a different world: a
	 * later moment, and possibly a different processor.
	 */
	percpu()->prev_thread = (void *) old;

	context_switch(&old_act->mact.pcb->ctx, &new_act->mact.pcb->ctx);

	return (thread_t) percpu()->prev_thread;
}

/*
 * Become a thread that has never run, from a context that will not be
 * resumed -- the boot path, which is not a thread and does not need saving.
 *
 * ⚠️ Does not return, and must not: there is nowhere to return to.  The
 * caller's stack belongs to whatever was running before there were threads,
 * and the first thing the new thread does is take over the processor.
 */
void
load_context(thread_t thread)
{
	thread_act_t	act = thread->top_act;
	/*
	 * ⚠️ Zeroed, and that is not tidiness.  context_switch() reads
	 * old->fpu_area to decide whether there is outgoing state to save,
	 * and an uninitialised local would offer it whatever the stack last
	 * held -- a pointer it would then write a register file through.  The
	 * zero is the statement that there is no outgoing thread (#458).
	 */
	struct context	discard = { 0 };

	assert(act != THR_ACT_NULL && act->mact.pcb != PCB_NULL);

	thread_machine_set_current(thread);
	act_machine_switch_pcb(act);

	percpu()->prev_thread = (void *) 0;

	/*
	 * Start this processor's clock (#459).
	 *
	 * Here, and not in machine_init(), because this is the first instant
	 * at which a tick could be handled: hertz_tick() charges time to
	 * current_thread() and raises an AST against it, and until the line
	 * above there was no current thread to charge.
	 *
	 * ⚠️ i386 can arm earlier because its timer sits in a class that spl
	 * masks, so the first tick simply waits at the door until the AP drops
	 * to spllo inside the scheduler.  Here it cannot: SPLHI is 14 and the
	 * timer is class 15, left deliberately unmaskable so that a processor
	 * holding a lock never stops answering the others.  So the clock is
	 * started when it is safe rather than held back after it is started.
	 *
	 * Every processor runs this for its own first thread, which is what
	 * makes the tick per-processor: scheduler accounting on 64 processors
	 * cannot be one CPU's business, and i386's boot processor already
	 * shows what happens when it is -- it receives no LAPIC tick at all.
	 */
	clock_event_setup_cpu();
	if (!clock_event_arm_tick())
		panic("load_context: the %s clock would not arm — this "
		      "processor would run the first thread and never be "
		      "preempted (#459)", clock_event_name());

	/*
	 * A context to save into that nobody will read.  context_switch()
	 * wants somewhere to put the outgoing registers and there is no
	 * outgoing thread, so it gets a local -- which becomes unreachable
	 * the moment the switch takes effect, which is exactly right.
	 */
	/*
	 * -P: prove the clock actually preempts, before handing the processor
	 * to the first thread (#459).
	 *
	 * Here because this is the first point at which threads can be created
	 * -- task_init() and thread_init() have run, which they had not when
	 * machine_init() was called -- and because the test needs the processor
	 * for a second, which the ordinary boot does not have to give: it stops
	 * at bootstrap_create (#422) within a few milliseconds.
	 *
	 * The three threads it creates are bound to this processor and simply
	 * become runnable; the switch below still goes to `thread', and the
	 * scheduler reaches them the moment it takes the processor back -- which
	 * is exactly the property under test.
	 */
	if (boot_flag('P')) {
		thread_t reporter = preempt_test_run();

		if (reporter != THREAD_NULL) {
			thread = reporter;
			act = thread->top_act;
			thread_machine_set_current(thread);
			act_machine_switch_pcb(act);
		}
	}

	context_switch(&discard, &act->mact.pcb->ctx);

	panic("load_context: returned");
}

/*
 * Record which thread this processor is running.
 *
 * Kept in the per-CPU block rather than derived from the stack pointer, the
 * way some kernels do: the derivation needs the stack to be aligned to its
 * own size and this machine's kernel stacks are not, because they are
 * allocated by the machine-independent side.
 */
void
thread_machine_set_current(thread_t thread)
{
	/*
	 * ⚠️ TWO places, and the machine-independent one is the one that
	 * counts (#458).
	 *
	 * current_thread() is `cpu_data[cpu_number()].active_thread' --
	 * <kern/cpu_data.h>, an array the shared kernel owns.  This function
	 * used to set only the per-CPU block below, so the whole
	 * machine-independent tree asked cpu_data[] and got NULL: the first
	 * thread reached thread_continue(), read self->continuation through a
	 * null self, and called whatever the low physical page happened to
	 * hold.  Two halves that never met, again.
	 *
	 * The per-CPU copy is kept, and is not redundant: the syscall entry
	 * path reads it out of %gs before it has a stack to index an array
	 * with.  It is a cache of the line above, and this is the one place
	 * that writes either.
	 */
	cpu_data[cpu_number()].active_thread = thread;
	percpu()->active_thread = (void *) thread;
}

/*
 * Leave this processor's thread behind and run the shutdown context on it.
 *
 * ⚠️ Not implemented, and it panics rather than doing something plausible.  The
 * machine-independent side calls it when a processor is being taken out of
 * service, and what it wants is for the current thread to be abandoned
 * safely -- which needs the shutdown context to exist and needs the
 * processor to be removable from the scheduler's sets.  Neither is true
 * here yet, and a version that switched somewhere plausible would leave a
 * processor running an unowned stack.
 */
void
switch_to_shutdown_context(thread_t thread, void (*routine)(processor_t),
			   processor_t processor)
{
	(void) thread;
	(void) routine;
	(void) processor;

	panic("switch_to_shutdown_context: taking a processor out of service "
	      "is not implemented on this machine yet (#453)");
}
