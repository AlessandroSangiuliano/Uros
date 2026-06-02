/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 */

/*
 * mp_table.c — Intel MultiProcessor Specification 1.4 table parser.
 *
 * Replaces the get_ncpus()/validate_cpus()/mp_v1_1_init() stubs that
 * mp_stub.c installed in Increment 2.  Walks the MP Floating Pointer
 * Structure (FPS) and the MP Configuration Table to enumerate logical
 * CPUs and the LAPIC base address, populating machine_slot[].
 *
 * Reference: Intel MultiProcessor Specification 1.4 (1997), chapter 4.
 *
 * Scope for #300 Increment 3: enumerate processors and capture the LAPIC
 * physical address.  I/O APIC routing and per-CPU interrupt programming
 * land in #302.  Actually firing INIT/SIPI to wake the APs lands in
 * Increment 4.
 */

#include <types.h>
#include <mach/machine.h>
#include <kern/misc_protos.h>
#include <i386/AT386/mp/mp.h>
#include <i386/apic.h>
#include <i386/io_map_entries.h>		/* io_map() */
#include <vm/vm_map.h>			/* kernel_map, VM_MAP_NULL */

/*
 * Floating Pointer Structure — 16 bytes, byte-packed on the wire.
 */
struct mp_fps {
	unsigned char		signature[4];	/* "_MP_" */
	unsigned int	config_phys;	/* physical address of mp_config */
	unsigned char		length;		/* in 16-byte paragraphs */
	unsigned char		spec_rev;	/* 0x01 or 0x04 */
	unsigned char		checksum;
	unsigned char		feature1;	/* 0 -> use config; 1-7 -> default cfg */
	unsigned char		feature2;	/* bit 7 -> IMCR present */
	unsigned char		feature[3];	/* reserved */
} __attribute__((packed));

/*
 * MP Configuration Table header — 44 bytes.
 */
struct mp_config {
	unsigned char		signature[4];	/* "PCMP" */
	unsigned short	base_length;	/* length including header */
	unsigned char		spec_rev;
	unsigned char		checksum;
	unsigned char		oem_id[8];
	unsigned char		product_id[12];
	unsigned int	oem_table_phys;
	unsigned short	oem_table_length;
	unsigned short	entry_count;
	unsigned int	lapic_phys;	/* local APIC base, normally 0xFEE00000 */
	unsigned short	ext_length;
	unsigned char		ext_checksum;
	unsigned char		reserved;
} __attribute__((packed));

/*
 * Entry types in the configuration table.
 */
#define MP_ENTRY_PROCESSOR	0
#define MP_ENTRY_BUS		1
#define MP_ENTRY_IOAPIC		2
#define MP_ENTRY_IO_INTR	3
#define MP_ENTRY_LOCAL_INTR	4

#define MP_CPU_FLAG_ENABLED	0x01
#define MP_CPU_FLAG_BSP		0x02

/* Per-spec sizes — used to walk past entries we don't decode. */
#define MP_ENTRY_SIZE_PROCESSOR	20
#define MP_ENTRY_SIZE_BUS	8
#define MP_ENTRY_SIZE_IOAPIC	8
#define MP_ENTRY_SIZE_IO_INTR	8
#define MP_ENTRY_SIZE_LOCAL_INTR 8

struct mp_processor_entry {
	unsigned char		type;		/* MP_ENTRY_PROCESSOR */
	unsigned char		lapic_id;
	unsigned char		lapic_version;
	unsigned char		cpu_flags;
	unsigned int	cpu_signature;	/* family/model/stepping */
	unsigned int	feature_flags;
	unsigned int	reserved[2];
} __attribute__((packed));

struct mp_ioapic_entry {
	unsigned char		type;		/* MP_ENTRY_IOAPIC */
	unsigned char		ioapic_id;
	unsigned char		ioapic_version;
	unsigned char		ioapic_flags;	/* bit 0 = enabled */
	unsigned int	ioapic_phys;
} __attribute__((packed));

/*
 * ACPI MADT structures.  Modern SeaBIOS/QEMU (and most UEFI machines)
 * advertise only the BSP through the MPS 1.4 table and expect the OS to
 * walk ACPI MADT to find the APs.  We parse the minimum needed to
 * enumerate processors — no AML interpreter required.
 */
struct acpi_rsdp_v1 {
	char		signature[8];	/* "RSD PTR " */
	unsigned char	checksum;	/* first 20 bytes must sum to 0 */
	char		oem_id[6];
	unsigned char	revision;	/* 0 = ACPI 1.0, 2 = ACPI 2.0+ */
	unsigned int	rsdt_phys;
} __attribute__((packed));

struct acpi_sdt_header {
	char		signature[4];
	unsigned int	length;
	unsigned char	revision;
	unsigned char	checksum;
	char		oem_id[6];
	char		oem_table_id[8];
	unsigned int	oem_revision;
	unsigned int	creator_id;
	unsigned int	creator_revision;
} __attribute__((packed));

struct acpi_madt {
	struct acpi_sdt_header	header;
	unsigned int		lapic_phys;
	unsigned int		flags;	/* bit 0 = 8259 present */
	/* followed by variable-length entries */
} __attribute__((packed));

#define MADT_ENTRY_LAPIC	0
#define MADT_ENTRY_IOAPIC	1
#define MADT_ENTRY_LAPIC_FLAG_ENABLED	0x01

struct madt_lapic_entry {
	unsigned char	type;		/* MADT_ENTRY_LAPIC */
	unsigned char	length;		/* 8 */
	unsigned char	proc_id;
	unsigned char	lapic_id;
	unsigned int	flags;
} __attribute__((packed));

struct madt_ioapic_entry {
	unsigned char	type;		/* MADT_ENTRY_IOAPIC */
	unsigned char	length;		/* 12 */
	unsigned char	ioapic_id;
	unsigned char	reserved;
	unsigned int	ioapic_phys;
	unsigned int	gsi_base;
} __attribute__((packed));

/*
 * Cached state — populated on first call to mp_v1_1_init() and read back
 * by get_ncpus()/validate_cpus().
 */
static boolean_t	mp_phase1_done = FALSE;	/* low-mem search done */
static boolean_t	mp_phase2_done = FALSE;	/* high-mem MADT + io_map done */
static int		mp_cpu_count;
static unsigned char	mp_cpu_lapic_id[NCPUS];
static unsigned char	mp_bsp_lapic_id;
static unsigned int	mp_lapic_phys;
static unsigned int	mp_rsdt_phys;		/* found in phase 1, parsed in phase 2 */
static unsigned int	mp_ioapic_phys[4];	/* up to 4 I/O APICs */
static int		mp_ioapic_count;

/* Externals that the rest of the kernel exports — set up here once we
 * know where the local APIC actually sits in memory. */
extern vm_offset_t	lapic_start;
extern int		lapic_id;

/*
 * Public accessors used by the AP boot path in mp.c.
 */
unsigned char
mp_cpu_lapic_id_get(int slot)
{
	if (slot < 0 || slot >= mp_cpu_count)
		return 0xFF;
	return mp_cpu_lapic_id[slot];
}

unsigned char
mp_bsp_lapic_id_get(void)
{
	return mp_bsp_lapic_id;
}

/*
 * I/O APIC accessors used by ioapic.c (#311) to find I/O APIC #0's MMIO
 * base.  Populated by the MPS / ACPI MADT walk above.
 */
unsigned int
mp_ioapic_phys_get(int idx)
{
	if (idx < 0 || idx >= mp_ioapic_count)
		return 0;
	return mp_ioapic_phys[idx];
}

int
mp_ioapic_count_get(void)
{
	return mp_ioapic_count;
}

/*
 * Physical→kernel-virtual translation.
 *
 * - MP_PHYS_TO_KV: identity-mapped low memory (RSDP/FPS searches), reach
 *   via VM_MIN_KERNEL_ADDRESS offset.  Only safe for phys < ~16 MiB which
 *   is where start.S sets up its initial kernel mapping.
 * - acpi_map: arbitrary phys → kernel-virt via io_map().  ACPI tables on
 *   QEMU live just below RAM top (e.g., 0x1FFE0000 with -m 512M), well
 *   above the bootstrap mapping; io_map allocates a kernel-virt window
 *   and pmap_map_bd's the physical page in.  Returns a page-aligned
 *   virtual base plus the in-page offset already applied.
 */
#define MP_PHYS_TO_KV(pa)	((vm_offset_t)(pa) + 0xC0000000U)

static vm_offset_t
acpi_map(unsigned int pa, vm_size_t len)
{
	unsigned int page_pa  = pa & ~0xFFFu;
	unsigned int page_off = pa & 0xFFFu;
	vm_size_t    mapped   = (vm_size_t)((page_off + len + 0xFFFu) &
					    ~0xFFFu);

	return io_map(page_pa, mapped) + page_off;
}

static unsigned char
mp_checksum(const void *buf, vm_size_t len)
{
	const unsigned char *p = (const unsigned char *)buf;
	unsigned char sum = 0;
	vm_size_t i;

	for (i = 0; i < len; i++)
		sum += p[i];
	return sum;
}

/*
 * Look for the "_MP_" signature in [start, end) at 16-byte alignment.
 * Returns the FPS virtual address on success, 0 otherwise.
 */
static struct mp_fps *
mp_find_fps_in_range(vm_offset_t start, vm_offset_t end)
{
	vm_offset_t p;
	struct mp_fps *fps;

	for (p = start; p + sizeof(struct mp_fps) <= end; p += 16) {
		fps = (struct mp_fps *)p;
		if (fps->signature[0] != '_' || fps->signature[1] != 'M' ||
		    fps->signature[2] != 'P' || fps->signature[3] != '_')
			continue;
		if (fps->length == 0)
			continue;
		if (mp_checksum(fps, (vm_size_t)fps->length * 16) != 0)
			continue;
		return fps;
	}
	return (struct mp_fps *)0;
}

/*
 * Search the three regions called out by MPS 1.4 §4.1:
 *   1) First 1 KiB of the EBDA (segment from BDA word at phys 0x40E).
 *   2) Last 1 KiB of base memory (segment from BDA word at phys 0x413).
 *   3) BIOS ROM, 0xF0000-0xFFFFF.
 *
 * All three regions are below 1 MiB and reachable through the bootstrap
 * identity mapping that start.S sets up — this function is intended to
 * run *before* pmap_bootstrap.
 */
static struct mp_fps *
mp_find_fps(void)
{
	struct mp_fps *fps;
	unsigned short ebda_seg;
	unsigned short base_kb;
	vm_offset_t ebda_pa;
	vm_offset_t basemem_top_pa;

	/* 1) EBDA */
	ebda_seg = *(unsigned short *)MP_PHYS_TO_KV(0x40E);
	if (ebda_seg) {
		ebda_pa = (vm_offset_t)ebda_seg << 4;
		fps = mp_find_fps_in_range(MP_PHYS_TO_KV(ebda_pa),
					   MP_PHYS_TO_KV(ebda_pa + 1024));
		if (fps)
			return fps;
	}

	/* 2) Last KiB of conventional memory.  BDA word at 0x413 holds
	 * base RAM size in KiB. */
	base_kb = *(unsigned short *)MP_PHYS_TO_KV(0x413);
	if (base_kb) {
		basemem_top_pa = ((vm_offset_t)base_kb << 10) - 1024;
		fps = mp_find_fps_in_range(MP_PHYS_TO_KV(basemem_top_pa),
					   MP_PHYS_TO_KV(basemem_top_pa + 1024));
		if (fps)
			return fps;
	}

	/* 3) BIOS ROM. */
	fps = mp_find_fps_in_range(MP_PHYS_TO_KV(0xF0000),
				   MP_PHYS_TO_KV(0x100000));
	return fps;
}

/*
 * Walk the configuration table starting at `config`, picking up CPUs and
 * I/O APIC addresses.  Returns TRUE on a valid table.
 */
static boolean_t
mp_parse_config(struct mp_config *config)
{
	const unsigned char *p;
	const unsigned char *end;
	int i;

	if (config->signature[0] != 'P' || config->signature[1] != 'C' ||
	    config->signature[2] != 'M' || config->signature[3] != 'P')
		return FALSE;
	if (mp_checksum(config, config->base_length) != 0)
		return FALSE;

	mp_lapic_phys = config->lapic_phys;

	p = (const unsigned char *)(config + 1);
	end = (const unsigned char *)config + config->base_length;

	for (i = 0; i < config->entry_count && p < end; i++) {
		switch (*p) {
		case MP_ENTRY_PROCESSOR: {
			const struct mp_processor_entry *cpu =
			    (const struct mp_processor_entry *)p;
			if (cpu->cpu_flags & MP_CPU_FLAG_ENABLED) {
				if (mp_cpu_count < NCPUS) {
					mp_cpu_lapic_id[mp_cpu_count] =
					    cpu->lapic_id;
					mp_cpu_count++;
				}
				if (cpu->cpu_flags & MP_CPU_FLAG_BSP)
					mp_bsp_lapic_id = cpu->lapic_id;
			}
			p += MP_ENTRY_SIZE_PROCESSOR;
			break;
		}
		case MP_ENTRY_BUS:
			p += MP_ENTRY_SIZE_BUS;
			break;
		case MP_ENTRY_IOAPIC: {
			const struct mp_ioapic_entry *io =
			    (const struct mp_ioapic_entry *)p;
			if ((io->ioapic_flags & 0x01) &&
			    mp_ioapic_count <
				(int)(sizeof(mp_ioapic_phys) /
				      sizeof(mp_ioapic_phys[0]))) {
				mp_ioapic_phys[mp_ioapic_count++] =
				    io->ioapic_phys;
			}
			p += MP_ENTRY_SIZE_IOAPIC;
			break;
		}
		case MP_ENTRY_IO_INTR:
			p += MP_ENTRY_SIZE_IO_INTR;
			break;
		case MP_ENTRY_LOCAL_INTR:
			p += MP_ENTRY_SIZE_LOCAL_INTR;
			break;
		default:
			/* Unknown entry — bail rather than risk
			 * miscounting the rest. */
			return mp_cpu_count > 0;
		}
	}

	return mp_cpu_count > 0;
}

/*
 * Look for the "RSD PTR " RSDP signature in [start, end) on 16-byte
 * alignment.  Returns a virtual pointer to the RSDP on success.
 */
static struct acpi_rsdp_v1 *
acpi_find_rsdp_in_range(vm_offset_t start, vm_offset_t end)
{
	vm_offset_t p;
	struct acpi_rsdp_v1 *rsdp;

	for (p = start; p + sizeof(struct acpi_rsdp_v1) <= end; p += 16) {
		rsdp = (struct acpi_rsdp_v1 *)p;
		if (rsdp->signature[0] != 'R' || rsdp->signature[1] != 'S' ||
		    rsdp->signature[2] != 'D' || rsdp->signature[3] != ' ' ||
		    rsdp->signature[4] != 'P' || rsdp->signature[5] != 'T' ||
		    rsdp->signature[6] != 'R' || rsdp->signature[7] != ' ')
			continue;
		if (mp_checksum(rsdp, 20) != 0)
			continue;
		return rsdp;
	}
	return (struct acpi_rsdp_v1 *)0;
}

static struct acpi_rsdp_v1 *
acpi_find_rsdp(void)
{
	struct acpi_rsdp_v1 *rsdp;
	unsigned short ebda_seg;
	vm_offset_t ebda_pa;

	ebda_seg = *(unsigned short *)MP_PHYS_TO_KV(0x40E);

	if (ebda_seg) {
		ebda_pa = (vm_offset_t)ebda_seg << 4;
		rsdp = acpi_find_rsdp_in_range(MP_PHYS_TO_KV(ebda_pa),
					       MP_PHYS_TO_KV(ebda_pa + 1024));
		if (rsdp)
			return rsdp;
	}

	return acpi_find_rsdp_in_range(MP_PHYS_TO_KV(0xE0000),
				       MP_PHYS_TO_KV(0x100000));
}

/*
 * Walk a MADT, picking up processor LAPIC IDs and I/O APIC addresses.
 * Returns TRUE on a valid MADT.
 */
static boolean_t
acpi_parse_madt(struct acpi_madt *madt)
{
	const unsigned char *p;
	const unsigned char *end;

	if (mp_checksum(madt, madt->header.length) != 0)
		return FALSE;

	mp_lapic_phys = madt->lapic_phys;

	p = (const unsigned char *)(madt + 1);
	end = (const unsigned char *)madt + madt->header.length;

	while (p + 2 <= end) {
		unsigned char etype = p[0];
		unsigned char elen  = p[1];

		if (elen < 2 || p + elen > end)
			break;

		switch (etype) {
		case MADT_ENTRY_LAPIC: {
			const struct madt_lapic_entry *lapic =
			    (const struct madt_lapic_entry *)p;
			if ((lapic->flags & MADT_ENTRY_LAPIC_FLAG_ENABLED) &&
			    mp_cpu_count < NCPUS) {
				mp_cpu_lapic_id[mp_cpu_count++] =
				    lapic->lapic_id;
			}
			break;
		}
		case MADT_ENTRY_IOAPIC: {
			const struct madt_ioapic_entry *io =
			    (const struct madt_ioapic_entry *)p;
			if (mp_ioapic_count <
			    (int)(sizeof(mp_ioapic_phys) /
				  sizeof(mp_ioapic_phys[0]))) {
				mp_ioapic_phys[mp_ioapic_count++] =
				    io->ioapic_phys;
			}
			break;
		}
		default:
			/* Unknown entry — skip past via its length. */
			break;
		}
		p += elen;
	}

	if (mp_cpu_count > 0) {
		/* ACPI doesn't dedicate a BSP flag in the LAPIC entry; the
		 * BSP is the CPU that is currently executing this code.
		 * cpu_number() will return 0 until we map the LAPIC, so the
		 * first LAPIC ID listed is by convention the BSP on every
		 * platform we have seen — but to be safe we read it directly
		 * from the running LAPIC once it is mapped.  For now record
		 * the first id; init() fixes it up below if needed. */
		mp_bsp_lapic_id = mp_cpu_lapic_id[0];
	}
	return mp_cpu_count > 0;
}

/*
 * Walk an RSDT (or XSDT) looking for the "APIC" subtable, then parse it.
 * Returns TRUE if a usable MADT is found and at least one CPU emerged.
 */
static boolean_t
acpi_walk_rsdt(struct acpi_sdt_header *rsdt, boolean_t use_xsdt)
{
	unsigned int entries;
	unsigned int i;
	struct acpi_sdt_header *sub;
	unsigned int sub_phys;

	if (mp_checksum(rsdt, rsdt->length) != 0)
		return FALSE;

	if (use_xsdt) {
		const unsigned int *xptr64_lo;
		entries = (rsdt->length - sizeof(*rsdt)) / 8;
		xptr64_lo = (const unsigned int *)(rsdt + 1);
		for (i = 0; i < entries; i++) {
			/* On i386 we only consume the low 32 bits of each
			 * XSDT pointer; ACPI tables in QEMU all live below
			 * 4 GiB so the high half is 0. */
			sub_phys = xptr64_lo[i * 2];
			sub = (struct acpi_sdt_header *)
			    acpi_map(sub_phys, 0x1000);
			if (sub->signature[0] == 'A' &&
			    sub->signature[1] == 'P' &&
			    sub->signature[2] == 'I' &&
			    sub->signature[3] == 'C') {
				if (sub->length > 0x1000)
					sub = (struct acpi_sdt_header *)
					    acpi_map(sub_phys, sub->length);
				return acpi_parse_madt(
				    (struct acpi_madt *)sub);
			}
		}
	} else {
		const unsigned int *ptr32;
		entries = (rsdt->length - sizeof(*rsdt)) / 4;
		ptr32 = (const unsigned int *)(rsdt + 1);
		for (i = 0; i < entries; i++) {
			sub_phys = ptr32[i];
			sub = (struct acpi_sdt_header *)
			    acpi_map(sub_phys, 0x1000);
			if (sub->signature[0] == 'A' &&
			    sub->signature[1] == 'P' &&
			    sub->signature[2] == 'I' &&
			    sub->signature[3] == 'C') {
				if (sub->length > 0x1000)
					sub = (struct acpi_sdt_header *)
					    acpi_map(sub_phys, sub->length);
				return acpi_parse_madt(
				    (struct acpi_madt *)sub);
			}
		}
	}
	return FALSE;
}

/*
 * Phase 1: just remember the RSDT physical address from the RSDP.
 * Returns TRUE if an RSDP was located.  No io_map calls — RSDT lives in
 * high memory and we follow the pointer later in phase 2.
 */
static boolean_t
mp_acpi_locate(void)
{
	struct acpi_rsdp_v1 *rsdp;

	rsdp = acpi_find_rsdp();
	if (!rsdp)
		return FALSE;

	mp_rsdt_phys = rsdp->rsdt_phys;
	printf("mp_table: ACPI RSDP at %p, rev %u, rsdt_phys=0x%x\n",
	       (void *)rsdp, (unsigned)rsdp->revision, mp_rsdt_phys);
	return TRUE;
}

/*
 * Phase 2: io_map the RSDT, find the MADT, parse it.  Called from the
 * second mp_v1_1_init() once kernel_pmap is alive.
 */
static boolean_t
mp_acpi_parse(void)
{
	struct acpi_sdt_header *rsdt;

	if (mp_rsdt_phys == 0)
		return FALSE;

	rsdt = (struct acpi_sdt_header *)acpi_map(mp_rsdt_phys, 0x1000);
	if (rsdt->signature[0] != 'R' || rsdt->signature[1] != 'S' ||
	    rsdt->signature[2] != 'D' || rsdt->signature[3] != 'T') {
		printf("mp_table: bad RSDT signature at 0x%x\n",
		       mp_rsdt_phys);
		return FALSE;
	}

	if (rsdt->length > 0x1000)
		rsdt = (struct acpi_sdt_header *)
		    acpi_map(mp_rsdt_phys, rsdt->length);

	return acpi_walk_rsdt(rsdt, FALSE);
}

/*
 * Try the legacy MPS 1.4 path.  Returns TRUE if a valid table was found
 * and populated the cached state.
 */
static boolean_t
mp_try_mps(void)
{
	struct mp_fps *fps;
	struct mp_config *config;

	fps = mp_find_fps();
	if (!fps)
		return FALSE;

	printf("mp_table: FPS at %p, spec %d.%d%s\n",
	       (void *)fps, fps->spec_rev >> 4 ? fps->spec_rev >> 4 : 1,
	       fps->spec_rev & 0xF,
	       (fps->feature2 & 0x80) ? ", IMCR" : "");

	if (fps->config_phys == 0) {
		/* Default configuration: feature1 picks one of 7 layouts,
		 * each of which always describes 2 CPUs (MPS 1.4 ch.5). */
		mp_cpu_count = 2;
		mp_lapic_phys = LAPIC_START;
		mp_bsp_lapic_id = 0;
		mp_cpu_lapic_id[0] = 0;
		mp_cpu_lapic_id[1] = 1;
		printf("mp_table: default config %u, assuming 2 CPUs\n",
		       fps->feature1);
		return TRUE;
	}

	config = (struct mp_config *)MP_PHYS_TO_KV(fps->config_phys);
	if (!mp_parse_config(config)) {
		printf("mp_table: PCMP at 0x%x invalid\n",
		       fps->config_phys);
		return FALSE;
	}

	printf("mp_table: %d CPU(s) via MPS, BSP lapic_id=%d, "
	       "lapic@0x%x, %d IOAPIC(s)\n",
	       mp_cpu_count, mp_bsp_lapic_id, mp_lapic_phys,
	       mp_ioapic_count);
	return TRUE;
}

/*
 * mp_v1_1_init() — entry point called from model_dep.c during platform
 * bring-up.  Tries ACPI MADT first (modern firmware), falls back to MPS
 * 1.4 (BIOS-era).  Caches the result for get_ncpus()/validate_cpus().
 * Idempotent.
 *
 * Note we override the stub installed by mp_stub.c when this file is
 * compiled in.
 */
extern vm_map_t kernel_map;	/* from vm/vm_map.h */

void
mp_v1_1_init(void)
{
	/*
	 * Phase 1 — search BIOS-area low memory for the FPS / RSDP.  Runs
	 * before pmap_bootstrap (from i386_init via mp_probe_cpus → lazy
	 * get_ncpus), so we cannot io_map anything.  The MPS config table
	 * (when present) lives in low memory and is parsed here.  The ACPI
	 * RSDT lives in high memory; we just remember its phys address and
	 * defer the walk to phase 2.
	 */
	if (!mp_phase1_done) {
		mp_phase1_done = TRUE;
		mp_cpu_count = 0;
		mp_ioapic_count = 0;
		mp_rsdt_phys = 0;

		if (mp_acpi_locate()) {
			/* ACPI found.  The real CPU list comes from MADT in
			 * phase 2; report a single CPU here so the early
			 * bookkeeping (mp_probe_cpus → validate_cpus →
			 * interrupt_stack_alloc → mp_desc_init) runs as UP.
			 * Phase 2 discovers the real APs and brings them up
			 * via start_other_cpus(). */
			mp_cpu_count = 1;
			mp_lapic_phys = LAPIC_START;
			mp_bsp_lapic_id = 0;
			printf("mp_table: phase 1 deferred to ACPI MADT\n");
		} else if (mp_try_mps()) {
			printf("mp_table: phase 1 used MPS, %d CPU(s)\n",
			       mp_cpu_count);
		} else {
			printf("mp_table: phase 1 found neither, UP fallback\n");
			mp_cpu_count = 1;
			mp_bsp_lapic_id = 0;
		}
	}

	/*
	 * Phase 2 — kernel_map is alive, so we can io_map() arbitrary
	 * physical addresses.  Map the LAPIC and, if we deferred ACPI,
	 * walk the RSDT/MADT now to obtain the real CPU list.
	 */
	if (!mp_phase2_done && kernel_map != VM_MAP_NULL) {
		mp_phase2_done = TRUE;

		if (mp_rsdt_phys != 0) {
			/* Wipe the conservative phase-1 cpu list before MADT
			 * rewrites it with the real lapic IDs. */
			mp_cpu_count = 0;
			if (mp_acpi_parse()) {
				printf("mp_table: %d CPU(s) via ACPI MADT, "
				       "lapic@0x%x, %d IOAPIC(s)\n",
				       mp_cpu_count, mp_lapic_phys,
				       mp_ioapic_count);
			} else {
				printf("mp_table: ACPI MADT parse failed, "
				       "UP fallback\n");
				mp_cpu_count = 1;
				mp_bsp_lapic_id = 0;
			}
		}

		/* #302: resync the kernel-wide CPU bookkeeping now that we
		 * have the real LAPIC count.  Phase 1 only marked slot 0
		 * (ACPI MADT defers to phase 2), so start_kernel_threads
		 * would otherwise create just one idle thread and the AP
		 * would crash in cpu_launch_first_thread(THREAD_NULL). */
		{
			extern int real_ncpus;
			extern void validate_cpus(int);
			real_ncpus = mp_cpu_count;
			validate_cpus(mp_cpu_count);
		}

		lapic_start = io_map(mp_lapic_phys, LAPIC_SIZE);
		lapic_id    = (int)(lapic_start + LAPIC_ID);
	}
}

/*
 * get_ncpus() — number of enabled logical CPUs.  Triggers a one-shot
 * parse if the caller arrived before mp_v1_1_init() did.
 */
int
get_ncpus(void)
{
	if (!mp_phase1_done)
		mp_v1_1_init();
	return mp_cpu_count;
}

/*
 * validate_cpus() — mark the first `ncpus` slots in machine_slot[] as
 * containing real processors.  The slot index does NOT have to match the
 * LAPIC ID — kern/processor.c walks slots in order to allocate processor
 * structures, and the AP boot path (Incrementi 4) takes care of pairing
 * the slot back to the right LAPIC ID.
 */
void
validate_cpus(int ncpus)
{
	int i;

	if (ncpus > NCPUS)
		ncpus = NCPUS;
	for (i = 0; i < ncpus; i++)
		machine_slot[i].is_cpu = TRUE;
}
