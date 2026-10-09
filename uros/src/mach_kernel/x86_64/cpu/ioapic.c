/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The I/O APIC: where device interrupts come in (#409).
 */

#include <stdint.h>

#include <cpu/acpi.h>
#include <cpu/ioapic.h>
#include <cpu/iommu.h>		/* a pin through its remapping entry, #598 */
#include <cpu/regs.h>		/* read_rflags, cpu_pause */
#include <pmap/pmap.h>
#include <sync/atomic.h>	/* the window's lock below */
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
#define IOAPIC_EOI		0x40	/* version 0x20 and up: EOI by vector */

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
static uint32_t version;	/* read once, at ioapic_init() */
static uint32_t base_gsi;

/*
 * 🔴 THE WINDOW IS TAKEN IN TURNS (#593).  A register is reached by writing
 * its number to one address and then using the other, so a second processor
 * selecting between the two makes the first read or write a DIFFERENT
 * register -- a pin's destination written into its neighbour's, a mask bit
 * read from somebody else's entry -- and nothing downstream can tell.
 *
 * <cpu/ioapic.h> used to say only the boot processor programmed pins, before
 * the others were started.  It stopped being true when user-level drivers
 * could claim a line (#457): device_intr_enable() unmasks a pin from thread
 * context on whatever processor the driver's call runs on, while the line's
 * interrupt masks it on the processor it is routed to (device_master.c), and
 * #593 moves the tick onto the HPET's pin at run time.
 *
 * The same shape as the PCI port pair's lock (cpu/pci_cfg.c) and for the same
 * two reasons: ioapic_init() runs before percpu_activate(), where the lock
 * package could not be used until the boot block (#665), and an interrupt
 * landing between the two accesses
 * on ONE processor is the same failure without any second processor at all.
 * Read-modify-write sequences below take it once around both halves.
 */
static volatile uint8_t	window_lock;

/*
 * #599: the -Y test's two ablations (ioapic_race_test.c).  WINDOW_OPEN uses
 * the window without the lock and still with interrupts masked, so what it
 * reopens is the race between processors; WINDOW_WIDEN puts that many port
 * 0x80 reads between selecting a register and using it, so a second
 * processor's select lands in the gap far more often.
 */
#ifndef	ABLATE_599_WINDOW_OPEN
#define	ABLATE_599_WINDOW_OPEN	0
#endif
#ifndef	ABLATE_599_WINDOW_WIDEN
#define	ABLATE_599_WINDOW_WIDEN	0
#endif

static uint64_t window_enter(void)
{
	uint64_t flags = read_rflags();

	interrupts_disable();
	while (!ABLATE_599_WINDOW_OPEN && atomic_swap8(&window_lock, 1) != 0)
		cpu_pause();
	return flags;
}

static void window_leave(uint64_t flags)
{
	__asm__ volatile("" ::: "memory");
	window_lock = 0;
	if (flags & RFLAGS_IF)
		interrupts_enable();
}

static void window_widen(void)
{
	for (unsigned i = 0; i < ABLATE_599_WINDOW_WIDEN; i++)
		(void) inb(0x80);
}

static uint32_t window_read(unsigned reg)
{
	*(volatile uint32_t *)(io + IOAPIC_REGSEL) = reg;
	window_widen();
	return *(volatile uint32_t *)(io + IOAPIC_WINDOW);
}

static void window_write(unsigned reg, uint32_t value)
{
	*(volatile uint32_t *)(io + IOAPIC_REGSEL) = reg;
	window_widen();
	*(volatile uint32_t *)(io + IOAPIC_WINDOW) = value;
}

static uint32_t ioapic_read(unsigned reg)
{
	uint64_t f = window_enter();
	uint32_t v = window_read(reg);

	window_leave(f);
	return v;
}

static void ioapic_write(unsigned reg, uint32_t value)
{
	uint64_t f = window_enter();

	window_write(reg, value);
	window_leave(f);
}

/* Clear and set bits of one register, as one turn at the window. */
static void ioapic_modify(unsigned reg, uint32_t clear, uint32_t set)
{
	uint64_t f = window_enter();

	window_write(reg, (window_read(reg) & ~clear) | set);
	window_leave(f);
}

int ioapic_present(void)
{
	return io != 0;
}

/*
 * The controller's id as the MADT gives it, eight bits: the number the DMAR's
 * and the IVRS's scopes name it by.  This used to be the ID register's field
 * read as four bits, which cut any id above 15 to its low nibble (#598's C16).
 */
uint32_t ioapic_id(void)
{
	const struct acpi_ioapic *a = acpi_ioapic(0);

	return ioapic_present() && a != 0 ? a->id : 0;
}

/*
 * A redirection entry's destination: the APIC id in bits 63:56, eight of them.
 * An id that does not fit is refused, where a shift would have dropped its
 * high bits and delivered to whichever processor has the rest (#598's C16).
 */
int ioapic_rte_destination(uint32_t apic_id, uint32_t *high)
{
	if (apic_id > 0xFFu)
		return 0;
	*high = apic_id << 24;
	return 1;
}

uint32_t ioapic_version(void)
{
	return ioapic_present() ? ioapic_read(IOAPIC_REG_VERSION) & 0xFF : 0;
}

unsigned ioapic_pin_count(void)
{
	return pins;
}

/*
 * Whether this controller owns a pin, by its global number: pins from
 * base_gsi, which is where the MADT says this controller's first pin sits --
 * 24, on a board whose first I/O APIC starts there (#598's C12).
 */
int ioapic_owns(uint32_t gsi)
{
	return ioapic_present() && gsi >= base_gsi && gsi - base_gsi < pins;
}

/* Which pair of registers describes a pin, by its global number. */
static unsigned redir_reg(uint32_t gsi)
{
	if (!ioapic_owns(gsi))
		panic("ioapic: asked about a pin this controller does not own");

	return IOAPIC_REG_REDIR + 2 * (gsi - base_gsi);
}

int ioapic_init(void)
{
	const struct acpi_ioapic *a = acpi_ioapic(0);

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
	pins = ((ioapic_read(IOAPIC_REG_VERSION) >> 16) & 0xFF) + 1;
	version = ioapic_read(IOAPIC_REG_VERSION) & 0xFF;

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

	return 1;
}

void ioapic_route(uint32_t gsi, uint8_t vector, uint32_t apic_id,
		  uint16_t flags)
{
	unsigned reg = redir_reg(gsi);
	int active_low = (flags & ACPI_POLARITY_MASK) == ACPI_POLARITY_LOW;
	int level = (flags & ACPI_TRIGGER_MASK) == ACPI_TRIGGER_LEVEL;
	uint32_t low = vector & RTE_VECTOR_MASK, high;
	uint64_t f;

	if (!ioapic_rte_destination(apic_id, &high))
		panic("ioapic: apic id %u does not fit a redirection entry "
		      "(#598)", apic_id);
	low |= RTE_DELIVERY_FIXED | RTE_DEST_PHYSICAL;

	/*
	 * The electrical arrangement comes from the firmware and is not
	 * guessed. Only the explicit values change anything: the MADT's "bus
	 * default" is zero in both fields, and for ISA that default *is* edge
	 * triggered and active high, which is what the bits already say.
	 */
	if (active_low)
		low |= RTE_POLARITY_LOW;
	if (level)
		low |= RTE_TRIGGER_LEVEL;

	/*
	 * #598: with interrupts remapped, the same pin in the remappable
	 * format -- its entry written first, and the pair below only selecting
	 * it.  A pin that cannot have an entry cannot be routed at all: every
	 * message it sent in the old format would be refused.
	 */
	if (iommu_interrupts_remapped()
	    && !iommu_remap_pin(gsi - base_gsi, vector, apic_id, level,
				active_low, &low, &high))
		panic("ioapic: pin %u has no remapping entry, and interrupts "
		      "are remapped (#598)", gsi - base_gsi);

	/*
	 * Destination first, then the low half, and the order is the whole
	 * point: unmasking lives in the low half, so writing it first would
	 * open the pin for the interval before the destination is set — and
	 * the destination it would use meanwhile is whatever the firmware
	 * left.  Remapped, the high half is the entry's index instead, and the
	 * same order holds for the same reason.
	 */
	f = window_enter();
	window_write(reg + 1, high);
	window_write(reg, low);
	window_leave(f);
}

void ioapic_mask(uint32_t gsi)
{
	ioapic_modify(redir_reg(gsi), 0, RTE_MASKED);
}

void ioapic_unmask(uint32_t gsi)
{
	ioapic_modify(redir_reg(gsi), RTE_MASKED, 0);
}

int ioapic_is_masked(uint32_t gsi)
{
	return (ioapic_read(redir_reg(gsi)) & RTE_MASKED) != 0;
}

int ioapic_direct_eoi(void)
{
	return ioapic_present() && version >= 0x20;
}

/* Its own register, not one behind the window: no lock to take. */
void ioapic_eoi(uint8_t vector)
{
	if (ioapic_direct_eoi())
		*(volatile uint32_t *)(io + IOAPIC_EOI) = vector;
}

/*
 * #599: the -Y test's way in (ioapic_race_test.c), and nothing else's.  A pin
 * whose low half is still exactly what ioapic_init() wrote has never been
 * routed -- ioapic_route() always writes a vector -- so the test may use it,
 * and puts the low half back when it is done.  The vector is changed by the
 * same read-modify-write that masks and unmasks a pin, which is the sequence
 * under test.
 */
int ioapic_pin_untouched(uint32_t gsi)
{
	return ioapic_owns(gsi) && ioapic_read(redir_reg(gsi)) == RTE_MASKED;
}

uint32_t ioapic_low_half(uint32_t gsi)
{
	return ioapic_read(redir_reg(gsi));
}

void ioapic_set_low_half(uint32_t gsi, uint32_t low)
{
	ioapic_write(redir_reg(gsi), low);
}

void ioapic_set_vector(uint32_t gsi, uint8_t vector)
{
	ioapic_modify(redir_reg(gsi), RTE_VECTOR_MASK, vector);
}

uint32_t ioapic_first_gsi(void)
{
	return base_gsi;
}

uint32_t ioapic_set_first_gsi(uint32_t gsi)
{
	uint32_t was = base_gsi;

	base_gsi = gsi;
	return was;
}
