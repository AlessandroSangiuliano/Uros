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
 * Cached state — populated on first call to mp_v1_1_init() and read back
 * by get_ncpus()/validate_cpus().
 */
static boolean_t	mp_parsed = FALSE;
static int		mp_cpu_count;
static unsigned char		mp_cpu_lapic_id[NCPUS];
static unsigned char		mp_bsp_lapic_id;
static unsigned int		mp_lapic_phys;
static unsigned int		mp_ioapic_phys[4];	/* up to 4 I/O APICs */
static int		mp_ioapic_count;

/*
 * Physical→kernel-virtual mapping.  Identity-mapped low memory (under
 * 1 MiB) is reachable through phystokv(); the MP table always lives
 * either there or in the BIOS ROM region, both of which qualify.
 */
extern vm_offset_t	phystokv_off;	/* unused but documents intent */
#define MP_PHYS_TO_KV(pa)	((vm_offset_t)(pa) + 0xC0000000U)

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
 *   3) BIOS ROM, 0xF0000–0xFFFFF.
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
 * mp_v1_1_init() — entry point called from model_dep.c during platform
 * bring-up.  Searches the FPS, parses the configuration table, and caches
 * the result for get_ncpus()/validate_cpus().  Idempotent.
 *
 * Note we override the stub installed by mp_stub.c when this file is
 * compiled in.
 */
void
mp_v1_1_init(void)
{
	struct mp_fps *fps;
	struct mp_config *config;

	if (mp_parsed)
		return;
	mp_parsed = TRUE;
	mp_cpu_count = 0;
	mp_ioapic_count = 0;

	fps = mp_find_fps();
	if (!fps) {
		printf("mp_table: no MP floating pointer found, UP fallback\n");
		mp_cpu_count = 1;
		mp_bsp_lapic_id = 0;
		return;
	}

	printf("mp_table: FPS at %p, spec %d.%d%s\n",
	       (void *)fps, fps->spec_rev >> 4 ? fps->spec_rev >> 4 : 1,
	       fps->spec_rev & 0xF, (fps->feature2 & 0x80) ? ", IMCR" : "");

	if (fps->config_phys == 0) {
		/* Default configuration — feature1 selects one of 7 layouts.
		 * Always 2 CPUs in default configs (MPS 1.4 §5).  Treat as
		 * such; LAPIC is the standard 0xFEE00000. */
		mp_cpu_count = 2;
		mp_lapic_phys = LAPIC_START;
		mp_bsp_lapic_id = 0;
		mp_cpu_lapic_id[0] = 0;
		mp_cpu_lapic_id[1] = 1;
		printf("mp_table: default config %u, assuming 2 CPUs\n",
		       fps->feature1);
		return;
	}

	config = (struct mp_config *)MP_PHYS_TO_KV(fps->config_phys);
	if (!mp_parse_config(config)) {
		printf("mp_table: PCMP at 0x%x invalid, UP fallback\n",
		       fps->config_phys);
		mp_cpu_count = 1;
		mp_bsp_lapic_id = 0;
		return;
	}

	printf("mp_table: %d CPU(s), BSP lapic_id=%d, lapic@0x%x, %d IOAPIC(s)\n",
	       mp_cpu_count, mp_bsp_lapic_id, mp_lapic_phys, mp_ioapic_count);
}

/*
 * get_ncpus() — number of enabled logical CPUs.  Triggers a one-shot
 * parse if the caller arrived before mp_v1_1_init() did.
 */
int
get_ncpus(void)
{
	if (!mp_parsed)
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
