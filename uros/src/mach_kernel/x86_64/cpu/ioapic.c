/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The I/O APIC: where device interrupts come in (#409).
 */

#include <stdint.h>

#include <cpu/acpi.h>
#include <cpu/ioapic.h>
#include <cpu/regs.h>	/* read_rflags, the pair lock below */
#include <pmap/pmap.h>
#include <sync/atomic.h>	/* atomic_swap8, the pair lock below */
#include <trap/trap.h>

/*
 * The two mapped addresses. Everything else is reached by writing a register
 * number to the first and using the second.
 */
#define IOAPIC_REGSEL	0x00
#define IOAPIC_WINDOW	0x10

#define IOAPIC_REG_ID		0x00
#define IOAPIC_REG_VERSION	0x01
#define IOAPIC_REG_REDIR	0x10	/* two registers per pin, from here */

/*
 * A redirection entry is sixty-four bits across two thirty-two bit
 * registers. The low half is everything about the interrupt and the high
 * half is only the destination, which is why the two are written in that
 * order below: the destination has to be right before the pin is unmasked,
 * and the mask lives in the low half.
 */
#define RTE_VECTOR_MASK		0x000000FFu
#define RTE_DELIVERY_FIXED	(0u << 8)
#define RTE_DEST_PHYSICAL	(0u << 11)
#define RTE_POLARITY_LOW	(1u << 13)
#define RTE_TRIGGER_LEVEL	(1u << 15)
#define RTE_MASKED		(1u << 16)

static volatile uint8_t *io;
static unsigned pins;
static uint32_t base_gsi;

/*
 * 🔴 ONE PAIR FOR THE WHOLE MACHINE, AND A READ-MODIFY-WRITE SELECTS TWICE
 * (#599).  Two processors interleaving inside it each read or write the
 * other's pin.  On i386 that put pin 11's entry on pin 2 and stopped the
 * clock for good, and put an unhandled line's entry on AHCI's pin.  Here
 * device_machdep.c masks and unmasks from whichever processor handles the
 * interrupt or returns the line, so the same race is open.
 *
 * So every access sequence holds ioapic_pair_lock, the pattern of the PCI
 * configuration pair (pci_cfg.c): a leaf, interrupts off for the hold so an
 * interrupt on the same processor cannot start a sequence inside one, and
 * not simple_lock, because ioapic_init() runs before percpu_activate().
 */
static volatile uint8_t	ioapic_pair_lock;

static inline uint64_t ioapic_pair_enter(void)
{
	uint64_t flags = read_rflags();

	interrupts_disable();
	while (atomic_swap8(&ioapic_pair_lock, 1) != 0)
		cpu_pause();

	return flags;
}

static inline void ioapic_pair_leave(uint64_t flags)
{
	__asm__ volatile("" ::: "memory");
	ioapic_pair_lock = 0;

	if (flags & RFLAGS_IF)
		interrupts_enable();
}

static uint32_t ioapic_read(unsigned reg)
{
	*(volatile uint32_t *)(io + IOAPIC_REGSEL) = reg;
	return *(volatile uint32_t *)(io + IOAPIC_WINDOW);
}

static void ioapic_write(unsigned reg, uint32_t value)
{
	*(volatile uint32_t *)(io + IOAPIC_REGSEL) = reg;
	*(volatile uint32_t *)(io + IOAPIC_WINDOW) = value;
}

int ioapic_present(void)
{
	return io != 0;
}

uint32_t ioapic_id(void)
{
	uint64_t flags;
	uint32_t id;

	if (!ioapic_present())
		return 0;
	flags = ioapic_pair_enter();
	id = (ioapic_read(IOAPIC_REG_ID) >> 24) & 0xF;
	ioapic_pair_leave(flags);
	return id;
}

uint32_t ioapic_version(void)
{
	uint64_t flags;
	uint32_t version;

	if (!ioapic_present())
		return 0;
	flags = ioapic_pair_enter();
	version = ioapic_read(IOAPIC_REG_VERSION) & 0xFF;
	ioapic_pair_leave(flags);
	return version;
}

unsigned ioapic_pin_count(void)
{
	return pins;
}

/* Which pair of registers describes a pin, by its global number. */
static unsigned redir_reg(uint32_t gsi)
{
	if (!ioapic_present() || gsi < base_gsi || gsi - base_gsi >= pins)
		panic("ioapic: asked about a pin this controller does not own");

	return IOAPIC_REG_REDIR + 2 * (gsi - base_gsi);
}

int ioapic_init(void)
{
	const struct acpi_ioapic *a = acpi_ioapic(0);
	uint64_t flags;

	if (a == 0 || a->address == 0)
		return 0;

	base_gsi = a->gsi_base;
	io = (volatile uint8_t *)(uintptr_t)pmap_map_device(a->address, 0x1000);

	/*
	 * How many pins, from the controller rather than from a constant. The
	 * count is one more than the highest entry number, and it is not
	 * always twenty-four — assuming it would program registers that do not
	 * exist on a controller with fewer, and leave pins unmasked on one
	 * with more.
	 */
	flags = ioapic_pair_enter();
	pins = ((ioapic_read(IOAPIC_REG_VERSION) >> 16) & 0xFF) + 1;

	/*
	 * Every pin masked, because the firmware does not hand over a blank
	 * controller: it has been routing interrupts for its own purposes until
	 * this instant. A pin left enabled delivers to a vector this kernel
	 * never chose, which arrives as an unclaimed interrupt — or, if the
	 * vector happens to be one we did choose, as a count that is wrong for
	 * a reason nobody would look for.
	 */
	for (unsigned i = 0; i < pins; i++)
		ioapic_write(IOAPIC_REG_REDIR + 2 * i, RTE_MASKED);
	ioapic_pair_leave(flags);

	return 1;
}

void ioapic_route(uint32_t gsi, uint8_t vector, uint32_t apic_id,
		  uint16_t flags)
{
	unsigned reg = redir_reg(gsi);
	uint32_t low = vector & RTE_VECTOR_MASK;

	low |= RTE_DELIVERY_FIXED | RTE_DEST_PHYSICAL;

	/*
	 * The electrical arrangement comes from the firmware and is not
	 * guessed. Only the explicit values change anything: the MADT's "bus
	 * default" is zero in both fields, and for ISA that default *is* edge
	 * triggered and active high, which is what the bits already say.
	 */
	if ((flags & ACPI_POLARITY_MASK) == ACPI_POLARITY_LOW)
		low |= RTE_POLARITY_LOW;
	if ((flags & ACPI_TRIGGER_MASK) == ACPI_TRIGGER_LEVEL)
		low |= RTE_TRIGGER_LEVEL;

	/*
	 * Destination first, then the low half, and the order is the whole
	 * point: unmasking lives in the low half, so writing it first would
	 * open the pin for the interval before the destination is set — and
	 * the destination it would use meanwhile is whatever the firmware
	 * left.
	 */
	uint64_t pair = ioapic_pair_enter();

	ioapic_write(reg + 1, apic_id << 24);
	ioapic_write(reg, low);
	ioapic_pair_leave(pair);
}

void ioapic_mask(uint32_t gsi)
{
	unsigned reg = redir_reg(gsi);
	uint64_t flags = ioapic_pair_enter();

	ioapic_write(reg, ioapic_read(reg) | RTE_MASKED);
	ioapic_pair_leave(flags);
}

void ioapic_unmask(uint32_t gsi)
{
	unsigned reg = redir_reg(gsi);
	uint64_t flags = ioapic_pair_enter();

	ioapic_write(reg, ioapic_read(reg) & ~RTE_MASKED);
	ioapic_pair_leave(flags);
}

int ioapic_is_masked(uint32_t gsi)
{
	unsigned reg = redir_reg(gsi);	/* can panic: not inside the hold */
	uint64_t flags = ioapic_pair_enter();
	int masked = (ioapic_read(reg) & RTE_MASKED) != 0;

	ioapic_pair_leave(flags);
	return masked;
}
