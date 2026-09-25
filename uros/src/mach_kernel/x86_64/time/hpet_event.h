/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The HPET as a clock-event device (#593): the third backend, for when the
 * local APIC's timer cannot be used.
 *
 * ── ONE COMPARATOR, AND A BROADCAST ───────────────────────────────────
 *
 * The HPET is one device for the whole machine, and the tick is per
 * processor.  A comparator per processor would need as many comparators as
 * processors, each able to reach its own: QEMU gives three, and on its `pc'
 * board every one of them can reach only I/O APIC input 2 unless FSB
 * delivery was asked for.  So per-processor comparators could only ever be
 * the case some machines allow, with a broadcast behind them for the rest --
 * two mechanisms, and the second is needed anyway.  Linux and FreeBSD
 * decide the same way when the local timer is unusable: one global device
 * fires on one processor, and the others are sent their tick by IPI (Linux's
 * lapic_timer_broadcast(), FreeBSD's IPI_HARDCLOCK in kern_clocksource.c).
 *
 * So: one comparator, delivered to the boot processor.  Every processor
 * arms its own deadline here; the comparator is programmed for the earliest;
 * when it fires, every processor whose deadline has come is sent the tick
 * vector -- the boot processor included, by a self-IPI -- and the
 * comparator is programmed for the next.
 *
 * 🔑 THE COMPARATOR'S VECTOR IS CLASS FIFTEEN, THE TICK IT SENDS IS FOURTEEN.
 * The broadcast must not wait for the boot processor's spl level: if it
 * did, one processor's critical section would stop every processor's clock,
 * the coupling the 8254 had and the local timer was chosen to end (see
 * <cpu/lapic.h>).  And each processor's tick must still wait for ITS OWN
 * level, because the scheduler is written on the promise that splsched()
 * stops the clock (#522) -- which is why the boot processor gets its tick by
 * a self-IPI on the tick's vector rather than by a call from here.
 *
 * ── DEEP IDLE ─────────────────────────────────────────────────────────
 *
 * The same mechanism is what a processor whose local timer stops in a deep
 * C-state needs: a timer outside the core that wakes it.  The idle loop only
 * halts today, and C1 keeps the local timer running, so nothing needs it
 * yet; when an idle loop enters deeper states on a processor without ARAT
 * (CPUID.06H:EAX[2]), this backend's arm() is what that processor calls
 * before it sleeps.
 */

#ifndef _X86_64_TIME_HPET_EVENT_H_
#define _X86_64_TIME_HPET_EVENT_H_

#include <x86_64/time/clock_event.h>

/*
 * The comparator's own vector: class fifteen, the one spl never defers --
 * see above and <trap/trap.h>'s plan of the vector space.  0xF0 is where the
 * tick itself lived until #522 moved it to class fourteen.
 */
#define HPET_EVENT_VECTOR	0xF0

extern const struct clock_event_ops hpet_event_ops;

#endif	/* _X86_64_TIME_HPET_EVENT_H_ */
