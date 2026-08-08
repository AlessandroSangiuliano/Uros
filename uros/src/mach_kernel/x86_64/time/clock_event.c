/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 */

/*
 * clock_event.c — the two backends, and how one is chosen (#459).
 *
 * Design rationale lives in clock_event.h.  What is here is the arithmetic,
 * which is where a timer is actually got wrong.
 */

#include <x86_64/time/clock_event.h>
#include <x86_64/time/tsc.h>
#include <x86_64/cpu/lapic.h>
#include <x86_64/cpu/regs.h>
#include <x86_64/boot/bootarg.h>
#include <kern/misc_protos.h>		/* printf */
#include <ddb/ddb.h>		/* #428: the console door */
#include <kern/time_out.h>		/* hertz_tick -- the whole point */
#include <sync/barrier.h>		/* #461: publish the tick reports */
#include <kern/cpu_number.h>		/* cpu_number */
#include <kern/cpu_data.h>		/* #459: disable_preemption */
#include <cpus.h>			/* NCPUS */
#include <trap/trap.h>			/* struct trap_frame */
#include <mach/machine/vm_types.h>	/* vm_offset_t */
#include <cpu/regs.h>			/* cpu_pause */
#include <kern/ast.h>			/* #459 TEMP */

/* ------------------------------------------------------------------ rate */

/*
 * The scheduler tick.
 *
 * 100 Hz, and chosen rather than inherited: on i386 HZ is #defined in
 * i386/AT386/fdreg.h -- the floppy driver's header -- which is not a decision,
 * it is where the constant happened to land.
 *
 * Why not faster.  Measured on i386 with the same shape of handler:
 * hertz_tick() costs 2786 cycles, 931 ns at 2.99 GHz.  At 100 Hz that is
 * 0.0093% of a processor; at 1000 Hz, 0.093%.  So throughput is not what
 * decides -- at either rate the tick is noise.  What decides is that the cost
 * is paid PER PROCESSOR: this machine is built for 64 of them, each with its
 * own timer, and a rate ten times higher is ten times as many processors woken
 * out of HLT (#357) to be told there is nothing to do.
 *
 * Why not slower.  10 ms is already a coarse quantum; below that the tick
 * stops being able to express the scheduler's own accounting.
 *
 * ⚠️ And the number matters less than it looks, because the tick is a
 * FALLBACK here, not the heartbeat: an idle processor arms for the next real
 * deadline instead of for the next tick.  The rate is what a busy processor
 * gets, not what an idle one costs.
 */
#define	CLOCK_EVENT_HZ		100u
#define	NS_PER_SEC		1000000000ULL

static unsigned		event_hz = CLOCK_EVENT_HZ;
static uint64_t		tick_ns;
static uint8_t		event_vector;

/* ------------------------------------------------------ tsc-deadline ---- */

#define	MSR_IA32_TSC_DEADLINE	0x6E0
#define	CPUID_01_ECX_TSC_DEADLINE (1u << 24)

static uint64_t		tsc_per_ns_num;	/* tsc_hz, as a numerator */

static int tscdl_probe(void)
{
	uint32_t a, b, c, d;

	/*
	 * Two conditions, and both are needed for a REASON, not for symmetry:
	 * the CPUID bit says the MSR exists, and a calibrated tsc_hz says we
	 * can turn nanoseconds into counter ticks.  Without the second, the
	 * MSR would be written with a number that means nothing.
	 */
	cpuid(1, &a, &b, &c, &d);
	if ((c & CPUID_01_ECX_TSC_DEADLINE) == 0) {
		/*
		 * Which of the two failed, said out loud.  "unavailable" alone
		 * is the shape of message that sends someone to read the wrong
		 * half: the CPUID bit is a property of the machine (or of what
		 * the emulator was told to advertise), the calibration is a
		 * property of our own boot, and they are fixed in different
		 * places.
		 */
		printf("clock_event: tsc-deadline: CPUID.01H:ECX[24] clear — "
		       "the MSR does not exist here (try -cpu max, or "
		       "-cpu host,+tsc-deadline: KVM emulates it even where "
		       "the silicon has none)\n");
		return 0;
	}
	if (tsc_hz() == 0) {
		printf("clock_event: tsc-deadline: the MSR exists but tsc_hz "
		       "is 0 — nothing to convert nanoseconds with, so the "
		       "deadline would be written in units of nothing\n");
		return 0;
	}

	tsc_per_ns_num = tsc_hz();
	return 1;
}

static void tscdl_setup(uint8_t vector)
{
	/*
	 * The APIC timer LVT still selects the vector in TSC-deadline mode --
	 * the deadline replaces the countdown, not the delivery path.  Mode
	 * bits 18:17 == 10b.
	 */
	lapic_timer_deadline_setup(vector);

	/* Disarm: a stale deadline from before this processor was configured
	 * would fire once, immediately, and be indistinguishable from a real
	 * tick arriving impossibly early. */
	wrmsr(MSR_IA32_TSC_DEADLINE, 0);
}

static int tscdl_arm(uint64_t ns)
{
	uint64_t ticks, now;

	if (tsc_per_ns_num == 0)
		return 0;

	/*
	 * ticks = ns * tsc_hz / 1e9, computed so that neither end overflows.
	 *
	 * ⚠️ ns * tsc_hz overflows 64 bits for ns beyond a few seconds at
	 * gigahertz rates, so the division comes first for the seconds part
	 * and second for the remainder.  A timer that silently wraps arms for
	 * a moment in the past, which fires immediately, forever -- a livelock
	 * that reads as "the clock is too fast".
	 */
	ticks = (ns / NS_PER_SEC) * tsc_per_ns_num
	      + ((ns % NS_PER_SEC) * tsc_per_ns_num) / NS_PER_SEC;
	if (ticks == 0)
		ticks = 1;		/* never arm for "now or earlier" */

	now = rdtsc();
	wrmsr(MSR_IA32_TSC_DEADLINE, now + ticks);
	return 1;
}

static void tscdl_stop(void)
{
	/* Zero disarms the deadline: architecturally defined, unlike the APIC
	 * countdown where zero also means stopped but by a different route. */
	wrmsr(MSR_IA32_TSC_DEADLINE, 0);
}

static const struct clock_event_ops tscdl_ops = {
	"tsc-deadline", tscdl_probe, tscdl_setup, tscdl_arm, tscdl_stop
};

/* ----------------------------------------------------- lapic one-shot --- */

static int lapic_probe_ev(void)
{
	return lapic_present() && lapic_timer_hz() != 0;
}

static void lapic_setup_ev(uint8_t vector)
{
	lapic_timer_oneshot_setup(vector);
}

static int lapic_arm(uint64_t ns)
{
	uint64_t hz = lapic_timer_hz();
	uint64_t count;

	if (hz == 0)
		return 0;

	/* Same split as above, same reason. */
	count = (ns / NS_PER_SEC) * hz + ((ns % NS_PER_SEC) * hz) / NS_PER_SEC;
	if (count == 0)
		count = 1;
	/*
	 * The countdown is 32 bits.  An interval longer than it can express is
	 * clamped rather than truncated: waking early is a wasted interrupt,
	 * waking after a wrap is a deadline missed by minutes.
	 */
	if (count > 0xFFFFFFFFULL)
		count = 0xFFFFFFFFULL;

	return lapic_timer_oneshot_arm((uint32_t)count);
}

static void lapic_stop_ev(void)
{
	lapic_timer_stop();
}

static const struct clock_event_ops lapic_ops = {
	"lapic-oneshot", lapic_probe_ev, lapic_setup_ev, lapic_arm,
	lapic_stop_ev
};

/* --------------------------------------------------------- selection ---- */

static const struct clock_event_ops *ops;

/*
 * Preference order, best first.  Kept as a table so that adding a backend is
 * adding a row, and so the boot log can say what was rejected as well as what
 * won -- "why did it pick that one" is a question that gets asked at 2am.
 */
static const struct clock_event_ops * const backends[] = {
	&tscdl_ops,
	&lapic_ops,
};

void
clock_event_init(uint8_t vector)
{
	unsigned i;
	int forced_lapic;

	tick_ns = NS_PER_SEC / event_hz;
	event_vector = vector;

	/*
	 * -T forces the LAPIC backend even where the deadline works, so the
	 * two can be compared on the SAME binary.  An A/B across two builds
	 * measures the builds as much as the change.
	 */
	forced_lapic = boot_flag('T');

	ops = (const struct clock_event_ops *) 0;
	for (i = 0; i < sizeof(backends) / sizeof(backends[0]); i++) {
		const struct clock_event_ops *b = backends[i];
		int usable = b->probe();

		if (forced_lapic && b == &tscdl_ops) {
			printf("clock_event: %s %s, skipped by -T\n",
			       b->name, usable ? "available" : "unavailable");
			continue;
		}
		printf("clock_event: %s %s\n",
		       b->name, usable ? "available" : "unavailable");
		if (usable && ops == (const struct clock_event_ops *) 0)
			ops = b;
	}

	if (ops == (const struct clock_event_ops *) 0) {
		/*
		 * Loud, and not a fallback.  A kernel whose clock never fires
		 * boots, runs the first thread and then stops scheduling --
		 * which on the console is indistinguishable from a hang, and
		 * that is precisely the failure #459 exists to end.
		 */
		panic("clock_event: no usable timer backend — the scheduler "
		      "would never preempt anything (#459)");
	}

	printf("clock_event: using %s at %u Hz (%llu ns per tick)\n",
	       ops->name, event_hz, (unsigned long long) tick_ns);
}

void
clock_event_setup_cpu(void)
{
	if (ops)
		ops->setup(event_vector);
}

int
clock_event_arm_ns(uint64_t ns)
{
	return ops ? ops->arm(ns) : 0;
}

int
clock_event_arm_tick(void)
{
	return clock_event_arm_ns(tick_ns);
}

void
clock_event_stop(void)
{
	if (ops)
		ops->stop();
}

const char *
clock_event_name(void)
{
	return ops ? ops->name : "none";
}

unsigned
clock_event_hz(void)
{
	return event_hz;
}

uint64_t
clock_event_tick_ns(void)
{
	return tick_ns;
}

/* ------------------------------------------------------- burn-in ------- */

/*
 * Does the clock keep time, and for how long does it keep keeping it?
 *
 * `first tick arrived' proves the wire; it says nothing about the rate.  And
 * the rate cannot be measured from the ordinary boot, because this kernel
 * currently stops at bootstrap_create (#422) within a few milliseconds of
 * starting its first thread -- less than one tick.  So the measurement gets a
 * boot of its own, under -C, before the machine-independent kernel is entered.
 *
 * ⚠️ Its own handler, which does NOT call hertz_tick().  There is no current
 * thread here to charge time to, and calling it would be measuring a crash.
 * What this checks is everything below hertz_tick -- arm, deliver, re-arm --
 * against the TSC, which is a different counter driven by a different thing
 * than the APIC bus clock, so their agreement is evidence rather than a
 * tautology.
 */
static volatile unsigned long	burnin_ticks;

static void
burnin_handler(struct trap_frame *frame)
{
	(void) frame;
	burnin_ticks++;
	lapic_eoi();
	(void) clock_event_arm_tick();

	/*
	 * _no_check: the balanced re-enable, WITHOUT the preemption check it
	 * would otherwise run.  Taking the switch here would put it back
	 * inside the interrupt, which is the thing being avoided; the AST
	 * raised above is taken on the way out of the trap instead.
	 */
	enable_preemption_no_check();
}

void
clock_event_burnin(unsigned seconds)
{
	uint64_t	t0, t1, expect, got;
	unsigned long	want = (unsigned long) event_hz * seconds;

	if (tsc_hz() == 0) {
		printf("clock_event: burn-in skipped — no calibrated TSC to "
		       "check the tick against, and checking a timer against "
		       "itself proves nothing\n");
		return;
	}

	trap_set_handler(event_vector, burnin_handler);
	clock_event_setup_cpu();
	burnin_ticks = 0;

	t0 = rdtsc();
	if (!clock_event_arm_tick()) {
		printf("clock_event: burn-in could not arm %s\n",
		       clock_event_name());
		return;
	}

	/*
	 * Interrupts on, and a bounded wait.  Bounded against the TSC and not
	 * against the tick count, because the failure being looked for is
	 * exactly "the tick stops arriving" -- a loop that waited for ticks
	 * would hang on the bug instead of reporting it.
	 */
	__asm__ __volatile__("sti");
	expect = tsc_hz() * (uint64_t) seconds;
	do {
		cpu_pause();
		t1 = rdtsc();
	} while (t1 - t0 < expect);
	__asm__ __volatile__("cli");

	clock_event_stop();
	got = burnin_ticks;

	printf("clock_event: burn-in %s — %llu ticks in %u s, expected %lu "
	       "(%llu per mille)\n",
	       clock_event_name(), (unsigned long long) got, seconds, want,
	       (unsigned long long) (want ? (got * 1000ULL) / want : 0));

	if (got == 0)
		panic("clock_event: the timer armed and never fired — the "
		      "kernel would have no clock at all (#459)");
}

/* ------------------------------------------------------- the tick ------ */

/*
 * The wire from the timer to the kernel (#459).
 *
 * This is what did not exist: hertz_tick() had zero callers on x86-64, so no
 * quantum was ever decremented, no AST was ever raised, and the scheduler --
 * up, and having run the first thread -- would never have taken a processor
 * back.
 *
 * ⚠️ THE FRAME MAY BE A STUB.  trap.h's contract: a handler reached through
 * an spl deferral is handed a frame with only `vector` valid; the interrupt it
 * stands for happened at a moment whose registers are gone.  This handler
 * reads cs and rip, so it must cope with their absence rather than believe
 * zeros -- a cs of zero would read as ring 0 and charge userland's time to the
 * kernel, quietly, forever.
 *
 * i386 solved the same problem with a per-CPU masked_pc[] captured when the
 * tick was deferred (hardclock.c).  Doing the same here means reaching into
 * the spl path, which belongs to #454; until then a replayed tick is counted
 * as kernel time with pc 0 AND COUNTED SEPARATELY, so the accounting says how
 * much of itself is approximate instead of hiding it.
 */
unsigned long	clock_tick_replayed[NCPUS];
unsigned long	clock_tick_delivered[NCPUS];

/*
 * Does the clock actually tick, and at the rate it claims?
 *
 * Asked because everything else here only shows that the code RUNS: the boot
 * reaching setup_main proves the timer was armed, not that a single interrupt
 * ever arrived.  A clock that is armed and silent looks exactly like a clock
 * that works until something needs preempting.
 *
 * Checked against the TSC, which is a different counter driven by a different
 * thing than the APIC's bus clock -- so agreement between them is evidence,
 * where the timer counting its own ticks would only be a tautology.
 *
 * One report per processor, at one second's worth of ticks.  Not repeated:
 * this answers "did it start and is the rate right", and a line per second
 * forever would bury every other message on the console.
 */
static unsigned long	selftest_ticks[NCPUS];
static uint64_t		selftest_tsc0[NCPUS];

/*
 * 🔥 RECORDED HERE, PRINTED SOMEWHERE ELSE, AND THAT IS NOT TIDINESS (#461).
 *
 * This used to call printf() directly, from the timer interrupt handler.  It
 * deadlocked the machine.
 *
 * printf() takes printf_lock.  A thread holding it can be interrupted -- the
 * timer is class 15 on this machine, deliberately unmaskable, so it arrives
 * wherever the processor happens to be -- and the handler then reaches for the
 * same lock, on the same processor, with IF already cleared by the gate.  It
 * waits for a lock that only it could release.  The other processors pile onto
 * the same lock the next time any of them prints, and the machine stops: four
 * processors, one instruction, interrupts off.  Measured at three boots in six
 * of the #461 test; the mid-message truncation in the log was the holder being
 * caught in the act.
 *
 * A lock shared between thread context and interrupt context has to be taken
 * with interrupts off on BOTH sides, and printf_lock is not -- so the rule
 * that applies is the older and simpler one: a timer interrupt handler does
 * not print.  It leaves a note, and a thread reads it.
 *
 * The bits are published after the data with a write barrier, and read before
 * it with a read one, so a reader that sees the flag sees the numbers.
 */
#define CLOCK_REPORT_FIRST	0x1
#define CLOCK_REPORT_RATE	0x2

struct clock_report {
	volatile uint32_t	pending;
	uint64_t		elapsed;
};

static struct clock_report clock_report[NCPUS];

static void
clock_selftest(unsigned cpu)
{
	unsigned long	n;

	if (cpu >= NCPUS || tsc_hz() == 0)
		return;

	n = ++selftest_ticks[cpu];
	if (n == 1) {
		selftest_tsc0[cpu] = rdtsc();
		/*
		 * Two questions, not one, and the first has to be answered on
		 * its own: DID a tick arrive.  Reporting only a rate after a
		 * second of them means a boot that ends sooner says nothing --
		 * and silence there is indistinguishable from a clock that
		 * never ticked at all.
		 *
		 * Per processor, because the claim is per processor.  Scheduler
		 * accounting on 64 processors cannot be one CPU\'s business, and
		 * i386 shows what the alternative costs: its boot processor
		 * receives no LAPIC tick at all and takes its accounting from a
		 * device the whole machine shares.
		 */
		smp_wmb();
		clock_report[cpu].pending |= CLOCK_REPORT_FIRST;
		return;
	}
	/*
	 * The rate, over as many ticks as the boot lives long enough to give.
	 * A tenth of a second is enough to catch a rate that is wrong by a
	 * factor, which is the failure worth catching early; a full second is
	 * longer than this kernel currently runs before it stops at #422.
	 */
	if (n != (unsigned long) (event_hz / 10) + 1)
		return;

	clock_report[cpu].elapsed = rdtsc() - selftest_tsc0[cpu];
	smp_wmb();
	clock_report[cpu].pending |= CLOCK_REPORT_RATE;
}

void
clock_event_drain_reports(void)
{
	unsigned	cpu;

	/*
	 * ⚠️ Thread context only.  This prints, and printing is exactly what the
	 * handler may not do -- calling this from an interrupt would put the
	 * deadlock back where it was found.
	 *
	 * Any processor may drain any other\'s: the numbers are written once by
	 * their owner and never changed, and the flag is what says they are
	 * there.  Cleared before printing rather than after, so that two
	 * processors draining at once produce one report rather than two.
	 */
	for (cpu = 0; cpu < NCPUS; cpu++) {
		uint32_t	bits = clock_report[cpu].pending;
		uint64_t	elapsed, expect;

		if (bits == 0)
			continue;

		smp_rmb();
		clock_report[cpu].pending &= ~bits;

		if (bits & CLOCK_REPORT_FIRST)
			printf("clock_event: cpu %u — first tick arrived (%s)\n",
			       cpu, clock_event_name());

		if ((bits & CLOCK_REPORT_RATE) == 0)
			continue;

		elapsed = clock_report[cpu].elapsed;
		expect = tsc_hz() / 10;		/* a tenth of a second of it */

		/*
		 * Reported as a ratio in tenths of a percent rather than as a
		 * verdict.  A tolerance here would be a number invented at the
		 * desk: under TCG the guest\'s TSC and its emulated APIC are not
		 * driven by the same thing at all.  The number is printed so it
		 * can be READ.
		 */
		printf("clock_event: cpu %u — %u ticks took %llu TSC, a tenth of "
		       "a second is %llu (%llu per mille of nominal), backend "
		       "%s\n", cpu, event_hz / 10,
		       (unsigned long long) elapsed, (unsigned long long) expect,
		       (unsigned long long) (expect ? (elapsed * 1000ULL) / expect
						   : 0),
		       clock_event_name());
	}
}

void
clock_event_tick(struct trap_frame *frame)
{
	unsigned	cpu = cpu_number();
	boolean_t	usermode;
	vm_offset_t	pc;

	/*
	 * A real frame always has a code segment with the selector's index
	 * set; a stub frame is all zeros.  Distinguishing them on cs is exact
	 * here because no valid cs is zero -- a null selector cannot be the
	 * one that was executing.
	 */
	if (frame->cs == 0) {
		clock_tick_replayed[cpu]++;
		usermode = FALSE;
		pc = 0;
	} else {
		clock_tick_delivered[cpu]++;
		usermode = (frame->cs & 3) != 0;
		pc = (vm_offset_t) frame->rip;
	}

	/*
	 * Preemption held off ACROSS hertz_tick (#459).
	 *
	 * Not a precaution: hertz_tick() disables and re-enables preemption
	 * internally, and its re-enable reaches kernel_preempt_check() -- which
	 * context-switches.  From inside an interrupt handler that means the
	 * switch happens with IF clear and the frame on the interrupt stack,
	 * and this function never returns: the EOI below is never written and
	 * the timer is never re-armed, so the local APIC holds this vector's
	 * priority busy and not one further tick is ever delivered.
	 *
	 * Measured exactly so, three runs out of three: the first tick arrived,
	 * one thread was preempted into, and the processor was never
	 * interrupted again.  The AST check that follows this handler was
	 * called zero times, because the handler it follows never came back.
	 *
	 * i386 holds it off here for the same reason and says so at
	 * lapic_timer_handler; that comment was read this morning and its
	 * consequence was not.  The switch still happens -- it happens on the
	 * way OUT of the trap, where the frame is a real one and the hardware
	 * has already been put back.
	 */
	/*
	 * The debugger's console door (#428), before the tick's own work.
	 *
	 * Here because this is the one thing that runs on every processor at a
	 * fixed rate whatever else the machine is doing, which is exactly the
	 * property a way in has to have.  Before hertz_tick() rather than
	 * after, so that a machine wedged INSIDE the scheduler's own accounting
	 * can still be interrupted by an operator.
	 *
	 * ⚠️ Only the boot processor asks.  The UART is one device.
	 */
	if (cpu == (unsigned) master_cpu)
		ddb_poll_console(frame);

	disable_preemption();

	/*
	 * ⚠️ The full rip, not its low half.  #453 widened this argument to
	 * vm_offset_t, and i386 still has one call site casting it to
	 * natural_t -- harmless where they are the same width and a silent
	 * truncation to the low 32 bits here.
	 */
	hertz_tick(usermode, pc);
	clock_selftest(cpu);

	/*
	 * Acknowledge, then re-arm.
	 *
	 * In this order because they are not interchangeable: the APIC holds
	 * this vector's priority busy until the EOI, so arming first would
	 * schedule an interrupt that the APIC is still refusing to deliver.
	 *
	 * And re-arming is not optional the way it was under a periodic timer:
	 * a one-shot that nobody re-arms fires exactly once, which is a clock
	 * that ticks a single time and then a machine that looks hung.
	 */
	lapic_eoi();
	(void) clock_event_arm_tick();

	/*
	 * And the other half of the pair above (#461).
	 *
	 * 🔥 IT WAS MISSING, AND NOTHING COULD TELL.  #459 wrote the
	 * disable_preemption() above together with the paragraph explaining why
	 * it has to be there, and no matching re-enable.  While the primitive
	 * expanded to nothing, a missing nothing is still nothing: the leak was
	 * exactly as invisible as the mechanism.
	 *
	 * The moment the counter became real (#461) it grew by one per tick and
	 * never came back down, so trap_take_ast() -- which refuses to preempt
	 * above zero -- refused forever.  Measured: 1, then 221, then 440, then
	 * 660 over eight hundred million turns of a thread that should have
	 * been taken off the processor a thousand times.
	 *
	 * _no_check, deliberately, and for the reason the paragraph above
	 * gives: taking the switch HERE is the thing being avoided.  The AST is
	 * taken on the way out of the trap, where the frame is real and the
	 * hardware has been put back.
	 */
	enable_preemption_no_check();
}
