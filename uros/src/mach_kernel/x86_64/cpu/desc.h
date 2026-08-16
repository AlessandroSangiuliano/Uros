/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Descriptor tables, one processor's worth at a time (#438).
 *
 * These were the boot processor's private business while there was only one
 * processor.  There is not: an application processor comes out of the
 * trampoline running on the sixteen bytes of descriptor table that lived in
 * the trampoline page, with no task register and — this is the one that
 * matters — no interrupt descriptor table at all.  A processor in that state
 * does not fail an interrupt.  It triple-faults and resets, silently,
 * because reporting the problem is the very thing it has no table for.
 *
 * So everything the boot processor did in #406 has to be doable again, per
 * processor, and the split is by what is genuinely shared:
 *
 *   the GDT      one table, every processor loads the same image
 *   the IDT      likewise; only the register that points at it is per-CPU
 *   the TSS      one each, unavoidably — it is a table of stacks, and two
 *                processors handling a fault on the same stack would be
 *                writing over each other's report of it
 *
 * Which is why the GDT has a TSS descriptor per processor rather than one:
 * the table stays shared and read-only, and the only per-CPU step left is
 * which selector goes into the task register.
 */

#ifndef _X86_64_CPU_DESC_H_
#define _X86_64_CPU_DESC_H_

/*
 * The fixed selectors, the same on every processor — and their order is not
 * a choice (#411).
 *
 * SYSCALL and SYSRET do not take a selector.  They take one number, held in
 * STAR, and derive four selectors from it by arithmetic:
 *
 *   SYSCALL   CS = STAR[47:32]          SS = STAR[47:32] + 8
 *   SYSRET    CS = STAR[63:48] + 16     SS = STAR[63:48] + 8
 *
 * So the descriptor table has to be laid out to suit the arithmetic, rather
 * than the instruction being told where things are.  Kernel code must be
 * immediately followed by kernel data; user data must sit eight bytes past
 * the SYSRET base and user code sixteen — in that order, which puts data
 * *before* code on the user side and is the opposite of the kernel side.
 *
 * Getting it wrong does not fail at setup.  It fails on the first return to
 * user mode, by loading whatever descriptor happens to live at the computed
 * offset — which is how a return to ring 3 lands on a kernel data segment
 * that a user program can then write through.
 *
 * The gap at USER_CS32_SELECTOR is the arithmetic's, not ours: the 64-bit
 * return skips it, and only the compatibility-mode form of SYSRET would load
 * it.  Nothing emits that form yet.  It holds a correct 32-bit user code
 * descriptor rather than a null one, because a null there would turn the day
 * someone adds a 32-bit userland into a fault with no obvious cause.
 */
#define KERNEL_CS_SELECTOR	0x08
#define KERNEL_DS_SELECTOR	0x10

#define SYSRET_SELECTOR_BASE	0x18
#define USER_CS32_SELECTOR	0x18	/* compatibility mode; unused so far  */
#define USER_DS_SELECTOR	0x20	/* = base + 8,  loaded into SS        */
#define USER_CS_SELECTOR	0x28	/* = base + 16, loaded into CS        */

/* What a selector looks like once the processor has added the ring. */
#define USER_RPL		3
#define USER_DS_RPL3		(USER_DS_SELECTOR | USER_RPL)
#define USER_CS_RPL3		(USER_CS_SELECTOR | USER_RPL)

#ifndef __ASSEMBLER__

#include <stdint.h>

/*
 * Build the shared tables and put this processor on them.
 *
 * The boot processor only, and before anything that can fault — which is
 * everything, so in practice this is the first thing after reaching C.  Its
 * own stacks are statically allocated for exactly that reason: it needs
 * somewhere to take a double fault long before there is an allocator to ask.
 */
void desc_init_bsp(void);

/*
 * Give a processor that has not been woken yet its task-state segment and
 * the stacks that go with it.
 *
 * Boot processor, before waking anybody — the same division of labour as
 * percpu_alloc(): this allocates and writes into the shared GDT, which is
 * work that must not be happening on several processors at once, and
 * desc_activate() below is the part that touches nothing but the caller's
 * own registers.
 */
void desc_alloc(uint32_t cpu_id);

/*
 * Load the shared tables and this processor's task register, then the IDT.
 *
 * Order is load-bearing and has been since #406: a gate naming an
 * interrupt-stack-table slot is a promise the CPU keeps by reading the task
 * register, so the TSS has to be loaded before any such gate exists.
 */
void desc_activate(uint32_t cpu_id);

/*
 * Is `addr` on the interrupt stack belonging to IST slot `slot` (#409)?
 *
 * The one observable difference between a gate that switched stacks and a gate
 * that did not is where the frame was built, so this is what turns the IST
 * assignment from a table somebody wrote into something a test can check.
 */
int desc_on_ist_stack(uint32_t cpu_id, unsigned slot, uint64_t addr);

/*
 * One entry of the shared table, named by its selector.
 *
 * So that the layout can be checked against the arithmetic the processor
 * will actually perform, rather than against the comment sitting next to it.
 * The comment is what would be wrong.
 */
uint64_t desc_gdt_entry(unsigned selector);

/*
 * The same entry, for a caller that must not be right (#477).
 *
 * desc_gdt_entry() panics on a selector past the table, which is the correct
 * contract for code that knows which selector it is asking about.  A FAULT
 * REPORT does not: the selector it asks about is one the processor handed it
 * in an error code, from a fault that may have happened precisely because
 * something was wrong with it.  Answering a bad selector with a panic there
 * would replace the diagnosis with a second fault about the reporter -- the
 * same reason the backtrace and the instruction bytes check every address
 * before touching it.
 *
 * Returns 0 and writes nothing if the selector is past the table.
 */
int desc_gdt_peek(unsigned selector, uint64_t *out);

/*
 * The stack a privilege transition lands on for a given processor.
 *
 * One stack, shared by both ways in: a trap gate takes it from the
 * task-state segment and the syscall entry is handed the same value, which
 * is safe because the two cannot be in flight at once — a syscall is not
 * interruptible before it has switched, and a trap from ring 3 is not a
 * syscall.  Two stacks would mean two things to keep current at every
 * context switch, for no gain.
 */
uint64_t desc_rsp0(uint32_t cpu_id);

/*
 * Point a processor's ring-3 entry at a different stack (#422).
 *
 * Called by the context switch: a trap from ring 3 has to land on the kernel
 * stack of the thread it came from, and until there was a user task nothing
 * needed to say so.
 */
void desc_set_rsp0(uint32_t cpu_id, uint64_t rsp0);

/* The fields of a descriptor that decide who may use it and how. */
#define DESC_ACCESS(d)		(((d) >> 40) & 0xFF)
#define DESC_DPL(d)		(((d) >> 45) & 0x3)
#define DESC_IS_CODE(d)		((((d) >> 43) & 0x1) != 0)
#define DESC_IS_PRESENT(d)	((((d) >> 47) & 0x1) != 0)
#define DESC_IS_LONG(d)		((((d) >> 53) & 0x1) != 0)

#endif	/* __ASSEMBLER__ */

#endif	/* _X86_64_CPU_DESC_H_ */
