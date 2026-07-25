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

/* How many processors are running, the boot processor included. */
unsigned smp_online_count(void);

/* Whether a given APIC id has reported in. */
int smp_is_online(uint32_t apic_id);

#endif	/* _X86_64_CPU_SMP_H_ */
