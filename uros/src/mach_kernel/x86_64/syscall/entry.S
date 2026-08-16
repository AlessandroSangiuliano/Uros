/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The syscall entry path (#411, MD contract 6/6).
 *
 * The register contract this implements is written down in
 * <syscall/syscall.h> and is the thing to read first; what follows is how
 * few instructions it takes to honour it.
 *
 * Every Mach trap and every POSIX call in the system passes through these
 * instructions, so each one of them is paid for by everything above.
 */

#include <cpu/desc.h>
#include <cpu/percpu.h>
#include <syscall/syscall.h>
#include <trap/frame.h>	/* #474: where the saved user frame's fields are */

/*
 * Taking the second half of #475 back out, for one run (#475).
 *
 * The other half is in x86_64/thread/machine.c and has its own switch; they
 * are separate because they fail differently and a run that removed both
 * would only ever report the first thing to go wrong.  Zero in every build
 * that ships.
 */
#ifndef ABLATE_475_AST
#define ABLATE_475_AST	0	/* a syscall return runs its AST handlers */
#endif

	.section .text
	.code64

/*
 * Arguments seven and up, put where a C function with that many looks for
 * them (#426).
 *
 * They arrive in rbx and r12-r15 — see the contract in <syscall/syscall.h>
 * for why registers and not the user's stack — and the C ABI wants them in
 * memory just below the return address, seventh nearest.  So they are
 * pushed in reverse, and the count is a run-time value, which is why this
 * is a descending chain of compares rather than a loop: five pushes at
 * most, no induction variable, and no register to hold one in.
 *
 * ⚠️ The padding is not decoration.  The ABI wants rsp sixteen-byte aligned
 * at the call, the entry has pushed an odd number of words by the time it
 * gets here (four, plus rbp — it was two before #473 added the user stack
 * pointer and the word that keeps the count even, and what matters here is
 * the parity, which has not moved), so an EVEN number of arguments needs one
 * word of padding and an odd number needs none.  Getting this backwards does
 * not fail: it misaligns every stack frame under the trap, which the kernel
 * survives only because it is compiled -mno-sse and nothing executes an
 * aligned vector move.  It would fail the day that changed, somewhere else.
 *
 * Expects: r11d = how many, 1..5.  Clobbers nothing.
 */
	.macro	PUSH_WIDE_ARGS
	testb	$1, %r11b
	jnz	1f
	pushq	$0
1:
	cmpl	$5, %r11d
	jb	2f
	pushq	%r15			/* argument eleven */
2:	cmpl	$4, %r11d
	jb	3f
	pushq	%r14			/* ten   */
3:	cmpl	$3, %r11d
	jb	4f
	pushq	%r13			/* nine  */
4:	cmpl	$2, %r11d
	jb	5f
	pushq	%r12			/* eight */
5:	pushq	%rbx			/* seven, nearest the top */
	.endm

/*
 * Entry from ring 3.  The processor has already done, in the instruction
 * itself and without touching memory:
 *
 *   rcx = the return address        r11 = the caller's flags
 *   rflags &= ~FMASK                cs, ss = from STAR
 *
 * and nothing else.  In particular it has *not* switched the stack — unlike
 * a trap gate, which takes one from the task-state segment.  So the stack
 * pointer still belongs to a user program, and everything below has to
 * happen before anything is pushed.
 *
 * That absence is a saving, not a burden: it is precisely the per-switch
 * write of IA32_SYSENTER_ESP that #348 built a per-CPU trampoline on i386
 * to avoid, measured there at about 144 cycles. There is no such register
 * here and nothing to keep up to date, so that machinery is not ported —
 * the cost it removed does not exist on this architecture.
 *
 * Aligned to a cache line because the first instruction of a syscall is
 * fetched cold more often than almost anything else in the kernel.
 */
	.globl	syscall_entry
	.align	64
syscall_entry:
	/*
	 * %gs becomes the kernel's.  Until this instruction retires there is
	 * no way to reach per-CPU state at all, which is why it is first and
	 * why nothing above it may fault.
	 */
	swapgs

	movq	%rsp, %gs:PERCPU_USER_RSP
	movq	%gs:PERCPU_KERNEL_RSP, %rsp

	/*
	 * 🔥 WRITTEN, not pushed, and into the frame this thread already owns
	 * (#474).
	 *
	 * %rsp now holds PERCPU_KERNEL_RSP, and that value is the BASE of the
	 * trap frame reserved at the top of this thread's kernel stack -- the
	 * one pcb->user points at, the one <trap/trap.h> reserves and
	 * KERNEL_STACK_USER_FRAME() names.  It used to be the stack TOP, so
	 * every push below and every C frame after it ran straight down through
	 * those 176 bytes: the saved user frame and the syscall's working stack
	 * were the same memory.
	 *
	 * What that cost was not theoretical.  thread_get_state() on a thread
	 * asleep in mach_msg -- which is where a server thread spends its life,
	 * and exactly the thread a debugger stops -- answered out of pcb->user
	 * and therefore out of the kernel's own stack, reporting KERN_SUCCESS.
	 * Measured, before this changed: `cs 0x0 ss 0x7ffffff8fd38 rip
	 * 0xffffffff8011ba19'.  The fields read one slot out of step, because
	 * the four pushes landed on ss/rsp/rflags/cs in that order -- and the
	 * rip a user program got back was a KERNEL TEXT ADDRESS.  A disclosure
	 * through a call that succeeds, not merely a wrong answer.
	 *
	 * So the five words a return needs go into the frame, at their own
	 * offsets, and the frame then describes the caller for as long as the
	 * thread is inside the kernel.  It is the same number of stores as the
	 * pushes were, in the same place on the same cache line, and it lands
	 * somewhere a reader can make sense of.
	 *
	 * ⚠️ And they are per-THREAD by construction, which is what #473 had to
	 * arrange by hand: the frame is on this thread's stack, so mach_msg can
	 * block, the scheduler can run somebody else, and no other entry can
	 * write these words.  The per-CPU slot keeps only its job as the scratch
	 * across the two instructions above, where it is safe for a reason
	 * rather than by luck -- SYSCALL masks interrupts through FMASK, so
	 * nothing runs between the store and the load.
	 *
	 * ⚠️ Order matters in one place only: %rcx carries the return address
	 * and is then reused, so TF_RIP is written before anything else touches
	 * it.
	 */
	movq	%rcx, TF_RIP(%rsp)		/* where ring 3 resumes    */
	movq	%r11, TF_RFLAGS(%rsp)		/* the flags it resumes on */
	movq	%gs:PERCPU_USER_RSP, %rcx
	movq	%rcx, TF_RSP(%rsp)		/* the stack it came in on */
	movq	$USER_CS_RPL3, TF_CS(%rsp)
	movq	$USER_DS_RPL3, TF_SS(%rsp)

	/*
	 * ⚠️ The fifteen general-purpose registers above are NOT written, and
	 * that is the contract rather than an omission: <syscall/syscall.h>
	 * declares them destroyed, and storing them on every call in the system
	 * to preserve values the caller has already written off would be the
	 * most expensive thing this kernel does.
	 *
	 * What is left in them is the frame's PREVIOUS contents -- what
	 * thread_frame_init() zeroed, what thread_set_state() wrote, or the
	 * ring-3 registers the thread's last trap saved.  Never kernel stack:
	 * these bytes stopped being working stack the moment kernel_rsp moved
	 * below them, which is the whole of this fix.
	 *
	 * ⚠️ A mark saying "a syscall wrote this" lived here briefly, so that
	 * readers could blank those fifteen.  It came out: with the stack change
	 * in place the ablation showed the blanking changed nothing, because
	 * there is no longer a path that puts kernel memory there.  A store on
	 * this path, for a value no reader needed, is exactly what this file
	 * spends its comments refusing.
	 */

	/*
	 * ⚠️ NOTHING is pushed, and the alignment that used to need a fourth
	 * word now comes for free.
	 *
	 * The System V ABI wants %rsp sixteen-byte aligned AT the call, so that
	 * the callee sees it eight past a boundary once the return address is
	 * on.  The frame base is sixteen-byte aligned -- kernel_stack_top is,
	 * and sizeof(struct trap_frame) is a multiple of sixteen -- so the
	 * calls below are legal with zero pushes, and zero is even.
	 *
	 * ⚠️ The two wide paths lean on that parity: they push %rbp and an odd
	 * number of stacked arguments on top of this, and land their callee on
	 * the boundary only because what is under them is even.  It still is.
	 */

	/*
	 * The fourth argument moves into the register the C ABI expects.  The
	 * instruction took rcx for the return address, which is why the
	 * caller was asked for r10 instead — one move, here, rather than a
	 * different convention for every call.
	 */
	movq	%r10, %rcx

	/*
	 * 🔥 Interrupts ON, and not before here (#411).
	 *
	 * SYSCALL clears IF through FMASK, which is a saving on the way in --
	 * no CLI to execute -- and becomes a trap if nothing turns it back on:
	 * the whole call then runs with interrupts disabled.  Anything that
	 * blocks cannot be woken, and anything that needs the other processors
	 * deadlocks outright.  Found as `ipi: a cross-call with interrupts off
	 * would deadlock', from a page fault inside a copyin that wanted a TLB
	 * shootdown.
	 *
	 * ⚠️ Here and not earlier.  Above this point the kernel is not yet
	 * standing on its own stack or its own %gs, and an interrupt arriving
	 * in that window would push a frame at an address ring 3 chose.  The
	 * swapgs and the stack switch come first for exactly that reason, and
	 * this has to come after both.
	 */
	sti

	/*
	 * An index from ring 3 into a table of function pointers, checked.
	 * Unsigned, so one comparison covers both ends: a number above the
	 * table and a number that looked negative are the same test.
	 *
	 * This is the boundary. Everything before it was arranged by the
	 * processor; %rax is the first value in the kernel that a user chose.
	 */
	/*
	 * Mach traps live above SYSCALL_MACH_BASE (#411).  Taken first because
	 * it is the branch that will carry the traffic: every message in the
	 * system comes through here, and the kernel's own calls are a handful.
	 *
	 * ⚠️ The subtraction happens AFTER the range test, not before.  Doing it
	 * first and testing the result would be arithmetic on a number a user
	 * chose, and a number below the base would wrap to an enormous one --
	 * still caught by the unsigned bound below, but caught by accident
	 * rather than by the test that is written.
	 */
	cmpq	$SYSCALL_MACH_BASE, %rax
	jb	.Lown_call

	subq	$SYSCALL_MACH_BASE, %rax
	cmpq	mach_syscall_count(%rip), %rax
	jae	.Lbad_call

	/*
	 * Does this trap have arguments beyond the six in the C ABI's
	 * registers?  All but sixteen do not (#426).
	 *
	 * ⚠️ This load is on the hot path, and it is here rather than folded
	 * into the pointer table on purpose.  The alternative — pointing the
	 * table at a thunk for the wide traps — costs nothing here and a call
	 * and a return THERE, and `there' is mach_msg: the widest trap in the
	 * table is also the one every message in the system goes through.
	 * Four instructions for everybody beats a call for the hot path.
	 */
	leaq	mach_syscall_stack(%rip), %r11
	movzbl	(%r11, %rax), %r11d
	testl	%r11d, %r11d
	jnz	.Lmach_wide

	leaq	mach_syscall_table(%rip), %r11
	callq	*(%r11, %rax, 8)
	jmp	.Lreturn

.Lown_call:
	cmpq	$SYSCALL_NR_MAX, %rax
	jae	.Lbad_call

	leaq	syscall_stack(%rip), %r11
	movzbl	(%r11, %rax), %r11d
	testl	%r11d, %r11d
	jnz	.Lown_wide

	leaq	syscall_table(%rip), %r11
	callq	*(%r11, %rax, 8)

.Lreturn:
	/*
	 * 🔥 Pending ASTs FIRST, and this path did not have the check (#475).
	 *
	 * i386 gets it for nothing: its system call return joins
	 * return_from_trap, whose first act is the AST check.  Here SYSCALL has
	 * its own way out and the check was in the OTHER one -- trap/entry.S
	 * tests it on every trap return and on the three thread-return entry
	 * points -- so the whole of it was missing from the road every message
	 * in the system travels.
	 *
	 * What that costs is not a missed opportunity, it is a promise the
	 * kernel breaks.  AST_APC is how kern/thread_act.c stops a thread:
	 * thread_terminate, thread_abort and thread_suspend all end in
	 * install_special_handler() and nudge(), and nudge()'s own comment says
	 * it "ensures that the activation will execute its returnhandlers
	 * before it next executes any of its USER-level code."  A thread that
	 * leaves through here executed its user-level code with the handler
	 * still installed, so:
	 *
	 *   - thread_abort left TH_ABORT set for ever, because clearing it is
	 *     the first thing special_handler() does.  The thread's NEXT wait
	 *     then returned THREAD_INTERRUPTED immediately, and the one after
	 *     that, and so on -- one abort poisoning every subsequent sleep.
	 *   - thread_suspend never suspended anything: the parking is done BY
	 *     the handler, so the thread ran on while its suspender's
	 *     thread_wait() believed it had stopped.
	 *
	 * Both are shown by act_test (#475), each with its own ablation.
	 *
	 * ⚠️ What it looked like from far away is worth recording, because it
	 * is why the cause took a while to name.  OBSERVED: after this program
	 * aborted one worker thread, a later pthread_create() returned success
	 * and the thread it promised never executed an instruction.  INFERRED,
	 * from reading lib/libpthreads/pthread.c rather than from measuring it:
	 * pthread_create() recycles parked workers, a parked worker waits on a
	 * futex, and a thread carrying a stale TH_ABORT cannot stay in a wait
	 * -- so the pool would hand out a thread that was counted as parked and
	 * was in fact asleep somewhere else.  Nothing in that sentence mentions
	 * an AST, which is the point.
	 *
	 * ⚠️ A call rather than a test-and-branch, deliberately, on the hottest
	 * path in the kernel.  thread_return_ast() opens by reading
	 * need_ast[cpu_number()] and returning, so the fast case is a call, a
	 * load through %gs, an indexed load and a return.  Spelling that test
	 * here in assembly would save the call and would put a second copy of
	 * need_ast's type and indexing in a file the C declaration cannot
	 * check: ast_t is an int TODAY.  If this ever measures, it becomes an
	 * inline guard with a number behind it, not before.
	 *
	 * ⚠️ %rax is the trap's answer and thread_return_ast() is entitled to
	 * destroy it.  Sixteen bytes rather than eight so the stack stays
	 * aligned for the call, which is the same arithmetic the entry path's
	 * fourth push exists for.
	 */
#if	!ABLATE_475_AST
	subq	$16, %rsp
	movq	%rax, (%rsp)
	callq	thread_return_ast
	movq	(%rsp), %rax
	addq	$16, %rsp
#endif

	/*
	 * Read back out of the frame the entry filled (#474), rather than
	 * popped off a working stack that no longer holds them.  %rsp is the
	 * frame base again here: the plain paths never moved it, and the wide
	 * paths restore it from %rbp before jumping here.
	 */
	movq	TF_RFLAGS(%rsp), %r11	/* flags */
	movq	TF_RIP(%rsp), %rcx	/* resume address */

	/*
	 * SYSRET takes the return address from rcx, and on Intel parts it
	 * faults if that address is not canonical — after switching to the
	 * user's stack pointer and before leaving ring 0.  A general
	 * protection fault at kernel privilege, on a stack the caller chose,
	 * with an address the caller chose: that is CVE-2012-0217, and it is
	 * a privilege escalation rather than a crash.
	 *
	 * The address is checked instead, which is three instructions and a
	 * branch never taken. Canonical here means bit 47 sign-extended,
	 * because the tables are four levels deep; five-level paging moves
	 * the bit to 56 and this shift with it (#405 keeps that door open).
	 *
	 * r10 is scratch: the contract already declares it destroyed.
	 */
	movq	%rcx, %r10
	shlq	$16, %r10
	sarq	$16, %r10
	cmpq	%rcx, %r10
	jne	.Lreturn_the_slow_way

	/*
	 * ⚠️ And off again before the way out begins.  From here to SYSRET the
	 * stack pointer and %gs belong to ring 3 for a few instructions, and an
	 * interrupt in that window is the one #440 exists about.
	 */
	cli
	movq	TF_RSP(%rsp), %rsp	/* this thread's, not this processor's */
	swapgs
	sysretq

/*
 * A call number the table does not have.  Answering with a value would be
 * answering, and a caller cannot tell -1 from a syscall that returned -1 —
 * so this is the one case where the number itself is the answer, and the
 * convention has to be stated somewhere it cannot be missed.
 *
 * It is stated in <syscall/syscall.h> with the rest of the contract, and it
 * is the same value POSIX has always used for this.
 */
.Lbad_call:
	movq	$SYSCALL_ENOSYS, %rax	/* and nothing that looks plausible */
	jmp	.Lreturn

/*
 * The two calls that are too wide for the argument registers alone.
 *
 * rbp anchors the stack because the count is not knowable again after the
 * call: rax comes back holding a result, and recomputing the count from the
 * index would mean keeping the index somewhere, which is the same problem
 * one register further along.  The C function preserves rbp, so restoring
 * from it is one instruction and holds across everything the trap does —
 * including blocking, being switched away from, and coming back on another
 * processor, which is precisely what a per-CPU scratch slot would NOT have
 * survived.
 *
 * The user's rbp is saved by the push and restored by the pop, so it stays
 * preserved exactly as the contract says.
 */
.Lmach_wide:
	pushq	%rbp
	movq	%rsp, %rbp
	PUSH_WIDE_ARGS
	leaq	mach_syscall_table(%rip), %r11
	callq	*(%r11, %rax, 8)
	movq	%rbp, %rsp
	popq	%rbp
	jmp	.Lreturn

.Lown_wide:
	pushq	%rbp
	movq	%rsp, %rbp
	PUSH_WIDE_ARGS
	leaq	syscall_table(%rip), %r11
	callq	*(%r11, %rax, 8)
	movq	%rbp, %rsp
	popq	%rbp
	jmp	.Lreturn

/*
 * The return that cannot use SYSRET.
 *
 * IRETQ costs several times what SYSRET does, which is exactly why it is
 * here and not above: it is correct for every case, and reaching for it
 * always would make every syscall in the system pay for a case that only
 * arises when a user program has already done something it had no reason
 * to do.
 *
 * The frame is built by hand from what the entry saved, in the order IRETQ
 * pops it.
 *
 * ⚠️ The stack pointer comes off this thread's stack, not out of the per-CPU
 * block (#473).  .Lreturn has already popped the flags and the resume
 * address, so it is what is under %rsp now; the iretq frame is then built
 * below it, on the kernel stack, which is why the pop has to come first.
 */
.Lreturn_the_slow_way:
	cli
	movq	TF_RSP(%rsp), %r10	/* the stack ring 3 came in on (#474) */
	swapgs

	/*
	 * ⚠️ The five words below are pushed BELOW the frame, not into it: %rsp
	 * is the frame's base, so these land in the working stack under it and
	 * leave the saved user frame exactly as the entry wrote it.  Which is
	 * what iretq wants anyway -- it consumes them from where they are.
	 */

	pushq	$USER_DS_RPL3		/* ss     */
	pushq	%r10			/* rsp    */
	pushq	%r11			/* rflags */
	pushq	$USER_CS_RPL3		/* cs     */
	pushq	%rcx			/* rip    */
	iretq
