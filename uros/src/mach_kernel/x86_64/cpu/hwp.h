/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Hardware P-states at bring-up, and the clock each processor ran at (#736).
 */

#ifndef _X86_64_CPU_HWP_H_
#define _X86_64_CPU_HWP_H_

/*
 * Once on every processor, the boot one from machine_kernel_ready() and each
 * other one from slave_machine_init(): hand the clock to the hardware where
 * the processor offers HWP and the firmware left it off, as i386/hwp.c does
 * (#358), and then measure the clock the processor runs at, from IA32_APERF
 * and IA32_MPERF over a spin of a calibrated length.  CPUID-gated: a processor
 * without HWP, every KVM vCPU among them, is left as the firmware left it, and
 * no MSR is touched that CPUID does not promise.
 *
 * hwp=0 on the command line leaves HWP as the firmware left it everywhere --
 * the other arm of an A/B -- and hwp_epp=N writes N into the Energy/
 * Performance Preference of a request this kernel programs (0 is performance;
 * without it, 0x80, the balanced midpoint).
 */
void hwp_init_cpu(int boot_processor);

/*
 * What every processor found and set, and the clock each ran at, in two lines:
 * from machine_processors_ready(), once all of them are in the scheduler and
 * so past hwp_init_cpu().
 */
void hwp_report(void);

#endif	/* _X86_64_CPU_HWP_H_ */
