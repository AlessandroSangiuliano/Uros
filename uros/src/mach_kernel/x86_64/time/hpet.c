/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The HPET's main counter, as a ruler (#508).  See hpet.h.
 */

#include <stdint.h>

#include <cpu/acpi.h>
#include <pmap/pmap.h>
#include <time/hpet.h>

/* Register offsets and fields, IA-PC HPET 1.0a, 2.3. */
#define HPET_GCAP_ID		0x000	/* read-only, 64 bits */
#define HPET_GEN_CONF		0x010	/* read-write */
#define HPET_MAIN_COUNTER	0x0f0	/* read-write while halted */
#define HPET_BLOCK_SIZE		0x400	/* one block is 1 KiB (2.1.1) */

#define GCAP_COUNT_SIZE_CAP	(1U << 13)
#define GCAP_LEG_RT_CAP		(1U << 15)
#define GCAP_NUM_TIM_SHIFT	8
#define GCAP_NUM_TIM_MASK	0x1fU
#define GCAP_PERIOD_MAX		0x05f5e100U	/* 100 ns in fs (2.3.4) */
#define GEN_CONF_ENABLE		(1U << 0)
#define GEN_CONF_LEG_RT		(1U << 1)

/* The comparators, 2.3.8 - 2.3.10: 0x20 bytes each from 0x100. */
#define HPET_TN_CONF(n)		(0x100 + 0x20 * (n))	/* + 4: route cap */
#define HPET_TN_COMPARATOR(n)	(0x108 + 0x20 * (n))
#define HPET_TN_FSB_ROUTE(n)	(0x110 + 0x20 * (n))	/* value, + 4: address */

#define TN_INT_TYPE_LEVEL	(1U << 1)
#define TN_INT_ENB		(1U << 2)
#define TN_TYPE_PERIODIC	(1U << 3)
#define TN_PER_INT_CAP		(1U << 4)
#define TN_SIZE_CAP		(1U << 5)
#define TN_VAL_SET		(1U << 6)
#define TN_32MODE		(1U << 8)
#define TN_INT_ROUTE_SHIFT	9
#define TN_INT_ROUTE_MASK	(0x1fU << TN_INT_ROUTE_SHIFT)
#define TN_FSB_EN		(1U << 14)
#define TN_FSB_INT_DEL_CAP	(1U << 15)
/* Bits 0, 7 and 31:16 are reserved, "write 0" (2.3.8). */
#define TN_WRITABLE		(TN_INT_TYPE_LEVEL | TN_INT_ENB \
				 | TN_TYPE_PERIODIC | TN_VAL_SET | TN_32MODE \
				 | TN_INT_ROUTE_MASK | TN_FSB_EN)

#define FS_PER_SECOND		1000000000000000ULL

static int			present;
static uint64_t			address;
static volatile uint8_t		*regs;
static uint32_t			period_fs;
static int			counter_64;
static int			legacy_capable;
static unsigned			comparators;
static uint16_t			vendor;
static int			started_here;
static int			taken;
static int			legacy_found_on;
static uint32_t			comparators_found_on;

/*
 * #593: the block as a firmware that drove its own tick with it would leave
 * it -- LegacyReplacement on, and the last comparator interrupting -- for
 * take_block() to find, silence and report.  QEMU's firmware leaves neither,
 * so without this no boot would ever print those two findings, and a report
 * that no boot prints is not known to work.
 */
#ifndef	ABLATE_593_FIRMWARE_LEFT
#define	ABLATE_593_FIRMWARE_LEFT	0
#endif

/*
 * Every access is 32 bits wide and at an offset the specification allows
 * for one (2.3.2 rule 1).  A 64-bit access would be allowed at the offsets
 * used here, but the specification also warns that some platforms split it
 * in two (2.4.7), and a split read of the counter is exactly the torn value
 * the 32-bit protocol below exists to avoid.
 */
static uint32_t rd32(unsigned off)
{
	return *(volatile uint32_t *)(regs + off);
}

static void wr32(unsigned off, uint32_t v)
{
	*(volatile uint32_t *)(regs + off) = v;
}

static void comparator_config(unsigned n, uint32_t set);

/*
 * 🔴 THE BLOCK IS TAKEN AT THE FIRST LOOK, WHOEVER DRIVES IT LATER (#593).
 *
 * The firmware may have used the block for a tick of its own, and nothing
 * obliges it to stop.  Left as found:
 *
 *   - a comparator still interrupting sends its FSB message, or pulses its
 *     input, on a vector nobody here chose -- and an FSB message goes
 *     straight to a local APIC, where no I/O APIC mask stops it.  The boot
 *     that never puts its tick on the HPET is the one that would take it,
 *     so this cannot wait for the HPET's backend to start.
 *
 *   - LegacyReplacement left on makes the routing of timers 0 and 1 "have no
 *     impact" (2.3.5), FSB included: the backend's message from comparator 0
 *     would never be sent, and every processor's clock would stop.  On
 *     hardware only -- QEMU sends it anyway, so no boot here would show it.
 *     And the 8254 and the RTC stay silent, which device_md_irq_register()
 *     would blame on this kernel's clock.
 *
 * So every comparator is silenced first, before the switch below could send
 * timer 0 or 1 to an input of its own, and then LegacyReplacement goes off.
 * Linux does the same in hpet_enable(), FreeBSD in its attach.  Once only,
 * because a second look after the backend has started would silence the
 * backend; and what was found is only ever added to, for rulers_selftest()
 * to say whichever look came first.
 */
static void take_block(void)
{
	uint32_t	conf;
	unsigned	n;

#if ABLATE_593_FIRMWARE_LEFT
	if (legacy_capable)
		wr32(HPET_GEN_CONF, rd32(HPET_GEN_CONF) | GEN_CONF_LEG_RT);
	wr32(HPET_TN_CONF(comparators - 1),
	     (rd32(HPET_TN_CONF(comparators - 1)) & TN_WRITABLE) | TN_INT_ENB);
#endif

	for (n = 0; n < comparators; n++) {
		if (rd32(HPET_TN_CONF(n)) & TN_INT_ENB)
			comparators_found_on |= 1U << n;
		comparator_config(n, 0);
	}

	conf = rd32(HPET_GEN_CONF);
	if (conf & GEN_CONF_LEG_RT) {
		legacy_found_on = 1;
		wr32(HPET_GEN_CONF, conf & ~GEN_CONF_LEG_RT);
	}
	taken = 1;
}

int hpet_init(void)
{
	struct acpi_hpet	t;
	uint32_t		cap_lo, conf;

	present = 0;
	acpi_hpet(&t);
	if (!t.found || t.base.space_id != ACPI_GAS_MEMORY || t.base.address == 0)
		return 0;

	address = t.base.address;
	regs = (volatile uint8_t *)(uintptr_t)
		pmap_map_device(address, HPET_BLOCK_SIZE);

	cap_lo = rd32(HPET_GCAP_ID);
	period_fs = rd32(HPET_GCAP_ID + 4);
	counter_64 = (cap_lo & GCAP_COUNT_SIZE_CAP) != 0;
	legacy_capable = (cap_lo & GCAP_LEG_RT_CAP) != 0;
	comparators = ((cap_lo >> GCAP_NUM_TIM_SHIFT) & GCAP_NUM_TIM_MASK) + 1;
	vendor = (uint16_t)(cap_lo >> 16);

	/*
	 * A period of zero is forbidden, and one above 100 ns is outside the
	 * range the specification allows.  Either means the block is not what
	 * its table said, and a ruler whose scale is wrong is worse than none.
	 */
	if (period_fs == 0 || period_fs > GCAP_PERIOD_MAX)
		return 0;

	if (!taken)
		take_block();

	/*
	 * Start the counter if it is halted: read-modify-write, because every
	 * other bit of this register is reserved (2.3.5) but LegacyReplacement,
	 * which take_block() has decided and which is the clock-event backend's
	 * from then on.  Only ever set, like take_block()'s findings: rulers_find()
	 * looks twice, and the second look finds the counter running.
	 */
	conf = rd32(HPET_GEN_CONF);
	if ((conf & GEN_CONF_ENABLE) == 0) {
		wr32(HPET_GEN_CONF, conf | GEN_CONF_ENABLE);
		started_here = 1;
	}

	present = 1;
	return 1;
}

int hpet_present(void)
{
	return present;
}

uint64_t hpet_address(void)
{
	return address;
}

uint32_t hpet_period_fs(void)
{
	return period_fs;
}

uint64_t hpet_hz(void)
{
	return period_fs ? FS_PER_SECOND / period_fs : 0;
}

int hpet_counter_64(void)
{
	return counter_64;
}

unsigned hpet_comparators(void)
{
	return comparators;
}

uint16_t hpet_vendor(void)
{
	return vendor;
}

int hpet_started_here(void)
{
	return started_here;
}

int hpet_legacy_found_on(void)
{
	return legacy_found_on;
}

uint32_t hpet_comparators_found_on(void)
{
	return comparators_found_on;
}

/*
 * The counter, read so that it is never torn.
 *
 * A 64-bit counter is read high, low, high, and again if the two highs differ
 * (2.4.7): the low half may carry into the high half between two 32-bit
 * reads, and the value made of the old high and the new low is 2^32 counts
 * -- 43 s at 100 MHz, five minutes at 14.318 MHz -- in the past, not a
 * rounding error.
 */
uint64_t hpet_read(void)
{
	uint32_t hi, lo, hi2;

	if (!counter_64)
		return rd32(HPET_MAIN_COUNTER);

	do {
		hi = rd32(HPET_MAIN_COUNTER + 4);
		lo = rd32(HPET_MAIN_COUNTER);
		hi2 = rd32(HPET_MAIN_COUNTER + 4);
	} while (hi != hi2);

	return ((uint64_t)hi << 32) | lo;
}

uint32_t hpet_read32(void)
{
	return rd32(HPET_MAIN_COUNTER);
}

/* ------------------------------------------------------------------ */
/*  The comparators (#593)                                              */
/* ------------------------------------------------------------------ */

int hpet_legacy_capable(void)
{
	return legacy_capable;
}

void hpet_comparator_caps(unsigned n, struct hpet_comparator_caps *out)
{
	uint32_t conf = rd32(HPET_TN_CONF(n));

	out->fsb = (conf & TN_FSB_INT_DEL_CAP) != 0;
	out->periodic = (conf & TN_PER_INT_CAP) != 0;
	out->size_64 = (conf & TN_SIZE_CAP) != 0;
	out->route_cap = rd32(HPET_TN_CONF(n) + 4);
}

/*
 * The one configuration this kernel uses, whatever the delivery: one-shot,
 * edge-triggered, and 32 bits wide.
 *
 * One-shot, because a deadline is what the clock-event layer asks for.
 * Edge, because an FSB message has no level to hold (2.4.2.3 requires edge
 * there) and because a level-triggered comparator must be cleared in the
 * status register by every handler (2.4.6), which is one more access to a
 * device that costs an exit to the host per access under an emulator.
 *
 * 🔑 32 bits even on a 64-bit comparator.  A 64-bit comparator is written in
 * two 32-bit halves, and between the two writes it holds a value made of one
 * old half and one new -- a match nobody asked for, or a deadline in the far
 * past.  One 32-bit write is atomic, and the counter's low half is what the
 * backend compares against anyway (hpet_read32()).  The cost is the wrap:
 * a one-shot 32-bit comparator also fires when the counter wraps (2.3.9.2.1),
 * every 43 s at 100 MHz, and the backend's handler has to take an interrupt
 * with nothing due -- it counts them.
 *
 * ⚠️ Read-modify-write over the writable bits only: 0, 7 and 31:16 are
 * reserved and to be written as zero (2.3.8).
 */
static void comparator_config(unsigned n, uint32_t set)
{
	uint32_t conf = rd32(HPET_TN_CONF(n)) & TN_WRITABLE;

	conf &= ~(TN_INT_TYPE_LEVEL | TN_INT_ENB | TN_TYPE_PERIODIC
		  | TN_VAL_SET | TN_FSB_EN);
	conf |= TN_32MODE | set;
	wr32(HPET_TN_CONF(n), conf);
}

/*
 * Before interrupts are enabled, a comparator that cannot match for a whole
 * wrap: the reset value is all ones, which in 32-bit mode is a match at the
 * next wrap of the low half -- up to 43 s away at 100 MHz, or a few
 * microseconds.
 */
static void comparator_park(unsigned n)
{
	wr32(HPET_TN_COMPARATOR(n), rd32(HPET_MAIN_COUNTER) - 1);
}

void hpet_comparator_off(unsigned n)
{
	comparator_config(n, 0);
}

void hpet_comparator_fsb(unsigned n, uint32_t addr, uint32_t data)
{
	comparator_config(n, 0);
	comparator_park(n);

	/*
	 * The message before the switch that sends it: the route register
	 * holds the value in its low half and the address in its high half
	 * (2.3.10), and until FSB_EN is set neither is used.
	 */
	wr32(HPET_TN_FSB_ROUTE(n), data);
	wr32(HPET_TN_FSB_ROUTE(n) + 4, addr);
	comparator_config(n, TN_FSB_EN | TN_INT_ENB);
}

int hpet_comparator_legacy(unsigned n)
{
	if (!legacy_capable || n > 1)
		return 0;

	comparator_config(n, 0);
	comparator_park(n);

	/*
	 * LegacyReplacement takes timer 0 to the 8254's line and timer 1 to the
	 * RTC's, and silences both of those devices' interrupts (2.4.2.1) --
	 * both, whichever timer is used, because the bit is one for the block.
	 * Read-modify-write, for hpet_init()'s reason.
	 */
	wr32(HPET_GEN_CONF, rd32(HPET_GEN_CONF) | GEN_CONF_LEG_RT);
	comparator_config(n, TN_INT_ENB);
	return 1;
}

int hpet_legacy_routed(void)
{
	return present && (rd32(HPET_GEN_CONF) & GEN_CONF_LEG_RT) != 0;
}

void hpet_comparator_set(unsigned n, uint32_t value)
{
	wr32(HPET_TN_COMPARATOR(n), value);
}
