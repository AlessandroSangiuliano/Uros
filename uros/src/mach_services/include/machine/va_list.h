/* Machine header wrapper (static): prefer the i386 sa_mach implementation */
#ifndef _MACHINE_VALIST_H_
#define _MACHINE_VALIST_H_

/*
 * ⚠️ This used to name sa_mach/i386/va_list.h outright, and a userland built for
 * any other machine got i386's varargs in silence (#426).  The architecture is
 * chosen by the COMPILER now, not by a path written once: a predefined macro
 * cannot be out of step with the code being compiled, where an include
 * directory or a build-tree symlink can.
 */
#if	defined(__x86_64__)
#include <sa_mach/x86_64/va_list.h>
#elif	defined(__i386__)
#include <sa_mach/i386/va_list.h>
#else
#error "no <machine/va_list.h> for this architecture"
#endif

#endif /* _MACHINE_VALIST_H_ */
