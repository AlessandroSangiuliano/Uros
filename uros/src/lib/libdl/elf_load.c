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
#include <libelf.h>
#include "dl_internal.h"

/* Page size — 4 KiB on both targets */
#define DL_PAGE_SIZE	4096
#define DL_PAGE_MASK	(DL_PAGE_SIZE - 1)

/*
 * The machine this copy of libdl runs on (#423).
 *
 * ⚠️ Not decoration, and not the same check the old code made.  It used to
 * refuse a foreign object by CLASS -- `e_ident[EI_CLASS] != ELFCLASS32' --
 * which worked only while libelf could not read ELF64 either.  elf_open()
 * accepts both classes now, so refusing an object that does not match the
 * loader has to be said here or it stops being said at all: dlopen maps into
 * ITS OWN address space, so a 64-bit object in a 32-bit loader is not a
 * format to support, it is a file to reject.
 */
#if defined(__x86_64__)
#define DL_ELF_MACHINE	EM_X86_64
#elif defined(__i386__)
#define DL_ELF_MACHINE	EM_386
#else
#error "libdl: no ELF machine for this target"
#endif

static inline uintptr_t
dl_trunc_page(uintptr_t v)
{
	return v & ~(uintptr_t)DL_PAGE_MASK;
}

static inline uintptr_t
dl_round_page(uintptr_t v)
{
	return (v + DL_PAGE_MASK) & ~(uintptr_t)DL_PAGE_MASK;
}

/*
 * Convert ELF segment flags to Mach VM protection.
 */
static vm_prot_t
convert_prot(uint32_t flags)
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
	int i, nsegs, nphdr, rc;
	uintptr_t base_vaddr, base_vlimit;
	vm_size_t mapsize;
	vm_offset_t mapbase;
	kern_return_t kr;
	uintptr_t dyn_vaddr;
	int have_dyn;
	struct dl_object *obj;
	elf_image_t img;
	elf_phdr_view_t segs[16];	/* max 16 PT_LOAD segments */

	/* --- Read the header and the segment table --- */

	/*
	 * ⚠️ This used to be ninety lines of header validation and a phdr walk
	 * written here, which made libdl the SECOND ELF reader in this tree
	 * (#423).  libelf is the first, it reads both classes, and bootstrap
	 * exercises its ELF64 path on every x86-64 boot -- so the duplicate was
	 * also the half that would have had to be taught ELF64 a second time.
	 *
	 * 🔑 elf_open() checks strictly MORE than what it replaces: magic,
	 * class, byte order, version, machine, e_ehsize and e_phentsize, AND
	 * that the header tables fall inside the buffer -- which the code here
	 * never did.  A truncated module used to be walked off the end.
	 */
	rc = elf_open(filebuf, (size_t)filesize, &img);
	if (rc != ELF_OK) {
		snprintf(dl_error_msg, DL_ERRMSG_SIZE,
			 "%s: not a loadable ELF image (elf_open=%d)", path, rc);
		return NULL;
	}

	if (elf_machine(&img) != DL_ELF_MACHINE) {
		snprintf(dl_error_msg, DL_ERRMSG_SIZE,
			 "%s: wrong architecture (e_machine=%u, this loader is %u)",
			 path, (unsigned)elf_machine(&img),
			 (unsigned)DL_ELF_MACHINE);
		elf_close(&img);
		return NULL;
	}

	if (elf_type(&img) != ET_DYN) {
		snprintf(dl_error_msg, DL_ERRMSG_SIZE,
			 "%s: not a shared object (e_type=%u)", path,
			 (unsigned)elf_type(&img));
		elf_close(&img);
		return NULL;
	}

	/* --- Scan program headers --- */

	nsegs = 0;
	have_dyn = 0;
	dyn_vaddr = 0;
	nphdr = elf_phdr_count(&img);

	for (i = 0; i < nphdr; i++) {
		elf_phdr_view_t ph;

		if (elf_phdr_get(&img, i, &ph) != ELF_OK) {
			snprintf(dl_error_msg, DL_ERRMSG_SIZE,
				 "%s: program header %d unreadable", path, i);
			elf_close(&img);
			return NULL;
		}

		if (ph.type == PT_LOAD) {
			if (nsegs >= 16) {
				snprintf(dl_error_msg, DL_ERRMSG_SIZE,
					 "%s: too many PT_LOAD segments",
					 path);
				elf_close(&img);
				return NULL;
			}
			segs[nsegs++] = ph;
		}

		if (ph.type == PT_DYNAMIC) {
			dyn_vaddr = ph.vaddr;
			have_dyn = 1;
		}
	}

	/*
	 * The views above are copies, so nothing below reads through img.  The
	 * file buffer stays the caller's until this function returns, which is
	 * what the segment copy below still needs.
	 */
	elf_close(&img);

	if (nsegs == 0) {
		snprintf(dl_error_msg, DL_ERRMSG_SIZE,
			 "%s: no PT_LOAD segments", path);
		return NULL;
	}

	if (!have_dyn) {
		snprintf(dl_error_msg, DL_ERRMSG_SIZE,
			 "%s: no PT_DYNAMIC segment", path);
		return NULL;
	}

	/* --- Compute total VM footprint --- */

	base_vaddr = dl_trunc_page(segs[0].vaddr);
	base_vlimit = dl_round_page(segs[nsegs - 1].vaddr +
				    segs[nsegs - 1].memsz);
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
		uintptr_t seg_vaddr = segs[i].vaddr;
		uint64_t seg_offset = segs[i].offset;
		uint64_t seg_filesz = segs[i].filesz;
		uint64_t seg_memsz = segs[i].memsz;
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
			       (size_t)seg_filesz);

		/*
		 * Zero BSS (memsz > filesz).
		 *
		 * ⚠️ The narrowing is at the pointer and not on the values,
		 * deliberately: filesz and memsz are 64-bit because the view
		 * is class-neutral, and adding one of them to a 32-bit
		 * vm_offset_t would promote the sum and then cut it on the
		 * cast.  The bound above -- seg_offset + seg_filesz <=
		 * filesize -- is what makes this narrowing safe, so it has to
		 * stay in front of it.
		 */
		if (seg_memsz > seg_filesz)
			memset((void *)(dest + (vm_size_t)seg_filesz), 0,
			       (size_t)(seg_memsz - seg_filesz));
	}

	/* --- Set page protections for each segment --- */

	for (i = 0; i < nsegs; i++) {
		uintptr_t seg_start = dl_trunc_page(segs[i].vaddr);
		uintptr_t seg_end = dl_round_page(segs[i].vaddr +
						  segs[i].memsz);
		vm_offset_t addr = mapbase + (seg_start - base_vaddr);
		vm_size_t size = seg_end - seg_start;
		vm_prot_t prot = convert_prot(segs[i].flags);

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
	obj->dynamic = (const Elf32_Dyn *)(obj->relocbase + dyn_vaddr);
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
