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

	/*
	 * ⚠️ And a sizeless symbol stops where the next one begins (#409).
	 *
	 * #428 gave the lookup section bounds so that an address outside every
	 * loaded section could be refused instead of attributed to the last
	 * label before it.  That closed the failure across sections and left it
	 * standing INSIDE one: a symbol with no size — which is every assembly
	 * label — still claimed everything above it up to the end of .text.
	 *
	 * It was not hypothetical.  Every boot printed, in the backtrace of
	 * every idle processor, `<thread_frame_return+0x3c516>' for an address
	 * that is in `idle_thread': the sized symbol that really covered it was
	 * correctly declined (see ksym_lookup_call below for why), and the
	 * fallback was the last sizeless label in the section, a quarter of a
	 * megabyte away.  It had been there since the first backtrace and
	 * nobody read it, which is what a plausible wrong answer buys.
	 *
	 * A label says nothing about where it ends, but the symbol above it
	 * does: whatever that is, this one cannot reach past it.  So the bound
	 * exists after all, and refusing is what is left when the address is
	 * beyond it.  Second pass rather than a second tracker, because `best'
	 * is not known until the first one finishes.
	 */
	if (best->size == 0) {
		uint64_t limit = 0;

		for (unsigned i = 0; i < nsymbols; i++) {
			const struct elf64_sym *s = &symbols[i];

			if (s->value <= best->value || s->shndx != sp->shndx)
				continue;

			if (limit == 0 || s->value < limit)
				limit = s->value;
		}

		if (limit != 0 && addr >= limit)
			return 0;
	}

	if (best->name >= strings_size)
		return 0;		/* a name outside its own table */

	if (offset != 0)
		*offset = addr - best->value;

	return strings + best->name;
}

/*
 * The same, for an address that came off a stack as a RETURN address (#409).
 *
 * ⚠️ A return address is not an address in the function that is returning to
 * it — it is one byte past the end of the call.  Everywhere in the middle of a
 * function the difference does not show.  At the end of one it is the whole
 * answer: when the last instruction a function executes is a call, the byte
 * after it is the first byte of the NEXT function, and looking the return
 * address up directly names that one, or, if alignment padding sits between,
 * names nothing and falls back to whatever came before.
 *
 * That is not a corner case here.  A continuation is exactly this shape —
 * `idle_thread' ends with `call idle_thread_continue' and the call does not
 * come back — so the frame that gets misnamed is the one every blocked thread
 * in the system has.
 *
 * The instruction is what is looked up, therefore, and the address is what is
 * printed: `ret - 1' is inside the call whatever its length, so it lands in
 * the calling function without needing to know how long the call was.  The
 * offset is then measured from the symbol to the address that was printed, so
 * the two agree on the same line.
 */
const char *ksym_lookup_call(uint64_t ret, uint64_t *offset)
{
	const char *name;
	uint64_t off = 0;

	if (ret == 0)
		return 0;

	name = ksym_lookup(ret - 1, &off);
	if (name == 0)
		return 0;

	if (offset != 0)
		*offset = off + 1;

	return name;
}

/*
 * The tail-call property, checked over every function in the kernel (#409).
 *
 * ⚠️ The point is that it is not checked against this file's own rule.  A test
 * that asked "does the lookup return a symbol that covers the address" would be
 * restating the implementation and would have passed on the broken one.
 *
 * What is checked instead is an outside fact: for a function that ends in a
 * call, the byte after that call is where the return address points, and the
 * name for it must be THAT function, at offset exactly its size.  The symbol
 * table states each function's size independently of anything the lookup does,
 * so `off == size' is an agreement between two sources rather than one source
 * with itself.
 *
 * Every sized function, because a defect here is a property of a *shape* — a
 * call as the last instruction — and which functions have that shape changes
 * with every build.  Three thousand of them cost a few milliseconds once.
 *
 * `worst' comes back with the largest offset any of the plain lookups produced
 * over the same addresses, and that is the number worth printing: the failure
 * this replaces did not look like an error, it looked like `+0x3c516'.
 */
unsigned ksym_tailcall_check(unsigned *checked, uint64_t *worst)
{
	unsigned bad = 0, n = 0;
	uint64_t far = 0;

	for (unsigned i = 0; i < nsymbols; i++) {
		const struct elf64_sym *s = &symbols[i];
		uint64_t past, off = 0;
		const char *name;

		if (s->value == 0 || s->size == 0 || !in_code(s->shndx))
			continue;

		past = s->value + s->size;
		n++;

		name = ksym_lookup_call(past, &off);
		if (name == 0 || off != s->size)
			bad++;

		/*
		 * And the plain lookup at the same address, which is entitled to
		 * refuse — padding belongs to nobody — but not to answer with a
		 * symbol a quarter of a megabyte below it.
		 */
		off = 0;
		if (ksym_lookup(past, &off) != 0 && off > far)
			far = off;
	}

	if (checked != 0)
		*checked = n;
	if (worst != 0)
		*worst = far;

	return bad;
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
