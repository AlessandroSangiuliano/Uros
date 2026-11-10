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
#include <mach/mach_host.h>		/* host_get_clock_service (#923) */
#include <mach/clock.h>			/* clock_get_time */
#include <mach/clock_types.h>		/* REALTIME_CLOCK, URMACH_MONOTONIC_CLOCK */

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

/*
 * ── clock_gettime() and gettimeofday() (#923) ──────────────────────────
 *
 * musl's numbers for the two clocks it asks for, which are Linux's.
 */
#define	UROS_CLOCK_REALTIME	0
#define	UROS_CLOCK_MONOTONIC	1

/*
 * Which Mach clock serves each.  x86-64's REALTIME_CLOCK is the time of day,
 * and URMACH_MONOTONIC_CLOCK is time since boot that never goes back.
 * i386's REALTIME_CLOCK is time since boot already, so it serves the
 * monotonic clock there, and the time of day is it plus the calendar's
 * distance from boot, read once below.
 */
#if defined(__x86_64__)
static const int uros_clock_id[2] = { REALTIME_CLOCK, URMACH_MONOTONIC_CLOCK };
#else
static const int uros_clock_id[2] = { REALTIME_CLOCK, REALTIME_CLOCK };
#endif

/*
 * The clocks' ports, asked for once and kept for the life of the process:
 * a reading is then one trap.  fork() gives the child a right under each
 * name (posix_fork.c), as it does the exec server's -- a name copied into the
 * child's memory with no right behind it would name nothing, or something
 * else.  Two threads asking at once both ask, and the one that loses gives
 * its right back.
 */
static mach_port_t	uros_clock_port[2];

static mach_port_t
uros_clock(int which)
{
    mach_port_t port = __atomic_load_n(&uros_clock_port[which],
                                       __ATOMIC_ACQUIRE);
    mach_port_t none = MACH_PORT_NULL;
    mach_port_t host;
    kern_return_t kr;

    if (port != MACH_PORT_NULL)
        return port;
    host = mach_host_self();
    kr = host_get_clock_service(host, uros_clock_id[which], &port);
    (void)mach_port_deallocate(mach_task_self(), host);
    if (kr != KERN_SUCCESS)
        return MACH_PORT_NULL;
    if (!__atomic_compare_exchange_n(&uros_clock_port[which], &none, port,
                                     0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        (void)mach_port_deallocate(mach_task_self(), port);
        port = none;
    }
    return port;
}

/* For fork(): the clock port kept under index `which', or MACH_PORT_NULL. */
mach_port_t
__uros_get_clock_port(int which)
{
    return __atomic_load_n(&uros_clock_port[which], __ATOMIC_ACQUIRE);
}

#if !defined(__x86_64__)
/*
 * i386: the calendar's distance from boot, in seconds.  BATTERY_CLOCK is the
 * CMOS clock, and a read of it spins while the chip updates (#290), so it is
 * read once, beside REALTIME_CLOCK, and kept.  Two threads reading at once
 * both read, and agree to within the second the CMOS clock counts in.
 */
static volatile long uros_epoch_at_boot;
static volatile int uros_epoch_known;

static int
uros_epoch(long *epoch)
{
    mach_port_t host, cal = MACH_PORT_NULL, clk;
    tvalspec_t wall, up;
    kern_return_t kr;

    if (!__atomic_load_n(&uros_epoch_known, __ATOMIC_ACQUIRE)) {
        clk = uros_clock(UROS_CLOCK_MONOTONIC);
        if (clk == MACH_PORT_NULL)
            return -1;
        host = mach_host_self();
        kr = host_get_clock_service(host, BATTERY_CLOCK, &cal);
        (void)mach_port_deallocate(mach_task_self(), host);
        if (kr != KERN_SUCCESS)
            return -1;
        kr = clock_get_time(cal, &wall);
        (void)mach_port_deallocate(mach_task_self(), cal);
        if (kr != KERN_SUCCESS || clock_get_time(clk, &up) != KERN_SUCCESS)
            return -1;
        uros_epoch_at_boot = (long)wall.tv_sec - (long)up.tv_sec;
        __atomic_store_n(&uros_epoch_known, 1, __ATOMIC_RELEASE);
    }
    *epoch = uros_epoch_at_boot;
    return 0;
}
#endif

/*
 * SYS_clock_gettime, SYS_clock_gettime32 on i386: a timespec of two longs on
 * both.  Two failures, two answers, as libmach's getclock() learned: no clock
 * service at all is -ENXIO, a reading refused is -EIO.
 */
long
__uros_clock_gettime(long clk, void *ts_p)
{
    struct uros_ktimespec *ts = ts_p;
    tvalspec_t now;
    mach_port_t port;

    if (clk != UROS_CLOCK_REALTIME && clk != UROS_CLOCK_MONOTONIC)
        return -EINVAL;
    if (ts == NULL)
        return -EFAULT;
    port = uros_clock((int)clk);
    if (port == MACH_PORT_NULL)
        return -ENXIO;
    if (clock_get_time(port, &now) != KERN_SUCCESS)
        return -EIO;
    ts->tv_sec = (long)now.tv_sec;
    ts->tv_nsec = (long)now.tv_nsec;
#if !defined(__x86_64__)
    if (clk == UROS_CLOCK_REALTIME) {
        long epoch;

        if (uros_epoch(&epoch) != 0)
            return -EIO;
        ts->tv_sec += epoch;
    }
#endif
    return 0;
}

/*
 * SYS_gettimeofday, SYS_gettimeofday_time32 on i386, on the calendar clock
 * above.  The zone is UTC, as the CMOS clock is read (#290), and a caller
 * that asks for it is told so.
 */
struct uros_ktimeval { long tv_sec; long tv_usec; };
struct uros_ktimezone { int tz_minuteswest; int tz_dsttime; };

long
__uros_gettimeofday(void *tv_p, void *tz_p)
{
    struct uros_ktimespec ts;
    long r;

    if (tv_p != NULL) {
        struct uros_ktimeval *tv = tv_p;

        r = __uros_clock_gettime(UROS_CLOCK_REALTIME, &ts);
        if (r != 0)
            return r;
        tv->tv_sec = ts.tv_sec;
        tv->tv_usec = ts.tv_nsec / 1000;
    }
    if (tz_p != NULL) {
        struct uros_ktimezone *tz = tz_p;

        tz->tz_minuteswest = 0;
        tz->tz_dsttime = 0;
    }
    return 0;
}
