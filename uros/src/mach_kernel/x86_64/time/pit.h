/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The 8254 interval timer, used only as a ruler (#409).
 *
 * This is not the kernel's clock and will never become it.  The tick comes
 * from the local APIC timer, which is per-processor; the 8254 is a single
 * global device, and a single global device with a single global mask is the
 * structural coupling that starved the disk on i386 when the boot processor
 * sat at a high priority level (#304).  Building the tick on one again would
 * be repeating a mistake whose cost is already written down.
 *
 * What it is good for is being a *known frequency*.  The local APIC timer
 * counts at a rate nobody documents — it is derived from the bus or core
 * crystal, and the processor does not always say what that is — so it has to
 * be measured against something that does say.  The 8254 says: 1193182 Hz,
 * fixed since the part was designed around a television colour subcarrier
 * crystal, and unchanged through every machine that has inherited it.
 *
 * ── Channel 2, and why not channel 0 ──────────────────────────────────
 *
 * Channel 0 is wired to the interrupt controller.  Using it as a ruler means
 * either taking its interrupts, which is the thing being bootstrapped, or
 * reprogramming the line the firmware left armed.  Channel 2 is wired to the
 * speaker instead, and its gate is a bit in a port the kernel already owns —
 * so it can be started, polled and stopped without an interrupt controller
 * existing at all, which is exactly the situation here.
 *
 * The speaker output is disconnected while this runs.  A calibration that
 * announces itself audibly is a calibration somebody will eventually be
 * asked to explain.
 */

#ifndef _X86_64_TIME_PIT_H_
#define _X86_64_TIME_PIT_H_

#include <stdint.h>

/*
 * The crystal, in hertz.  1193182 is 105/88 MHz, which is the colour
 * subcarrier frequency divided down — the number is strange because its
 * origin is television and not computing.
 */
#define PIT_HZ		1193182u

/*
 * The longest interval one countdown can express.  The counter is sixteen
 * bits, so it cannot be asked for more than 65535 ticks, which is about
 * 54.9 milliseconds.  Asking for more would silently wrap and return an
 * answer that looks reasonable, so the caller is stopped instead.
 */
#define PIT_MAX_US	54000u

/*
 * Run channel 2 for `us` microseconds and return when it has finished.
 *
 * Polled, not waited on: there is no interrupt controller yet, and the point
 * of this file is to work before there is one.  The poll watches the gate
 * output bit rather than reading the counter back, because reading a counter
 * that is still running takes a latch command and is a longer sequence than
 * the thing being measured.
 *
 * Returns zero if `us` is out of range and nothing was measured, so a caller
 * cannot mistake a refusal for a very fast machine.
 */
int pit_delay_us(unsigned us);

#endif	/* _X86_64_TIME_PIT_H_ */
