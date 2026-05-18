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
 * dl_internal.h — Internal structures and declarations for libdl.
 *
 * Based on FreeBSD rtld-elf by John D. Polstra (BSD 2-clause license).
 * Adapted for Uros/OSF Mach: mmap → vm_allocate, file I/O via callbacks,
 * symbol resolution against host-provided symbol table.
 */

#ifndef _DL_INTERNAL_H_
#define _DL_INTERNAL_H_

#include <mach/elf.h>
#include <mach.h>

/* ================================================================
 * Loaded object descriptor
 * ================================================================ */

struct dl_object {
	char		*path;		/* pathname of the .so file */
	vm_offset_t	mapbase;	/* base address of mapped region */
	vm_size_t	mapsize;	/* total size of mapped region */
	Elf32_Addr	vaddrbase;	/* lowest vaddr from PT_LOAD segments */
	vm_offset_t	relocbase;	/* relocation delta = mapbase - vaddrbase */

	/* Pointers into the mapped image (set by digest_dynamic) */
	const Elf32_Dyn	*dynamic;	/* .dynamic section */
	const Elf32_Sym	*symtab;	/* DT_SYMTAB */
	const char	*strtab;	/* DT_STRTAB */
	unsigned long	strsize;	/* DT_STRSZ */

	/* SYSV hash table */
	const Elf32_Word *buckets;	/* hash buckets array */
	unsigned long	nbuckets;
	const Elf32_Word *chains;	/* hash chains array */
	unsigned long	nchains;

	/* Relocations */
	const Elf32_Rel	*rel;		/* DT_REL */
	unsigned long	relsize;	/* DT_RELSZ */
	const Elf32_Rel	*pltrel;	/* DT_JMPREL (PLT relocations) */
	unsigned long	pltrelsize;	/* DT_PLTRELSZ */

	/* Init/fini */
	void		(*init)(void);	/* DT_INIT */
	void		(*fini)(void);	/* DT_FINI */

	/* Text relocations flag */
	int		textrel;	/* DT_TEXTREL present */

	/* Bookkeeping */
	int		ref_count;
	struct dl_object *next;		/* linked list of loaded objects */
};

/* ================================================================
 * Host symbol table — symbols exported by the loading server
 * ================================================================ */

struct dl_host_sym {
	const char	*name;
	void		*addr;
};

/* ================================================================
 * I/O callbacks — how the loader reads files
 * ================================================================ */

struct dl_io_ops {
	/*
	 * Read an entire file into memory.
	 * On success: set *buf and *size, return 0.
	 * On failure: return -1.
	 */
	int	(*read_file)(const char *path, void **buf,
			     unsigned int *size);
	/*
	 * Free a buffer returned by read_file.
	 */
	void	(*free_buf)(void *buf, unsigned int size);
};

/* ================================================================
 * Global state (internal to libdl)
 * ================================================================ */

extern struct dl_object		*dl_obj_list;
extern const struct dl_host_sym	*dl_host_symtab;
extern const struct dl_io_ops	*dl_io;
extern char			dl_error_msg[];

#define DL_ERRMSG_SIZE		256

/* ================================================================
 * Internal function declarations
 * ================================================================ */

/* elf_load.c — Load ELF shared object into memory */
struct dl_object	*dl_map_object(const void *filebuf,
				       unsigned int filesize,
				       const char *path);

/* elf_load.c — Parse .dynamic section, fill in symtab/strtab/rel/hash */
void			dl_digest_dynamic(struct dl_object *obj);

/* elf_reloc.c — Apply relocations */
int			dl_relocate(struct dl_object *obj);

/* elf_symbol.c — ELF SYSV hash */
unsigned long		dl_elf_hash(const char *name);

/* elf_symbol.c — Look up symbol in one object's hash table */
const Elf32_Sym		*dl_symlook_obj(const char *name,
					unsigned long hash,
					const struct dl_object *obj);

/* elf_symbol.c — Look up symbol: first in object, then in host table */
void			*dl_resolve_symbol(const char *name,
					   const struct dl_object *obj);

/* elf_load.c — Free a loaded object */
void			dl_free_object(struct dl_object *obj);

#endif /* _DL_INTERNAL_H_ */
