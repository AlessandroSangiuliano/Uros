/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Starting the other processors (#438).
 */

#ifndef _X86_64_CPU_SMP_H_
#define _X86_64_CPU_SMP_H_

#include <stdint.h>

/*
 * The most processors this kernel will start.  A cap on the stack table,
 * which has to exist before the count is known — the table is what an AP
 * indexes with its own id on its way in, so it cannot be sized from the
 * answer it is part of finding out.
 */
#define SMP_MAX_CPUS	64

/*
 * Wake every processor ACPI listed as startable, and answer with how many
 * reported in.
 *
 * All of them at once: the trampoline shares nothing writable between
 * processors, so there is no reason to wake them one at a time and no
 * funnel for them to queue at.  What this waits for is the count, not a
 * sequence.
 */
unsigned smp_start_others(void);

/*
 * Have an application processor cross-call the boot processor, and answer
 * with how many of them reached it.
 *
 * The other direction, which is not the same claim.  Everything so far has
 * been the boot processor asking and the others answering — which exercises
 * its command register and their handlers, and says nothing about theirs or
 * about its own.  A processor that can serve a message but not send one is
 * a processor that can never initiate a shootdown, and the only thing
 * keeping it from having to is that nothing schedules work on it yet.
 *
 * Timing is the whole difficulty, and why this is a call rather than
 * something an arriving processor simply does.  A broadcast reaches every
 * processor in the machine, including ones still in the trampoline: they
 * have no interrupt table yet, and while they cannot take the message with
 * interrupts off, it waits in their controller and is taken the moment they
 * enable them — against a request that finished long before.  So the first
 * processor to arrive claims the job and then waits, and the boot processor
 * opens the gate once bring-up is over and everybody has a table.
 */
unsigned smp_ap_call_probe(void);

/* How many processors are running, the boot processor included. */
unsigned smp_online_count(void);

/*
 * Publish that count as real_ncpus, which the machine-independent scheduler
 * reads to decide when the machine is up.  Called once, after the others
 * have reported in -- there is nothing to count before that (#453).
 *
 * ⚠️ real_ncpus is declared HERE, beside the call that sets it, and this is
 * the first header in the tree to declare it at all.  Every one of its users
 * -- kern/sched_prim.c, i386's power_save.c and mp_table.c -- writes its own
 * `extern int real_ncpus;' at the point of use, which is the arrangement that
 * makes a type change invisible to the compiler and to the linker both (#448).
 * x86_64/cpu/machdep.c defines it and includes this, so on this target the
 * two halves meet.
 */
extern int real_ncpus;

void machine_real_ncpus_init(void);

/*
 * Register those processors as slots of machine_slot[], which is what the
 * machine-independent kernel walks to give each one an idle thread (#461).
 *
 * Called immediately after machine_real_ncpus_init(), and it checks the two
 * against each other rather than trusting either.
 */
void machine_slots_init(void);

/* Whether a given APIC id has reported in. */
int smp_is_online(uint32_t apic_id);

#endif	/* _X86_64_CPU_SMP_H_ */
