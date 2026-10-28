/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The console's marks, asked of the console itself at every boot (#757).
 *
 * printf_lines_selftest() asks the rules of the marks of printf.c's own
 * functions, with writers it makes up and a buffer of its own.  What it cannot
 * see is whether the console's writers use them: printf_writer_take() and
 * printf_writer_give(), through which printf() and consolewrite() take their
 * turns, could stop calling the rules and the self-test would still pass.  Nor
 * would a boot say so.  Once a report is a block nothing on these boots prints
 * into another writer's line -- no line was cut in 12 boots, two of entry 0
 * and ten of the bench at four processors under KVM -- so a console that
 * spliced lines in silence again would print the same logs.
 *
 * So here two threads write on the console itself, and its ring is read back.
 * Two rounds, each through both ways in:
 *
 *   1. this thread begins a line with printf(), another thread writes a whole
 *      line with consolewrite() -- a task's write(), without the task -- and
 *      this one ends its line with printf();
 *   2. the same, with the two ways swapped.
 *
 * Each round must leave in the ring this thread's beginning closed with
 * " <-cut [t:...]" naming this thread, the other thread's line whole at the
 * start of a line, and this thread's end behind "[t:...]-> " naming it again.
 * Anybody else who prints meanwhile may make the cut in place of the other
 * thread; the marks are the same either way.
 *
 * ⚠️ Not the raw writer -- the debugger's, a nested printf's -- which no boot
 * can be made to produce on demand.  Its rules are the self-test's.
 */

#include <string.h>

#include <device/console_cdev.h>
#include <device/io_req.h>
#include <kern/klog.h>
#include <kern/lock.h>			/* mutex_pause() */
#include <kern/misc_protos.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/time_out.h>		/* hz */

#include <cpu/console_marks_test.h>

#define	CONSOLE_MARKS_WAIT_S	5	/* for the other thread's line */
#define	CONSOLE_MARKS_TEXT	96	/* more than the longest text, 59 */
#define	CONSOLE_MARKS_MARKED	160	/* a text and its mark */
#define	CONSOLE_MARKS_RING	4096	/* how much of the ring is read back */
#define	CONSOLE_MARKS_SHOWN	240	/* how much of it a WRONG quotes */

static volatile int	console_marks_round;	/* 1 or 2 */
static volatile int	console_marks_written;	/* the other line is out */
static char		console_marks_ring[CONSOLE_MARKS_RING];

/* The two ways in: printf(), or a task's write() as the console device has it. */
static void
console_marks_put(boolean_t by_printf, char *s)
{
	struct io_req	ior;

	if (by_printf) {
		printf("%s", s);
		return;
	}
	memset(&ior, 0, sizeof ior);
	ior.io_op = IO_WRITE | IO_INBAND;
	ior.io_data = s;
	ior.io_count = (io_buf_len_t) strlen(s);
	(void) consolewrite(0, &ior);
}

/* What a round writes: this thread's beginning and end, the other's line. */
static void
console_marks_texts(int round, char *begin, char *other, char *end)
{
	const char	*mine = round == 1 ? "printf()" : "consolewrite()";
	const char	*theirs = round == 1 ? "consolewrite()" : "printf()";

	if (begin != 0)
		sprintf(begin, "console_marks: %d: a line begun by %s,", round,
			mine);
	if (other != 0)
		sprintf(other, "console_marks: %d: another thread's line, by %s\n",
			round, theirs);
	if (end != 0)
		sprintf(end, " and ended by %s\n", mine);
}

static void
console_marks_other(void)
{
	char	line[CONSOLE_MARKS_TEXT];
	int	round = console_marks_round;

	console_marks_texts(round, 0, line, 0);
	console_marks_put(round == 2, line);
	console_marks_written = 1;
	thread_terminate_self();
}

/* Where `what' begins in the first n bytes read back, from `from' on; or -1. */
static int
console_marks_find(int n, int from, const char *what)
{
	int	len = (int) strlen(what), i;

	for (i = from; i + len <= n; i++)
		if (memcmp(console_marks_ring + i, what, (size_t) len) == 0)
			return i;
	return -1;
}

void
console_marks_test(void)
{
	char			begin[CONSOLE_MARKS_TEXT], end[CONSOLE_MARKS_TEXT];
	char			other[CONSOLE_MARKS_TEXT + 1], name[24];
	char			cut[CONSOLE_MARKS_MARKED];
	char			rest[CONSOLE_MARKS_MARKED];
	const char		*missing = 0;
	natural_t		from, next;
	mach_msg_type_number_t	n;
	int			round, bad = 0, at = 0, c, o, r, i, t;
	boolean_t		made;

	sprintf(name, "t:%lx", (unsigned long) current_thread());
	from = klog_cursor();

	for (round = 1; round <= 2; round++) {
		console_marks_texts(round, begin, 0, end);
		console_marks_round = round;
		console_marks_written = 0;
		console_marks_put(round == 1, begin);

		/*
		 * Asked, not waited for.  A wait declared here would be one
		 * kernel_thread() takes a mutex inside, which this kernel
		 * panics on (#624, #687) -- the first boot of this probe did.
		 * mutex_pause() sleeps a tick, so on one processor the other
		 * thread runs meanwhile.
		 */
		made = kernel_thread(kernel_task, console_marks_other,
				     (char *) 0) != THREAD_NULL;
		for (t = 0; made && !console_marks_written &&
		     t < CONSOLE_MARKS_WAIT_S * hz; t++)
			mutex_pause();

		console_marks_put(round == 1, end);
		if (!console_marks_written) {
			printf("console_marks: NOT ASKED -- round %d's other "
			       "thread %s, so nothing cut this thread's line "
			       "(#757)\n", round,
			       made ? "did not write in time" : "was not made");
			return;
		}
	}

	n = CONSOLE_MARKS_RING - 1;
	(void) klog_read(from, console_marks_ring, &n, &next);

	for (round = 1; round <= 2 && bad == 0; round++) {
		other[0] = '\n';	/* the other line starts a line */
		console_marks_texts(round, begin, other + 1, end);
		sprintf(cut, "%s <-cut [%s]\n", begin, name);
		sprintf(rest, "[%s]-> %s", name, end);

		c = console_marks_find((int) n, at, cut);
		o = c < 0 ? -1 : console_marks_find((int) n,
						      c + (int) strlen(cut) - 1, other);
		r = o < 0 ? -1 : console_marks_find((int) n,
						      o + (int) strlen(other), rest);
		if (c < 0)
			missing = "beginning closed with the mark naming this thread";
		else if (o < 0)
			missing = "line of the other thread's, whole, after the cut";
		else if (r < 0)
			missing = "end behind the mark naming this thread";
		else {
			at = r + (int) strlen(rest);
			continue;
		}
		bad = round;
	}

	if (bad == 0) {
		printf("console_marks: a line of thread %s begun by printf() and "
		       "one begun by consolewrite() were each cut by another "
		       "thread's, and each went on behind its mark -- PASS "
		       "(#757)\n", name);
		return;
	}

	/* What the ring held from the round on, as one line: '|' a newline. */
	for (i = at; i < (int) n; i++)
		if (console_marks_ring[i] == '\n')
			console_marks_ring[i] = '|';
	if ((int) n - at > CONSOLE_MARKS_SHOWN)
		n = (mach_msg_type_number_t) (at + CONSOLE_MARKS_SHOWN);
	console_marks_ring[n] = '\0';
	printf("console_marks: round %d: no %s in the ring, which held \"%s\" "
	       "-- WRONG (#757)\n", bad, missing, console_marks_ring + at);
}
