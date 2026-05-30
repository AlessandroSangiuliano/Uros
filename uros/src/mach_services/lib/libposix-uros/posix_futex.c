/*
 * libposix-uros — futex via mach_msg (#260).
 *
 * Replaces the busy-wait stub that lived in handlers.c since Phase 6a.
 * Every Linux futex address `uaddr` maps to a Mach port whose receive
 * right is owned by this task; FUTEX_WAIT does `mach_msg(MACH_RCV_MSG)`
 * on that port and blocks at scheduler level, FUTEX_WAKE sends N empty
 * messages and the kernel hands one to each blocked receiver.
 *
 * Mach allows several threads to block in receive on the same port
 * concurrently — the kernel queues them and delivers messages one at a
 * time, which is exactly the semantics futex_wake(N) wants.  No
 * port_set is needed; a single per-address port works.
 *
 * Important: this is called from inside musl's mutex/condvar primitives,
 * including the ones malloc() takes.  We must NOT call malloc()/free()
 * from here — would recurse.  All state lives in a fixed-size static
 * pool with a single spinlock; the lock is held only across small
 * O(NENTRIES) operations and is dropped before any blocking mach_msg.
 *
 * Out of scope:
 *   - FUTEX_PRIVATE / FUTEX_CLOCK_REALTIME flag bits: ignored.  Every
 *     futex here is intra-task and timeouts are interpreted as relative
 *     (which matches FUTEX_WAIT, the only operation that takes one in
 *     the workloads pthread_mutex/cond_wait generate).
 *   - FUTEX_CMP_REQUEUE / FUTEX_WAKE_OP: not used by musl-pthread on
 *     i386 in the common path.  Caller (h_futex) returns -ENOSYS.
 *
 * Author: Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * License: MIT
 */

#include <stdint.h>
#include <stddef.h>

#include <mach.h>
#include <mach/mach_traps.h>
#include <mach/mach_port.h>
#include <mach/message.h>


/* Linux 32-bit timespec — SYS_futex (240) takes the time32 layout on
 * i386 even when the rest of musl uses 64-bit time_t.  See musl
 * src/thread/__timedwait.c.  */
struct __uros_futex_timespec {
    long tv_sec;
    long tv_nsec;
};

#define UROS_FUTEX_NBUCKETS  64u
#define UROS_FUTEX_NENTRIES  256

struct futex_entry {
    uintptr_t           addr;       /* 0 marks a free slot */
    mach_port_t         port;       /* per-address receive+send right */
    unsigned int        waiters;
    struct futex_entry *next;       /* bucket chain */
};

static struct futex_entry  futex_pool[UROS_FUTEX_NENTRIES];
static struct futex_entry *futex_buckets[UROS_FUTEX_NBUCKETS];
static volatile int        futex_lock;

static inline void
fx_lock(void)
{
    while (__atomic_test_and_set(&futex_lock, __ATOMIC_ACQUIRE))
        __asm__ __volatile__("pause");
}

static inline void
fx_unlock(void)
{
    __atomic_clear(&futex_lock, __ATOMIC_RELEASE);
}

static unsigned
fx_hash(uintptr_t a)
{
    return (unsigned)(((a >> 2) ^ (a >> 12)) & (UROS_FUTEX_NBUCKETS - 1));
}

/* Caller holds futex_lock. */
static struct futex_entry *
fx_lookup(uintptr_t addr)
{
    struct futex_entry *e = futex_buckets[fx_hash(addr)];
    while (e && e->addr != addr)
        e = e->next;
    return e;
}

/* Caller holds futex_lock.  Returns NULL if pool is full or port alloc
 * fails — h_futex falls back to -EAGAIN in that case, matching the old
 * stub's behaviour. */
static struct futex_entry *
fx_lookup_or_create(uintptr_t addr)
{
    struct futex_entry *e = fx_lookup(addr);
    if (e)
        return e;

    int slot = -1;
    for (int i = 0; i < UROS_FUTEX_NENTRIES; i++) {
        if (futex_pool[i].addr == 0) { slot = i; break; }
    }
    if (slot < 0)
        return NULL;

    mach_port_t p = MACH_PORT_NULL;
    if (mach_port_allocate(mach_task_self(),
                           MACH_PORT_RIGHT_RECEIVE, &p) != KERN_SUCCESS)
        return NULL;
    (void)mach_port_insert_right(mach_task_self(), p, p,
                                 MACH_MSG_TYPE_MAKE_SEND);

    unsigned h = fx_hash(addr);
    futex_pool[slot].addr    = addr;
    futex_pool[slot].port    = p;
    futex_pool[slot].waiters = 0;
    futex_pool[slot].next    = futex_buckets[h];
    futex_buckets[h]         = &futex_pool[slot];
    return &futex_pool[slot];
}

/* Caller holds futex_lock.  GC the entry if no one is waiting. */
static void
fx_release(struct futex_entry *e)
{
    if (e->waiters != 0)
        return;
    unsigned h = fx_hash(e->addr);
    struct futex_entry **pp = &futex_buckets[h];
    while (*pp && *pp != e) pp = &(*pp)->next;
    if (*pp == e) *pp = e->next;
    (void)mach_port_destroy(mach_task_self(), e->port);
    e->addr = 0;
    e->port = MACH_PORT_NULL;
    e->next = NULL;
}

/* ------------------------------------------------------------------ */
/* Public API consumed by handlers.c                                   */
/* ------------------------------------------------------------------ */

/* Max single mach_msg receive slice (ms).  We always cap the kernel
 * receive at this even for untimed futexes so the wait periodically
 * re-checks *uaddr.  This covers wakeups the kernel performs WITHOUT a
 * userspace FUTEX_WAKE — notably CLONE_CHILD_CLEARTID, where the kernel
 * zeroes __thread_list_lock on thread death (thread_act.c) but has no
 * way to send our per-address mach_msg.  Explicit FUTEX_WAKE still
 * returns the waiter immediately; this only bounds the latency of the
 * silent-clear case.  Not CPU pegging: the thread sleeps in-kernel
 * between slices rather than spinning in userspace. */
#define UROS_FUTEX_POLL_MS  16u

int
__uros_futex_wait(uint32_t *uaddr, uint32_t val, const void *timeout)
{
    if (!uaddr)
        return -22;     /* -EINVAL */

    /* Total budget in ms when the caller supplied a (relative) timeout;
     * 0 here means "no caller deadline — wait indefinitely, re-checking
     * every POLL_MS". */
    unsigned long budget_ms = 0;
    if (timeout) {
        const struct __uros_futex_timespec *ts = timeout;
        budget_ms = (unsigned long)ts->tv_sec * 1000UL
                  + (unsigned long)ts->tv_nsec / 1000000UL;
        if (budget_ms == 0)
            budget_ms = 1;
    }

    fx_lock();
    struct futex_entry *e = fx_lookup_or_create((uintptr_t)uaddr);
    if (!e) {
        fx_unlock();
        return -11;     /* -EAGAIN — pool exhausted, caller retries */
    }
    /* Value check under the lock: the waker takes futex_lock too, so
     * this pairs the publication of the new value with the wake's
     * release of the lock. */
    if (*(volatile uint32_t *)uaddr != val) {
        fx_release(e);
        fx_unlock();
        return -11;     /* -EAGAIN */
    }
    e->waiters++;
    mach_port_t port = e->port;
    fx_unlock();

    int rc = 0;
    for (;;) {
        mach_msg_timeout_t slice = UROS_FUTEX_POLL_MS;
        if (timeout) {
            if (budget_ms == 0) { rc = -110 /* -ETIMEDOUT */; break; }
            if (budget_ms < slice)
                slice = (mach_msg_timeout_t)budget_ms;
            budget_ms -= slice;
        }

        mach_msg_header_t hdr = {0};
        kern_return_t kr = mach_msg(&hdr,
                                    MACH_RCV_MSG | MACH_RCV_TIMEOUT,
                                    0,             /* send_size */
                                    sizeof(hdr),   /* recv max */
                                    port,
                                    slice,
                                    MACH_PORT_NULL);
        if (kr == KERN_SUCCESS) {
            rc = 0;             /* explicit FUTEX_WAKE delivered */
            break;
        }
        if (kr != MACH_RCV_TIMED_OUT) {
            rc = -4;            /* -EINTR */
            break;
        }
        /* Slice expired with no wake — re-check the value.  A kernel
         * cleartid (or any waker that changed *uaddr without managing to
         * reach our FUTEX_WAKE) is observed here. */
        if (*(volatile uint32_t *)uaddr != val) {
            rc = 0;
            break;
        }
        /* Still equal and (if timed) budget remains — loop and block
         * for another slice. */
    }

    fx_lock();
    if (e->waiters > 0)
        e->waiters--;
    fx_release(e);
    fx_unlock();

    return rc;
}

int
__uros_futex_wake(uint32_t *uaddr, int n)
{
    if (!uaddr || n <= 0)
        return 0;

    fx_lock();
    struct futex_entry *e = fx_lookup((uintptr_t)uaddr);
    if (!e || e->waiters == 0) {
        fx_unlock();
        return 0;
    }
    unsigned want = (unsigned)n;
    if (want > e->waiters) want = e->waiters;
    mach_port_t port = e->port;
    fx_unlock();

    int woke = 0;
    for (unsigned i = 0; i < want; i++) {
        mach_msg_header_t hdr;
        hdr.msgh_bits        = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
        hdr.msgh_size        = sizeof(hdr);
        hdr.msgh_remote_port = port;
        hdr.msgh_local_port  = MACH_PORT_NULL;
        hdr.msgh_id          = 0x46555458;   /* 'FUTX' */
        kern_return_t kr = mach_msg(&hdr,
                                    MACH_SEND_MSG,
                                    sizeof(hdr),
                                    0,
                                    MACH_PORT_NULL,
                                    MACH_MSG_TIMEOUT_NONE,
                                    MACH_PORT_NULL);
        if (kr != KERN_SUCCESS)
            break;
        woke++;
    }
    return woke;
}
