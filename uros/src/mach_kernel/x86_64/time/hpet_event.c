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
static int		programmed;
static uint32_t		programmed_at;
#if ABLATE_593_BEHIND
static unsigned		behind_writes;
#endif

/* What the broadcast counted, all in HPET counts. */
struct window {
	uint64_t	fires;		/* comparator interrupts */
	uint64_t	idle_fires;	/* ... with nothing due */
	uint64_t	passed;		/* writes found behind the counter */
	uint64_t	kicks;		/* ticks sent, self-IPIs included */
	uint64_t	late_n, late_sum;
	uint32_t	late_min, late_max;
	uint64_t	rearm_n[NCPUS], rearm_sum[NCPUS];
	uint32_t	rearm_max[NCPUS];
};

static struct window	w;

static uint32_t ns_to_counts(uint64_t ns)
{
	uint64_t c = (ns / NS_PER_SEC) * hz + ((ns % NS_PER_SEC) * hz) / NS_PER_SEC;

	if (c == 0)
		c = 1;
	if (c > HPET_EV_MAX_COUNTS)
		c = HPET_EV_MAX_COUNTS;
	return (uint32_t)c;
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

	w = (struct window){ .late_min = UINT32_MAX };
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

static int hpet_ev_arm(uint64_t ns)
{
	unsigned	self = (unsigned)cpu_number();
	uint64_t	kick = 0;
	uint32_t	counts, now, base;

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
		if ((int32_t)(woken_for[self] + counts - now) > (int32_t)ahead)
			base = woken_for[self];
	}

	due[self] = base + counts;
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
