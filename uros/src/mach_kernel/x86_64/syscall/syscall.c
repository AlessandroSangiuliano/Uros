/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The syscall path (#411, MD contract 6/6).
 */

#include <stdint.h>

#include <cpu/desc.h>
#include <cpu/percpu.h>
#include <cpu/regs.h>
#include <mach/kern_return.h>
#include <mach/boolean.h>
#include <kern/syscall_sw.h>	/* #411: mach_trap_table */
#include <syscall/syscall.h>
#include <kern/misc_protos.h>	/* #422: printf */
#include <trap/trap.h>

#define MSR_STAR	0xC0000081
#define MSR_LSTAR	0xC0000082
#define MSR_FMASK	0xC0000084

/*
 * The flags SYSCALL clears on the way in.
 *
 * This register is worth more than it looks: every bit named here is an
 * instruction the entry path does not have to execute, on every call in the
 * system, and two of them are correctness rather than speed.
 *
 *   IF   no CLI — the kernel is entered with interrupts already off
 *   DF   no CLD — and the C ABI *requires* the direction flag clear on
 *        entry to a function, so a kernel that did not clear it would be
 *        one string instruction away from a user program deciding which
 *        way its memcpy runs
 *   AC   user code can set this to switch SMAP off for itself; clearing it
 *        here is what stops a syscall from carrying that permission into
 *        the kernel
 *   TF   otherwise the first kernel instruction single-steps into a debug
 *        exception chosen by the caller
 *   NT   stale nested-task state has no business surviving into ring 0
 */
#define RFLAGS_TF	(1UL << 8)
#define RFLAGS_DF	(1UL << 10)
#define RFLAGS_NT	(1UL << 14)
#define RFLAGS_AC	(1UL << 18)

#define SYSCALL_FMASK	(RFLAGS_IF | RFLAGS_DF | RFLAGS_AC | RFLAGS_TF | RFLAGS_NT)


/*
 * Which per-CPU block the last probe call was reached with.
 *
 * Recorded because the entry path's swapgs is otherwise unobservable from
 * outside it: the instruction leaves no trace except which block the kernel
 * can see afterwards, and that is only a distinguishing answer when the two
 * bases differ.
 */
static uint64_t probe_gs_base;

uint64_t syscall_probe_gs(void)
{
	return probe_gs_base;
}

/*
 * What the entry left on the kernel stack, read back from the handler (#409).
 *
 * ── Why this is the whole of a syscall's "frame" ──────────────────────
 *
 * The other five entries build a struct trap_frame and the check is that its
 * fields describe the interrupted code.  A syscall has no such structure and
 * must not grow one: the contract in <syscall/syscall.h> declares the argument
 * registers destroyed precisely so the entry does not save them, and that
 * absence is measured in cycles on every call in the system.
 *
 * What it does save is two words, and they are the two the return cannot do
 * without: where ring 3 resumes, and the flags it resumes with.  So they are
 * the frame, and reading them back from the stack the entry chose is the only
 * way to see BOTH that the stack switch happened and that the right things
 * went onto it — if `movq %gs:PERCPU_KERNEL_RSP, %rsp' had not run, the pushes
 * would have landed on the user's stack and these two addresses would hold
 * whatever was there.
 */
static uint64_t probe_saved_rip;
static uint64_t probe_saved_flags;
static uint64_t probe_kernel_rsp;
static unsigned probe_depth;
static const char *probe_top;
static int probe_reached_entry;

/*
 * The backtrace from inside a syscall, taken WHERE IT IS TRUE (#409).
 *
 * ⚠️ Recorded as a result and not as a frame pointer to walk later, and that
 * distinction cost a run: the first version handed the caller this function's
 * %rbp, and by the time anything walked it the kernel stack had been reused by
 * every trap since.  The walk came back empty and looked like a syscall entry
 * with no chain at all.  A backtrace is a live thing; the address of a dead
 * frame is not one.
 *
 * ⚠️ And the answer is short on purpose.  The entry keeps no frame pointer —
 * it pushes two words and calls — so the chain runs through this function to
 * `syscall_entry' and STOPS: below that is the register ring 3 was holding,
 * which is not the kernel's to walk.  That is the right answer rather than a
 * missing one.  Establishing a frame in the entry would put instructions on
 * the one path every call in the system takes, to add a frame at a boundary
 * where there is nothing further to say.
 */
unsigned syscall_probe_depth(void)
{
	return probe_depth;
}

const char *syscall_probe_top(void)
{
	return probe_top;
}

int syscall_probe_reached_entry(void)
{
	return probe_reached_entry;
}

uint64_t syscall_probe_saved_rip(void)
{
	return probe_saved_rip;
}

uint64_t syscall_probe_saved_flags(void)
{
	return probe_saved_flags;
}

uint64_t syscall_probe_kernel_rsp(void)
{
	return probe_kernel_rsp;
}

/*
 * The first call from the BOOT IMAGE, said once (#422/#467).
 *
 * ⚠️ The proof that a user task is alive cannot be its absence.  Once the boot
 * image runs, the machine simply carries on -- no panic, no halt, the clock
 * ticking -- and "nothing went wrong" is exactly what a task that never
 * started also looks like from outside.  So the task says so, through the only
 * channel it has, and the kernel repeats it.
 *
 * The arguments are what distinguishes it from the ring-3 self-test, which
 * takes the same path a few hundred milliseconds earlier with 1..6.  A count
 * both could have produced would prove neither.
 */
#define BOOT_IMAGE_A1	0x51
#define BOOT_IMAGE_A6	0x56

static uint64_t boot_image_calls;

uint64_t syscall_probe_boot_image_calls(void)
{
	return boot_image_calls;
}

static void note_boot_image(uint64_t a1, uint64_t a6)
{
	if (a1 != BOOT_IMAGE_A1 || a6 != BOOT_IMAGE_A6)
		return;

	if (boot_image_calls++ != 0)
		return;

	printf("boot_probe: the 64-bit boot image is running in ring 3 and "
	       "has called into the kernel\n");
}

/*
 * The first message, reported by the only thing that can judge it (#426).
 *
 * The boot image sends a message to a port it holds the receive right for and
 * takes it off again — the whole IPC path, from a task, with no server in it.
 * What it cannot do is say whether the answer was right: it has no way to
 * print a number.  So it hands the kernel four, under a marker of its own, and
 * the kernel says.
 *
 * ⚠️ msgh_local_port is the field that decides, and the identifier is not.
 * Both come out of a buffer the sender filled, so an id that comes back is
 * equally consistent with "the message went round" and with "the receive did
 * nothing and this is still what I wrote".  The sender zeroes the local port;
 * the kernel fills it in on the way out — ipc_kmsg_copyout_header does
 * `msg->msgh_local_port = dest_name'.  So it is the one field in the answer
 * that ring 3 could not have produced, and the one this asserts on.
 */
#define BOOT_IPC_A1	0x60
#define BOOT_IPC_A6	0x66
#define BOOT_IPC_ID	0x5eed

static uint64_t boot_ipc_calls;

uint64_t syscall_probe_boot_ipc_calls(void)
{
	return boot_ipc_calls;
}

static void note_boot_ipc(uint64_t a1, uint64_t kr, uint64_t id,
			  uint64_t local, uint64_t port, uint64_t a6)
{
	if (a1 != BOOT_IPC_A1 || a6 != BOOT_IPC_A6)
		return;

	if (boot_ipc_calls++ != 0)
		return;

	printf("boot_probe: mach_msg on port 0x%lx answered 0x%lx, id 0x%lx "
	       "back, and the kernel wrote local port 0x%lx%s\n",
	       (unsigned long) port, (unsigned long) kr, (unsigned long) id,
	       (unsigned long) local,
	       (kr == 0 && id == BOOT_IPC_ID && port != 0 && local == port)
	       ? " — the first IPC on x86-64, through the nine-argument stub"
	       : " — WRONG");
}

uint64_t syscall_probe(uint64_t a1, uint64_t a2, uint64_t a3,
		       uint64_t a4, uint64_t a5, uint64_t a6)
{
	uint64_t top = percpu()->kernel_rsp;
	const uint64_t *saved = (const uint64_t *)(uintptr_t)top;

	probe_gs_base = rdmsr(MSR_GS_BASE);

	/*
	 * In the order the entry pushed them, which is the reverse of the order
	 * they come off: rcx first, so it is nearest the top.
	 */
	probe_kernel_rsp = top;
	probe_saved_rip = saved[-1];
	probe_saved_flags = saved[-2];
	note_boot_image(a1, a6);
	note_boot_ipc(a1, a2, a3, a4, a5, a6);

	probe_depth = x86_64_backtrace_probe(
			(uint64_t)(uintptr_t)__builtin_frame_address(0),
			&probe_top, "syscall_entry", &probe_reached_entry);

	return  (a1 & 0xFF)
	     | ((a2 & 0xFF) << 8)
	     | ((a3 & 0xFF) << 16)
	     | ((a4 & 0xFF) << 24)
	     | ((a5 & 0xFF) << 32)
	     | ((a6 & 0xFF) << 40);
}

/*
 * The wide call, and what it is for (#426).
 *
 * Nothing in the kernel calls this: it exists to be called from ring 3 with
 * eleven arguments, so that the path which pushes the last five onto the
 * kernel stack is executed by every boot.  Without it the first exercise of
 * that path would be a real mach_msg, where a misplaced argument is a
 * message with the wrong reply port rather than a number that does not
 * match.
 */
uint64_t syscall_probe_wide(uint64_t a1, uint64_t a2, uint64_t a3,
			    uint64_t a4, uint64_t a5, uint64_t a6,
			    uint64_t a7, uint64_t a8, uint64_t a9,
			    uint64_t a10, uint64_t a11)
{
	return  (a1 & 0xF)
	     | ((a2 & 0xF) << 4)
	     | ((a3 & 0xF) << 8)
	     | ((a4 & 0xF) << 12)
	     | ((a5 & 0xF) << 16)
	     | ((a6 & 0xF) << 20)
	     | ((a7 & 0xF) << 24)
	     | ((a8 & 0xF) << 28)
	     | ((a9 & 0xF) << 32)
	     | ((a10 & 0xF) << 36)
	     | ((a11 & 0xF) << 40);
}

/*
 * What each call number does.  Read from ring 3's index, so its length is
 * the only thing standing between a user program and an arbitrary call
 * through kernel memory — which is why the check against it is in the entry
 * path and not here.
 */
void *const syscall_table[SYSCALL_NR_MAX] = {
	[SYSCALL_NR_PROBE] = (void *)syscall_probe,
	[SYSCALL_NR_PROBE_WIDE] = (void *)syscall_probe_wide,
};

/*
 * And how many arguments each of them carries beyond the register six.
 *
 * Written out rather than derived, because there is nothing to derive it
 * from: this kernel's own calls have no table of arities the way the Mach
 * traps do.  ⚠️ Which means this is the one place in the mechanism where two
 * halves can disagree — a call added above with seven arguments and a zero
 * here would take its seventh from whatever the entry left on the stack.
 * The declaration in <syscall/syscall.h> is the check that exists: the
 * prototype there and the entry here are compiled together, so a count that
 * is too small is at least visible in one file.
 */
const uint8_t syscall_stack[SYSCALL_NR_MAX] = {
	[SYSCALL_NR_PROBE] = 0,
	[SYSCALL_NR_PROBE_WIDE] = 11 - SYSCALL_REG_ARGS,
};

/*
 * The Mach traps, as plain function pointers (#411).
 *
 * ⚠️ A copy rather than an indirection into mach_trap_table[], and the copy is
 * the safe choice.  That table's elements are a structure, and an entry path
 * indexing it would carry its layout -- element size, the offset of the
 * function pointer -- as constants in assembly.  Nothing would then compare
 * those constants with the structure again, and the day it gains a field the
 * entry calls whatever is now at offset eight, correctly, forever.  #448 is
 * that shape and it took an issue to find.
 *
 * Here the assembly knows one fact, that the elements are pointers, and it is
 * a fact about the array it is reading rather than about a structure defined
 * somewhere else.
 */
void *mach_syscall_table[SYSCALL_MACH_MAX];
uint8_t mach_syscall_stack[SYSCALL_MACH_MAX];
uint64_t mach_syscall_count;

static void mach_syscall_table_init(void)
{
	unsigned n = (unsigned) mach_trap_count;

	/*
	 * ⚠️ Refused rather than truncated.  Silently dispatching the first 128
	 * and answering ENOSYS for the rest would be a kernel where some traps
	 * work and some do not, with nothing saying which -- and the boundary
	 * would move whenever the table did.
	 */
	if (n > SYSCALL_MACH_MAX)
		panic("syscall: %u Mach traps, the entry table holds %u -- "
		      "raise SYSCALL_MACH_MAX", n, (unsigned) SYSCALL_MACH_MAX);

	for (unsigned i = 0; i < n; i++) {
		unsigned args = (unsigned) mach_trap_table[i].mach_trap_arg_count;

		/*
		 * ⚠️ Refused rather than clamped, and this is the check the
		 * whole wide-argument mechanism rests on.
		 *
		 * The entry path pushes the overflow from five named
		 * registers.  A trap wider than that would have its last
		 * arguments taken from registers nobody filled -- which is
		 * not a crash, it is a pointer argument holding whatever the
		 * caller happened to be using rbx for.  There is no run-time
		 * symptom to notice, so the noticing happens here.
		 */
		if (args > SYSCALL_ARGS_MAX)
			panic("syscall: Mach trap %u takes %u arguments and "
			      "the entry path carries %u -- see the register "
			      "contract in <syscall/syscall.h>",
			      i, args, (unsigned) SYSCALL_ARGS_MAX);

		mach_syscall_table[i] = (void *) mach_trap_table[i].mach_trap_function;
		mach_syscall_stack[i] = (uint8_t)
			(args > SYSCALL_REG_ARGS ? args - SYSCALL_REG_ARGS : 0);
	}

	mach_syscall_count = n;
}

void syscall_init(void)
{
	mach_syscall_table_init();

	/*
	 * STAR carries two selector bases and no selectors: the processor
	 * derives four from them by addition, which is why <cpu/desc.h> lays
	 * the table out the way it does rather than the way it reads.
	 */
	wrmsr(MSR_STAR, ((uint64_t)SYSRET_SELECTOR_BASE << 48)
			| ((uint64_t)KERNEL_CS_SELECTOR << 32));

	wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry);
	wrmsr(MSR_FMASK, SYSCALL_FMASK);

	/*
	 * Last, and it is the switch: without this bit SYSCALL is not a slow
	 * instruction, it is not an instruction at all, and a program using
	 * one takes an invalid-opcode fault.  Enabled only once everything it
	 * would jump to is in place.
	 */
	wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);
}
