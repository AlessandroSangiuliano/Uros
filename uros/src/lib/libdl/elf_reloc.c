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
 * elf_reloc.c — ELF relocation processing for libdl, both machines (#423).
 *
 * Relocation logic derived from FreeBSD rtld-elf i386/reloc.c
 * by John D. Polstra (BSD 2-clause license), adapted for Uros:
 * symbol resolution via host symbol table, no goto.
 *
 * ⚠️ The heading said "i386" and this file now serves both, which is not the
 * same as having been widened: the loop was reshaped.  i386 relocations ADD to
 * the slot and x86-64 relocations REPLACE it, so the cases are written as
 * "*where = f(symbol, addend, base)" with the addend coming from
 * reloc_addend() below -- one body that is correct on both rather than one
 * that is correct on one and quietly wrong on the other.
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
static Elf_Addr
resolve_reloc_sym(const struct dl_object *obj, unsigned long symnum)
{
	const Elf_Sym *sym;
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
		return (Elf_Addr)(obj->relocbase + sym->st_value);

	/* Otherwise resolve via object hash + host symtab */
	addr = dl_resolve_symbol(name, obj);
	if (addr != NULL)
		return (Elf_Addr)addr;

	/* Weak undefined symbols resolve to zero */
	if (ELF_ST_BIND(sym->st_info) == STB_WEAK)
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

/*
 * The addend of one relocation entry (#423).
 *
 * 🔑 This is the whole shape difference between the two machines, and it is
 * not a type: i386 uses REL, where the addend is already sitting in the slot
 * and a relocation ADDS to it; x86-64 uses RELA, where the addend travels in
 * the entry and the relocation REPLACES the slot.  Writing every case as
 * "*where = f(S, A, B)" with A read from here makes one body correct for both,
 * where "*where += ..." is correct for exactly one and silently doubles the
 * bias on the other.
 *
 * ⚠️ And a measured caveat, because the obvious claim about this is FALSE.
 *
 * It is tempting to say that reading the wrong source gives a pointer wrong by
 * one load address.  It does not, with this linker: GNU ld writes the addend
 * into the slot as well as into the entry, so on dl_module.so the three
 * RELATIVE entries carry addends 0x1020, 0x2000, 0x2000 and their slots hold
 * exactly those.  Reading *where instead of r_addend gives the same number,
 * and the ablation that swapped them changed nothing -- 4 of 4 either way.
 *
 * 🔑 So the RELA source is used because it is the one the format DEFINES, not
 * because the other is observably broken here.  The slot agreeing is this
 * linker's courtesy: a RELA producer is permitted to leave zeros there, and
 * anything that relocates twice reads its own first pass.  A correctness that
 * depends on a courtesy is not one that should be relied on, and it is exactly
 * the kind that holds until the day it does not.
 */

static inline Elf_Addr
reloc_addend(const Elf_Reloc *rel, const Elf_Addr *where)
{
#if ELF_RELA_HAS_ADDEND
	(void)where;
	return (Elf_Addr)rel->r_addend;
#else
	(void)rel;
	return *where;
#endif
}

int
dl_relocate(struct dl_object *obj)
{
	const Elf_Reloc *rel, *rellim;
	Elf_Addr *where;
	unsigned long sym_idx;
	Elf_Addr sym_addr, addend;
	int reltype;

	/* Make text writable if needed */
	if (obj->textrel) {
		vm_protect(mach_task_self(), obj->mapbase, obj->mapsize,
			   FALSE,
			   VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE);
	}

	/* --- Process DT_REL relocations --- */

	if (obj->rel != NULL && obj->relsize > 0) {
		rellim = (const Elf_Reloc *)
			((const char *)obj->rel + obj->relsize);

		for (rel = obj->rel; rel < rellim; rel++) {
			where = (Elf_Addr *)
				(obj->relocbase + rel->r_offset);
			reltype = (int)ELF_R_TYPE(rel->r_info);
			sym_idx = (unsigned long)ELF_R_SYM(rel->r_info);
			addend = reloc_addend(rel, where);

			switch (reltype) {

			case ELF_R_NONE:
				break;

			case ELF_R_DIRECT:
				sym_addr = resolve_reloc_sym(obj, sym_idx);
				if (sym_addr == 0 && dl_error_msg[0] != '\0')
					return -1;
				*where = sym_addr + addend;
				break;

			case ELF_R_PC32:
				sym_addr = resolve_reloc_sym(obj, sym_idx);
				if (sym_addr == 0 && dl_error_msg[0] != '\0')
					return -1;
				/*
				 * ⚠️ Thirty-two bits on BOTH machines, and the
				 * only case here where that is true: the name
				 * says so and R_X86_64_PC32 means it -- the
				 * displacement is a 32-bit field even though
				 * addresses are 64.  Storing through where as
				 * an Elf_Addr would write eight bytes over the
				 * four the linker reserved, and over whatever
				 * follows them.
				 */
				*(Elf_Word *)where = (Elf_Word)
					(sym_addr + addend - (Elf_Addr)where);
				break;

			case ELF_R_GLOB_DAT:
				sym_addr = resolve_reloc_sym(obj, sym_idx);
				if (sym_addr == 0 && dl_error_msg[0] != '\0')
					return -1;
				*where = sym_addr;
				break;

			case ELF_R_RELATIVE:
				*where = (Elf_Addr)obj->relocbase + addend;
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
		rellim = (const Elf_Reloc *)
			((const char *)obj->pltrel + obj->pltrelsize);

		for (rel = obj->pltrel; rel < rellim; rel++) {
			where = (Elf_Addr *)
				(obj->relocbase + rel->r_offset);
			sym_idx = (unsigned long)ELF_R_SYM(rel->r_info);

			/*
			 * ⚠️ The type is NOT checked here, and that was true
			 * before this issue too: everything in DT_JMPREL is
			 * assumed to be a JMP_SLOT.  It holds -- a PLT table
			 * carries nothing else -- but it means the assumption
			 * is the only thing keeping a foreign entry from being
			 * written as an address.  Named rather than fixed,
			 * because refusing it needs a decision about what a
			 * mixed table should do.
			 */
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
