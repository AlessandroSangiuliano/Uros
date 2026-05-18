/* Minimal compatibility typedefs for building on modern hosts */
#ifndef LIBSA_MACH_COMPAT_H
#define LIBSA_MACH_COMPAT_H

#include <stdint.h>
#include <limits.h>

#ifndef HAVE_QUAD_TYPES
typedef long long quad_t;
typedef unsigned long long u_quad_t;
#define QUAD_MAX LLONG_MAX
#define QUAD_MIN LLONG_MIN
#define UQUAD_MAX ULLONG_MAX
#endif

#endif /* LIBSA_MACH_COMPAT_H */
