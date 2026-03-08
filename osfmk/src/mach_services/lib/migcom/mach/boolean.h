/*
 * Minimal definition of boolean_t for building host tools like migcom.
 * This avoids the historical dependency on mach/machine/boolean.h
 * (which is resolved via symlinks in the original ODE export).
 */

#ifndef MIGCOM_LOCAL_BOOLEAN_H_
#define MIGCOM_LOCAL_BOOLEAN_H_

typedef int boolean_t;

#ifndef NOBOOL
#ifndef TRUE
#define TRUE 1
#endif

#ifndef FALSE
#define FALSE 0
#endif
#endif /* NOBOOL */

#endif /* MIGCOM_LOCAL_BOOLEAN_H_ */


