/*
 * Copyright (c) 2026 Alessandro Sangiuliano <alex22_7@hotmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * POSIX Threads test suite for Uros.
 *
 * Tests: create/join, mutex, cond signal/broadcast, TSD, once, cancel.
 * Runs as a Mach standalone server (loaded by bootstrap).
 */

#include <pthread.h>
#include <stdio.h>
#include <sys/timers.h>
#include <mach/mach_traps.h>
#include <mach/thread_switch.h>
#include <mach/policy.h>		/* POLICY_TIMESHARE_INFO (#153) */
#include <mach/thread_info.h>		/* THREAD_SCHED_TIMESHARE_INFO (#153) */
#include <mach.h>			/* thread_info() user stub (#153) */
#include <mach/clock.h>			/* host_get_clock_service, REALTIME_CLOCK */
#include <signal.h>
#include <mach/port.h>
#include "gpu_console.h"

static int pass = 1;
static int test_num;

static void
test_ok(const char *name)
{
	printf("  [%d] %s: OK\n", ++test_num, name);
}

static void
test_fail(const char *name, const char *detail)
{
	printf("  [%d] %s: FAIL — %s\n", ++test_num, name, detail);
	pass = 0;
}

/* ----------------------------------------------------------------
 * Test 1: create + join + return value
 * ---------------------------------------------------------------- */

#define NTHREADS	4
#define ITERATIONS	1000

static pthread_mutex_t counter_mutex = PTHREAD_MUTEX_INITIALIZER;
static int shared_counter;

static void *
thread_counter(void *arg)
{
	int id = (int)(long)arg;
	int i;

	for (i = 0; i < ITERATIONS; i++) {
		pthread_mutex_lock(&counter_mutex);
		shared_counter++;
		pthread_mutex_unlock(&counter_mutex);
	}
	return (void *)(long)(id * 10);
}

static void
test_create_join(void)
{
	pthread_t threads[NTHREADS];
	void *retval;
	int i, rc;
	int expected = NTHREADS * ITERATIONS;

	shared_counter = 0;

	for (i = 0; i < NTHREADS; i++) {
		rc = pthread_create(&threads[i], NULL, thread_counter,
				    (void *)(long)i);
		if (rc != 0) {
			test_fail("create", "pthread_create failed");
			return;
		}
	}

	for (i = 0; i < NTHREADS; i++) {
		rc = pthread_join(threads[i], &retval);
		if (rc != 0) {
			test_fail("join", "pthread_join failed");
			return;
		}
		if ((int)(long)retval != i * 10) {
			test_fail("join retval", "wrong return value");
			return;
		}
	}

	if (shared_counter != expected) {
		char buf[80];
		snprintf(buf, sizeof(buf), "counter=%d expected=%d",
			 shared_counter, expected);
		test_fail("mutex correctness", buf);
		return;
	}

	test_ok("create/join/mutex");
}

/* ----------------------------------------------------------------
 * Test 2: condition variable — signal
 * ---------------------------------------------------------------- */

static pthread_mutex_t cond_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cond_var   = PTHREAD_COND_INITIALIZER;
static int cond_flag;

static void *
thread_cond_waiter(void *arg)
{
	pthread_mutex_lock(&cond_mutex);
	while (!cond_flag)
		pthread_cond_wait(&cond_var, &cond_mutex);
	pthread_mutex_unlock(&cond_mutex);
	return (void *)42;
}

static void
test_cond_signal(void)
{
	pthread_t th;
	void *retval;

	cond_flag = 0;
	pthread_create(&th, NULL, thread_cond_waiter, NULL);

	/* Give the waiter time to block */
	thread_switch(MACH_PORT_NULL, SWITCH_OPTION_DEPRESS, 10);

	pthread_mutex_lock(&cond_mutex);
	cond_flag = 1;
	pthread_cond_signal(&cond_var);
	pthread_mutex_unlock(&cond_mutex);

	pthread_join(th, &retval);
	if ((int)(long)retval == 42)
		test_ok("cond_signal");
	else
		test_fail("cond_signal", "wrong retval from waiter");
}

/* ----------------------------------------------------------------
 * Test 3: condition variable — broadcast
 * ---------------------------------------------------------------- */

#define NBCAST 3

static pthread_mutex_t bcast_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  bcast_cond  = PTHREAD_COND_INITIALIZER;
static int bcast_go;
static int bcast_count;

static void *
thread_bcast_waiter(void *arg)
{
	pthread_mutex_lock(&bcast_mutex);
	while (!bcast_go)
		pthread_cond_wait(&bcast_cond, &bcast_mutex);
	bcast_count++;
	pthread_mutex_unlock(&bcast_mutex);
	return NULL;
}

static void
test_cond_broadcast(void)
{
	pthread_t threads[NBCAST];
	int i;

	bcast_go = 0;
	bcast_count = 0;

	for (i = 0; i < NBCAST; i++)
		pthread_create(&threads[i], NULL, thread_bcast_waiter, NULL);

	thread_switch(MACH_PORT_NULL, SWITCH_OPTION_DEPRESS, 10);

	pthread_mutex_lock(&bcast_mutex);
	bcast_go = 1;
	pthread_cond_broadcast(&bcast_cond);
	pthread_mutex_unlock(&bcast_mutex);

	for (i = 0; i < NBCAST; i++)
		pthread_join(threads[i], NULL);

	if (bcast_count == NBCAST)
		test_ok("cond_broadcast");
	else {
		char buf[80];
		snprintf(buf, sizeof(buf), "woke %d/%d", bcast_count, NBCAST);
		test_fail("cond_broadcast", buf);
	}
}

/* ----------------------------------------------------------------
 * Test 4: thread-specific data (TSD)
 * ---------------------------------------------------------------- */

static pthread_key_t tsd_key;
static int tsd_destructor_called;

static void
tsd_destructor(void *val)
{
	tsd_destructor_called++;
}

static void *
thread_tsd(void *arg)
{
	int id = (int)(long)arg;
	pthread_setspecific(tsd_key, (void *)(long)(id + 100));
	int val = (int)(long)pthread_getspecific(tsd_key);
	if (val != id + 100)
		pass = 0;
	return NULL;
}

static void
test_tsd(void)
{
	pthread_t t1, t2;

	tsd_destructor_called = 0;
	pthread_key_create(&tsd_key, tsd_destructor);

	pthread_create(&t1, NULL, thread_tsd, (void *)1);
	pthread_create(&t2, NULL, thread_tsd, (void *)2);
	pthread_join(t1, NULL);
	pthread_join(t2, NULL);

	/* Each thread should have called the destructor */
	if (tsd_destructor_called >= 2)
		test_ok("TSD key/get/set/destructor");
	else {
		char buf[80];
		snprintf(buf, sizeof(buf), "destructor called %d times (expected 2)",
			 tsd_destructor_called);
		test_fail("TSD destructor", buf);
	}

	pthread_key_delete(tsd_key);
}

/* ----------------------------------------------------------------
 * Test 5: pthread_once
 * ---------------------------------------------------------------- */

static pthread_once_t once_ctl = PTHREAD_ONCE_INIT;
static int once_count;

static void
once_init(void)
{
	once_count++;
}

static void *
thread_once(void *arg)
{
	pthread_once(&once_ctl, once_init);
	return NULL;
}

static void
test_once(void)
{
	pthread_t t1, t2;

	once_count = 0;
	pthread_create(&t1, NULL, thread_once, NULL);
	pthread_create(&t2, NULL, thread_once, NULL);
	pthread_join(t1, NULL);
	pthread_join(t2, NULL);

	/* Also call from main thread */
	pthread_once(&once_ctl, once_init);

	if (once_count == 1)
		test_ok("pthread_once");
	else {
		char buf[80];
		snprintf(buf, sizeof(buf), "init called %d times", once_count);
		test_fail("pthread_once", buf);
	}
}

/* ----------------------------------------------------------------
 * Test 6: cancel + testcancel
 * ---------------------------------------------------------------- */

static void *
thread_cancel_target(void *arg)
{
	/*
	 * Spin calling testcancel until the main thread sets the
	 * cancel-pending flag.  testcancel will call pthread_exit
	 * once the flag is set.
	 */
	for (;;)
		pthread_testcancel();
	return NULL;
}

static void
test_cancel(void)
{
	pthread_t th;
	void *retval;

	pthread_create(&th, NULL, thread_cancel_target, NULL);

	/* Give target a chance to start */
	thread_switch(MACH_PORT_NULL, SWITCH_OPTION_DEPRESS, 10);

	pthread_cancel(th);
	pthread_join(th, &retval);

	/*
	 * When testcancel fires it calls pthread_exit(0),
	 * so retval should be 0 (== PTHREAD_CANCELED in our impl).
	 */
	if (retval == (void *)0)
		test_ok("cancel/testcancel");
	else
		test_fail("cancel/testcancel", "unexpected retval");
}

/* ----------------------------------------------------------------
 * Test 7: stacksize attribute
 * ---------------------------------------------------------------- */

static void
test_stacksize_attr(void)
{
	pthread_attr_t attr;
	size_t sz;

	pthread_attr_init(&attr);

	/* Default should be _PTHREAD_DEFAULT_STACKSIZE (64K) */
	pthread_attr_getstacksize(&attr, &sz);
	if (sz != 0x10000) {
		test_fail("stacksize default", "unexpected default size");
		return;
	}

	/* Set to 128K */
	if (pthread_attr_setstacksize(&attr, 0x20000) != 0) {
		test_fail("stacksize set 128K", "setstacksize failed");
		return;
	}
	pthread_attr_getstacksize(&attr, &sz);
	if (sz != 0x20000) {
		test_fail("stacksize get 128K", "wrong value after set");
		return;
	}

	/* Non-power-of-two should fail */
	if (pthread_attr_setstacksize(&attr, 0x18000) == 0) {
		test_fail("stacksize non-pow2", "should have failed");
		return;
	}

	/* Too small should fail */
	if (pthread_attr_setstacksize(&attr, 0x1000) == 0) {
		test_fail("stacksize too small", "should have failed");
		return;
	}

	pthread_attr_destroy(&attr);
	test_ok("stacksize attr");
}

/* ----------------------------------------------------------------
 * Test 8: read-write lock
 * ---------------------------------------------------------------- */

#define NRW_READERS 3
#define NRW_ITERS   500

static pthread_rwlock_t test_rwlock;
static int rwlock_shared_val;

static void *
thread_rwlock_reader(void *arg)
{
	int i;
	for (i = 0; i < NRW_ITERS; i++) {
		pthread_rwlock_rdlock(&test_rwlock);
		/* Just read — no modification */
		(void)rwlock_shared_val;
		pthread_rwlock_unlock(&test_rwlock);
	}
	return NULL;
}

static void *
thread_rwlock_writer(void *arg)
{
	int i;
	for (i = 0; i < NRW_ITERS; i++) {
		pthread_rwlock_wrlock(&test_rwlock);
		rwlock_shared_val++;
		pthread_rwlock_unlock(&test_rwlock);
	}
	return NULL;
}

static void
test_rwlock_func(void)
{
	pthread_t readers[NRW_READERS], writer;
	int i;

	rwlock_shared_val = 0;
	pthread_rwlock_init(&test_rwlock, NULL);

	pthread_create(&writer, NULL, thread_rwlock_writer, NULL);
	for (i = 0; i < NRW_READERS; i++)
		pthread_create(&readers[i], NULL, thread_rwlock_reader, NULL);

	pthread_join(writer, NULL);
	for (i = 0; i < NRW_READERS; i++)
		pthread_join(readers[i], NULL);

	if (rwlock_shared_val == NRW_ITERS) {
		test_ok("rwlock (readers+writer)");
	} else {
		char buf[80];
		snprintf(buf, sizeof(buf), "val=%d expected=%d",
			 rwlock_shared_val, NRW_ITERS);
		test_fail("rwlock", buf);
	}
	pthread_rwlock_destroy(&test_rwlock);
}

/* ----------------------------------------------------------------
 * Test 9: barrier
 * ---------------------------------------------------------------- */

#define NBARRIER 4

static pthread_barrier_t test_barrier;
static int barrier_serial_count;

static void *
thread_barrier(void *arg)
{
	int rc = pthread_barrier_wait(&test_barrier);
	if (rc == PTHREAD_BARRIER_SERIAL_THREAD)
		__sync_fetch_and_add(&barrier_serial_count, 1);
	return NULL;
}

static void
test_barrier_func(void)
{
	pthread_t threads[NBARRIER];
	int i;

	barrier_serial_count = 0;
	pthread_barrier_init(&test_barrier, NULL, NBARRIER);

	for (i = 0; i < NBARRIER; i++)
		pthread_create(&threads[i], NULL, thread_barrier, NULL);

	for (i = 0; i < NBARRIER; i++)
		pthread_join(threads[i], NULL);

	/* Exactly one thread should get SERIAL_THREAD */
	if (barrier_serial_count == 1)
		test_ok("barrier");
	else {
		char buf[80];
		snprintf(buf, sizeof(buf), "serial_count=%d (expected 1)",
			 barrier_serial_count);
		test_fail("barrier", buf);
	}
	pthread_barrier_destroy(&test_barrier);
}

/* ----------------------------------------------------------------
 * Test 10: spinlock
 * ---------------------------------------------------------------- */

#define NSPIN_THREADS 4
#define NSPIN_ITERS   1000

static pthread_spinlock_t test_spinlock;
static int spin_counter;

static void *
thread_spinlock(void *arg)
{
	int i;
	for (i = 0; i < NSPIN_ITERS; i++) {
		pthread_spin_lock(&test_spinlock);
		spin_counter++;
		pthread_spin_unlock(&test_spinlock);
	}
	return NULL;
}

static void
test_spinlock_func(void)
{
	pthread_t threads[NSPIN_THREADS];
	int i;
	int expected = NSPIN_THREADS * NSPIN_ITERS;

	spin_counter = 0;
	pthread_spin_init(&test_spinlock, 0);

	for (i = 0; i < NSPIN_THREADS; i++)
		pthread_create(&threads[i], NULL, thread_spinlock, NULL);

	for (i = 0; i < NSPIN_THREADS; i++)
		pthread_join(threads[i], NULL);

	if (spin_counter == expected)
		test_ok("spinlock");
	else {
		char buf[80];
		snprintf(buf, sizeof(buf), "counter=%d expected=%d",
			 spin_counter, expected);
		test_fail("spinlock", buf);
	}
	pthread_spin_destroy(&test_spinlock);
}

/* ----------------------------------------------------------------
 * Test 11: thread-safe errno
 * ---------------------------------------------------------------- */

static int errno_ok;

static void *
thread_errno(void *arg)
{
	int id = (int)(long)arg;

	/* Set errno to a unique value per thread */
	errno = 100 + id;

	/* Yield to let other thread run */
	thread_switch(MACH_PORT_NULL, SWITCH_OPTION_DEPRESS, 10);

	/* Verify our errno wasn't clobbered */
	if (errno != 100 + id)
		errno_ok = 0;
	return NULL;
}

static void
test_errno_threadsafe(void)
{
	pthread_t t1, t2;

	errno_ok = 1;
	pthread_create(&t1, NULL, thread_errno, (void *)1);
	pthread_create(&t2, NULL, thread_errno, (void *)2);
	pthread_join(t1, NULL);
	pthread_join(t2, NULL);

	if (errno_ok)
		test_ok("thread-safe errno");
	else
		test_fail("thread-safe errno", "errno clobbered across threads");
}

/* ----------------------------------------------------------------
 * Test 12: mutex ERRORCHECK type
 * ---------------------------------------------------------------- */

static void
test_mutex_errorcheck(void)
{
	pthread_mutex_t mtx;
	pthread_mutexattr_t attr;
	int rc;

	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
	pthread_mutex_init(&mtx, &attr);
	pthread_mutexattr_destroy(&attr);

	rc = pthread_mutex_lock(&mtx);
	if (rc != 0) {
		test_fail("errorcheck first lock", "unexpected error");
		return;
	}

	/* Re-lock should return EDEADLK, not deadlock */
	rc = pthread_mutex_lock(&mtx);
	if (rc != EDEADLK) {
		char buf[80];
		snprintf(buf, sizeof(buf), "expected EDEADLK(%d), got %d",
			 EDEADLK, rc);
		test_fail("errorcheck re-lock", buf);
		pthread_mutex_unlock(&mtx);
		pthread_mutex_destroy(&mtx);
		return;
	}

	pthread_mutex_unlock(&mtx);
	pthread_mutex_destroy(&mtx);
	test_ok("mutex ERRORCHECK");
}

/* ----------------------------------------------------------------
 * Test 13: mutex RECURSIVE type
 * ---------------------------------------------------------------- */

static void
test_mutex_recursive(void)
{
	pthread_mutex_t mtx;
	pthread_mutexattr_t attr;
	int i, rc;

	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&mtx, &attr);
	pthread_mutexattr_destroy(&attr);

	/* Lock 5 times recursively */
	for (i = 0; i < 5; i++) {
		rc = pthread_mutex_lock(&mtx);
		if (rc != 0) {
			char buf[80];
			snprintf(buf, sizeof(buf), "lock %d failed rc=%d", i, rc);
			test_fail("recursive lock", buf);
			pthread_mutex_destroy(&mtx);
			return;
		}
	}

	/* Unlock 5 times — all should succeed */
	for (i = 0; i < 5; i++) {
		rc = pthread_mutex_unlock(&mtx);
		if (rc != 0) {
			char buf[80];
			snprintf(buf, sizeof(buf), "unlock %d failed rc=%d", i, rc);
			test_fail("recursive unlock", buf);
			pthread_mutex_destroy(&mtx);
			return;
		}
	}

	/* Verify fully unlocked: trylock should succeed */
	rc = pthread_mutex_trylock(&mtx);
	if (rc != 0) {
		test_fail("recursive fully unlocked", "trylock failed after full unlock");
		pthread_mutex_destroy(&mtx);
		return;
	}
	pthread_mutex_unlock(&mtx);

	pthread_mutex_destroy(&mtx);
	test_ok("mutex RECURSIVE");
}

/* ----------------------------------------------------------------
 * Test 14: mutex timedlock (timeout)
 * ---------------------------------------------------------------- */

static pthread_mutex_t timedlock_mtx;

/*
 * #393: this test needs the holder to provably own the mutex while the prober
 * runs, so the prober exercises the CONTENDED path -- the only one that ever
 * consults the deadline (POSIX: an uncontended timedlock succeeds without
 * looking at abstime, so a free mutex legitimately returns 0).
 *
 * It used to arrange that with thread_switch(DEPRESS, ms) on both sides, which
 * is a scheduler hint, not synchronisation: DEPRESS lowers the caller's
 * priority and yields, it does not block for the given time.  On UP that
 * happened to hand the CPU to the holder and the test passed.  On SMP the
 * prober has its own CPU and yields to nobody, so it raced ahead and probed a
 * mutex the holder had not taken yet -- and timedlock correctly returned 0.
 *
 * These two flags make the ordering explicit and total, so the test asserts the
 * timeout and nothing about the scheduler or the CPU count.  One condvar serves
 * two predicates, hence broadcast rather than signal.
 */
static pthread_mutex_t hs_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  hs_cv  = PTHREAD_COND_INITIALIZER;
static int	       holder_locked;	/* holder -> prober: mutex is taken */
static int	       probe_done;	/* prober -> holder: you may release */

static void *
thread_timedlock_holder(void *arg)
{
	pthread_mutex_lock(&timedlock_mtx);

	/* Publish that the mutex is taken, then keep holding it until the
	 * prober says it is done -- so the probe cannot miss the contention in
	 * either direction. */
	pthread_mutex_lock(&hs_mtx);
	holder_locked = 1;
	pthread_cond_broadcast(&hs_cv);
	while (!probe_done)
		pthread_cond_wait(&hs_cv, &hs_mtx);
	pthread_mutex_unlock(&hs_mtx);

	pthread_mutex_unlock(&timedlock_mtx);
	return NULL;
}

static void
test_mutex_timedlock(void)
{
	struct timespec deadline;
	int rc;

	pthread_mutex_init(&timedlock_mtx, NULL);

	/* Test 1: timedlock on uncontended mutex — should succeed */
	rc = getclock(TIMEOFDAY, &deadline);
	if (rc != 0) {
		char		buf[128];
		mach_port_t	host = mach_host_self();
		mach_port_t	clk = MACH_PORT_NULL;
		kern_return_t	kr;

		/* getclock() collapses three kernel refusals into one errno.
		 * Ask the same question again here, where the answer can be
		 * printed: HOST_NULL, a clock_id out of range and a clock with
		 * no operations are three different bugs. */
		kr = host_get_clock_service(host, REALTIME_CLOCK, &clk);
		/* ⚠️ Two short lines, not one long one: libmach's printf
		 * flushes at 128 characters, so a diagnostic that runs past
		 * that is delivered in pieces. */
		printf("  clock: host 0x%x, hgcs 0x%x, port 0x%x\n",
		       (unsigned)host, (unsigned)kr, (unsigned)clk);
		snprintf(buf, sizeof(buf), "getclock %d (ENXIO %d, EIO %d)",
			 rc, ENXIO, EIO);
		test_fail("timedlock uncontended", buf);
		pthread_mutex_destroy(&timedlock_mtx);
		return;
	}
	deadline.tv_sec += 5;
	rc = pthread_mutex_timedlock(&timedlock_mtx, &deadline);
	if (rc != 0) {
		test_fail("timedlock uncontended", "unexpected error");
		pthread_mutex_destroy(&timedlock_mtx);
		return;
	}
	pthread_mutex_unlock(&timedlock_mtx);

	/* Test 2: timedlock with expired deadline — should return ETIMEDOUT */
	pthread_t holder;
	holder_locked = 0;
	probe_done = 0;
	pthread_create(&holder, NULL, thread_timedlock_holder, NULL);

	/* Wait until the holder provably owns the mutex (#393) — never assume a
	 * yield handed it the CPU. */
	pthread_mutex_lock(&hs_mtx);
	while (!holder_locked)
		pthread_cond_wait(&hs_cv, &hs_mtx);
	pthread_mutex_unlock(&hs_mtx);

	/* Set deadline in the past */
	deadline.tv_sec = 0;
	deadline.tv_nsec = 0;
	rc = pthread_mutex_timedlock(&timedlock_mtx, &deadline);

	/* Release the holder before judging rc: on the failure path we still
	 * join it below, and a holder parked on probe_done would wedge there. */
	pthread_mutex_lock(&hs_mtx);
	probe_done = 1;
	pthread_cond_broadcast(&hs_cv);
	pthread_mutex_unlock(&hs_mtx);

	if (rc != ETIMEDOUT) {
		char buf[80];
		snprintf(buf, sizeof(buf), "expected ETIMEDOUT(%d), got %d",
			 ETIMEDOUT, rc);
		test_fail("timedlock expired", buf);
		pthread_join(holder, NULL);
		pthread_mutex_destroy(&timedlock_mtx);
		return;
	}

	pthread_join(holder, NULL);
	pthread_mutex_destroy(&timedlock_mtx);
	test_ok("mutex timedlock");
}

/* ----------------------------------------------------------------
 * TSC timing for the two benchmarks below (#523)
 * ---------------------------------------------------------------- */

/*
 * 🔴 THE WHOLE COUNTER.  `rdtsc' returns 64 bits in EDX:EAX and both of
 * these benchmarks used to read `"=a"(start)' -- EAX alone, the high half
 * discarded.  That is a difference modulo 2^32, which at 3.9 GHz wraps every
 * 1.1 seconds, and a wrapped value is indistinguishable from a small one.
 *
 * ⚠️ [18] on i386 was wrapping, and reported i386 as 110x slower than x86-64
 * at creating a thread.  The number was believed long enough to be written
 * down.  What settled it was not reading the assembly: it was cutting the
 * loop count from 1000 to 100 and finding the reported TOTAL unchanged --
 * 983,177,000 against 921,955,771.  🔑 A total that does not scale with the
 * iteration count is not a time, whatever the explanation.
 *
 * [15] was never near the wrap, because its loop is short.  It is fixed in
 * the same pass anyway: it is the same instruction with the same defect, and
 * what keeps it safe is an iteration count, which is a thing somebody edits.
 */
static unsigned long long
tsc_now(void)
{
	unsigned int lo, hi;

	__asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
	return ((unsigned long long)hi << 32) | lo;
}

/*
 * One line of the two benchmarks below, so the printing rule lives once.
 *
 * 🔑 `total / n' IS A PLAIN 64-BIT DIVIDE, and on i386 that is a call to
 * __udivdi3 which this tree links no libgcc for.  It resolves anyway: #415
 * wrote that helper, MIT and ours, and libmach compiles the KERNEL's copy
 * rather than keeping a second one -- see the comment at
 * lib/libmach/CMakeLists.txt around the divide64.c line.  Confirmed from the
 * archive and not from the CMake:
 *
 *	nm libmach.a -> 00000000 T __udivdi3
 *
 * ⚠️ I wrote a shift-and-scale loop here first, to avoid a helper that was
 * already present and already linked.  The rule about libgcc is real; the
 * conclusion that the divide would not link was not checked.
 *
 * The 32-bit cast is checked rather than assumed, because an unrepresentable
 * value quietly narrowed is the entire subject of #523 and it would be a poor
 * joke to reintroduce it at the printf.  doprnt.c reads no `l' length
 * modifier for integers, so %llu is not available to say it any other way.
 */
static void
print_per_iter(const char *what, int n, unsigned long long total)
{
	unsigned long long	per = total / (unsigned int)n;

	if ((per >> 32) != 0)
		printf("  [%d] %s: %d iters, and the cost per iteration does "
		       "not fit the 32 bits this printf can show\n",
		       ++test_num, what, n);
	else
		printf("  [%d] %s: %d iters, %u cycles/pair\n",
		       ++test_num, what, n, (unsigned int)per);
}

/* ----------------------------------------------------------------
 * Test 15: mutex fast-path benchmark
 * ---------------------------------------------------------------- */

static void
test_mutex_bench(void)
{
	pthread_mutex_t bench_mtx = PTHREAD_MUTEX_INITIALIZER;
	unsigned long long start, end;
	int i;
	int n = 100000;

	start = tsc_now();
	for (i = 0; i < n; i++) {
		pthread_mutex_lock(&bench_mtx);
		pthread_mutex_unlock(&bench_mtx);
	}
	end = tsc_now();

	print_per_iter("mutex uncontended", n, end - start);
}

/* ----------------------------------------------------------------
 * Test 16: thread name kernel propagation
 * ---------------------------------------------------------------- */

/* Declared in <mach/mach_traps.h>, already included above (#426). */

static void *
thread_set_own_name(void *arg)
{
	int rc = pthread_setname_np(pthread_self(), "worker-1");
	return (void *)(long)rc;
}

static void
test_thread_name_kernel(void)
{
	kern_return_t kr;

	/* Direct trap call — set name on current thread */
	kr = mach_thread_set_name("main-thread");
	if (kr != KERN_SUCCESS) {
		char buf[80];
		snprintf(buf, sizeof(buf), "trap returned %d", kr);
		test_fail("thread name kernel (direct)", buf);
		return;
	}

	/* Verify userspace side still works after trap */
	char readback[16];
	pthread_setname_np(pthread_self(), "test-main");
	pthread_getname_np(pthread_self(), readback, sizeof(readback));
	if (readback[0] != 't' || readback[5] != 'm') {
		test_fail("thread name kernel (readback)", readback);
		return;
	}

	/* Test from child thread — pthread_setname_np should call the trap */
	pthread_t th;
	void *retval;
	pthread_create(&th, NULL, thread_set_own_name, NULL);
	pthread_join(th, &retval);
	if ((int)(long)retval != 0) {
		test_fail("thread name kernel (child)", "setname_np failed in child");
		return;
	}

	test_ok("thread name kernel");
}

/* ----------------------------------------------------------------
 * Test 17: per-thread signals (sigmask + kill + sigwait)
 * ---------------------------------------------------------------- */

static void *
thread_sigwait(void *arg)
{
	sigset_t wait_set;
	int sig;

	sigemptyset(&wait_set);
	sigaddset(&wait_set, SIGUSR1);

	/* Block until SIGUSR1 arrives */
	int rc = sigwait(&wait_set, &sig);
	if (rc != 0)
		return (void *)(long)-1;
	return (void *)(long)sig;
}

static void
test_signals(void)
{
	sigset_t set, old;
	int rc;

	/* Test 1: pthread_sigmask SIG_BLOCK / SIG_UNBLOCK */
	sigemptyset(&set);
	sigaddset(&set, SIGUSR1);
	rc = pthread_sigmask(SIG_BLOCK, &set, &old);
	if (rc != 0) {
		test_fail("sigmask block", "unexpected error");
		return;
	}
	/* old should be 0 (no signals blocked initially) */
	if (old != 0) {
		test_fail("sigmask old", "expected empty old mask");
		return;
	}
	/* Read back current mask */
	rc = pthread_sigmask(SIG_BLOCK, (sigset_t *)NULL, &old);
	if (rc != 0 || !sigismember(&old, SIGUSR1)) {
		test_fail("sigmask readback", "SIGUSR1 not in mask");
		return;
	}
	/* Unblock */
	rc = pthread_sigmask(SIG_UNBLOCK, &set, (sigset_t *)NULL);
	if (rc != 0) {
		test_fail("sigmask unblock", "unexpected error");
		return;
	}

	/* Test 2: pthread_kill(self, 0) — validity check */
	rc = pthread_kill(pthread_self(), 0);
	if (rc != 0) {
		test_fail("kill sig=0", "validity check failed");
		return;
	}

	/* Test 3: pthread_kill + sigwait across threads */
	pthread_t th;
	void *retval;
	pthread_create(&th, NULL, thread_sigwait, NULL);

	/* Give the waiter time to block in sigwait */
	thread_switch(MACH_PORT_NULL, SWITCH_OPTION_DEPRESS, 10);

	/* Send SIGUSR1 to the waiting thread */
	rc = pthread_kill(th, SIGUSR1);
	if (rc != 0) {
		test_fail("kill SIGUSR1", "pthread_kill failed");
		pthread_join(th, NULL);
		return;
	}

	pthread_join(th, &retval);
	if ((int)(long)retval != SIGUSR1) {
		char buf[80];
		snprintf(buf, sizeof(buf), "expected sig=%d, got %d",
			 SIGUSR1, (int)(long)retval);
		test_fail("sigwait result", buf);
		return;
	}

	test_ok("per-thread signals");
}

/* ----------------------------------------------------------------
 * Test 18: thread pool benchmark (create/join cycles)
 * ---------------------------------------------------------------- */

static void *
thread_noop(void *arg)
{
	return NULL;
}

static void
test_thread_pool_bench(void)
{
	unsigned long long start, end;
	int i;
	int n = 1000;
	pthread_t th;

	/* Warm up the pool with a few create/join cycles */
	for (i = 0; i < 4; i++) {
		pthread_create(&th, NULL, thread_noop, NULL);
		pthread_join(th, NULL);
	}

	/* Benchmark: create+join with thread pool active */
	start = tsc_now();
	for (i = 0; i < n; i++) {
		pthread_create(&th, NULL, thread_noop, NULL);
		pthread_join(th, NULL);
	}
	end = tsc_now();

	print_per_iter("create/join (pooled)", n, end - start);
}

/* ----------------------------------------------------------------
 * Test: pthread_timedjoin_np (#156)
 *
 * Two checks:
 *   a) timeout fires when the joinee is still alive
 *   b) success after the joinee terminates
 * ---------------------------------------------------------------- */

/*
 * #425: check (a) only means something while the joinee is still ALIVE -- a
 * timedjoin of a thread that has already terminated harvests it and returns 0
 * without ever consulting the deadline, exactly as an uncontended timedlock
 * legitimately returns 0.  So the joinee has to be provably running, and
 * thread_switch(DEPRESS, 200) cannot arrange that: it is a scheduler hint, not
 * a sleep.  thread_depress_priority() lowers the caller's priority and arms a
 * timer to restore it (kern/syscall_subr.c) -- the thread stays runnable and
 * merely runs last, so with a processor of its own the "slow" joinee ran
 * straight to completion.
 *
 * That made the check a measurement of the processor's clock: it won the race
 * at 3.99 GHz and lost it at 1.40 GHz, four runs out of four.
 *
 * This is the correction #393 already made to test 14, applied there and not
 * here.  The two flags below make the ordering explicit and total, so the
 * check asserts the timeout and nothing about the scheduler or the CPU count.
 */
static pthread_mutex_t tj_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  tj_cv  = PTHREAD_COND_INITIALIZER;
static int	       tj_running;	/* joinee -> main: I am alive */
static int	       tj_may_exit;	/* main -> joinee: you may finish */

static void *
timedjoin_joinee(void *arg)
{
	(void)arg;

	pthread_mutex_lock(&tj_mtx);
	tj_running = 1;
	pthread_cond_broadcast(&tj_cv);
	while (!tj_may_exit)
		pthread_cond_wait(&tj_cv, &tj_mtx);
	pthread_mutex_unlock(&tj_mtx);

	return (void *)0x42424242;
}

static void
test_timedjoin_np(void)
{
	pthread_t t;
	struct timespec deadline;
	void *retval = NULL;
	int rc;

	tj_running = 0;
	tj_may_exit = 0;

	if (pthread_create(&t, NULL, timedjoin_joinee, NULL) != 0) {
		test_fail("timedjoin_np", "pthread_create failed");
		return;
	}

	/* Wait until the joinee provably exists -- never assume a yield handed
	 * it the CPU (#393). */
	pthread_mutex_lock(&tj_mtx);
	while (!tj_running)
		pthread_cond_wait(&tj_cv, &tj_mtx);
	pthread_mutex_unlock(&tj_mtx);

	/* (a) Past deadline, joinee alive → must return ETIMEDOUT immediately. */
	deadline.tv_sec = 0;
	deadline.tv_nsec = 0;
	rc = pthread_timedjoin_np(t, NULL, &deadline);

	/* Release the joinee before judging rc: on the failure path it would
	 * otherwise stay parked on tj_may_exit for ever. */
	pthread_mutex_lock(&tj_mtx);
	tj_may_exit = 1;
	pthread_cond_broadcast(&tj_cv);
	pthread_mutex_unlock(&tj_mtx);

	if (rc != ETIMEDOUT) {
		char buf[80];
		snprintf(buf, sizeof(buf), "(a) expected ETIMEDOUT(%d), got %d",
			 ETIMEDOUT, rc);
		test_fail("timedjoin_np", buf);
		/* Join only if (a) did NOT already harvest the thread.  A
		 * pthread_t whose join succeeded describes a stack that has
		 * gone back on the free list, and joining it a second time
		 * reads recycled bytes: if they still look like a joinable
		 * thread, the join parks on the joiners futex with no timeout
		 * and never returns.  A failing test that wedges the machine
		 * reports nothing at all. */
		if (rc != 0)
			pthread_join(t, NULL);
		return;
	}

	/* (b) Generous deadline → succeed once the joinee finishes.
	 *
	 * `deadline' still holds the expired {0,0} from (a).  A getclock that
	 * fails silently would leave it there, +5 would name five seconds
	 * after the epoch, and the join would report ETIMEDOUT the instant it
	 * was called -- a wrong answer indistinguishable from a real timeout.
	 * That is the defect this file was already carrying in four places
	 * inside libpthreads; the test must not carry it too. */
	rc = getclock(TIMEOFDAY, &deadline);
	if (rc != 0) {
		char buf[80];
		snprintf(buf, sizeof(buf),
			 "(b) getclock(TIMEOFDAY) returned %d (ENXIO %d, EIO %d)",
			 rc, ENXIO, EIO);
		test_fail("timedjoin_np", buf);
		return;
	}
	deadline.tv_sec += 5;
	rc = pthread_timedjoin_np(t, &retval, &deadline);
	if (rc != 0 || retval != (void *)0x42424242) {
		char buf[96];
		/* Name both halves: a join that timed out and a join that
		 * harvested the wrong value are different defects, and one
		 * message for the two cannot tell them apart. */
		snprintf(buf, sizeof(buf),
			 "(b) rc %d (want 0), retval %p (want 0x42424242)",
			 rc, retval);
		test_fail("timedjoin_np", buf);
		return;
	}

	test_ok("pthread_timedjoin_np");
}

/* ----------------------------------------------------------------
 * Test: pthread_setschedprio (#153)
 * ---------------------------------------------------------------- */

static void
test_setschedprio(void)
{
	pthread_t			self = pthread_self();
	struct sched_param		p;
	policy_timeshare_info_data_t	ti;
	mach_msg_type_number_t		cnt = POLICY_TIMESHARE_INFO_COUNT;
	int				policy, rc, target;

	/* Negative priority must be rejected up front. */
	if (pthread_setschedprio(self, -1) != EINVAL) {
		test_fail("setschedprio", "negative prio not rejected");
		return;
	}

	/* A wildly out-of-range priority must be rejected by the kernel. */
	if (pthread_setschedprio(self, 1000000) != EINVAL) {
		test_fail("setschedprio", "huge prio not rejected");
		return;
	}

	/*
	 * Pick a kernel-valid priority by reading the thread's current
	 * base priority: in Mach a thread may only lower its priority
	 * (numerically >= its max_priority), and the absolute range is
	 * NRQS-dependent, so re-applying the current value is the only
	 * portable "always valid" choice.
	 */
	target = -1;
	if (thread_info(mach_thread_self(), THREAD_SCHED_TIMESHARE_INFO,
			(thread_info_t)&ti, &cnt) == KERN_SUCCESS)
		target = ti.base_priority;
	if (target < 0) {
		test_fail("setschedprio", "could not read current priority");
		return;
	}

	rc = pthread_setschedprio(self, target);
	if (rc != 0) {
		char buf[80];
		snprintf(buf, sizeof(buf), "valid prio %d rc=%d", target, rc);
		test_fail("setschedprio", buf);
		return;
	}
	if (pthread_getschedparam(self, &policy, &p) != 0 ||
	    p.sched_priority != target) {
		test_fail("setschedprio", "getschedparam mismatch");
		return;
	}

	test_ok("pthread_setschedprio");
}

/* ----------------------------------------------------------------
 * Test: pthread_setschedparam / getschedparam round-trip (#273)
 * ---------------------------------------------------------------- */

static void
test_setschedparam(void)
{
	pthread_t		self = pthread_self();
	struct sched_param	p;
	int			policy, rc;

	/* The default policy must now be SCHED_OTHER (timeshare), matching
	 * the policy the kernel actually runs threads under (#273). */
	if (pthread_getschedparam(self, &policy, &p) != 0) {
		test_fail("setschedparam", "getschedparam failed");
		return;
	}
	if (policy != SCHED_OTHER) {
		char buf[80];
		snprintf(buf, sizeof(buf), "default policy %d != SCHED_OTHER",
			 policy);
		test_fail("setschedparam", buf);
		return;
	}

	/* NULL param and a bogus policy must both be rejected. */
	if (pthread_setschedparam(self, SCHED_OTHER, NULL) != EINVAL) {
		test_fail("setschedparam", "NULL param not rejected");
		return;
	}
	if (pthread_setschedparam(self, 999, &p) != EINVAL) {
		test_fail("setschedparam", "bogus policy not rejected");
		return;
	}

	/* Re-applying the current policy + base priority must take effect
	 * (write-through, not the old no-op stub) and be observable. */
	rc = pthread_setschedparam(self, SCHED_OTHER, &p);
	if (rc != 0) {
		char buf[80];
		snprintf(buf, sizeof(buf), "SCHED_OTHER apply rc=%d", rc);
		test_fail("setschedparam", buf);
		return;
	}
	if (pthread_getschedparam(self, &policy, &p) != 0 ||
	    policy != SCHED_OTHER) {
		test_fail("setschedparam", "policy not SCHED_OTHER after set");
		return;
	}

	test_ok("pthread_setschedparam");
}

/* ----------------------------------------------------------------
 * Test: PTHREAD_EXPLICIT_SCHED at thread creation + FIFO/RR enabled (#274)
 * ---------------------------------------------------------------- */

static void *
thread_report_policy(void *arg)
{
	int			*out = (int *)arg;	/* [0]=rc [1]=policy */
	struct sched_param	p;
	int			pol = -1;

	out[0] = pthread_getschedparam(pthread_self(), &pol, &p);
	out[1] = pol;
	return NULL;
}

static void
test_explicit_sched(void)
{
	pthread_attr_t		attr;
	pthread_t		th;
	struct sched_param	p;
	int			policy, rc, report[2] = { -1, -1 };

	/* FIFO/RR are now enabled in the default pset (#274): setting RR on
	 * the current thread must succeed (was ENOTSUP before). */
	pthread_getschedparam(pthread_self(), &policy, &p);
	rc = pthread_setschedparam(pthread_self(), SCHED_RR, &p);
	if (rc != 0) {
		char buf[80];
		snprintf(buf, sizeof(buf), "SCHED_RR rc=%d (FIFO/RR enabled?)",
			 rc);
		test_fail("explicit_sched", buf);
		return;
	}
	/* Put ourselves back to timesharing so the rest of the run behaves. */
	(void)pthread_setschedparam(pthread_self(), SCHED_OTHER, &p);

	/*
	 * A thread created with PTHREAD_EXPLICIT_SCHED + SCHED_RR must come
	 * up under RR.  Reuse a kernel-valid base priority (the current
	 * thread's) so the priority is in range.
	 */
	pthread_attr_init(&attr);
	pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
	pthread_attr_setschedpolicy(&attr, SCHED_RR);
	pthread_attr_setschedparam(&attr, &p);

	if (pthread_create(&th, &attr, thread_report_policy, report) != 0) {
		test_fail("explicit_sched", "pthread_create failed");
		return;
	}
	pthread_join(th, NULL);

	if (report[0] != 0) {
		test_fail("explicit_sched", "child getschedparam failed");
		return;
	}
	if (report[1] != SCHED_RR) {
		char buf[80];
		snprintf(buf, sizeof(buf), "child policy %d != SCHED_RR",
			 report[1]);
		test_fail("explicit_sched", buf);
		return;
	}

	test_ok("explicit_sched (PTHREAD_EXPLICIT_SCHED + FIFO/RR)");
}

/* ----------------------------------------------------------------
 * main
 * ---------------------------------------------------------------- */

int
main(int argc, char **argv)
{
	/* Mirror printf onto gpu_server's text plane so test output lands
	 * on the QEMU/VGA window too, not only the serial console.  Async
	 * because servers launch in parallel and gpu_server may not be
	 * netname-registered yet. */
	(void)gpu_console_init_async("pthread_test", 100u, 50u);

	printf("pthread_test: starting\n");
	test_num = 0;

	test_create_join();
	test_cond_signal();
	test_cond_broadcast();
	test_tsd();
	test_once();
	test_cancel();
	test_stacksize_attr();
	test_rwlock_func();
	test_barrier_func();
	test_spinlock_func();
	test_errno_threadsafe();
	test_mutex_errorcheck();
	test_mutex_recursive();
	test_mutex_timedlock();
	test_mutex_bench();
	test_thread_name_kernel();
	test_signals();
	test_thread_pool_bench();
	{
		extern int sched_yield(void);
		extern int pthread_yield(void);
		if (sched_yield() == 0 && pthread_yield() == 0)
			test_ok("yield (sched_yield/pthread_yield)");
		else
			test_fail("yield", "non-zero return");
	}
	test_timedjoin_np();
	test_setschedprio();
	test_setschedparam();
	test_explicit_sched();

	if (pass)
		printf("pthread_test: ALL %d TESTS PASSED\n", test_num);
	else
		printf("pthread_test: SOME TESTS FAILED\n");

	return pass ? 0 : 1;
}
