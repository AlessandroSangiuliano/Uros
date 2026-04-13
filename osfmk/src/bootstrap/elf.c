/*
 * Copyright 1991-1998 by Open Software Foundation, Inc. 
 *              All Rights Reserved 
 *  
 * Permission to use, copy, modify, and distribute this software and 
 * its documentation for any purpose and without fee is hereby granted, 
 * provided that the above copyright notice appears in all copies and 
 * that both the copyright notice and this permission notice appear in 
 * supporting documentation. 
 *  
 * OSF DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE 
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS 
 * FOR A PARTICULAR PURPOSE. 
 *  
 * IN NO EVENT SHALL OSF BE LIABLE FOR ANY SPECIAL, INDIRECT, OR 
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM 
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN ACTION OF CONTRACT, 
 * NEGLIGENCE, OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION 
 * WITH THE USE OR PERFORMANCE OF THIS SOFTWARE. 
 */
/*
 * MkLinux
 */

#include "bootstrap.h"

#include "elf.h"

#include <ddb/nlist.h>

int elf_recog(struct file *, objfmt_t, void *);
int elf_load(struct file *, objfmt_t, void *);
void elf_symload(struct file *, mach_port_t, task_port_t, const char *,
		  objfmt_t);

struct objfmt_switch elf_switch = {
    "elf",
    elf_recog,
    elf_load,
    elf_symload
};

int
elf_recog(struct file *fp, objfmt_t ofmt, void *hdr)
{
    Elf32_Ehdr *x = *(Elf32_Ehdr **)hdr;

    return (x->e_ident[EI_MAG0] == ELFMAG0 &&
	    x->e_ident[EI_MAG1] == ELFMAG1 &&
	    x->e_ident[EI_MAG2] == ELFMAG2 &&
	    x->e_ident[EI_MAG3] == ELFMAG3);
}

int
elf_load(struct file *fp, objfmt_t ofmt, void *hdr)
{
    struct loader_info *lp = &ofmt->info;
    Elf32_Ehdr *ehdr = (Elf32_Ehdr *)hdr;
    Elf32_Phdr *phdr, *ph;
    Elf32_Shdr *shdr, *sh;
    size_t phsize;
    size_t shsize;
    int i;
    int result;

    /*
     * ET_DYN (PIE) support: pick a fixed load bias above the kernel's
     * legacy static link base (1 MiB).  Every PT_LOAD vaddr, the entry
     * point, and PT_DYNAMIC's VA are shifted by this bias.  Relocations
     * are applied later by the loader (load.c) before vm_write.
     */
    if ((int)ehdr->e_type == ET_DYN) {
	lp->load_bias = 0x40000000;
    } else {
	lp->load_bias = 0;
    }
    lp->dyn_vaddr  = 0;
    lp->dyn_filesz = 0;

    lp->entry_1 = (vm_offset_t) ehdr->e_entry + lp->load_bias;
    lp->entry_2 = 0;

    phsize = ehdr->e_phnum * ehdr->e_phentsize;
    phdr = (Elf32_Phdr *) malloc(phsize);

    result = read_file(fp, ehdr->e_phoff, (vm_offset_t)phdr, phsize);
    if (result)
	return result;

    lp->num_segments = 0;

    for (i = 0, ph = phdr; i < ehdr->e_phnum; i++, ph++) {
	/* Record the post-bias VA of PT_DYNAMIC for later relocation */
	if ((int)ph->p_type == PT_DYNAMIC) {
	    lp->dyn_vaddr  = (vm_offset_t)ph->p_vaddr + lp->load_bias;
	    lp->dyn_filesz = (vm_size_t)ph->p_filesz;
	    continue;
	}

	if ((int)ph->p_type != PT_LOAD || ph->p_memsz == 0)
	    continue;

	/* Record every PT_LOAD segment for the multi-segment loader */
	if (lp->num_segments < MAX_LOAD_SEGMENTS) {
	    struct load_segment *seg = &lp->segments[lp->num_segments++];
	    seg->seg_vaddr  = (vm_offset_t)ph->p_vaddr + lp->load_bias;
	    seg->seg_filesz = ph->p_filesz;
	    seg->seg_memsz  = ph->p_memsz;
	    seg->seg_offset = ph->p_offset;
	    seg->seg_prot   = VM_PROT_NONE;
	    if (ph->p_flags & PF_R) seg->seg_prot |= VM_PROT_READ;
	    if (ph->p_flags & PF_W) seg->seg_prot |= VM_PROT_WRITE;
	    if (ph->p_flags & PF_X) seg->seg_prot |= VM_PROT_EXECUTE;
	}

	/* Also fill legacy text/data fields for symbol classification */
	if (ph->p_flags & PF_X) {
	    lp->text_start  = trunc_page((vm_offset_t)ph->p_vaddr
					 + lp->load_bias);
	    lp->text_size   = (vm_size_t)ph->p_vaddr + lp->load_bias
				+ ph->p_filesz - lp->text_start;
	    lp->text_offset = trunc_page(ph->p_offset);
	} else if (ph->p_flags & PF_W) {
	    lp->data_start  = trunc_page((vm_offset_t)ph->p_vaddr
					 + lp->load_bias);
	    lp->data_size   = (vm_size_t)ph->p_vaddr + lp->load_bias
				+ ph->p_filesz - lp->data_start;
	    lp->bss_size    = ph->p_memsz - ph->p_filesz;
	    lp->data_offset = trunc_page(ph->p_offset);
	}
    }
    free(phdr);

    shsize = ehdr->e_shnum * ehdr->e_shentsize;
    shdr = (Elf32_Shdr *) malloc(shsize);

    result = read_file(fp, ehdr->e_shoff, (vm_offset_t)shdr, shsize);
    if (result)
	return result;

    lp->sym_offset[0] = 0;
    lp->sym_size[0] = 0;
    lp->sym_offset[1] = 0;
    lp->sym_size[1] = 0;
    lp->str_offset = 0;
    lp->str_size = 0;

    for (i = 0, sh = shdr; i < ehdr->e_shnum; i++, sh++) {
	switch ((int)sh->sh_type) {
	case SHT_SYMTAB:
	    lp->sym_offset[0] = (vm_offset_t) sh->sh_offset;
	    lp->sym_size[0] =
		(vm_size_t) sh->sh_size / sh->sh_entsize *
		    sizeof(struct nlist);
	    if (sh->sh_link > 0
		&& sh->sh_link < ehdr->e_shnum
		&& (int)shdr[sh->sh_link].sh_type == SHT_STRTAB) {

		lp->str_offset = (vm_offset_t) shdr[sh->sh_link].sh_offset;
		lp->str_size = (vm_size_t) shdr[sh->sh_link].sh_size;
	    }
	    break;
	case SHT_NULL:
	case SHT_STRTAB:
	case SHT_PROGBITS:
	case SHT_RELA:
	case SHT_HASH:
	case SHT_DYNAMIC:
	case SHT_NOTE:
	case SHT_NOBITS:
	case SHT_REL:
	case SHT_SHLIB:
	case SHT_DYNSYM:
	    break;
	}
    }
    free(shdr);

    return 0;
}

/*
 * Load symbols from file into kernel debugger.
 */
void
elf_symload(struct file *fp,
	    mach_port_t host_port,
	    task_port_t task,
	    const char *symtab_name,
	    objfmt_t ofmt)
{
    struct loader_info *lp = &ofmt->info;
    kern_return_t result;
    vm_offset_t	  symtab;
    vm_size_t     table_size;
    struct nlist  *nl;
    vm_offset_t	  buf;
    vm_offset_t	  strings;
    int      symsize;
    Elf32_Sym *sstab, *estab;

    if ((lp->sym_size[0] == 0) || (lp->str_size == 0))
	return;

    /*
     * Allocate space for the symbol table, preceding it by the header.
     */
    table_size = sizeof(int) + lp->sym_size[0] + lp->str_size + sizeof(int);
    result = vm_allocate(mach_task_self(), &symtab, table_size, TRUE);
    if (result)
    {
	BOOTSTRAP_IO_LOCK();
	printf("[ error %d allocating space for %s symbol table ]\n",
	       result, symtab_name);
	BOOTSTRAP_IO_UNLOCK();
	return;
    }

    *(int32_t*)symtab = lp->sym_size[0];

    nl = (struct nlist *)(symtab + sizeof(int));
    symsize = lp->sym_size[0] / sizeof(struct nlist) * sizeof(Elf32_Sym);
    buf = (vm_offset_t)malloc(symsize);
    result = read_file(fp, lp->sym_offset[0], buf, symsize);
    if (result) {
	BOOTSTRAP_IO_LOCK();
	printf("[ error %d reading %s symbol table ]\n", result, symtab_name);
	BOOTSTRAP_IO_UNLOCK();
	(void) vm_deallocate(mach_task_self(), symtab, table_size);
	return;
    }

    sstab = (Elf32_Sym *)buf;
    estab = (Elf32_Sym *)(buf + symsize);

    while(sstab < estab)
    {
	nl->n_un.n_strx = (long)sstab->st_name + sizeof(int);
	switch (ELF32_ST_TYPE(sstab->st_info)) {
	case STT_FILE:
	    nl->n_type = N_FN;
	    break;
	case STT_OBJECT:
	case STT_FUNC:
	    if (sstab->st_value >= lp->text_start &&
		sstab->st_value < lp->text_start + lp->text_size) {
		nl->n_type = N_TEXT;
		break;
	    }
	    if (sstab->st_value >= lp->data_start &&
		sstab->st_value < lp->data_start + lp->data_size) {
		nl->n_type = N_DATA;
		break;
	    }
	    if (sstab->st_value >= lp->data_start + lp->data_size &&
		sstab->st_value < lp->data_start + lp->data_size + lp->bss_size + sizeof(int)) {
		nl->n_type = N_BSS;
		break;
	    }
	    nl->n_type = N_ABS;
	    break;
	case STT_NOTYPE:
	    if (sstab->st_shndx == SHN_UNDEF) {
		nl->n_type = N_UNDF;
		break;
	    }
	case STT_LOPROC:
	case STT_HIPROC:
	case STT_SECTION:
	    /* Ignore symbol */
	    *(int32_t*)symtab -= sizeof(struct nlist);
	    table_size -= sizeof(struct nlist);
	    sstab++;
	    continue;
	
	default:
	    BOOTSTRAP_IO_LOCK();
	    printf("ELF32_ST_TYPE %d\n", ELF32_ST_TYPE(sstab->st_info));
	    BOOTSTRAP_IO_UNLOCK();
	    break;
	}
	switch (ELF32_ST_BIND(sstab->st_info)) {
	case STB_GLOBAL:
	    nl->n_type |= N_EXT;
	    break;
	case STB_LOCAL:
	    break;
	default:
	    BOOTSTRAP_IO_LOCK();
	    printf("ELF32_ST_BIND %d\n", ELF32_ST_BIND(sstab->st_info));
	    BOOTSTRAP_IO_UNLOCK();
	    break;
	}
	nl->n_other = (char)0;
	nl->n_desc = (short)0;
	nl->n_value = (unsigned long)sstab->st_value;
	sstab++;
	nl++;
    }
    free((void *)buf);

    *(int32_t*)nl = lp->str_size + sizeof(int32_t);
    strings = (vm_offset_t)nl + sizeof(int);

    /* 
     * Get the strings after the symbol table
     * Load the symbols into the kernel
     */
    if (!read_file(fp, lp->str_offset, strings, lp->str_size)) {
	    host_load_symbol_table(host_port, task,
				   (char *)symtab_name, symtab, table_size);
	    /* Ignore error - kern might not have kdb */
    }

    (void) vm_deallocate(mach_task_self(), symtab, table_size);
}
