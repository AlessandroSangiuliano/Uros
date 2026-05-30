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

#endif /* _I386_AT386_CPUS_H_ */
