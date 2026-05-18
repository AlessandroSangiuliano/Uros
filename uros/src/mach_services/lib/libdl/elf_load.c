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
 * elf_load.c — Load ELF shared objects into Mach VM for libdl.
 *
 * Segment mapping logic derived from FreeBSD rtld-elf map_object.c
 * by John D. Polstra (BSD 2-clause license), adapted to use Mach VM
 * primitives (vm_allocate/vm_protect/vm_deallocate) instead of mmap.
 */

#include <mach.h>
#include <mach/vm_prot.h>
#include <mach/mach_traps.h>
#include <string.h>
#include <stdio.h>
#include "dl_internal.h"

/* Page size — i386 */
#define DL_PAGE_SIZE	4096
#define DL_PAGE_MASK	(DL_PAGE_SIZE - 1)

static inline Elf32_Addr
dl_trunc_page(Elf32_Addr v)
{
	return v & ~DL_PAGE_MASK;
}

static inline Elf32_Addr
dl_round_page(Elf32_Addr v)
{
	return (v + DL_PAGE_MASK) & ~DL_PAGE_MASK;
}

/*
 * Convert ELF segment flags to Mach VM protection.
 */
static vm_prot_t
convert_prot(Elf32_Word flags)
{
	vm_prot_t prot = 0;

	if (flags & PF_R)
		prot |= VM_PROT_READ;
	if (flags & PF_W)
		prot |= VM_PROT_WRITE;
	if (flags & PF_X)
		prot |= VM_PROT_EXECUTE;
	return prot;
}

/* ================================================================
 * Map a shared object from an in-memory buffer into Mach VM.
 *
 * The caller has already read the entire file into filebuf/filesize
 * via the I/O callback.  We parse the ELF header and program headers,
 * allocate a contiguous VM region, copy PT_LOAD segments, zero BSS,
 * and set protections.
 *
 * Returns a new dl_object on success, NULL on failure (sets dl_error_msg).
 * ================================================================ */

struct dl_object *
dl_map_object(const void *filebuf, unsigned int filesize, const char *path)
{
	const Elf32_Ehdr *ehdr;
	const Elf32_Phdr *phdr, *ph;
	int i, nsegs;
	Elf32_Addr base_vaddr, base_vlimit;
	vm_size_t mapsize;
	vm_offset_t mapbase;
	kern_return_t kr;
	const Elf32_Phdr *dyn_phdr;
	struct dl_object *obj;
	const Elf32_Phdr *segs[16];	/* max 16 PT_LOAD segments */

	/* --- Validate ELF header --- */

	if (filesize < sizeof(Elf32_Ehdr)) {
		snprintf(dl_error_msg, DL_ERRMSG_SIZE,
			 "%s: file too small", path);
		return NULL;
	}

	ehdr = (const Elf32_Ehdr *)filebuf;

	if (ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
	    ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
	    ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
	    ehdr->e_ident[EI_MAG3] != ELFMAG3) {
		snprintf(dl_error_msg, DL_ERRMSG_SIZE,
			 "%s: not an ELF file", path);
		return NULL;
	}

	if (ehdr->e_ident[EI_CLASS] != ELFCLASS32) {
		snprintf(dl_error_msg, DL_ERRMSG_SIZE,
			 "%s: not 32-bit ELF", path);
		return NULL;
	}

	if (ehdr->e_type != ET_DYN) {
		snprintf(dl_error_msg, DL_ERRMSG_SIZE,
			 "%s: not a shared object (e_type=%d)", path,
			 ehdr->e_type);
		return NULL;
	}

	if (ehdr->e_machine != EM_386) {
		snprintf(dl_error_msg, DL_ERRMSG_SIZE,
			 "%s: wrong architecture (e_machine=%d)", path,
			 ehdr->e_machine);
		return NULL;
	}

	if (ehdr->e_phentsize != sizeof(Elf32_Phdr)) {
		snprintf(dl_error_msg, DL_ERRMSG_SIZE,
			 "%s: unexpected phentsize %d", path,
			 ehdr->e_phentsize);
		return NULL;
	}

	/* --- Scan program headers --- */

	phdr = (const Elf32_Phdr *)((const char *)filebuf + ehdr->e_phoff);
	nsegs = 0;
	dyn_phdr = NULL;

	for (i = 0; i < ehdr->e_phnum; i++) {
		ph = &phdr[i];

		if (ph->p_type == PT_LOAD) {
			if (nsegs >= 16) {
				snprintf(dl_error_msg, DL_ERRMSG_SIZE,
					 "%s: too many PT_LOAD segments",
					 path);
				return NULL;
			}
			segs[nsegs++] = ph;
		}

		if (ph->p_type == PT_DYNAMIC)
			dyn_phdr = ph;
	}

	if (nsegs == 0) {
		snprintf(dl_error_msg, DL_ERRMSG_SIZE,
			 "%s: no PT_LOAD segments", path);
		return NULL;
	}

	if (dyn_phdr == NULL) {
		snprintf(dl_error_msg, DL_ERRMSG_SIZE,
			 "%s: no PT_DYNAMIC segment", path);
		return NULL;
	}

	/* --- Compute total VM footprint --- */

	base_vaddr = dl_trunc_page(segs[0]->p_vaddr);
	base_vlimit = dl_round_page(segs[nsegs - 1]->p_vaddr +
				    segs[nsegs - 1]->p_memsz);
	mapsize = base_vlimit - base_vaddr;

	/* --- Allocate contiguous VM region --- */

	mapbase = 0;
	kr = vm_allocate(mach_task_self(), &mapbase, mapsize, TRUE);
	if (kr != KERN_SUCCESS) {
		snprintf(dl_error_msg, DL_ERRMSG_SIZE,
			 "%s: vm_allocate(%u bytes) failed: %d", path,
			 (unsigned)mapsize, kr);
		return NULL;
	}

	/* --- Copy each PT_LOAD segment into the mapped region --- */

	for (i = 0; i < nsegs; i++) {
		Elf32_Addr seg_vaddr = segs[i]->p_vaddr;
		Elf32_Off seg_offset = segs[i]->p_offset;
		Elf32_Word seg_filesz = segs[i]->p_filesz;
		Elf32_Word seg_memsz = segs[i]->p_memsz;
		vm_offset_t dest;

		/* Bounds check against input file */
		if (seg_offset + seg_filesz > filesize) {
			snprintf(dl_error_msg, DL_ERRMSG_SIZE,
				 "%s: PT_LOAD[%d] extends past end of file",
				 path, i);
			vm_deallocate(mach_task_self(), mapbase, mapsize);
			return NULL;
		}

		dest = mapbase + (seg_vaddr - base_vaddr);

		/* Copy file data */
		if (seg_filesz > 0)
			memcpy((void *)dest,
			       (const char *)filebuf + seg_offset,
			       seg_filesz);

		/* Zero BSS (memsz > filesz) */
		if (seg_memsz > seg_filesz)
			memset((void *)(dest + seg_filesz), 0,
			       seg_memsz - seg_filesz);
	}

	/* --- Set page protections for each segment --- */

	for (i = 0; i < nsegs; i++) {
		Elf32_Addr seg_start = dl_trunc_page(segs[i]->p_vaddr);
		Elf32_Addr seg_end = dl_round_page(segs[i]->p_vaddr +
						   segs[i]->p_memsz);
		vm_offset_t addr = mapbase + (seg_start - base_vaddr);
		vm_size_t size = seg_end - seg_start;
		vm_prot_t prot = convert_prot(segs[i]->p_flags);

		/*
		 * Skip vm_protect if RWX — that's the default from
		 * vm_allocate, and calling vm_protect with full perms
		 * is a no-op.
		 */
		if (prot != (VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE))
			vm_protect(mach_task_self(), addr, size, FALSE, prot);
	}

	/* --- Allocate and populate dl_object --- */

	/*
	 * Use vm_allocate for the object struct itself — we don't have
	 * malloc in the minimal server environment.
	 */
	{
		vm_offset_t obj_mem = 0;
		kr = vm_allocate(mach_task_self(), &obj_mem,
				 dl_round_page(sizeof(struct dl_object)),
				 TRUE);
		if (kr != KERN_SUCCESS) {
			vm_deallocate(mach_task_self(), mapbase, mapsize);
			snprintf(dl_error_msg, DL_ERRMSG_SIZE,
				 "%s: vm_allocate for dl_object failed", path);
			return NULL;
		}
		obj = (struct dl_object *)obj_mem;
	}

	memset(obj, 0, sizeof(*obj));
	obj->mapbase = mapbase;
	obj->mapsize = mapsize;
	obj->vaddrbase = base_vaddr;
	obj->relocbase = mapbase - base_vaddr;
	obj->dynamic = (const Elf32_Dyn *)(obj->relocbase + dyn_phdr->p_vaddr);
	obj->ref_count = 1;

	/* Copy path string */
	{
		unsigned int pathlen = strlen(path) + 1;
		vm_offset_t pathbuf = 0;
		kr = vm_allocate(mach_task_self(), &pathbuf,
				 dl_round_page(pathlen), TRUE);
		if (kr == KERN_SUCCESS) {
			memcpy((void *)pathbuf, path, pathlen);
			obj->path = (char *)pathbuf;
		}
	}

	/* Parse .dynamic section */
	dl_digest_dynamic(obj);

	return obj;
}

/* ================================================================
 * Parse the .dynamic section of a loaded object.
 *
 * Fills in symtab, strtab, hash table, relocations, init/fini.
 * Derived from FreeBSD rtld-elf digest_dynamic() by John D. Polstra.
 * ================================================================ */

void
dl_digest_dynamic(struct dl_object *obj)
{
	const Elf32_Dyn *dyn;

	for (dyn = obj->dynamic; dyn->d_tag != DT_NULL; dyn++) {
		switch (dyn->d_tag) {

		case DT_SYMTAB:
			obj->symtab = (const Elf32_Sym *)
				(obj->relocbase + dyn->d_un.d_ptr);
			break;

		case DT_STRTAB:
			obj->strtab = (const char *)
				(obj->relocbase + dyn->d_un.d_ptr);
			break;

		case DT_STRSZ:
			obj->strsize = dyn->d_un.d_val;
			break;

		case DT_HASH:
		{
			const Elf32_Word *hashtab = (const Elf32_Word *)
				(obj->relocbase + dyn->d_un.d_ptr);
			obj->nbuckets = hashtab[0];
			obj->nchains = hashtab[1];
			obj->buckets = hashtab + 2;
			obj->chains = obj->buckets + obj->nbuckets;
			break;
		}

		case DT_REL:
			obj->rel = (const Elf32_Rel *)
				(obj->relocbase + dyn->d_un.d_ptr);
			break;

		case DT_RELSZ:
			obj->relsize = dyn->d_un.d_val;
			break;

		case DT_JMPREL:
			obj->pltrel = (const Elf32_Rel *)
				(obj->relocbase + dyn->d_un.d_ptr);
			break;

		case DT_PLTRELSZ:
			obj->pltrelsize = dyn->d_un.d_val;
			break;

		case DT_INIT:
			obj->init = (void (*)(void))
				(obj->relocbase + dyn->d_un.d_ptr);
			break;

		case DT_FINI:
			obj->fini = (void (*)(void))
				(obj->relocbase + dyn->d_un.d_ptr);
			break;

		case DT_TEXTREL:
			obj->textrel = 1;
			break;

		default:
			break;
		}
	}
}

/* ================================================================
 * Free a loaded object — unmap VM and release dl_object struct.
 * ================================================================ */

void
dl_free_object(struct dl_object *obj)
{
	if (obj == NULL)
		return;

	if (obj->mapbase != 0 && obj->mapsize > 0)
		vm_deallocate(mach_task_self(), obj->mapbase, obj->mapsize);

	if (obj->path != NULL)
		vm_deallocate(mach_task_self(), (vm_offset_t)obj->path,
			      dl_round_page(strlen(obj->path) + 1));

	vm_deallocate(mach_task_self(), (vm_offset_t)obj,
		      dl_round_page(sizeof(struct dl_object)));
}
