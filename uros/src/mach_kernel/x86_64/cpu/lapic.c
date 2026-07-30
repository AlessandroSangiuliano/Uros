/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Local APIC (#438).
 */

#include <stdint.h>

#include <cpu/lapic.h>
#include <cpu/regs.h>
#include <pmap/pmap.h>
#include <time/pit.h>
#include <trap/trap.h>

#define MSR_APIC_BASE		0x1B
#define APIC_BASE_BSP		(1UL << 8)	/* this is the boot processor */
#define APIC_BASE_X2APIC	(1UL << 10)
#define APIC_BASE_ENABLE	(1UL << 11)
#define APIC_BASE_ADDR_MASK	0x000ffffffffff000ULL

/* Register offsets, in bytes from the base. */
#define LAPIC_ID		0x020
#define LAPIC_VERSION		0x030
#define LAPIC_TPR		0x080	/* task priority           */
#define LAPIC_EOI		0x0B0	/* end of interrupt        */
#define LAPIC_SVR		0x0F0	/* spurious vector, and the enable bit */
#define LAPIC_ICR_LOW		0x300
#define LAPIC_ICR_HIGH		0x310
#define LAPIC_LVT_TIMER		0x320	/* vector, mode and mask   */
#define LAPIC_TIMER_INIT	0x380	/* what it counts down from */
#define LAPIC_TIMER_CUR		0x390	/* where it is now          */
#define LAPIC_TIMER_DIV		0x3E0	/* the divisor, oddly coded */

#define LVT_MASKED		(1U << 16)
#define LVT_TIMER_PERIODIC	(1U << 17)

/*
 * The divisor field is not a number: its three bits are 3, 1 and 0, with bit
 * 2 unused, so the encodings do not run in order.  0b1011 is divide by one,
 * and 0b0011 — the value below — is divide by sixteen.
 *
 * Sixteen because both ends matter.  Undivided, a fast bus overflows the
 * thirty-two bit counter in a few seconds, which is not enough range for a
 * slow tick to be expressed as one countdown; divided far harder, a fast
 * tick starts being quantised by the divisor itself.  Sixteen leaves room at
 * both ends on every machine this will see.
 */
#define TIMER_DIVIDE_16		0x3

#define SVR_APIC_ENABLE		(1U << 8)

/* Interrupt command, low half. */
#define ICR_DELIVERY_FIXED	(0U << 8)
#define ICR_DELIVERY_NMI	(4U << 8)
#define ICR_DELIVERY_INIT	(5U << 8)
#define ICR_DELIVERY_STARTUP	(6U << 8)
#define ICR_LEVEL_ASSERT	(1U << 14)
#define ICR_TRIGGER_LEVEL	(1U << 15)
#define ICR_DELIVERY_PENDING	(1U << 12)

/*
 * Destination shorthands, which name a set of processors in the command
 * itself instead of one processor in the high half.  Two of the three are
 * worth having: "self" needs no destination at all, and "all but self" is
 * one message where naming them one at a time would be one each.
 */
#define ICR_DEST_SELF		(1U << 18)
#define ICR_DEST_ALL_BUT_SELF	(3U << 18)

static volatile uint8_t *lapic;

static uint32_t lapic_read(unsigned reg)
{
	return *(volatile uint32_t *)(lapic + reg);
}

static void lapic_write(unsigned reg, uint32_t value)
{
	*(volatile uint32_t *)(lapic + reg) = value;
}

void lapic_init(uint64_t madt_base)
{
	uint64_t msr = rdmsr(MSR_APIC_BASE);
	uint64_t base = msr & APIC_BASE_ADDR_MASK;

	if (!(msr & APIC_BASE_ENABLE))
		panic("lapic: the local APIC is disabled in IA32_APIC_BASE");

	/*
	 * x2APIC replaces the memory-mapped registers with MSRs entirely, so
	 * the mapping below would be reading nothing.  Nothing enables it yet;
	 * finding it already on means the firmware did, and the register
	 * access here has to grow the other form before anything else can be
	 * believed.
	 */
	if (msr & APIC_BASE_X2APIC)
		panic("lapic: x2APIC mode is on and the MSR interface is not written yet");

	/*
	 * Two sources, and the MSR wins: the MADT records where the firmware
	 * placed the registers, the MSR where they are now.  A mismatch is
	 * worth saying out loud rather than silently preferring one — it means
	 * something moved them, and whatever did may have had reasons that
	 * matter elsewhere.
	 */
	if (madt_base != 0 && madt_base != base)
		panic("lapic: the MADT and IA32_APIC_BASE disagree about the base");

	lapic = (volatile uint8_t *)(uintptr_t)pmap_map_device(base, 0x1000);
}

void lapic_enable(void)
{
	if (lapic == 0)
		panic("lapic: asked to enable an APIC that was never mapped");

	/*
	 * Accept every priority.  Reset leaves this at zero, which is what we
	 * want — but a startup interrupt is not a reset, and an application
	 * processor inherits whatever the firmware left here.  A threshold set
	 * high enough turns every interrupt this CPU is sent into silence, and
	 * silence is the one symptom that points nowhere.
	 */
	lapic_write(LAPIC_TPR, 0);

	/*
	 * And the enable bit, which shares its register with the spurious
	 * vector — so naming the vector and turning the APIC on are one write,
	 * and neither can be done without the other.
	 */
	lapic_write(LAPIC_SVR, SVR_APIC_ENABLE | LAPIC_SPURIOUS_VECTOR);
}

void lapic_eoi(void)
{
	lapic_write(LAPIC_EOI, 0);
}

/* Where the registers ended up, so the mapping itself can be inspected. */
uint64_t lapic_probe_va(void)
{
	return (uint64_t)(uintptr_t)lapic;
}

int lapic_present(void)
{
	return lapic != 0;
}

uint32_t lapic_id(void)
{
	/*
	 * The id sits in the top byte of the register, not the bottom: the
	 * low bits are reserved, and reading the word as an id would give a
	 * number no CPU has.
	 */
	return lapic_read(LAPIC_ID) >> 24;
}

int lapic_is_bsp(void)
{
	return (rdmsr(MSR_APIC_BASE) & APIC_BASE_BSP) != 0;
}

/*
 * Wait for the previous command to be accepted.  The interrupt command
 * register holds one message at a time, and writing a second before the
 * first has been delivered loses it — silently, which is the whole reason
 * this loop exists rather than a delay.
 */
static void icr_wait_idle(void)
{
	while (lapic_read(LAPIC_ICR_LOW) & ICR_DELIVERY_PENDING)
		cpu_pause();
}

static void icr_send(uint32_t apic_id, uint32_t command)
{
	icr_wait_idle();

	/*
	 * The destination goes in the high half and the command in the low
	 * half, and writing the low half is what sends it — so the order is
	 * not a style question.
	 */
	lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
	lapic_write(LAPIC_ICR_LOW, command);
}

void lapic_send_init(uint32_t apic_id)
{
	icr_send(apic_id, ICR_DELIVERY_INIT | ICR_LEVEL_ASSERT
		 | ICR_TRIGGER_LEVEL);
	icr_wait_idle();

	/*
	 * The de-assert, which the older parts require and the newer ones
	 * ignore.  Sending it costs a message; not sending it leaves those
	 * older parts holding the reset line.
	 */
	icr_send(apic_id, ICR_DELIVERY_INIT | ICR_TRIGGER_LEVEL);
	icr_wait_idle();
}

void lapic_send_startup(uint32_t apic_id, uint64_t entry_pa)
{
	/*
	 * The message carries a vector, and the target turns it into an
	 * address by shifting left twelve — so the entry point has to be a
	 * page number below 1 MiB, and anything else is not a startup address
	 * that can be expressed at all.
	 */
	if (entry_pa >= 0x100000 || (entry_pa & 0xFFF))
		panic("lapic: startup entry is not a page below 1 MiB");

	icr_send(apic_id, ICR_DELIVERY_STARTUP | ICR_LEVEL_ASSERT
		 | (uint32_t)(entry_pa >> 12));
	icr_wait_idle();
}

void lapic_send_self(uint8_t vector)
{
	/*
	 * No wait for delivery afterwards, unlike the startup sequence: this
	 * one is aimed at the processor that is asking, so waiting for the
	 * command to be accepted would be waiting for an interrupt that cannot
	 * be taken until this returns and enables it.  The caller looks for
	 * the effect instead.
	 */
	icr_send(0, ICR_DELIVERY_FIXED | ICR_DEST_SELF | ICR_LEVEL_ASSERT
		 | vector);
}

void lapic_send_ipi(uint32_t apic_id, uint8_t vector)
{
	icr_send(apic_id, ICR_DELIVERY_FIXED | ICR_LEVEL_ASSERT | vector);
	icr_wait_idle();
}

void lapic_send_nmi(uint32_t apic_id)
{
	/*
	 * No vector: NMI delivery ignores the field and the processor takes
	 * vector 2 by definition.  Named destination rather than the self
	 * shorthand even when the destination is this processor, because that
	 * shorthand is defined for fixed delivery only.
	 */
	icr_send(apic_id, ICR_DELIVERY_NMI | ICR_LEVEL_ASSERT);
	icr_wait_idle();
}

/* ------------------------------------------------------------------ */
/*  The timer (#409)                                                    */
/* ------------------------------------------------------------------ */
/*
 * Measured once, on the boot processor, and used by all of them.
 *
 * The rate is a property of the machine rather than of a processor — every
 * local APIC timer counts off the same bus or crystal clock — while the
 * timer itself is per processor, which is the whole reason for preferring it
 * to the 8254.  So one measurement is shared and each processor arms its own
 * countdown from it.
 *
 * ⚠️ It has to be measured on one processor at a time regardless: the ruler
 * is the 8254, and the 8254 is a single global device. Two processors
 * calibrating at once would be two callers programming one counter.
 */
static uint32_t timer_hz;

static uint32_t timer_measure_once(void)
{
	uint32_t before, after;

	/*
	 * Masked throughout.  Calibration is a measurement, not a service, and
	 * a vector delivered in the middle of it would be an interrupt nobody
	 * has arranged to handle yet.
	 */
	lapic_write(LAPIC_LVT_TIMER, LVT_MASKED);
	lapic_write(LAPIC_TIMER_DIV, TIMER_DIVIDE_16);

	/*
	 * Count down from the top, so the interval cannot reach zero and wrap
	 * into a small difference that looks like a slow clock.
	 */
	lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFFu);
	before = lapic_read(LAPIC_TIMER_CUR);

	if (!pit_delay_us(LAPIC_CALIBRATE_US)) {
		lapic_write(LAPIC_TIMER_INIT, 0);
		return 0;
	}

	after = lapic_read(LAPIC_TIMER_CUR);
	lapic_write(LAPIC_TIMER_INIT, 0);	/* stop */

	if (after >= before)
		return 0;			/* it never counted */

	return (uint32_t)(((uint64_t)(before - after) * 1000000u)
			  / LAPIC_CALIBRATE_US);
}

/*
 * Twice, and the two must agree — the same discipline the timestamp counter's
 * calibration already had, applied here because the two are compared against
 * each other later and a bound on that comparison is only as good as the
 * weaker of the two measurements.
 *
 * A single reading cannot tell a frequency from an interval that was
 * interrupted: a system-management interrupt inside the window, or the host
 * taking the emulator off its processor, gives a number that is wrong and
 * looks ordinary. Two that agree are not proof — they share a ruler — but one
 * that disagrees with itself is proof of the opposite, which is the case this
 * can decide.
 *
 * One part in sixty-four, matching the counter's, so the two sides of the
 * comparison are held to the same standard rather than to two numbers chosen
 * separately.
 */
#define TIMER_CALIBRATE_TOLERANCE	64

static uint32_t timer_hz_run[2];

uint32_t lapic_timer_calibrate(void)
{
	uint32_t spread, allowed;

	timer_hz = 0;

	if (!lapic_present())
		return 0;

	timer_hz_run[0] = timer_measure_once();
	timer_hz_run[1] = timer_measure_once();

	if (timer_hz_run[0] == 0 || timer_hz_run[1] == 0)
		return 0;

	spread = timer_hz_run[0] > timer_hz_run[1]
	       ? timer_hz_run[0] - timer_hz_run[1]
	       : timer_hz_run[1] - timer_hz_run[0];
	allowed = timer_hz_run[0] / TIMER_CALIBRATE_TOLERANCE;

	if (spread > allowed)
		return 0;

	timer_hz = (timer_hz_run[0] + timer_hz_run[1]) / 2;
	return timer_hz;
}

uint32_t lapic_timer_hz_run(unsigned which)
{
	return which < 2 ? timer_hz_run[which] : 0;
}

uint32_t lapic_timer_hz(void)
{
	return timer_hz;
}

int lapic_timer_start(unsigned hz, uint8_t vector)
{
	uint32_t count;

	if (timer_hz == 0 || hz == 0)
		return 0;

	count = timer_hz / hz;

	/*
	 * A count of zero means "stopped", not "as fast as possible", so a
	 * tick faster than the timer can express has to be refused rather than
	 * silently turned off.  This is the shape of failure that looks like a
	 * working kernel with no clock.
	 */
	if (count == 0)
		return 0;

	/*
	 * The divisor again on every processor: it is per-APIC state, and an
	 * application processor arrives with whatever reset left in it rather
	 * than with what the boot processor chose during calibration.
	 */
	lapic_write(LAPIC_TIMER_DIV, TIMER_DIVIDE_16);
	lapic_write(LAPIC_LVT_TIMER, LVT_TIMER_PERIODIC | vector);
	lapic_write(LAPIC_TIMER_INIT, count);
	return 1;
}

void lapic_timer_stop(void)
{
	if (!lapic_present())
		return;

	/* Count first, then mask: masking alone leaves it counting. */
	lapic_write(LAPIC_TIMER_INIT, 0);
	lapic_write(LAPIC_LVT_TIMER, LVT_MASKED);
}

void lapic_broadcast_ipi(uint8_t vector)
{
	icr_send(0, ICR_DELIVERY_FIXED | ICR_DEST_ALL_BUT_SELF
		 | ICR_LEVEL_ASSERT | vector);
	icr_wait_idle();
}
