/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Kernel symbols, so a backtrace names functions (#428).
 */

#include <stdint.h>

#include <boot/multiboot2.h>
#include <ddb/ksym.h>
#include <pmap/layout.h>

/* The section header, in the shape the file format defines it. */
struct elf64_shdr {
	uint32_t name;
	uint32_t type;
	uint64_t flags;
	uint64_t addr;
	uint64_t offset;
	uint64_t size;
	uint32_t link;
	uint32_t info;
	uint64_t addralign;
	uint64_t entsize;
};

#define SHT_SYMTAB	2
#define SHT_STRTAB	3
#define SHF_ALLOC		0x2
#define SHF_EXECINSTR	0x4

struct elf64_sym {
	uint32_t name;
	uint8_t  info;
	uint8_t  other;
	uint16_t shndx;
	uint64_t value;
	uint64_t size;
};

/*
 * Which sections hold code, as a bitmap of their indices.
 *
 * ⚠️ This is the test a symbol has to pass, and the obvious one — that it is
 * typed STT_FUNC — is wrong. A compiler emits that type; an assembler does
 * not, unless the source says `.type` by hand. So requiring it silently
 * excludes every hand-written entry point: the trap stubs, syscall_entry,
 * context_switch_raw, the halt the boot ends on. Precisely the code a
 * backtrace is most needed for, and precisely the code least likely to have
 * remembered a directive.
 *
 * Found here: the first unnamed frame in a backtrace was `hang64`, in
 * boot.S, skipped for having no type.
 *
 * Living in an executable section is a property of the file rather than of a
 * convention somebody followed, so it holds for assembly nobody here wrote.
 */
static uint64_t exec_sections;

/* The low nibble of `info` is the kind; 2 is a function. */
#define ELF64_ST_TYPE(i)	((i) & 0xF)
#define STT_FUNC		2
#define KSYM_MAX_SECTIONS	64

static const struct elf64_sym *symbols;
static const char *strings;
static unsigned nsymbols;
static uint64_t strings_size;

/*
 * Where each loaded section lies, so that an address outside all of them can
 * be refused rather than attributed to the nearest preceding symbol.
 */
struct ksym_span {
	uint16_t	shndx;
	uint64_t	start;
	uint64_t	end;
};

static struct ksym_span	spans[KSYM_MAX_SECTIONS];
static unsigned		nspans;

/* The section an address is in, or none. */
static const struct ksym_span *span_of(uint64_t addr)
{
	for (unsigned i = 0; i < nspans; i++)
		if (addr >= spans[i].start && addr < spans[i].end)
			return &spans[i];
	return 0;
}

static int in_code(uint16_t shndx)
{
	return shndx < KSYM_MAX_SECTIONS
	       && (exec_sections >> shndx & 1) != 0;
}

/*
 * A section's data, wherever the loader put it.
 *
 * `addr` is the address the section will have when running, which for a
 * section that is part of a loaded segment is a kernel virtual address and
 * for one that is not — and `.symtab` is not — is whatever the loader chose,
 * a physical address in low memory. Telling them apart by which half of the
 * address space they are in is exactly what the layout predicate is for.
 */
static const void *section_data(const struct elf64_shdr *sh)
{
	if (sh->addr == 0)
		return 0;

	return va_is_kernel(sh->addr)
	       ? (const void *)(uintptr_t)sh->addr
	       : (const void *)(uintptr_t)phys_to_direct(sh->addr);
}

static const struct mb2_tag_elf_sections *find_sections(uint32_t info_pa)
{
	const struct mb2_tag_elf_sections *t;

	t = (const struct mb2_tag_elf_sections *)
	    mb2_find_tag(info_pa, MB2_TAG_ELF_SECTIONS);

	/*
	 * The entry size is read from the tag rather than assumed, for the
	 * reason the memory map's is: the loader states it, and striding by
	 * our own structure would misread anything that ever differs.
	 */
	if (t == 0 || t->entsize < sizeof(struct elf64_shdr) || t->num == 0)
		return 0;

	return t;
}

static const struct elf64_shdr *section(const struct mb2_tag_elf_sections *t,
					unsigned i)
{
	return (const struct elf64_shdr *)((const uint8_t *)t + sizeof(*t)
					   + (uint64_t)i * t->entsize);
}

unsigned ksym_init(uint32_t info_pa)
{
	const struct mb2_tag_elf_sections *t = find_sections(info_pa);

	if (t == 0)
		return 0;

	exec_sections = 0;
	nspans = 0;
	for (unsigned i = 0; i < t->num && i < KSYM_MAX_SECTIONS; i++) {
		const struct elf64_shdr *sh = section(t, i);

		if (sh->flags & SHF_EXECINSTR)
			exec_sections |= 1ULL << i;

		/*
		 * And where every section that is IN MEMORY actually lies
		 * (#428).
		 *
		 * Without this a lookup has only the symbols to go on, and a
		 * symbol with no size -- every assembly label -- claims
		 * everything above it for ever.  Asking about a piece of data
		 * therefore produced a confident function name:
		 * `<__user_probe_end+0x879e1>' for an address half a megabyte
		 * past the end of the code, printed beside every wait event
		 * the debugger reported.
		 *
		 * A name that is wrong is worse than no name, because it is
		 * believed.  These bounds are what let the lookup refuse.
		 */
		if ((sh->flags & SHF_ALLOC) && sh->addr != 0 && sh->size != 0
		    && nspans < KSYM_MAX_SECTIONS) {
			spans[nspans].shndx = (uint16_t) i;
			spans[nspans].start = sh->addr;
			spans[nspans].end = sh->addr + sh->size;
			nspans++;
		}
	}

	for (unsigned i = 0; i < t->num; i++) {
		const struct elf64_shdr *sh = section(t, i);
		const struct elf64_shdr *str;

		if (sh->type != SHT_SYMTAB || sh->entsize == 0)
			continue;

		/*
		 * A symbol table names its own string table, and using any
		 * other one produces names that are real strings from the
		 * wrong place — which reads as a symbol and is not.
		 */
		if (sh->link >= t->num)
			continue;

		str = section(t, sh->link);
		if (str->type != SHT_STRTAB)
			continue;

		symbols = section_data(sh);
		strings = section_data(str);
		if (symbols == 0 || strings == 0)
			continue;	/* described but never loaded */

		nsymbols = (unsigned)(sh->size / sh->entsize);
		strings_size = str->size;
		return nsymbols;
	}

	return 0;
}

unsigned ksym_count(void)
{
	return nsymbols;
}

/*
 * Whether an address is inside a section that will be EXECUTED (#428).
 *
 * A breakpoint on anything else can never fire, and a debugger that accepted
 * one would report success and then let the operator wait for it -- the
 * quietest way to waste an afternoon.  The section bounds are already here
 * for the symbol lookup; this asks the other question of them.
 */
int span_is_code(uint64_t addr)
{
	const struct ksym_span *sp = span_of(addr);

	return sp != 0 && in_code(sp->shndx);
}

const char *ksym_lookup(uint64_t addr, uint64_t *offset)
{
	const struct elf64_sym *best = 0;
	const struct ksym_span *sp = span_of(addr);

	/*
	 * ⚠️ Outside every loaded section there is no name to give, and the
	 * honest answer is none.  This used to fall through to the symbol
	 * scan, where the last sizeless label in the last code section
	 * matched anything above it.
	 */
	if (sp == 0)
		return 0;

	for (unsigned i = 0; i < nsymbols; i++) {
		const struct elf64_sym *s = &symbols[i];

		/*
		 * In the SAME section as the address.  A symbol in another one
		 * cannot describe it however close it is, and this is what
		 * lets data be named by data symbols and code by code symbols
		 * without either borrowing the other's names.
		 */
		if (s->value == 0 || s->shndx != sp->shndx)
			continue;

		if (addr < s->value)
			continue;

		/*
		 * A symbol with a size covers a known span, and an address
		 * past its end belongs to whatever comes after it — so it is
		 * not a candidate at all. One without a size says nothing
		 * about where it ends, and the nearest preceding is the best
		 * available answer.
		 */
		if (s->size != 0 && addr >= s->value + s->size)
			continue;

		if (best == 0 || s->value > best->value)
			best = s;
	}

	if (best == 0)
		return 0;

	if (best->name >= strings_size)
		return 0;		/* a name outside its own table */

	if (offset != 0)
		*offset = addr - best->value;

	return strings + best->name;
}

uint64_t ksym_data_end(uint32_t info_pa)
{
	const struct mb2_tag_elf_sections *t = find_sections(info_pa);
	uint64_t end = 0;

	if (t == 0)
		return 0;

	/*
	 * The end of the highest section the loader placed in low memory.
	 *
	 * Only those: a section that landed in the kernel half is part of the
	 * image and is already accounted for, while one in low memory is
	 * somewhere the loader chose and is exactly what the allocator would
	 * otherwise hand out.
	 */
	for (unsigned i = 0; i < t->num; i++) {
		const struct elf64_shdr *sh = section(t, i);
		uint64_t sh_end;

		if (sh->addr == 0 || va_is_kernel(sh->addr))
			continue;

		sh_end = sh->addr + sh->size;
		if (sh_end > end)
			end = sh_end;
	}

	return end;
}
