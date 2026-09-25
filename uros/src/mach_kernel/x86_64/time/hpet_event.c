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
#include <sync/barrier.h>		/* smp_wmb, smp_rmb */
#include <sync/lock.h>

#include <cpu/acpi.h>
#include <cpu/ioapic.h>
#include <cpu/lapic.h>
#include <cpu/percpu.h>
#include <cpu/regs.h>
#include <cpu/smp.h>
#include <trap/trap.h>
#include <time/hpet.h>
#include <time/hpet_event.h>
#include <time/pmtimer.h>
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

#define	NS_PER_SEC		1000000000ULL
#define	FS_PER_NS		1000000ULL
#define	BIT(c)			(1ULL << (c))

/*
 * The longest interval one arm may ask for: a quarter of the 32-bit wrap.
 *
 * Every comparison below is on the counter's low 32 bits, signed, so a
 * deadline has to lie within half a wrap of the moment it is compared; a
 * quarter leaves the other quarter for a handler that runs late.  At 100 MHz
 * that is 10.7 s, at 14.318 MHz 75 s -- far beyond a tick.  Longer requests
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

enum { ROUTE_NONE, ROUTE_FSB, ROUTE_LEGACY };

/*
 * Report windows, in seconds by the counter: the first says early whether
 * the rate is right at all, the second is long enough to catch what a
 * second hides, and after that one a minute.
 */
#define	WINDOW_FIRST_S		1
#define	WINDOW_SECOND_S		10
#define	WINDOW_REST_S		60

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
static uint32_t		pm_last;
static uint64_t		win_pm;
static int		pm_gap;		/* a fire interval too long for the PM
					   timer's wrap: its total is not a
					   time */
static uint64_t		tsc0;
static unsigned long	ticks0[NCPUS];

/* The window closed, for the thread that prints it. */
static struct report {
	struct window	w;
	unsigned	index;
	uint64_t	counts, pm, tsc;
	int		pm_valid;
	unsigned long	ticks[NCPUS];
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
	if (hpet_legacy_capable() && ioapic_present()
	    && acpi_irq_to_gsi(0) < ioapic_pin_count()) {
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

static uint64_t window_length(unsigned index)
{
	if (index == 0)
		return hz * WINDOW_FIRST_S;
	if (index == 1)
		return hz * WINDOW_SECOND_S;
	return hz * WINDOW_REST_S;
}

static void window_open(uint32_t now)
{
	unsigned c;

	w = (struct window){ .late_min = UINT32_MAX };
	for (c = 0; c < NCPUS; c++)
		ticks0[c] = clock_event_ticks(c);
	win_last = now;
	win_counts = 0;
	win_length = window_length(win_index);
	win_pm = 0;
	pm_gap = 0;
	if (pmtimer_present())
		pm_last = pmtimer_read();
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
	unsigned c;

	win_counts += gap;
	win_last = now;
	if (gap > hz * 4)
		pm_gap = 1;
	if (pmtimer_present()) {
		uint32_t pm = pmtimer_read();

		win_pm += pmtimer_delta(pm_last, pm);
		pm_last = pm;
	}

	if (win_counts < win_length)
		return;

	if (report_pending) {
		reports_dropped++;
	} else {
		report.w = w;
		report.index = win_index;
		report.counts = win_counts;
		report.pm = win_pm;
		report.pm_valid = pmtimer_present() && !pm_gap;
		report.tsc = rdtsc() - tsc0;
		for (c = 0; c < NCPUS; c++)
			report.ticks[c] = clock_event_ticks(c) - ticks0[c];
		smp_wmb();
		report_pending = 1;
	}
	win_index++;
	window_open(now);
}

/* ------------------------------------------------------------ backend -- */

static int hpet_ev_probe(void)
{
	unsigned	n;
	uint32_t	cap0;

	if (!hpet_present()) {
		printf("clock_event: hpet: no timer block -- the ACPI table "
		       "names none, or its period was out of range (#508)\n");
		return 0;
	}
	if (!lapic_present()) {
		printf("clock_event: hpet: no local APIC to deliver to\n");
		return 0;
	}
	if (choose_route(&n, &cap0) == ROUTE_NONE) {
		printf("clock_event: hpet: no comparator can send an FSB "
		       "message and the block cannot take the 8254's line; "
		       "comparator 0 can reach I/O APIC inputs 0x%x, and "
		       "choosing among them is not written (#593)\n", cap0);
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
	broadcaster = (uint32_t)cpu_number();
	hw_lock_init(&ev_lock);
	armed = woken = 0;
	programmed = 0;

	route = choose_route(&comparator, &cap0);
	trap_set_handler(HPET_EVENT_VECTOR, hpet_ev_intr);

	if (route == ROUTE_FSB) {
		/*
		 * The compatibility-format message, as <device/device_machdep>
		 * composes one for MSI-X: 0xFEE00000 with the destination APIC
		 * id at bit 12, physical, no redirection; the vector as the
		 * data, fixed delivery, edge.  No interrupt remapping is on in
		 * this kernel, on either vendor, so nothing translates it.
		 */
		hpet_comparator_fsb(comparator,
				    0xFEE00000U | (broadcaster << 12),
				    HPET_EVENT_VECTOR);
	} else {
		route_gsi = acpi_irq_to_gsi(0);
		(void)hpet_comparator_legacy(comparator);
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
static uint32_t join_locked(unsigned self, uint32_t want, uint32_t counts)
{
	uint64_t left = (armed | woken) & ~BIT(self);

	while (left != 0) {
		unsigned	c = (unsigned)__builtin_ctzll(left);
		uint32_t	grid;
		int32_t		after;
		uint32_t	k;

		left &= left - 1;
		if (interval[c] != counts)
			continue;
		grid = (armed & BIT(c)) ? due[c] : woken_for[c];
		after = (int32_t)(want - grid);
		k = after <= 0 ? 0 : ((uint32_t)after + counts - 1) / counts;
		w.joins++;
		return grid + k * counts;
	}
	return want;
}

static int hpet_ev_arm(uint64_t ns)
{
	unsigned	self = (unsigned)cpu_number();
	uint64_t	kick = 0;
	uint32_t	counts, now, base;
	int		fresh = 1;

	if (!started || self >= NCPUS)
		return 0;

	counts = ns_to_counts(ns);

	percpu_intr_disable();
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
		if ((int32_t)(woken_for[self] + counts - now) > (int32_t)ahead) {
			base = woken_for[self];
			fresh = 0;
		}
	}

	due[self] = base + counts;
	if (fresh)
		due[self] = join_locked(self, due[self], counts);
	interval[self] = counts;
	armed |= BIT(self);
	if (!programmed || (int32_t)(due[self] - programmed_at) < 0)
		reprogram_locked(&kick);

	hw_lock_unlock(&ev_lock);
	percpu_intr_enable();

	kick_send(kick);
	return 1;
}

static void hpet_ev_stop(void)
{
	unsigned self = (unsigned)cpu_number();

	if (!started || self >= NCPUS)
		return;

	/*
	 * Out of the set, and the comparator left as it is: if it was
	 * programmed for this processor alone, it fires once for nobody and
	 * the handler programmes the next.  Cheaper than a rescan here, and
	 * counted as a fire with nothing due.
	 */
	percpu_intr_disable();
	hw_lock_lock(&ev_lock);
	armed &= ~BIT(self);
	woken &= ~BIT(self);
	hw_lock_unlock(&ev_lock);
	percpu_intr_enable();
}

const struct clock_event_ops hpet_event_ops = {
	"hpet", hpet_ev_probe, hpet_ev_setup, hpet_ev_arm, hpet_ev_stop,
	hpet_ev_start
};

/* ------------------------------------------------------------- report -- */

/*
 * A line built in a local buffer and printed by ONE printf: a line made of
 * several is a line another processor's output can land inside (#578).  Not
 * the kernel's sprintf(), which writes through one static pointer and is not
 * for two processors at once.
 */
struct line {
	char		b[400];
	unsigned	n;
};

static void put_s(struct line *l, const char *s)
{
	while (*s != '\0' && l->n < sizeof(l->b) - 1)
		l->b[l->n++] = *s++;
	l->b[l->n] = '\0';
}

static void put_u(struct line *l, uint64_t v)
{
	char	d[21];
	char	*p = d + sizeof(d) - 1;

	*p = '\0';
	do {
		*--p = (char)('0' + v % 10);
		v /= 10;
	} while (v != 0);
	put_s(l, p);
}

/*
 * The window's line, and one per processor.  Claimed by compare-and-swap,
 * because the idle loop drains reports on every processor and two of them
 * finding the same window would print it twice.
 */
void hpet_event_drain_report(void)
{
	struct report	*r = &report;
	uint64_t	tsc_rate = tsc_hz();
	unsigned	ev_hz = clock_event_hz();
	unsigned	c;
	struct line	l;

	if (report_pending != 1
	    || !__sync_bool_compare_and_swap(&report_pending, 1, 2))
		return;
	smp_rmb();

	l.n = 0;
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
		put_s(&l, " windows closed while one waited to be printed");
	}
	put_s(&l, " (#593)");
	printf("%s\n", l.b);

	for (c = 0; c < NCPUS; c++) {
		unsigned long t = r->ticks[c];

		if (!smp_is_online(c) && t == 0)
			continue;
		l.n = 0;
		put_s(&l, "clock_event: hpet: cpu ");
		put_u(&l, c);
		put_s(&l, " took ");
		put_u(&l, t);
		put_s(&l, " ticks");
		if (tsc_rate != 0 && r->tsc != 0) {
			put_s(&l, ", ");
			put_u(&l, (uint64_t)t * 1000 * tsc_rate
				  / ((uint64_t)ev_hz * r->tsc));
			put_s(&l, " per mille of nominal by the TSC");
		}
		if (r->pm_valid && r->pm != 0) {
			put_s(&l, ", ");
			put_u(&l, (uint64_t)t * 1000 * PMTIMER_HZ
				  / ((uint64_t)ev_hz * r->pm));
			put_s(&l, " by the PM timer");
		}
		if (r->w.rearm_n[c] != 0) {
			put_s(&l, "; due to re-armed ");
			put_u(&l, counts_to_ns(r->w.rearm_sum[c]
					       / r->w.rearm_n[c]));
			put_s(&l, " ns mean, ");
			put_u(&l, counts_to_ns(r->w.rearm_max[c]));
			put_s(&l, " max");
		}
		printf("%s\n", l.b);
	}

	smp_wmb();
	report_pending = 0;
}
