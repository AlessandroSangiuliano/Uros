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
 * ⚠️ switch_context() is deliberately NOT here.  Mach's version returns the
 * thread that was running *before the caller resumed*, which is not the one
 * it was handed and cannot be produced by a switch that returns void.  That
 * convention has to be built into the switch itself rather than wrapped
 * around it, so it gets its own change instead of riding along with
 * seventeen straightforward ones.
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
kern_return_t
thread_machine_create(thread_t thread, thread_act_t thr_act,
		      void (*start_pos)(void))
{
	pcb_t	pcb = thr_act->mact.pcb;

	if (pcb == PCB_NULL)
		return KERN_FAILURE;

	if (thread->kernel_stack == 0)
		return KERN_RESOURCE_SHORTAGE;

	context_init(&pcb->ctx,
		     (uint64_t) thread->kernel_stack + KERNEL_STACK_SIZE,
		     (void (*)(void *)) start_pos, (void *) 0,
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
 * An activation for a kernel-loaded task, which is the same facility again.
 * kern/thread.c calls it only on that path.
 */
kern_return_t
act_create_kernel(task_t task, vm_offset_t stack, vm_size_t stack_size,
		  thread_act_t *out_act)
{
	(void) task;
	(void) stack;
	(void) stack_size;
	(void) out_act;

	panic("act_create_kernel: this machine has no kernel-loaded tasks "
	      "(#453)");
	return KERN_FAILURE;
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
