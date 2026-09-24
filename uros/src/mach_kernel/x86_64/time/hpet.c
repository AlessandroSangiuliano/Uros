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
#define GCAP_NUM_TIM_SHIFT	8
#define GCAP_NUM_TIM_MASK	0x1fU
#define GCAP_PERIOD_MAX		0x05f5e100U	/* 100 ns in fs (2.3.4) */
#define GEN_CONF_ENABLE		(1U << 0)

#define FS_PER_SECOND		1000000000000000ULL

static int			present;
static uint64_t			address;
static volatile uint8_t		*regs;
static uint32_t			period_fs;
static int			counter_64;
static unsigned			comparators;
static uint16_t			vendor;
static int			started_here;

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
	comparators = ((cap_lo >> GCAP_NUM_TIM_SHIFT) & GCAP_NUM_TIM_MASK) + 1;
	vendor = (uint16_t)(cap_lo >> 16);

	/*
	 * A period of zero is forbidden, and one above 100 ns is outside the
	 * range the specification allows.  Either means the block is not what
	 * its table said, and a ruler whose scale is wrong is worse than none.
	 */
	if (period_fs == 0 || period_fs > GCAP_PERIOD_MAX)
		return 0;

	/*
	 * Start the counter if it is halted: read-modify-write, because every
	 * other bit of this register is either reserved or the legacy routing
	 * choice, and neither is this code's to change (2.3.5).
	 */
	conf = rd32(HPET_GEN_CONF);
	started_here = 0;
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
