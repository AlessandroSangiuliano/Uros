#ifndef _MACH_I386_THREAD_STATUS_H_
#define _MACH_I386_THREAD_STATUS_H_

/* Minimal stub for userspace build */
struct i386_thread_state {
    unsigned int gs;
    unsigned int fs;
    unsigned int es;
    unsigned int ds;
    unsigned int edi;
    unsigned int esi;
    unsigned int ebp;
    unsigned int esp;
    unsigned int ebx;
    unsigned int edx;
    unsigned int ecx;
    unsigned int eax;
    unsigned int eip;
    unsigned int cs;
    unsigned int efl;
    unsigned int uesp;
    unsigned int ss;
};
#define i386_THREAD_STATE_COUNT (sizeof(struct i386_thread_state)/sizeof(unsigned int))

/* Userspace stack offset for rthreads (i386):
 * This value should match the stack alignment and calling convention.
 * 16 is a safe default for i386 System V ABI (align to 16 bytes).
 */
#ifndef RTHREAD_STACK_OFFSET
#define RTHREAD_STACK_OFFSET 16
#endif

#ifndef i386_THREAD_STATE
#define i386_THREAD_STATE 1 /* Arbitrary nonzero value for userspace stub */
#endif

#endif /* _MACH_I386_THREAD_STATUS_H_ */
