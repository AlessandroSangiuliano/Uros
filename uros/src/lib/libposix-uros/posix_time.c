/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * libposix-uros — POSIX sleep (#375 follow-up).
 *
 * musl's nanosleep()/usleep() reach the kernel through SYS_nanosleep
 * (i386 __NR 162) — for CLOCK_REALTIME with no flags, clock_nanosleep()
 * funnels there with a 32-bit {tv_sec, tv_nsec} timespec.  Uros has no
 * Linux-style interval timer to service it, so we borrow Mach's timed
 * receive: a mach_msg on a freshly made, sender-less port with
 * MACH_RCV_TIMEOUT blocks for exactly the requested milliseconds and
 * comes back MACH_RCV_TIMED_OUT, because no message can ever arrive.
 *
 * This is a real descheduling wait, unlike
 * thread_switch(SWITCH_OPTION_WAIT): that one only calls thread_block
 * when the run queue is non-empty, so a lone tool on an otherwise idle
 * SMP box (exactly cpustat's case) would get an immediate no-op instead
 * of a sleep.
 *
 * handlers.c stays freestanding (no <mach.h>) and forwards SYS_nanosleep
 * here, the same split it uses for __uros_open / __uros_write.
 */

#include <errno.h>
#include <stddef.h>

#include <mach.h>
#include <mach/mach_traps.h>
#include <mach/mach_port.h>
#include <mach/message.h>
#include <mach_init.h>

/*
 * The i386 timespec musl hands to SYS_nanosleep is two 32-bit longs.
 * Declare it locally so this Mach-typed TU doesn't drag in musl's
 * 64-bit-time_t <time.h> (whose struct timespec has a different layout).
 */
struct uros_ktimespec { long tv_sec; long tv_nsec; };

/*
 * SYS_nanosleep handler body.  Returns 0 having slept the whole
 * interval (Uros never interrupts the wait, so *rem is always zero),
 * or a negative errno.
 */
long
__uros_nanosleep(const void *req_p, void *rem_p)
{
    const struct uros_ktimespec *req = req_p;
    unsigned long long ms;
    mach_port_t port = MACH_PORT_NULL;
    mach_msg_header_t hdr;
    kern_return_t kr;

    if (req == NULL)
        return -EFAULT;
    if (req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L)
        return -EINVAL;

    /*
     * Whole milliseconds (mach_msg_timeout_t granularity), rounding a
     * sub-millisecond remainder up so a tiny request still yields the
     * CPU rather than busy-returning.
     */
    ms = (unsigned long long)req->tv_sec * 1000ULL
       + ((unsigned long long)req->tv_nsec + 999999ULL) / 1000000ULL;
    if (ms == 0)
        return 0;
    if (ms > 0x7fffffffULL)
        ms = 0x7fffffffULL;

    kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port);
    if (kr != KERN_SUCCESS)
        return -EAGAIN;

    /*
     * No sender exists, so this can only return MACH_RCV_TIMED_OUT after
     * `ms` milliseconds — the sleep.  A per-call port keeps the handler
     * stateless and safe under concurrent sleepers.
     */
    (void)mach_msg(&hdr, MACH_RCV_MSG | MACH_RCV_TIMEOUT,
                   0, sizeof(hdr), port,
                   (mach_msg_timeout_t)ms, MACH_PORT_NULL);

    (void)mach_port_destroy(mach_task_self(), port);

    if (rem_p != NULL) {
        struct uros_ktimespec *rem = rem_p;
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }
    return 0;
}
