/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * ACPI table walk, as far as finding the CPUs (#438).
 *
 * The kernel has to know which processors exist before it can start any of
 * them, and the firmware's answer lives in the MADT.  i386 could also ask
 * the older MP tables (MPS 1.x, #300's `MP_V1_1`); x86-64 does not need to,
 * because ACPI is not optional on any machine that runs in long mode.  One
 * source of truth is worth having where two were tolerated.
 *
 * This is not a general ACPI implementation and does not pretend to be: it
 * walks the tables to find one, and the interpreter that AML would need is
 * a different problem entirely.
 */

#ifndef _X86_64_CPU_ACPI_H_
#define _X86_64_CPU_ACPI_H_

#include <stdint.h>

/*
 * Every ACPI table starts with this, and every one of them is checksummed.
 *
 * Out here rather than in acpi.c because a table reader that lives elsewhere
 * -- the IOMMU's, #432 -- needs the length to know where the table ends, and
 * the length is the only thing that says so.
 */
struct acpi_header {
	char     signature[4];
	uint32_t length;
	uint8_t  revision;
	uint8_t  checksum;
	char     oem_id[6];
	char     oem_table_id[8];
	uint32_t oem_revision;
	uint32_t creator_id;
	uint32_t creator_revision;
} __attribute__((packed));

/*
 * One table by signature, or zero if this machine has none of that kind.
 *
 * 🔑 The walk that finds it is the one #457 generalised out of find_madt() --
 * checksum the root, step its packed entries, follow each to a header -- and
 * the only thing that was ever specific to a table was its four characters.
 * Exposing it is what keeps a second reader from making the 1.0-versus-2.0
 * root-pointer decision a second time, which is a decision there is no reason
 * to have two copies of and every reason to have one.
 *
 * ⚠️ Answers zero before acpi_find_cpus() has run: that is what remembers the
 * root.  A table that fails its checksum stops the machine rather than being
 * skipped, for the reason the walk has always done so -- a description of the
 * hardware that does not add up is not a reason to look for a better one.
 */
const struct acpi_header *acpi_find_table(const char *signature);

/* What one processor's MADT entry says about it. */
struct acpi_cpu {
	uint32_t apic_id;
	uint32_t acpi_id;
	int      usable;	/* the firmware says this one may be started */
};

/*
 * Find the MADT through the root pointer the bootloader handed over, and
 * record what it says.  Returns the number of processors found, or zero if
 * there is no usable ACPI — which is different from "one processor", and is
 * why it is not reported as one.
 *
 * Every table is checksummed before it is believed.  A table that fails is
 * not partially trusted: the walk stops there.
 */
unsigned acpi_find_cpus(uint32_t mb2_info_pa);

/* How many the walk found, and the details of each. */
unsigned acpi_cpu_count(void);
const struct acpi_cpu *acpi_cpu(unsigned index);

/* How many of those the firmware says are startable. */
unsigned acpi_usable_cpu_count(void);

/* Physical address of the local APIC, from the MADT.  Zero if unknown. */
uint64_t acpi_lapic_base(void);

/* ------------------------------------------------------------------ */
/*  Where device interrupts come from (#409)                            */
/* ------------------------------------------------------------------ */
/*
 * One I/O APIC, and the slice of the interrupt space it owns.
 *
 * A machine may have several, and they do not overlap: each is given a base
 * in one flat *global system interrupt* space and owns as many pins upward
 * from it as it has. So a pin number alone does not name an interrupt — it
 * has to be found in the controller whose range contains it, which is why
 * the base is recorded and not only the address.
 */
struct acpi_ioapic {
	uint8_t  id;
	uint32_t address;
	uint32_t gsi_base;
};

/*
 * A legacy line that is not where its number says.
 *
 * The ISA interrupts were numbered when there was one controller and the
 * number *was* the pin. In the global space the firmware is free to wire
 * them anywhere, and it usually does: IRQ 0 is almost always on pin 2.
 * Programming pin 0 for it gets a pin nobody drives, and the failure is
 * silence — a timer that never fires, with nothing to read.
 *
 * The flags carry the electrical arrangement, which cannot be guessed. The
 * 8259 kept it in a separate register; here it travels with the override.
 * Polarity backwards means the controller sees an interrupt whenever the
 * device is *not* asserting one.
 */
struct acpi_irq_override {
	uint8_t  source;	/* the legacy IRQ number  */
	uint32_t gsi;		/* the pin it is truly on */
	uint16_t flags;
};

#define ACPI_MAX_IOAPICS	8
#define ACPI_MAX_OVERRIDES	16

/*
 * The flag encoding. Zero in either field means "whatever the bus does by
 * default", which for ISA is active high and edge triggered — and that
 * default is why an absent override is not missing information.
 */
#define ACPI_POLARITY_MASK	0x3
#define ACPI_POLARITY_BUS	0x0
#define ACPI_POLARITY_HIGH	0x1
#define ACPI_POLARITY_LOW	0x3
#define ACPI_TRIGGER_MASK	0xC
#define ACPI_TRIGGER_BUS	0x0
#define ACPI_TRIGGER_EDGE	0x4
#define ACPI_TRIGGER_LEVEL	0xC

unsigned acpi_ioapic_count(void);
const struct acpi_ioapic *acpi_ioapic(unsigned index);

unsigned acpi_override_count(void);
const struct acpi_irq_override *acpi_override(unsigned index);

/*
 * Which pin a legacy ISA interrupt is really on, and how it is wired.
 *
 * The identity and the bus default unless an override says otherwise, which
 * is exactly what the specification means by an override — so a caller that
 * uses these two never has to ask whether one was present.
 */
uint32_t acpi_irq_to_gsi(uint8_t irq);
uint16_t acpi_irq_flags(uint8_t irq);

/* ------------------------------------------------------------------ */
/*  Where PCI configuration space is (#457)                             */
/* ------------------------------------------------------------------ */
/*
 * The physical base of the memory-mapped configuration space for one bus, or
 * zero if this machine has no MCFG or no segment group covering it.
 *
 * A configuration register of a function on that bus is then at
 *
 *	base + (device << 15) + (function << 12) + offset
 *
 * the bus part having been folded into the base already, because a segment
 * does not have to start at bus zero.
 *
 * ⚠️ Zero is an answer, not a failure: a machine without an MCFG has a
 * configuration space that is reachable only through 0xCF8/0xCFC, and the
 * caller is the one that has to decide what to do about that.  Nothing above
 * four gigabytes can be described that way, so a caller that needs it must
 * check rather than assume.
 */
uint64_t acpi_ecam_base(uint16_t segment, uint8_t bus);

#endif	/* _X86_64_CPU_ACPI_H_ */
