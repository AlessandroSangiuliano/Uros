/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * pthread_min.c — minimal musl pthread regression (#291).
 *
 * A static musl binary that creates a worker thread, has it run + return a
 * value, joins it, and reports.  Proves the musl threading path works end
 * to end without any per-binary -Wl,-u workaround: with the weak
 * uros_syscall_stub.lo stripped from libc-musl.a (#291), the strong
 * __uros_clone / __uros_syscallN from libposix-uros are pulled
 * automatically, so pthread_create no longer fails with EAGAIN.
 *
 * Also does a round of mutex lock/unlock across a few threads so the
 * mutex/condvar futex path is exercised too.
 */

#include <stdio.h>
#include <pthread.h>

#define NTHREADS  4
#define ITERS     1000

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static volatile long   counter;

static void *
worker(void *arg)
{
    long id = (long)arg;
    for (int i = 0; i < ITERS; i++) {
        pthread_mutex_lock(&mtx);
        counter++;
        pthread_mutex_unlock(&mtx);
    }
    return (void *)(id + 1);
}

int
main(void)
{
    pthread_t t[NTHREADS];
    int created = 0, joined = 0, first_err = 0;

    for (long i = 0; i < NTHREADS; i++) {
        int rc = pthread_create(&t[i], 0, worker, (void *)i);
        if (rc == 0) {
            created++;
        } else {
            if (!first_err) first_err = rc;
            t[i] = 0;
        }
    }
    for (int i = 0; i < NTHREADS; i++) {
        void *ret = 0;
        if (t[i] && pthread_join(t[i], &ret) == 0)
            joined++;
    }

    printf("pthread_min: created=%d joined=%d counter=%ld first_err=%d\n",
           created, joined, counter, first_err);
    if (created == NTHREADS && joined == NTHREADS &&
        counter == (long)NTHREADS * ITERS)
        printf("pthread_min: PASS\n");
    else
        printf("pthread_min: FAIL\n");
    return 0;
}
