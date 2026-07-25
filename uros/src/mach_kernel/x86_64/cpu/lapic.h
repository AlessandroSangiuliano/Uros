/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Local APIC (#438).
 *
 * Each processor has one, all of them behind the same physical address —
 * so a CPU reading the local APIC reads *its own*, and the same instruction
 * gives a different answer on each.  That property is what makes it usable
 * as identity, and it is also what makes it the only way one CPU can wake
 * another.
 */

#ifndef _X86_64_CPU_LAPIC_H_
#define _X86_64_CPU_LAPIC_H_

#include <stdint.h>

/*
 * Map the registers and take note of where they are.
 *
 * The base comes from the MSR rather than the MADT, deliberately: the MADT
 * says where the firmware put it, the MSR says where it is now, and they
 * differ on any machine where it has been relocated.  The MADT value is
 * still worth having as a cross-check, which is why the caller passes it.
 */
void lapic_init(uint64_t madt_base);

/* Whether lapic_init() found a usable local APIC. */
int lapic_present(void);

/* This CPU's APIC id, read from its own local APIC. */
uint32_t lapic_id(void);

/* Whether this CPU is the one the firmware started — the MSR says so. */
int lapic_is_bsp(void);

/*
 * The wake-up sequence, INIT then two start-up interrupts.
 *
 * `entry_pa` is where the target begins executing, in real mode, and must
 * be page-aligned below 1 MiB: the message carries a vector, and the CPU
 * turns it into a physical address by shifting it left by twelve, so only
 * the page number fits.  It is the addressing limit of the mechanism, not a
 * convention.
 */
void lapic_send_init(uint32_t apic_id);
void lapic_send_startup(uint32_t apic_id, uint64_t entry_pa);

#endif	/* _X86_64_CPU_LAPIC_H_ */
