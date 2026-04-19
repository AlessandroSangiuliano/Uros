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
 * elf_reloc.c — i386 ELF relocation processing for libdl.
 *
 * Relocation logic derived from FreeBSD rtld-elf i386/reloc.c
 * by John D. Polstra (BSD 2-clause license), adapted for Uros:
 * symbol resolution via host symbol table, no goto.
 */

#include <mach.h>
#include <mach/vm_prot.h>
#include <mach/mach_traps.h>
#include <stdio.h>
#include <string.h>
#include "dl_internal.h"

/*
 * Resolve symbol for a relocation entry.
 *
 * Looks up the symbol name by index in the object's symtab/strtab,
 * then resolves via dl_resolve_symbol (object first, then host table).
 *
 * Returns the resolved address, or 0 with dl_error_msg set on failure.
 */
static Elf32_Addr
resolve_reloc_sym(const struct dl_object *obj, unsigned long symnum)
{
	const Elf32_Sym *sym;
	const char *name;
	void *addr;

	if (symnum >= obj->nchains) {
		snprintf(dl_error_msg, DL_ERRMSG_SIZE,
			 "%s: relocation symbol index %lu out of range",
			 obj->path ? obj->path : "?", symnum);
		return 0;
	}

	sym = obj->symtab + symnum;
	name = obj->strtab + sym->st_name;

	/* If the symbol is defined in this object, use it directly */
	if (sym->st_shndx != SHN_UNDEF)
		return (Elf32_Addr)(obj->relocbase + sym->st_value);

	/* Otherwise resolve via object hash + host symtab */
	addr = dl_resolve_symbol(name, obj);
	if (addr != NULL)
		return (Elf32_Addr)addr;

	/* Weak undefined symbols resolve to zero */
	if (ELF32_ST_BIND(sym->st_info) == STB_WEAK)
		return 0;

	snprintf(dl_error_msg, DL_ERRMSG_SIZE,
		 "%s: undefined symbol \"%s\"",
		 obj->path ? obj->path : "?", name);
	return 0;
}

/* ================================================================
 * Apply all relocations (DT_REL + DT_JMPREL) to a loaded object.
 *
 * Phase 1: all relocations are resolved eagerly (no lazy PLT).
 *
 * If the object has DT_TEXTREL, text pages are temporarily made
 * writable for the duration of relocation processing.
 *
 * Returns 0 on success, -1 on failure (dl_error_msg set).
 * ================================================================ */

int
dl_relocate(struct dl_object *obj)
{
	const Elf32_Rel *rel, *rellim;
	Elf32_Addr *where;
	unsigned long sym_idx;
	Elf32_Addr sym_addr;
	int reltype;

	/* Make text writable if needed */
	if (obj->textrel) {
		vm_protect(mach_task_self(), obj->mapbase, obj->mapsize,
			   FALSE,
			   VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE);
	}

	/* --- Process DT_REL relocations --- */

	if (obj->rel != NULL && obj->relsize > 0) {
		rellim = (const Elf32_Rel *)
			((const char *)obj->rel + obj->relsize);

		for (rel = obj->rel; rel < rellim; rel++) {
			where = (Elf32_Addr *)
				(obj->relocbase + rel->r_offset);
			reltype = ELF32_R_TYPE(rel->r_info);
			sym_idx = ELF32_R_SYM(rel->r_info);

			switch (reltype) {

			case R_386_NONE:
				break;

			case R_386_32:
				sym_addr = resolve_reloc_sym(obj, sym_idx);
				if (sym_addr == 0 && dl_error_msg[0] != '\0')
					return -1;
				*where += sym_addr;
				break;

			case R_386_PC32:
				sym_addr = resolve_reloc_sym(obj, sym_idx);
				if (sym_addr == 0 && dl_error_msg[0] != '\0')
					return -1;
				*where += sym_addr - (Elf32_Addr)where;
				break;

			case R_386_GLOB_DAT:
				sym_addr = resolve_reloc_sym(obj, sym_idx);
				if (sym_addr == 0 && dl_error_msg[0] != '\0')
					return -1;
				*where = sym_addr;
				break;

			case R_386_RELATIVE:
				*where += (Elf32_Addr)obj->relocbase;
				break;

			default:
				snprintf(dl_error_msg, DL_ERRMSG_SIZE,
					 "%s: unsupported relocation type %d",
					 obj->path ? obj->path : "?",
					 reltype);
				return -1;
			}
		}
	}

	/* --- Process DT_JMPREL (PLT) relocations eagerly --- */

	if (obj->pltrel != NULL && obj->pltrelsize > 0) {
		rellim = (const Elf32_Rel *)
			((const char *)obj->pltrel + obj->pltrelsize);

		for (rel = obj->pltrel; rel < rellim; rel++) {
			where = (Elf32_Addr *)
				(obj->relocbase + rel->r_offset);
			sym_idx = ELF32_R_SYM(rel->r_info);

			sym_addr = resolve_reloc_sym(obj, sym_idx);
			if (sym_addr == 0 && dl_error_msg[0] != '\0')
				return -1;
			*where = sym_addr;
		}
	}

	/* Restore text protection */
	if (obj->textrel) {
		vm_protect(mach_task_self(), obj->mapbase, obj->mapsize,
			   FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
	}

	return 0;
}
