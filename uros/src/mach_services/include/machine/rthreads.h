/* Minimal wrapper to select the architecture-specific rthreads header
 * for host builds so <machine/rthreads.h> is available to
 * librthreads sources when building on the host.
 */
#ifndef _MACHINE_RTHREADS_H_
#define _MACHINE_RTHREADS_H_

#if defined(__i386__) || defined(__x86_64__)
#include <i386/rthreads.h>
#elif defined(__powerpc__) || defined(__ppc__)
#include <ppc/rthreads.h>
#else
/* Generic fallback: provide minimal stubs so code compiles on unknown archs. */
#include <mach/machine/vm_types.h>

extern int rthread_sp(void);
static inline int rthread_sp(void) { return 0; }

#define STATE_STACK(state) (0)

#endif

#endif /* _MACHINE_RTHREADS_H_ */
