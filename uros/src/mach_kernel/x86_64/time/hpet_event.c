/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The HPET as a clock-event device (#593).  See hpet_event.h for why it is
 * one comparator and a broadcast; what is here is how.
 */

#include <stdint.h>

#include <cpus.h>			/* NCPUS */
#include <kern/misc_protos.h>		/* printf */
#include <kern/cpu_number.h>		/* cpu_number */
#include <sync/lock.h>

#include <cpu/acpi.h>
#include <cpu/ioapic.h>
#include <cpu/lapic.h>
#include <cpu/percpu.h>
#include <cpu/regs.h>
#include <trap/trap.h>
#include <time/hpet.h>
#include <time/hpet_event.h>
#include <time/line.h>		/* one printf per line (#578) */
#include <time/window.h>		/* what both report windows share */
#include <time/pmtimer.h>
#include <time/rulers.h>		/* whether the counter has been named */
#include <time/tsc.h>

_Static_assert(NCPUS <= 64, "a processor is one bit of a 64-bit mask here");

/*
 * #593's ablations of the "already passed" case.  BEHIND makes every 128th
 * comparator write land after the counter has passed the deadline it carries
 * -- what an SMI, or a host that deschedules the vCPU, does between reading
 * the counter and writing the comparator.  UNCHECKED takes away the read-back
 * that notices.  BEHIND alone must cost a late tick and a count; both
 * together must cost every processor its clock until the counter's low half
 * wraps.
 */
#ifndef	ABLATE_593_BEHIND
#define	ABLATE_593_BEHIND	0
#endif
#ifndef	ABLATE_593_UNCHECKED
#define	ABLATE_593_UNCHECKED	0
#endif

/*
 * #593: ISA 8's input unmasked before the backend chooses its route, as a
 * driver that had claimed the RTC's line would leave it -- so that
 * LegacyReplacement must be refused, and a tick #594 moves off a named TSC
 * must stay where it is and say so.  The entry keeps whatever vector
 * ioapic_init() left, and nothing asserts the line: the RTC's interrupts
 * are off.
 */
#ifndef	ABLATE_593_LINE_TAKEN
#define	ABLATE_593_LINE_TAKEN	0
#endif

/*
 * #593: the last processor to leave the broadcast leaves the comparator
 * interrupting, as before the tick could leave the HPET at all -- for the line
 * that reads it back after the move to say so.
 */
#ifndef	ABLATE_593_LEFT_ON
#define	ABLATE_593_LEFT_ON	0
#endif

#define	NS_PER_SEC		1000000000ULL
#define	FS_PER_NS		1000000ULL
#define	BIT(c)			(1ULL << (c))

/*
 * The longest interval one arm may ask for: a quarter of the 32-bit wrap.
 *
 * Every comparison below is on the counter's low 32 bits, so a deadline has
 * to lie within half a wrap of the moment it is compared; a quarter leaves
 * the other quarter for a handler that runs late.  At 100 MHz that is
 * 10.7 s, at 14.318 MHz 75 s -- far beyond a tick.  Longer requests
 * are clamped, for lapic_arm()'s reason: waking early is a wasted interrupt,
 * waking after a wrap is a deadline missed by minutes.
 */
#define	HPET_EV_MAX_COUNTS	(1U << 30)

/*
 * How far ahead of the counter a comparator must be, read back after the
 * write, to be trusted to match.  The specification gives no number, only
 * the warning (2.3.9.2.1); a microsecond is several counts on every HPET
 * there is, and far more than the time a posted write takes to land.  A
 * deadline closer than this is treated as due now -- a microsecond early,
 * against a comparator that might never have matched at all.
 */
#define	HPET_EV_AHEAD_NS	1000ULL

enum { ROUTE_NONE, ROUTE_FSB, ROUTE_LEGACY, ROUTE_TAKEN };

static int		started;
static int		route;
static unsigned		comparator;
static uint32_t		route_gsi;
static uint32_t		broadcaster;	/* the APIC id the comparator reaches */
static uint8_t		tick_vector;
static uint64_t		hz;
static uint32_t		ahead;

/*
 * The shared state, under one lock that the comparator's handler also takes
 * -- so every other holder masks interrupts first, or a handler landing on
 * the holder's own processor would wait for it for ever (#528).
 */
static hw_lock_data_t	ev_lock;
static uint64_t		armed;		/* processors with a deadline here */
static uint32_t		due[NCPUS];
static uint64_t		woken;		/* sent their tick, not re-armed yet */
static uint32_t		woken_for[NCPUS];
static uint32_t		interval[NCPUS];	/* each one's last request */
static int		programmed;
static int		silenced;	/* nobody left on it: see hpet_ev_stop() */
static uint32_t		programmed_at;
#if ABLATE_593_BEHIND
static unsigned		behind_writes;
#endif

/* What a window counted, all in HPET counts. */
struct window {
	uint64_t	fires;		/* comparator interrupts */
	uint64_t	idle_fires;	/* ... with nothing due */
	uint64_t	passed;		/* writes found behind the counter */
	uint64_t	kicks;		/* ticks sent, self-IPIs included */
	uint64_t	joins;		/* arms moved onto another's grid */
	uint64_t	late_n, late_sum;
	uint32_t	late_min, late_max;
	uint64_t	rearm_n[NCPUS], rearm_sum[NCPUS];
	uint32_t	rearm_max[NCPUS];
};

static struct window	w;
static uint32_t		win_last;	/* the counter at the last fire */
static uint64_t		win_counts;
static uint64_t		win_length;
static unsigned		win_index;
static struct pm_total	win_pm;
static uint64_t		tsc0;

/* The window closed, for the thread that prints it. */
static struct report {
	struct window	w;
	unsigned	index;
	uint64_t	counts, pm, tsc;
	int		pm_valid;
} report;
static volatile int	report_pending;
static unsigned		reports_dropped;

static uint32_t ns_to_counts(uint64_t ns)
{
	uint64_t c = (ns / NS_PER_SEC) * hz + ((ns % NS_PER_SEC) * hz) / NS_PER_SEC;

	if (c == 0)
		c = 1;
	if (c > HPET_EV_MAX_COUNTS)
		c = HPET_EV_MAX_COUNTS;
	return (uint32_t)c;
}

static uint64_t counts_to_ns(uint64_t counts)
{
	return counts * hpet_period_fs() / FS_PER_NS;
}

/* ------------------------------------------------------------ routing -- */

/*
 * An FSB message where a comparator can send one, and LegacyReplacement
 * otherwise.
 *
 * FSB first: the message goes straight to a local APIC, so no I/O APIC input
 * is taken from anybody, and the 8254 and the RTC keep their lines.
 *
 * LegacyReplacement second, and not an input from the route capability.  It
 * is the one route every PC HPET offers -- QEMU's `pc' board offers no other
 * input at all, only IRQ2 -- and it is the specification's own way of making
 * input 2 the HPET's alone: "the 8254 timer will not cause any interrupts"
 * (2.4.2.1).  An input chosen from the capability would be shared with the
 * 8254 on input 2, or be a PCI input (16 and up on `q35') whose polarity and
 * trigger belong to the PCI devices wired to it -- and an edge-triggered
 * timer "should not be shared with any PCI interrupts" (2.4.2.2).  FreeBSD
 * reaches the same answer for virtual machines, whose capability masks it
 * does not trust.  A block with neither route is refused, and says so: the
 * inputs its capability names are not chosen from, and code that no machine
 * here would run is not written.
 */
static int choose_route(unsigned *which, uint32_t *cap0)
{
	struct hpet_comparator_caps caps;
	unsigned n;

#if ABLATE_593_LINE_TAKEN
	if (!started && ioapic_present()
	    && acpi_irq_to_gsi(8) < ioapic_pin_count())
		ioapic_unmask(acpi_irq_to_gsi(8));
#endif

	*cap0 = 0;
	for (n = 0; n < hpet_comparators(); n++) {
		hpet_comparator_caps(n, &caps);
		if (n == 0)
			*cap0 = caps.route_cap;
		if (caps.fsb) {
			*which = n;
			return ROUTE_FSB;
		}
	}
	/*
	 * Only where ISA 0 arrives on input 2.  The specification sends timer
	 * 0 to "IRQ2 in the I/O APIC" (2.3.5) whatever the tables say about
	 * the 8254, and this routes ISA 0's input with ISA 0's flags: the same
	 * pin wherever the MADT carries the usual override of ISA 0 to GSI 2,
	 * as `pc' and `q35' do and every PC with an HPET does -- and a pin the
	 * comparator never reaches where it does not.  Refused there, and
	 * said, rather than written for a machine none of these is.
	 */
	if (hpet_legacy_capable() && ioapic_present()
	    && acpi_irq_to_gsi(0) == 2
	    && acpi_irq_to_gsi(8) < ioapic_pin_count()) {
		/*
		 * Not over a driver's line.  LegacyReplacement takes ISA 0's
		 * input and silences the RTC's, and #594 can bring the tick
		 * here while the system runs -- after a driver may have claimed
		 * either, since device_md_irq_register() refuses them only once
		 * the bit is on.  An unmasked input is one somebody is using;
		 * this backend's own, once started, is nobody else's.
		 *
		 * ⚠️ Checked, not locked: a driver claiming ISA 0 or 8 between
		 * this and the switch in hpet_ev_start() keeps a line that goes
		 * silent.  No driver here claims either -- the drivers take PCI
		 * inputs, MSI, and ISA 1 and 4 -- and the day one does, the
		 * check and the claim go under one lock.  Until then the start
		 * says so if it finds the line taken after all.
		 */
		if (!started && (!ioapic_is_masked(acpi_irq_to_gsi(0))
				 || !ioapic_is_masked(acpi_irq_to_gsi(8))))
			return ROUTE_TAKEN;
		*which = 0;
		return ROUTE_LEGACY;
	}
	return ROUTE_NONE;
}

/* ------------------------------------------------------ the deadlines -- */

/*
 * Every processor whose deadline has come moves from `armed' to `kick', and
 * the comparator is programmed for the earliest deadline left.  ev_lock held,
 * interrupts off.
 *
 * 🔴 THE WRITE IS READ BACK, and a comparator found behind the counter is the
 * whole of the trap this issue names: in one-shot mode the counter has to
 * EQUAL the value to match, so a value written just behind it matches only
 * when the low 32 bits come round again -- 43 s at 100 MHz, five minutes at
 * 14.318 MHz -- and every processor's clock stops with it.  The write can
 * land behind a deadline that was ahead when it was computed: a posted write
 * is slow, an SMI takes the processor away, a host deschedules the vCPU.
 * Found behind, the deadline has come, so the loop goes round again and the
 * scan below finds its processor due.  Each pass takes at least one processor
 * out of `armed', so the loop ends.
 */
static void reprogram_locked(uint64_t *kick)
{
	for (;;) {
		uint32_t	now = hpet_read32();
		uint32_t	next = 0, best = 0;
		uint64_t	left = armed;
		int		any = 0;

		while (left != 0) {
			unsigned c = (unsigned)__builtin_ctzll(left);
			uint32_t to_go = due[c] - now;

			left &= left - 1;
			if ((int32_t)to_go < (int32_t)ahead) {
				armed &= ~BIT(c);
				woken |= BIT(c);
				woken_for[c] = due[c];
				*kick |= BIT(c);
				w.kicks++;
				continue;
			}
			if (!any || to_go < best) {
				best = to_go;
				next = due[c];
				any = 1;
			}
		}

		if (!any) {
			programmed = 0;
			return;
		}

#if ABLATE_593_BEHIND
		if (++behind_writes % 128 == 0)
			while ((int32_t)(next - hpet_read32()) >= 0)
				cpu_pause();
#endif
		hpet_comparator_set(comparator, next);
		programmed = 1;
		programmed_at = next;

#if !ABLATE_593_UNCHECKED
		if ((int32_t)(next - hpet_read32()) >= (int32_t)ahead)
			return;
		w.passed++;
#else
		return;
#endif
	}
}

/*
 * The ticks, sent after the lock is dropped: the boot processor's own by a
 * self-IPI, so that it arrives at the tick's class and waits for its spl
 * level like every other processor's (hpet_event.h).
 */
static void kick_send(uint64_t kick)
{
	unsigned self = (unsigned)cpu_number();

	while (kick != 0) {
		unsigned c = (unsigned)__builtin_ctzll(kick);

		kick &= kick - 1;
		if (c == self)
			lapic_send_self(tick_vector);
		else
			lapic_send_ipi(c, tick_vector);
	}
}

/* ------------------------------------------------------------ windows -- */

/* Windows by the counter, as long as <time/window.h> says. */
static void window_open(uint32_t now)
{
	w = (struct window){ .late_min = UINT32_MAX };
	win_last = now;
	win_counts = 0;
	win_length = hz * window_seconds(win_index);
	pm_total_open(&win_pm);
	tsc0 = rdtsc();
}

/*
 * The time between two fires, added up on three clocks.  The PM timer wraps
 * every 4.7 s at 24 bits, so a gap longer than four seconds -- a stall, which
 * is what the UNCHECKED ablation is for -- makes its total no longer a time,
 * and the report says so rather than printing it.
 */
static void window_account(uint32_t now)
{
	uint32_t gap = now - win_last;

	win_counts += gap;
	win_last = now;
	pm_total_step(&win_pm, gap > hz * 4);

	if (win_counts < win_length)
		return;

	if (!handoff_free(&report_pending)) {
		reports_dropped++;
	} else {
		report.w = w;
		report.index = win_index;
		report.counts = win_counts;
		report.pm = win_pm.sum;
		report.pm_valid = pm_total_valid(&win_pm);
		report.tsc = rdtsc() - tsc0;
		handoff_publish(&report_pending);
	}
	win_index++;
	window_open(now);
}

/* ------------------------------------------------------------ backend -- */

/*
 * The chosen comparator configured for the chosen route and interrupting:
 * when the backend starts, and again when a processor arms it after the last
 * one had left (hpet_ev_stop()).  ev_lock held, or not shared yet.
 *
 * The FSB message is in the compatibility format, as <device/device_machdep>
 * composes one for MSI-X: 0xFEE00000 with the destination APIC id at bit 12,
 * physical, no redirection; the vector as the data, fixed delivery, edge.  No
 * interrupt remapping is on in this kernel, on either vendor, so nothing
 * translates it.
 */
static void comparator_route(void)
{
	if (route == ROUTE_FSB)
		hpet_comparator_fsb(comparator,
				    0xFEE00000U | (broadcaster << 12),
				    HPET_EVENT_VECTOR);
	else
		(void)hpet_comparator_legacy(comparator);
}

static int hpet_ev_probe(void)
{
	unsigned	n;
	uint32_t	cap0;

	if (!hpet_present()) {
		printf("clock_event: hpet: no timer block -- the ACPI table "
		       "names none, or its period was out of range (#508)\n");
		return 0;
	}
	/*
	 * 🔴 NOT A COUNTER THAT HAS BEEN NAMED.  The rulers' vote at boot names
	 * one that disagrees with the other two, the watchdog one that stopped
	 * or drifted while the system ran (#594), and a ruler named is never
	 * used again (tsc_watch.c).  The tick here runs on that same counter,
	 * at the rate its table states: a tick at a rate the kernel has itself
	 * judged wrong, and the time of day with it.  The TSC's side keeps the
	 * same rule through its rate, which tsc_distrust() withdraws and
	 * tscdl_probe() reads.
	 */
	if (rulers_get(RULER_HPET)->dissents) {
		printf("clock_event: hpet: its counter has been named -- by the "
		       "rulers' vote or by the watchdog -- and is not used "
		       "(#593, #594)\n");
		return 0;
	}
	if (!lapic_present()) {
		printf("clock_event: hpet: no local APIC to deliver to\n");
		return 0;
	}
	switch (choose_route(&n, &cap0)) {
	case ROUTE_NONE:
		printf("clock_event: hpet: no comparator can send an FSB "
		       "message, and LegacyReplacement is %s; comparator 0 can "
		       "reach I/O APIC inputs 0x%x, and choosing among them is "
		       "not written (#593)\n",
		       !hpet_legacy_capable() ? "not in this block"
		       : acpi_irq_to_gsi(0) != 2 ? "not where ISA 0 arrives "
						  "here (its timer 0 goes to "
						  "input 2)"
		       : "without an I/O APIC input to reach", cap0);
		return 0;
	case ROUTE_TAKEN:
		printf("clock_event: hpet: no comparator can send an FSB "
		       "message, and LegacyReplacement would take the 8254's "
		       "or the RTC's input from the driver that routed it "
		       "(#593)\n");
		return 0;
	}
	return 1;
}

/*
 * The comparator's interrupt.  Class fifteen: never deferred by spl, so never
 * entered by a replay, and its acknowledgement is unconditional.
 */
static void hpet_ev_intr(struct trap_frame *frame)
{
	uint64_t	kick = 0;
	uint32_t	now;

	(void)frame;

	hw_lock_lock(&ev_lock);		/* an interrupt gate: IF is clear */
	now = hpet_read32();
	w.fires++;
	if (programmed && (int32_t)(now - programmed_at) >= 0) {
		uint32_t late = now - programmed_at;

		w.late_n++;
		w.late_sum += late;
		if (late < w.late_min)
			w.late_min = late;
		if (late > w.late_max)
			w.late_max = late;
	}
	reprogram_locked(&kick);
	if (kick == 0)
		w.idle_fires++;
	window_account(now);
	hw_lock_unlock(&ev_lock);

	lapic_eoi();
	kick_send(kick);
}

/*
 * Once, on the boot processor, when this backend is chosen.  Idempotent,
 * because a -C boot chooses the clock twice: once for the burn-in, and again
 * in machine_init().
 */
static void hpet_ev_start(uint8_t vector)
{
	uint32_t	cap0;

	if (started)
		return;

	tick_vector = vector;
	hz = hpet_hz();
	ahead = (uint32_t)((HPET_EV_AHEAD_NS * hz + NS_PER_SEC - 1) / NS_PER_SEC);
	if (ahead < 2)
		ahead = 2;
	/*
	 * A processor number used as an APIC id -- here for the FSB message's
	 * destination and the I/O APIC entry's, in kick_send() for every IPI.
	 * They are the same number on this target, which cause_ast_check()
	 * and ddb_stop_others() rely on as well; said here too, because all of
	 * them would have to change together.
	 */
	broadcaster = (uint32_t)cpu_number();
	hw_lock_init(&ev_lock);
	armed = woken = 0;
	programmed = 0;

	route = choose_route(&comparator, &cap0);

	/*
	 * The probe found a route a moment ago, and nothing it looked at
	 * changes but the masks: only a driver that claimed ISA 0 or 8 in
	 * between can take the route away (choose_route()).  The clock has to
	 * run, so the line is taken anyway -- and said, because that driver's
	 * device has just gone silent.  No route at all cannot follow a probe
	 * that found one.
	 */
	if (route == ROUTE_TAKEN) {
		printf("clock_event: hpet: WRONG -- ISA 0 or 8 was claimed "
		       "between the probe and the start; LegacyReplacement "
		       "takes it all the same (#593)\n");
		route = ROUTE_LEGACY;
		comparator = 0;
	} else if (route == ROUTE_NONE) {
		panic("clock_event: hpet: started with no route, where the "
		      "probe had found one (#593)");
	}
	trap_set_handler(HPET_EVENT_VECTOR, hpet_ev_intr);

	/*
	 * Every other comparator is silent, and LegacyReplacement off unless
	 * the branch below turns it on: the block was taken from the firmware
	 * at the first look (hpet.c, take_block()), not here -- a boot whose
	 * tick never comes here needs it as much.
	 */
	comparator_route();
	if (route != ROUTE_FSB) {
		route_gsi = acpi_irq_to_gsi(0);
		ioapic_route(route_gsi, HPET_EVENT_VECTOR, broadcaster,
			     acpi_irq_flags(0));
	}

	window_open(hpet_read32());
	started = 1;

	if (route == ROUTE_FSB)
		printf("clock_event: hpet: comparator %u of %u, one-shot, 32 "
		       "bits, FSB message to cpu %u on vector 0x%x; each "
		       "processor's tick sent by IPI on 0x%x (#593)\n",
		       comparator, hpet_comparators(), broadcaster,
		       HPET_EVENT_VECTOR, vector);
	else
		printf("clock_event: hpet: comparator %u of %u, one-shot, 32 "
		       "bits, LegacyReplacement to I/O APIC input %u, cpu %u "
		       "on vector 0x%x -- the 8254 and the RTC interrupt no "
		       "more; each processor's tick sent by IPI on 0x%x "
		       "(#593)\n", comparator, hpet_comparators(), route_gsi,
		       broadcaster, HPET_EVENT_VECTOR, vector);
}

static void hpet_ev_setup(uint8_t vector)
{
	(void)vector;

	/*
	 * This processor's own timer, silenced: the tick now comes from the
	 * broadcast, on the same vector, and a countdown or a deadline left
	 * armed from before would be a second tick nobody asked for.
	 */
	lapic_timer_stop();
}

/*
 * A deadline counted from now -- a processor's first, or one whose tick came
 * too late to count from the deadline that fired -- moved onto the grid of
 * another processor with the same interval: the first point of that grid at
 * or after the deadline asked for.  Never earlier than asked, at most one
 * interval later, and once: from then on the two re-arm from the same
 * deadlines and share every broadcast.
 *
 * Without it, processors that started a few milliseconds apart keep their
 * own phases for good, and every period costs one interrupt per phase:
 * measured at 1.8 comparator interrupts per period at four processors under
 * KVM, and the writes found behind the counter came from one phase's
 * deadline arriving while the other's broadcast was still being sent.
 *
 * ⚠️ A processor that has been sent its tick and has not re-armed yet has a
 * grid too -- the deadline that fired, plus its interval -- and it counts.
 * Looking only at armed processors, TCG kept three phases at four processors
 * for minutes: its vCPUs take their ticks one after another, and a fresh arm
 * made while the others were all between their kick and their re-arm found
 * nobody to join.
 */
static uint32_t join_locked(unsigned self, uint32_t now, uint32_t want,
			    uint32_t counts)
{
	uint64_t left = (armed | woken) & ~BIT(self);

	while (left != 0) {
		unsigned	c = (unsigned)__builtin_ctzll(left);
		uint32_t	grid, behind;

		left &= left - 1;
		if (interval[c] != counts)
			continue;
		if (armed & BIT(c)) {
			grid = due[c];
		} else {
			/*
			 * Sent its tick an interval ago or more and not
			 * re-armed since: held, or stopped.  Its grid is the
			 * one it will leave when it does re-arm, and a stale
			 * one would put this deadline in the past -- a tick
			 * that fires at once, and then again, for ever.
			 */
			if ((uint32_t)(now - woken_for[c]) >= counts)
				continue;
			grid = woken_for[c];
		}

		/*
		 * Unsigned, and bounded on both sides: a grid point a valid
		 * processor offers lies less than two intervals before `want'
		 * or at most one after it, and anything else is a deadline
		 * gone stale across the 32-bit wrap, which a signed difference
		 * would read as the near future.
		 */
		behind = want - grid;
		if (behind == 0 || behind >= 2 * counts) {
			if ((uint32_t)(grid - want) > counts)
				continue;
			w.joins++;
			return grid;
		}
		w.joins++;
		return grid + ((behind + counts - 1) / counts) * counts;
	}
	return want;
}

static int hpet_ev_arm(uint64_t ns)
{
	unsigned	self;
	uint64_t	kick = 0;
	uint32_t	counts, now, base;
	int		fresh = 1;

	if (!started)
		return 0;

	counts = ns_to_counts(ns);

	/*
	 * Which processor this is, read with interrupts off, and the ticks
	 * sent before they are back on: a thread that moved in between would
	 * arm another processor's deadline, or send its self-IPI to the wrong
	 * one.  Every caller has them off already (clock_event.c); this does
	 * not lean on it.
	 */
	percpu_intr_disable();
	self = (unsigned)cpu_number();
	if (self >= NCPUS) {
		percpu_intr_enable();
		return 0;
	}
	hw_lock_lock(&ev_lock);

	now = hpet_read32();
	base = now;

	/*
	 * 🔑 COUNTED FROM THE DEADLINE THAT FIRED, not from the moment the IPI
	 * happened to arrive.  Every processor woken by one broadcast then
	 * asks for the SAME next deadline, and the next broadcast is one
	 * interrupt for all of them rather than one per processor, each a few
	 * microseconds after the last.  It is also a tick that does not drift
	 * by its own delivery time.  Unless the handler ran so late that the
	 * next deadline has gone as well -- a tick held back by spl -- in which
	 * case it is counted from now, and the ticks owed are the spl replay's
	 * business (#522).
	 */
	if (woken & BIT(self)) {
		uint32_t late = now - woken_for[self];

		woken &= ~BIT(self);
		w.rearm_n[self]++;
		w.rearm_sum[self] += late;
		if (late > w.rearm_max[self])
			w.rearm_max[self] = late;
		/*
		 * Unsigned, like join_locked()'s: a processor held between
		 * 2^31 and 2^32 counts past its deadline -- 21 to 43 s at
		 * 100 MHz -- reads as early to a signed difference, and would
		 * be handed a base that far in the past.
		 */
		if (counts > ahead && late < counts - ahead) {
			base = woken_for[self];
			fresh = 0;
		}
	}

	if (silenced) {
		comparator_route();
		silenced = 0;
	}

	due[self] = base + counts;
	if (fresh)
		due[self] = join_locked(self, now, due[self], counts);
	interval[self] = counts;
	armed |= BIT(self);
	if (!programmed || (int32_t)(due[self] - programmed_at) < 0)
		reprogram_locked(&kick);

	hw_lock_unlock(&ev_lock);
	kick_send(kick);
	percpu_intr_enable();
	return 1;
}

static void hpet_ev_stop(void)
{
	unsigned self;

	if (!started)
		return;

	/*
	 * Out of the set, and the comparator left as it is: if it was
	 * programmed for this processor alone, it fires once for nobody and
	 * the handler programmes the next.  Cheaper than a rescan here, and
	 * counted as a fire with nothing due.
	 *
	 * 🔴 BUT THE LAST ONE OUT SILENCES IT.  With nobody left -- the tick
	 * has left the HPET (clock_event_leave()), or the burn-in is over --
	 * a comparator left interrupting is not one fire for nobody: in 32-bit
	 * one-shot mode the counter matches it again at every wrap, and the
	 * wrap itself interrupts (2.3.9.2.1), so on hardware it would take the
	 * boot processor every 43 s for good -- from a block the watchdog may
	 * just have named broken.  QEMU re-arms a one-shot only when it is
	 * written, so no boot here would have shown it.  The next arm, if the
	 * tick ever comes back, routes it again.
	 */
	percpu_intr_disable();
	self = (unsigned)cpu_number();	/* interrupts off: arm()'s reason */
	if (self >= NCPUS) {
		percpu_intr_enable();
		return;
	}
	hw_lock_lock(&ev_lock);
	armed &= ~BIT(self);
	woken &= ~BIT(self);
	if (!ABLATE_593_LEFT_ON && (armed | woken) == 0 && !silenced) {
		hpet_comparator_off(comparator);
		programmed = 0;
		silenced = 1;
	}
	hw_lock_unlock(&ev_lock);
	percpu_intr_enable();
}

/*
 * After every processor has left: what the block was left doing, read back
 * from it rather than assumed.  Thread context.
 */
void hpet_event_left_report(void)
{
	struct line	l;

	line_start(&l);
	put_s(&l, "clock_event: hpet: no processor is left on the broadcast, "
		  "and comparator ");
	put_u(&l, comparator);
	if (hpet_comparator_interrupting(comparator))
		put_s(&l, " -- WRONG: is still interrupting, read back");
	else
		put_s(&l, " is silenced, read back");
	if (route == ROUTE_LEGACY)
		put_s(&l, "; LegacyReplacement stays on, so the 8254 and the "
			  "RTC stay silent and ISA 0 and 8 stay refused");
	put_s(&l, " (#593)");
	printf("%s\n", l.b);
}

const struct clock_event_ops hpet_event_ops = {
	"hpet", hpet_ev_probe, hpet_ev_setup, hpet_ev_arm, hpet_ev_stop,
	hpet_ev_start
};

/* ------------------------------------------------------------- report -- */

/*
 * The window's line, and one per processor.  Claimed by compare-and-swap,
 * because the idle loop drains reports on every processor and two of them
 * finding the same window would print it twice.
 */
void hpet_event_drain_report(void)
{
	struct report	*r = &report;
	uint64_t	tsc_rate = tsc_hz();
	unsigned	c;
	struct line	l;

	if (!handoff_claim(&report_pending))
		return;

	line_start(&l);
	put_s(&l, "clock_event: hpet: window ");
	put_u(&l, r->index);
	put_s(&l, ", ");
	put_u(&l, r->counts * 1000 / hz);
	put_s(&l, " ms by the counter, ");
	if (tsc_rate != 0) {
		put_u(&l, r->tsc * 1000 / tsc_rate);
		put_s(&l, " ms by the TSC, ");
	} else {
		put_s(&l, "the TSC has no rate, ");
	}
	if (r->pm_valid) {
		put_u(&l, r->pm * 1000 / PMTIMER_HZ);
		put_s(&l, " ms by the PM timer");
	} else if (pmtimer_present()) {
		put_s(&l, "the PM timer wrapped inside a gap between two fires");
	} else {
		put_s(&l, "no PM timer");
	}
	put_s(&l, " -- ");
	put_u(&l, r->w.fires);
	put_s(&l, " comparator interrupts (");
	put_u(&l, r->w.idle_fires);
	put_s(&l, " with nothing due), ");
	put_u(&l, r->w.kicks);
	put_s(&l, " ticks sent, ");
	put_u(&l, r->w.passed);
	put_s(&l, " writes found behind the counter, ");
	put_u(&l, r->w.joins);
	put_s(&l, " arms moved onto another processor's grid");
	if (r->w.late_n != 0) {
		put_s(&l, "; the comparator late by ");
		put_u(&l, counts_to_ns(r->w.late_min));
		put_s(&l, "..");
		put_u(&l, counts_to_ns(r->w.late_max));
		put_s(&l, " ns, mean ");
		put_u(&l, counts_to_ns(r->w.late_sum / r->w.late_n));
	}
	if (reports_dropped != 0) {
		put_s(&l, "; ");
		put_u(&l, reports_dropped);
		put_s(&l, " windows since boot closed while one waited to be "
			  "printed");
	}
#if defined(ABLATE_593_ICR_OPEN) && ABLATE_593_ICR_OPEN
	put_s(&l, "; since boot, ");
	put_u(&l, lapic_icr_nested());
	put_s(&l, " IPI sends began between another's wait and its second "
		  "write on the same processor (UROS_ABLATE_593_ICR_OPEN)");
#endif
	put_s(&l, " (#593)");
	printf("%s\n", l.b);

	/*
	 * Each processor's ticks against the TSC and the PM timer are
	 * clock_event.c's lines, whatever the backend; what only the broadcast
	 * can say is how long after its deadline each processor re-armed --
	 * the IPI's delivery and the tick handler together.
	 */
	for (c = 0; c < NCPUS; c++) {
		if (r->w.rearm_n[c] == 0)
			continue;
		line_start(&l);
		put_s(&l, "clock_event: hpet: cpu ");
		put_u(&l, c);
		put_s(&l, " re-armed ");
		put_u(&l, counts_to_ns(r->w.rearm_sum[c] / r->w.rearm_n[c]));
		put_s(&l, " ns after its deadline on average, ");
		put_u(&l, counts_to_ns(r->w.rearm_max[c]));
		put_s(&l, " at most, over ");
		put_u(&l, r->w.rearm_n[c]);
		put_s(&l, " ticks");
		printf("%s\n", l.b);
	}

	handoff_release(&report_pending);
}
