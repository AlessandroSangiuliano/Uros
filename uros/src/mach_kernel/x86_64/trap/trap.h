/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * x86-64 trap and interrupt entry (#409, MD contract 4/6).
 *
 * Until this exists, every fault is a triple fault: the CPU tries to report
 * the problem, finds no descriptor to report it through, fails to report
 * that, and resets.  What reaches the serial line is silence, and the last
 * banner printed is the only clue.  An IDT turns the same event into a
 * vector, a faulting address and an instruction pointer.
 */

#ifndef _X86_64_TRAP_TRAP_H_
#define _X86_64_TRAP_TRAP_H_

#include <stdint.h>

/*
 * The architectural exception vectors.  Named because a number on a serial
 * line is a lookup, and the lookup happens at the worst possible moment.
 */
#define T_DIVIDE_ERROR		0
#define T_DEBUG			1
#define T_NMI			2
#define T_BREAKPOINT		3
#define T_OVERFLOW		4
#define T_BOUND_RANGE		5
#define T_INVALID_OPCODE	6
#define T_NO_FPU		7
#define T_DOUBLE_FAULT		8
#define T_FPU_OVERRUN		9
#define T_INVALID_TSS		10
#define T_SEGMENT_NOT_PRESENT	11
#define T_STACK_FAULT		12
#define T_GENERAL_PROTECTION	13
#define T_PAGE_FAULT		14
#define T_FPU_ERROR		16
#define T_ALIGNMENT_CHECK	17
#define T_MACHINE_CHECK		18
#define T_SIMD_ERROR		19

#define T_VECTORS		32	/* the architecture reserves 0..31 */

/*
 * And the rest of the table, which the architecture leaves to us.
 *
 * Nothing has arrived through one of these yet — interrupts have been off
 * since the first instruction — but they are where everything that is not a
 * fault comes in: device interrupts, and before those, the messages one
 * processor sends another.  A processor cannot be interrupted at a vector
 * that has no gate, so the table has to reach this far before any of it can
 * be tried.
 *
 * The assignments below are the whole plan for the space:
 *
 *   0x00..0x1F  the architectural exceptions
 *   0x40..0x4F  the legacy interrupt requests, on their I/O APIC pins
 *   0x50..0x5F  message-signalled interrupts, which have no pin (#457)
 *   0xE0        one vector kept for exercising this path by hand
 *   0xE1        the local APIC timer (#522)
 *   0xF1..0xFE  processor-to-processor messages
 *   0xFF        the local APIC's spurious vector
 *
 * 🔴 THE TIMER IS IN CLASS FOURTEEN AND THE MESSAGES ARE IN FIFTEEN, and the
 * boundary between those two classes is what SPLHI means.  A vector is
 * deferred when its class is at or below the level, so class fifteen is
 * everything splsched() must NOT stop -- a processor that stopped answering
 * its neighbours while holding a lock would be a deadlock rather than a
 * priority.  The tick was at 0xF0, inside that exemption, and it is the one
 * interrupt the machine-independent scheduler is written on the promise that
 * splsched() stops.  It cost a self-deadlock on the run queue lock (#522).
 *
 * ⚠️ The message-signalled block is the class ABOVE the pinned one, and
 * <cpu/spl.h>'s SPL_DEVICE covers both -- a level called "the device level"
 * that stopped one kind of device interrupt and not the other would be a
 * level whose name is wrong.  Sixteen of them, which is what one class holds;
 * a machine that wants more takes another class and SPL_DEVICE follows it,
 * because on this machine the vector IS the priority.
 *
 * The high end for the messages on purpose: the interrupt controller
 * prioritises by vector number, so a message that another processor is
 * waiting on outranks a device that is not.
 *
 * ⚠️ 0x40 and not the 0x20 this said until the pins were actually routed.
 * The number is arbitrary, and <cpu/ioapic.h> takes i386's for the reason that
 * two architectures picking the same arbitrary number is one fewer thing to
 * remember.  What is not arbitrary is that the plan written here and the base
 * the code uses were different numbers for as long as nobody compared them —
 * and a table of assignments is read precisely by someone who is not going to
 * go and check.
 */
#define IDT_VECTORS		256
#define T_EXTERNAL_FIRST	T_VECTORS

/*
 * One vector reserved for exercising the entry path by hand.  Two hundred
 * and twenty-four stubs were written by a macro and none of them has ever
 * run; a software interrupt through one is the cheapest way to find out
 * whether the macro was right before an asynchronous interrupt arrives and
 * the answer is a reset.
 */
#define T_PROBE_VECTOR		0xE0

/*
 * The page-fault error code, which says what was attempted rather than what
 * is wrong — the distinction that separates "not mapped" from "mapped and
 * refused", and so a missing page from a protection violation.
 */
#define PF_PRESENT		0x01	/* clear: nothing mapped there   */
#define PF_WRITE		0x02	/* set: it was a write           */
#define PF_USER			0x04	/* set: from ring 3              */
#define PF_RESERVED		0x08	/* a reserved bit was set        */
#define PF_INSTRUCTION		0x10	/* an instruction fetch          */

/*
 * What the handler is handed.  The order is the order the registers reach
 * the stack in trap/entry.S, which is why the two must be read together:
 * the CPU pushes the last five, the stub supplies the vector and — for the
 * exceptions that do not have one — a zero in place of the error code, so
 * that every vector arrives in the same shape.
 */
/* The offsets the syscall entry uses; asserted against this struct below. */
#include <trap/frame.h>

struct trap_frame {
	uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
	uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
	uint64_t vector;
	uint64_t error;
	uint64_t rip;
	uint64_t cs;
	uint64_t rflags;
	uint64_t rsp;
	uint64_t ss;
};

/*
 * Where that frame lives on a thread's kernel stack, said once (#422).
 *
 * A trap from ring 3 lands on the stack the task-state segment names, and the
 * processor aligns down to sixteen before pushing its five words -- so the
 * frame a trap builds is exactly here, at the top of the stack, and
 * act_machine_set_state() writes the first one for a thread that has never
 * trapped at the same address for the same reason.
 *
 * 🔥 Which means kernel code on that stack must start BELOW it, and it did
 * not.  context_init() started a new thread at the very top, so the trampoline,
 * thread_continue and everything they call ran through the bytes reserved for
 * the frame -- and the frame, once thread_set_state had written it, ran through
 * their return addresses.  The first thread bootstrap created returned to
 * address zero out of ast_taken(), which is a page fault in the kernel four
 * calls from anything that mentions a stack.
 *
 * Two users, one expression, because they are the same claim about the same
 * bytes: a second copy of this arithmetic is a second chance to disagree.
 */
#define	KERNEL_STACK_USER_FRAME(top)					\
	((((uint64_t) (top)) & ~15UL) - sizeof(struct trap_frame))

/*
 * The same fields as numbers, for the assembly that writes them (#474).
 *
 * The offsets live in <trap/frame.h> because the syscall entry needs them and
 * cannot see this structure.  They are checked HERE, against the structure
 * itself, because that is the only place both halves exist at once: adding a
 * field above moves the real offsets and stops the build on the line below,
 * rather than in a boot three weeks later.
 */
_Static_assert(TF_RIP    == __builtin_offsetof(struct trap_frame, rip),
	       "TF_RIP and struct trap_frame disagree");
_Static_assert(TF_CS     == __builtin_offsetof(struct trap_frame, cs),
	       "TF_CS and struct trap_frame disagree");
_Static_assert(TF_RFLAGS == __builtin_offsetof(struct trap_frame, rflags),
	       "TF_RFLAGS and struct trap_frame disagree");
_Static_assert(TF_RSP    == __builtin_offsetof(struct trap_frame, rsp),
	       "TF_RSP and struct trap_frame disagree");
_Static_assert(TF_SS     == __builtin_offsetof(struct trap_frame, ss),
	       "TF_SS and struct trap_frame disagree");

/*
 * Interrupt stack table slots, numbered as the gate field is: 1 to 7, with
 * zero meaning "whatever stack was current".
 *
 * The first three get their own stack because they are the ones most likely
 * to fire when the current stack is the problem — a double fault often *is* a
 * stack that cannot be pushed to, and an NMI can arrive in the middle of
 * anything, including that.  Handling them on the broken stack is how a
 * fault becomes a reset with nothing on the wire.
 *
 * A machine check gets one for a different reason: it can arrive at any
 * point, including inside the handler for something else.
 *
 * ── And the debug exception, which is here because of #440 ────────────
 *
 * A trap that arrives at ring 0 does *not* switch stacks: the processor
 * changes stacks on a privilege transition, or when the gate names a slot in
 * this table, and neither applies.  Everywhere else that is exactly right,
 * because a kernel trapping at ring 0 was already standing on a kernel
 * stack.
 *
 * Not in the syscall window.  SYSCALL does not switch the stack either, so
 * for the few instructions before the entry loads one, the processor is at
 * ring 0 with %rsp still holding whatever a user program left in it — and a
 * user program may leave any value at all in %rsp, including the address of
 * something the kernel cares about.  A debug exception delivered there would
 * push its frame at that address, at ring 0, through the kernel's own
 * mapping.  That is not a leak of information; it is a write.
 *
 * Which is why the three vectors that could already arrive in that window
 * had stacks and this one has to as well.  It is the same list as the one in
 * entry.S, for the same reason, and the two must not drift apart: the vector
 * that decides %gs from the base is the vector that cannot trust %rsp
 * either.
 *
 * ⚠️ An IST slot is a fixed address, so a second exception on the same slot
 * lands on top of the first one's frame.  Nothing here arms a breakpoint
 * from inside a handler, so it cannot nest today — the day something does,
 * it inherits this.
 */
#define IST_DOUBLE_FAULT	1
#define IST_NMI			2
#define IST_MACHINE_CHECK	3
#define IST_DEBUG		4
#define IST_COUNT		4

struct tss64;

/*
 * Point a TSS at the stacks its vectors will run on: IST_COUNT consecutive
 * stacks of `stack_size` bytes, the lowest starting at `block`.
 *
 * The caller provides the memory and this decides which stack belongs to
 * which slot, because that is the part that follows from the table above and
 * would otherwise have to be duplicated by every caller.  Whose memory it is
 * matters: two processors sharing one of these would be writing over each
 * other's account of the fault that brought them here.
 *
 * Must happen before the TSS is loaded, and the TSS must be loaded before
 * trap_init(): a gate naming an IST slot is a promise the CPU will keep by
 * reading the task register, and it has to have something to read.
 */
void trap_ist_setup(struct tss64 *tss, uint64_t block, uint64_t stack_size);

/*
 * Stop, and say why.
 *
 * For the conditions where carrying on would mean the caller believing
 * something that is not true — a per-CPU block that was never built, a
 * mapping that went unrecorded, an index that silently tracks nothing.
 * Returning quietly from those leaves a kernel that looks like it works and
 * is wrong somewhere else, later, for reasons that no longer point back
 * here.
 *
 * This is the bootstrap answer, not the final one: once there is a caller
 * that can do something about a failure, these become returned errors. What
 * they must not be in the meantime is invisible.
 *
 * #415: variadic, and not by taste.  kern/misc_protos.h declares
 * `void panic(const char *string, ...)` and the MI tree calls it that way in
 * a hundred and thirty-five places.  A non-variadic definition behind that
 * declaration is undefined behaviour on the SysV ABI, and the concrete cost
 * is that "%x" arrives as four characters instead of the value it was asked
 * to print -- at the one moment when the value is the entire message.
 *
 * noreturn is deliberately NOT repeated here: the MI declaration does not
 * carry it, and two declarations that differ are what this issue is about.
 * The loop at the end of the definition says the same thing to the compiler.
 */
void panic(const char *what, ...) __attribute__((format(printf, 1, 2)));

/* Install the IDT on this CPU. */
void trap_init(void);

/* Entry from the assembly stubs. */
void trap_dispatch(struct trap_frame *frame);

/*
 * Entry from the four stubs that cannot decide from the saved code segment
 * (#440): #DB, NMI, #DF and #MC.  See trap/entry.S for why those four and no
 * others, and for how the decision is made.
 *
 * The two extra arguments are the evidence, passed along rather than
 * recomputed: `gs_on_entry` is the base the vector arrived with, before the
 * entry did anything about it, and `swapped` is what the entry decided.
 * Neither can be recovered afterwards — by the time C runs the base is the
 * kernel's whichever way the decision went, which is precisely the property
 * that makes it correct and also the reason it has to be carried.
 */
void trap_dispatch_paranoid(struct trap_frame *frame, uint64_t gs_on_entry,
			    uint64_t swapped);

/*
 * What a vector is called.  Exported because a number on a serial line is a
 * lookup performed at the worst possible moment, and the tests report faults
 * they provoked on purpose as often as the handler reports the others.
 */
const char *trap_name(uint64_t vector);

/*
 * Claim one of the vectors above T_EXTERNAL_FIRST.
 *
 * Only those: an exception vector already means something the kernel has to
 * decide about, and letting it be overwritten would turn a diagnosis into
 * whatever the last caller installed.
 *
 * The table is written once, by the boot processor, before any processor is
 * in a position to take an interrupt — so it is shared without a lock and
 * must stay that way, which is what the check in trap_set_handler() is for.
 */
typedef void (*trap_handler_t)(struct trap_frame *frame);

void trap_set_handler(unsigned vector, trap_handler_t handler);

/*
 * Run the handler for `vector` as though it had just arrived.
 *
 * For the priority level (<cpu/spl.h>) to replay what it deferred.
 *
 * ⚠️ The frame it is handed has **only `vector` valid**. The interrupt this
 * stands for happened at an earlier moment whose registers are gone, and
 * they are not reconstructed — the remaining fields are zero, which is not a
 * plausible frame and is not meant to be one.
 *
 * That is a contract on handlers, not a detail: a handler that reads
 * anything else from the frame must not be reachable through a deferral. The
 * one handler that reads anything today reads exactly `vector`, which is
 * why this is the shape it is.
 */
void trap_replay_vector(unsigned vector);

/*
 * Whether this processor is inside a replayed handler (#457).
 *
 * 🔑 Asked by a handler that acknowledges its own interrupt, because on the
 * replay path the acknowledgement has already been given -- trap_dispatch()
 * issues it when it defers, so that the local APIC does not hold the class
 * busy for the length of the deferral.  Giving it a second time is not a
 * harmless repeat: EOI clears the highest bit in service at that moment,
 * which during a replay is some other interrupt, dismissed before it is done.
 *
 * ⚠️ This is the second half of the contract in <cpu/spl.h>.  The first is
 * that a replayed handler may read only `vector' from its frame; this is that
 * it may not acknowledge.  Both exist because the interrupt happened at an
 * earlier moment that is over.
 */
int trap_in_replay(void);

/*
 * Arrange for one expected fault to be survived rather than reported and
 * halted on: if the next trap is `vector`, the handler rewrites the return
 * address to `resume_rip` and returns, so execution continues there instead
 * of at the instruction that faulted.
 *
 * The kernel already needs this shape of thing on i386, where copyin and
 * copyout recover from faults on user addresses rather than dying with them
 * (i386/trap.c). This is the same idea at its smallest: one armed
 * expectation, cleared as soon as it fires, with none of the address-range
 * table that the real mechanism will want.
 *
 * It is also what lets a deliberate fault be a test instead of the end of
 * the boot — a protection can only be shown to hold by provoking it.
 */
void trap_expect(uint64_t vector, uint64_t resume_rip);

/*
 * Resume at the instruction the trap arrived on, rather than somewhere else.
 *
 * For the faults trap_expect() was written for, resuming where they happened
 * would repeat them forever, so a different address is the only useful
 * answer.  For the ones that are not the instruction's fault — a debug
 * exception armed from outside, an interrupt that is not maskable — the
 * instruction has not run yet or has run correctly, and continuing is what
 * the handler is supposed to do.  Zero is not an address a resume could
 * legitimately be, which is what lets it mean this.
 */
#define TRAP_RESUME_HERE	0UL

/*
 * What the last expected trap turned out to be.  Reporting a fault is one
 * thing; being able to check that the frame carried the right vector, the
 * right error code and a plausible instruction pointer is what tells us the
 * entry path built it correctly — which for thirty-two hand-written stubs is
 * not something to assume.
 */
struct trap_record {
	uint64_t vector;
	uint64_t error;
	uint64_t rip;
	uint64_t cr2;
	uint64_t cs;		/* its low two bits are the ring it came from */
	uint64_t gs_base;	/* which block %gs reached the handler with  */
	/*
	 * And the two the fault report gets wrong most invisibly (#409).
	 *
	 * `rsp` is the interrupted stack pointer, which the report prints and
	 * nothing has ever checked; `frame` is where the frame was built, which
	 * is not in the frame at all and is the only evidence of whether a gate
	 * naming an IST slot actually switched stacks.
	 *
	 * They are here because #458 spent two hours reconciling an rsp that
	 * could not be right: a report is the primary instrument for every
	 * fault on this target and is believed, so the numbers in it have to be
	 * checkable against something the test knows independently.
	 */
	uint64_t rsp;
	uint64_t frame;
	uint64_t rbp;		/* what a backtrace has to be walked from     */
	int      caught;
};

const struct trap_record *trap_last(void);

/*
 * What the last vector that took the paranoid entry found there (#440).
 *
 * A record of its own rather than two more fields on the one above, because
 * only one of the two paths can fill them in.  A field that is meaningful
 * after some traps and stale after the others is worse than no field: it
 * reads the same either way, and the reader has no way to ask which it got.
 *
 * `cs` is here because it is the evidence *against* the rule this path
 * replaces — a window trap carries a ring-0 code segment, so recording it
 * alongside a user base is what makes the two disagree in the record rather
 * than only in the argument.
 */
struct trap_paranoid_record {
	uint64_t vector;
	uint64_t rip;			/* and where in the window it landed  */
	uint64_t cs;			/* what the ring rule would have read */
	uint64_t gs_on_entry;		/* the base the window left loaded    */
	uint64_t gs_on_dispatch;	/* and the one the handler ran with   */
	uint64_t swapped;		/* whether the entry exchanged them   */
	int      taken;
};

const struct trap_paranoid_record *trap_last_paranoid(void);

/* Forget the last one, so that "it happened" can be distinguished from "it
 * happened earlier".  A test that only checks the contents of this record
 * would pass on a stale one from a previous experiment. */
void trap_paranoid_forget(void);

/*
 * How many traps arrived with SMAP lifted, and whether the last such trap was
 * still lifted after the dispatcher had done what it does about it (#468).
 *
 * Two numbers rather than a verdict because they answer different halves of
 * the same question, and a test needs both: the count says the observation
 * can see the condition at all, and the second says it was closed.  A test
 * that only checked the second would pass on a machine where the window
 * never opened.
 */
uint64_t trap_smap_lifted_count(void);
uint64_t trap_smap_after_last(void);

/*
 * Arrange for the next fault *from ring 3* to return to the kernel instead
 * of being reported and halted on.
 *
 * A separate arrangement from trap_expect(), because the recovery is a
 * different operation.  That one rewrites the instruction pointer and lets
 * the return continue on the same stack at the same privilege; there is no
 * such stack here.  A fault from ring 3 arrives on the task-state segment's
 * stack, and the frame the processor pushed describes a user context in
 * every field — code segment, stack segment, stack pointer, flags.  Coming
 * back means rewriting all of them, which is the same thing the real user
 * fault path will do once there is a thread to return *to* rather than a
 * saved kernel frame.
 *
 * Any vector, deliberately: what makes a fault recoverable here is where it
 * came from, not what it was.  A kernel that only expected the one it meant
 * to provoke would report the others from a context it cannot resume.
 */
void trap_expect_user(uint64_t resume_rip, uint64_t resume_rsp);

/*
 * Store TRAP_PROBE_PATTERN at `addr` and report whether the machine allowed
 * it: 0 if the store completed, 1 if it faulted.  Checking for the pattern
 * afterwards distinguishes a store that happened from one that was never
 * needed, which a zero could not.  Arm trap_expect() with
 * trap_probe_faulted first — this is the store, not the arrangement to
 * survive it.
 *
 * The pair lives in trap/entry.S, where the resume point can be a symbol
 * rather than a label the compiler is entitled to optimise away.
 */
#define TRAP_PROBE_PATTERN	0x5a5a5a5a

int trap_probe_write(volatile void *addr);

/*
 * One probe per exception family, each returning 0 if the instruction
 * completed and 1 if the handler stepped over it.  Between them they cover
 * both sides of the error-code asymmetry the stubs exist to hide: the first
 * three arrive without one and must be given a zero, while a general
 * protection arrives with the offending selector and must keep it.
 */
int trap_probe_ud(void);	/* invalid opcode      */
int trap_probe_bp(void);	/* breakpoint          */
int trap_probe_de(void);	/* divide error        */
int trap_probe_gp(void);	/* general protection  */

extern char trap_probe_faulted[];

/*
 * Print the call stack above a frame pointer, symbolised, straight to the
 * console.
 *
 * Not machine_callstack(), which fills an array for a caller that will do
 * something with it later: this prints, now, on the assumption that there may
 * be no later.  That is why halt_cpu() uses it on the panic path (#453).
 */
void	x86_64_backtrace(uint64_t rbp);

/*
 * The same walk, answered rather than printed, so a backtrace can be CHECKED
 * (#409).
 *
 * Returns how many frames were reached; `first` comes back with the name just
 * above the interrupted code, which says the entry did not lose the frame
 * pointer, and `found` says whether `want` appeared anywhere in the chain,
 * which is what separates a whole chain from a merely non-empty one — a walk
 * that breaks in the middle still returns frames and they still have names.
 *
 * Any of the three may be null, and `first` may come back null when no symbol
 * covers that return address.
 */
unsigned x86_64_backtrace_probe(uint64_t rbp, const char **first,
				const char *want, int *found);

#endif	/* _X86_64_TRAP_TRAP_H_ */
