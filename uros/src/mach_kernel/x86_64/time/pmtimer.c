/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The ACPI power-management timer, as a ruler (#508).  See pmtimer.h.
 */

#include <stdint.h>

#include <cpu/acpi.h>
#include <cpu/regs.h>
#include <pmap/pmap.h>
#include <time/pmtimer.h>

static int			present;
static unsigned			width;
static uint32_t			mask;
static int			is_io;
static uint16_t			port;
static uint64_t			address;
static volatile uint32_t	*mmio;

int pmtimer_init(void)
{
	struct acpi_pm_timer	pm;

	present = 0;
	acpi_pm_timer(&pm);
	if (pm.address == 0)
		return 0;

	width = pm.width;
	mask = width == 32 ? 0xffffffffu : 0x00ffffffu;
	address = pm.address;

	if (pm.space_id == ACPI_GAS_IO) {
		/*
		 * The address field is 64 bits and a port is 16.  A FADT that
		 * names a port above 0xffff is not describing this machine's
		 * I/O space, and reading the truncated port would read
		 * somebody else's register.
		 */
		if (pm.address > 0xffff - 3)
			return 0;
		is_io = 1;
		port = (uint16_t)pm.address;
	} else {
		is_io = 0;
		mmio = (volatile uint32_t *)(uintptr_t)
			pmap_map_device(pm.address, sizeof(uint32_t));
	}

	present = 1;
	return 1;
}

int pmtimer_present(void)
{
	return present;
}

unsigned pmtimer_width(void)
{
	return width;
}

int pmtimer_is_io(void)
{
	return is_io;
}

uint64_t pmtimer_address(void)
{
	return address;
}

/*
 * One 32-bit read, which is how the specification says the register is
 * accessed, masked to the width the FADT states: the bits above 24 of a
 * 24-bit timer are reserved, not zero.
 */
uint32_t pmtimer_read(void)
{
	uint32_t v;

	v = is_io ? inl(port) : *mmio;
	return v & mask;
}

uint32_t pmtimer_delta(uint32_t from, uint32_t to)
{
	return (to - from) & mask;
}
