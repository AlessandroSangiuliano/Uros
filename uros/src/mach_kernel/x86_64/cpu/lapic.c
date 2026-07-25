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
#include <trap/trap.h>

#define MSR_APIC_BASE		0x1B
#define APIC_BASE_BSP		(1UL << 8)	/* this is the boot processor */
#define APIC_BASE_X2APIC	(1UL << 10)
#define APIC_BASE_ENABLE	(1UL << 11)
#define APIC_BASE_ADDR_MASK	0x000ffffffffff000ULL

/* Register offsets, in bytes from the base. */
#define LAPIC_ID		0x020
#define LAPIC_VERSION		0x030
#define LAPIC_ICR_LOW		0x300
#define LAPIC_ICR_HIGH		0x310

/* Interrupt command, low half. */
#define ICR_DELIVERY_INIT	(5U << 8)
#define ICR_DELIVERY_STARTUP	(6U << 8)
#define ICR_LEVEL_ASSERT	(1U << 14)
#define ICR_TRIGGER_LEVEL	(1U << 15)
#define ICR_DELIVERY_PENDING	(1U << 12)

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
