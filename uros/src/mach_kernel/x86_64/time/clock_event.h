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
 * clock_event.h — when this processor wants to be woken (#459).
 *
 * The kernel had no clock: hertz_tick() had zero callers on x86-64, so no
 * quantum was ever decremented and the scheduler, having handed out the first
 * thread, would never have taken it back.  This is the wire from the hardware
 * to the kernel -- and the shape of that wire matters more than the timer
 * behind it.
 *
 * ONE QUESTION, NOT TWO.  A timer answers "wake me at T".  It does not answer
 * "what time is it" -- that is the timebase (#318), a global clock that has
 * to agree across cores.  They both read the TSC and they are not the same
 * job: a deadline only needs THIS core's counter to move forward, and a ten
 * percent frequency error over ten milliseconds is invisible to a quantum.
 * Conflating them is how a scheduler ends up depending on cross-core
 * synchronisation it never needed.
 *
 * TWO BACKENDS, BOTH REAL.
 *
 *   tsc-deadline   IA32_TSC_DEADLINE (MSR 0x6E0), CPUID.01H:ECX[24].  Write
 *                  an absolute TSC value, get an interrupt when the counter
 *                  passes it.  A WRMSR, not an MMIO round trip, and precise
 *                  to the counter rather than to a bus-clock division.
 *   lapic-oneshot  The local APIC countdown, one-shot.  Works everywhere and
 *                  needs no CPUID feature, at the price of an MMIO write per
 *                  arm -- which on an emulated APIC leaves the guest.
 *
 * Both are implemented and both run here: KVM emulates the TSC deadline timer
 * in its in-kernel APIC even on hosts whose silicon lacks it, so `-cpu max'
 * exercises the deadline path on a machine with no tsc_deadline_timer flag at
 * all.  That is the whole reason the second backend was written now rather
 * than "later, on hardware that has it": a backend nothing ever executes is
 * collaudated by nobody, and this tree has already paid for that lesson three
 * times in one day.
 *
 * WHY ONE-SHOT AT ALL, GIVEN THE COST.  Measured on i386, which has the same
 * shape of handler: hertz_tick() itself costs 2786 cycles (931 ns), while the
 * whole handler including the APIC EOI costs 15022 -- so an MMIO to the APIC
 * dominates everything else under KVM.  One-shot pays one more of those per
 * event than periodic does.  It is still right, because the win is not cycles
 * per tick: it is the ticks that never happen.  An idle processor under
 * periodic wakes a hundred times a second to learn there is nothing to do,
 * which is exactly what the #357 idle HLT went to the trouble of avoiding.
 */

#ifndef	_X86_64_TIME_CLOCK_EVENT_H_
#define	_X86_64_TIME_CLOCK_EVENT_H_

#include <stdint.h>

/*
 * A backend.  Deliberately small: everything above it asks only "wake me in
 * this many nanoseconds", and any timer that can do that fits.
 */
struct clock_event_ops {
	const char	*name;

	/* Usable on this machine at all?  Runs once, on the boot processor. */
	int		(*probe)(void);

	/* Per-processor arming state.  Runs on every processor including the
	 * boot one -- an application processor arrives with whatever reset
	 * left in its APIC, not with what the boot processor configured. */
	void		(*setup)(uint8_t vector);

	/* Fire once, about `ns' from now.  Returns zero if the interval
	 * cannot be expressed, which the caller must treat as a failure to
	 * arm rather than as "armed for zero" -- a countdown of zero means
	 * STOPPED, and that failure looks exactly like a working kernel whose
	 * clock never fires. */
	int		(*arm)(uint64_t ns);

	void		(*stop)(void);
};

/*
 * Choose a backend.  Boot processor, once, before any processor arms.
 * `vector' is the IDT vector the backend must deliver on.
 */
void		clock_event_init(uint8_t vector);

/* Per-processor setup.  Every processor, boot one included. */
void		clock_event_setup_cpu(void);

/* Arm this processor's timer.  Zero means it could not be armed. */
int		clock_event_arm_ns(uint64_t ns);

/* Arm for one scheduler tick (the rate chosen by clock_event_init). */
int		clock_event_arm_tick(void);

void		clock_event_stop(void);

/* Which backend won, for the boot log and for DDB.  Never NULL. */
const char	*clock_event_name(void);

/* The scheduler tick rate, in Hz, and its period in nanoseconds. */
unsigned	clock_event_hz(void);

/*
 * Print whatever the tick handlers have recorded and not yet said (#461).
 *
 * ⚠️ THREAD CONTEXT ONLY.  The reports are recorded in the timer interrupt and
 * printed here because printf() takes a lock that a thread on the same
 * processor may already hold -- and the handler, reached through a gate that
 * clears IF, would wait for it forever.  Calling this from an interrupt puts
 * that deadlock straight back.
 */
void	clock_event_drain_reports(void);
uint64_t	clock_event_tick_ns(void);

/*
 * The tick handler itself: hertz_tick(), the EOI, and the re-arm.
 *
 * ⚠️ Reads cs and rip from the frame, so it must NOT be reachable through an
 * spl deferral without the frame being real -- see the note in the
 * implementation.  trap.h's replay contract hands out a frame with only
 * `vector` valid.
 */
struct trap_frame;
void		clock_event_tick(struct trap_frame *frame);

/*
 * Measure the tick against the TSC for `seconds', before the
 * machine-independent kernel is entered (-C).  Its own handler, which does
 * not call hertz_tick(): there is no thread here to charge time to.
 */
void		clock_event_burnin(unsigned seconds);

#endif	/* _X86_64_TIME_CLOCK_EVENT_H_ */
