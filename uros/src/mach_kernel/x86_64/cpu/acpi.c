/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * ACPI table walk, as far as finding the CPUs (#438).
 */

#include <stdint.h>

#include <boot/multiboot2.h>
#include <cpu/acpi.h>
#include <pmap/layout.h>
#include <trap/trap.h>

/*
 * The root pointer.  Its first twenty bytes are the 1.0 layout and are
 * checksummed on their own; a 2.0 pointer adds the rest and checksums the
 * whole thing again, which is why the two sums are separate below.
 */
struct acpi_rsdp {
	char     signature[8];		/* "RSD PTR " */
	uint8_t  checksum;		/* over the first 20 bytes */
	char     oem_id[6];
	uint8_t  revision;		/* 0 = 1.0, 2 = 2.0+ */
	uint32_t rsdt_address;
	/* 2.0 onward */
	uint32_t length;
	uint64_t xsdt_address;
	uint8_t  extended_checksum;	/* over `length` bytes */
	uint8_t  reserved[3];
} __attribute__((packed));

/* Every table starts with this, and every one of them is checksummed. */
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

struct acpi_madt {
	struct acpi_header header;
	uint32_t lapic_address;
	uint32_t flags;
	/* entries follow */
} __attribute__((packed));

struct madt_entry {
	uint8_t type;
	uint8_t length;
} __attribute__((packed));

#define MADT_LOCAL_APIC		0
#define MADT_IOAPIC		1
#define MADT_INTERRUPT_OVERRIDE	2
#define MADT_LOCAL_X2APIC	9

struct madt_ioapic {
	struct madt_entry entry;
	uint8_t  id;
	uint8_t  reserved;
	uint32_t address;
	uint32_t gsi_base;	/* the first global interrupt this one owns */
} __attribute__((packed));

/*
 * The entry that says an ISA interrupt is not where its number suggests.
 *
 * The legacy lines were numbered when there was one interrupt controller and
 * the number *was* the pin.  With an I/O APIC the pins are a global space
 * shared by every controller in the machine, and the firmware is free to
 * wire an ISA line to whichever pin it likes — so IRQ 0 is almost always on
 * pin 2, and reading it as pin 0 gets a line nobody is driving.
 *
 * It also carries the electrical arrangement, which the 8259 kept in a
 * separate register and which cannot be guessed: getting the polarity
 * backwards means the controller sees an interrupt whenever the device is
 * *not* asserting one.
 */
struct madt_override {
	struct madt_entry entry;
	uint8_t  bus;		/* always 0, meaning ISA */
	uint8_t  source;	/* the legacy IRQ number */
	uint32_t gsi;		/* the pin it is really on */
	uint16_t flags;		/* polarity and trigger mode */
} __attribute__((packed));

struct madt_local_apic {
	struct madt_entry entry;
	uint8_t  acpi_id;
	uint8_t  apic_id;
	uint32_t flags;
} __attribute__((packed));

struct madt_local_x2apic {
	struct madt_entry entry;
	uint16_t reserved;
	uint32_t apic_id;
	uint32_t flags;
	uint32_t acpi_id;
} __attribute__((packed));

/*
 * Bit 0 says the processor is present and usable.  Bit 1 says it is absent
 * now but could appear later — hot-plug — which is not the same thing and is
 * not something to start.
 */
#define MADT_CPU_ENABLED	0x1
#define MADT_CPU_ONLINE_CAPABLE	0x2

/*
 * A cap on what is recorded.  Not a guess about hardware: the array is
 * static because this runs before any allocator is safe to lean on, and
 * finding more processors than fit is reported rather than truncated
 * silently.
 */
#define ACPI_MAX_CPUS	64

static struct acpi_cpu cpus[ACPI_MAX_CPUS];
static unsigned ncpus;
static unsigned nusable;
static uint64_t lapic_base;

/*
 * The interrupt controllers, and the corrections to what their pin numbers
 * mean.
 *
 * Both are capped and both report overflow rather than truncating, for the
 * reason the processor array is: this runs before an allocator, and a table
 * that was quietly cut short is a machine with interrupts nobody routes.
 */
static struct acpi_ioapic ioapics[ACPI_MAX_IOAPICS];
static unsigned nioapics;

static struct acpi_irq_override overrides[ACPI_MAX_OVERRIDES];
static unsigned noverrides;

static void record_ioapic(uint8_t id, uint32_t address, uint32_t gsi_base)
{
	if (nioapics >= ACPI_MAX_IOAPICS)
		panic("acpi: more I/O APICs than there is room to record");

	ioapics[nioapics].id = id;
	ioapics[nioapics].address = address;
	ioapics[nioapics].gsi_base = gsi_base;
	nioapics++;
}

static void record_override(uint8_t source, uint32_t gsi, uint16_t flags)
{
	if (noverrides >= ACPI_MAX_OVERRIDES)
		panic("acpi: more interrupt overrides than there is room to record");

	overrides[noverrides].source = source;
	overrides[noverrides].gsi = gsi;
	overrides[noverrides].flags = flags;
	noverrides++;
}

/* Physical memory is reachable by addition; the tables are just there. */
static const void *at_phys(uint64_t pa)
{
	return (const void *)(uintptr_t)phys_to_direct(pa);
}

static int checksum_ok(const void *p, uint64_t len)
{
	const uint8_t *b = p;
	uint8_t sum = 0;

	while (len--)
		sum += *b++;

	/*
	 * The field is chosen so the bytes add to zero.  Anything else means
	 * the table is not the table it claims to be — a corrupt one is worse
	 * than an absent one, since it looks like an answer.
	 */
	return sum == 0;
}

static int signature_is(const char *field, const char *want, unsigned n)
{
	for (unsigned i = 0; i < n; i++)
		if (field[i] != want[i])
			return 0;
	return 1;
}

static void record_cpu(uint32_t apic_id, uint32_t acpi_id, uint32_t flags)
{
	if (ncpus == ACPI_MAX_CPUS)
		panic("acpi: more processors than this kernel can record");

	cpus[ncpus].apic_id = apic_id;
	cpus[ncpus].acpi_id = acpi_id;
	cpus[ncpus].usable = (flags & MADT_CPU_ENABLED) != 0;
	if (cpus[ncpus].usable)
		nusable++;
	ncpus++;
}

static void read_madt(const struct acpi_madt *madt)
{
	const uint8_t *p = (const uint8_t *)madt + sizeof(*madt);
	const uint8_t *end = (const uint8_t *)madt + madt->header.length;

	lapic_base = madt->lapic_address;

	while (p + sizeof(struct madt_entry) <= end) {
		const struct madt_entry *e = (const struct madt_entry *)p;

		/*
		 * An entry shorter than its own header would not advance the
		 * cursor.  Stop rather than spin on a malformed table.
		 */
		if (e->length < sizeof(*e))
			return;

		if (e->type == MADT_LOCAL_APIC
		    && e->length >= sizeof(struct madt_local_apic)) {
			const struct madt_local_apic *l = (const void *)p;

			record_cpu(l->apic_id, l->acpi_id, l->flags);
		} else if (e->type == MADT_LOCAL_X2APIC
			   && e->length >= sizeof(struct madt_local_x2apic)) {
			/*
			 * The wide form, for machines with more than 255
			 * processors or APIC ids that do not fit a byte.
			 */
			const struct madt_local_x2apic *l = (const void *)p;

			record_cpu(l->apic_id, l->acpi_id, l->flags);
		} else if (e->type == MADT_IOAPIC
			   && e->length >= sizeof(struct madt_ioapic)) {
			const struct madt_ioapic *io = (const void *)p;

			record_ioapic(io->id, io->address, io->gsi_base);
		} else if (e->type == MADT_INTERRUPT_OVERRIDE
			   && e->length >= sizeof(struct madt_override)) {
			const struct madt_override *o = (const void *)p;

			record_override(o->source, o->gsi, o->flags);
		}

		p += e->length;
	}
}

/* Walk a root table of `entry_bytes`-wide pointers looking for the MADT. */
static const struct acpi_madt *find_madt(uint64_t root_pa, unsigned entry_bytes)
{
	const struct acpi_header *root = at_phys(root_pa);
	unsigned count;

	if (!checksum_ok(root, root->length))
		panic("acpi: root table fails its checksum");

	count = (root->length - sizeof(*root)) / entry_bytes;

	for (unsigned i = 0; i < count; i++) {
		const uint8_t *slot = (const uint8_t *)root + sizeof(*root)
				    + i * entry_bytes;
		uint64_t pa = 0;
		const struct acpi_header *h;

		/*
		 * Copied out byte by byte rather than dereferenced: the
		 * entries are packed, so a 64-bit one in the XSDT is not
		 * guaranteed to be eight-byte aligned.
		 */
		for (unsigned b = 0; b < entry_bytes; b++)
			pa |= (uint64_t)slot[b] << (b * 8);

		h = at_phys(pa);
		if (!signature_is(h->signature, "APIC", 4))
			continue;

		if (!checksum_ok(h, h->length))
			panic("acpi: the MADT fails its checksum");

		return (const struct acpi_madt *)h;
	}

	return 0;
}

unsigned acpi_find_cpus(uint32_t mb2_info_pa)
{
	const struct mb2_tag *tag;
	const struct acpi_rsdp *rsdp;
	const struct acpi_madt *madt;

	/*
	 * Prefer the 2.0 pointer when the loader offers both: it carries the
	 * XSDT, whose entries are 64-bit, and on a machine with tables above
	 * 4 GiB the 1.0 pointer cannot describe them at all.
	 */
	tag = mb2_find_tag(mb2_info_pa, MB2_TAG_ACPI_NEW);
	if (tag == 0)
		tag = mb2_find_tag(mb2_info_pa, MB2_TAG_ACPI_OLD);
	if (tag == 0)
		return 0;

	rsdp = (const struct acpi_rsdp *)((const uint8_t *)tag
					  + sizeof(struct mb2_tag));

	if (!signature_is(rsdp->signature, "RSD PTR ", 8))
		panic("acpi: the loader's root pointer is not one");

	/* The 1.0 fields are checksummed on their own, in both revisions. */
	if (!checksum_ok(rsdp, 20))
		panic("acpi: root pointer fails its checksum");

	if (rsdp->revision >= 2 && checksum_ok(rsdp, rsdp->length)
	    && rsdp->xsdt_address != 0)
		madt = find_madt(rsdp->xsdt_address, 8);
	else
		madt = find_madt(rsdp->rsdt_address, 4);

	if (madt == 0)
		return 0;

	read_madt(madt);
	return ncpus;
}

unsigned acpi_cpu_count(void)
{
	return ncpus;
}

unsigned acpi_usable_cpu_count(void)
{
	return nusable;
}

const struct acpi_cpu *acpi_cpu(unsigned index)
{
	return index < ncpus ? &cpus[index] : 0;
}

uint64_t acpi_lapic_base(void)
{
	return lapic_base;
}

unsigned acpi_ioapic_count(void)
{
	return nioapics;
}

const struct acpi_ioapic *acpi_ioapic(unsigned index)
{
	return index < nioapics ? &ioapics[index] : 0;
}

unsigned acpi_override_count(void)
{
	return noverrides;
}

const struct acpi_irq_override *acpi_override(unsigned index)
{
	return index < noverrides ? &overrides[index] : 0;
}

/*
 * The identity unless the firmware says otherwise, which is what an override
 * is. Written as a search rather than a table so that a machine with no
 * overrides at all needs no special case: the loop finds nothing and the
 * answer is the number that was asked about.
 */
uint32_t acpi_irq_to_gsi(uint8_t irq)
{
	for (unsigned i = 0; i < noverrides; i++)
		if (overrides[i].source == irq)
			return overrides[i].gsi;

	return irq;
}

uint16_t acpi_irq_flags(uint8_t irq)
{
	for (unsigned i = 0; i < noverrides; i++)
		if (overrides[i].source == irq)
			return overrides[i].flags;

	/* No override: the bus default, which for ISA is high and edge. */
	return ACPI_POLARITY_BUS | ACPI_TRIGGER_BUS;
}
