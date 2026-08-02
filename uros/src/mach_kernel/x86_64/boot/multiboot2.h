/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Reading the multiboot2 handoff (#406/#407).
 *
 * boot.S captured the boot-information pointer GRUB left in %ebx and has
 * carried it, unread, since the first increment.  This is what finally opens
 * it: a length-prefixed list of tags, each 8-byte aligned, ending with a tag
 * of type zero.  The one the pmap wants is the memory map — the only account
 * of which physical addresses are RAM, which the direct map must be sized
 * against instead of guessing.
 */

#ifndef _X86_64_BOOT_MULTIBOOT2_H_
#define _X86_64_BOOT_MULTIBOOT2_H_

#include <stdint.h>

#include <mach/boolean.h>

#define MB2_BOOTLOADER_MAGIC	0x36d76289	/* what GRUB leaves in %eax */

#define MB2_TAG_END		0
#define MB2_TAG_CMDLINE		1	/* the string the loader was given */
#define MB2_TAG_MODULE		3	/* a module the loader placed */
#define MB2_TAG_MEMORY_MAP	6

/*
 * Where ACPI starts.  The root pointer is normally found by scanning the
 * BIOS areas for its signature, but the loader has already done that and
 * hands it over — which is both faster and the only reliable way under UEFI,
 * where the tables are not in the legacy region at all.
 *
 * Two tags because ACPI has two root-pointer formats: the 1.0 one with a
 * 32-bit RSDT address, and the 2.0 one that adds a 64-bit XSDT address.  A
 * loader supplies whichever the firmware offered.
 */
#define MB2_TAG_ELF_SECTIONS	9	/* the kernel's own section headers */
#define MB2_TAG_ACPI_OLD	14	/* ACPI 1.0 RSDP */
#define MB2_TAG_ACPI_NEW	15	/* ACPI 2.0+ RSDP */

/*
 * The kernel's own ELF section headers, which the loader passes back because
 * it has them anyway.  `.symtab` and `.strtab` are among them, which is why
 * this target needs no separate symbol file (#428).
 *
 * `shndx` names the section holding the section *names*; the symbol names
 * are in a different string table, the one the symbol table itself links to.
 */
/*
 * The kernel's command line, as one NUL-terminated string after the header.
 *
 * ⚠️ Nothing on this target clears .bss — the loader does it, by zeroing each
 * segment beyond what the file supplies. So a flag parsed in C and kept in a
 * static is safe here, unlike on i386 where the kernel's own clear ran after
 * the parse and #337 was the bill for it.
 */
struct mb2_tag_string {
	uint32_t type;
	uint32_t size;
	char     string[];
};

/*
 * A module: where the loader put it, and the string it was given with.
 *
 * ⚠️ Unlike multiboot 1, there is no array and no count.  Each module is its
 * own tag in the chain, so "module N" means the N-th tag of this type and
 * finding it is a walk.  Anything that wants them all should walk once
 * rather than ask N times.
 */
struct mb2_tag_module {
	uint32_t type;
	uint32_t size;
	uint32_t mod_start;	/* physical, inclusive */
	uint32_t mod_end;	/* physical, exclusive */
	char     string[];
};

struct mb2_tag_elf_sections {
	uint32_t type;
	uint32_t size;
	uint32_t num;
	uint32_t entsize;
	uint32_t shndx;
	/* section headers follow */
};

struct mb2_tag {
	uint32_t type;
	uint32_t size;
};

/* Memory-map tag: a header followed by entry_size-sized entries. */
struct mb2_tag_mmap {
	uint32_t type;
	uint32_t size;
	uint32_t entry_size;
	uint32_t entry_version;
};

struct mb2_mmap_entry {
	uint64_t addr;
	uint64_t len;
	uint32_t type;
	uint32_t reserved;
};

#define MB2_MEM_AVAILABLE	1	/* every other type is not ours to use */

/*
 * The tag of the given type, or NULL if the loader did not supply one.
 *
 * info_pa is a physical address: GRUB places the structure in low memory and
 * the boot identity map still covers it, so it is dereferenced directly.
 * That is deliberate — this has to be readable before the direct map exists,
 * since the direct map is sized from what it says.
 */
const struct mb2_tag *mb2_find_tag(uint32_t info_pa, uint32_t type);

/*
 * One past the highest byte of available RAM, or zero when the loader gave
 * no memory map.  Regions the firmware has claimed are ignored: it is the
 * extent that matters, since the direct map spans holes rather than
 * describing them.
 */
uint64_t mb2_top_of_ram(uint32_t info_pa);

/* Total bytes marked available — what the machine actually has to spend. */
uint64_t mb2_usable_ram(uint32_t info_pa);

/*
 * Remember where the loader's structure is, so that code running long after
 * boot can still ask.  Called once, from the boot entry (#453).
 *
 * Kept here rather than passed down because <kern/boot_modules.h> is asked
 * its questions from kern/bootstrap.c, which runs after every trace of the
 * boot handoff has gone out of scope.
 */
void mb2_remember(uint32_t info_pa);

/* How many modules the loader placed, by walking the chain. */
unsigned int mb2_module_count(void);

/* Module n's physical range.  FALSE when there is no such module. */
boolean_t mb2_module_range(unsigned int n, uint64_t *start, uint64_t *size);

/* The command line, or "" when the loader supplied none. */
const char *mb2_cmdline(void);

/* What mb2_remember() was given, or zero if it was never called. */
uint32_t mb2_info(void);

#endif	/* _X86_64_BOOT_MULTIBOOT2_H_ */
