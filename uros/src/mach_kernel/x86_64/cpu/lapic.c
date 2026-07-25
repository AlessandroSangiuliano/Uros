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
#define LAPIC_TPR		0x080	/* task priority           */
#define LAPIC_EOI		0x0B0	/* end of interrupt        */
#define LAPIC_SVR		0x0F0	/* spurious vector, and the enable bit */
#define LAPIC_ICR_LOW		0x300
#define LAPIC_ICR_HIGH		0x310

#define SVR_APIC_ENABLE		(1U << 8)

/* Interrupt command, low half. */
#define ICR_DELIVERY_FIXED	(0U << 8)
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

void lapic_broadcast_ipi(uint8_t vector)
{
	icr_send(0, ICR_DELIVERY_FIXED | ICR_DEST_ALL_BUT_SELF
		 | ICR_LEVEL_ASSERT | vector);
	icr_wait_idle();
}
