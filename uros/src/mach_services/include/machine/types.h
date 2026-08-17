#ifndef _MACHINE_TYPES_H_
#define _MACHINE_TYPES_H_

/*
 * The fixed-width quantities, declared once (#481).
 *
 * ⚠️ This used to be a wrapper naming <sa_mach/i386/types.h> outright, with a
 * second copy of itself written by CMake into generated/include -- also naming
 * i386, and answering first for 477 of the 489 units that could see either.
 * The obvious repair was the one #426 made next door in <machine/va_list.h>:
 * let the compiler pick the architecture.  It is the wrong repair here, and
 * what says so is the only code that uses these types.
 *
 * UFS declares `u_bit64_t di_qsize' as the eight-byte size field of an on-disk
 * inode, and reads it through `_SIG64_BITS' (dinode.h, fs.h).  So bit64_t being
 * `struct { int __val[2]; }' rather than a 64-bit integer is not i386 showing
 * through -- it is the layout of bytes on a disk, and a disk written on one
 * machine is read on the other.  Making it `long' where `long' is 64 bits would
 * change the format, and would not even compile: `.  _SIG64_BITS' is a member
 * access.
 *
 * Fixed-width means fixed.  There is nothing here for a machine to decide, so
 * nothing chooses, and there is one file instead of one per architecture.  The
 * per-architecture copies under sa_mach/ are now on no include path; they stay
 * for study, like the PowerPC headers beside them.
 */
typedef signed char	bit8_t;		/* signed 8-bit quantity */
typedef unsigned char	u_bit8_t;	/* unsigned 8-bit quantity */

typedef short		bit16_t;	/* signed 16-bit quantity */
typedef unsigned short	u_bit16_t;	/* unsigned 16-bit quantity */

typedef int		bit32_t;	/* signed 32-bit quantity */
typedef unsigned int	u_bit32_t;	/* unsigned 32-bit quantity */

typedef struct { int __val[2]; }	  bit64_t;	/* signed 64-bit */
typedef struct { unsigned int __val[2]; } u_bit64_t;	/* unsigned 64-bit */

#define	_SIG64_BITS	__val[0]	/* bits of interest (32) */

#endif /* _MACHINE_TYPES_H_ */
