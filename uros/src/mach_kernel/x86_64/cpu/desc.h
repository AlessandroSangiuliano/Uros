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

#include <stdint.h>

/* The fixed selectors, the same on every processor. */
#define KERNEL_CS_SELECTOR	0x08
#define KERNEL_DS_SELECTOR	0x10

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

#endif	/* _X86_64_CPU_DESC_H_ */
