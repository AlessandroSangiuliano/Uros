/*
 * libposix-uros — Linux futex backed by urmach_futex (#324).
 *
 * Replaces the per-address Mach-port + mach_msg scheme (#260).  Every
 * Linux FUTEX_WAIT/FUTEX_WAKE now maps directly onto the kernel's
 * urmach_futex trap: the wait blocks at scheduler level on the word's
 * own address (no port to allocate, no message to send), and the value
 * re-check that closes the lost-wakeup window happens in the kernel,
 * atomically with the wait-queue enqueue.  This is the fast block/wake
 * path that backs every musl pthread mutex / condvar / semaphore / join.
 *
 * All of these futexes are intra-task, so we pass URMACH_FUTEX_PRIVATE:
 * the kernel keys on (address space, virtual address) and skips the
 * VM-object lookup that the cross-task (FLIPC) path needs.
 *
 * Untimed waits still block in bounded slices and re-check *uaddr each
 * slice.  This covers wakeups the kernel performs WITHOUT a userspace
 * FUTEX_WAKE — notably CLONE_CHILD_CLEARTID, where the kernel zeroes the
 * thread's tid slot on death (thread_act.c) but issues no futex wake, so
 * a pthread_join blocked on that slot would otherwise sleep forever.
 * Explicit FUTEX_WAKE (mutex unlock, cond signal, sem_post) still returns
 * the waiter immediately; the slice only bounds the silent-clear latency.
 *
 * No malloc()/free() here: this runs inside musl's mutex/condvar
 * primitives, including the ones malloc() itself takes.  The new path is
 * allocation-free (a single trap), which also drops the old static pool
 * and its spinlock.
 *
 * Author: Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * License: MIT
 */

#include <stdint.h>
#include <stddef.h>

#include <mach/kern_return.h>
#include <mach/urmach_futex.h>

/* Linux 32-bit timespec — SYS_futex (240) takes the time32 layout on
 * i386 even when the rest of musl uses 64-bit time_t.  See musl
 * src/thread/__timedwait.c.  */
struct __uros_futex_timespec {
    long tv_sec;
    long tv_nsec;
};

/* Max single block slice (ms): bounds the latency of a kernel wakeup that
 * carries no userspace FUTEX_WAKE (CLONE_CHILD_CLEARTID on thread death).
 * Not CPU pegging: the thread sleeps in-kernel between slices. */
#define UROS_FUTEX_POLL_MS  16u

int
__uros_futex_wait(uint32_t *uaddr, uint32_t val, const void *timeout)
{
    if (!uaddr)
        return -22;     /* -EINVAL */

    /* Caller's relative deadline in ms; 0 == no deadline (block until an
     * explicit wake or an observed value change, re-checking each slice). */
    unsigned long budget_ms = 0;
    int timed = 0;
    if (timeout) {
        const struct __uros_futex_timespec *ts = timeout;
        timed = 1;
        budget_ms = (unsigned long)ts->tv_sec * 1000UL
                  + (unsigned long)ts->tv_nsec / 1000000UL;
        if (budget_ms == 0)
            budget_ms = 1;
    }

    for (;;) {
        unsigned int slice = UROS_FUTEX_POLL_MS;
        if (timed) {
            if (budget_ms == 0)
                return -110;            /* -ETIMEDOUT */
            if (budget_ms < slice)
                slice = (unsigned int)budget_ms;
            budget_ms -= slice;
        }

        kern_return_t kr = urmach_futex(uaddr,
                                        URMACH_FUTEX_WAIT | URMACH_FUTEX_PRIVATE,
                                        val, slice, (unsigned int *)0);
        switch (kr) {
        case KERN_SUCCESS:
            return 0;                   /* explicit FUTEX_WAKE delivered */
        case KERN_NOT_WAITING:
            return -11;                 /* -EAGAIN: *uaddr already != val  */
        case KERN_INVALID_ADDRESS:
            return -14;                 /* -EFAULT */
        case KERN_OPERATION_TIMED_OUT:
            /* Slice expired with no wake: observe a silent value change
             * (kernel cleartid) here, else block for another slice. */
            if (*(volatile uint32_t *)uaddr != val)
                return 0;
            continue;
        default:
            return -22;                 /* -EINVAL */
        }
    }
}

int
__uros_futex_wake(uint32_t *uaddr, int n)
{
    if (!uaddr || n <= 0)
        return 0;

    kern_return_t kr = urmach_futex(uaddr,
                                    URMACH_FUTEX_WAKE | URMACH_FUTEX_PRIVATE,
                                    (unsigned int)n, 0, (unsigned int *)0);
    /* musl ignores the woken count; report best-effort success. */
    return (kr == KERN_SUCCESS) ? n : 0;
}
