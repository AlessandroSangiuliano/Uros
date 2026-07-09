/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

#ifndef _I386_HWP_H_
#define _I386_HWP_H_

#include <mach/boolean.h>

/*
 * #358: Hardware P-states (Intel HWP / Speed Shift) at CPU bring-up.
 *
 * hwp_init_cpu() runs once per CPU — the BSP from setup_main() next to
 * fpu_sanity_check(), each AP from slave_machine_init() — and hands the
 * clocks to the hardware if the firmware has not already done so.  With
 * bsp=TRUE it also prints the one-line clock verdict; APs stay silent
 * (a summary line follows from start_other_cpus()).  CPUID-gated: a
 * no-op on silicon/hypervisors without HWP (QEMU/KVM vCPUs).
 */
extern void	hwp_init_cpu(boolean_t bsp);

/* CPUs that ended up with HWP active (BSP + APs), for the summary line. */
extern int	hwp_cpus_on;

/* '-E' boot arg: bias the HWP Energy/Performance Preference to performance
 * (0x00) on every CPU, for benchmark runs.  Lives in .data (#337). */
extern int	hwp_epp_performance;

/* '-Q' boot arg: skip enabling HWP (leave it as the firmware left it) for a
 * same-binary A/B of the HWP win.  Lives in .data (#337). */
extern int	hwp_skip_enable;

#endif	/* _I386_HWP_H_ */
