/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * A byte the console sends must not land in COM1's divisor latch (#599).
 *
 * ── The window ────────────────────────────────────────────────────────
 *
 * Setting a 16550's divisor is four writes: LCR with bit 7 (DLAB), DLL, DLM,
 * LCR as it was.  While DLAB is set, offset 0 is DLL and not THR, so a byte
 * written to THR in that window becomes half of the divisor.  The console
 * writes THR from any processor (com_putc), and the divisor is set for the
 * task that holds COM1 (com_set_divisor, device_io_port_set_divisor).  Both
 * now hold com_bank_lock.
 *
 * ── What this asks ────────────────────────────────────────────────────
 *
 * Processor 1 sends lines through the console without pause while processor 0
 * runs the divisor sequence again and again, writing the divisor it found and
 * reading the latch back each time.  With the lock, every read-back is the
 * value written.  Without it (UROS_ABLATE_599_COM_NO_LOCK) a byte lands between
 * DLL and the read-back, and the read-back says so; UROS_ABLATE_599_WIDEN_DIVISOR
 * holds the window open between DLL and DLM so that the ablation does not
 * depend on luck.
 *
 * ⚠️ TWO PROCESSORS, and a uniprocessor answers NOT ASKED.  The sequence runs
 * with interrupts off, so on one processor nothing can come between its
 * halves but an NMI -- which takes nothing and writes THR only with DLAB clear
 * (com_putc).  The race this closes is a race between processors.
 *
 * ⚠️ The flood is whole lines through printf, so the log stays readable: the
 * console's other writers take printf_lock too, and com_set_divisor does not,
 * which is the pair this asks about.
 */

#include <cpus.h>

#include <kern/misc_protos.h>
#include <kern/thread.h>
#include <kern/thread_swap.h>
#include <kern/sched_prim.h>
#include <kern/processor.h>
#include <kern/thread_act.h>		/* act_deallocate */
#include <kern/task.h>
#include <kern/cpu_number.h>
#include <kern/spl.h>
#include <mach/machine.h>
#include <i386/AT386/com_divisor_test.h>

extern unsigned int	com_set_divisor(unsigned int divisor);
extern unsigned int	com_get_divisor(void);

/* '-U'; in .data for model_dep.c's reason: set before the BSS is cleared */
int	com_divisor_test_wanted __attribute__((section(".data"))) = 0;

/*
 * Rounds at least, and lines of flood at least while they run: the rounds
 * are counted only once whole lines have gone out inside them, so a flood
 * that started late or stopped early reads as NOT ASKED and not as a pass.
 */
#define	CDT_ROUNDS	256
#define	CDT_LINES	4
#define	CDT_ROUNDS_MAX	1000000

static volatile int		cdt_flooding;
static volatile int		cdt_done;
static volatile unsigned int	cdt_lines;

static void
cdt_sleep(int ticks)
{
	assert_wait((event_t) &cdt_sleep, FALSE);
	thread_set_timeout(ticks);
	thread_block((void (*)(void)) 0);
}

static void
cdt_park(void)
{
	for (;;) {
		assert_wait((event_t) &cdt_park, FALSE);
		thread_block((void (*)(void)) 0);
	}
}

#if	NCPUS > 1
static void
cdt_flooder(void)
{
	cdt_flooding = 1;
	while (!cdt_done) {
		printf("com: [divisor-race] flood from processor %d "
		       "..........................................\n",
		       cpu_number());
		cdt_lines++;
	}
	cdt_park();
}
#endif	/* NCPUS > 1 */

static thread_t
cdt_thread(void (*body)(void), processor_t where)
{
	thread_t	th;
	thread_act_t	act;
	spl_t		s;

	if (thread_create_at(kernel_task, &th, body) != KERN_SUCCESS)
		return THREAD_NULL;
	thread_swappable(th->top_act, FALSE);

	s = splsched();
	thread_lock(th);
	act = th->top_act;
	th->max_priority = BASEPRI_SYSTEM;
	th->priority = BASEPRI_SYSTEM;
	th->sched_pri = BASEPRI_SYSTEM;
	thread_bind_locked(th, where);	/* before it can run anywhere */
	th->state |= TH_RUN;
	thread_setrun(th, TRUE, TAIL_Q);
	thread_unlock(th);
	splx(s);

	act_deallocate(act);
	thread_resume(act);
	return th;
}

static void
cdt_driver(void)
{
#if	NCPUS > 1
	unsigned int	divisor, got, last = 0, rounds, wrong = 0;
	unsigned int	lines0, lines1;
	int		i;

	/*
	 * Processor 1 comes up at the end of start_kernel_threads(), after
	 * this thread was made.  Thirty seconds in tenths; a processor that
	 * never came is the answer for this boot, not a wait for ever.
	 */
	for (i = 0; i < 300 && !machine_slot[1].running; i++)
		cdt_sleep(hz / 10);
	if (!machine_slot[1].running) {
		printf("com: [divisor-race] NOT ASKED — processor 1 did not come "
		       "up, and the race is between processors (#599)\n");
		cdt_park();
	}
	if (cdt_thread(cdt_flooder, cpu_to_processor(1)) == THREAD_NULL) {
		printf("com: [divisor-race] NOT ASKED — no thread for the flood "
		       "(#599)\n");
		cdt_park();
	}
	for (i = 0; i < 300 && !cdt_flooding; i++)
		cdt_sleep(hz / 10);
	if (!cdt_flooding) {
		printf("com: [divisor-race] NOT ASKED — processor 1 never ran the "
		       "flood (#599)\n");
		cdt_done = 1;
		cdt_park();
	}

	divisor = com_get_divisor();
	lines0 = cdt_lines;
	for (rounds = 0; rounds < CDT_ROUNDS_MAX; rounds++) {
		if (rounds >= CDT_ROUNDS && cdt_lines - lines0 >= CDT_LINES)
			break;
		got = com_set_divisor(divisor);
		if (got != divisor) {
			wrong++;
			last = got;
		}
	}
	lines1 = cdt_lines;
	cdt_done = 1;
	cdt_sleep(hz / 10);			/* the flood's last line */
	if (wrong != 0)
		(void) com_set_divisor(divisor);	/* put it back, alone */

	if (lines1 - lines0 < CDT_LINES)
		printf("com: [divisor-race] NOT ASKED — %u rounds and only %u "
		       "lines of flood went out inside them (#599)\n",
		       rounds, lines1 - lines0);
	else if (wrong == 0)
		printf("com: [divisor-race] PASS — %u divisor sequences on "
		       "processor %d while processor 1 sent %u lines: every "
		       "one read back 0x%04x (#599)\n",
		       rounds, cpu_number(), lines1 - lines0, divisor);
	else
		printf("com: [divisor-race] WRONG — %u of %u divisor sequences "
		       "read back something other than 0x%04x (last 0x%04x) "
		       "while processor 1 sent %u lines: the console's bytes "
		       "went into the latch (#599)\n",
		       wrong, rounds, divisor, last, lines1 - lines0);
#else	/* NCPUS > 1 */
	printf("com: [divisor-race] NOT ASKED — a uniprocessor build: the "
	       "sequence runs with interrupts off, and nothing but an NMI can "
	       "come between its halves (#599)\n");
#endif	/* NCPUS > 1 */
	cdt_park();
}

void
com_divisor_test_start(void)
{
	if (cdt_thread(cdt_driver, cpu_to_processor(master_cpu)) == THREAD_NULL)
		printf("com: [divisor-race] NOT ASKED — no thread to drive it "
		       "(#599)\n");
}
