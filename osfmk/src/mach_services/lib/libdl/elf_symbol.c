/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/*
 * elf_symbol.c — ELF symbol hash and lookup for libdl.
 *
 * SYSV ELF hash and hash-table lookup are derived from the ELF
 * specification (System V ABI) and from FreeBSD rtld-elf by
 * John D. Polstra (BSD 2-clause license).
 */

#include "dl_internal.h"

/* ================================================================
 * SYSV ELF hash (from ELF specification, figure 2-14)
 * ================================================================ */

unsigned long
dl_elf_hash(const char *name)
{
	const unsigned char *p = (const unsigned char *)name;
	unsigned long h = 0;
	unsigned long g;

	while (*p != '\0') {
		h = (h << 4) + *p++;
		g = h & 0xf0000000;
		if (g != 0)
			h ^= g >> 24;
		h &= ~g;
	}
	return h;
}

/* ================================================================
 * Look up a symbol in one loaded object's SYSV hash table.
 *
 * Returns the Elf32_Sym pointer if found and defined, NULL otherwise.
 * ================================================================ */

const Elf32_Sym *
dl_symlook_obj(const char *name, unsigned long hash,
	       const struct dl_object *obj)
{
	unsigned long symnum;
	const Elf32_Sym *symp;
	const char *strp;

	if (obj->buckets == NULL)
		return NULL;

	symnum = obj->buckets[hash % obj->nbuckets];

	while (symnum != STN_UNDEF) {
		if (symnum >= obj->nchains)
			return NULL;	/* corrupt object */

		symp = obj->symtab + symnum;
		strp = obj->strtab + symp->st_name;

		if (name[0] == strp[0] && strcmp(name, strp) == 0) {
			/* Found name match — return if symbol is defined */
			if (symp->st_shndx != SHN_UNDEF)
				return symp;
			return NULL;
		}

		symnum = obj->chains[symnum];
	}

	return NULL;
}

/* ================================================================
 * Resolve a symbol: first in the loaded object, then in the host
 * symbol table provided by the server that called dlopen().
 *
 * Returns the resolved address, or NULL if not found.
 * ================================================================ */

void *
dl_resolve_symbol(const char *name, const struct dl_object *obj)
{
	unsigned long hash;
	const Elf32_Sym *sym;
	const struct dl_host_sym *hs;

	/* 1. Try the object's own symbol table */
	hash = dl_elf_hash(name);
	sym = dl_symlook_obj(name, hash, obj);
	if (sym != NULL)
		return (void *)(obj->relocbase + sym->st_value);

	/* 2. Try the host symbol table */
	if (dl_host_symtab != NULL) {
		for (hs = dl_host_symtab; hs->name != NULL; hs++) {
			if (name[0] == hs->name[0] &&
			    strcmp(name, hs->name) == 0)
				return hs->addr;
		}
	}

	return NULL;
}
