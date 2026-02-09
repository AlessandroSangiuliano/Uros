#ifndef _MACH_LOCK_H_
#define _MACH_LOCK_H_

/* Minimal userspace stub for Mach lock API */

typedef int lock_set_t;
#define KERN_LOCK_OWNED 40
#define KERN_LOCK_SET_DESTROYED 38
#define KERN_LOCK_UNSTABLE 39
#define KERN_LOCK_OWNED_SELF 41

#endif /* _MACH_LOCK_H_ */
