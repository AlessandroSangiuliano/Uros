/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * ddb/db_elf.c — DDB symbol-table backend for the ksyms.bin blob (#211).
 *
 * The kernel's own .symtab / .strtab live outside any PT_LOAD segment,
 * so multiboot does not load them.  Build-time helper scripts/gen-ksyms.py
 * extracts the symbol table into a self-describing binary
 * (see scripts/gen-ksyms.py for the layout) and the boot script ships it
 * as a separate multiboot module.  Early kernel boot (model_dep.c) scans
 * the module list, finds the "KSYM" magic and hands the blob to
 * elf_db_register() before ddb_init() runs.
 *
 * Design notes:
 *   - Single instance.  We only ever load one kernel symtab; no need
 *     to reuse the slot for user binaries.
 *   - Lookup by name: linear scan.  At ~3500 symbols a hash would help
 *     interactive completion but the per-call cost is microseconds and
 *     DDB is not on any hot path.
 *   - Lookup by address: binary search (entries sorted ascending by addr).
 *   - We slot into x_db[SYMTAB_MACHDEP] (the existing unused "NONE" entry)
 *     to avoid touching every symtab_type() consumer.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <mach/boolean.h>
#include <machine/db_machdep.h>
#include <ddb/db_sym.h>
#include <ddb/db_output.h>
#include <ddb/db_elf.h>

/* ============================================================
 * Blob layout (must match scripts/gen-ksyms.py).
 * ============================================================ */

struct ksyms_header {
	char		magic[4];		/* "KSYM" */
	uint32_t	version;		/* 1 */
	uint32_t	n_syms;
	uint32_t	strtab_off;		/* from start of header */
	uint32_t	strtab_size;
};

struct ksyms_entry {
	uint32_t	addr;
	uint32_t	name_off;		/* into strtab */
};

#define KSYMS_VERSION	1

/* ============================================================
 * Module-wide state.
 * ============================================================ */

static const struct ksyms_header *ks_hdr;
static const struct ksyms_entry  *ks_ent;
static const char                *ks_str;

/*
 * Called from model_dep.c after parse_arguments() has located the
 * multiboot module containing the "KSYM" magic.  base/size describe
 * the raw blob; we just keep pointers into it.  Returns FALSE if the
 * magic / version mismatch (caller usually just ignores).
 */
boolean_t
elf_db_register(const void *base, size_t size)
{
	const struct ksyms_header *h = (const struct ksyms_header *)base;

	if (size < sizeof(*h))
		return FALSE;
	if (h->magic[0] != 'K' || h->magic[1] != 'S' ||
	    h->magic[2] != 'Y' || h->magic[3] != 'M')
		return FALSE;
	if (h->version != KSYMS_VERSION)
		return FALSE;
	if ((size_t)h->strtab_off + (size_t)h->strtab_size > size)
		return FALSE;

	ks_hdr = h;
	ks_ent = (const struct ksyms_entry *)((const char *)h + sizeof(*h));
	ks_str = (const char *)h + h->strtab_off;
	return TRUE;
}

/* ============================================================
 * Internal helpers.
 * ============================================================ */

static const char *
ksyms_name(const struct ksyms_entry *e)
{
	if (e == 0 || ks_str == 0)
		return "";
	if (e->name_off >= ks_hdr->strtab_size)
		return "";
	return ks_str + e->name_off;
}

/* Binary search for the entry whose addr is the largest <= target.
 * Returns NULL if target is below the lowest symbol. */
static const struct ksyms_entry *
ksyms_addr_floor(uint32_t target)
{
	uint32_t lo, hi;
	if (ks_hdr == 0 || ks_hdr->n_syms == 0)
		return 0;

	lo = 0;
	hi = ks_hdr->n_syms;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;
		if (ks_ent[mid].addr <= target)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo == 0)
		return 0;
	return &ks_ent[lo - 1];
}

/* ============================================================
 * x_db backend hooks.
 * ============================================================ */

void
elf_db_init(void)
{
	if (ks_hdr == 0)
		return;

	(void)db_add_symbol_table(SYMTAB_MACHDEP,
				  (char *)ks_ent,
				  (char *)(ks_ent + ks_hdr->n_syms),
				  "mach",
				  (char *)ks_str,
				  (char *)0,
				  ks_ent[0].addr,
				  ks_ent[ks_hdr->n_syms - 1].addr,
				  /*sorted=*/TRUE);
}

boolean_t
elf_db_sym_init(char *start, char *end, char *name, char *task_addr)
{
	(void)start; (void)end; (void)name; (void)task_addr;
	/* Nothing per-table to initialise — register has done it. */
	return TRUE;
}

db_sym_t
elf_db_lookup(db_symtab_t *stab, char *symstr)
{
	uint32_t i;
	(void)stab;
	if (ks_hdr == 0)
		return DB_SYM_NULL;
	for (i = 0; i < ks_hdr->n_syms; i++) {
		const char *nm = ks_str + ks_ent[i].name_off;
		if (strcmp(nm, symstr) == 0)
			return (db_sym_t)&ks_ent[i];
	}
	return DB_SYM_NULL;
}

db_sym_t
elf_db_search_symbol(db_symtab_t *stab, db_addr_t off, db_strategy_t strategy,
		     db_expr_t *diffp)
{
	const struct ksyms_entry *e;
	(void)stab; (void)strategy;

	e = ksyms_addr_floor((uint32_t)off);
	if (e == 0) {
		*diffp = (db_expr_t)off;
		return DB_SYM_NULL;
	}
	*diffp = (db_expr_t)((uint32_t)off - e->addr);
	return (db_sym_t)e;
}

boolean_t
elf_db_line_at_pc(db_symtab_t *stab, db_sym_t sym, char **file, int *line,
		  db_expr_t pc)
{
	(void)stab; (void)sym; (void)file; (void)line; (void)pc;
	/* No line / file info in ksyms.bin (and DWARF would be huge). */
	return FALSE;
}

void
elf_db_symbol_values(db_sym_t sym, char **namep, db_expr_t *valuep)
{
	const struct ksyms_entry *e = (const struct ksyms_entry *)sym;
	if (e == 0)
		return;
	if (namep)
		*namep = (char *)ksyms_name(e);
	if (valuep)
		*valuep = (db_expr_t)e->addr;
}

db_sym_t
elf_db_search_by_addr(db_symtab_t *stab, db_addr_t addr, char **file,
		      char **func, int *line, db_expr_t *diffp, int *args)
{
	const struct ksyms_entry *e;
	(void)stab;

	if (file)  *file = 0;
	if (line)  *line = 0;
	if (args)  *args = -1;

	e = ksyms_addr_floor((uint32_t)addr);
	if (e == 0) {
		if (diffp) *diffp = (db_expr_t)addr;
		if (func)  *func  = 0;
		return DB_SYM_NULL;
	}
	if (diffp) *diffp = (db_expr_t)((uint32_t)addr - e->addr);
	if (func)  *func  = (char *)ksyms_name(e);
	return (db_sym_t)e;
}

int
elf_db_print_completion(db_symtab_t *stab, char *symstr)
{
	uint32_t i;
	int n = 0;
	size_t len = strlen(symstr);
	(void)stab;
	if (ks_hdr == 0)
		return 0;
	for (i = 0; i < ks_hdr->n_syms; i++) {
		const char *nm = ks_str + ks_ent[i].name_off;
		if (strncmp(nm, symstr, len) == 0) {
			db_printf("%s ", nm);
			n++;
		}
	}
	if (n) db_printf("\n");
	return n;
}

int
elf_db_lookup_incomplete(db_symtab_t *stab, char *symstr, char **name,
			 int *len, int *toadd)
{
	uint32_t i;
	int n = 0;
	int common = -1;
	size_t inlen;
	const char *first_match = 0;
	(void)stab;

	if (ks_hdr == 0)
		return 0;

	inlen = strlen(symstr);
	for (i = 0; i < ks_hdr->n_syms; i++) {
		const char *nm = ks_str + ks_ent[i].name_off;
		if (strncmp(nm, symstr, inlen) != 0)
			continue;
		n++;
		if (first_match == 0) {
			first_match = nm;
			common = (int)strlen(nm);
		} else {
			int j;
			for (j = (int)inlen; j < common; j++) {
				if (first_match[j] != nm[j])
					break;
			}
			common = j;
		}
	}

	if (n == 0)
		return 0;

	if (n == 1) {
		strcpy(symstr, first_match);
		if (name) *name = symstr;
		if (len)  *len  = (int)strlen(first_match);
		if (toadd) *toadd = (int)(strlen(first_match) - inlen);
		return 1;
	}

	/* Common prefix completion. */
	{
		int extra = common - (int)inlen;
		if (extra > 0) {
			strncpy(symstr + inlen, first_match + inlen,
				(size_t)extra);
			symstr[inlen + extra] = '\0';
		}
	}
	if (name)  *name  = symstr;
	if (len)   *len   = common;
	if (toadd) *toadd = common - (int)inlen;
	return n;
}
