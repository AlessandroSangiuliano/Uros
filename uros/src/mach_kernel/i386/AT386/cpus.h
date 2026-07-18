/*
 * ODE-style configuration header for CPUS.
 *
 * #300 (Fase A SMP) added UROS_NCPUS as a cmake option that injects
 * -DNCPUS=${UROS_NCPUS} into KERNEL_DEFINES.  The guard below lets the
 * cmake value win when set, and keeps the historical UP default of 1
 * for translation units that include <cpus.h> without going through
 * the kernel build.
 *
 * The whole stub will be removed under #306 once the ~142 callers stop
 * including it explicitly.
 */
#ifndef _I386_AT386_CPUS_H_
#define _I386_AT386_CPUS_H_

#ifndef NCPUS
#define NCPUS 1
#endif

/*
 * #330 (dynamic per-CPU, option B): NCPUS is the fixed *maximum* number of
 * CPUs the kernel is built for -- the size of every per-CPU array -- NOT the
 * number of CPUs actually booted.  The real count is detected at runtime from
 * the ACPI MADT / MP table (real_ncpus, clamped to wncpu <= NCPUS in
 * i386/AT386/mp/mp.c); bring-up and per-CPU sweeps iterate the real CPUs
 * (machine_slot[i].is_cpu / .running) or harmlessly touch zeroed absent slots.
 *
 * Consequence: set UROS_NCPUS once to a generous cap (e.g. 64) and the kernel
 * image layout is FIXED -- it no longer shifts with the number of CPUs you
 * run, so the same binary boots 1, 8, 16, ... CPUs with no rebuild and the
 * layout-shift class of bug (the old 8-CPU bring-up wedge) cannot recur.
 * MAX_CPUS is the preferred name for this cap in new code.
 */
#define MAX_CPUS	NCPUS

#endif /* _I386_AT386_CPUS_H_ */
