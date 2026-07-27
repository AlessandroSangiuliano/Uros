/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * ACPI table walk, as far as finding the CPUs (#438).
 *
 * The kernel has to know which processors exist before it can start any of
 * them, and the firmware's answer lives in the MADT.  i386 could also ask
 * the older MP tables (MPS 1.x, #300's `MP_V1_1`); x86-64 does not need to,
 * because ACPI is not optional on any machine that runs in long mode.  One
 * source of truth is worth having where two were tolerated.
 *
 * This is not a general ACPI implementation and does not pretend to be: it
 * walks the tables to find one, and the interpreter that AML would need is
 * a different problem entirely.
 */

#ifndef _X86_64_CPU_ACPI_H_
#define _X86_64_CPU_ACPI_H_

#include <stdint.h>

/* What one processor's MADT entry says about it. */
struct acpi_cpu {
	uint32_t apic_id;
	uint32_t acpi_id;
	int      usable;	/* the firmware says this one may be started */
};

/*
 * Find the MADT through the root pointer the bootloader handed over, and
 * record what it says.  Returns the number of processors found, or zero if
 * there is no usable ACPI — which is different from "one processor", and is
 * why it is not reported as one.
 *
 * Every table is checksummed before it is believed.  A table that fails is
 * not partially trusted: the walk stops there.
 */
unsigned acpi_find_cpus(uint32_t mb2_info_pa);

/* How many the walk found, and the details of each. */
unsigned acpi_cpu_count(void);
const struct acpi_cpu *acpi_cpu(unsigned index);

/* How many of those the firmware says are startable. */
unsigned acpi_usable_cpu_count(void);

/* Physical address of the local APIC, from the MADT.  Zero if unknown. */
uint64_t acpi_lapic_base(void);

#endif	/* _X86_64_CPU_ACPI_H_ */
