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
 * Minimal POSIX Threads test for Uros.
 *
 * Tests: pthread_create, pthread_join, pthread_mutex, pthread_self.
 * Runs as a Mach standalone server (loaded by bootstrap).
 */

#include <pthread.h>
#include <stdio.h>

#define NTHREADS	4
#define ITERATIONS	1000

static pthread_mutex_t counter_mutex = PTHREAD_MUTEX_INITIALIZER;
static int shared_counter;

static void *
thread_func(void *arg)
{
	int id = (int)(long)arg;
	int i;

	printf("pthread_test: thread %d started (self=%x)\n",
	       id, (unsigned int)pthread_self());

	for (i = 0; i < ITERATIONS; i++) {
		pthread_mutex_lock(&counter_mutex);
		shared_counter++;
		pthread_mutex_unlock(&counter_mutex);
	}

	printf("pthread_test: thread %d done\n", id);
	return (void *)(long)(id * 10);
}

int
main(int argc, char **argv)
{
	pthread_t threads[NTHREADS];
	void *retval;
	int i, rc;
	int expected = NTHREADS * ITERATIONS;
	int pass = 1;

	printf("pthread_test: starting (%d threads, %d iterations)\n",
	       NTHREADS, ITERATIONS);

	/* Test 1: create + join */
	for (i = 0; i < NTHREADS; i++) {
		rc = pthread_create(&threads[i], NULL, thread_func,
				    (void *)(long)i);
		if (rc != 0) {
			printf("pthread_test: FAIL pthread_create[%d] rc=%d\n",
			       i, rc);
			pass = 0;
		}
	}

	for (i = 0; i < NTHREADS; i++) {
		rc = pthread_join(threads[i], &retval);
		if (rc != 0) {
			printf("pthread_test: FAIL pthread_join[%d] rc=%d\n",
			       i, rc);
			pass = 0;
		} else if ((int)(long)retval != i * 10) {
			printf("pthread_test: FAIL thread %d retval=%d "
			       "expected=%d\n",
			       i, (int)(long)retval, i * 10);
			pass = 0;
		}
	}

	/* Test 2: mutex correctness */
	if (shared_counter != expected) {
		printf("pthread_test: FAIL counter=%d expected=%d\n",
		       shared_counter, expected);
		pass = 0;
	} else {
		printf("pthread_test: mutex OK counter=%d\n", shared_counter);
	}

	/* Summary */
	if (pass)
		printf("pthread_test: ALL TESTS PASSED\n");
	else
		printf("pthread_test: SOME TESTS FAILED\n");

	return pass ? 0 : 1;
}
