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
#include <signal.h>
#include <mach/port.h>

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

static void *
thread_timedlock_holder(void *arg)
{
	/* Hold the mutex for a while, then release */
	pthread_mutex_lock(&timedlock_mtx);
	thread_switch(MACH_PORT_NULL, SWITCH_OPTION_DEPRESS, 50);
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
	getclock(TIMEOFDAY, &deadline);
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
	pthread_create(&holder, NULL, thread_timedlock_holder, NULL);
	/* Let holder grab the lock */
	thread_switch(MACH_PORT_NULL, SWITCH_OPTION_DEPRESS, 10);

	/* Set deadline in the past */
	deadline.tv_sec = 0;
	deadline.tv_nsec = 0;
	rc = pthread_mutex_timedlock(&timedlock_mtx, &deadline);
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
 * Test 15: mutex fast-path benchmark
 * ---------------------------------------------------------------- */

static void
test_mutex_bench(void)
{
	pthread_mutex_t bench_mtx = PTHREAD_MUTEX_INITIALIZER;
	unsigned int start, end;
	int i;
	int n = 100000;

	/* rdtsc for timing */
	__asm__ volatile("rdtsc" : "=a"(start) : : "edx");
	for (i = 0; i < n; i++) {
		pthread_mutex_lock(&bench_mtx);
		pthread_mutex_unlock(&bench_mtx);
	}
	__asm__ volatile("rdtsc" : "=a"(end) : : "edx");

	unsigned int cycles = end - start;
	printf("  [%d] mutex uncontended: %u cycles / %d iters = %u cycles/pair\n",
	       ++test_num, cycles, n, cycles / n);
}

/* ----------------------------------------------------------------
 * Test 16: thread name kernel propagation
 * ---------------------------------------------------------------- */

extern kern_return_t mach_thread_set_name(const char *name);

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
 * main
 * ---------------------------------------------------------------- */

int
main(int argc, char **argv)
{
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

	if (pass)
		printf("pthread_test: ALL %d TESTS PASSED\n", test_num);
	else
		printf("pthread_test: SOME TESTS FAILED\n");

	return pass ? 0 : 1;
}
