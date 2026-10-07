/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * -K: two processors call sprintf() at once (#655).
 *
 * The kernel's sprintf() wrote through one static pointer, so two calls at
 * once wrote into each other's buffers.  Here a thread bound to another
 * processor and this thread, bound where it is, each format their own string
 * into their own buffer SPRINTF_TEST_CALLS times, starting together, and check
 * every result against the text built by hand: all of it, nothing of the
 * other side's, the count sprintf() answered, and a guard byte behind the
 * buffer.
 */

#include <stdint.h>
#include <string.h>

#include <kern/cpu_number.h>
#include <kern/lock.h>			/* mutex_pause */
#include <kern/misc_protos.h>
#include <kern/processor.h>
#include <kern/sched_prim.h>
#include <kern/task.h>
#include <kern/thread.h>

#include <cpu/regs.h>			/* cpu_pause */
#include <cpu/sprintf_test.h>
#include <time/tsc.h>

#define	SPRINTF_TEST_CALLS	20000
#define	SPRINTF_TEST_PAD	48	/* the side's letter, this many times */
#define	SPRINTF_TEST_BUF	96	/* more than the longest text, 73 */
#define	SPRINTF_TEST_GUARD	0x5a
#define	SPRINTF_TEST_WAIT_S	10

static volatile int	sprintf_test_cpu;	/* where the other side goes */
static volatile int	sprintf_test_bound;	/* it is there */
static volatile int	sprintf_test_go;	/* both start */
static volatile int	sprintf_test_done;	/* the other side has finished */
static volatile int	sprintf_test_other_wrong;

/*
 * The text sprintf_test_side() asks for, put together without sprintf():
 * "sprintf_test <tag> <i> <pad>".  Answers its length.
 */
static int
sprintf_test_expected(char *out, const char *tag, int i, char letter)
{
	char	digits[12];
	int	n = 0, d = 0, k;

	memcpy(out, "sprintf_test ", 13);
	n = 13;
	for (k = 0; tag[k] != '\0'; k++)
		out[n++] = tag[k];
	out[n++] = ' ';
	do {
		digits[d++] = (char) ('0' + i % 10);
		i /= 10;
	} while (i != 0);
	while (d > 0)
		out[n++] = digits[--d];
	out[n++] = ' ';
	for (k = 0; k < SPRINTF_TEST_PAD; k++)
		out[n++] = letter;
	out[n] = '\0';
	return n;
}

/* One side's calls: answers how many came out wrong. */
static int
sprintf_test_side(char letter, const char *tag)
{
	char	buf[SPRINTF_TEST_BUF + 1];	/* the last byte is the guard */
	char	want[SPRINTF_TEST_BUF];
	char	pad[SPRINTF_TEST_PAD + 1];
	int	i, n, len, wrong = 0;

	memset(pad, letter, SPRINTF_TEST_PAD);
	pad[SPRINTF_TEST_PAD] = '\0';
	for (i = 0; i < SPRINTF_TEST_CALLS; i++) {
		memset(buf, 0, SPRINTF_TEST_BUF);
		buf[SPRINTF_TEST_BUF] = SPRINTF_TEST_GUARD;
		n = sprintf(buf, "sprintf_test %s %d %s", tag, i, pad);
		len = sprintf_test_expected(want, tag, i, letter);
		if (n != len || memcmp(buf, want, (size_t) len + 1) != 0 ||
		    buf[SPRINTF_TEST_BUF] != SPRINTF_TEST_GUARD)
			wrong++;
	}
	return wrong;
}

static void
sprintf_test_other(void)
{
	thread_bind(current_thread(), cpu_to_processor(sprintf_test_cpu));
	thread_block((void (*)(void)) 0);	/* runs next where it is bound */
	sprintf_test_bound = 1;
	while (!sprintf_test_go)
		cpu_pause();
	sprintf_test_other_wrong = sprintf_test_side('R', "right");
	sprintf_test_done = 1;
	thread_terminate_self();
}

static boolean_t
sprintf_test_wait(volatile int *v, uint64_t hz)
{
	uint64_t	t0 = rdtsc();

	while (*v == 0) {
		if (hz != 0 && rdtsc() - t0 >= hz * SPRINTF_TEST_WAIT_S)
			return FALSE;
		mutex_pause();
	}
	return TRUE;
}

void
sprintf_test(void)
{
	uint64_t	hz = tsc_hz();
	int		me, i, r = -1, ncpu = 0, wrong;

	for (i = 0; i < NCPUS; i++) {
		processor_t p = cpu_to_processor(i);

		if (p == PROCESSOR_NULL || p->state == PROCESSOR_OFF_LINE)
			continue;
		ncpu++;
	}
	if (ncpu < 2) {
		printf("sprintf_test: NOT ASKED — %d processor: nothing else can "
		       "call sprintf() while this one does (#655)\n", ncpu);
		return;
	}

	me = thread_bind_here();
	for (i = 0; i < NCPUS; i++) {
		processor_t p = cpu_to_processor(i);

		if (p != PROCESSOR_NULL && p->state != PROCESSOR_OFF_LINE &&
		    i != me)
			r = i;
	}
	sprintf_test_cpu = r;
	if (r < 0 || kernel_thread(kernel_task, sprintf_test_other,
				   (char *) 0) == THREAD_NULL) {
		printf("sprintf_test: NOT ASKED — no second thread on another "
		       "processor (#655)\n");
		thread_bind(current_thread(), PROCESSOR_NULL);
		return;
	}
	if (!sprintf_test_wait(&sprintf_test_bound, hz)) {
		printf("sprintf_test: NOT ASKED — the second thread did not "
		       "reach processor %d in %u s (#655)\n", r,
		       SPRINTF_TEST_WAIT_S);
		sprintf_test_go = 1;
		thread_bind(current_thread(), PROCESSOR_NULL);
		return;
	}

	printf("sprintf_test: starting: processors %d and %d each format their "
	       "own string %u times at once (#655)\n", me, r,
	       SPRINTF_TEST_CALLS);
	sprintf_test_go = 1;
	wrong = sprintf_test_side('L', "left");
	(void) sprintf_test_wait(&sprintf_test_done, hz);
	thread_bind(current_thread(), PROCESSOR_NULL);

	if (!sprintf_test_done)
		printf("sprintf_test: WRONG — the second thread did not finish "
		       "its calls in %u s (#655)\n", SPRINTF_TEST_WAIT_S);
	else if (wrong != 0 || sprintf_test_other_wrong != 0)
		printf("sprintf_test: WRONG — %d of %u calls on processor %d and "
		       "%d of %u on processor %d came out wrong: two sprintf() "
		       "calls wrote into each other's buffers (#655)\n", wrong,
		       SPRINTF_TEST_CALLS, me, sprintf_test_other_wrong,
		       SPRINTF_TEST_CALLS, r);
	else
		printf("sprintf_test: PASS — %u calls on each of processors %d "
		       "and %d at once, every result whole (#655)\n",
		       SPRINTF_TEST_CALLS, me, r);
}
